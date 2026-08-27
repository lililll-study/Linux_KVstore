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
typedef int (*snapshot_load_fn)(const char *filename);
// 定义回调函数类型
typedef int (*kv_set_fn)(void *inst, char *key, char *value);

typedef struct engine_persist_s {
    const char *name;
    const char *snap_file;
    snapshot_save_fn save_fn;
    snapshot_load_fn load_fn;
} engine_persist_t;

typedef enum {
    CMD_SET, CMD_MOD, CMD_DEL,
    CMD_RSET, CMD_RMOD, CMD_RDEL,
    CMD_HSET, CMD_HMOD, CMD_HDEL,
    CMD_SSET, CMD_SMOD, CMD_SDEL
} aof_cmd_t;

// 引擎配置
static engine_persist_t engine_persist_table[] = {
    {"array",   "data/array.snap",       save_snapshot_array,     load_snapshot_array},
    {"rbtree",  "data/rbtree.snap",      save_snapshot_rbtree,    load_snapshot_rbtree},
    {"hash",    "data/hash.snap",        save_snapshot_hash,      load_snapshot_hash},
    {"skiptable", "data/skiptable.snap", save_snapshot_skiptable, load_snapshot_skiptable},
    {NULL, NULL, NULL, NULL}
};


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

/* 底层函数 */
// 快照保存
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

// 快照加载

// 定义底层解析接口
static int load_snap_func(const char *filename, void *inst, kv_set_fn set_fn) {
    FILE *fp = fopen(filename, "r");
    if (!fp) return -1;
    char ch;
    char *key = NULL;
    char *value = NULL;
    int key_len = 0;
    int value_len = 0;
    int key_cap = 64;
    int value_cap = 64;

    key = (char *)malloc(key_cap);
    value = (char *)malloc(value_cap);

    int read_status = READ_KEY;
    while ((ch = fgetc(fp)) != EOF) {
        if (ch == ' ') {
            // key读结束，开始读value
            key[key_len] = '\0';
            value_len = 0;
            read_status = READ_VALUE;
        } else if (ch == '\n') {
            // 换行：一条数据结束，执行set
            value[value_len] = '\0';
            if (key_len > 0) {
                set_fn(inst, key, value);
            }
            // 准备读下一条
            read_status = READ_KEY;
            key_len = 0;
        } else {
            if (read_status == READ_KEY){
                if (key_len + 1 >= key_cap) {
                    key_cap *= 2;
                    key = (char*)realloc(key, key_cap);
                }
                key[key_len++] = ch;
            } else {
                if (value_len + 1 >= value_cap) {
                    value_cap *= 2;
                    value = (char*)realloc(value, value_cap);
                }
                value[value_len++] = ch;
            }

        }
    }
    free(key);
    free(value);
    fclose(fp);
    return 0;
}

// 封装kv引擎的API
int load_snapshot_array(const char *filename) {
    return load_snap_func(filename, &global_array, (kv_set_fn)kvs_array_set);
}
int load_snapshot_rbtree(const char *filename) {
    return load_snap_func(filename, &global_rbtree, (kv_set_fn)kvs_rbtree_set);
}
int load_snapshot_hash(const char *filename) {
    return load_snap_func(filename, &global_hash, (kv_set_fn)kvs_hash_set);
}
int load_snapshot_skiptable(const char *filename) {
    return load_snap_func(filename, &global_skiptable, (kv_set_fn)kvs_skiptable_set);
}

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
    int ret = 0;

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
                    int ret = kvs_array_set(&global_array, key, value);
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







