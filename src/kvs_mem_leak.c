#include "kvstore.h"

#if ENABLE_LEAK

#include <stdio.h>
#include <stdlib.h>

typedef struct leak_rec {
    struct leak_rec *next;
    size_t size;
    const char *file;
    int line;
} leak_rec_t;

static leak_rec_t *g_head = NULL;   // 活跃分配登记表
static size_t g_live = 0;
static int g_leak_on = 0;           // 运行时开关，由配置 leak.enable 控制，默认关

void kvs_leak_set_enable(int on) { g_leak_on = on; }
int  kvs_leak_enabled(void)     { return g_leak_on; }

void kvs_leak_init(void) {
    atexit(kvs_leak_report);
}

void *kvs_leak_malloc(size_t size, const char *file, int line) {
    if (!g_leak_on) return malloc(size);    // 开关关闭：直通，不登记
    leak_rec_t *rec = (leak_rec_t*)malloc(sizeof(leak_rec_t) + size);
    if (!rec) return NULL;
    rec->size = size;
    rec->file = file;
    rec->line = line;
    rec->next = g_head;     // 头插登记，O(1)
    g_head = rec;
    g_live++;
    return (char*)rec + sizeof(leak_rec_t);
}

void kvs_leak_free(void *ptr, const char *file, int line) {
    (void)file; (void)line;
    if (!ptr) return;
    if (!g_leak_on) { free(ptr); return; }  // 开关关闭：直通释放
    leak_rec_t **pp = &g_head;
    while (*pp && *pp != (leak_rec_t*)ptr - 1) pp = &(*pp)->next;
    if (!*pp) return;                       // 非追踪内存（如内存池块），直接忽略
    *pp = (*pp)->next;                      // 摘除登记
    g_live--;
    free((leak_rec_t*)ptr - 1);
}

// 程序结束时簿上剩下的就是疑似泄漏
void kvs_leak_report(void) {
    if (!g_live) { LOG_INFO("[LEAK] clean, no leak\n"); return; }
    LOG_INFO("[LEAK] %zu suspected leak(s):\n", g_live);
    leak_rec_t *r;
    for (r = g_head; r; r = r->next) {
        LOG_INFO("[LEAK] %zu bytes at %s:%d\n", r->size, r->file, r->line);
    }
}

#endif