
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

    inst->table = kvs_malloc(KVS_ARRAY_SIZE * sizeof(kvs_array_item_t));
    if (!inst->table) return -1;

    inst->total = 0;

    return 0;
}

void kvs_array_destory(kvs_array_t *inst) {
    if (!inst) return;

    if (inst->table) {
        kvs_free(inst->table);
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

    char *kcopy = kvs_malloc(strlen(key) +1);
    if (kcopy == NULL) return -2;
    memset(kcopy, 0, strlen(key) +1);
    strncpy(kcopy, key, strlen(key));

    char *kvalue = kvs_malloc(strlen(value) +1);
    if (kvalue == NULL) return -2;
    memset(kvalue, 0, strlen(value) +1);
    strncpy(kvalue, value, strlen(value));

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
            kvs_free(inst->table[i].key);
            inst->table[i].key = NULL;

            kvs_free(inst->table[i].value);
            inst->table[i].value = NULL;
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
            // 先释放value
            kvs_free(inst->table[i].value);
            // 重新分配value
            char *kvalue = kvs_malloc(strlen(value) +1);
            if (kvalue == NULL) return -2;
            memset(kvalue, 0, strlen(value) +1);
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