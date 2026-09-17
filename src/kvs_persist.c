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
#else

// AOF
int append_aof(const char *filename, const char *cmd, const char *key, const char *value) {
    FILE *fp = fopen(filename, "a");
    if (!fp) return -1;

    if (value) { // set mod
        fprintf(fp, "%s %s %s\n", cmd, key, value);
    } else {// del
        fprintf(fp, "%s %s\n", cmd, key);
    }
    fclose(fp);
    return 0;
}

int replay_aof(const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) return -1;

    char *line = NULL, *cmd = NULL, *key = NULL, *value = NULL;
    size_t line_len = 0;

    while (getline(&line, &line_len, fp) != -1) {

        // 去掉换行符
        line[strcspn(line, "\n")] = 0;
        printf("%s\n", line);

        cmd = (char*)malloc(strlen(line) + 1);
        key = (char*)malloc(strlen(line) + 1);
        value = (char*)malloc(strlen(line) + 1);

        if (!cmd || !key || !value) {
            if (cmd) free(cmd);
            if (key) free(key);
            if (value) free(value);
            break;
        }

        // 解析命令
        int count = sscanf(line, "%s %s %s", cmd, key, value);
        // printf("%d\n", count);
        if (count >= 2) {
            // printf("count start\n");
            // 根据命令前缀分发
            if (cmd[0] == 'S' && cmd[1] == 'S') {
                // printf("set start S\n");
                // 跳表命令
                if (strcmp(cmd, "SSET") == 0 && count == 3) {
                    kvs_skiptable_set(&global_skiptable, key, value);
                } else if (strcmp(cmd, "SMOD") == 0 && count == 3) {
                    kvs_skiptable_mod(&global_skiptable, key, value);
                } else if (strcmp(cmd, "SDEL") == 0) {
                    kvs_skiptable_del(&global_skiptable, key);
                }
            } else if (cmd[0] == 'R' && cmd[1] == 'S') {
                // printf("set start R\n");
                // 红黑树命令
                if (strcmp(cmd, "RSET") == 0 && count == 3) {
                    kvs_rbtree_set(&global_rbtree, key, value);
                } else if (strcmp(cmd, "RMOD") == 0 && count == 3) {
                    kvs_rbtree_mod(&global_rbtree, key, value);
                } else if (strcmp(cmd, "RDEL") == 0) {
                    kvs_rbtree_del(&global_rbtree, key);
                }
            } else if (cmd[0] == 'H' && cmd[1] == 'S') {
                // printf("set start H\n");
                // 哈希命令
                if (strcmp(cmd, "HSET") == 0 && count == 3) {
                    kvs_hash_set(&global_hash, key, value);
                } else if (strcmp(cmd, "HMOD") == 0 && count == 3) {
                    kvs_hash_mod(&global_hash, key, value);
                } else if (strcmp(cmd, "HDEL") == 0) {
                    kvs_hash_del(&global_hash, key);
                }
            } else {
                // 数组命令
                // printf("set start A\n");
                if (strcmp(cmd, "SET") == 0 && count == 3) {
                    // printf("set start\n");
                    kvs_array_set(&global_array, key, value);
                    // printf("kvs_array_set returned: %d\n", ret);
                } else if (strcmp(cmd, "MOD") == 0 && count == 3) {
                    kvs_array_mod(&global_array, key, value);
                } else if (strcmp(cmd, "DEL") == 0) {
                    kvs_array_del(&global_array, key);
                }
            }
        }
        // 释放内存
        free(cmd);
        free(key);
        free(value);
        cmd = NULL;
        key = NULL;
        value = NULL;
    }
    free(line);
    fclose(fp);
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





