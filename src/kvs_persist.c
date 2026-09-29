#include "kvstore.h"
#include "kvs_uring.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

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

#define READ_KEY 0
#define READ_VALUE 1

/*snap快照统一调度接口*/
typedef int (*snapshot_save_fn)(const char *filename);
// 定义回调函数类型
typedef int (*kv_set_fn)(void *inst, char *key, char *value);


int save_snapshot_array(const char *filename);
int save_snapshot_rbtree(const char *filename);
int save_snapshot_hash(const char *filename) ;
int save_snapshot_skiptable(const char *filename) ;

// 使用结构体+函数指针解决不同引擎独立保存和加载复杂的问题
typedef struct engine_persist_s {
    const char *name;
    snapshot_save_fn save_fn;
} engine_persist_t;

typedef enum {
    CMD_SET, CMD_MOD, CMD_DEL,
    CMD_RSET, CMD_RMOD, CMD_RDEL,
    CMD_HSET, CMD_HMOD, CMD_HDEL,
    CMD_SSET, CMD_SMOD, CMD_SDEL
} aof_cmd_t;

// 引擎配置
static engine_persist_t engine_persist_table[] = {
    {"array",       save_snapshot_array},
    {"rbtree",      save_snapshot_rbtree},
    {"hash",        save_snapshot_hash},
    {"skiptable",   save_snapshot_skiptable},
    {NULL, NULL}
};

// ===== 写入一个 token =====
static int write_token(FILE *fp, const char *data) {
    if (!data) return -1;
    int len = strlen(data);
    fprintf(fp, "$%d\r\n%s\r\n", len, data);
    return 0;
}

// ===== 读取一个 token =====
static char* read_token(FILE *fp) {
    char line[64];
    // 读取 $长度\r\n
    if (!fgets(line, sizeof(line), fp)) return NULL;
    
    // 跳过空行
    while (line[0] == '\n' || line[0] == '\r') {
        if (!fgets(line, sizeof(line), fp)) return NULL;
    }
    
    // 必须是 $ 开头
    if (line[0] != '$') {
        printf("[Snap] ERROR: expected '$', got '%c'\n", line[0]);
        return NULL;
    }
    
    // 解析长度
    int len = atoi(line + 1);
    if (len < 0) return NULL;
    
    // 读取数据
    char *data = (char*)malloc(len + 1);
    if (!data) return NULL;
    
    int n = fread(data, 1, len, fp);
    if (n != len) {
        free(data);
        return NULL;
    }
    data[len] = '\0';
    
    // 跳过 \r\n 或 \n
    int c1 = fgetc(fp);
    if (c1 == '\r') {
        int c2 = fgetc(fp);
        if (c2 != '\n' && c2 != EOF) ungetc(c2, fp);
    }
    
    return data;
}




#if ENABLE_NEW_FUN
static void travel_rbtree_node(rbtree *T, rbtree_node *node, FILE *fp) {
    if (node == T->nil || node == NULL) return;
    
    travel_rbtree_node(T, node->left, fp);
    
    write_token(fp, node->key);
    write_token(fp, (char*)node->value);   // 直接连在一起
    
    travel_rbtree_node(T, node->right, fp);
}
#else
// 递归遍历红黑树
static void travel_rbtree_node(rbtree *T, rbtree_node *node, FILE *fp) {
    if (node == T->nil || node == NULL) {
        return; // 遇到哨兵节点或空节点则返回
    }
    // 1. 先遍历左子树
    travel_rbtree_node(T, node->left, fp);
    // 2. 保存当前节点（中序遍历的位置）
    fprintf(fp, "%s %s\n", node->key, (char*)node->value);
    // 3. 再遍历右子树
    travel_rbtree_node(T, node->right, fp);
}
#endif




/* 底层函数 */
// 快照保存
#if ENABLE_NEW_FUN
int save_snapshot_array(const char *filename) {
    FILE *fp = fopen(filename, "a");
    if (!fp) return -1;
    
    fprintf(fp, "# array\n");
    int i = 0;
    for (i = 0; i < global_array.total; i++) {
        if (global_array.table[i].key != NULL) {
            write_token(fp, global_array.table[i].key);
            write_token(fp, global_array.table[i].value);   // 直接连在一起
        }
    }
    
    fclose(fp);
    printf("Snapshot array saved to %s\n", filename);
    return 0;
}

int save_snapshot_rbtree(const char *filename) {
    FILE *fp = fopen(filename, "a");
    if (!fp) return -1;
    
    fprintf(fp, "# rbtree\n");
    travel_rbtree_node(&global_rbtree, global_rbtree.root, fp);
    
    fclose(fp);
    printf("Snapshot rbtree saved to %s\n", filename);
    return 0;
}

int save_snapshot_hash(const char *filename) {
    FILE *fp = fopen(filename, "a");
    if (!fp) return -1;
    
    fprintf(fp, "# hash\n");
    int i = 0;
    for (i = 0; i < global_hash.max_slots; i++) {
        hashnode_t *node = global_hash.nodes[i];
        while (node != NULL) {
            write_token(fp, node->key);
            write_token(fp, node->value);   // 直接连在一起
            node = node->next;
        }
    }
    
    fclose(fp);
    printf("Snapshot hash saved to %s\n", filename);
    return 0;
}

int save_snapshot_skiptable(const char *filename) {
    FILE *fp = fopen(filename, "a");
    if (!fp) return -1;
    
    fprintf(fp, "# skiptable\n");
    
    kvs_node_t *curr = global_skiptable.header->forward[0];
    while (curr != NULL) {
        write_token(fp, curr->key);
        write_token(fp, curr->value);   // 直接连在一起
        curr = curr->forward[0];
    }
    
    fclose(fp);
    printf("Snapshot skiptable saved to %s\n", filename);
    return 0;
}

#else
int save_snapshot_array(const char *filename) {
    FILE *fp = fopen(filename, "w");
    if (!fp) return -1;
    int i = 0;

    for (i=0; i<global_array.total; i++) {
        if (global_array.table[i].key != NULL) {
            fprintf(fp, "%s %s\n", global_array.table[i].key, global_array.table[i].value);
        }
    }
    fclose(fp);
    printf("Snapshot saved to %s\n", filename);
    return 0;
}

int save_snapshot_rbtree(const char *filename) {
    FILE *fp = fopen(filename, "w");
    if (!fp) return -1;

    travel_rbtree_node(&global_rbtree, global_rbtree.root, fp);
    fclose(fp);
    printf("Snapshot saved to %s\n", filename);
    return 0;
}

int save_snapshot_hash(const char *filename) {
    FILE *fp = fopen(filename, "w");
    if (!fp) return -1;
    int i = 0;
    for (i=0; i<global_hash.max_slots; i++) {
        // 遍历当前槽位的链表
        hashnode_t *node = global_hash.nodes[i];
        while (node != NULL) {
            fprintf(fp, "%s %s\n", node->key, node->value);
            node = node->next;
        }
    }
    fclose(fp);
    printf("Snapshot saved to %s\n", filename);
    return 0;
}

int save_snapshot_skiptable(const char *filename) {
    FILE *fp = fopen(filename, "w");
    if (!fp) return -1;
    // 从第0层开始遍历
    kvs_node_t *curr = global_skiptable.header->forward[0];

    while (curr != NULL) {
        fprintf(fp, "%s %s\n", curr->key, curr->value);
        curr = curr->forward[0];    // 沿着第0层往下走
    }
    fclose(fp);
    printf("Snapshot saved to %s\n", filename);
    return 0;
}

#endif




#if ENABLE_NEW_FUN

// aof文件的fd，进程启动打开一次，且常开
static int g_aof_fd = -1;
// uring封装实例，管理写入队列和worker线程
static kvs_uring_t *g_aof_uring = NULL;

// 构建aof字节串
static int build_aof_record(const char *cmd, const char *key, const char *value, char **out_buf, size_t *out_len) {
    if (!cmd || !key || !out_buf || !out_len)
        return -EINVAL;

    int need;
    // 如果没有value表示是删除del等cmd，有value表示是set等
    if (value) {
        // 用snprintf预计算长度，计算aof record需要的字节数
        // 此时str==null&&size==0，不写入如任何东西
        need = snprintf(NULL, 0,
                        "%s$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                        cmd, strlen(key), key,
                        strlen(value), value);
    } else {
        need = snprintf(NULL, 0,
                        "%s$%zu\r\n%s\r\n",
                        cmd, strlen(key), key);
    }

    if (need < 0)
        return -EINVAL;
    // 给返回数据分配内存，且末尾保留\0：因为snprintf总是再末尾加入\0，如果不写，会被截断
    char *buf = (char *)malloc((size_t)need + 1);
    if (!buf)
        return -ENOMEM;

    if (value) {
        snprintf(buf, (size_t)need + 1,
                 "%s$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                 cmd, strlen(key), key,
                 strlen(value), value);
    } else {
        snprintf(buf, (size_t)need + 1,
                 "%s$%zu\r\n%s\r\n",
                 cmd, strlen(key), key);
    }

    *out_buf = buf;
    *out_len = (size_t)need;
    return 0;
}

// 进程启动时调用一次，初始化aof持久化的uring环境
/**
 * 返回值：
 * 0：aof未启用/ 已经初始化过了/ 正常初始化成功
 */
int persistence_init(void)
{
    if (!g_config.aof_enable) return 0;

    if (g_aof_uring) return 0;
    // 打开aof文件获得fd    读写模式    不存在则创建 exec 时自动关闭
    g_aof_fd = open(AOF_FILE, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (g_aof_fd < 0) {
        printf("[AOF] open %s failed: %s\n", AOF_FILE, strerror(errno));
        return -errno;
    }

    struct stat st;
    // 获取文件大小，作为首次写入offset
    if (fstat(g_aof_fd, &st) < 0) {
        int err = errno;
        close(g_aof_fd);
        g_aof_fd = -1;
        return -err;
    }

    // 初始化uring子系统，失败就清理fd
    int ret = kvs_uring_init(&g_aof_uring, g_aof_fd, st.st_size);
    if (ret < 0) {
        printf("[AOF] io_uring init failed: %s\n",
               strerror(-ret));
        close(g_aof_fd);
        g_aof_fd = -1;
        return ret;
    }

    printf("[AOF] io_uring writer started, offset=%lld\n", (long long)st.st_size);

    return 0;
}

/**
 * 业务层调用，异步写aof
 * 构造 AOF 字节串 → 入队到 io_uring → 立刻返回（不阻塞）
 */
int append_aof(const char *cmd, const char *key, const char *value)
{
    if (!g_config.aof_enable) return 0;
    if (!g_aof_uring) return -ESHUTDOWN;    //返回-108：未初始化

    char *record = NULL;
    size_t record_len = 0;

    // 把cmd拼接为aof格式
    int ret = build_aof_record(cmd, key, value, &record, &record_len);
    if (ret < 0) return ret;
    // 入队@@@
    ret = kvs_uring_append(g_aof_uring, record, record_len);
    free(record);   // 释放

    return ret;
}

// 等所有已经入队的 AOF 写请求完成。
// 返回0：AOF 未启用，没有请求要等；io_uring 未初始化，没有队列
// 上级调用：SAVE命令中
int persistence_flush(void)
{
    if (!g_config.aof_enable || !g_aof_uring) return 0;
    return kvs_uring_flush(g_aof_uring);
}
// 清空aof
// 清空文件+重置uring的offset
// 上级调用：SAVE 命令生成快照后
int persistence_reset_aof(void)
{
    if (!g_config.aof_enable || !g_aof_uring) return 0;
    return kvs_uring_reset(g_aof_uring);
}

// 退出清理：进程退出时清理 AOF 子系统。
void persistence_shutdown(void)
{   
    // 先关闭uring
    if (g_aof_uring) {
        kvs_uring_shutdown(g_aof_uring);
        g_aof_uring = NULL;
    }
    // 再关闭fd
    if (g_aof_fd >= 0) {
        close(g_aof_fd);
        g_aof_fd = -1;
    }
}


int replay_aof(const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) return -1;

    int count = 0;
    int ch;

    while ((ch = fgetc(fp)) != EOF) {
        if (ch == '\n' || ch == '\r' || ch == ' ' || ch == '\t') continue;

        char cmd[16];
        int i = 0;
        while (ch != EOF && ch != '$' && ch != '\n' && ch != '\r') {
            if (i < (int)sizeof(cmd) - 1) cmd[i++] = (char)ch;
            ch = fgetc(fp);
        }
        cmd[i] = '\0';

        if (ch == EOF) break;
        if (i == 0) continue;

        if (ch == '$') ungetc(ch, fp);

        char *key = read_token(fp);
        if (!key) {
            printf("[AOF] ERROR: failed to read key\n");
            break;
        }

        char *value = NULL;
        int has_value = (strcmp(cmd, "DEL") != 0 &&
                         strcmp(cmd, "RDEL") != 0 &&
                         strcmp(cmd, "HDEL") != 0 &&
                         strcmp(cmd, "SDEL") != 0);

        if (has_value) {
            value = read_token(fp);
            if (!value) {
                printf("[AOF] ERROR: failed to read value\n");
                free(key);
                break;
            }
        }

        if (strcmp(cmd, "SET") == 0)       kvs_array_set(&global_array, key, value);
        else if (strcmp(cmd, "DEL") == 0)  kvs_array_del(&global_array, key);
        else if (strcmp(cmd, "MOD") == 0)  kvs_array_mod(&global_array, key, value);
        else if (strcmp(cmd, "RSET") == 0) kvs_rbtree_set(&global_rbtree, key, value);
        else if (strcmp(cmd, "RDEL") == 0) kvs_rbtree_del(&global_rbtree, key);
        else if (strcmp(cmd, "RMOD") == 0) kvs_rbtree_mod(&global_rbtree, key, value);
        else if (strcmp(cmd, "HSET") == 0) kvs_hash_set(&global_hash, key, value);
        else if (strcmp(cmd, "HDEL") == 0) kvs_hash_del(&global_hash, key);
        else if (strcmp(cmd, "HMOD") == 0) kvs_hash_mod(&global_hash, key, value);
        else if (strcmp(cmd, "SSET") == 0) kvs_skiptable_set(&global_skiptable, key, value);
        else if (strcmp(cmd, "SDEL") == 0) kvs_skiptable_del(&global_skiptable, key);
        else if (strcmp(cmd, "SMOD") == 0) kvs_skiptable_mod(&global_skiptable, key, value);

        free(key);
        if (value) free(value);
        count++;
    }

    fclose(fp);
    printf("Replayed %d entries from %s\n", count, filename);
    return 0;
}
#else

int append_aof(const char *cmd, const char *key, const char *value) {
    FILE *fp = fopen(AOF_FILE, "a");
    if (!fp) return -1;
    
    // 写命令（不带空格）
    fprintf(fp, "%s", cmd);
    
    // 写 key（直接连在一起）
    write_token(fp, key);
    
    // 写 value（如果有，直接连在一起）
    if (value) {
        write_token(fp, value);
    }
    
    fclose(fp);
    return 0;
}

int replay_aof(const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) return -1;
    
    int count = 0;
    int ch;
    
    while ((ch = fgetc(fp)) != EOF) {
        // 跳过换行符和空格
        if (ch == '\n' || ch == '\r' || ch == ' ' || ch == '\t') continue;
        
        // ===== 读取命令名（到 $ 或换行为止） =====
        char cmd[16];
        int i = 0;
        while (ch != EOF && ch != '$' && ch != '\n' && ch != '\r') {
            if (i < sizeof(cmd) - 1) cmd[i++] = ch;
            ch = fgetc(fp);
        }
        cmd[i] = '\0';
        
        if (ch == EOF) break;
        if (i == 0) continue;
        
        // 把 $ 放回
        if (ch == '$') ungetc(ch, fp);
        
        // ===== 读取 key =====
        char *key = read_token(fp);
        if (!key) {
            printf("[AOF] ERROR: failed to read key\n");
            break;
        }
        
        // ===== 判断是不是删除cmd=====
        char *value = NULL;
        int has_value = (strcmp(cmd, "DEL") != 0 &&
                        strcmp(cmd, "RDEL") != 0 &&
                        strcmp(cmd, "HDEL") != 0 &&
                        strcmp(cmd, "SDEL") != 0);
        // 不读，value = NULL
        if (has_value) {
            value = read_token(fp);
            if (!value) {
                printf("[AOF] ERROR: failed to read value\n");
                free(key);
                break;
            }
        }
        
        // ===== 分发 =====
        if (strcmp(cmd, "SET") == 0)       kvs_array_set(&global_array, key, value);
        else if (strcmp(cmd, "DEL") == 0)  kvs_array_del(&global_array, key);
        else if (strcmp(cmd, "MOD") == 0)  kvs_array_mod(&global_array, key, value);
        else if (strcmp(cmd, "RSET") == 0) kvs_rbtree_set(&global_rbtree, key, value);
        else if (strcmp(cmd, "RDEL") == 0) kvs_rbtree_del(&global_rbtree, key);
        else if (strcmp(cmd, "RMOD") == 0) kvs_rbtree_mod(&global_rbtree, key, value);
        else if (strcmp(cmd, "HSET") == 0) kvs_hash_set(&global_hash, key, value);
        else if (strcmp(cmd, "HDEL") == 0) kvs_hash_del(&global_hash, key);
        else if (strcmp(cmd, "HMOD") == 0) kvs_hash_mod(&global_hash, key, value);
        else if (strcmp(cmd, "SSET") == 0) kvs_skiptable_set(&global_skiptable, key, value);
        else if (strcmp(cmd, "SDEL") == 0) kvs_skiptable_del(&global_skiptable, key);
        else if (strcmp(cmd, "SMOD") == 0) kvs_skiptable_mod(&global_skiptable, key, value);
        
        free(key);
        if (value) free(value);
        count++;
    }
    
    fclose(fp);
    printf("Replayed %d entries from %s\n", count, filename);
    return 0;
}
#endif




#if ENABLE_NEW_FUN
int save_snapshots() {
    // 先清空文件
    FILE *fp = fopen(SNAP_FILE, "w");
    if (!fp) return -1;
    fclose(fp);

    // 依次调用各引擎的保存函数
    int ret = 0, i = 0;
    for (i = 0; engine_persist_table[i].name != NULL; i++) {
        if (engine_persist_table[i].save_fn(SNAP_FILE) != 0) {
            printf("failed to save %s snap\n", engine_persist_table[i].name);
            ret = -1;
        }
    }
    return ret;
}

int load_snapshots() {
    FILE *fp = fopen(SNAP_FILE, "r");
    if (!fp) {
        printf("No snapshot file: %s\n", SNAP_FILE);
        return -1;
    }
    
    int count = 0;
    int current_engine = -1;
    int ch;
    
    while ((ch = fgetc(fp)) != EOF) {
        // 跳过换行符和空格
        if (ch == '\n' || ch == '\r' || ch == ' ' || ch == '\t') continue;
        
        // ===== 检测引擎标记 =====
        if (ch == '#') {
            char line[64];
            int i = 0;
            while ((ch = fgetc(fp)) != EOF && ch != '\n') {
                if (i < sizeof(line) - 1) line[i++] = ch;
            }
            line[i] = '\0';
            line[strcspn(line, "\r\n")] = '\0';
            
            if (strstr(line, "array"))          current_engine = 0;
            else if (strstr(line, "rbtree"))    current_engine = 1;
            else if (strstr(line, "hash"))      current_engine = 2;
            else if (strstr(line, "skiptable")) current_engine = 3;
            continue;
        }
        
        // ===== 读取 $ 开头的 token =====
        if (ch == '$') {
            ungetc(ch, fp);
            
            char *key = read_token(fp);
            if (!key) {
                printf("[Snap] ERROR: failed to read key\n");
                break;
            }
            
            char *value = read_token(fp);
            if (!value) {
                printf("[Snap] ERROR: failed to read value\n");
                free(key);
                break;
            }
            
            // ===== 根据当前引擎分发 =====
            switch (current_engine) {
                case 0: kvs_array_set(&global_array, key, value); break;
                case 1: kvs_rbtree_set(&global_rbtree, key, value); break;
                case 2: kvs_hash_set(&global_hash, key, value); break;
                case 3: kvs_skiptable_set(&global_skiptable, key, value); break;
            }
            
            free(key);
            free(value);
            count++;
        }
    }
    
    fclose(fp);
    printf("Loaded %d entries from %s\n", count, SNAP_FILE);
    return 0;
}
#else

//snap保存接口
int save_snapshots() {
    int ret = 0, i = 0;
    for (i=0; engine_persist_table[i].name!=NULL; i++) {
        if (engine_persist_table[i].save_fn(engine_persist_table[i].snap_file)!=0) {
            printf("failed to save %s snap\n", engine_persist_table[i].name);
            ret = -1;
        }
    }
    return ret;
}

//snap加载接口
int load_snapshots() {
    int ret = 0, i = 0;
    for (i=0; engine_persist_table[i].name!=NULL; i++) {
        if (engine_persist_table[i].load_fn(engine_persist_table[i].snap_file)!=0) {
            printf("failed to load %s snap\n", engine_persist_table[i].name);
            ret = -1;
        }
    }
    return ret;
}

#endif





