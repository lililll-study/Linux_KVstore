#include "kvstore.h"
#include <stdbool.h>
// 定义跳表最大层数，每个节点可以出现在多个层级中，6表示最高允许建到第五层
// 这样查找时最多跳跃6次就能找到目标
// 一般取log2(n)作为最大层数，比如100w节点，log2(100w)=20


kvs_skiptable_t global_skiptable;

kvs_node_t* createNode(int level, char *key, char *value) {
    kvs_node_t* newNode = (kvs_node_t*)malloc(sizeof(kvs_node_t));
    newNode->key = key ? strdup(key) : NULL;
    newNode->value = value ? strdup(value) : NULL;
    // 指针数组，指向每个元素对应层的下一个节点
    // forward[0] 指向第 0 层的下一个节点，forward[1] 指向第 1 层的下一个节点
    newNode->forward = (kvs_node_t**)malloc((level + 1) * sizeof(kvs_node_t*));
    return newNode;
}

void free_node(kvs_node_t *node) {
    if (node == NULL) return;
    
    if (node->key) kvs_free(node->key);
    if (node->value) kvs_free(node->value);
    if (node->forward) kvs_free(node->forward);
    kvs_free(node);
}

// 生成随机层数
//@func: 用于决定新节点在跳跃表中的高度
// 从第0层开始，每次以1/2的概率决定是否继续增加层数
// 最多增加到max_level层，并返回最终层数
int random_level() {
    int level = 0;
    while (rand() < RAND_MAX/2 && level < MAX_LEVEL) {
        level ++;
    }
    return level;
}

kvs_node_t* search(kvs_skiptable_t* skipList, char* key) {
    if (skipList == NULL || key == NULL) return NULL;

    kvs_node_t* current = skipList->header;
    int i = 0;

    for (i = skipList->level; i >= 0; --i) {
        while (current->forward[i] != NULL && strcmp(current->forward[i]->key, key) < 0)
            current = current->forward[i];
    }

    current = current -> forward[0];

    if(current && strcmp(current->key, key) == 0) {
        // printf("Key %d found with value %d\n", key, current->value);
        return current;
    }else{
        // printf("Key %d not found\n", key);
        return NULL;
    }
}

// 创建一个空的跳表
int kvs_skiptable_create(kvs_skiptable_t *inst) {
    if (inst == NULL) return -1;
    inst->level = 0;
    // 分配具有maxlevel层的头节点（头节点总是有最高层数）
    inst->header = createNode(MAX_LEVEL, NULL, NULL);

    if (inst->header == NULL) return -1;
    return 0;
}

void kvs_skiptable_destory(kvs_skiptable_t *inst) {
    if (inst == NULL || inst->header == NULL) return;

    kvs_node_t *current = inst->header->forward[0];
    kvs_node_t *next;

    // 从第0层开始遍历，释放所有节点
    while (current != NULL) {
        next = current->forward[0];// current指向第0层后继
        free_node(current); // 会释放当前节点及其所有层级的数组指针
        current = next;
    }

    // 释放头节点
    free_node(inst->header);
    inst->header = NULL;
    inst->level = 0;
}

int kvs_skiptable_set(kvs_skiptable_t *inst, char *key, char *value) {
    if (inst == NULL || key == NULL || value == NULL) return -1;

    if (search(inst, key) != NULL) return 1; // key已经存在了


    // update表示在第i层，新节点应该插入在哪个节点之后
    // 1. 初始化查找update和current
    kvs_node_t *update[MAX_LEVEL + 1];
    kvs_node_t *current = inst->header;
    int i = 0;
    // 从最高层向下寻找要插入的位置
    // 2. 查找插入位置
    for (i = inst->level; i>=0; i--) {
        // 在每一层向右移动，直到找到key大于等于当前key的位置
        while (current->forward[i] != NULL && strcmp(current->forward[i]->key, key) <0) {
                current = current->forward[i];
            }
            update[i] = current;// 记录每层的前驱节点
    }
    // 找到位置
    current = current->forward[0];


    // 插入新节点
    // 4. 先分配层数
    int level = random_level();
    if (level > inst->level) {
        // 如果插入层数高于当前跳表层数，补齐update
        for (i = inst->level +1; i<=level; i++) {
            update[i] = inst->header;
        }
        inst->level = level;
    }
    // 5. 创建并插入节点
    // 创建新节点
    kvs_node_t *new_node = createNode(level, key, value);
    if (new_node == NULL) return -1;

    // 在各层插入新节点
    for (i=0; i<=level; i++) {
        // 先把第i层的待插入的前一个节点update的下一个节点，用new_node指向
        new_node->forward[i] = update[i]->forward[i]; // 新节点指向后继
        // 然后更新update指向new_node
        // 这样就变成update->new_node->forward
        update[i]->forward[i] = new_node;           // 前驱指向新节点
    }
    return 0;
}
char *kvs_skiptable_get(kvs_skiptable_t *inst, char *key) {
    if (inst == NULL || key == NULL) return NULL;

    kvs_node_t* node = search(inst, key);
    if (node != NULL) return node->value;
    
    return NULL;
}
int kvs_skiptable_del(kvs_skiptable_t *inst, char *key) {
    if (inst == NULL || key == NULL) return -1;
    
    // 先检查key是否存在
    if (search(inst, key) == NULL) return 1; // key不存在

    kvs_node_t *update[MAX_LEVEL + 1];
    kvs_node_t *current = inst->header;
    int i = 0;
    
    // 初始化 update 数组
    for (i = 0; i <= MAX_LEVEL; i++) {
        update[i] = NULL;
    }

    // 查找要删除的节点
    for (i = inst->level; i >= 0; i--) {
        while (current->forward[i] != NULL && 
               strcmp(current->forward[i]->key, key) < 0) {
            current = current->forward[i];
        }
        update[i] = current;
    }
    
    // 移动到目标节点
    current = current->forward[0];
    
    // 在各层删除节点
    for (i = 0; i <= inst->level; i++) {
        if (update[i] != NULL && update[i]->forward[i] == current) {
            update[i]->forward[i] = current->forward[i];
        }
    }
    
    // 释放节点
    free_node(current);
    
    // 更新跳表层数（如果最高层没有节点了，降低 level）
    while (inst->level > 0 && 
           inst->header->forward[inst->level] == NULL) {
        inst->level--;
    }
    
    return 0;
}
int kvs_skiptable_mod(kvs_skiptable_t *inst, char *key, char *value) {
    if (inst == NULL || key == NULL || value == NULL) return -1;
    
    // 先检查key是否存在
    if (search(inst, key) == NULL) return 1; // key不存在
    
    // update表示在第i层，新节点应该插入在哪个节点之后
    // 1. 初始化查找update和current
    kvs_node_t *update[MAX_LEVEL + 1];
    kvs_node_t *current = inst->header;

    int i = 0;
    // 从最高层向下寻找要插入的位置
    // 2. 查找插入位置
    for (i = inst->level; i>=0; i--) {
        // 在每一层向右移动，直到找到key大于等于当前key的位置
        while (current->forward[i] != NULL && strcmp(current->forward[i]->key, key) <0) {
                current = current->forward[i];
            }
            update[i] = current;// 记录每层的前驱节点
    }
    // 找到位置
    current = current->forward[0];

    // 3. 更新value
    if (current == NULL || strcmp(current->key, key) != 0) {
        // 释放旧的value 替换为新的
        return -1;
    }
    kvs_free(current->value);
    current->value = strdup(value);
    return 0;
}
int kvs_skiptable_exist(kvs_skiptable_t *inst, char *key) {
    if (inst == NULL || key == NULL) return 1;
    // 先检查key是否存在
    if (search(inst, key) == NULL) return 1; // key不存在

    return 0;
}
