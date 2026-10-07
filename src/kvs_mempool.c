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

// 字符串池 size-class 配置
static const size_t g_string_sizes[]  = KVS_STRING_CLASS_SIZES;
static const size_t g_string_counts[] = KVS_STRING_CLASS_COUNTS;

// 首批批量：上限的 1/64，小池直接全量，降低启动预付
static size_t _initial_blocks(size_t max_blocks) {
    size_t n = max_blocks / 64;
    if (n < 16) n = max_blocks;
    if (n > max_blocks) n = max_blocks;
    return n;
}

// 把一段 chunk 的所有块串入空闲链
static void _link_chunk(mempool_t *pool, mempool_chunk_t *chunk) {
    char *base = (char*)(chunk + 1);
    size_t i;
    for (i = 0; i < chunk->blocks; i++) {
        mempool_block_hdr_t *hdr = (mempool_block_hdr_t*)(base + i * pool->block_size);
        hdr->next = pool->free_list;
        pool->free_list = hdr;
    }
}

// 扩容：批量翻倍，直到容量上限
static int _mempool_grow(mempool_t *pool) {
    // 已分配总量（扩容翻倍时段数只有几段，遍历代价可忽略）
    size_t total = 0;
    mempool_chunk_t *c;
    for (c = pool->chunks; c; c = c->next) total += c->blocks;
    if (total >= pool->max_blocks) return -1;

    // 批量 = 最近一段的 +25%（过预留率上限 25%，远低于翻倍策略的 100%）
    size_t batch = pool->chunks ? pool->chunks->blocks + pool->chunks->blocks / 4 : _initial_blocks(pool->max_blocks);
    if (batch < 16) batch = 16;             // 小池起步阶段保证有效增长
    if (batch > pool->max_blocks - total)  batch = pool->max_blocks - total;

    mempool_chunk_t *chunk = (mempool_chunk_t*)malloc(
        sizeof(mempool_chunk_t) + batch * pool->block_size);
    if (!chunk) return -1;
    chunk->blocks = batch;
    chunk->next = pool->chunks;
    pool->chunks = chunk;
    _link_chunk(pool, chunk);
    return 0;
}

// 指定块大小和块数量（max_blocks 为容量上限，首批小批量、按需翻倍增长）
mempool_t* kvs_mempool_create(size_t user_block_size, size_t max_blocks) {
    if (user_block_size == 0 || max_blocks == 0) {
        return NULL;
    }
    // 块大小 = 块头 + 用户空间，8 字节对齐
    size_t block_size = (user_block_size + KVS_MEMPOOL_HDR + 7) & ~(size_t)7;

    mempool_t* pool = (mempool_t*)malloc(sizeof(mempool_t));
    if (!pool) return NULL;

    pool->block_size = block_size;
    pool->max_blocks = max_blocks;
    pool->free_list = NULL;

    pool->chunks = NULL;
    pool->sub_pools = NULL;

    if (_mempool_grow(pool) != 0) {   // 首批段
        free(pool);
        return NULL;
    }
    return pool;
}

// 创建 size-class 组池外壳：alloc 按 size 路由到最小可容纳的子池
mempool_t* kvs_mempool_create_group(const size_t *sizes, const size_t *counts, int nclasses) {
    if (!sizes || !counts || nclasses <= 0) return NULL;

    mempool_t *pool = (mempool_t*)calloc(1, sizeof(mempool_t));
    if (!pool) return NULL;

    pool->sub_pools = (mempool_t**)calloc(nclasses + 1, sizeof(mempool_t*));
    if (!pool->sub_pools) {
        free(pool);
        return NULL;
    }


    int i;
    for (i = 0; i < nclasses; i++) {
        pool->sub_pools[i] = kvs_mempool_create(sizes[i], counts[i]);
        if (!pool->sub_pools[i]) goto fail;
    }

    return pool;

fail:
    {
        int j;
        for (j = 0; j < nclasses; j++) {
            if (pool->sub_pools[j]) kvs_mempool_destroy(pool->sub_pools[j]);
        }
    }
    free(pool->sub_pools);
    free(pool);
    return NULL;
}

// 从单个池分配（内部）
static void* _pool_alloc(mempool_t *pool, size_t size) {
    size_t user_capacity = pool->block_size - KVS_MEMPOOL_HDR;
    if (size > user_capacity) {
        return kvs_malloc(size);    // 超过块容量，溢出降级
    }

    if (!pool->free_list && _mempool_grow(pool) != 0) {
        return kvs_malloc(size);    // 达到容量上限，溢出降级
    }

    mempool_block_hdr_t *hdr = (mempool_block_hdr_t*)pool->free_list;
    pool->free_list = hdr->next;
    hdr->next = (void*)((uintptr_t)hdr->next | KVS_MEMPOOL_INUSE);  // 置 in-use 标记
    return (char*)hdr + KVS_MEMPOOL_HDR;
}

// 分配块
void* kvs_mempool_alloc(mempool_t *pool, size_t size) {
    if (!pool) return kvs_malloc(size);

    if (pool->sub_pools) {              // size-class 组池：按大小路由
        int i;
        mempool_t *sub = NULL;
        for (i = 0; pool->sub_pools[i]; i++) {
            if (size <= pool->sub_pools[i]->block_size - KVS_MEMPOOL_HDR) {
                sub = pool->sub_pools[i];
                break;
            }
        }
        if (!sub) return kvs_malloc(size);  // 超过最大 class，溢出降级
        pool = sub;
    }
    return _pool_alloc(pool, size);
}
// 在 pool 的段链中定位 hdr 所在段（含块对齐校验）；不在则返回 NULL
static mempool_chunk_t* _chunk_of(mempool_t *pool, mempool_block_hdr_t *hdr) {
    mempool_chunk_t *c;
    for (c = pool->chunks; c; c = c->next) {
        char *base = (char*)(c + 1);
        char *end = base + c->blocks * pool->block_size;
        if ((char*)hdr >= base && (char*)hdr < end
            && ((char*)hdr - base) % pool->block_size == 0)
            return c;
    }
    return NULL;
}

// 释放块：段范围+对齐校验归属；malloc 降级块走 kvs_free；bit0 检测 double-free
void kvs_mempool_free(mempool_t *pool, void *ptr) {
    if (!ptr) return;
    mempool_block_hdr_t *hdr = (mempool_block_hdr_t*)((char*)ptr - KVS_MEMPOOL_HDR);

    mempool_t *owner = NULL;
    if (pool) {
        if (pool->sub_pools) {              // 组池：在各子池中定位归属
            int i;
            for (i = 0; pool->sub_pools[i] && !owner; i++) {
                if (_chunk_of(pool->sub_pools[i], hdr)) owner = pool->sub_pools[i];
            }
        } else if (_chunk_of(pool, hdr)) {
            owner = pool;
        }
    }
    if (!owner) {                           // 非池块：malloc 降级块        
        kvs_free(ptr);
        return;
    }

    if (!((uintptr_t)hdr->next & KVS_MEMPOOL_INUSE)) {   // double-free 检测
        LOG_INFO("mempool: double-free detected ptr=%p\n", ptr);
        return;
    }

    hdr->next = owner->free_list;
    owner->free_list = hdr;
}

// 重置池：组池递归重置各子池
void kvs_mempool_reset(mempool_t *pool) {
    if (!pool) {
        return;
    }

    if (pool->sub_pools) {
        int i;
        for (i = 0; pool->sub_pools[i]; i++) {
            kvs_mempool_reset(pool->sub_pools[i]);
        }
        return;
    }

    // 重新串联所有已分配段的块
    pool->free_list = NULL;
    mempool_chunk_t *c;
    for (c = pool->chunks; c; c = c->next) {
        _link_chunk(pool, c);
    }
}

// 销毁内存池：释放所有段与结构体，组池递归销毁子池
void kvs_mempool_destroy(mempool_t *pool) {
    if (!pool) return;

    if (pool->sub_pools) {
        int i;
        for (i = 0; pool->sub_pools[i]; i++) {
            kvs_mempool_destroy(pool->sub_pools[i]);
        }
        free(pool->sub_pools);
        free(pool);
        return;
    }

    mempool_chunk_t *c = pool->chunks;
    while (c) {
        mempool_chunk_t *next = c->next;
        free(c);
        c = next;
    }
    free(pool);
}

int init_mempools(void) {
    // 数组：1024个item，2048个字符串（key+value）
    array_item_pool = kvs_mempool_create(sizeof(kvs_array_item_t), KVS_POOL_NODE_COUNT);
    array_string_pool = kvs_mempool_create_group(g_string_sizes, g_string_counts, KVS_STRING_CLASSES);
    if (!array_item_pool || !array_string_pool){
        // printf("Failed to create array_item_pool\n");
        return -1;
    }
    
    // 红黑树：1024个节点，2048个字符串
    rbtree_node_pool = kvs_mempool_create(sizeof(rbtree_node), KVS_POOL_NODE_COUNT);
    rbtree_string_pool = kvs_mempool_create_group(g_string_sizes, g_string_counts, KVS_STRING_CLASSES);
    if (!rbtree_node_pool || !rbtree_string_pool){
        // printf("Failed to create rbtree_node_pool\n");
        return -1;
    }
    
    // 哈希：1024个节点，2048个字符串
    hash_node_pool = kvs_mempool_create(sizeof(hashnode_t), KVS_POOL_NODE_COUNT);
    hash_string_pool = kvs_mempool_create_group(g_string_sizes, g_string_counts, KVS_STRING_CLASSES);
    if (!hash_node_pool || !hash_string_pool){
        // printf("Failed to create hash_node_pool\n");
        return -1;
    }
    
    // 跳表：1024个节点（包含forward数组），2048个字符串
    skiptable_node_pool = kvs_mempool_create(
        sizeof(kvs_node_t) + (MAX_LEVEL + 1) * sizeof(kvs_node_t*),
        KVS_POOL_NODE_COUNT
    );
    skiptable_string_pool = kvs_mempool_create_group(g_string_sizes, g_string_counts, KVS_STRING_CLASSES);
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
