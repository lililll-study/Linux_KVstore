#include "kvstore.h"


#if ENABLE_MEMPOOL

// 响应缓冲池
mempool_t *resp_pool = NULL;

// 定义内存池
 mempool_t *array_item_pool = NULL;     // 数组节点池
 mempool_t *array_string_pool = NULL;   // 数组字符串池

 mempool_t *rbtree_node_pool = NULL;    // 红黑树节点池
 mempool_t *rbtree_string_pool = NULL;  // 红黑树字符串池

 mempool_t *hash_node_pool = NULL;      // 哈希节点池
 mempool_t *hash_string_pool = NULL;    // 哈希字符串池

 mempool_t *skiptable_node_pool = NULL; // 跳表节点池
 mempool_t *skiptable_string_pool = NULL; // 跳表字符串池

// 指定块大小和块数量 
mempool_t* kvs_mempool_create(size_t user_block_size, size_t num_blocks) {
    if (user_block_size == 0 || num_blocks == 0) {
        return NULL;
    }

    // 每个块至少要能容纳一个指针
    // 块在空闲的时候，前8字节存下一个块的地址
    // 块被分配后，前8字节用户看到不，只能感知到实际使用空间
    size_t actual_block_size = user_block_size + sizeof(void *); // 块大小
    // 关键修复：对齐到 8 字节边界（或指针大小）
    // 这样确保每个块起始地址对齐，避免内存访问问题
    size_t alignment = sizeof(void *);
    actual_block_size = (actual_block_size + alignment - 1) & ~(alignment - 1);
    size_t pool_size = sizeof(mempool_t) + actual_block_size * num_blocks; // 池大小

    // 分配内存池结构体及内存块空间
    mempool_t* pool = (mempool_t*)malloc(pool_size);
    if (!pool) return NULL;

    // 初始化内存池
    pool->block_size = actual_block_size;
    pool->total_blocks = num_blocks;
    pool->free_blocks = num_blocks;
    pool->free_list = NULL;

    // 初始化空闲链表
    size_t i = 0;
    for (i=0; i<num_blocks; ++i) {
        // block 是一个指针，指向某个块的起始地址
        void* block = pool->data + i * actual_block_size; 
        // 得到块起始地址处存储的内容 ,这一段就是把pool->free_list的值（开始为null）写入块起始地址
        *((void**)block) = pool->free_list; // 此时block0存储null
        pool->free_list = block;    // free_list = block0
    }
    // printf("  mempool created: block_size=%zu, total_blocks=%zu\n",  actual_block_size, num_blocks);
    return pool;
}
// 分配块
void* kvs_mempool_alloc(mempool_t *pool, size_t size) {
    if (!pool || !pool->free_list || pool->free_blocks == 0) {
        return NULL;
    }
#if ENABLE_LEVEL_MEMPOOL

    // 计算用户可用空间（块大小 - 指针头）
    size_t user_capacity = pool->block_size - sizeof(void *);
    
    // 如果请求大小超过块容量，直接用 malloc
    if (size > user_capacity) {
        // printf("  WARN: size %zu > block capacity %zu, using malloc\n", size, user_capacity);
        return kvs_malloc(size);
    }
#endif
    void* block = pool->free_list;
    pool->free_list = *((void**)block);
    // 检查请求大小
    if (size > pool->block_size - sizeof(void*)) {
        // printf("  ERROR: size %zu > block capacity %zu\n",  size, pool->block_size - sizeof(void*));
        return NULL;
    }
    pool->free_blocks--;

    // 返回用户可用的内存起始地址
    void* user_ptr = (char*)block + sizeof(void*);
    // printf("  alloc success: user_ptr=%p\n", user_ptr);
    
    return user_ptr;
}
// 释放块
void kvs_mempool_free(mempool_t *pool, void *ptr) {
    if (!pool || !ptr) {
        return;
    }

#if ENABLE_LEVEL_MEMPOOL
    // 检查指针是否在内存池范围内
    void* block_start = (void*)pool->data;
    void* block_end = (void*)(pool->data + pool->total_blocks * pool->block_size);
    void* block = (char*)ptr - sizeof(void*);
    // 如果指针在池范围内，用池释放
    if (block >= block_start && block < block_end) {
        size_t offset = (char*)block - (char*)block_start;
        if (offset % pool->block_size == 0) {
            *((void**)block) = pool->free_list;
            pool->free_list = block;
            pool->free_blocks++;
            return;
        }
    }
    // 否则用 free（来自 malloc）
    kvs_free(ptr);
#else
    // 回退8字节，得到要释放块的起始地址
    void* block = (char*)ptr - sizeof(void*);

    // 在待释放块的头写入当前链表头（第一个空闲块）
    *((void**)block) = pool->free_list;
    // 更新free_list头，指向新插入的块
    pool->free_list = block;
    pool->free_blocks++;
#endif
}
// 重置块
void kvs_mempool_reset(mempool_t *pool) {
    if (!pool) {
        return;
    }

    // 重新构建空闲链表
    pool->free_blocks = pool->total_blocks;
    pool->free_list = NULL;
    size_t i = 0;
    for (i = 0; i < pool->total_blocks; ++i) {
        void* block = pool->data + i * pool->block_size;
        *((void**)block) = pool->free_list;
        pool->free_list = block;
    }
}
// 销毁内存池
void kvs_mempool_destroy(mempool_t *pool) {
    if (pool) {
        free(pool);
    }
}

int init_mempools(void) {
    // 数组：1024个item，2048个字符串（key+value）
    array_item_pool = kvs_mempool_create(sizeof(kvs_array_item_t), KVS_POOL_NODE_COUNT);
    array_string_pool = kvs_mempool_create(256, KVS_POOL_STRING_COUNT);
    if (!array_item_pool || !array_string_pool){
        // printf("Failed to create array_item_pool\n");
        return -1;
    }
    
    // 红黑树：1024个节点，2048个字符串
    rbtree_node_pool = kvs_mempool_create(sizeof(rbtree_node), KVS_POOL_NODE_COUNT);
    rbtree_string_pool = kvs_mempool_create(256, KVS_POOL_STRING_COUNT);
    if (!rbtree_node_pool || !rbtree_string_pool){
        // printf("Failed to create rbtree_node_pool\n");
        return -1;
    }
    
    // 哈希：1024个节点，2048个字符串
    hash_node_pool = kvs_mempool_create(sizeof(hashnode_t), KVS_POOL_NODE_COUNT);
    hash_string_pool = kvs_mempool_create(256, KVS_POOL_STRING_COUNT);
    if (!hash_node_pool || !hash_string_pool){
        // printf("Failed to create hash_node_pool\n");
        return -1;
    }
    
    // 跳表：1024个节点（包含forward数组），2048个字符串
    skiptable_node_pool = kvs_mempool_create(
        sizeof(kvs_node_t) + (MAX_LEVEL + 1) * sizeof(kvs_node_t*),
        KVS_POOL_NODE_COUNT
    );
    skiptable_string_pool = kvs_mempool_create(256, KVS_POOL_STRING_COUNT);
    if (!skiptable_node_pool || !skiptable_string_pool){
        // printf("Failed to create skiptable_node_pool\n");
        return -1;
    }
    // 创建响应缓冲池
    resp_pool = kvs_mempool_create(RESP_POOL_BLOCK_SIZE, RESP_POOL_BLOCK_COUNT);
    if (!resp_pool) {
        LOG_INFO("Failed to create response pool\n");
        return -1;
    }
    
    return 0;
}

// 销毁内存池
void dest_mempools(void) {
    if (array_item_pool) kvs_mempool_destroy(array_item_pool);
    if (array_string_pool) kvs_mempool_destroy(array_string_pool);
    if (rbtree_node_pool) kvs_mempool_destroy(rbtree_node_pool);
    if (rbtree_string_pool) kvs_mempool_destroy(rbtree_string_pool);
    if (hash_node_pool) kvs_mempool_destroy(hash_node_pool);
    if (hash_string_pool) kvs_mempool_destroy(hash_string_pool);
    if (skiptable_node_pool) kvs_mempool_destroy(skiptable_node_pool);
    if (skiptable_string_pool) kvs_mempool_destroy(skiptable_string_pool);
    if (resp_pool) kvs_mempool_destroy(resp_pool);
}
#endif
