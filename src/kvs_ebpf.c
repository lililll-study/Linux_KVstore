// /home/lhy/share/kvs_lhy/src/kvs_ebpf.c
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "aof_capture.skel.h"
#include "aof_capture.h"

typedef uint32_t u32;
typedef uint64_t u64;

static volatile sig_atomic_t g_stop;
static u64 g_event_count;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* ringbuf 每来一条事件，libbpf 就回调一次 */
static int handle_event(void *ctx, void *data, size_t len)
{
    (void)ctx;
    const struct aof_event *e = data;

    if (len < sizeof(*e)) {
        fprintf(stderr, "事件长度异常: %zu\n", len);
        return 0;
    }

    g_event_count++;
    if (e->has_value)
        printf("[%llu] %s %s %s%s\n",
               (unsigned long long)g_event_count,
               e->cmd, e->key, e->value,
               e->truncated ? "  <TRUNCATED>" : "");
    else
        printf("[%llu] %s %s (nil)%s\n",
               (unsigned long long)g_event_count,
               e->cmd, e->key,
               e->truncated ? "  <TRUNCATED>" : "");

    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <kvstore_binary> <pid>\n", argv[0]);
        return 1;
    }
    if (geteuid() != 0) {
        fprintf(stderr, "需要 root：sudo %s ...\n", argv[0]);
        return 1;
    }

    const char *bin_path = argv[1];
    int target_pid = atoi(argv[2]);
    if (target_pid <= 0) {
        fprintf(stderr, "pid 必须 > 0\n");
        return 1;
    }

    /* 1. 打开 + 加载 */
    // 打开并加载 skeleton
    struct aof_capture_bpf *skel = aof_capture_bpf__open_and_load();
    if (!skel) {
        fprintf(stderr, "加载 BPF 失败：检查 root / BTF / verifier\n");
        return 1;
    }

    /* 2. 写入目标 pid */
    u32 key = 0, pid_val = (u32)target_pid;
    if (bpf_map_update_elem(bpf_map__fd(skel->maps.target_pid),
                            &key, &pid_val, BPF_ANY) != 0) {
        fprintf(stderr, "写入 target_pid 失败: %s\n", strerror(errno));
        goto out;
    }

    /* 3. 挂到 append_aof */
    LIBBPF_OPTS(bpf_uprobe_opts, opts, .func_name = "append_aof", .retprobe  = false,);
    struct bpf_link *link = bpf_program__attach_uprobe_opts(
        skel->progs.on_append_aof, target_pid, bin_path, 0, &opts);
    if (!link) {
        fprintf(stderr, "attach uprobe 失败: %s（检查符号/路径/-no-pie）\n",
                strerror(errno));
        goto out;
    }

    /* 4. 创建 ringbuf 消费者 */
    struct ring_buffer *rb = ring_buffer__new(bpf_map__fd(skel->maps.events),
                                              handle_event, NULL, NULL);
    if (!rb) {
        fprintf(stderr, "创建 ringbuf 消费者失败: %s\n", strerror(errno));
        bpf_link__destroy(link);
        goto out;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    printf("已挂载 append_aof：bin=%s pid=%d\n", bin_path, target_pid);
    printf("逐条打印捕获到的命令，Ctrl-C 退出\n");

    /* 5. 100ms 超时轮询，既能及时退出，又不会空转 */
    while (!g_stop) {
        int err = ring_buffer__poll(rb, 100);
        if (err == -EINTR)
            continue;
        if (err < 0) {
            fprintf(stderr, "ringbuf poll 失败: %s\n", strerror(-err));
            break;
        }
    }

    ring_buffer__free(rb);

    u64 lost = 0;
    if (bpf_map_lookup_elem(bpf_map__fd(skel->maps.lost_count), &key, &lost) == 0)
        printf("\n共捕获 %llu 条，ringbuf 丢弃 %llu 条\n",
               (unsigned long long)g_event_count,
               (unsigned long long)lost);

    bpf_link__destroy(link);
out:
    aof_capture_bpf__destroy(skel);
    return 0;
}