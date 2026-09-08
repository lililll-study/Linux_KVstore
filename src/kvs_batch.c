#include "kvstore.h"


#if ENABLE_CMD_BATCH

kvs_conn_ctx_t kvs_conn_ctxs[MAX_CONN];

int kvs_ctx_init(kvs_conn_ctx_t *ctx) {
    if (!ctx) return -1;
    ctx->cmd_buffer_cap = 4096;
    ctx->cmd_buffer = (char *)malloc(ctx->cmd_buffer_cap);
    if (!ctx->cmd_buffer) return -1;
    ctx->cmd_buffer_len = 0;
    ctx->cmd_buffer[0] = '\0';


#if ENABLE_REPBUF
    // 响应缓冲区初始 4KB
    ctx->resp_cap = 4096;
    ctx->resp_buffer = (char *)malloc(ctx->resp_cap);
    if (!ctx->resp_buffer) {
        free(ctx->cmd_buffer);
        return -1;
    }
    ctx->resp_len = 0;
    ctx->resp_sent = 0;
    ctx->send_pending = 0;
    ctx->resp_buffer[0] = '\0';
#endif

    return 0;
}

void kvs_ctx_free(kvs_conn_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->cmd_buffer) {
        free(ctx->cmd_buffer);
        ctx->cmd_buffer = NULL;
    }
    ctx->cmd_buffer_len = 0;
    ctx->cmd_buffer_cap = 0;

#if ENABLE_REPBUF
    if (ctx->resp_buffer) {
        free(ctx->resp_buffer);
        ctx->resp_buffer = NULL;
    }
    ctx->resp_len = 0;
    ctx->resp_cap = 0;
    ctx->resp_sent = 0;
    ctx->send_pending = 0;
#endif
}

int kvs_ctx_expand(kvs_conn_ctx_t *ctx, int need) {
    if (ctx->cmd_buffer_cap >= need) return 0;
    
    int new_cap = ctx->cmd_buffer_cap;
    while (new_cap < need) new_cap *= 2;        // 以两倍的形式逐次递增
    
    char *new_buf = (char *)realloc(ctx->cmd_buffer, new_cap);
    if (!new_buf) return -1;
    ctx->cmd_buffer = new_buf;
    ctx->cmd_buffer_cap = new_cap;
    return 0;
}

int kvs_ctx_append(kvs_conn_ctx_t *ctx, const char *data, int len) {
    if (!ctx || !data || len <= 0) return -1;
    
    // 预留 \0 空间
    if (kvs_ctx_expand(ctx, ctx->cmd_buffer_len + len + 1) < 0) {
        return -1;
    }
    
    memcpy(ctx->cmd_buffer + ctx->cmd_buffer_len, data, len);   // 添加数据
    ctx->cmd_buffer_len += len;                                 // 添加长度
    ctx->cmd_buffer[ctx->cmd_buffer_len] = '\0';                // 添加结束符
    return 0;
}

void kvs_ctx_consume(kvs_conn_ctx_t *ctx, int consumed) {
    if (!ctx || consumed <= 0) return;
    if (consumed >= ctx->cmd_buffer_len) {
        ctx->cmd_buffer_len = 0;
        ctx->cmd_buffer[0] = '\0';
        return;
    }
    memmove(ctx->cmd_buffer, ctx->cmd_buffer + consumed, 
            ctx->cmd_buffer_len - consumed);
    ctx->cmd_buffer_len -= consumed;
    ctx->cmd_buffer[ctx->cmd_buffer_len] = '\0';
}


// 单条命令响应的最大预留空间（GET 类响应可能较长）
#define KVS_MAX_RESP_PER_CMD 512
int p_cmd_count = 0;

// ===== 处理缓冲区中的完整命令 =====
int kvs_ctx_process(int fd, kvs_conn_ctx_t *ctx, char *response, int resp_cap) {
    if (!ctx) return -1;
    
    char *line_start = ctx->cmd_buffer; // 当前行的起始位置
    int processed_bytes = 0;                  // 已处理的字节数
    
    while (processed_bytes < ctx->cmd_buffer_len) {   // 只要已经处理的字数小于缓冲区数量，就循环处理
        
        // 1. 查找换行符 
        // 从行开始到缓冲区末尾剩余的字节数，查找\n
        char *line_end = memchr(line_start, '\n', ctx->cmd_buffer_len - processed_bytes);
        // 边界条件
        if (!line_end) break;// 没找到说明无完整命令，直接跳出
        // 找到了，命令长度
        int cmd_len = line_end - line_start + 1;

        // 2. 分配本次命令空间，并把cmd复制到cmd_buf缓存
        char *cmd_temp_buf = (char *)malloc(cmd_len + 1);    // 分配本次命令的空间
        if (!cmd_temp_buf) break;
        memcpy(cmd_temp_buf, line_start, cmd_len);
        // printf("debug: cmd_temp_buf = '%s'\n", cmd_temp_buf);

        // 3. 去掉末尾的 \n 和 \r
        char *p = cmd_temp_buf + cmd_len - 1;
        while (p >= cmd_temp_buf && (*p == '\n' || *p == '\r')) {
            *p = '\0';
            p--;
        }


        #if ENABLE_REPBUF
            // 响应写入 ctx->resp_buffer，自动扩容，无需容量检查
        #else
            if (total_len + KVS_MAX_RESP_PER_CMD > resp_cap) {
                free(cmd_temp_buf);
                break;
            }
        #endif

        // 5. 如果有完整命令，处理命令
        char *tokens[KVS_MAX_TOKENS] = {0};
        int count = kvs_split_token(cmd_temp_buf, tokens);
        // printf("debug 0。5:\n");
#if ENABLE_REPBUF
        if (count > 0) {
            // printf("debug 0。3:\n");
            // 传入 NULL，不使用 response
            kvs_filter_protocol(fd, tokens, count, NULL);
            // printf("debug 0。4:\n");
        }
        free(cmd_temp_buf);
        line_start += cmd_len;
        processed_bytes += cmd_len;
    }
    if (processed_bytes > 0) {
        kvs_ctx_consume(ctx, processed_bytes);
    }
    
    return processed_bytes;
#else
        if (count > 0) {
            
            int len = kvs_filter_protocol(fd, tokens, count, response + total_len);
            if (len > 0) {
                total_len += len;
            }
        }
        p_cmd_count++;
        // 6. 处理完了数据，释放内存，前进line_start和processed_bytes
        free(cmd_temp_buf);
        line_start += cmd_len;
        processed_bytes += cmd_len;
    }
    // 7. 如果有字符被用了，就将字符从缓冲区移除
    if (processed_bytes > 0) {
        kvs_ctx_consume(ctx, processed_bytes);
        // if (ctx->cmd_buffer_len > 0) {
        //     printf("DEBUG: After consume, remaining in buffer: %d bytes\n", ctx->cmd_buffer_len);
        //     printf("DEBUG: remaining content: %s\n", ctx->cmd_buffer);
        // } else {
        //     printf("DEBUG: Buffer is empty after consume\n");
        // }
    }
    
    return total_len;
#endif
}

void cleanup_connection(int fd) {
    if (fd >= 0 && fd < MAX_CONN) {
        kvs_ctx_free(&kvs_conn_ctxs[fd]);
    }
}

#endif


#if ENABLE_REPBUF
//往缓冲区写入数据
// 计算需要的大小 need = resp_len + len + 1
int resp_buf_append(kvs_conn_ctx_t *ctx, const char *data, int len) {
    if (!ctx || !data || len <= 0) return -1;
    
    int need = ctx->resp_len + len + 1;
    if (need > ctx->resp_cap) {
        int new_cap = ctx->resp_cap;
        if (new_cap == 0) new_cap = 4096;
        while (new_cap < need) new_cap *= 2;
        char *new_buf = (char *)realloc(ctx->resp_buffer, new_cap);
        if (!new_buf) return -1;
        ctx->resp_buffer = new_buf;
        ctx->resp_cap = new_cap;
    }
    
    memcpy(ctx->resp_buffer + ctx->resp_len, data, len);
    ctx->resp_len += len;
    ctx->resp_buffer[ctx->resp_len] = '\0';
    return 0;
}

//从缓冲区取出数据，out_len告诉调用者数据长度
char* resp_buf_get_data(kvs_conn_ctx_t *ctx, int *out_len) {
    if (!ctx || !out_len) return NULL;
    if (ctx->resp_sent >= ctx->resp_len) {
        *out_len = 0;
        return NULL;
    }
    *out_len = ctx->resp_len - ctx->resp_sent;
    return ctx->resp_buffer + ctx->resp_sent;
}
// 标记数据已发送
void resp_buf_advance(kvs_conn_ctx_t *ctx, int sent_len) {
    if (!ctx || sent_len <= 0) return;
    ctx->resp_sent += sent_len;
    if (ctx->resp_sent >= ctx->resp_len) {
        ctx->resp_len = 0;
        ctx->resp_sent = 0;
    }
}
// 判断是否还有数据
int resp_buf_has_data(kvs_conn_ctx_t *ctx) {
    return ctx && ctx->resp_sent < ctx->resp_len;
}
#endif