#ifndef AOF_CAPTURE_H
#define AOF_CAPTURE_H

/* 固定长度上限：ringbuf 事件必须是固定大小 */
#define AOF_CMD_MAX   16
#define AOF_KEY_MAX   256
#define AOF_VALUE_MAX 1024

struct aof_event {
    char cmd[AOF_CMD_MAX];
    char key[AOF_KEY_MAX];
    char value[AOF_VALUE_MAX];

    unsigned int cmd_len;     /* 不含结尾 NUL */
    unsigned int key_len;
    unsigned int value_len;

    unsigned char has_value;  /* DEL 类命令为 0 */
    unsigned char truncated;  /* 任一字段超长被截断 */
    unsigned char _pad[2];
};

#endif