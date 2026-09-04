
#include "server.h"
#include "kvstore.h"

#if ENABLE_ARRAY
extern kvs_array_t global_array;
#endif

#if ENABLE_RBTREE
extern kvs_rbtree_t global_rbtree;
#endif

#if ENABLE_HASH
extern kvs_hash_t global_hash;
#endif

#if ENABLE_SKIPTABLE
extern kvs_skiptable_t global_skiptable;
#endif

#if ENABLE_MEMPOOL
#define KVS_POOL_NODE_COUNT   100000
#define KVS_POOL_STRING_COUNT 200000

// 定义内存池
 mempool_t *array_item_pool = NULL;     // 数组节点池
 mempool_t *array_string_pool = NULL;   // 数组字符串池

 mempool_t *rbtree_node_pool = NULL;    // 红黑树节点池
 mempool_t *rbtree_string_pool = NULL;  // 红黑树字符串池

 mempool_t *hash_node_pool = NULL;      // 哈希节点池
 mempool_t *hash_string_pool = NULL;    // 哈希字符串池

 mempool_t *skiptable_node_pool = NULL; // 跳表节点池
 mempool_t *skiptable_string_pool = NULL; // 跳表字符串池

// 指定块大小和块数量 
mempool_t* kvs_mempool_create(size_t user_block_size, size_t num_blocks) {
    if (user_block_size == 0 || num_blocks == 0) {
        return NULL;
    }

    // 每个块至少要能容纳一个指针
    // 块在空闲的时候，前8字节存下一个块的地址
    // 块被分配后，前8字节用户看到不，只能感知到实际使用空间
    size_t actual_block_size = user_block_size + sizeof(void *); // 块大小
    // 关键修复：对齐到 8 字节边界（或指针大小）
    // 这样确保每个块起始地址对齐，避免内存访问问题
    size_t alignment = sizeof(void *);
    actual_block_size = (actual_block_size + alignment - 1) & ~(alignment - 1);
    size_t pool_size = sizeof(mempool_t) + actual_block_size * num_blocks; // 池大小

    // 分配内存池结构体及内存块空间
    mempool_t* pool = (mempool_t*)malloc(pool_size);
    if (!pool) return NULL;

    // 初始化内存池
    pool->block_size = actual_block_size;
    pool->total_blocks = num_blocks;
    pool->free_blocks = num_blocks;
    pool->free_list = NULL;

    // 初始化空闲链表
    size_t i = 0;
    for (i=0; i<num_blocks; ++i) {
        // block 是一个指针，指向某个块的起始地址
        void* block = pool->data + i * actual_block_size; 
        // 得到块起始地址处存储的内容 ,这一段就是把pool->free_list的值（开始为null）写入块起始地址
        *((void**)block) = pool->free_list; // 此时block0存储null
        pool->free_list = block;    // free_list = block0
    }
    // printf("  mempool created: block_size=%zu, total_blocks=%zu\n",  actual_block_size, num_blocks);
    return pool;
}
// 分配块
void* kvs_mempool_alloc(mempool_t *pool, size_t size) {
    if (!pool || !pool->free_list || pool->free_blocks == 0) {
        return NULL;
    }
#if ENABLE_LEVEL_MEMPOOL

    // 计算用户可用空间（块大小 - 指针头）
    size_t user_capacity = pool->block_size - sizeof(void *);
    
    // 如果请求大小超过块容量，直接用 malloc
    if (size > user_capacity) {
        // printf("  WARN: size %zu > block capacity %zu, using malloc\n", size, user_capacity);
        return kvs_malloc(size);
    }
#endif
    void* block = pool->free_list;
    pool->free_list = *((void**)block);
    // 检查请求大小
    if (size > pool->block_size - sizeof(void*)) {
        // printf("  ERROR: size %zu > block capacity %zu\n",  size, pool->block_size - sizeof(void*));
        return NULL;
    }
    pool->free_blocks--;

    // 返回用户可用的内存起始地址
    void* user_ptr = (char*)block + sizeof(void*);
    // printf("  alloc success: user_ptr=%p\n", user_ptr);
    
    return user_ptr;
}
// 释放块
void kvs_mempool_free(mempool_t *pool, void *ptr) {
    if (!pool || !ptr) {
        return;
    }

#if ENABLE_LEVEL_MEMPOOL
    // 检查指针是否在内存池范围内
    void* block_start = (void*)pool->data;
    void* block_end = (void*)(pool->data + pool->total_blocks * pool->block_size);
    void* block = (char*)ptr - sizeof(void*);
    // 如果指针在池范围内，用池释放
    if (block >= block_start && block < block_end) {
        size_t offset = (char*)block - (char*)block_start;
        if (offset % pool->block_size == 0) {
            *((void**)block) = pool->free_list;
            pool->free_list = block;
            pool->free_blocks++;
            return;
        }
    }
    // 否则用 free（来自 malloc）
    kvs_free(ptr);
#else
    // 回退8字节，得到要释放块的起始地址
    void* block = (char*)ptr - sizeof(void*);

    // 在待释放块的头写入当前链表头（第一个空闲块）
    *((void**)block) = pool->free_list;
    // 更新free_list头，指向新插入的块
    pool->free_list = block;
    pool->free_blocks++;
#endif
}
// 重置块
void kvs_mempool_reset(mempool_t *pool) {
    if (!pool) {
        return;
    }

    // 重新构建空闲链表
    pool->free_blocks = pool->total_blocks;
    pool->free_list = NULL;
    size_t i = 0;
    for (i = 0; i < pool->total_blocks; ++i) {
        void* block = pool->data + i * pool->block_size;
        *((void**)block) = pool->free_list;
        pool->free_list = block;
    }
}
// 销毁内存池
void kvs_mempool_destroy(mempool_t *pool) {
    if (pool) {
        free(pool);
    }
}

// 大字符串分配函数

// 
int init_mempools(void) {
    // 数组：1024个item，2048个字符串（key+value）
    array_item_pool = kvs_mempool_create(sizeof(kvs_array_item_t), KVS_POOL_NODE_COUNT);
    array_string_pool = kvs_mempool_create(256, KVS_POOL_STRING_COUNT);
    if (!array_item_pool || !array_string_pool){
        // printf("Failed to create array_item_pool\n");
        return -1;
    }
    
    // 红黑树：1024个节点，2048个字符串
    rbtree_node_pool = kvs_mempool_create(sizeof(rbtree_node), KVS_POOL_NODE_COUNT);
    rbtree_string_pool = kvs_mempool_create(256, KVS_POOL_STRING_COUNT);
    if (!rbtree_node_pool || !rbtree_string_pool){
        // printf("Failed to create rbtree_node_pool\n");
        return -1;
    }
    
    // 哈希：1024个节点，2048个字符串
    hash_node_pool = kvs_mempool_create(sizeof(hashnode_t), KVS_POOL_NODE_COUNT);
    hash_string_pool = kvs_mempool_create(256, KVS_POOL_STRING_COUNT);
    if (!hash_node_pool || !hash_string_pool){
        // printf("Failed to create hash_node_pool\n");
        return -1;
    }
    
    // 跳表：1024个节点（包含forward数组），2048个字符串
    skiptable_node_pool = kvs_mempool_create(
        sizeof(kvs_node_t) + (MAX_LEVEL + 1) * sizeof(kvs_node_t*),
        KVS_POOL_NODE_COUNT
    );
    skiptable_string_pool = kvs_mempool_create(256, KVS_POOL_STRING_COUNT);
    if (!skiptable_node_pool || !skiptable_string_pool){
        // printf("Failed to create skiptable_node_pool\n");
        return -1;
    }
    
    return 0;
}

// 销毁内存池
void dest_mempools(void) {
    if (array_item_pool) kvs_mempool_destroy(array_item_pool);
    if (array_string_pool) kvs_mempool_destroy(array_string_pool);
    if (rbtree_node_pool) kvs_mempool_destroy(rbtree_node_pool);
    if (rbtree_string_pool) kvs_mempool_destroy(rbtree_string_pool);
    if (hash_node_pool) kvs_mempool_destroy(hash_node_pool);
    if (hash_string_pool) kvs_mempool_destroy(hash_string_pool);
    if (skiptable_node_pool) kvs_mempool_destroy(skiptable_node_pool);
    if (skiptable_string_pool) kvs_mempool_destroy(skiptable_string_pool);
}
#endif


#if ENABLE_CMD_BATCH

kvs_conn_ctx_t kvs_conn_ctxs[MAX_CONN];

// 初始化
int kvs_ctx_init(kvs_conn_ctx_t *ctx) {
    if (!ctx) return -1;
    ctx->cmd_buffer_cap = 4096;
    ctx->cmd_buffer = (char *)malloc(ctx->cmd_buffer_cap);
    if (!ctx->cmd_buffer) return -1;
    ctx->cmd_buffer_len = 0;
    ctx->cmd_buffer[0] = '\0';
    return 0;
}

// 释放
void kvs_ctx_free(kvs_conn_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->cmd_buffer) {
        free(ctx->cmd_buffer);
        ctx->cmd_buffer = NULL;
    }
    ctx->cmd_buffer_len = 0;
    ctx->cmd_buffer_cap = 0;
}

// 动态扩展缓冲区
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

// 追加数据到缓冲区
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

// 移除已处理的数据
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
    if (!ctx || !response) return -1;
    
    int total_len = 0;
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
        

        // 3. 去掉末尾的 \n 和 \r
        char *p = cmd_temp_buf + cmd_len - 1;
        while (p >= cmd_temp_buf && (*p == '\n' || *p == '\r')) {
            *p = '\0';
            p--;
        }


        // 4. 当响应缓冲区数据不足，本条命令不执行@@@
        if (total_len + KVS_MAX_RESP_PER_CMD > resp_cap) {
            free(cmd_temp_buf); // 释放本次临时空间
            break;
        }

        // 5. 如果有完整命令，处理命令
        char *tokens[KVS_MAX_TOKENS] = {0};
        int count = kvs_split_token(cmd_temp_buf, tokens);
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
}

void cleanup_connection(int fd) {
    if (fd >= 0 && fd < MAX_CONN) {
        kvs_ctx_free(&kvs_conn_ctxs[fd]);
    }
}

#endif



#if ENABLE_SYNC
int g_is_master = 1;  // 1: 主节点, 0: 从节点
#endif

void *kvs_malloc (size_t size) {
    return malloc(size);
}

void kvs_free(void *ptr) {
    return free(ptr);
}


const char *command[] = {
    "SET", "GET", "DEL", "MOD", "EXIST",
    "RSET", "RGET", "RDEL", "RMOD", "REXIST",
    "HSET", "HGET", "HDEL", "HMOD", "HEXIST",
    "SSET", "SGET", "SDEL", "SMOD", "SEXIST",
    "SAVE", "SYNC"

};

enum {
    KVS_CMD_START = 0,
    //array
    KVS_CMD_SET = KVS_CMD_START,
    KVS_CMD_GET,
    KVS_CMD_DEL,
    KVS_CMD_MOD,
    KVS_CMD_EXIST,
    // rbtree
    KVS_CMD_RSET,
    KVS_CMD_RGET,
    KVS_CMD_RDEL,
    KVS_CMD_RMOD,
    KVS_CMD_REXIST,
	// hash
	KVS_CMD_HSET,
	KVS_CMD_HGET,
	KVS_CMD_HDEL,
	KVS_CMD_HMOD,
	KVS_CMD_HEXIST,
	// skiplist
	KVS_CMD_SSET,
	KVS_CMD_SGET,
	KVS_CMD_SDEL,
	KVS_CMD_SMOD,
	KVS_CMD_SEXIST,

    KVS_CMD_SAVE,
    KVS_CMD_SYNC,
	KVS_CMD_COUNT,

};

const char *response[] = {};


int check_datafile_exist() {
    struct stat st = {0};
    if (stat("data", &st) == -1) {
        printf("Error: data directory not exist. Please create it.\n");
        return -1;
    }
    return 0;
}

int kvs_split_token(char *msg, char *tokens[]) {

    if (msg == NULL || tokens == NULL)return -1;
    int idx = 0;
    char *token = strtok(msg, " ");

    while (token != NULL) {
        // printf("idx: %d, %s\n", idx, token);
        tokens[idx ++] = token;
        token = strtok(NULL, " ");
    }

    return idx;

}

int kvs_filter_protocol(int fd, char **tokens, int count, char *response) {
// SET KEY VALUE
// tokens[0]: SET ; tokens[1]: KEY; tokens[0]: VALUE
// 进行协议数据过滤
    if (tokens == NULL || count == 0 || response == NULL) return -1;

    int cmd = KVS_CMD_START;
    for (cmd = KVS_CMD_START; cmd < KVS_CMD_COUNT; cmd++) {
        if (strcmp(tokens[0], command[cmd]) == 0 ) {
            break;
        }
    }

    int len = 0;
    int ret = 0;
    char *key = tokens[1];
    char *value = tokens[2];
    char *result = 0;

    switch (cmd) {
#if ENABLE_ARRAY
        case KVS_CMD_SET:
            ret = kvs_array_set(&global_array, key, value);
            if (ret < 0) {
                len = sprintf(response, "set error\r\n");
            } else if (ret == 0) {
                append_aof(AOF_ARRAY_FILE, "SET", key, value);  // 追加AOF
                len = sprintf(response, "ok\r\n");
            } else {
                len = sprintf(response, "exist\r\n");
            }
            break;

        case KVS_CMD_GET:{
            result = kvs_array_get(&global_array, key);
            if (result == NULL) {
                len = sprintf(response, "no exist\r\n");
            } else {
                len = sprintf(response, "%s\r\n", result);
            }
            break;
        }
        case KVS_CMD_DEL:
            ret = kvs_array_del(&global_array, key);
            if (ret < 0) {
                len = sprintf(response, "error\r\n");
            } else if (ret == 0) {
                append_aof(AOF_ARRAY_FILE, "DEL", key, NULL);  // 追加AOF
                len = sprintf(response, "ok\r\n");
            } else {
                len = sprintf(response, "no exist\r\n");
            }
            break;

        case KVS_CMD_MOD:
            ret = kvs_array_mod(&global_array, key, value);
            if (ret < 0) {
                len = sprintf(response, "error\r\n");
            } else if (ret == 0) {
                append_aof(AOF_ARRAY_FILE, "MOD", key, value);  // 追加AOF
                len = sprintf(response, "ok\r\n");
            } else {
                len = sprintf(response, "no exist\r\n");
            }
            break;
        case KVS_CMD_EXIST:
            ret = kvs_array_exist(&global_array, key);
            if (ret == 0) {
                len = sprintf(response, "exist\r\n");
            } else {
                len = sprintf(response, "no exist\r\n");
            }
            break;
#endif
#if ENABLE_RBTREE
        case KVS_CMD_RSET:
            ret = kvs_rbtree_set(&global_rbtree, key, value);
            if (ret < 0) {
                len = sprintf(response, "set error\r\n");
            } else if (ret == 0) {
                append_aof(AOF_RBTREE_FILE, "RSET", key, value);  // 追加AOF
                len = sprintf(response, "ok\r\n");
            } else {
                len = sprintf(response, "exist\r\n");
            }
            break;

        case KVS_CMD_RGET:{
            char *result = kvs_rbtree_get(&global_rbtree, key);
            if (result == NULL) {
                len = sprintf(response, "no exist\r\n");
            } else {
                len = sprintf(response, "%s\r\n", result);
            }
            break;
        }
        case KVS_CMD_RDEL:
            ret = kvs_rbtree_del(&global_rbtree, key);
            if (ret < 0) {
                len = sprintf(response, "error\r\n");
            } else if (ret == 0) {
                append_aof(AOF_RBTREE_FILE, "RDEL", key, NULL);  // 追加AOF
                len = sprintf(response, "ok\r\n");
            } else {
                len = sprintf(response, "no exist\r\n");
            }
            break;

        case KVS_CMD_RMOD:
            ret = kvs_rbtree_mod(&global_rbtree, key, value);
            if (ret < 0) {
                len = sprintf(response, "error\r\n");
            } else if (ret == 0) {
                append_aof(AOF_RBTREE_FILE, "RMOD", key, value);  // 追加AOF
                len = sprintf(response, "ok\r\n");
            } else {
                len = sprintf(response, "no exist\r\n");
            }
            break;
        case KVS_CMD_REXIST:
            ret = kvs_rbtree_exist(&global_rbtree, key);
            if (ret == 0) {
                len = sprintf(response, "exist\r\n");
            } else {
                len = sprintf(response, "no exist\r\n");
            }
            break;
#endif
#if ENABLE_HASH
        case KVS_CMD_HSET:
            ret = kvs_hash_set(&global_hash ,key, value);
            if (ret < 0) {
                len = sprintf(response, "error\r\n");
            } else if (ret == 0) {
                append_aof(AOF_HASH_FILE, "HSET", key, value);  // 追加AOF
                len = sprintf(response, "ok\r\n");
            } else {
                len = sprintf(response, "exist\r\n");
            } 
            
            break;
        case KVS_CMD_HGET: {
            char *result = kvs_hash_get(&global_hash, key);
            if (result == NULL) {
                len = sprintf(response, "no exist\r\n");
            } else {
                len = sprintf(response, "%s\r\n", result);
            }
            break;
        }
        case KVS_CMD_HDEL:
            ret = kvs_hash_del(&global_hash ,key);
            if (ret < 0) {
                len = sprintf(response, "error\r\n");
            } else if (ret == 0) {
                append_aof(AOF_HASH_FILE, "HDEL", key, NULL);  // 追加AOF
                len = sprintf(response, "ok\r\n");
            } else {
                len = sprintf(response, "no exist\r\n");
            }
            break;
        case KVS_CMD_HMOD:
            ret = kvs_hash_mod(&global_hash ,key, value);
            if (ret < 0) {
                len = sprintf(response, "error\r\n");
            } else if (ret == 0) {
                append_aof(AOF_HASH_FILE, "HMOD", key, value);  // 追加AOF
                len = sprintf(response, "ok\r\n");
            } else {
                len = sprintf(response, "no exist\r\n");
            }
            break;
        case KVS_CMD_HEXIST:
            ret = kvs_hash_exist(&global_hash ,key);
            if (ret == 0) {
                len = sprintf(response, "exist\r\n");
            } else {
                len = sprintf(response, "no exist\r\n");
            }
            break;
#endif
#if ENABLE_SKIPTABLE
        case KVS_CMD_SSET:
            ret = kvs_skiptable_set(&global_skiptable, key, value);
            if (ret < 0) {
                len = sprintf(response, "set error\r\n");
            } else if (ret == 0) {
                append_aof(AOF_SKIPTABLE_FILE, "SSET", key, value);  // 追加AOF
                len = sprintf(response, "ok\r\n");
            } else {
                len = sprintf(response, "exist\r\n");
            }
            break;

        case KVS_CMD_SGET:{
            char *result = kvs_skiptable_get(&global_skiptable, key);
            if (result == NULL) {
                len = sprintf(response, "no exist\r\n");
            } else {
                len = sprintf(response, "%s\r\n", result);
            }
            break;
        }
        case KVS_CMD_SDEL:
            ret = kvs_skiptable_del(&global_skiptable, key);
            if (ret < 0) {
                len = sprintf(response, "error\r\n");
            } else if (ret == 0) {
                append_aof(AOF_SKIPTABLE_FILE, "SDEL", key, NULL);  // 追加AOF
                len = sprintf(response, "ok\r\n");
            } else {
                len = sprintf(response, "no exist\r\n");
            }
            break;

        case KVS_CMD_SMOD:
            ret = kvs_skiptable_mod(&global_skiptable, key, value);
            if (ret < 0) {
                len = sprintf(response, "error\r\n");
            } else if (ret == 0) {
                append_aof(AOF_SKIPTABLE_FILE, "SMOD", key, value);  // 追加AOF
                len = sprintf(response, "ok\r\n");
            } else {
                len = sprintf(response, "no exist\r\n");
            }
            break;
        case KVS_CMD_SEXIST:
            ret = kvs_skiptable_exist(&global_skiptable, key);
            if (ret == 0) {
                len = sprintf(response, "exist\r\n");
            } else {
                len = sprintf(response, "no exist\r\n");
            }
            break;
#endif

#if ENABLE_PERSIST
        case KVS_CMD_SAVE:
            ret = save_snapshots();
            if (ret == 0) {
                // 清空所有 AOF 文件
                FILE *fp = fopen(AOF_ARRAY_FILE, "w"); // w模式会清空文件内容
                if (fp) fclose(fp);
                fp = fopen(AOF_RBTREE_FILE, "w");
                if (fp) fclose(fp);
                fp = fopen(AOF_HASH_FILE, "w");
                if (fp) fclose(fp);
                fp = fopen(AOF_SKIPTABLE_FILE, "w");
                if (fp) fclose(fp);

                len = sprintf(response, "ok\r\n");
            } else {
                len = sprintf(response, "error\r\n");
            }
            break;
#endif

#if ENABLE_SYNC
        case KVS_CMD_SYNC:
            printf("sync now\n");
            if (g_is_master) {
                // 主节点：则处理同步请求
                ret = handle_sync_request(fd);
                if (ret < 0) {
                    len = sprintf(response, "error\r\n");
                } else if (ret == 0) {
                    len = sprintf(response, "ok\r\n");
                }
            } else {
                // 从节点：发起同步
                if (count < 3) {
                    len = sprintf(response, "error: need master_ip, port\r\n");
                    break;
                }
                char *maser_ip = tokens[1];
                int port = atoi(tokens[2]);
                ret = sync_from_master(maser_ip, port);
                if (ret < 0) {
                    len = sprintf(response, "error\r\n");
                } else if (ret == 0) {
                    len = sprintf(response, "ok\r\n");
                }
            }
            break;
#endif
        default:
            assert(0);
    }

    return len;

}

int kvs_protocol(int fd, char *msg, int length, char *response) {

#if ENABLE_CMD_BATCH

    if (fd < 0 || fd >= MAX_CONN) return -1;
    if (msg == NULL || length <= 0 || response == NULL) return -1;
    
    // ===== 获取或初始化连接上下文 =====
    kvs_conn_ctx_t *ctx = &kvs_conn_ctxs[fd];
    if (!ctx->cmd_buffer) {
        kvs_ctx_init(ctx);
    }
    
    // ===== 追加新数据到缓冲区 =====
    kvs_ctx_append(ctx, msg, length);

    p_cmd_count = 0;
    int ret =  kvs_ctx_process(fd, ctx, response, BUFFER_LENGTH);
    // printf("has pd cmd conts: %d\n", p_cmd_count);

    // ===== 处理缓冲区中的完整命令 =====
    return ret;

#else
/*
msg: request message
length:length of request msg
response: need to send
@return : len of response
协议解析：拆分命令 + 协议处理 = 返回响应
*/
    if (msg == NULL || length <= 0 || response == NULL) return -1;
    // printf("kvstore.c recv %d: %s\n", length, msg);

    char *tokens[KVS_MAX_TOKENS] = {0};

    int count = kvs_split_token(msg, tokens);
    if (count == -1)return -1;

    // memcpy(response, msg, length);
    return kvs_filter_protocol(fd, tokens, count, response);
#endif
}


int init_kvengine(void) {
    // ===== 初始化所有连接上下文 =====
    int i = 0;
    for (i = 0; i < MAX_CONN; i++) {
        memset(&kvs_conn_ctxs[i], 0, sizeof(kvs_conn_ctx_t));
    }

#if ENABLE_MEMPOOL
    // 先初始化内存池
    // printf("Initializing memory pools...\n");
    if (init_mempools() != 0) {
        printf("Failed to init memory pools\n");
        return -1;
    }
    printf("********----  1. Memory-pools init success!  ----********\n");
#endif
#if ENABLE_ARRAY
    memset(&global_array, 0, sizeof(kvs_array_t));
    kvs_array_create(&global_array);
    // printf("Array created\n");
#endif

#if ENABLE_HASH
	memset(&global_hash, 0, sizeof(kvs_hash_t));
	kvs_hash_create(&global_hash);
    // printf("hash created\n");
#endif

#if ENABLE_SKIPTABLE
	memset(&global_skiptable, 0, sizeof(kvs_skiptable_t));
	kvs_skiptable_create(&global_skiptable);
    // printf("skiptable created\n");
#endif

#if ENABLE_RBTREE
    memset(&global_rbtree, 0, sizeof(kvs_rbtree_t));
    kvs_rbtree_create(&global_rbtree);
    // printf("Rbtree created\n");
#endif

printf("------------ 2. kvs_engine created ----\n");


#if ENABLE_PERSIST
    // 先加载快照
    if (load_snapshots() != 0) {
        printf("No snapshot found\n");
    }
    printf("------------ 3. Snapshots loaded ---\n");
    // 再重放AOF
    replay_aof(AOF_ARRAY_FILE);
    replay_aof(AOF_RBTREE_FILE);
    replay_aof(AOF_HASH_FILE);
    replay_aof(AOF_SKIPTABLE_FILE);
    printf("------------ 4. AOF done ---\n");
#endif

    return 0;
}

void dest_kvengine(void) {
#if ENABLE_ARRAY
    kvs_array_destory(&global_array);
#endif
#if ENABLE_RBTREE
    kvs_rbtree_destory(&global_rbtree);
#endif
#if ENABLE_HASH
	kvs_hash_destory(&global_hash);
#endif
#if ENABLE_SKIPTABLE
	kvs_skiptable_destory(&global_skiptable);
#endif
#if ENABLE_MEMPOOL
    // 最后销毁内存池
    dest_mempools();
#endif
}




// 命令行线程函数
void* cli_thread(void* arg) {
    char input[256];
    int is_master = *(int*)arg;
    
    printf("\n=== KVS CLI ===\n");
    if (is_master) {
        printf("Mode: MASTER\n");
    } else {
        printf("Mode: SLAVE\n");
        printf("  sync <ip> <port>  - Sync from master\n");
    }
    printf("  help              - Show this help\n");
    printf("  exit              - Exit\n");
    printf("================\n\n");
    
    while (1) {
        printf("kvs> ");
        fflush(stdout);
        
        if (!fgets(input, sizeof(input), stdin)) {
            break;
        }
        
        // 去除换行
        input[strcspn(input, "\n")] = '\0';
        
        // 跳过空行
        if (strlen(input) == 0) continue;
        
        // 分割命令
        char *cmd = strtok(input, " ");
        
        if (strcmp(cmd, "sync") == 0) {
            if (!is_master) {
                char *ip = strtok(NULL, " ");
                char *port_str = strtok(NULL, " ");
                if (ip && port_str) {
                    int master_port = atoi(port_str);
                    printf("\n>>> Syncing from %s:%d...\n", ip, master_port);
                    fflush(stdout);
                    
                    // 执行同步
                    int ret = sync_from_master(ip, master_port);
                    if (ret == 0) {
                        printf(">>> Sync completed successfully!\n");
                    } else {
                        printf(">>> Sync failed!\n");
                    }
                    fflush(stdout);
                } else {
                    printf("Usage: sync <master_ip> <port>\n");
                }
            } else {
                printf("Master node doesn't need to sync\n");
            }
        }
        else if (strcmp(cmd, "help") == 0) {
            printf("Commands:\n");
            if (!is_master) {
                printf("  sync <ip> <port>  - Sync data from master\n");
            }
            printf("  help              - Show this help\n");
            printf("  exit              - Exit program\n");
        }
        else if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
            printf("Exiting...\n");
            exit(0);
        }
        else {
            printf("Unknown command: %s. Type 'help' for available commands.\n", cmd);
        }
    }
    return NULL;
}


int main(int argc, char *argv[]) {

    // 禁用缓冲
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

 //   ./kvstore 2000 --master 
    if (argc < 2) {
        printf("Usage: ./kvstore <port> [--master|--slave]\n");
        return -1;
    }
    int port = atoi(argv[1]);
    g_is_master = 1;  // 默认主节点

    // 判断备份角色
    if (argc >= 3) {
        if (strcmp(argv[2], "--slave") == 0) { // 从节点
            g_is_master = 0;
            printf("Running as SLAVE node\n");
        } else if (strcmp(argv[2], "--master") == 0) { // 主节点
            g_is_master = 1;
            printf("Running as MASTER node\n");
        } else {
            printf("Unknown option: %s\n", argv[2]);
            return -1;
        }
    } else {
        printf("Running as MASTER node (default)\n");
    }

#if ENABLE_PERSIST // 检查data目录是否存在
    if (check_datafile_exist()== -1) return -1;
#endif

    // 用于创建kv存储全局单例内存块global_array
    init_kvengine();

    // 如果是从节点
    if (g_is_master == 0) {
    // ===== 创建 CLI 线程 =====
        pthread_t cli_tid;
        pthread_create(&cli_tid, NULL, cli_thread, &g_is_master);
    // =========================    
    }

// 功能入口函数，第二个参数为函数指针msg_handler，把kvs_protocol传入作为函数
// 间接
#if (NET_SELECT == NET_REACTOR)
    reactor_start(port, kvs_protocol);
#elif (NET_SELECT == NET_NTYCO)
    ntyco_start(port, kvs_protocol);
#elif (NET_SELECT == NET_PROACTOR)
    proactor_start(port, kvs_protocol);
#endif
    dest_kvengine();
    return 0;
}