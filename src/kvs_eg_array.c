
#include "kvstore.h"


kvs_array_t global_array = {0};

// 用单例模式
// 对于整个项目而言，数组是独一份的
int kvs_array_create(kvs_array_t *inst) {
    if (!inst) return -1;
    if (inst->table) {
        printf("table has alloc\n");
        return -1;
    }
    inst->capacity = KVS_ARRAY_INIT_SIZE;
    inst->total = 0;
    
#if ENABLE_MEMPOOL
    // 从内存池分配表空间
    inst->table = (kvs_array_item_t *)kvs_mempool_alloc(
        array_item_pool, 
        sizeof(kvs_array_item_t) * inst->capacity
    );
#else
    inst->table = (kvs_array_item_t *)kvs_malloc(
        sizeof(kvs_array_item_t) * inst->capacity
    );
#endif
    
    if (!inst->table) return -1;
    int i = 0;
    // 初始化所有槽位
    for (i = 0; i < inst->capacity; i++) {
        inst->table[i].key = NULL;
        inst->table[i].value = NULL;
    }
    
    return 0;
}

void kvs_array_destory(kvs_array_t *inst) {
    if (!inst) return;
    int i = 0;
    if (inst->table) {
        // 先释放所有 key/value
        for (i = 0; i < inst->total; i++) {
            if (inst->table[i].key) {
#if ENABLE_MEMPOOL
                kvs_mempool_free(array_string_pool, inst->table[i].key);
#else
                kvs_free(inst->table[i].key);
#endif
            }
            if (inst->table[i].value) {
#if ENABLE_MEMPOOL
                kvs_mempool_free(array_string_pool, inst->table[i].value);
#else
                kvs_free(inst->table[i].value);
#endif
            }
        }
        
#if ENABLE_MEMPOOL
        kvs_mempool_free(array_item_pool, inst->table);
#else
        kvs_free(inst->table);
#endif
        inst->table = NULL;
    }
    inst->total = 0;
    inst->capacity = 0;
}

// ===== 扩容函数 =====

#if 0
static int kvs_array_expand(kvs_array_t *inst) {
    int new_cap = inst->capacity * KVS_ARRAY_GROW_FACTOR;
    size_t new_size = sizeof(kvs_array_item_t) * new_cap;

    /* ★ 变长表统一 malloc：池块固定大小，装不下翻倍的表 */
    kvs_array_item_t *new_table = (kvs_array_item_t *)kvs_malloc(new_size);
    if (!new_table) {
        printf("[Array] Expand failed: out of memory\n");
        return -1;
    }

    memcpy(new_table, inst->table, sizeof(kvs_array_item_t) * inst->capacity);

    int i = 0;
    for (i = inst->capacity; i < new_cap; i++) {
        new_table[i].key = NULL;
        new_table[i].value = NULL;
    }

    /* ★ 与分配严格配对 */
    kvs_free(inst->table);

    inst->table = new_table;
    inst->capacity = new_cap;

    printf("[Array] Expanded: %d -> %d slots\n", inst->capacity / 2, new_cap);
    return 0;
}
#else
static int kvs_array_expand(kvs_array_t *inst) {
    int new_cap = inst->capacity * KVS_ARRAY_GROW_FACTOR;
    size_t new_size = sizeof(kvs_array_item_t) * new_cap;
    
    kvs_array_item_t *new_table;
    
#if ENABLE_MEMPOOL
    // 从内存池分配新表
    new_table = (kvs_array_item_t *)kvs_mempool_alloc(array_item_pool, new_size);
    if (!new_table) {
        // 内存池分配失败，降级到 malloc
        new_table = (kvs_array_item_t *)kvs_malloc(new_size);
        if (!new_table) {
            printf("[Array] Expand failed: out of memory\n");
            return -1;
        }
        printf("[Array] Expand: using malloc fallback\n");
    }
#else
    new_table = (kvs_array_item_t *)kvs_malloc(new_size);
    if (!new_table) {
        printf("[Array] Expand failed: out of memory\n");
        return -1;
    }
#endif
    
    // 复制旧数据到新表
    memcpy(new_table, inst->table, sizeof(kvs_array_item_t) * inst->capacity);
    int i = 0;
    // 初始化新增的槽位
    for (i = inst->capacity; i < new_cap; i++) {
        new_table[i].key = NULL;
        new_table[i].value = NULL;
    }
    
    // 释放旧表
#if ENABLE_MEMPOOL
    kvs_mempool_free(array_item_pool, inst->table);
#else
    kvs_free(inst->table);
#endif
    
    inst->table = new_table;
    inst->capacity = new_cap;
    
    printf("[Array] Expanded: %d -> %d slots\n", inst->capacity / 2, new_cap);
    return 0;
}
#endif

// 实现五个接口

/*
@return :<0 error; =0 success; >0 exist;
*/

// ===== SET 接口（自动扩容） =====
int kvs_array_set(kvs_array_t *inst, char *key, char *value) {
    if (inst == NULL || key == NULL || value == NULL) return -1;
    int i = 0;
    // 先查找 key 是否已存在（更新）
    for (i = 0; i < inst->total; i++) {
        if (inst->table[i].key != NULL && strcmp(inst->table[i].key, key) == 0) {
//             // 更新 value
// #if ENABLE_MEMPOOL
//             kvs_mempool_free(array_string_pool, inst->table[i].value);
//             char *kvalue = kvs_mempool_alloc(array_string_pool, strlen(value) + 1);
// #else
//             kvs_free(inst->table[i].value);
//             char *kvalue = kvs_malloc(strlen(value) + 1);
// #endif
//             if (!kvalue) return -2;
//             strcpy(kvalue, value);
//             inst->table[i].value = kvalue;
            return 1;
        }
    }
    
    // ===== 如果满了，自动扩容 =====
    if (inst->total >= inst->capacity) {
        if (kvs_array_expand(inst) < 0) {
            return -1;
        }
    }
    
    // 分配 key
#if ENABLE_MEMPOOL
    char *kcopy = kvs_mempool_alloc(array_string_pool, strlen(key) + 1);
    if (!kcopy) return -2;
    strcpy(kcopy, key);
    
    char *kvalue = kvs_mempool_alloc(array_string_pool, strlen(value) + 1);
    if (!kvalue) {
        kvs_mempool_free(array_string_pool, kcopy);
        return -2;
    }
    strcpy(kvalue, value);
#else
    char *kcopy = kvs_malloc(strlen(key) + 1);
    if (!kcopy) return -2;
    strcpy(kcopy, key);
    
    char *kvalue = kvs_malloc(strlen(value) + 1);
    if (!kvalue) {
        kvs_free(kcopy);
        return -2;
    }
    strcpy(kvalue, value);
#endif
    
    // 插入到第一个空位
    for (i = 0; i < inst->capacity; i++) {
        if (inst->table[i].key == NULL) {
            inst->table[i].key = kcopy;
            inst->table[i].value = kvalue;
            inst->total++;
            return 0;
        }
    }
    
    return 0;
}
// @return NULL noexist; value exist
char *kvs_array_get(kvs_array_t *inst, char *key) {

    if (inst == NULL || key == NULL) return NULL;

    int i = 0;
    for (i=0; i<inst->total; i++) {
        if (inst->table[i].key == NULL) {
            continue;
        }
        if (strcmp(inst->table[i].key, key) == 0) {
            return inst->table[i].value;
        }
    }
    return NULL;
}

/*
@return <0, error;
        =0, success;
        >0, noexist
*/

int kvs_array_del(kvs_array_t *inst, char *key) {
    if (inst == NULL || key == NULL) return -1;

    int i =0;
    for (i=0; i< inst->total; i++) {
        if (strcmp(inst->table[i].key, key) == 0) {
#if ENABLE_MEMPOOL
            kvs_mempool_free(array_string_pool, inst->table[i].key);
            kvs_mempool_free(array_string_pool, inst->table[i].value);
#else
            kvs_free(inst->table[i].key);
            kvs_free(inst->table[i].value);
#endif
            inst->table[i].value = NULL;
            inst->table[i].key = NULL;
 // 关键修复：无论删除哪个位置，total 都应该减
            inst->total--;
            return 0;
        }
    }
    return i;
}

/*
@return : <0, error; =0 success; >0 noexist
*/

int kvs_array_mod(kvs_array_t *inst, char *key, char *value) {
    if (inst == NULL || key == NULL || value == NULL) return -1;
    int i = 0;
    for (i = 0; i < inst->total; i++) {
        if (inst->table[i].key == NULL) continue;
        if (strcmp(inst->table[i].key, key) == 0) {
#if ENABLE_MEMPOOL
            kvs_mempool_free(array_string_pool, inst->table[i].value);
            char *kvalue = kvs_mempool_alloc(array_string_pool, strlen(value) + 1);
#else
            kvs_free(inst->table[i].value);
            char *kvalue = kvs_malloc(strlen(value) + 1);
#endif
            if (!kvalue) return -2;
            strcpy(kvalue, value);
            inst->table[i].value = kvalue;
            return 0;
        }
    }
    return 1;  // 不存在
}

/*
@return 0 exist; 1 no exist;
*/
int kvs_array_exist(kvs_array_t *inst, char *key) {

    char *str = kvs_array_get(inst, key);
    if (!str) {
        return 1;
    }
    return 0;
}