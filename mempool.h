#ifndef MEMPOOL_H
#define MEMPOOL_H

#include <stddef.h>



// 内存池结构
typedef struct mempool_s{
    void *memory;       // 大块内存
    size_t total_size;  // 总大小
    size_t used_size;   // 已使用大小

    struct mempool_s *next; // 笑一个内存池 （链表）
} mempool_t;



#endif