#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

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
static volatile int            g_forward;    /* 0: 只消费不转发（全量窗口）；1: 正常转发 */
static volatile int            g_running;    /* 表示bpf捕获是否启动并运行 */
static u64                     g_captured;
static u64                     g_forwarded;

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
 * @name 把数据通过网络连接转发给“从节点（slave）”
 */
static int batch_flush(void)
{
    if (g_batch_len <= 0) return 0;

    int len = g_batch_len;
    g_batch_len = 0;

    /* 全量同步窗口关闭了：这一批命令已经在全量 AOF 里，丢弃 */
    if (!g_forward) return 0;

    return master_aof_send_raw(g_batch, len);
}


/**
 * @name ringbuf 的事件回调函数
 * ring_buffer__poll(g_rb, 100) 每从内核 ringbuf 取出一条事件，libbpf 就调它一次
 * 它是 eBPF 捕获链路里"用户态收数据"的第一站，
 * 负责把 aof_event 结构体转换成 RESP 协议、攒进批量缓冲，等待 batch_flush 发往从机。
 */
static int poll_event(void *ctx, void *data, size_t len)
{
    (void)ctx;
    const struct aof_event *e = data;

    if (len < sizeof(*e))
        return 0;

    g_captured++;

    /* 全量同步窗口内：这些命令已经在全量 AOF 里了，丢弃避免从机重复执行 */
    if (!g_forward)
        return 0;

    if (e->truncated)
        LOG_WARN("[EBPF] 事件被截断，cmd=%s key=%s", e->cmd, e->key);

    /* 只拼 RESP 不发送，攒进批量缓冲 */
    char one[4096];
    int n = master_aof_format_cmd(one, sizeof(one), e->cmd, e->key, e->has_value ? e->value : NULL);
    if (n < 0) {
        LOG_ERROR("[EBPF] 命令过长无法拼包，丢弃 cmd=%s key=%s", e->cmd, e->key);
        return 0;
    }

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
 * @fn: 把编译好的bpf程序装载进内核，hook到进程的append_aof函数上
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

    /* 3. 挂载 uprobe：挂到本进程的 append_aof */
    // 进程执行append_aof程序时陷入内核，跑 BPF，跑完恢复原指令继续
    LIBBPF_OPTS(bpf_uprobe_opts, opts, .func_name = "append_aof", .retprobe  = false,);
                                            // 要attach的BPF程序
    g_link = bpf_program__attach_uprobe_opts(g_skel->progs.on_append_aof, target_pid, bin_path, 0, &opts);
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

    /* 5. 起消费线程；初始 forward=0，等从机全量同步结束后再打开 */
    g_stop = 0; // 让 poll 线程的循环条件为真
    g_forward = 0;  // 初始旁路模式，不直接打开转发
    if (pthread_create(&g_poll_tid, NULL, poll_ringbuf_thread, NULL) != 0) {
        LOG_ERROR("[EBPF] 创建消费线程失败");
        goto fail;
    }
    g_thread_created = 1;   // 标记线程已创建, 方便 ebpf_capture_stop 里 pthread_join 收割

    g_running = 1;
    LOG_INFO("[EBPF] 已挂载 append_aof：bin=%s pid=%d", bin_path, target_pid);
    return 0;

fail:
    ebpf_capture_stop();
    return -1;
}



int ebpf_capture_is_running(void)
{
    return g_running;
}

void ebpf_capture_set_forward(int on)
{
    g_forward = on ? 1 : 0;
}

void ebpf_capture_stop(void)
{
    if (!g_skel && !g_thread_created && !g_rb && !g_link)
        return;

    if (g_thread_created)
        batch_flush();          /* 先把攒下的命令发出去，再停线程 */
    g_forward = 0;

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