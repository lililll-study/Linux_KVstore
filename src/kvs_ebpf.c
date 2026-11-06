#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <bpf/libbpf.h>
#include <bpf/bpf.h>

#include "kvstore.h"
#include "aof_capture.skel.h"
#include "aof_capture.h"

typedef uint32_t u32;
typedef uint64_t u64;

/* 全局句柄：整个进程只有一份 */
static struct aof_capture_bpf *g_skel;
static struct bpf_link        *g_link;
static struct ring_buffer     *g_rb;
static pthread_t               g_poll_tid;
static int                     g_thread_created;
static volatile int            g_stop;
static volatile int            g_running;    /* 表示bpf捕获是否启动并运行 */
static u64                     g_captured;
static u64                     g_forwarded;

/* ===== 三态机状态：正常转发 / 全量同步窗口缓存 / 冲刷补发 ===== */
static volatile int g_in_sync = 0;      /* 1: 处于 SYNC~SYNC-FINISHED 窗口 */
static volatile int g_slave_fd = -1;    /* 从机增量连接 fd，由 SYNC 事件赋值
                                         * ⚠ 与 kvs_sync.c 的 g_slave_incr_fd 指向同一条连接：
                                         *   两处各自独立登记，改动需同步评估 */

/* 同步窗口缓存：窗口内主机新收到的写命令先存这里，FINISHED 后一次性补发 */
#define EBPF_CACHE_MAX (64u << 20)      /* 64MB 上限（malloc 懒加载，实际只占用到量） */
static char *g_cache;
static unsigned int g_cache_len;

/* libbpf 自己的日志转 stderr，DEBUG 屏蔽掉（否则骨架加载时刷屏） */
static int libbpf_log(enum libbpf_print_level level, const char *fmt, va_list args)
{
    if (level == LIBBPF_DEBUG)
        return 0;
    fprintf(stderr, "[EBPF][libbpf] ");
    vfprintf(stderr, fmt, args);
    return 0;
}

/* 批量转发缓冲：一条事件一次 send() 太慢（5w 条 = 5w 次系统调用），
 * 攒满 64KB 再发一次，5w 条命令只需约 40 次 send()，消费者速度提升两个数量级 */
#define EBPF_BATCH_SIZE 65536

static char g_batch[EBPF_BATCH_SIZE];
static int  g_batch_len;
static u64  g_lost_seen;      /* 上次看到的 ringbuf 丢弃计数，用于只报新增 */


/**
 * @name 把一条命令拼成 RESP 协议（只拼不发），返回拼好的字节数
 * eBPF 模块内部实现，与 kvs_sync 解耦
 */
static int ebpf_resp_format(char *buf, int cap,
                            const char *cmd, const char *key, const char *value)
{
    if (!buf || cap <= 0 || !cmd) return -1;

    if (value) {
        return snprintf(buf, cap,
                        "*3\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                        strlen(cmd), cmd,
                        strlen(key), key,
                        strlen(value), value);
    }
    if (key) {
        return snprintf(buf, cap,
                        "*2\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                        strlen(cmd), cmd,
                        strlen(key), key);
    }
    /* 无参命令（SAVE）：*1 单元素数组 */
    return snprintf(buf, cap,
                    "*1\r\n$%zu\r\n%s\r\n",
                    strlen(cmd), cmd);
}

/**
 * @name 把同步窗口内缓存的命令全部推给从机
 * FINISHED 时刻从机接收线程刚起，可能短时读得慢：
 * EAGAIN 重试最多约 5s，出错/超时就放弃整批（下次全量同步会重新对齐）
 */
static void cache_flush(void)
{
    if (g_cache_len == 0)
        return;

    if (g_slave_fd <= 0) {
        LOG_ERROR("[EBPF] 缓存无处可发（从机通道未登记），丢弃 %u 字节", g_cache_len);
        g_cache_len = 0;
        return;
    }

    unsigned int off = 0;
    int retry = 0;

    while (off < g_cache_len) {
        int n = (int)send(g_slave_fd, g_cache + off, g_cache_len - off, 0);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (++retry > 50) {          /* 100ms × 50 = 最多等约 5s */
                LOG_ERROR("[EBPF] 从机读取过慢，放弃剩余 %u 字节", g_cache_len - off);
                break;
            }
            usleep(100000);
            continue;
        }
        if (n <= 0) {
            LOG_ERROR("[EBPF] 缓存补发失败 fd=%d errno=%d", g_slave_fd, errno);
            break;
        }
        off += (unsigned int)n;
        retry = 0;
    }

    LOG_INFO("[EBPF] 缓存补发完成：%u/%u 字节", off, g_cache_len);
    g_cache_len = 0;
}

/**
 * @name 把批量转发缓冲通过网络连接发给从机
 */
static int batch_flush(void)
{
    if (g_batch_len <= 0) return 0;

    int len = g_batch_len;
    g_batch_len = 0;

    /* 从机通道未登记：无处可发（全量同步尚未开始/从机已断开） */
    if (g_slave_fd <= 0) return 0;

    int sent = 0;
    while (sent < len) {
        int n = (int)send(g_slave_fd, g_batch + sent, len - sent, 0);
        if (n <= 0) {
            LOG_ERROR("[EBPF] 增量转发失败 fd=%d errno=%d", g_slave_fd, errno);
            return -1;
        }
        sent += n;
    }
    return 0;
}


/**
 * @name ringbuf 的事件回调函数
 * 四态分发：
 *   EVT_SYNC_BEGIN → 登记从机通道 fd，进入缓存模式
 *   EVT_SYNC_END   → 退出缓存模式，把窗口内命令一次性补发给从机
 *   EVT_WRITE      → 缓存模式存入 g_cache；正常模式攒批发送
 *   EVT_SAVE       → 同写命令路径转发（无 key，拼 *1 格式），从机收到后自行落盘
 */
static int poll_event(void *ctx, void *data, size_t len)
{
    (void)ctx;
    const struct aof_event *e = data;

    if (len < sizeof(*e))
        return 0;

    g_captured++;

    /* ===== 1. 同步开始：登记从机通道，进入缓存模式 ===== */
    if (e->type == EVT_SYNC_BEGIN) {
        g_slave_fd = e->fd;
        g_cache_len = 0;             /* 上一轮残留丢弃，从机此时会重新全量 */
        g_in_sync = 1;

        /* 放大发送缓冲 + 限制阻塞时间：防 5w 条瞬时灌入把 socket 堵死 */
        int sndbuf = 8 << 20;        /* 8MB */
        setsockopt(g_slave_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
        struct timeval tv = {5, 0};  /* send 最多阻塞 5s，超时返回 EAGAIN */
        setsockopt(g_slave_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        LOG_INFO("[EBPF] 探测到 SYNC，进入缓存模式，从机通道 fd=%d", e->fd);
        return 0;
    }

    /* ===== 2. 同步结束：补发窗口内数据，回到正常转发 ===== */
    if (e->type == EVT_SYNC_END) {
        g_in_sync = 0;
        LOG_INFO("[EBPF] 探测到 SYNC-FINISHED，补发缓存 %u 字节", g_cache_len);
        cache_flush();
        return 0;
    }

    /* ===== 3. 写命令 / SAVE：只拼 RESP 不发送 ===== */
    if (e->truncated)
        LOG_WARN("[EBPF] 事件被截断，cmd=%s key=%s", e->cmd, e->key);

    char one[4096];
    int n = ebpf_resp_format(one, sizeof(one), e->cmd,
                             e->key_len > 0 ? e->key : NULL,
                             e->has_value ? e->value : NULL);
    if (n < 0) {
        LOG_ERROR("[EBPF] 命令过长无法拼包，丢弃 cmd=%s key=%s", e->cmd, e->key);
        return 0;
    }

    /* 全量同步窗口内：先缓存，等 FINISHED 再一次性补发（窗口内不丢数据） */
    if (g_in_sync) {
        if (g_cache_len + (unsigned int)n > EBPF_CACHE_MAX) {
            LOG_ERROR("[EBPF] 同步窗口缓存已满（%u 字节），本条丢弃 cmd=%s key=%s",
                      EBPF_CACHE_MAX, e->cmd, e->key);
            return 0;
        }
        memcpy(g_cache + g_cache_len, one, n);
        g_cache_len += (unsigned int)n;
        return 0;
    }

    /* 正常转发：攒满 64KB 才发一次 */
    if (g_batch_len + n > EBPF_BATCH_SIZE) {
        if (batch_flush() < 0) {
            LOG_ERROR("[EBPF] 批量转发失败（从机连接可能已断），本批丢弃");
            return 0;
        }
    }

    memcpy(g_batch + g_batch_len, one, n);
    g_batch_len += n;

    g_forwarded++;
    if ((g_forwarded % 1000) == 0)
        LOG_DEBUG("[EBPF] 已转发 %llu 条（累计捕获 %llu 条）",
                  (unsigned long long)g_forwarded,
                  (unsigned long long)g_captured);
    return 0;
}


/**
 * @name eBPF 捕获模块的后台消费线程
 * @param 
 * @fn: poll 线程从内核提交到用户态的ringbuf里读事件
 *      1：从内核 ringbuf 里把事件泵出来（ring_buffer__poll → 触发 handle_event 把 RESP 攒进 g_batch）
 *      2：把攒下的命令批量发给从机（batch_flush）；
 *      3：周期性检查丢包并告警（读 lost_count）。
 */
static void *poll_ringbuf_thread(void *arg)
{
    (void)arg;
    int tick = 0;   // 计数器，每轮循环+1。每10次poll检查一次丢包

    // 线程一跑起来，就进入主循环。
    // g_stop：poll 线程每轮循环都会重新从内存读，能及时看到主线程的修改。
    while (!g_stop) {
        // ring_buffer__poll是 libbpf 提供的一次性消费入口，用于检查用户态已消费位置 和 内核已写入位置
        int err = ring_buffer__poll(g_rb, 100);   /* 100ms 超时，保证能及时退出 */
        // 返回值err是本次处理的事件条数
        if (err == -EINTR) continue;
        if (err < 0) {
            LOG_ERROR("[EBPF] ringbuf poll 失败: %s", strerror(-err));
            break;
        }

        /* 立刻 flush：把这一轮消费的攒批发出去 */
        // 因为handle_event只在满64KB时才主动flush，如果这一轮只来了几条命令，那命令会一直卡在用户态缓冲区
        // 所以此处线程中在每次poll时都立刻flush一次，保证转发延迟最多100ms
        if (batch_flush() < 0) LOG_ERROR("[EBPF] 批量转发失败（从机连接可能已断）");

        /* 周期性检查丢包：每 1 秒一次 */
        if (++tick % 10 == 0) {
            u64 lost = 0;
            u32 key = 0;
            if (bpf_map_lookup_elem(bpf_map__fd(g_skel->maps.lost_count),
                                    &key, &lost) == 0 && lost != g_lost_seen) {
                LOG_ERROR("[EBPF] ringbuf 丢弃 %llu 条（累计），已转发 %llu 条 "
                          "—— 请调大 ringbuf 或降低写入速率",
                          (unsigned long long)lost,
                          (unsigned long long)g_forwarded);
                g_lost_seen = lost;
            }
        }
    }

    batch_flush();   /* 退出前把最后一批刷掉 */
    return NULL;
}


/**
 * @name ebpf捕获模块的启动入口
 * @param bin_path，target_pid: 进程的路径，进程id
 * @fn: 把编译好的bpf程序装载进内核，hook到进程的kvs_filter_protocol函数上
 *      并启动用户态消费线程
 */
int master_ebpf_start(const char *bin_path, int target_pid)
{
    if (g_running) return 0;    // 如果已经启动过了ebpf模块则直接返回
    if (!bin_path || target_pid <= 0) return -1;
    if (geteuid() != 0) {
        LOG_ERROR("[EBPF] 需要 root 权限（请用 sudo 启动 kvstore）");
        return -1;
    }

    // 把 libbpf 库内部的所有打印输出，重定向到 libbpf_log 回调函数
    // 用于统一日志格式，使得所有 libbpf 输出带 [EBPF][libbpf] 前缀
    libbpf_set_print(libbpf_log);

    /* 1. 打开并加载 BPF 程序 */
    g_skel = aof_capture_bpf__open_and_load(); // 解析ELF，并把程序送入内核，然后拿到fd
    if (!g_skel) {
        LOG_ERROR("[EBPF] 加载 BPF 失败：检查 root / BTF / verifier");
        return -1;
    }

    /* 2. 写入目标 pid（BPF 侧还会用 bpf_get_current_pid_tgid 二次过滤） */
    u32 key = 0, pid_val = (u32)target_pid;
    if (bpf_map_update_elem(bpf_map__fd(g_skel->maps.target_pid), &key, &pid_val, BPF_ANY) != 0) {
        LOG_ERROR("[EBPF] 写入 target_pid 失败: %s", strerror(errno));
        goto fail;
    }

    /* 3. 挂载 uprobe：挂到本进程的网络命令统一入口 kvs_filter_protocol */
    // 进程执行kvs_filter_protocol时陷入内核，跑 BPF，跑完恢复原指令继续
    // 与持久化解耦：读命令 fd/tokens，白名单过滤写命令 + SYNC 边界
    LIBBPF_OPTS(bpf_uprobe_opts, opts, .func_name = "kvs_filter_protocol", .retprobe  = false,);
                                            // 要attach的BPF程序
    g_link = bpf_program__attach_uprobe_opts(g_skel->progs.on_net_cmd, target_pid, bin_path, 0, &opts);
    if (!g_link) {
        LOG_ERROR("[EBPF] attach uprobe 失败: %s（检查符号/路径/-no-pie）", strerror(errno));
        goto fail;
    }

    /* 4. ringbuf 消费者 */
    // events是 BPF 侧定义的 BPF_MAP_TYPE_RINGBUF，大小64MB
    // ring_buffer__new 在用户态把这个内核环形队列mmap 映射进来, 并注册回调 handle_event
    // ring_buffer__new 就是在用户态为这块内核共享内存创建一个消费者、
    // BPF_MAP_TYPE_RINGBUF 是 Linux 5.8 引入的一种 BPF map 类型，专门用于内核态 BPF 程序向用户态高效传输事件流。
    // 一块在内核分配、被 mmap 到用户态虚拟地址空间的多生产者单消费者环形缓冲区。
    // 这块内存的作用是，内核往里面写事件（cmd / key / value / truncated ...），然后submit提交到用户态
    g_rb = ring_buffer__new(bpf_map__fd(g_skel->maps.events), poll_event, NULL, NULL);
    if (!g_rb) {
        LOG_ERROR("[EBPF] 创建 ringbuf 消费者失败: %s", strerror(errno));
        goto fail;
    }

    /* 5. 分配同步窗口缓存，起消费线程 */
    if (!g_cache) {
        g_cache = (char *)malloc(EBPF_CACHE_MAX);
        if (!g_cache) {
            LOG_ERROR("[EBPF] 分配同步窗口缓存失败（%u 字节）", EBPF_CACHE_MAX);
            goto fail;
        }
    }
    g_in_sync = 0;
    g_slave_fd = -1;
    g_cache_len = 0;

    g_stop = 0; // 让 poll 线程的循环条件为真
    if (pthread_create(&g_poll_tid, NULL, poll_ringbuf_thread, NULL) != 0) {
        LOG_ERROR("[EBPF] 创建消费线程失败");
        goto fail;
    }
    g_thread_created = 1;   // 标记线程已创建, 方便 ebpf_capture_stop 里 pthread_join 收割

    g_running = 1;
    LOG_INFO("[EBPF] 已挂载 kvs_filter_protocol：bin=%s pid=%d", bin_path, target_pid);
    return 0;

fail:
    ebpf_capture_stop();
    return -1;
}



int ebpf_capture_is_running(void)
{
    return g_running;
}

void ebpf_capture_stop(void)
{
    if (!g_skel && !g_thread_created && !g_rb && !g_link)
        return;

    if (g_thread_created)
        batch_flush();          /* 先把攒下的命令发出去，再停线程 */
    g_in_sync = 0;
    g_slave_fd = -1;
    g_cache_len = 0;

    if (g_thread_created) {
        g_stop = 1;
        pthread_join(g_poll_tid, NULL);
        g_thread_created = 0;
    }

    u64 lost = 0;
    u32 key = 0;
    if (g_skel &&
        bpf_map_lookup_elem(bpf_map__fd(g_skel->maps.lost_count), &key, &lost) == 0 &&
        lost > 0) {
        LOG_WARN("[EBPF] ringbuf 丢弃 %llu 条事件", (unsigned long long)lost);
    }

    if (g_running) {
        LOG_INFO("[EBPF] 共捕获 %llu 条，转发 %llu 条，丢弃 %llu 条",
                 (unsigned long long)g_captured,
                 (unsigned long long)g_forwarded,
                 (unsigned long long)lost);
    }

    if (g_rb)   { ring_buffer__free(g_rb);      g_rb = NULL; }
    if (g_link) { bpf_link__destroy(g_link);    g_link = NULL; }
    if (g_skel) { aof_capture_bpf__destroy(g_skel); g_skel = NULL; }

    g_running = 0;
}