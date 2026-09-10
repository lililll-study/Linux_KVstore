#include "kvstore.h"


#if ENABLE_CMD_BATCH

// ===== RESP 解析状态枚举 =====
enum {
    RESP_TOTAL_HEAD = 0,      // 读 *N\r\n (数组头)
    RESP_CMD_HEAD,           // 读 *N\r\n (命令头)
    RESP_TOKEN,         // 读 $L\r\n (token头)
    RESP_DATA           // 读数据内容
};

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

    // ===== 补齐 RESP 解析器初始化 =====
    ctx->resp_state = RESP_TOTAL_HEAD;
    ctx->resp_cmd_num = 0;
    ctx->resp_cmd_idx = 0;
    ctx->resp_scmd_unit_num = 0;
    ctx->resp_scmd_unit_idx = 0;
    ctx->resp_token_len = 0;
    ctx->resp_temp_buffer = NULL;
    ctx->resp_temp_buffer_cap = 0;
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
    // ===== 释放 RESP 临时缓冲区 =====
    if (ctx->resp_temp_buffer) {
        free(ctx->resp_temp_buffer);
        ctx->resp_temp_buffer = NULL;
    }
    ctx->resp_temp_buffer_cap = 0;
    ctx->resp_state = RESP_TOTAL_HEAD;
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





/*
// 解析器收到的数据
*2\r\n                          // ← 外层数组，2个元素
*3\r\n $3\r\nSET\r\n $1\r\na\r\n $1\r\n1\r\n  // ← 元素1：数组
*2\r\n $3\r\nGET\r\n $1\r\na\r\n             // ← 元素2：数组
*/



/**
 * @param supcall: kvs_ctx_process循环处理cmd_buffer中的数据
 * @param locall: 从缓冲区cmd_buffer取出的数据，解析得到完整cmd并存入
 * @param input: 
 * @param output: 
 */
// int resp_parse_cmd(char *msg, char *tokens[]) {


// }

/**
 * 解析 RESP 协议中的一行：*N\r\n 或 $N\r\n
 * @param data      数据起始位置
 * @param remain    剩余数据长度
 * @param prefix    期望的前缀字符（'*' 或 '$'）
 * @param out_num   输出：解析出的数字
 * @param out_consumed 输出：消耗的字节数
 * @return 0=成功, 1=数据不完整, -1=错误
 */
static int parse_resp_line(const char *data, int remain, char prefix, 
                           int *out_num, int *out_consumed) {
    // 1. 检查是否有足够数据
    if (remain < 4) return 1;  // 至少需要 "x1\r\n"
    
    // 2. 检查前缀
    if (data[0] != prefix) {
        printf("[RESP] ERROR: expected '%c', got '%c'\n", prefix, data[0]);
        return -1;
    }
    
    // 3. 解析数字
    const char *p = data + 1;
    const char *num_start = p;
    int num_len = 0;
    while (p < data + remain && (*p >= '0' && *p <= '9')) {
        num_len++;
        p++;
    }
    
    if (num_len == 0) {
        printf("[RESP] ERROR: no number after '%c'\n", prefix);
        return -1;
    }
    
    // 4. 检查 \r\n
    if (p >= data + remain || *p != '\r') return 1;  // 数据不完整
    if (p + 1 >= data + remain || *(p + 1) != '\n') return 1;
    
    // 5. 提取数字
    char num_buf[32];
    if (num_len >= (int)sizeof(num_buf)) {
        printf("[RESP] ERROR: number too long\n");
        return -1;
    }
    memcpy(num_buf, num_start, num_len);
    num_buf[num_len] = '\0';
    *out_num = atoi(num_buf);
    *out_consumed = (p + 2) - data;  // 消耗的字节数（含 \r\n）
    
    return 0;
}


/**
 * 把 token 追加到 resp_temp_buffer
 * @param ctx       连接上下文
 * @param token     要追加的 token 数据
 * @param token_len token 长度
 * @return 0=成功, -1=失败
 */
static int append_token(kvs_conn_ctx_t *ctx, const char *token, int token_len) {
    if (!ctx->resp_temp_buffer) return -1;
    
    size_t cur = strlen(ctx->resp_temp_buffer);
    size_t need = cur + token_len + 2;  // +2 是空格和 \0
    
    // 扩容检查
    if (need >= (size_t)ctx->resp_temp_buffer_cap) {
        while (need >= (size_t)ctx->resp_temp_buffer_cap) {
            ctx->resp_temp_buffer_cap *= 2;
        }
        char *nb = realloc(ctx->resp_temp_buffer, ctx->resp_temp_buffer_cap);
        if (!nb) return -1;
        ctx->resp_temp_buffer = nb;
    }
    
    // 拼接
    if (cur > 0) {
        strncat(ctx->resp_temp_buffer, " ", ctx->resp_temp_buffer_cap - cur - 1);
    }
    strncat(ctx->resp_temp_buffer, token, 
            ctx->resp_temp_buffer_cap - strlen(ctx->resp_temp_buffer) - 1);
    
    return 0;
}

/**
 * 分配 resp_temp_buffer
 * @param ctx 连接上下文
 * @return 0=成功, -1=失败
 */
static int alloc_temp_buffer(kvs_conn_ctx_t *ctx) {
    if (ctx->resp_temp_buffer) {
        free(ctx->resp_temp_buffer);
        ctx->resp_temp_buffer = NULL;
    }
    
    ctx->resp_temp_buffer_cap = 1024;
    ctx->resp_temp_buffer = malloc(ctx->resp_temp_buffer_cap);
    if (!ctx->resp_temp_buffer) return -1;
    
    ctx->resp_temp_buffer[0] = '\0';
    return 0;
}


/**
 * @param supcall: kvs_protocol接收数据并append进cmd_buffer缓冲区
 * @param locall: 从cmd_buffer中循环接收数据并交由resp parse函数处理
 * @param input: fd连接描述符、ctx指定连接的上下文信息、response（无作用）、resp_cap（无作用）
 * @param output: 输出处理过的字节数，判断命令是否被处理完
 */
int kvs_ctx_process(int fd, kvs_conn_ctx_t *ctx, char *response, int resp_cap) {
    if (!ctx) return -1;
    
    char *data = ctx->cmd_buffer;           // 数据的起始位置
    int remain = ctx->cmd_buffer_len;  // 未处理的数据长度
    int processed_data = 0;                 // 已处理的数据长度
    printf("[DEBUG] kvs_ctx_process: cmd_buffer_len=%d, data='%.*s'\n", 
       ctx->cmd_buffer_len, ctx->cmd_buffer_len, ctx->cmd_buffer);

    while (processed_data < remain) {   // 只要已经处理的字数小于缓冲区数量，就循环处理

#if 1
        char *p = data + processed_data;    // 当前未处理的数据位置
        int left = remain - processed_data; // 当前剩余没处理的数据

        // 1. 检测开头*xxx\r\n，把总cmd数量拿到，此时正在识别开头
        if (ctx->resp_state == RESP_TOTAL_HEAD) {
            printf("debug RESP_TOTAL_HEAD\n");
            // 2. 如果没检测到直接break，说明数据不够，等更多数据再检测（防止分包粘包问题）
            if (left < 4) break;
            if (p[0] != '*') {//如果起始不是*说明数据错误
                printf("[RESP1] ERROR: cmd_total_head is wrong");
                break;
            }
            // 3. 如果检测到，就把cmd_num记录下来
            p++; // 跳过*
            char *num_start = p;
            int num_len = 0;
            // 只要p没越界且是数字0-9，就后移
            while (p < data + remain && (*p >= '0' && *p <= '9')) {
                num_len++;
                p++;
            }

            // 如果没有cmd长度，报错并停止循环
            if (num_len == 0) {
                printf("[RESP1] ERROR: cmd_num is null\n");
                break;
            }

            // 如果不是\r\n就等待更多数据
            if (p >= data + remain || *p != '\r') {
                break;  // 数据不完整，等待更多
            }
            if (p + 1 >= data + remain || *(p + 1) != '\n') {
                break;  // 数据不完整，等待更多
            }

            // 提取数字
            char cmd_num_buf[32];
            memcpy(cmd_num_buf, num_start, num_len);
            cmd_num_buf[num_len] = '\0'; // 确保字符串以结束符结尾
            ctx->resp_cmd_num = atoi(cmd_num_buf);// 把头值存入cmd_num

            // printf("debug resp_cmd_num: %d\n", ctx->resp_cmd_num);
            // 并更新已处理内容
            p += 2; //跳过\r\n
            processed_data = p - data; // 处理过的数据
            ctx->resp_cmd_idx = 0;// 当前准备处理第1个命令
            ctx->resp_state = RESP_CMD_HEAD;    // 可用处理命令头了
        }

        // 4. 然后找第一个cmd，同样检测头*y\r\n，y代表该cmd的token个数，此处循环检测x次
        if (ctx->resp_state == RESP_CMD_HEAD) {
            p = data + processed_data; // 当前数据起始位置
            left = remain - processed_data; // 当前未处理的数据
            // 5. 如果没找到也break等数据
            if (left < 4) break; // 如果剩余数据少于4个，那么继续攒数据
            if (p[0] != '*') {//如果起始不是*说明数据错误
                printf("[RESP2] ERROR: cmd_head is wrong");
                break;
            }

            // 3. 如果检测到，就把单条single cmd unit的数量记录下来
            p++; // 跳过*
            char *num_start = p;
            int num_len = 0;
            // 只要p没越界且是数字0-9，就后移
            while (p < data + remain && (*p >= '0' && *p <= '9')) {
                num_len++;
                p++;
            }

            // 如果没有cmd长度，报错并停止循环
            if (num_len == 0) {
                printf("[RESP2] ERROR: cmd_unit num is wrong");
                break;
            }

            // 如果不是\r\n就等待更多数据
            if (p >= data + remain || *p != '\r') {
                break;  // 数据不完整，等待更多
            }
            if (p + 1 >= data + remain || *(p + 1) != '\n') {
                break;  // 数据不完整，等待更多
            }
            // 提取数字
            char cmd_num_buf[32];
            memcpy(cmd_num_buf, num_start, num_len);
            cmd_num_buf[num_len] = '\0'; // 确保字符串以结束符结尾
            ctx->resp_scmd_unit_num = atoi(cmd_num_buf);// 把头值存入resp_sgl_cmd_unit_count
            ctx->resp_scmd_unit_idx = 0;    // 准备处理第一个元素
            printf("debug resp_scmd_unit_num: %d\n", ctx->resp_scmd_unit_num);
            // 跳过 \r\n
            p += 2;
            processed_data = p - data;

            // 分配resp_temp_buffer
            if (ctx->resp_temp_buffer) {
                free(ctx->resp_temp_buffer);
                ctx->resp_temp_buffer = NULL;
            }
            ctx->resp_temp_buffer_cap = 1024;
            ctx->resp_temp_buffer = malloc(ctx->resp_temp_buffer_cap);
            if (!ctx->resp_temp_buffer) return -1;
            ctx->resp_temp_buffer[0] = '\0';
            ctx->resp_state = RESP_TOKEN;
        }

        // 6. 如果找到了，检测token头$z\r\n，z代表输入的字节数
        if (ctx->resp_state == RESP_TOKEN) {
            p = data + processed_data;
            left = remain - processed_data;
            if (left < 4) break; // 如果剩余数据少于4个，那么继续攒数据
            if (p[0] != '$') {//如果起始不是$说明数据错误
                printf("[RESP2] ERROR: token_head is wrong");
                break;
            }
            p++;  // 跳过 $
            char *num_start = p;
            int num_len = 0;
            while (p < data + remain && (*p >= '0' && *p <= '9')) {
                num_len++;
                p++;
            }
            if (num_len == 0) {
                printf("[RESP3] ERROR: token_len is null\n");
                break;
            }
            if (p >= data + remain || *p != '\r') break;
            if (p + 1 >= data + remain || *(p + 1) != '\n') break;

            char token_len_buf[32];
            memcpy(token_len_buf, num_start, num_len);
            token_len_buf[num_len] = '\0';
            ctx->resp_token_len = atoi(token_len_buf);

            // 跳过 \r\n
            p += 2;
            processed_data = p - data;
            ctx->resp_state = RESP_DATA;
        }

        // 开始存储数据进resp_temp_buffer：SET a 1
        if (ctx->resp_state == RESP_DATA) {
            p = data + processed_data;
            left = remain - processed_data;

            // 如果剩余数据小于token长度+2,说明数据不够
            if (left < ctx->resp_token_len + 2) break;

            // 创建一个token内存
            char *token = malloc(ctx->resp_token_len + 1);
            if (!token) return -1;
            memcpy(token, p, ctx->resp_token_len); // 把从p开始的resp_token_len长度数据拷贝到token中
            token[ctx->resp_token_len] = '\0';  // 确保token以\0结尾

            // 把数据追加到resp_temp_buffer
            if (ctx->resp_temp_buffer) {
                size_t cur = strlen(ctx->resp_temp_buffer);
                size_t need = cur + ctx->resp_token_len + 2;
                // 如果需求的内存比临时缓冲区的容量大，那么扩大容量
                if (need >= (size_t)ctx->resp_temp_buffer_cap) {
                    while (need >= (size_t) ctx->resp_temp_buffer_cap) {
                        ctx->resp_temp_buffer_cap *= 2;
                    }
                    char *nb = realloc(ctx->resp_temp_buffer, ctx->resp_temp_buffer_cap);
                    if (!nb) {free(token); return -1;}
                    ctx->resp_temp_buffer = nb;
                }
                // 如果当前resp_temp_buffer不为空，就把token拼接到一起
                if (cur > 0) {
                    strncat(ctx->resp_temp_buffer, " ", ctx->resp_temp_buffer_cap - cur - 1);
                }
                strncat(ctx->resp_temp_buffer, token, ctx->resp_temp_buffer_cap - strlen(ctx->resp_temp_buffer) - 1);
            }
            free(token);
            // 跳过token_len 的数据后再跳过 \r\n
            p += ctx->resp_token_len + 2;
            processed_data = p - data;
            ctx->resp_scmd_unit_idx++;

            // 检查当前单条命令是否完成：如果完成了，再判断所有cmd是否执行完
            // 如果所有的都执行完了，那么本次批处理结束，置位所有结构体状态，并读下一次RESP_TOTAL_HEAD
            // 如果所有没读完，单条读完了，就执行单条的，并读下一个cmd 的RESP_CMD_HEAD
            if (ctx->resp_scmd_unit_idx >= ctx->resp_scmd_unit_num){
                // 执行命令（用 resp_temp_buffer）
                if (ctx->resp_temp_buffer && strlen(ctx->resp_temp_buffer) > 0) {
                    char *tokens[KVS_MAX_TOKENS] = {0};
                    char *saveptr;
                    char *token_str = strtok_r(ctx->resp_temp_buffer, " ", &saveptr);
                    int i = 0;
                    while (token_str && i < KVS_MAX_TOKENS) {
                        tokens[i++] = token_str;
                        token_str = strtok_r(NULL, " ", &saveptr);
                    }
                    kvs_filter_protocol(fd, tokens, i, NULL);
                }
                
                // 释放 resp_temp_buffer
                if (ctx->resp_temp_buffer) {
                    free(ctx->resp_temp_buffer);
                    ctx->resp_temp_buffer = NULL;
                    ctx->resp_temp_buffer_cap = 0;
                }
                
                ctx->resp_cmd_idx++;
                if (ctx->resp_cmd_idx >= ctx->resp_cmd_num) {
                    ctx->resp_state = RESP_TOTAL_HEAD;
                    ctx->resp_cmd_num = 0;
                    ctx->resp_cmd_idx = 0;
                } else {
                    ctx->resp_state = RESP_CMD_HEAD;
                }
            } else {
                ctx->resp_state = RESP_TOKEN;
            }
        }
    }
printf("[DEBUG] kvs_ctx_process: processed_data=%d\n", processed_data);
        // 8. 此处引入状态机，如果检测完开头，就把状态机置位，如果数据不够等下一次数据来了，直接检测不同状态，例如head，cmd，token
        //      这样即使token因为分包问题没收完退出循环后，下一次接着token解析的循环进行处理，不影响头和cmd个数的解析。
        
#else
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


        // 5. 如果有完整命令，处理命令
        char *tokens[KVS_MAX_TOKENS] = {0};
        int count = kvs_split_token(cmd_temp_buf, tokens);
        // printf("debug 0。5:\n");

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

