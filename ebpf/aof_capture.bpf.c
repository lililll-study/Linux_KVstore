
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

/* 写命令白名单：只有这 6 个会改变数据，读命令（HGET/SGET/HEXIST...）不转发 */
static __always_inline int is_write_cmd(const char *cmd)
{
    /* 6 个写命令都恰好 4 个字符，memcmp 5 字节 = 4 字符 + 结尾 NUL */
    if (__builtin_memcmp(cmd, "HSET", 5) == 0) return 1;
    if (__builtin_memcmp(cmd, "HDEL", 5) == 0) return 1;
    if (__builtin_memcmp(cmd, "HMOD", 5) == 0) return 1;
    if (__builtin_memcmp(cmd, "SSET", 5) == 0) return 1;
    if (__builtin_memcmp(cmd, "SDEL", 5) == 0) return 1;
    if (__builtin_memcmp(cmd, "SMOD", 5) == 0) return 1;
    return 0;
}

// 挂到网络命令统一入口 kvs_filter_protocol(int fd, char **tokens, int count, char *response)
// x86_64 寄存器映射：rdi=fd，rsi=tokens，rdx=count
// 与持久化彻底解耦：不再探测 append_aof，直接捕获网络端命令
SEC("uprobe")
int on_net_cmd(struct pt_regs *ctx)
{
    u32 key = 0;

    /* 只捕获指定进程 */
    u32 *want = bpf_map_lookup_elem(&target_pid, &key);
    if (want && *want != 0) {
        u32 cur = (u32)(bpf_get_current_pid_tgid() >> 32);
        if (cur != *want)
            return 0;
    }

    int fd    = (int)PT_REGS_PARM1(ctx);
    int count = (int)PT_REGS_PARM3(ctx);
    if (count < 1)
        return 0;

    /* tokens 是 char**（二级指针）：先读出字符串地址，再读内容 */
    char **tokens = (char **)PT_REGS_PARM2(ctx);
    if (!tokens)
        return 0;

    char *cmd_ptr = NULL;
    if (bpf_probe_read_user(&cmd_ptr, sizeof(cmd_ptr), &tokens[0]) != 0 || !cmd_ptr)
        return 0;

    char cmd[AOF_CMD_MAX];
    if (bpf_probe_read_user_str(cmd, sizeof(cmd), cmd_ptr) <= 0)
        return 0;

    /* 先判边界命令（SYNC-FINISHED 长的先判，避免被 SYNC 截断匹配） */
    unsigned char type;
    if (__builtin_memcmp(cmd, "SYNC-FINISHED", 14) == 0) {
        type = EVT_SYNC_END;
    } else if (__builtin_memcmp(cmd, "SYNC", 5) == 0) {
        type = EVT_SYNC_BEGIN;
    } else if (is_write_cmd(cmd)) {
        type = EVT_WRITE;
    } else {
        return 0;    /* 读命令 / PING / SAVE / 未知命令一律不管 */
    }

    struct aof_event *e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
    if (!e) {
        u64 *lost = bpf_map_lookup_elem(&lost_count, &key);
        if (lost)
            __sync_fetch_and_add(lost, 1);
        return 0;
    }

    e->type = type;
    e->fd   = fd;
    e->has_value = 0;
    e->truncated = 0;
    e->key_len = 0;
    e->value_len = 0;

    long n = bpf_probe_read_user_str(e->cmd, sizeof(e->cmd), cmd_ptr);
    e->cmd_len = n > 0 ? (u32)(n - 1) : 0;

    /* 只有写命令才需要读 key/value，同步边界事件直接提交 */
    if (type == EVT_WRITE) {
        if (count < 2) {
            bpf_ringbuf_discard(e, 0);
            return 0;
        }

        char *key_ptr = NULL, *value_ptr = NULL;
        if (bpf_probe_read_user(&key_ptr, sizeof(key_ptr), &tokens[1]) != 0 || !key_ptr) {
            bpf_ringbuf_discard(e, 0);
            return 0;
        }
        if (count > 2) {
            bpf_probe_read_user(&value_ptr, sizeof(value_ptr), &tokens[2]);
        }

        n = bpf_probe_read_user_str(e->key, sizeof(e->key), key_ptr);
        e->key_len = n > 0 ? (u32)(n - 1) : 0;
        if (n >= (long)sizeof(e->key))
            e->truncated = 1;

        if (value_ptr) {
            n = bpf_probe_read_user_str(e->value, sizeof(e->value), value_ptr);
            e->value_len = n > 0 ? (u32)(n - 1) : 0;
            e->has_value = 1;
            if (n >= (long)sizeof(e->value))
                e->truncated = 1;
        }
    }

    bpf_ringbuf_submit(e, 0);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";