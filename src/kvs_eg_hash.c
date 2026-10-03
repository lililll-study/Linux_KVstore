


#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>


#include "kvstore.h"


// Key, Value --> 
// Modify 



kvs_hash_t global_hash;
hashtable_t hash;

//Connection 
// 'C' + 'o' + 'n'
static int _hash(char *key, int size) {

	if (!key) return -1;

	int sum = 0;
	int i = 0;

	while (key[i] != 0) {
		sum += key[i];
		i ++;
	}

	return sum % size;

}

hashnode_t *_create_node(char *key, char *value) {
#if ENABLE_MEMPOOL
	hashnode_t *node = (hashnode_t*)kvs_mempool_alloc(hash_node_pool, sizeof(hashnode_t));
#else
	hashnode_t *node = (hashnode_t*)kvs_malloc(sizeof(hashnode_t));
#endif
	if (!node) return NULL;
	
#if ENABLE_KEY_POINTER

#if ENABLE_MEMPOOL
	char *kcopy = kvs_mempool_alloc(hash_string_pool,  strlen(key) + 1);
	if (kcopy == NULL) {
		kvs_mempool_free(hash_node_pool, node);
		return NULL;
	}

	char *kvalue = kvs_mempool_alloc(hash_string_pool, strlen(value) + 1);
	if (kvalue == NULL) { 
		kvs_mempool_free(hash_string_pool, kcopy);
		kvs_mempool_free(hash_node_pool, node);     // 释放 node
		return NULL;
	}
#else
	char *kcopy = kvs_malloc(strlen(key) + 1);
	if (kcopy == NULL) {
		kvs_free(node);
		return NULL;
	}

	char *kvalue = kvs_malloc(strlen(value) + 1);
	if (kvalue == NULL) { 
		kvs_free(kcopy);
		kvs_free(node);
		return NULL;
	}

#endif
	memset(kcopy, 0, strlen(key) + 1);
	strncpy(kcopy, key, strlen(key));
	node->key = kcopy;

	memset(kvalue, 0, strlen(value) + 1);
	strncpy(kvalue, value, strlen(value));
	node->value = kvalue;
	
#else
	strncpy(node->key, key, MAX_KEY_LEN);
	strncpy(node->value, value, MAX_VALUE_LEN);
#endif
	node->next = NULL;

	return node;
}

static void _free_node(hashnode_t *node) {
#if ENABLE_MEMPOOL
    kvs_mempool_free(hash_string_pool, node->key);
    kvs_mempool_free(hash_string_pool, node->value);
    kvs_mempool_free(hash_node_pool, node);
#else
    kvs_free(node->key);
    kvs_free(node->value);
    kvs_free(node);
#endif
}
//
int kvs_hash_create(kvs_hash_t *hash) {

	if (!hash) return -1;

	hash->nodes = (hashnode_t**)kvs_malloc(sizeof(hashnode_t*) * MAX_TABLE_SIZE);
	if (!hash->nodes) return -1;

	hash->max_slots = MAX_TABLE_SIZE;
	hash->count = 0; 

	return 0;
}

// 
void kvs_hash_destory(kvs_hash_t *hash) {

	if (!hash) return;
	printf("Destroying hash table, count=%d\n", hash->count);
	int i = 0;
	for (i = 0;i < hash->max_slots;i ++) {
		hashnode_t *node = hash->nodes[i];

		while (node != NULL) { // error

			hashnode_t *tmp = node;
			node = node->next;
			_free_node(tmp);
// 			hash->nodes[i] = node;
// #if ENABLE_KEY_POINTER
// #if ENABLE_MEMPOOL
// 			kvs_mempool_free(hash_node_pool, tmp);
// #else
// 			kvs_free(tmp->key);
//             kvs_free(tmp->value);
// #endif
// #endif
// 			kvs_free(tmp);
		}
		hash->nodes[i] = NULL;
	}

	kvs_free(hash->nodes);
	hash->nodes = NULL;
	printf("Hash destroyed\n");
}

// 5 + 2

// mp
int kvs_hash_set(kvs_hash_t *hash, char *key, char *value) {

	if (!hash || !key || !value) return -1;

	int idx = _hash(key, MAX_TABLE_SIZE);

	hashnode_t *node = hash->nodes[idx];
	// printf("alloc set hash\n");
#if 1
	while (node != NULL) {
		if (strcmp(node->key, key) == 0) { // exist
			return 1;
		}
		node = node->next;
	}
#endif

	hashnode_t *new_node = _create_node(key, value);
	new_node->next = hash->nodes[idx];
	hash->nodes[idx] = new_node;
	
	hash->count ++;
    // 每 10000 条打印一次
    if (hash->count % 10000 == 0) {
        printf("[Hash]      Progress: %d keys loaded\n", hash->count);
    }
	return 0;
}


char * kvs_hash_get(kvs_hash_t *hash, char *key) {

	if (!hash || !key) return NULL;

	int idx = _hash(key, MAX_TABLE_SIZE);

	hashnode_t *node = hash->nodes[idx];

	while (node != NULL) {

		if (strcmp(node->key, key) == 0) {
			return node->value;
		}

		node = node->next;
	}

	return NULL;

}


int kvs_hash_mod(kvs_hash_t *hash, char *key, char *value) {

	if (!hash || !key) return -1;

	int idx = _hash(key, MAX_TABLE_SIZE);

	hashnode_t *node = hash->nodes[idx];

	while (node != NULL) {

		if (strcmp(node->key, key) == 0) {
			break;
		}

		node = node->next;
	}

	if (node == NULL) {
		return 1;
	}

	// node --> 
#if ENABLE_MEMPOOL
    // 释放旧 value
    kvs_mempool_free(hash_string_pool, node->value);
    
    // 从内存池分配新 value
    char *kvalue = kvs_mempool_alloc(hash_string_pool, strlen(value) + 1);
    if (kvalue == NULL) return -2;
	memset(kvalue, 0, strlen(value) + 1);
#else
	kvs_free(node->value);

	char *kvalue = kvs_malloc(strlen(value) + 1);
	if (kvalue == NULL) return -2;
	memset(kvalue, 0, strlen(value) + 1);
#endif
	strncpy(kvalue, value, strlen(value));
	node->value = kvalue;
	
	return 0;
}

int kvs_hash_count(kvs_hash_t *hash) {
	return hash->count;
}

int kvs_hash_del(kvs_hash_t *hash, char *key) {
	if (!hash || !key) return -1;	// 参数错误
	if (!hash->nodes) return 1;  // key不存在
	int idx = _hash(key, MAX_TABLE_SIZE);

	hashnode_t *head = hash->nodes[idx];


	if (head == NULL) return 1; // noexist
	// 删除head node
	if (strcmp(head->key, key) == 0) {
		hash->nodes[idx] = head->next;   // 关键：先摘链
		_free_node(head);
// 		hash->nodes[idx] = tmp;
// #if ENABLE_KEY_POINTER
// #if ENABLE_MEMPOOL
//         kvs_mempool_free(hash_node_pool, head);
// 		kvs_mempool_free(hash_node_pool, tmp);
// #else
//         kvs_free(head->key);
//         kvs_free(head->value);
// #endif
// #endif
// 		kvs_free(head);
		hash->count --;
		return 0;	// 删除成功
	}
	// 遍历查找
	hashnode_t *cur = head;
	while (cur->next != NULL) {
		if (strcmp(cur->next->key, key) == 0) break; // search node
		
		cur = cur->next;
	}

	if (cur->next == NULL) {
		return 1;
	}

	hashnode_t *tmp = cur->next;
	cur->next = tmp->next;
// #if ENABLE_KEY_POINTER
// #if ENABLE_MEMPOOL
//     kvs_mempool_free(hash_string_pool, tmp->key);
//     kvs_mempool_free(hash_string_pool, tmp->value);
// #else
//     kvs_free(tmp->key);
//     kvs_free(tmp->value);
// #endif
// #endif
// 	kvs_free(tmp);
	_free_node(tmp);
	hash->count --;

	return 0;
}


int kvs_hash_exist(kvs_hash_t *hash, char *key) {

	char *value = kvs_hash_get(hash, key);
	if (!value) return 1;

	return 0;
	
}

#if 0
int main() {

	kvs_hash_create(&hash);

	kvs_hash_set(&hash, "Teacher1", "King");
	kvs_hash_set(&hash, "Teacher2", "Darren");
	kvs_hash_set(&hash, "Teacher3", "Mark");
	kvs_hash_set(&hash, "Teacher4", "Vico");
	kvs_hash_set(&hash, "Teacher5", "Nick");

	char *value1 = kvs_hash_get(&hash, "Teacher1");
	printf("Teacher1 : %s\n", value1);

	int ret = kvs_hash_mod(&hash, "Teacher1", "King1");
	printf("mode Teacher1 ret : %d\n", ret);
	
	char *value2 = kvs_hash_get(&hash, "Teacher1");
	printf("Teacher2 : %s\n", value1);

	ret = kvs_hash_del(&hash, "Teacher1");
	printf("delete Teacher1 ret : %d\n", ret);

	ret = kvs_hash_exist(&hash, "Teacher1");
	printf("Exist Teacher1 ret : %d\n", ret);

	kvs_hash_destory(&hash);

	return 0;
}

#endif


