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
}

int save_snapshot_hash(const char *filename) {
    FILE *fp = fopen(filename, "w");
    if (!fp) return -1;
}

int save_snapshot_skiptable(const char *filename) {
    FILE *fp = fopen(filename, "w");
    if (!fp) return -1;
}

// 快照加载
int load_snapshot_array(const char *filename) {
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
                kvs_array_set(&global_array, key, value);
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

int load_snapshot_rbtree(const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) return -1;
}

int load_snapshot_hash(const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) return -1;
}

int load_snapshot_skiptable(const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) return -1;
}

// AOF
int append_aof(const char *filename, const char *cmd, const char *key, const char *value) {
    FILE *fp = fopen(filename, "w");
    if (!fp) return -1;
}

int replay_aof(const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) return -1;
}













