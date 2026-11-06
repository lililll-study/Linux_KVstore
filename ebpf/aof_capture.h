#ifndef AOF_CAPTURE_H
#define AOF_CAPTURE_H

// 共享时间结构体
// 定义了内核 BPF 程序与用户态程序之间传递事件的数据结构

/* 固定长度上限：ringbuf 事件必须是固定大小 */
#define AOF_CMD_MAX   16    // 命令名最长（如 "SET"、"DEL"）
#define AOF_KEY_MAX   256   // key 最长
#define AOF_VALUE_MAX 1024  // value 最长

/* 事件类型：ringbuf 里同时承载"数据"和"同步边界"两种语义 */
#define EVT_WRITE       0   /* 普通写命令 */
#define EVT_SYNC_BEGIN  1   /* 主机收到从机 SYNC：全量同步开始 */
#define EVT_SYNC_END    2   /* 主机收到从机 SYNC-FINISHED：全量同步结束 */
#define EVT_SAVE        3   /* SAVE 命令：无 key/value，从机收到后也落盘一次 */

struct aof_event {
    char cmd[AOF_CMD_MAX];
    char key[AOF_KEY_MAX];
    char value[AOF_VALUE_MAX];

    unsigned int cmd_len;     /* 不含结尾 NUL */
    unsigned int key_len;
    unsigned int value_len;

    unsigned char type;       /* EVT_*：本事件是数据还是同步边界 */
    unsigned char has_value;  /* DEL 类命令没有 value，为 0 */
    unsigned char truncated;  /* 任一字段超长被截断 */
    unsigned char _pad;
    int fd;                   /* 事件来源连接：SYNC 事件用它登记从机增量通道 */
};

#endif