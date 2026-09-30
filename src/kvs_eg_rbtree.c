


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kvstore.h"




rbtree_node *rbtree_mini(rbtree *T, rbtree_node *x) {
	while (x->left != T->nil) {
		x = x->left;
	}
	return x;
}

rbtree_node *rbtree_maxi(rbtree *T, rbtree_node *x) {
	while (x->right != T->nil) {
		x = x->right;
	}
	return x;
}

rbtree_node *rbtree_successor(rbtree *T, rbtree_node *x) {
	rbtree_node *y = x->parent;

	if (x->right != T->nil) {
		return rbtree_mini(T, x->right);
	}

	while ((y != T->nil) && (x == y->right)) {
		x = y;
		y = y->parent;
	}
	return y;
}


void rbtree_left_rotate(rbtree *T, rbtree_node *x) {

	rbtree_node *y = x->right;  // x  --> y  ,  y --> x,   right --> left,  left --> right

	x->right = y->left; //1 1
	if (y->left != T->nil) { //1 2
		y->left->parent = x;
	}

	y->parent = x->parent; //1 3
	if (x->parent == T->nil) { //1 4
		T->root = y;
	} else if (x == x->parent->left) {
		x->parent->left = y;
	} else {
		x->parent->right = y;
	}

	y->left = x; //1 5
	x->parent = y; //1 6
}


void rbtree_right_rotate(rbtree *T, rbtree_node *y) {

	rbtree_node *x = y->left;

	y->left = x->right;
	if (x->right != T->nil) {
		x->right->parent = y;
	}

	x->parent = y->parent;
	if (y->parent == T->nil) {
		T->root = x;
	} else if (y == y->parent->right) {
		y->parent->right = x;
	} else {
		y->parent->left = x;
	}

	x->right = y;
	y->parent = x;
}

void rbtree_insert_fixup(rbtree *T, rbtree_node *z) {

	while (z->parent->color == RED) { //z ---> RED
		if (z->parent == z->parent->parent->left) {
			rbtree_node *y = z->parent->parent->right;
			if (y->color == RED) {
				z->parent->color = BLACK;
				y->color = BLACK;
				z->parent->parent->color = RED;

				z = z->parent->parent; //z --> RED
			} else {

				if (z == z->parent->right) {
					z = z->parent;
					rbtree_left_rotate(T, z);
				}

				z->parent->color = BLACK;
				z->parent->parent->color = RED;
				rbtree_right_rotate(T, z->parent->parent);
			}
		}else {
			rbtree_node *y = z->parent->parent->left;
			if (y->color == RED) {
				z->parent->color = BLACK;
				y->color = BLACK;
				z->parent->parent->color = RED;

				z = z->parent->parent; //z --> RED
			} else {
				if (z == z->parent->left) {
					z = z->parent;
					rbtree_right_rotate(T, z);
				}

				z->parent->color = BLACK;
				z->parent->parent->color = RED;
				rbtree_left_rotate(T, z->parent->parent);
			}
		}
		
	}

	T->root->color = BLACK;
}


void rbtree_insert(rbtree *T, rbtree_node *z) {

	rbtree_node *y = T->nil;
	rbtree_node *x = T->root;

	while (x != T->nil) {
		y = x;
#if ENABLE_KEY_CHAR
		if (strcmp(z->key, x->key) < 0) {
			x = x->left;
		} else if (strcmp(z->key, x->key) > 0){
			x = x->right;
		} else {
			return ;
		}
#else
		if (z->key < x->key) {
			x = x->left;
		} else if (z->key > x->key) {
			x = x->right;
		} else { //Exist
			return ;
		}
#endif
	}

	z->parent = y;
	if (y == T->nil) {
		T->root = z;
#if ENABLE_KEY_CHAR
	} else if (strcmp(z->key, y->key) < 0) {
#else
	} else if (z->key < y->key) {
#endif
		y->left = z;
	} else {
		y->right = z;
	}

	z->left = T->nil;
	z->right = T->nil;
	z->color = RED;

	rbtree_insert_fixup(T, z);
}

void rbtree_delete_fixup(rbtree *T, rbtree_node *x) {

	while ((x != T->root) && (x->color == BLACK)) {
		if (x == x->parent->left) {

			rbtree_node *w= x->parent->right;
			if (w->color == RED) {
				w->color = BLACK;
				x->parent->color = RED;

				rbtree_left_rotate(T, x->parent);
				w = x->parent->right;
			}

			if ((w->left->color == BLACK) && (w->right->color == BLACK)) {
				w->color = RED;
				x = x->parent;
			} else {

				if (w->right->color == BLACK) {
					w->left->color = BLACK;
					w->color = RED;
					rbtree_right_rotate(T, w);
					w = x->parent->right;
				}

				w->color = x->parent->color;
				x->parent->color = BLACK;
				w->right->color = BLACK;
				rbtree_left_rotate(T, x->parent);

				x = T->root;
			}

		} else {

			rbtree_node *w = x->parent->left;
			if (w->color == RED) {
				w->color = BLACK;
				x->parent->color = RED;
				rbtree_right_rotate(T, x->parent);
				w = x->parent->left;
			}

			if ((w->left->color == BLACK) && (w->right->color == BLACK)) {
				w->color = RED;
				x = x->parent;
			} else {

				if (w->left->color == BLACK) {
					w->right->color = BLACK;
					w->color = RED;
					rbtree_left_rotate(T, w);
					w = x->parent->left;
				}

				w->color = x->parent->color;
				x->parent->color = BLACK;
				w->left->color = BLACK;
				rbtree_right_rotate(T, x->parent);

				x = T->root;
			}

		}
	}

	x->color = BLACK;
}

rbtree_node *rbtree_delete(rbtree *T, rbtree_node *z) {

	rbtree_node *y = T->nil;
	rbtree_node *x = T->nil;

	if ((z->left == T->nil) || (z->right == T->nil)) {
		y = z;
	} else {
		y = rbtree_successor(T, z);
	}

	if (y->left != T->nil) {
		x = y->left;
	} else if (y->right != T->nil) {
		x = y->right;
	}
//  只有 x 不是 T->nil 时才设置 parent
    if (x != T->nil) {
        x->parent = y->parent;
    }
	
	if (y->parent == T->nil) {
		T->root = x;
	} else if (y == y->parent->left) {
		y->parent->left = x;
	} else {
		y->parent->right = x;
	}

	if (y != z) {
#if ENABLE_KEY_CHAR
	//先把z和y的key互换，后面删除的是172的kv， 206的需要保存
	// 把206的放到了172处，删除的是206里的东西
		void *tmp = z->key;
		z->key = y->key;
		y->key = tmp;

		tmp = z->value;
		z->value = y->value;
		y->value = tmp;
#else
		z->key = y->key;
		z->value = y->value;
#endif
	}

	if (y->color == BLACK) {
		rbtree_delete_fixup(T, x);
	}

	return y;
}

rbtree_node *rbtree_search(rbtree *T, KEY_TYPE key) {

	rbtree_node *node = T->root;
	while (node != T->nil) {
#if ENABLE_KEY_CHAR 
		if (strcmp(key, node->key) < 0){
			node = node->left;
		} else if (strcmp(key, node->key) > 0) {
			node = node->right;
		}  else {
			return node;
		}
#else

		if (key < node->key) {
			node = node->left;
		} else if (key > node->key) {
			node = node->right;
		} else {
			return node;
		}
#endif	
	}
	return T->nil;
}


void rbtree_traversal(rbtree *T, rbtree_node *node) {
	if (node != T->nil) {
		rbtree_traversal(T, node->left);
#if ENABLE_KEY_CHAR 
		printf("key:%s, value:%s\n", node->key, (char*)node->value);
#else 
		printf("key:%d, color:%d\n", node->key, node->color);
#endif
		rbtree_traversal(T, node->right);
	}
}

#if 0
int main() {

#if ENABLE_KEY_CHAR 
	char* keyArray[10] = {"king","lhy","wyb","wzn", "xiaoli", "xiaozhang","xiangwang","xiaonuo","tongtongzi", "221"};
	char* valueArray[10] = {"1king","2lhy","3wyb","4wzn", "5xiaoli","6xiaozhang","7xiangwang","8xiaonuo","9tongtongzi", "10221"};
	rbtree *T = (rbtree *)malloc(sizeof(rbtree));
	if (T == NULL) {
		printf("malloc failed\n");
		return -1;
	}
	
	T->nil = (rbtree_node*)malloc(sizeof(rbtree_node));
	T->nil->color = BLACK;
	T->root = T->nil;

	rbtree_node *node = T->nil;
	int i = 0;
	for (i = 0;i < 10;i ++) {
		node = (rbtree_node*)malloc(sizeof(rbtree_node));

		node->key = malloc(strlen(keyArray[i]) + 1);
		memset(node->key, 0, strlen(keyArray[i]) + 1);
		strcpy(node->key, keyArray[i]);

		node->value = malloc(strlen(valueArray[i]) + 1);
		memset(node->value, 0, strlen(valueArray[i]) + 1);
		strcpy(node->value, valueArray[i]);

		rbtree_insert(T, node);
		
	}

	rbtree_traversal(T, T->root);
	printf("----------------------------------------\n");

	for (i = 0;i < 10;i ++) {

		rbtree_node *node = rbtree_search(T, keyArray[i]);
		rbtree_node *cur = rbtree_delete(T, node);
		free(cur);

		rbtree_traversal(T, T->root);
		printf("----------------------------------------\n");
	}
#else
	int keyArray[20] = {24,25,13,35,23, 26,67,47,38,98, 20,19,17,49,12, 21,9,18,14,15};

	rbtree *T = (rbtree *)malloc(sizeof(rbtree));
	if (T == NULL) {
		printf("malloc failed\n");
		return -1;
	}
	
	T->nil = (rbtree_node*)malloc(sizeof(rbtree_node));
	T->nil->color = BLACK;
	T->root = T->nil;

	rbtree_node *node = T->nil;
	int i = 0;
	for (i = 0;i < 20;i ++) {
		node = (rbtree_node*)malloc(sizeof(rbtree_node));
		node->key = keyArray[i];
		node->value = NULL;

		rbtree_insert(T, node);
		
	}

	rbtree_traversal(T, T->root);
	printf("----------------------------------------\n");

	for (i = 0;i < 20;i ++) {

		rbtree_node *node = rbtree_search(T, keyArray[i]);
		rbtree_node *cur = rbtree_delete(T, node);
		free(cur);

		rbtree_traversal(T, T->root);
		printf("----------------------------------------\n");
	}
	
#endif

	
}

#endif

typedef struct _rbtree kvs_rbtree_t;
kvs_rbtree_t global_rbtree;


// 5+2
int kvs_rbtree_create(kvs_rbtree_t *inst) {
	if (inst == NULL) return -1;

#if ENABLE_MEMPOOL
    // 使用内存池分配 nil 节点
    inst->nil = (rbtree_node*)kvs_mempool_alloc(rbtree_node_pool, sizeof(rbtree_node));
#else
    inst->nil = (rbtree_node*)kvs_malloc(sizeof(rbtree_node));
#endif
	if (!inst->nil) return -1;
	inst->nil->color = BLACK;
	inst->root = inst->nil;

	return 0;
}

void kvs_rbtree_destory(kvs_rbtree_t *inst) {
	if (inst == NULL) return;

	while (inst->root != inst->nil) {
		rbtree_node *mini = rbtree_mini(inst, inst->root);// 每次找到最小的节点
		rbtree_node *cur = rbtree_delete(inst, mini); // 然后删除它，并free
#if ENABLE_MEMPOOL
        if (cur->key) kvs_mempool_free(rbtree_string_pool, cur->key);
        if (cur->value) kvs_mempool_free(rbtree_string_pool, cur->value);
        kvs_mempool_free(rbtree_node_pool, cur);
#else
        if (cur->key) kvs_free(cur->key);
        if (cur->value) kvs_free(cur->value);
        kvs_free(cur);
#endif

	} 
    // 释放 nil 节点
#if ENABLE_MEMPOOL
    kvs_mempool_free(rbtree_node_pool, inst->nil);
#else
    kvs_free(inst->nil);
#endif
}


int kvs_rbtree_set(kvs_rbtree_t *inst, char *key, char *value) {

	if (inst == NULL || key == NULL || value == NULL) return -1;
	// printf("alloc set rbtree\n");
	// printf("  rbtree_string_pool: free_blocks=%zu, total_blocks=%zu\n",  rbtree_string_pool->free_blocks, rbtree_string_pool->total_blocks);
	rbtree_node *exist = rbtree_search(inst, key);
	if (exist != inst->nil) {
		printf("node exist\n");
        return 1;  // 已存在
    }

#if ENABLE_MEMPOOL
    // 使用内存池分配节点	
    rbtree_node *node = (rbtree_node*)kvs_mempool_alloc(rbtree_node_pool, sizeof(rbtree_node));
    if (!node) {
		// printf("Failed to allocate rbtree node from pool\n");
		return -1;
	}
	// 关键：完全初始化节点
    node->left = inst->nil;
    node->right = inst->nil;
    node->parent = inst->nil;
    node->color = RED;
    node->key = NULL;
    node->value = NULL;
    // printf("  allocating key (len=%zu)...\n", strlen(key) + 1);
    // 使用内存池分配 key
    node->key = kvs_mempool_alloc(rbtree_string_pool, strlen(key) + 1);
    if (!node->key) {
		// printf("  ERROR: Failed to allocate key\n");
        kvs_mempool_free(rbtree_node_pool, node);
        return -2;
    }
    // printf("  key allocated at %p\n", node->key);
    // 使用内存池分配 value
    node->value = kvs_mempool_alloc(rbtree_string_pool, strlen(value) + 1);
    if (!node->value) {
		// printf("  ERROR: Failed to allocate value\n");
        kvs_mempool_free(rbtree_string_pool, node->key);
        kvs_mempool_free(rbtree_node_pool, node);
        return -2;
    }
	// printf("  value allocated at %p\n", node->value);
#else
    rbtree_node *node = (rbtree_node*)kvs_malloc(sizeof(rbtree_node));
    if (!node) return -1;
    
    node->key = kvs_malloc(strlen(key) + 1);
    if (!node->key) {
        kvs_free(node);
        return -2;
    }
    
    node->value = kvs_malloc(strlen(value) + 1);
    if (!node->value) {
        kvs_free(node->key);
        kvs_free(node);
        return -2;
    }
#endif
    
    strcpy(node->key, key);
    strcpy(node->value, value);
    
    rbtree_insert(inst, node);
	// printf("  insert done\n");
    return 0;
}

char *kvs_rbtree_get(kvs_rbtree_t *inst, char *key) {

	if (inst == NULL || key == NULL) return NULL;

	rbtree_node *node = rbtree_search(inst, key);
	if (node == inst->nil) return NULL;

	return node->value;
}

int kvs_rbtree_del(kvs_rbtree_t *inst, char *key) {

	if (inst == NULL || key == NULL) return -1;

	rbtree_node *node = rbtree_search(inst, key);
	// if (!node) return 1; //no exist
	if (node == inst->nil) return 1;

	rbtree_node *cur = rbtree_delete(inst, node);
#if ENABLE_MEMPOOL
    // 释放 key 和 value
    if (cur->key) kvs_mempool_free(rbtree_string_pool, cur->key);
    if (cur->value) kvs_mempool_free(rbtree_string_pool, cur->value);
    // 释放节点
    kvs_mempool_free(rbtree_node_pool, cur);
#else
    if (cur->key) kvs_free(cur->key);
    if (cur->value) kvs_free(cur->value);
    kvs_free(cur);
#endif
	// kvs_free(cur);
	return 0;
}


int kvs_rbtree_mod(kvs_rbtree_t *inst, char *key, char *value) {
	if (inst == NULL || key == NULL || value == NULL) return -1;

	rbtree_node *node = rbtree_search(inst, key);
	if (node == inst->nil) return 1;

#if ENABLE_MEMPOOL
    // 释放旧 value
    kvs_mempool_free(rbtree_string_pool, node->value);
    
    // 从内存池分配新 value
    node->value = kvs_mempool_alloc(rbtree_string_pool, strlen(value) + 1);
    if (!node->value) return -2;
#else
    kvs_free(node->value);
    
    node->value = kvs_malloc(strlen(value) + 1);
    if (!node->value) return -2;
#endif

	strcpy(node->value, value);
	return 0;
}

int kvs_rbtree_exist(kvs_rbtree_t *inst, char *key) {
	if (inst == NULL || key == NULL) return -1;

	rbtree_node *node = rbtree_search(inst, key);
	// if (!node) return 1;
	// 如果查找的节点在红黑树中不存在，则返回1（检查 node 是否等于哨兵节点）
	// 因为 node 是 T->nil，不是 NULL
	if (node == inst->nil) return 1;

	return 0;

}
