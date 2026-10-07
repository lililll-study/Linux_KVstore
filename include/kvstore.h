#ifndef __KV_STORE_H__
#define __KV_STORE_H__

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <stdint.h>

#include <sys/stat.h>
#include <sys/types.h>

#include <time.h>      
#include <pthread.h>   
#include <unistd.h>    

#define ENABLE_PRINTF       1       // 终端打印开关

#if ENABLE_PRINTF
    #define KVS_PRINTF(...)   printf(__VA_ARGS__)
#else
    #define KVS_PRINTF(...)   ((void)0)
#endif

#define ENABLE_MMAP         1   // 持久化数据加载
#define ENABLE_URING        1   // 持久化数据落盘
#define NET_REACTOR     0
#define NET_NTYCO       1
#define NET_PROACTOR    2

#define NET_SELECT NET_NTYCO


#define ENABLE_ARRAY         1
#define ENABLE_RBTREE        1
#define ENABLE_HASH			 1
#define ENABLE_SKIPTABLE	 1

#define ENABLE_PERSIST 		 1	 // 开启本地持久化存储
#define ENABLE_MEMPOOL		 0   // 开启内存池

#define ENABLE_CMD_BATCH	 1   // 开启cmd批处理

#define ENABLE_SYNC			 1   // 开启同步备份功能
#define ENABLE_AOF_SYNC      1   // 开启增量同步备份
#define ENABLE_REPBUF        1   // 启动响应缓冲区，避免响应过大溢出

#define ENABLE_SYNC_MODIFY 1    // 修改主从同步逻辑

#define ENABLE_CRC          1   // 打开crc校验功能



/* ===== 主机工作标记 ===== */
#define MASTER_WORKING(fd, cmd, key) do {                       \
    if (g_is_master && (fd) >= 0) {                             \
        LOG_DEBUG("[MASTER] working now~ fd=%d cmd=%s key=%s",  \
                  (fd), (cmd), (key));                          \
    }                                                           \
} while (0)

/*
======================================= 底层网络架构 ======================================
==========================================================================================
*/

extern  const char *command[];

#define KVS_MAX_TOKENS 128

typedef int (*msg_handler)(int fd, char *msg, int length, char *response);

extern int reactor_start(unsigned short port, msg_handler handler);
extern int ntyco_start(unsigned short port, msg_handler handler);
extern int proactor_start(unsigned short port, msg_handler handler);

/*
======================================= 1.kv引擎 ======================================
==========================================================================================
*/
#if ENABLE_ARRAY
typedef struct kvs_array_item_s {
    char *key;
    char *value;
} kvs_array_item_t;


#define KVS_ARRAY_INIT_SIZE 1024      // 初始容量
#define KVS_ARRAY_GROW_FACTOR 2       // 扩容因子

typedef struct kvs_array_s {
    kvs_array_item_t *table;
    int total;
    int capacity;    // 当前容量（新增）
} kvs_array_t;

void kvs_array_destory(kvs_array_t *inst);
int kvs_array_create(kvs_array_t *inst);
int kvs_array_set(kvs_array_t *inst, char *key, char *value);
char *kvs_array_get(kvs_array_t *inst, char *key);
int kvs_array_del(kvs_array_t *inst, char *key);
int kvs_array_mod(kvs_array_t *inst, char *key, char *value);
int kvs_array_exist(kvs_array_t *inst, char *key);
#endif

#if ENABLE_RBTREE

#define RED				1
#define BLACK 			2
#define ENABLE_KEY_CHAR 1

#if ENABLE_KEY_CHAR
typedef char* KEY_TYPE;
#else
typedef int KEY_TYPE;
#endif


typedef struct _rbtree_node {
	unsigned char color;
	struct _rbtree_node *right;
	struct _rbtree_node *left;
	struct _rbtree_node *parent;
	KEY_TYPE key;
	void *value;
} rbtree_node;

typedef struct _rbtree {
	rbtree_node *root;
	rbtree_node *nil;
} rbtree;


typedef struct _rbtree kvs_rbtree_t;
int kvs_rbtree_create(kvs_rbtree_t *inst);
void kvs_rbtree_destory(kvs_rbtree_t *inst) ;
int kvs_rbtree_set(kvs_rbtree_t *inst, char *key, char *value) ;
char *kvs_rbtree_get(kvs_rbtree_t *inst, char *key);
int kvs_rbtree_del(kvs_rbtree_t *inst, char *key) ;
int kvs_rbtree_mod(kvs_rbtree_t *inst, char *key, char *value) ;
int kvs_rbtree_exist(kvs_rbtree_t *inst, char *key) ;
#endif

#if ENABLE_HASH

#define MAX_KEY_LEN	128
#define MAX_VALUE_LEN	512
#define MAX_TABLE_SIZE	1024

#define ENABLE_KEY_POINTER	1


typedef struct hashnode_s {
#if ENABLE_KEY_POINTER
	char *key;
	char *value;
#else
	char key[MAX_KEY_LEN];
	char value[MAX_VALUE_LEN];
#endif
	struct hashnode_s *next;
	
} hashnode_t;


typedef struct hashtable_s {

	hashnode_t **nodes; //* change **, 

	int max_slots;
	int count;

} hashtable_t;

typedef struct hashtable_s kvs_hash_t;


int kvs_hash_create(kvs_hash_t *hash);
void kvs_hash_destory(kvs_hash_t *hash);
int kvs_hash_set(hashtable_t *hash, char *key, char *value);
char * kvs_hash_get(kvs_hash_t *hash, char *key);
int kvs_hash_mod(kvs_hash_t *hash, char *key, char *value);
int kvs_hash_del(kvs_hash_t *hash, char *key);
int kvs_hash_exist(kvs_hash_t *hash, char *key);


#endif

#if ENABLE_SKIPTABLE
#define MAX_LEVEL 6

typedef struct kvs_node {
    char *key;
    char *value;
    struct kvs_node** forward;  // 指向一个指针数组，forward[i]用来表示该节点在第i层的下一个节点
    // 作为一个二级指针，指向一个node*数组
} kvs_node_t;

typedef struct kvs_skiptable {
    int level; // 跳表层数
    kvs_node_t* header; // 指向头节点
} kvs_skiptable_t;

typedef struct kvs_skiptable kvs_skiptable_t;
int kvs_skiptable_create(kvs_skiptable_t *inst);
void kvs_skiptable_destory(kvs_skiptable_t *inst);
int kvs_skiptable_set(kvs_skiptable_t *inst, char *key, char *value);
char *kvs_skiptable_get(kvs_skiptable_t *inst, char *key);
int kvs_skiptable_del(kvs_skiptable_t *inst, char *key);
int kvs_skiptable_mod(kvs_skiptable_t *inst, char *key, char *value);
int kvs_skiptable_exist(kvs_skiptable_t *inst, char *key);
#endif

int kvs_split_token(char *msg, char *tokens[]);
int kvs_filter_protocol(int fd, char **tokens, int count, char *response);
int kvs_protocol(int fd, char *msg, int length, char *response);


/*
======================================= 2.持久化persist ======================================
==========================================================================================
*/
#if ENABLE_PERSIST

#if ENABLE_URING
#define SNAP_FILE "data/data_snap.snap"
#define AOF_FILE   "data/data_aof.aof"
int save_snapshots();
int load_snapshots();
// AOF
int append_aof(const char *cmd, const char *key, const char *value);
int replay_aof(const char *filename);
long replay_aof_from(const char *filename, long start_offset);
/* io_uring-backed AOF lifecycle */
int persistence_init(void);
int persistence_flush(void);
int persistence_reset_aof(void);
void persistence_shutdown(void);
#else
// 快照文件路径（放在 data 目录下）
#define SNAP_ARRAY_FILE     "data/array.snap"
#define SNAP_RBTREE_FILE    "data/rbtree.snap"
#define SNAP_HASH_FILE      "data/hash.snap"
#define SNAP_SKIPTABLE_FILE "data/skiptable.snap"

// AOF文件路径（放在 data 目录下）
#define AOF_ARRAY_FILE      "data/array.aof"
#define AOF_RBTREE_FILE     "data/rbtree.aof"
#define AOF_HASH_FILE       "data/hash.aof"
#define AOF_SKIPTABLE_FILE  "data/skiptable.aof"

int save_snapshots();
int load_snapshots();
// 快照保存
int save_snapshot_array(const char *filename);
int save_snapshot_rbtree(const char *filename);
int save_snapshot_hash(const char *filename);
int save_snapshot_skiptable(const char *filename);
// 快照加载
int load_snapshot_array(const char *filename);
int load_snapshot_rbtree(const char *filename);
int load_snapshot_hash(const char *filename);
int load_snapshot_skiptable(const char *filename);

// AOF
int append_aof(const char *filename, const char *cmd, const char *key, const char *value);
int replay_aof(const char *filename);
#endif
#endif


/*
======================================= 3.内存池 ======================================
==========================================================================================
*/
#if ENABLE_MEMPOOL

#define KVS_POOL_NODE_COUNT     500000      // 每个引擎的节点池：容量上限50万块
#define RESP_POOL_BLOCK_SIZE    4096
#define RESP_POOL_BLOCK_COUNT   1024        // 响应池：1024块 × 4KB

// 字符串池 size-class 配置（块大小从小到大，容量递减）
// 字符串池按大小分 4 类：
#define KVS_STRING_CLASSES  4
#define KVS_STRING_CLASS_SIZES {32, 64, 128, 256}
#define KVS_STRING_CLASS_COUNTS {1000000, 200000, 50000, 10000}

// 块头：仅 8 字节，用户数据紧随其后；归属由 free 时的段范围+对齐校验推出，不占块内空间
typedef struct mempool_block_hdr {
    void *next;                 // 空闲时：下一个空闲块；分配后 bit0=1 作 in-use 标记
} mempool_block_hdr_t;

#define KVS_MEMPOOL_INUSE   ((uintptr_t)1)  // next 低位 in-use 标记（块地址 8 字节对齐，低位恒为 0）
#define KVS_MEMPOOL_HDR     sizeof(mempool_block_hdr_t) // 8 字节


// 池按照chunk增长，每段是一块连续内存，段之间用链表连接；首次分配小批量，不够再翻倍。
typedef struct mempool_chunk_s {
    struct mempool_chunk_s *next;
    size_t blocks;              // 本段块数，块数据紧随其后
} mempool_chunk_t;

// 内存池结构（sub_pools 非空时为 size-class 组池外壳，子池数组以 NULL 结尾）
typedef struct mempool_s{
    size_t block_size;      // 每块总大小（含块头，8字节对齐）
    size_t max_blocks;      // 容量上限
    void* free_list;        // 空闲链表头指针
    mempool_chunk_t *chunks;    // 已分配段链表（最新段在前，段头记录各段块数）
    struct mempool_s **sub_pools;      // 组池子池数组，NULL 结尾；普通池为 NULL
} mempool_t;

extern mempool_t *resp_pool;

// 不同数据类型分类设计
extern mempool_t *array_item_pool;
extern mempool_t *array_string_pool;
extern mempool_t *rbtree_node_pool;
extern mempool_t *rbtree_string_pool;
extern mempool_t *hash_node_pool;
extern mempool_t *hash_string_pool;
extern mempool_t *skiptable_node_pool;
extern mempool_t *skiptable_string_pool;


// 指定块大小和块数量（num_blocks 为容量上限，首批小批量、按需翻倍增长）
mempool_t* kvs_mempool_create(size_t user_block_size, size_t max_blocks);
// 创建 size-class 组池：alloc 按 size 路由到最小可容纳的子池
mempool_t* kvs_mempool_create_group(const size_t *sizes, const size_t *counts, int nclasses);
void* kvs_mempool_alloc(mempool_t *pool, size_t size);
void kvs_mempool_free(mempool_t *pool, void *ptr);
void kvs_mempool_reset(mempool_t *pool);
void kvs_mempool_destroy(mempool_t *pool);
int init_mempools(void);
void dest_mempools(void);
#endif


/*
（设计请求缓冲区和响应缓冲区）
======================================= 4.批处理 ======================================
==========================================================================================
*/
#if ENABLE_CMD_BATCH

#define MAX_CONN 1024




typedef struct kvs_conn_ctx {
	char *cmd_buffer;      // 命令积累缓冲区
    int cmd_buffer_len;    // 当前有效数据长度
    int cmd_buffer_cap;    // 缓冲区容量
    
#if ENABLE_REPBUF
    // ===== 响应缓冲区 =====
    char *resp_buffer;
    int resp_len;
    int resp_cap;
    int resp_sent;          // 已发送位置
    
    // ===== 发送标志 =====
    int send_pending;       // 1: 有待发送数据, 0: 无

#endif

    int resp_state;     // RESP解析状态
    int resp_cmd_num;   // RESP头表示cmd的数量
    int resp_cmd_idx;   // 当前准备处理的cmd索引

    int resp_scmd_unit_num; // 单条命令的元素数量
    int resp_scmd_unit_idx; // 单条命令的元素索引

    int resp_token_len; //token的字节数
    char *resp_temp_buffer; // 用于存储解析好的cmd token的临时缓冲区
    int resp_temp_buffer_cap;   // 临时token缓冲区的容量

    int resp_cmd_start;     // 当前命令起始偏移

} kvs_conn_ctx_t;

extern kvs_conn_ctx_t kvs_conn_ctxs[MAX_CONN];
int kvs_ctx_init(kvs_conn_ctx_t *ctx);
void kvs_ctx_free(kvs_conn_ctx_t *ctx);
int kvs_ctx_append(kvs_conn_ctx_t *ctx, const char *data, int len);
void kvs_ctx_consume(kvs_conn_ctx_t *ctx, int consumed);
int kvs_ctx_process(int fd, kvs_conn_ctx_t *ctx, char *response, int resp_cap);
// 清理连接上下文（在连接关闭时调用）
void cleanup_connection(int fd);

#if ENABLE_REPBUF
int resp_buf_append(kvs_conn_ctx_t *ctx, const char *data, int len);
char* resp_buf_get_data(kvs_conn_ctx_t *ctx, int *out_len);
void resp_buf_advance(kvs_conn_ctx_t *ctx, int sent_len);
int resp_buf_has_data(kvs_conn_ctx_t *ctx);
#endif


#endif






/*
======================================= 5.  全量和增量同步 ======================================
==========================================================================================
*/
#if ENABLE_SYNC
#define SYNC_BUFFER_SIZE 2000

// ===== 心跳（RESP 格式） =====
#define HEARTBEAT_CMD     "*1\r\n*1\r\n$4\r\nPING\r\n"   // 14 字节
#define HEARTBEAT_CMD_LEN 18
#define HEARTBEAT_INTERVAL 5                        // 秒

extern int g_is_master;

// ======== 主从同步接口 ===========
int handle_sync_request(int fd);                        // 主机：处理 SYNC


int safe_send(int fd, const char* data, int len);
// 主节点传输和从节点接收
int send_file_content(int fd, const char *filepath, long max_bytes, const char *end_marker);
int recv_file_content(int fd, const char *filepath, const char *end_marker);


// 主从同步改造

int is_slave_save(int fd);

void init_aof_sync_ctx(void);

// 主节点同步模式及线程函数
void* master_aof_everysec_thread(void* arg);   //  主机：AOF everysec 模式
int master_aof_immediate(const char *cmd, const char *key, const char *value);    // immediate 同步（无线程）


// 从节点处理同步数据线程，以及接口函数
void* slave_aof_recv_thread(void* arg); 
void* slave_snap_recv_thread(void* arg); 
void slave_handle_aof_mode(int fd); 
void slave_handle_snap_mode(int fd);
int sync_from_master(const char *master_ip, int port);	// 从机：发起 SYNC

#endif

/*
======================================= 6.  CRC校验 ======================================
==========================================================================================
*/
#if ENABLE_CRC

#define CRC_LIST_SNAP_FILE  "data/crc_snap.dat"
#define CRC_LIST_AOF_FILE   "data/crc_aof.dat"

// CRC单元：单条CRC记录
typedef struct {
    uint32_t crc;   // 本条记录的内容指纹：CRC32(cmd+key+value) 或 CRC32(key+value)
    uint64_t seq;   // 全局递增序列号，用于对齐
} crc_uint_t;

uint32_t crc32_calc(const char *data, size_t len);
uint32_t crc32_calc_snap(const char *key, const char *value);
uint32_t crc32_calc_aof(const char *cmd, const char *key, const char *value);

// 整个CRC列表容器
typedef struct {
    crc_uint_t *units;  // 指向堆上的动态数组，存储所有crc单元
    int     count;  // 当前有多少个单元
    int     cap;    // 数组容量
    uint64_t next_seq;  // 下个单元应该分配哪个 seq
    pthread_mutex_t  lock;  // 多线程保护
} crc_list_t;

/* 主机/从机全局列表（在 crc.c 中定义） */
extern crc_list_t g_crc_list_snap;
extern crc_list_t g_crc_list_aof;

// 生命周期
int crc_list_create(crc_list_t *list);
void crc_list_destroy(crc_list_t *list);
void crc_list_reset(crc_list_t *list);

// 追加记录
int crc_list_append(crc_list_t *list, const char *type, const char *key, const char *value);

// 持久化
int crc_list_save(crc_list_t *list, const char *filename);
int crc_list_load(crc_list_t *list, const char *filename);
/* 便利封装：直接落盘到默认文件 */
int crc_save_snap(void);
int crc_save_aof(void);


int crc_rebuild_snap_list(void);
long replay_aof_from_with_crc(const char *filename, long start_offset, crc_list_t *crc_out);
#endif


/*
======================================= 7.  配置config ======================================
==========================================================================================
*/

// AOF 同步方式
#define AOF_MODE_EVERYSEC   0
#define AOF_MODE_IMMEDIATE  1

typedef enum {
    ROLE_MASTER = 0,
    ROLE_SLAVE
} kvs_role_t;

typedef struct {
    kvs_role_t role;
    int port;
    char master_ip[64];
    int master_port;

    int snap_enable;
    int aof_enable;

    int aof_mode;       // 0: everysec, 1: immediate
} kvs_config_t;

extern kvs_config_t g_config;

int kvs_config_load(const char *filename);
void kvs_config_print(void);



/*
======================================= 8.  日志功能 ======================================
==========================================================================================
*/
/* ===== 日志级别 ===== */
typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_FATAL
} log_level_t;

int  kvs_log_init(const char *filename, log_level_t min_level);
void kvs_log_fini(void);
void kvs_log_set_level(log_level_t level);
log_level_t kvs_log_get_level(void);

void kvs_log_write(log_level_t level,
                   const char *file, int line,
                   const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

#define LOG_DEBUG(fmt, ...) \
    kvs_log_write(LOG_LEVEL_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  \
    kvs_log_write(LOG_LEVEL_INFO,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  \
    kvs_log_write(LOG_LEVEL_WARN,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) \
    kvs_log_write(LOG_LEVEL_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_FATAL(fmt, ...) \
    kvs_log_write(LOG_LEVEL_FATAL, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

void kvs_free(void *ptr);
void *kvs_malloc (size_t size);


/* ===== 统一同步日志宏（走 logger） ===== */
#define SYNC_LOG(fmt, ...)   LOG_INFO ("[SYNC] " fmt, ##__VA_ARGS__)
#define SYNC_LOG_M(fmt, ...) LOG_INFO ("[SYNC][MASTER] " fmt, ##__VA_ARGS__)
#define SYNC_LOG_S(fmt, ...) LOG_INFO ("[SYNC][SLAVE] " fmt, ##__VA_ARGS__)
#define SYNC_ERR(fmt, ...)   LOG_ERROR("[SYNC] " fmt, ##__VA_ARGS__)
#endif