#ifndef KVS_URING_H
#define KVS_URING_H

#include <stddef.h>
#include <sys/types.h>

typedef struct kvs_uring kvs_uring_t;

int kvs_uring_init(kvs_uring_t **out, int fd, off_t initial_offset);
int kvs_uring_append(kvs_uring_t *ctx, const void *data, size_t len);
/* 截断到 base 偏移后追加 data，再追加 tail（用于 AOF 重写文件尾 CRC） */
int kvs_uring_append_cmd(kvs_uring_t *ctx, const void *data, size_t len, const void *tail, size_t tail_len, off_t base);
int kvs_uring_flush(kvs_uring_t *ctx);
int kvs_uring_reset(kvs_uring_t *ctx);
void kvs_uring_shutdown(kvs_uring_t *ctx);

#endif
