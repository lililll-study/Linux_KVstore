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
static volatile int            g_running;    /* attach 成功标记，供主节点降级判断 */
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

/* ringbuf 每来一条事件，libbpf 就回调一次（运行在 poll 线程里） */
static int handle_event(void *ctx, void *data, size_t len)
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

    /* 复用同步链路唯一的出口：拼 RESP + safe_send */
    if (master_aof_sync_send(e->cmd, e->key, e->has_value ? e->value : NULL) < 0) {
        LOG_ERROR("[EBPF] 转发失败 cmd=%s key=%s", e->cmd, e->key);
        return 0;
    }

    g_forwarded++;
    if ((g_forwarded % 1000) == 0)
        LOG_DEBUG("[EBPF] 已转发 %llu 条（累计捕获 %llu 条）",
                  (unsigned long long)g_forwarded,
                  (unsigned long long)g_captured);
    return 0;
}

static void *poll_thread(void *arg)
{
    (void)arg;
    while (!g_stop) {
        int err = ring_buffer__poll(g_rb, 100);   /* 100ms 超时，保证能及时退出 */
        if (err == -EINTR)
            continue;
        if (err < 0) {
            LOG_ERROR("[EBPF] ringbuf poll 失败: %s", strerror(-err));
            break;
        }
    }
    return NULL;
}

int ebpf_capture_start(const char *bin_path, int target_pid)
{
    if (g_running)
        return 0;
    if (!bin_path || target_pid <= 0)
        return -1;
    if (geteuid() != 0) {
        LOG_ERROR("[EBPF] 需要 root 权限（请用 sudo 启动 kvstore）");
        return -1;
    }

    libbpf_set_print(libbpf_log);

    /* 1. 打开 + 加载 BPF 程序 */
    g_skel = aof_capture_bpf__open_and_load();
    if (!g_skel) {
        LOG_ERROR("[EBPF] 加载 BPF 失败：检查 root / BTF / verifier");
        return -1;
    }

    /* 2. 写入目标 pid（BPF 侧还会用 bpf_get_current_pid_tgid 二次过滤） */
    u32 key = 0, pid_val = (u32)target_pid;
    if (bpf_map_update_elem(bpf_map__fd(g_skel->maps.target_pid),
                            &key, &pid_val, BPF_ANY) != 0) {
        LOG_ERROR("[EBPF] 写入 target_pid 失败: %s", strerror(errno));
        goto fail;
    }

    /* 3. 挂到本进程的 append_aof（业务代码一行不改） */
    LIBBPF_OPTS(bpf_uprobe_opts, opts,
        .func_name = "append_aof",
        .retprobe  = false,
    );
    g_link = bpf_program__attach_uprobe_opts(g_skel->progs.on_append_aof,
                                            target_pid, bin_path, 0, &opts);
    if (!g_link) {
        LOG_ERROR("[EBPF] attach uprobe 失败: %s（检查符号/路径/-no-pie）",
                  strerror(errno));
        goto fail;
    }

    /* 4. ringbuf 消费者 */
    g_rb = ring_buffer__new(bpf_map__fd(g_skel->maps.events),
                            handle_event, NULL, NULL);
    if (!g_rb) {
        LOG_ERROR("[EBPF] 创建 ringbuf 消费者失败: %s", strerror(errno));
        goto fail;
    }

    /* 5. 起消费线程；初始 forward=0，等从机全量同步结束后再打开 */
    g_stop = 0;
    g_forward = 0;
    if (pthread_create(&g_poll_tid, NULL, poll_thread, NULL) != 0) {
        LOG_ERROR("[EBPF] 创建消费线程失败");
        goto fail;
    }
    g_thread_created = 1;

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