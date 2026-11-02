
#include "server.h"
#include "kvstore.h"



extern kvs_hash_t global_hash;
extern kvs_skiptable_t global_skiptable;

#if ENABLE_MEMPOOL
extern mempool_t *resp_pool;
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
    "HSET", "HGET", "HDEL", "HMOD", "HEXIST",
    "SSET", "SGET", "SDEL", "SMOD", "SEXIST",
    "SAVE", "SYNC"

};

enum {
    KVS_CMD_START = 0,
	// hash
	KVS_CMD_HSET = KVS_CMD_START,
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


// ===== 从机自动连接线程 =====
static void* slave_sync_thread(void* arg) {
    sleep(1);
    SYNC_LOG_S("auto connecting to master %s:%d",
           g_config.master_ip, g_config.master_port);

    while (1) {
        int ret = sync_from_master(g_config.master_ip, g_config.master_port);
        if (ret == 0) {
            SYNC_LOG_S("sync started");
            break;
        }
        SYNC_ERR("sync failed, retry in 5s");
        sleep(5);
    }
    return NULL;
}

int check_datafile_exist() {
    struct stat st = {0};
    if (stat("data", &st) == -1) {
        LOG_INFO("Error: data directory not exist. Please create it.\n");
        return -1;
    }
    return 0;
}

int kvs_split_token(char *msg, char *tokens[]) {

    if (msg == NULL || tokens == NULL)return -1;
    int idx = 0;
    char *token = strtok(msg, " ");
    // printf("debug start\n");
    while (token != NULL) {
        // printf("idx: %d, %s\n", idx, token);
        tokens[idx ++] = token;
        token = strtok(NULL, " ");
    }
    // printf("debug done\n");
    return idx;

}

int kvs_filter_protocol(int fd, char **tokens, int count, char *response) {
// SET KEY VALUE
// tokens[0]: SET ; tokens[1]: KEY; tokens[0]: VALUE
// 进行协议数据过滤
    if (tokens == NULL || count == 0) return -1;
    // 移除 response == NULL 的检查
    // ===== PING/PONG =====
    //   redis-cli 扁平请求: 回复 +PONG（标准 RESP）
    //   主从同步连接（复制心跳）: 必须保持静默，避免从机把 /+PONG 误当 AOF 内容写入
    if (strcmp(tokens[0], "PING") == 0 || strcmp(tokens[0], "PONG") == 0) {
        if (is_sync_peer(fd)) return 0;
#if ENABLE_REPBUF
        if (fd >= 0 && fd < MAX_CONN) {
            kvs_conn_ctx_t *pctx = &kvs_conn_ctxs[fd];
            const char *pong = "+PONG\r\n";
            resp_buf_append(pctx, pong, (int)strlen(pong));
            pctx->send_pending = 1;
        }
#endif
        return 0;  // 直接返回，不处理
    }
    // 来自 AOF 同步的命令，不需要响应
    int is_aof_sync = (fd < 0);

    kvs_conn_ctx_t *ctx = NULL;
    char *single_resp = NULL;
    int resp_cap = 0;

    if (!is_aof_sync) {
#if ENABLE_REPBUF
        /* ===== REPBUF=1：响应先进缓冲区 ===== */
        ctx = &kvs_conn_ctxs[fd];
#  if ENABLE_MEMPOOL
        /* 池模式：从 resp_pool 拿 */
        single_resp = (char*)kvs_mempool_alloc(resp_pool, RESP_POOL_BLOCK_SIZE);
        resp_cap = RESP_POOL_BLOCK_SIZE;
        if (!single_resp) {
            /* 池耗尽降级 malloc */
            single_resp = (char*)kvs_malloc(RESP_POOL_BLOCK_SIZE);
            resp_cap = RESP_POOL_BLOCK_SIZE;
        }
#  else
        /* 非池模式：自己 malloc */
        resp_cap = BUFFER_LENGTH;
        single_resp = (char*)kvs_malloc(resp_cap);
#  endif
        if (!single_resp) return -1;
#else
        /* ===== REPBUF=0：直接用调用方 response ===== */
        single_resp = response;
        resp_cap = BUFFER_LENGTH;
        if (!single_resp) return -1;
#endif
    }


    int cmd = KVS_CMD_START;
    for (cmd = KVS_CMD_START; cmd < KVS_CMD_COUNT; cmd++) {
        if (strcmp(tokens[0], command[cmd]) == 0 ) {
            break;
        }
    }

    // 解析到错误token后，应该打印错误的token信息，然后跳过这条cmd命令，继续执行后续的。
    if (cmd >= KVS_CMD_COUNT) {
        // printf("[ERROR] unknown command token: '%s'\n", tokens[0]);

        char err[256];
        int err_len = snprintf(err, sizeof(err), "-ERR unknown command '%s'\r\n", tokens[0]);
        if (err_len < 0) err_len = 0;
        if (err_len >= (int)sizeof(err)) err_len = (int)sizeof(err) - 1;
        if (ctx != NULL) {
            resp_buf_append(ctx, err, err_len);
            ctx->send_pending = 1;
        }
#if ENABLE_MEMPOOL
        if (single_resp && !is_aof_sync) {
            kvs_mempool_free(resp_pool, single_resp);
        }
#else
        if (single_resp && single_resp != response && !is_aof_sync) {
            kvs_free(single_resp);
        }
#endif

        return -1;
    }
    int len = 0;
    int ret = 0;
    char *key = NULL;
    char *value = NULL;

    // ===== 关键：根据 count 判断是否有 key 和 value =====
    if (count > 1) key = tokens[1];
    if (count > 2) value = tokens[2];

#if ENABLE_MEMPOOL
    #define FREE_RESP(p)   kvs_mempool_free(resp_pool, (p))
#else
    #define FREE_RESP(p)   kvs_free(p)
#endif

     // 安全的追加宏 - 修复了 new_size 的问题
    #define SAFE_APPEND(fmt, ...) do { \
        if (!is_aof_sync) { \
            int need = snprintf(NULL, 0, fmt, ##__VA_ARGS__) + 1; \
            if (len + need >= resp_cap) { \
                int new_cap = resp_cap > 0 ? resp_cap : BUFFER_LENGTH; \
                while (new_cap < len + need + 1024) new_cap *= 2; \
                char *new_buf = (char*)kvs_malloc(new_cap); \
                if (!new_buf) break; \
                if (single_resp && len > 0) memcpy(new_buf, single_resp, len); \
                /* 释放旧缓冲 */ \
                if (single_resp && single_resp != response) FREE_RESP(single_resp); \
                single_resp = new_buf; \
                resp_cap = new_cap; \
            } \
            len += snprintf(single_resp + len, resp_cap - len, fmt, ##__VA_ARGS__); \
        } \
    } while (0)


    switch (cmd) {
#if ENABLE_HASH
        case KVS_CMD_HSET:
            ret = kvs_hash_set(&global_hash ,key, value);
            if (ret < 0) {
                SAFE_APPEND("-ERR invalid arguments\r\n");
            } else if (ret == 0) {
                if (!is_aof_sync) append_aof("HSET", key, value);  // 追加AOF
                // ===== immediate 同步 =====
                    master_aof_immediate("HSET", key, value);
                SAFE_APPEND(":1\r\n");      // 新插入 → 1
            } else {
                SAFE_APPEND(":0\r\n");      // 已存在（不覆盖）→ 0
            } 
            
            break;
        case KVS_CMD_HGET: {
            char *result = kvs_hash_get(&global_hash, key);
            if (result == NULL) {
                SAFE_APPEND("$-1\r\n");     // 空值
            } else {
                SAFE_APPEND("$%zu\r\n%s\r\n", strlen(result), result);
            }
            break;
        }
        case KVS_CMD_HDEL:
            ret = kvs_hash_del(&global_hash ,key);
            if (ret < 0) {
                SAFE_APPEND("-ERR invalid arguments\r\n");
            } else if (ret == 0) {
                if (!is_aof_sync) append_aof("HDEL", key, NULL);  // 追加AOF
                // ===== immediate 同步 =====
                    master_aof_immediate("HDEL", key, NULL);
                SAFE_APPEND(":1\r\n");      // 删除成功 → 1
            } else {
                SAFE_APPEND(":0\r\n");      // 不存在 → 0
            }
            break;
        case KVS_CMD_HMOD:
            ret = kvs_hash_mod(&global_hash ,key, value);
            if (ret < 0) {
                SAFE_APPEND("-ERR invalid arguments\r\n");
            } else if (ret == 0) {
                if (!is_aof_sync) append_aof("HMOD", key, value);  // 追加AOF
                // ===== immediate 同步 =====
                    master_aof_immediate("HMOD", key, value);
                SAFE_APPEND("+OK\r\n");
            } else {
                SAFE_APPEND("-ERR no such key\r\n");
            }
            break;
        case KVS_CMD_HEXIST:
            ret = kvs_hash_exist(&global_hash ,key);
            if (ret == 0) {
                SAFE_APPEND(":1\r\n");      // 存在 → 1
            } else {
                SAFE_APPEND(":0\r\n");      // 不存在 → 0
            }
            break;
#endif
#if ENABLE_SKIPTABLE
        case KVS_CMD_SSET:
            ret = kvs_skiptable_set(&global_skiptable, key, value);
            if (ret < 0) {
                SAFE_APPEND("-ERR invalid arguments\r\n");
            } else if (ret == 0) {
                if (!is_aof_sync) append_aof("SSET", key, value);  // 追加AOF
                // ===== immediate 同步 =====
                    master_aof_immediate("SSET", key, value);
                SAFE_APPEND(":1\r\n");      // 新插入 → 1
            } else {
                SAFE_APPEND(":0\r\n");      // 已存在（不覆盖）→ 0
            }
            break;

        case KVS_CMD_SGET:{
            char *result = kvs_skiptable_get(&global_skiptable, key);
            if (result == NULL) {
                SAFE_APPEND("$-1\r\n");     // 空值
            } else {
                SAFE_APPEND("$%zu\r\n%s\r\n", strlen(result), result);
            }
            break;
        }
        case KVS_CMD_SDEL:
            ret = kvs_skiptable_del(&global_skiptable, key);
            if (ret < 0) {
                SAFE_APPEND("-ERR invalid arguments\r\n");
            } else if (ret == 0) {
                if (!is_aof_sync) append_aof("SDEL", key, NULL);  // 追加AOF
                // ===== immediate 同步 =====
                    master_aof_immediate("SDEL", key, NULL);
                
                SAFE_APPEND(":1\r\n");      // 删除成功 → 1
            } else {
                SAFE_APPEND(":0\r\n");      // 不存在 → 0
            }
            break;

        case KVS_CMD_SMOD:
            ret = kvs_skiptable_mod(&global_skiptable, key, value);
            if (ret < 0) {
                SAFE_APPEND("-ERR invalid arguments\r\n");
            } else if (ret == 0) {
                if (!is_aof_sync) append_aof("SMOD", key, value);  // 追加AOF
                // ===== immediate 同步 =====
                
                    master_aof_immediate("SMOD", key, value);
                
                SAFE_APPEND("+OK\r\n");
            } else {
                SAFE_APPEND("-ERR no such key\r\n");
            }
            break;
        case KVS_CMD_SEXIST:
            ret = kvs_skiptable_exist(&global_skiptable, key);
            if (ret == 0) {
                SAFE_APPEND(":1\r\n");      // 存在 → 1
            } else {
                SAFE_APPEND(":0\r\n");      // 不存在 → 0
            }
            break;
#endif

#if ENABLE_PERSIST
#if ENABLE_URING
        case KVS_CMD_SAVE:
            // SAVE 只在 SNAP 模式有效
            if (!g_config.snap_enable) {
                SAFE_APPEND("-ERR SAVE only in SNAP mode\r\n");
                break;
            }
            ret = save_snapshots();
            // 如果是从机的 SAVE 请求，发送 SNAP
            if (g_config.snap_enable && is_slave_save(fd)) {
#if ENABLE_RDMA
                if (g_config.rdma_enable) {
                    rdma_send_file(SNAP_FILE, 0, "SNAP_END\n");
                } else {
                    send_file_content(fd, SNAP_FILE, 0, "SNAP_END\n");
                }
#else
                send_file_content(fd, SNAP_FILE, 0, "SNAP_END\n");
#endif
                // SAFE_APPEND("+OK\r\n");
                break;
            }
            if (ret == 0) {
                persistence_reset_aof();
            }
            SAFE_APPEND(ret == 0 ? "+OK\r\n" : "-ERR save failed\r\n");
            break;
#else
        case KVS_CMD_SAVE:
            ret = save_snapshots();
            if (ret == 0) {
                // 清空所有 AOF 文件
                FILE *fp = fopen(AOF_FILE, "w"); // w模式会清空文件内容
                if (fp) fclose(fp);

                SAFE_APPEND("+OK\r\n");
            } else {
                SAFE_APPEND("-ERR save failed\r\n");
            }
            break;
#endif
#endif

#if ENABLE_SYNC
        case KVS_CMD_SYNC:
            SYNC_LOG("SYNC cmd received on fd=%d, role=%s", fd, g_is_master ? "master" : "slave");
            if (g_is_master) {
                // 主节点：则处理同步请求
                ret = handle_sync_request(fd);
                break;
            // 并且会发送 AOF_SYNC_OK 确认
            }
            break;
#endif
        default:
            LOG_INFO("[ERROR] unhandled cmd=%d, token='%s'\n", cmd, tokens[0]);
            // 发送错误响应给客户端
            if (ctx != NULL) {
                const char *err = "-ERR unhandled command\r\n";
                resp_buf_append(ctx, err, strlen(err));
                ctx->send_pending = 1;
            }
            #if ENABLE_MEMPOOL
                if (single_resp && !is_aof_sync) {
                    kvs_mempool_free(resp_pool, single_resp);
                }
            #else
                if (single_resp && single_resp != response && !is_aof_sync) {
                    kvs_free(single_resp);
                }
            #endif

            return -1;
    }



#if ENABLE_REPBUF
    if (len > 0 && ctx != NULL) {
        resp_buf_append(ctx, single_resp, len);
        ctx->send_pending = 1;
    }
#endif

#if ENABLE_MEMPOOL
    if (single_resp && single_resp != response && !is_aof_sync) {
        kvs_mempool_free(resp_pool, single_resp);
    }
#else
    if (single_resp && single_resp != response && !is_aof_sync) {
        FREE_RESP(single_resp);
    }
#endif

    return len;
#undef SAFE_APPEND
#undef FREE_RESP
}

int kvs_protocol(int fd, char *msg, int length, char *response) {


#if ENABLE_CMD_BATCH

    if (fd < 0 || fd >= MAX_CONN) return -1;
    if (msg == NULL || length <= 0 ) return -1;
    
    // ===== 获取或初始化连接上下文 =====
    kvs_conn_ctx_t *ctx = &kvs_conn_ctxs[fd];
    if (!ctx->cmd_buffer) {
        kvs_ctx_init(ctx);
    }
    
    // 1. 追加数据到 cmd_buffer
    kvs_ctx_append(ctx, msg, length);
    // printf("debug 0。0:\n");
#if ENABLE_REPBUF
    // 2. RESP 解析 + 执行命令
    kvs_ctx_process(fd, ctx, NULL, 0);
    // printf("debug 0。1:\n");

    return 0;
#else

    int ret =  kvs_ctx_process(fd, ctx, response, BUFFER_LENGTH);

    return ret;
#endif

#else
/*
msg: request message
length:length of request msg
response: need to send
@return : len of response
协议解析：拆分命令 + 协议处理 = 返回响应   单条命令模式
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
        LOG_INFO("Failed to init memory pools\n");
        return -1;
    }
    LOG_INFO("********----  1. Memory-pools init success!  ----********\n");
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

LOG_INFO("------------ 2. kvs_engine created ----\n");


#if ENABLE_PERSIST
    if (g_config.role == ROLE_SLAVE) {
        // 从机：不加载本地 SNAP/AOF（数据从主机来）
        LOG_INFO("=== [INIT] SLAVE mode: skip local snap/AOF ===\n");
    } else {
        // 主机：正常加载
        if (g_config.snap_enable) {
            if (load_snapshots() != 0) {
                LOG_INFO("No snapshot found\n");
            }
            LOG_INFO("------------ 3. Snapshots loaded ---\n");
        } else {
            LOG_INFO("------------ 3. Snapshots disabled ---\n");
        }

        if (g_config.aof_enable) {
            long consumed = replay_aof_from(AOF_FILE, 0);
            if (consumed < 0) {
                LOG_INFO("AOF replay failed\n");
            }
            LOG_INFO("------------ 4. AOF done ---\n");
#if ENABLE_URING
            if (persistence_init() != 0) {
                LOG_INFO("------------ 5. AOF io_uring init FAILED ---\n");
                return -1;
            }
            LOG_INFO("------------ 5. AOF io_uring writer started ---\n");
#endif
        } else {
            LOG_INFO("------------ 4. AOF disabled ---\n");
        }
    }
#endif

    return 0;
}

void dest_kvengine(void) {
#if ENABLE_PERSIST
    /* Drain/stop AOF before destroying the KV engine. */
    persistence_shutdown();
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
    
    LOG_INFO("\n=== KVS CLI ===\n");
    if (is_master) {
        LOG_INFO("Mode: MASTER\n");
    } else {
        LOG_INFO("Mode: SLAVE\n");
        LOG_INFO("  sync <ip> <port>  - Sync from master\n");
    }
    LOG_INFO("  help              - Show this help\n");
    LOG_INFO("  exit              - Exit\n");
    LOG_INFO("================\n\n");
    
    while (1) {
        LOG_INFO("kvs> ");
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
                    LOG_INFO("\n>>> Syncing from %s:%d...\n", ip, master_port);
                    fflush(stdout);
                    
                    // 执行同步
                    int ret = sync_from_master(ip, master_port);
                    if (ret == 0) {
                        LOG_INFO(">>> Sync completed successfully!\n");
                    } else {
                        LOG_INFO(">>> Sync failed!\n");
                    }
                    fflush(stdout);
                } else {
                    LOG_INFO("Usage: sync <master_ip> <port>\n");
                }
            } else {
                LOG_INFO("Master node doesn't need to sync\n");
            }
        }
        else if (strcmp(cmd, "help") == 0) {
            LOG_INFO("Commands:\n");
            if (!is_master) {
                LOG_INFO("  sync <ip> <port>  - Sync data from master\n");
            }
            LOG_INFO("  help              - Show this help\n");
            LOG_INFO("  exit              - Exit program\n");
        }
        else if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
            LOG_INFO("Exiting...\n");
            exit(0);
        }
        else {
            LOG_INFO("Unknown command: %s. Type 'help' for available commands.\n", cmd);
        }
    }
    return NULL;
}


int main(int argc, char *argv[]) {

    // 禁用缓冲
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    kvs_log_init("logs/kvs.log", LOG_LEVEL_INFO);
/*=====================加载配置========================*/
    const char *config_file = NULL;
    int i = 0;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_file = argv[++i];
        }
    }
    if (!config_file) {
        LOG_INFO("Usage: ./kvstore --config <config_file>\n");
        return -1;
    }
    // 加载配置
    if (kvs_config_load(config_file) < 0) {
        return -1;
    }
    // 配置文件中的日志级别覆盖默认值
    kvs_log_set_level(g_config.log_level);
    // 打印配置
    kvs_config_print();
    // 设置全局角色
    g_is_master = (g_config.role == ROLE_MASTER);

    int port = (g_config.role == ROLE_MASTER) ? g_config.master_port : g_config.slave_port;

#if ENABLE_PERSIST // 检查data目录是否存在
    if (check_datafile_exist()== -1) return -1;
#endif

    // 用于创建kv存储全局单例内存块
    init_kvengine();

    if (g_config.role == ROLE_SLAVE) {
        // 从机：启动自动同步线程
        pthread_t sync_tid;
        pthread_create(&sync_tid, NULL, slave_sync_thread, NULL);
        pthread_detach(sync_tid);

        // 可选：保留 CLI
        // pthread_t cli_tid;
        // pthread_create(&cli_tid, NULL, cli_thread, &g_is_master);
        // pthread_detach(cli_tid);
    }

#if ENABLE_EBPF
    /* ★ 主节点：把 eBPF 加载器搬进本进程，uprobe 挂在自身的 append_aof 上 */
    if (g_is_master && g_config.ebpf_enable) {
        char self[512] = {0};
        ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (n <= 0) {
            strncpy(self, argv[0], sizeof(self) - 1);
        }
        if (ebpf_capture_start(self, (int)getpid()) != 0) {
            LOG_ERROR("[EBPF] 启动失败，降级为同步直发模式（功能不受影响）");
        }
    }
#endif

#if (NET_SELECT == NET_REACTOR)
    reactor_start(port, kvs_protocol);
#elif (NET_SELECT == NET_NTYCO)
    ntyco_start(port, kvs_protocol);
#endif
#if ENABLE_EBPF
    ebpf_capture_stop();
#endif
#if ENABLE_RDMA
    if (g_config.rdma_enable) {
        rdma_shutdown();       /* 必须在网络主循环退出之后、进程结束之前 */
    }
#endif

    dest_kvengine();
    
    LOG_INFO("kvstore stopped");   /* ★ 可选：退出前打一条 */
    kvs_log_fini();                /* ★ 关闭日志，刷盘 */
    return 0;
}