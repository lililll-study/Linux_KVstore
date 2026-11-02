
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include "aof_capture.h"


// 内核侧 BPF 程序


/* 目标进程 pid：0 表示不做过滤 */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, u32);
    __type(value, u32);
} target_pid SEC(".maps");

/* 事件队列：内核 → 用户态
 * 单个事件 1312B，1<<26 = 64MB 可缓存约 5 万个事件，
 * 足以兜住"1 秒内插入 5 万条"的突发，不会在消费者追赶期间丢事件 */
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 1 << 26);
} events SEC(".maps");

/* ringbuf 满时的丢弃计数 */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, u32);
    __type(value, u64);
} lost_count SEC(".maps");

// 挂到目标进程的 append_aof 函数上
// PID 过滤 → 读 rdi/rsi/rdx 字符串 → 填 aof_event
SEC("uprobe")
int on_append_aof(struct pt_regs *ctx)
{
    u32 key = 0;

    /* 只捕获指定进程 */
    u32 *want = bpf_map_lookup_elem(&target_pid, &key);
    if (want && *want != 0) {
        u32 cur = (u32)(bpf_get_current_pid_tgid() >> 32);
        if (cur != *want)
            return 0;
    }

    /* append_aof(cmd, key, value)，x86_64 下分别对应 rdi/rsi/rdx */
    const char *cmd = (const char *)PT_REGS_PARM1(ctx);
    const char *k   = (const char *)PT_REGS_PARM2(ctx);
    const char *v   = (const char *)PT_REGS_PARM3(ctx);
    if (!cmd || !k)
        return 0;

    struct aof_event *e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
    if (!e) {
        u64 *lost = bpf_map_lookup_elem(&lost_count, &key);
        if (lost)
            __sync_fetch_and_add(lost, 1);
        return 0;
    }

    long n;

    n = bpf_probe_read_user_str(e->cmd, sizeof(e->cmd), cmd);
    e->cmd_len = n > 0 ? (u32)(n - 1) : 0;
    e->truncated = (n >= (long)sizeof(e->cmd));

    n = bpf_probe_read_user_str(e->key, sizeof(e->key), k);
    e->key_len = n > 0 ? (u32)(n - 1) : 0;
    if (n >= (long)sizeof(e->key))
        e->truncated = 1;

    e->has_value = 0;
    e->value_len = 0;
    if (v) {
        n = bpf_probe_read_user_str(e->value, sizeof(e->value), v);
        e->value_len = n > 0 ? (u32)(n - 1) : 0;
        e->has_value = 1;
        if (n >= (long)sizeof(e->value))
            e->truncated = 1;
    }
    // 写入ringbuf，给用户态读取
    bpf_ringbuf_submit(e, 0);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";