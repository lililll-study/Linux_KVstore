#ifndef __KV_STORE_H__
#define __KV_STORE_H__

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include <sys/stat.h>
#include <sys/types.h>

#include <time.h>      
#include <pthread.h>   
#include <unistd.h>    


#define ENABLE_NEW_FUN 1
#define ENABLE_URING 1
#define NET_REACTOR 0
#define NET_NTYCO 1
#define NET_PROACTOR 2

#define NET_SELECT NET_NTYCO


#define ENABLE_ARRAY         1
#define ENABLE_RBTREE        1
#define ENABLE_HASH			 1
#define ENABLE_SKIPTABLE	 1

#define ENABLE_PERSIST 		 1	 // 开启本地持久化存储
#define ENABLE_MEMPOOL		 1   // 开启内存池
#define ENABLE_LEVEL_MEMPOOL 1   // 开启cmd批处理

#define ENABLE_CMD_BATCH	 1   // 开启cmd批处理

#define ENABLE_SYNC			 1   // 开启同步备份功能
#define ENABLE_AOF_SYNC      1   // 开启增量同步备份
#define ENABLE_REPBUF        1   // 启动响应缓冲区，避免响应过大溢出

/*
======================================= 底层网络架构 ======================================
==========================================================================================
*/

extern  const char *command[];
extern const char *response[];

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

#if ENABLE_NEW_FUN
#define SNAP_FILE "data/data_snap.snap"
#define AOF_FILE   "data/data_aof.aof"
int save_snapshots();
int load_snapshots();
// AOF
int append_aof(const char *cmd, const char *key, const char *value);
int replay_aof(const char *filename);

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

#define KVS_POOL_NODE_COUNT     100000
#define KVS_POOL_STRING_COUNT   200000
#define RESP_POOL_BLOCK_SIZE    4096
#define RESP_POOL_BLOCK_COUNT   1024

// 内存池结构
typedef struct mempool_s{
    size_t block_size;       // 每个内存块的总大小
    size_t total_blocks;    // 内存池总块数
    size_t free_blocks;     // 当前空闲块数量
    void* free_list;        // 空闲链表头指针
    char data[];        // 柔性数组
// data[] 不占结构体空间，但可以通过 pool->data 访问结构体后面的内存：
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

#define SMALL_STRING_THRESHOLD 128  // 小字符串阈值

// 指定块大小和块数量 
mempool_t* kvs_mempool_create(size_t user_block_size, size_t num_blocks);
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

extern int g_is_master;

// 同步函数
int handle_sync_request(int fd);          // 主节点：处理同步
int sync_from_master(const char *master_ip, int port);	// 由从节点发起同步
// 主节点传输和从节点接收
int send_file(int fd, const char *filepath);
int recv_file(int fd, const char *save_dir);

#if ENABLE_AOF_SYNC
// 函数声明
void init_aof_sync_ctx(void);
void disconnect_aof_sync(void);
void* aof_master_check_thread(void* arg);
void* aof_slave_recv_thread(void* arg);

#endif

#endif

/*
======================================= 6.  配置config ======================================
==========================================================================================
*/
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
} kvs_config_t;

extern kvs_config_t g_config;

int kvs_config_load(const char *filename);
void kvs_config_print(void);




void kvs_free(void *ptr);
void *kvs_malloc (size_t size);


#endif