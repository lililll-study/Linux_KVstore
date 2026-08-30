
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
#if 0
    inst->table = kvs_mempool_alloc(array_item_pool, KVS_ARRAY_SIZE * sizeof(kvs_array_item_t));
    if (!inst->table) return -1;
#else
    inst->table = kvs_malloc(KVS_ARRAY_SIZE * sizeof(kvs_array_item_t));
    if (!inst->table) return -1;
#endif

    inst->total = 0;

    return 0;
}

void kvs_array_destory(kvs_array_t *inst) {
    if (!inst) return;

    if (inst->table) {
#if ENABLE_MEMPOOL
        kvs_mempool_free(array_item_pool, inst->table);
#else
        kvs_free(inst->table);
#endif
    }
}


// 实现五个接口

/*
@return :<0 error; =0 success; >0 exist;
*/

int kvs_array_set(kvs_array_t *inst, char *key, char *value) {

    if (inst == NULL || key == NULL || value == NULL) return -1;
    if (inst->total == KVS_ARRAY_SIZE) return -1;

    char *str = kvs_array_get(inst, key);
    if (str) {
        return 1; // 已经存在
    }
#if ENABLE_MEMPOOL
// 使用内存池分配 key
    // printf("alloc set array\n");
    char *kcopy = kvs_mempool_alloc(array_string_pool, strlen(key) + 1);
    if (!kcopy) return -2;
    strcpy(kcopy, key);
    
    // 使用内存池分配 value
    char *kvalue = kvs_mempool_alloc(array_string_pool, strlen(value) + 1);
    if (!kvalue) {
        kvs_mempool_free(array_string_pool, kcopy);
        return -2;
    }
    strcpy(kvalue, value);
#else
    char *kcopy = kvs_malloc(strlen(key) +1);
    if (kcopy == NULL) return -2;
    memset(kcopy, 0, strlen(key) +1);
    strncpy(kcopy, key, strlen(key));

    char *kvalue = kvs_malloc(strlen(value) +1);
    if (kvalue == NULL) return -2;
    memset(kvalue, 0, strlen(value) +1);
    strncpy(kvalue, value, strlen(value));
#endif
    // 查找我们已经用了多少，去中间找空的
    int i = 0;
    for (i=0; i<inst->total; i++) {
        if (inst->table[i].key == NULL) {
            inst->table[i].key = kcopy;
            inst->table[i].value = kvalue;
            inst->total ++;

            return 0;
        }
    }
    if (i == inst->total && i < KVS_ARRAY_SIZE) {
            inst->table[i].key = kcopy;
            inst->table[i].value = kvalue;
            inst->total ++;
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
    if (inst->total == 0) {
        return KVS_ARRAY_SIZE;
    }
    
    int i=0;
    for (i=0; i<inst->total; i++) {
        if (inst->table[i].key == NULL) {
            continue;
        }
        // 说明找到了key
        if (strcmp(inst->table[i].key, key) == 0) {
            #if ENABLE_MEMPOOL
            // 释放旧 value
            kvs_mempool_free(array_string_pool, inst->table[i].value);
            
            // 从内存池分配新 value
            char *kvalue = kvs_mempool_alloc(array_string_pool, strlen(value) + 1);
            if (!kvalue) return -2;
#else
            // 释放旧 value
            kvs_free(inst->table[i].value);
            
            // 分配新 value
            char *kvalue = kvs_malloc(strlen(value) + 1);
            if (!kvalue) return -2;
#endif
            strncpy(kvalue, value, strlen(value));
            // 插入table
            inst->table[i].value = kvalue;
            return 0;
        }
    }
    return i;

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