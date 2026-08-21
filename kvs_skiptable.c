#include "kvstore.h"

// 定义跳表最大层数，每个节点可以出现在多个层级中，6表示最高允许建到第五层
// 这样查找时最多跳跃6次就能找到目标
// 一般取log2(n)作为最大层数，比如100w节点，log2(100w)=20


kvs_skiptable_t global_skiptable;

int kvs_skiptable_create(kvs_skiptable_t *inst) {

}
void kvs_skiptable_destory(kvs_skiptable_t *inst) {

}
int kvs_skiptable_set(kvs_skiptable_t *inst, char *key, char *value) {

}
char *kvs_skiptable_get(kvs_skiptable_t *inst, char *key) {

}
int kvs_skiptable_del(kvs_skiptable_t *inst, char *key) {

}
int kvs_skiptable_mod(kvs_skiptable_t *inst, char *key, char *value) {

}
int kvs_skiptable_exist(kvs_skiptable_t *inst, char *key) {

}
