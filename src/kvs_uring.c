#define _GNU_SOURCE
#include "kvs_uring.h"

#include <errno.h>
#include <linux/io_uring.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_io_uring_setup
#error "This platform does not expose SYS_io_uring_setup"
#endif
#ifndef SYS_io_uring_enter
#error "This platform does not expose SYS_io_uring_enter"
#endif

#define KVS_URING_ENTRIES 256
#define KVS_URING_BATCH    64

/* ===== 内部数据结构 ===== */
// 表示一个待写入请求
// 生命周期：
// 每次append都会创建一个uring_req_t，插入ctx->head/tail链表，队列尾部
// 消费：worker_main取出后填入SQE如何提交给uring
typedef struct kvs_uring_req {
    char *data;
    size_t len;
    off_t offset;
    struct kvs_uring_req *next;
} kvs_uring_req_t;

// 封装uring的裸ring操作，包括：
// io_uring_setup返回的fd和params
// mmap的三块内存：SQ ring ，CQ ring，SQE数组
// SQ/CQ的head/tail/mask等指针
typedef struct {
    int fd;     // io_uring实例fd
    struct io_uring_params p;   // 内核返回的布局信息

    void *sq_ring; size_t sq_ring_sz;   // SQ ring mmap：SQ ring 的元数据（head/tail/mask/array）
    void *cq_ring; size_t cq_ring_sz;   // CQ ring mmap：CQ ring 的元数据（head/tail/mask/cqes）
    struct io_uring_sqe *sqes; size_t sqes_sz;  // SQE 数组 mmap：存放提交的 SQE

    // 些指针指向 mmap 区域里的具体字段，通过它们读写 SQ/CQ 的 head/tail。
    unsigned *sq_head, *sq_tail, *sq_mask, *sq_entries, *sq_array;
    unsigned *cq_head, *cq_tail, *cq_mask;
    struct io_uring_cqe *cqes;
} kvs_ring_t;


// 对外实例
struct kvs_uring {
    int fd;     // aof文件fd
    kvs_ring_t ring;    // io_uring ring

    // 工作线程 + 同步原语
    pthread_t worker;
    pthread_mutex_t mutex;  // 保护队列和计数
    pthread_cond_t cond;    // 通知 worker 有新数据
    pthread_cond_t flush_cond;  // 通知 flush 有完成

    // FIFO队列
    kvs_uring_req_t *head, *tail;
    size_t queued, inflight;    // 队列长度、在途数量

    // 计数器 
    uint64_t submitted, completed;  // 入队总数、完成总数
    off_t next_offset;  // 下次写入偏移
    // 状态标志
    int running, worker_started, fatal_error;
};

/* ===== ring_setup：初始化 io_uring ===== */
static int ring_setup(kvs_ring_t *r, unsigned entries)
{
    memset(r, 0, sizeof(*r));
    r->fd = -1;

    r->fd = (int)syscall(SYS_io_uring_setup, entries, &r->p);
    if (r->fd < 0) return -errno;

    r->sq_ring_sz = r->p.sq_off.array + r->p.sq_entries * sizeof(unsigned);
    r->cq_ring_sz = r->p.cq_off.cqes + r->p.cq_entries * sizeof(struct io_uring_cqe);
    int single = r->p.features & IORING_FEAT_SINGLE_MMAP;

    if (single) {
        size_t sz = r->sq_ring_sz > r->cq_ring_sz ? r->sq_ring_sz : r->cq_ring_sz;
        r->sq_ring = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, r->fd, IORING_OFF_SQ_RING);
        if (r->sq_ring == MAP_FAILED) { r->sq_ring = NULL; close(r->fd); r->fd = -1; return -errno; }
        r->cq_ring = r->sq_ring;
        r->sq_ring_sz = r->cq_ring_sz = sz;
    } else {
        r->sq_ring = mmap(NULL, r->sq_ring_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, r->fd, IORING_OFF_SQ_RING);
        if (r->sq_ring == MAP_FAILED) { r->sq_ring = NULL; close(r->fd); r->fd = -1; return -errno; }
        r->cq_ring = mmap(NULL, r->cq_ring_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, r->fd, IORING_OFF_CQ_RING);
        if (r->cq_ring == MAP_FAILED) {
            r->cq_ring = NULL;
            munmap(r->sq_ring, r->sq_ring_sz); r->sq_ring = NULL;
            close(r->fd); r->fd = -1;
            return -errno;
        }
    }

    r->sqes_sz = r->p.sq_entries * sizeof(struct io_uring_sqe);
    r->sqes = mmap(NULL, r->sqes_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, r->fd, IORING_OFF_SQES);
    if (r->sqes == MAP_FAILED) {
        r->sqes = NULL;
        munmap(r->sq_ring, r->sq_ring_sz);
        if (!single) munmap(r->cq_ring, r->cq_ring_sz);
        r->sq_ring = r->cq_ring = NULL;
        close(r->fd); r->fd = -1;
        return -errno;
    }

    r->sq_head    = (unsigned *)((char *)r->sq_ring + r->p.sq_off.head);
    r->sq_tail    = (unsigned *)((char *)r->sq_ring + r->p.sq_off.tail);
    r->sq_mask    = (unsigned *)((char *)r->sq_ring + r->p.sq_off.ring_mask);
    r->sq_entries = (unsigned *)((char *)r->sq_ring + r->p.sq_off.ring_entries);
    r->sq_array   = (unsigned *)((char *)r->sq_ring + r->p.sq_off.array);
    r->cq_head    = (unsigned *)((char *)r->cq_ring + r->p.cq_off.head);
    r->cq_tail    = (unsigned *)((char *)r->cq_ring + r->p.cq_off.tail);
    r->cq_mask    = (unsigned *)((char *)r->cq_ring + r->p.cq_off.ring_mask);
    r->cqes       = (struct io_uring_cqe *)((char *)r->cq_ring + r->p.cq_off.cqes);
    return 0;
}

/* ===== ring_destroy：清理 ring ===== */
static void ring_destroy(kvs_ring_t *r)
{
    if (r->sqes) { munmap(r->sqes, r->sqes_sz); r->sqes = NULL; }
    if (r->sq_ring) {
        int single = r->p.features & IORING_FEAT_SINGLE_MMAP;
        munmap(r->sq_ring, r->sq_ring_sz);
        if (!single && r->cq_ring) munmap(r->cq_ring, r->cq_ring_sz);
        r->sq_ring = r->cq_ring = NULL;
    }
    if (r->fd >= 0) { close(r->fd); r->fd = -1; }
}

/* ===== 取一个 SQE 槽位 ===== */
static struct io_uring_sqe *ring_get_sqe(kvs_ring_t *r)
{
    unsigned head = __atomic_load_n(r->sq_head, __ATOMIC_ACQUIRE);
    unsigned tail = __atomic_load_n(r->sq_tail, __ATOMIC_RELAXED);
    if (tail - head >= *r->sq_entries) return NULL;

    unsigned index = tail & *r->sq_mask;
    struct io_uring_sqe *sqe = &r->sqes[index];
    memset(sqe, 0, sizeof(*sqe));
    r->sq_array[index] = index;
    __atomic_store_n(r->sq_tail, tail + 1, __ATOMIC_RELEASE);
    return sqe;
}

/* ===== 提交 SQE 到内核 ===== */
static int ring_submit(kvs_ring_t *r)
{
    unsigned head = __atomic_load_n(r->sq_head, __ATOMIC_RELAXED);
    unsigned tail = __atomic_load_n(r->sq_tail, __ATOMIC_ACQUIRE);
    unsigned n = tail - head;
    if (n == 0) return 0;
    int ret = (int)syscall(SYS_io_uring_enter, r->fd, n, 0, 0, NULL, 0);
    return ret < 0 ? -errno : ret;
}

/* ===== 等一个 CQE ===== */
static int ring_wait_cqe(kvs_ring_t *r, struct io_uring_cqe **out)
{
    for (;;) {
        unsigned head = __atomic_load_n(r->cq_head, __ATOMIC_ACQUIRE);
        unsigned tail = __atomic_load_n(r->cq_tail, __ATOMIC_ACQUIRE);
        if (head != tail) { *out = &r->cqes[head & *r->cq_mask]; return 0; }

        int ret = (int)syscall(SYS_io_uring_enter, r->fd, 0, 1, IORING_ENTER_GETEVENTS, NULL, 0);
        if (ret < 0) { if (errno == EINTR) continue; return -errno; }
    }
}

/* ===== 标记 CQE 已处理 ===== */
static void ring_cqe_seen(kvs_ring_t *r)
{
    unsigned head = __atomic_load_n(r->cq_head, __ATOMIC_RELAXED);
    __atomic_store_n(r->cq_head, head + 1, __ATOMIC_RELEASE);
}

/* ===== 释放一个 req ===== */
static void free_req(kvs_uring_req_t *req)
{
    if (req) { free(req->data); free(req); }
}

/* ===== 设置致命错误 ===== */
static void set_fatal(struct kvs_uring *ctx, int err)
{
    pthread_mutex_lock(&ctx->mutex);
    if (ctx->fatal_error == 0) ctx->fatal_error = err > 0 ? err : -err;
    pthread_cond_broadcast(&ctx->flush_cond);
    pthread_mutex_unlock(&ctx->mutex);
}

/* ===== 批量提交请求到 io_uring ===== */
static int submit_requests(struct kvs_uring *ctx, kvs_uring_req_t **reqs, size_t count)
{
    size_t n = 0;
    for (; n < count; ++n) {
        struct io_uring_sqe *sqe = ring_get_sqe(&ctx->ring);
        if (!sqe) break;

        kvs_uring_req_t *req = reqs[n];
        sqe->opcode = IORING_OP_WRITE;
        sqe->fd = ctx->fd;
        sqe->off = (uint64_t)req->offset;
        sqe->addr = (uint64_t)(uintptr_t)req->data;
        sqe->len = (unsigned)req->len;
        sqe->user_data = (uint64_t)(uintptr_t)req;
    }

    if (n == 0) return -EAGAIN;

    int ret = ring_submit(&ctx->ring);
    if (ret < 0) return ret;

    pthread_mutex_lock(&ctx->mutex);
    ctx->inflight += n;
    pthread_mutex_unlock(&ctx->mutex);

    return (int)n;
}

/* ===== 收割一个 CQE ===== */
static int reap_one(struct kvs_uring *ctx)
{
    struct io_uring_cqe *cqe = NULL;
    int ret = ring_wait_cqe(&ctx->ring, &cqe);
    if (ret < 0) return ret;

    kvs_uring_req_t *req = (kvs_uring_req_t *)(uintptr_t)cqe->user_data;
    int res = cqe->res;
    size_t expected = req->len;

    ring_cqe_seen(&ctx->ring);
    free_req(req);

    pthread_mutex_lock(&ctx->mutex);
    if (ctx->inflight > 0) ctx->inflight--;
    ctx->completed++;

    if (res < 0) { if (ctx->fatal_error == 0) ctx->fatal_error = -res; }
    else if ((size_t)res != expected) { if (ctx->fatal_error == 0) ctx->fatal_error = EIO; }

    pthread_cond_broadcast(&ctx->flush_cond);
    pthread_mutex_unlock(&ctx->mutex);

    return res < 0 ? res : ((size_t)res == expected ? 0 : -EIO);
}

/* ===== worker 线程主循环 ===== */
static void *worker_main(void *arg)
{
    struct kvs_uring *ctx = (struct kvs_uring *)arg;

    for (;;) {
        kvs_uring_req_t *batch[KVS_URING_BATCH];
        size_t count = 0;

        pthread_mutex_lock(&ctx->mutex);
        while (ctx->running && ctx->head == NULL)
            pthread_cond_wait(&ctx->cond, &ctx->mutex);

        if (!ctx->running && ctx->head == NULL && ctx->inflight == 0) {
            pthread_mutex_unlock(&ctx->mutex);
            break;
        }

        while (ctx->head && count < KVS_URING_BATCH) {
            kvs_uring_req_t *req = ctx->head;
            ctx->head = req->next;
            req->next = NULL;
            if (!ctx->head) ctx->tail = NULL;
            if (ctx->queued > 0) ctx->queued--;
            batch[count++] = req;
        }
        pthread_mutex_unlock(&ctx->mutex);

        size_t pos = 0;
        while (pos < count) {
            int n = submit_requests(ctx, &batch[pos], count - pos);
            if (n < 0) {
                set_fatal(ctx, -n);
                for (size_t i = pos; i < count; ++i) {
                    free_req(batch[i]);
                    pthread_mutex_lock(&ctx->mutex);
                    ctx->completed++;
                    pthread_cond_broadcast(&ctx->flush_cond);
                    pthread_mutex_unlock(&ctx->mutex);
                }
                break;
            }

            pos += (size_t)n;

            for (int i = 0; i < n; ++i) {
                int rc = reap_one(ctx);
                if (rc < 0) { set_fatal(ctx, -rc); break; }
            }

            pthread_mutex_lock(&ctx->mutex);
            int fatal = ctx->fatal_error;
            pthread_mutex_unlock(&ctx->mutex);
            if (fatal) break;
        }

        pthread_mutex_lock(&ctx->mutex);
        pthread_cond_broadcast(&ctx->flush_cond);
        pthread_mutex_unlock(&ctx->mutex);
    }

    return NULL;
}

/* ===== 对外接口：初始化 ===== */
int kvs_uring_init(kvs_uring_t **out, int fd, off_t initial_offset)
{
    if (!out || fd < 0 || initial_offset < 0) return -EINVAL;
    *out = NULL;

    struct kvs_uring *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return -ENOMEM;

    // 初始化字段
    ctx->fd = fd;
    ctx->next_offset = initial_offset;
    ctx->running = 1;   // 线程运行标志
    int ret = -ENOMEM;

    if (pthread_mutex_init(&ctx->mutex, NULL) != 0) goto fail_ctx;
    if (pthread_cond_init(&ctx->cond, NULL) != 0) goto fail_mutex;
    if (pthread_cond_init(&ctx->flush_cond, NULL) != 0) goto fail_cond;

    // 初始化ring
    // 创建 io_uring 实例/ mmap 三块内存（SQ ring、CQ ring、SQE 数组）/ 设置 sq_head / sq_tail 等指针
    ret = ring_setup(&ctx->ring, KVS_URING_ENTRIES);
    if (ret < 0) goto fail_cond2;
    // 启动 worker 线程
    if (pthread_create(&ctx->worker, NULL, worker_main, ctx) != 0) {
        ret = -errno;
        ring_destroy(&ctx->ring);
        goto fail_cond2;
    }

    ctx->worker_started = 1;
    *out = ctx;
    return 0;

fail_cond2: pthread_cond_destroy(&ctx->flush_cond);
fail_cond:  pthread_cond_destroy(&ctx->cond);
fail_mutex: pthread_mutex_destroy(&ctx->mutex);
fail_ctx:   free(ctx);
    return ret;
}

/* ===== 对外接口：追加数据 ===== */
// 业务线程调用，构造请求入队，立刻返回。
int kvs_uring_append(kvs_uring_t *ctx, const void *data, size_t len)
{
    if (!ctx || !data || len == 0) return -EINVAL;
    if (len > UINT_MAX) return -E2BIG;
    // 分配req
    kvs_uring_req_t *req = calloc(1, sizeof(*req));
    if (!req) return -ENOMEM;

    req->data = malloc(len);
    if (!req->data) { free(req); return -ENOMEM; }
    memcpy(req->data, data, len);   // 调用者的 data 可能是栈上临时 buffer，必须拷贝
    req->len = len;

    // 加锁检查状态
    pthread_mutex_lock(&ctx->mutex);
    if (!ctx->running || ctx->fatal_error != 0) {
        int err = ctx->fatal_error ? ctx->fatal_error : ESHUTDOWN;
        pthread_mutex_unlock(&ctx->mutex);
        free_req(req);
        return -err;
    }

    req->offset = ctx->next_offset;
    ctx->next_offset += (off_t)len;
    // 入队
    if (ctx->tail) ctx->tail->next = req;
    else ctx->head = req;
    ctx->tail = req;
    ctx->queued++;
    ctx->submitted++;
    // 唤醒worker
    pthread_cond_signal(&ctx->cond);
    pthread_mutex_unlock(&ctx->mutex);
    return 0;
}

/* ===== 对外接口：flush（等所有写完成） ===== */
int kvs_uring_flush(kvs_uring_t *ctx)
{
    if (!ctx) return -EINVAL;

    pthread_mutex_lock(&ctx->mutex);
    uint64_t target = ctx->submitted;

    while (ctx->completed < target) {
        if (ctx->fatal_error != 0) {
            int err = ctx->fatal_error;
            pthread_mutex_unlock(&ctx->mutex);
            return -err;
        }
        pthread_cond_wait(&ctx->flush_cond, &ctx->mutex);
    }

    int err = ctx->fatal_error;
    pthread_mutex_unlock(&ctx->mutex);
    return err ? -err : 0;
}

/* ===== 对外接口：reset（清空 AOF） ===== */
int kvs_uring_reset(kvs_uring_t *ctx)
{
    if (!ctx) return -EINVAL;

    int ret = kvs_uring_flush(ctx);
    if (ret < 0) return ret;

    if (ftruncate(ctx->fd, 0) < 0) return -errno;
    if (fsync(ctx->fd) < 0) return -errno;

    pthread_mutex_lock(&ctx->mutex);
    ctx->next_offset = 0;
    pthread_mutex_unlock(&ctx->mutex);
    return 0;
}

/* ===== 对外接口：关闭 ===== */
void kvs_uring_shutdown(kvs_uring_t *ctx)
{
    if (!ctx) return;

    pthread_mutex_lock(&ctx->mutex);
    ctx->running = 0;
    pthread_cond_signal(&ctx->cond);
    pthread_mutex_unlock(&ctx->mutex);

    if (ctx->worker_started) pthread_join(ctx->worker, NULL);

    for (kvs_uring_req_t *req = ctx->head, *next; req; req = next) {
        next = req->next;
        free_req(req);
    }

    ring_destroy(&ctx->ring);

    pthread_cond_destroy(&ctx->flush_cond);
    pthread_cond_destroy(&ctx->cond);
    pthread_mutex_destroy(&ctx->mutex);
    free(ctx);
}

