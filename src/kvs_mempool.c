#include "kvstore.h"
#include <sys/mman.h>



#if ENABLE_MEMPOOL

// 页向上对齐
static size_t _page_align(size_t n) {
    size_t ps = (size_t)sysconf(_SC_PAGESIZE);
    return (n + ps - 1) & ~(ps - 1);
}
// 响应缓冲池
mempool_t *resp_pool = NULL;

// 定义内存池
 mempool_t *hash_node_pool = NULL;      // 哈希节点池
 mempool_t *hash_string_pool = NULL;    // 哈希字符串池

 mempool_t *skiptable_node_pool = NULL; // 跳表节点池
 mempool_t *skiptable_string_pool = NULL; // 跳表字符串池

// 字符串池 size-class 配置
static const size_t g_string_sizes[]  = KVS_STRING_CLASS_SIZES;
static const size_t g_string_counts[] = KVS_STRING_CLASS_COUNTS;

// 跳表节点池 size-class 配置
static const size_t g_skipnode_sizes[]  = KVS_SKIPNODE_CLASS_SIZES;
static const size_t g_skipnode_counts[] = KVS_SKIPNODE_CLASS_COUNTS;

// 首批批量：上限的 1/64，小池直接全量，降低启动预付
static size_t _initial_blocks(size_t max_blocks) {
    size_t n = max_blocks / 64;
    if (n < 16) n = max_blocks;
    if (n > max_blocks) n = max_blocks;
    return n;
}

// 把一段 chunk 的所有块串入"本段自己的"空闲链（方案B：空闲块按段聚居）
static void _link_chunk(mempool_t *pool, mempool_chunk_t *chunk) {
    char *base = (char*)(chunk + 1);
    size_t i;
    chunk->free_list = NULL;
    for (i = 0; i < chunk->blocks; i++) {
        mempool_block_hdr_t *hdr = (mempool_block_hdr_t*)(base + i * pool->block_size);
        hdr->next = chunk->free_list;
        chunk->free_list = hdr;
    }
    chunk->free_cnt = chunk->blocks;
}

// 扩容：批量 = 最近一段的 +25%，直到容量上限
static int _mempool_grow(mempool_t *pool) {
    size_t total = 0;
    mempool_chunk_t *c;
    for (c = pool->chunks; c; c = c->next) total += c->blocks;
    if (total >= pool->max_blocks) return -1;

    size_t batch = pool->chunks ? pool->chunks->blocks + pool->chunks->blocks / 4 : _initial_blocks(pool->max_blocks);
    if (batch < 16) batch = 16;
    if (batch > pool->max_blocks - total)  batch = pool->max_blocks - total;

    mempool_chunk_t *chunk = NULL;

    // 1) 优先复用 trim 囤起来的空段（虚拟地址已在手，免 mmap，物理页首次触碰时自动缺页调入）
    if (pool->spare) {
        chunk = pool->spare;
        pool->spare = chunk->next;
        if (chunk->blocks != batch) {
            // 备用段大小不匹配就按备用段实际容量用（宁可多用不可截断）
            batch = chunk->blocks;
            if (total + batch > pool->max_blocks) {
                // 超上限则放回，走新分配
                chunk->next = pool->spare;
                pool->spare = chunk;
                chunk = NULL;
            } else {
                batch = chunk->blocks;
            }
        }
    }

    // 2) 没有备用段：mmap 新段（页对齐，为 madvise/munmap 做准备）
    if (!chunk) {
        size_t len = _page_align(sizeof(mempool_chunk_t) + batch * pool->block_size);
        chunk = (mempool_chunk_t *)mmap(NULL, len, PROT_READ | PROT_WRITE,
                                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (chunk == MAP_FAILED) {
            pool->fallback_alloc_cnt++;
            return -1;
        }
        chunk->blocks = batch;
    }

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

    pool->chunks = NULL;
    pool->spare = NULL;      // ← 加这行
    pool->sub_pools = NULL;
    pool->fallback_alloc_cnt = 0;    // ← 加这行
    pool->double_free_cnt = 0;       // ← 加这行

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

// 从单个池分配（内部）：找第一个有空闲块的段；全满则扩容
static void* _pool_alloc(mempool_t *pool, size_t size) {
    size_t user_capacity = pool->block_size - KVS_MEMPOOL_HDR;
    if (size > user_capacity) {
        pool->fallback_alloc_cnt++;
        return kvs_malloc(size);    // 超过块容量，溢出降级
    }

    // 找第一个有空闲块的段（段数只有几个到几十个，遍历可忽略）
    mempool_chunk_t *c;
    for (c = pool->chunks; c; c = c->next) {
        if (c->free_list) break;
    }
    if (!c) {
        if (_mempool_grow(pool) != 0) {
            pool->fallback_alloc_cnt++;
            return kvs_malloc(size);    // 达到容量上限，溢出降级
        }
        c = pool->chunks;               // 扩容成功：新段挂在段链表头
    }

    mempool_block_hdr_t *hdr = c->free_list;
    c->free_list = hdr->next;
    c->free_cnt--;
    hdr->next = (void*)((uintptr_t)hdr->next | KVS_MEMPOOL_INUSE);
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
        if (!sub) {
            pool->fallback_alloc_cnt++; // 统计：超过最大 class
            return kvs_malloc(size);  // 超过最大 class，溢出降级
        }
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
    mempool_chunk_t *oc = NULL;         // 块所属段
    if (pool) {
        if (pool->sub_pools) {
            int i;
            for (i = 0; pool->sub_pools[i] && !owner; i++) {
                oc = _chunk_of(pool->sub_pools[i], hdr);
                if (oc) owner = pool->sub_pools[i];
            }
        } else {
            oc = _chunk_of(pool, hdr);
            if (oc) owner = pool;
        }
    }
    if (!owner) {                       // 非池块：malloc 降级块
        kvs_free(ptr);
        return;
    }

    if (!((uintptr_t)hdr->next & KVS_MEMPOOL_INUSE)) {
        owner->double_free_cnt++;
        LOG_INFO("mempool: double-free detected ptr=%p\n", ptr);
        return;
    }

    hdr->next = oc->free_list;          // 归还到"本段自己的"空闲链
    oc->free_list = hdr;
    oc->free_cnt++;
}

// 重置池：组池递归重置各子池
void kvs_mempool_reset(mempool_t *pool) {
    if (!pool) return;

    if (pool->sub_pools) {
        int i;
        for (i = 0; pool->sub_pools[i]; i++) {
            kvs_mempool_reset(pool->sub_pools[i]);
        }
        return;
    }

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

    // 活跃段
    while (pool->chunks) {
        mempool_chunk_t *c = pool->chunks;
        pool->chunks = c->next;
        munmap(c, _page_align(sizeof(mempool_chunk_t) + c->blocks * pool->block_size));
    }
    // 备用段（trim 囤的）
    while (pool->spare) {
        mempool_chunk_t *c = pool->spare;
        pool->spare = c->next;
        munmap(c, _page_align(sizeof(mempool_chunk_t) + c->blocks * pool->block_size));
    }
    free(pool);
}

int init_mempools(void) {
    // 哈希：1024个节点，2048个字符串
    hash_node_pool = kvs_mempool_create(sizeof(hashnode_t), KVS_POOL_NODE_COUNT);
    hash_string_pool = kvs_mempool_create_group(g_string_sizes, g_string_counts, KVS_STRING_CLASSES);
    if (!hash_node_pool || !hash_string_pool){
        // printf("Failed to create hash_node_pool\n");
        return -1;
    }
    
    // 跳表：1024个节点（包含forward数组），2048个字符串
    skiptable_node_pool = kvs_mempool_create_group(
        g_skipnode_sizes, g_skipnode_counts, KVS_SKIPNODE_CLASSES);
        
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
    if (hash_node_pool) kvs_mempool_destroy(hash_node_pool);
    if (hash_string_pool) kvs_mempool_destroy(hash_string_pool);
    if (skiptable_node_pool) kvs_mempool_destroy(skiptable_node_pool);
    if (skiptable_string_pool) kvs_mempool_destroy(skiptable_string_pool);
    if (resp_pool) kvs_mempool_destroy(resp_pool);
}


/* ==================== 内存池统计（只读，用于诊断内存占用） ==================== */

typedef struct {
    size_t chunks, blocks, free_blocks;
    size_t chunk_bytes, inuse_payload;
    size_t fallback, dbl;
} mempool_state_t;

static mempool_state_t mempool_state_s;

// 数空闲块总数：直接读各段的 free_cnt（方案B后空闲链在段内）
static size_t count_free(mempool_t *pool, int *corrupt) {
    size_t n = 0;
    mempool_chunk_t *c;
    for (c = pool->chunks; c; c = c->next) n += c->free_cnt;
    (void)corrupt;
    return n;
}

// 打印一个"普通池"（单一块大小）
static void print_one_pool(mempool_t *p, const char *name) {
    size_t blocks = 0, chunks = 0;
    mempool_chunk_t *c;
    for (c = p->chunks; c; c = c->next) { chunks++; blocks += c->blocks; }

    int corrupt = 0;
    size_t frees  = count_free(p, &corrupt);
    size_t inuse  = (blocks > frees) ? (blocks - frees) : 0;

    size_t chunk_bytes = blocks * p->block_size;                    // 已向系统申请的总量
    size_t payload     = inuse * (p->block_size - KVS_MEMPOOL_HDR); // 真正交到业务手里的
    size_t waste       = chunk_bytes - payload;                     // 块头+空闲块+内部碎片

    LOG_INFO("[MEMPOOL] %-20s bs=%4zu chunks=%4zu blocks=%8zu inuse=%8zu "
             "chunkKB=%8zu payloadKB=%8zu wasteKB=%8zu fallback=%zu dbl=%zu%s",
             name, p->block_size, chunks, blocks, inuse,
             chunk_bytes / 1024, payload / 1024, waste / 1024,
             p->fallback_alloc_cnt, p->double_free_cnt,
             corrupt ? "  [FREE-LIST-CORRUPT]" : "");

    mempool_state_s.chunks       += chunks;
    mempool_state_s.blocks       += blocks;
    mempool_state_s.free_blocks  += frees;
    mempool_state_s.chunk_bytes  += chunk_bytes;
    mempool_state_s.inuse_payload+= payload;
    mempool_state_s.fallback     += p->fallback_alloc_cnt;
    mempool_state_s.dbl          += p->double_free_cnt;
}

void kvs_mempool_stats(mempool_t *pool, const char *name) {
    if (!pool) {
        LOG_INFO("[MEMPOOL] %-20s <NULL>", name ? name : "?");
        return;
    }
    if (pool->sub_pools) {                  // 组池：逐个 size-class 打印
        char buf[64];
        int i;
        for (i = 0; pool->sub_pools[i]; i++) {
            snprintf(buf, sizeof(buf), "%s[%d]", name, i);
            print_one_pool(pool->sub_pools[i], buf);
        }
        return;
    }
    print_one_pool(pool, name);
}

void kvs_mempool_stats_all(void) {

#if ENABLE_LEAK
    if (!kvs_leak_enabled()) return;    // 诊断开关关闭：不打统计（leak.enable=false 时静默）
#endif
    memset(&mempool_state_s, 0, sizeof(mempool_state_s));
    LOG_INFO("[MEMPOOL] ============== 内存池统计 BEGIN ==============");

    kvs_mempool_stats(hash_node_pool,       "hash_node_pool");
    kvs_mempool_stats(hash_string_pool,     "hash_string_pool");
    kvs_mempool_stats(skiptable_node_pool,  "skiptable_node_pool");
    kvs_mempool_stats(skiptable_string_pool,"skiptable_string_pool");
    kvs_mempool_stats(resp_pool,            "resp_pool");

    size_t waste = mempool_state_s.chunk_bytes > mempool_state_s.inuse_payload
                 ? mempool_state_s.chunk_bytes - mempool_state_s.inuse_payload : 0;
    size_t pct   = mempool_state_s.chunk_bytes ? waste * 100 / mempool_state_s.chunk_bytes : 0;

    LOG_INFO("[MEMPOOL] TOTAL chunks=%zu blocks=%zu free=%zu "
             "chunkKB=%zu payloadKB=%zu wasteKB=%zu (%zu%%) fallback=%zu dbl=%zu",
             mempool_state_s.chunks, mempool_state_s.blocks, mempool_state_s.free_blocks,
             mempool_state_s.chunk_bytes / 1024, mempool_state_s.inuse_payload / 1024,
             waste / 1024, pct, mempool_state_s.fallback, mempool_state_s.dbl);
    LOG_INFO("[MEMPOOL] ============== 内存池统计 END ==============");
}


/* ==================== 内存回收：整段空闲的 chunk 释放回系统 ==================== */

// 单池 trim：释放所有整段空闲的 chunk，返回释放的字节数
static size_t _pool_trim(mempool_t *pool) {
    if (!pool || pool->sub_pools) return 0;

    size_t released_bytes = 0;          // 统计的是"退还物理页"的量
    mempool_chunk_t **pp = &pool->chunks;
    while (*pp) {
        mempool_chunk_t *c = *pp;
        if (c->free_cnt == c->blocks) { // 整段空闲
            *pp = c->next;
            // 退物理页：RSS 下降；虚拟地址保留（VmSize 不变），spare 链囤着复用
            size_t len = _page_align(sizeof(mempool_chunk_t) + c->blocks * pool->block_size);
            madvise(c, len, MADV_DONTNEED);
            c->next = pool->spare;      // 摘出活跃链，挂备用链（不 munmap！）
            pool->spare = c;
            released_bytes += c->blocks * pool->block_size;
        } else {
            pp = &c->next;
        }
    }
    return released_bytes;
}

static size_t _trim_group(mempool_t *pool) {
    size_t total = 0;
    if (pool && pool->sub_pools) {
        for (int i = 0; pool->sub_pools[i]; i++) total += _pool_trim(pool->sub_pools[i]);
    }
    return total;
}

// 组池递归 + 单池，对外入口
size_t kvs_mempool_trim_all(void) {
    size_t total = 0;
    if (hash_node_pool)        total += _pool_trim(hash_node_pool);
    if (hash_string_pool)      total += _trim_group(hash_string_pool);
    if (skiptable_node_pool)   total += _trim_group(skiptable_node_pool);
    if (skiptable_string_pool) total += _trim_group(skiptable_string_pool);
    if (resp_pool)             total += _pool_trim(resp_pool);
    LOG_INFO("[MEMPOOL] trim released %zu KB", total / 1024);
    return total;
}

#endif
