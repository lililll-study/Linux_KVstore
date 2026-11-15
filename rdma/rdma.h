
#ifndef __RDMA_TRANSMIT_H__
#define __RDMA_TRANSMIT_H__
#include <sched.h>

#define RDMA_SERVER_PORT    9000
#define RDMA_SERVER_IP      "192.168.137.139"   /* 默认值，可被命令行参数覆盖 */
#define RDMA_CLIENT_PORT    9001


#define RDMA_TIMEOUT_MS     2000
#define RDMA_CQ_DEPTH       32
#define RDMA_BUFFER_SIZE    (1024 * 1024 * 8)       /* 每块 8MB：1000MB = 125 块，兼顾流水与内存占用 */

#define RDMA_FILENAME       "bench_100m.bin"    /* 默认文件，可被命令行参数覆盖 */

/* 同步开关：1 = 停等协议（每块等对端 ACK，可靠但每块一个往返）
 *          0 = 连发模式（不等对端确认，测极限吞吐；对端没收缓冲时靠 RNR 重试兜底）*/
#define ENABLE_RDMA_ACK     1

typedef struct conn_manager_s {

    // struct ibv_qp *qp;

    struct ibv_mr *recv_mr;
    struct ibv_mr *send_mr;

    char *rbuffer;
    char *sbuffer;
} conn_manager_t;

typedef struct file_type_s {
    uint32_t magic; // 0x12345678
    size_t total;
} file_type_t;



static int rdma_wait_cm_event(struct rdma_event_channel *channel, enum rdma_cm_event_type expect, struct rdma_cm_event **out) {

    int ret = 0;
    struct rdma_cm_event *event;
    if (0 != rdma_get_cm_event(channel, &event)) {
        perror("rdma_get_cm_event\n");
        return -1;
    }

    if (event->event != expect) {   // 失败
        fprintf(stderr, "unexpect RDMA CM event %s, expected = %s\n", rdma_event_str(event->event), rdma_event_str(expect));
        ret = -1;
        goto finished;
    }

    if (out) {
        *out = event;
        return 0;
    }

 finished:
    rdma_ack_cm_event(event);
    return ret;
}


static int rdma_create_queuepair(struct rdma_cm_id *id, struct ibv_pd **pd, struct ibv_cq **cq) {
    *pd = ibv_alloc_pd(id->verbs);
    if (NULL == *pd) {
        perror("ibv_alloc_pd\n");
        return -1;
    }
    // 创建一个complete queue
    *cq = ibv_create_cq(id->verbs, RDMA_CQ_DEPTH, NULL, NULL, 0);
    if (NULL == *cq) {
        perror("ibv_create_cq\n");
        return -1;
    }

    struct ibv_qp_init_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.send_cq = *cq;
    attr.recv_cq = *cq;
    attr.qp_type = IBV_QPT_RC;
    attr.cap.max_send_wr = RDMA_CQ_DEPTH;
    attr.cap.max_recv_wr = RDMA_CQ_DEPTH;
    attr.cap.max_send_sge = 1;
    attr.cap.max_recv_sge = 1;

    int ret = rdma_create_qp(id, *pd, &attr);

    if (ret != 0) {
        fprintf(stderr, "rdma_create_qp failed: %d\n", ret);
        return -1;
    }

    return 0;

}

static inline int rdma_destroy_queuepair(struct rdma_cm_id *id, struct ibv_pd *pd, struct ibv_cq *cq) {

    rdma_destroy_qp(id);

    ibv_destroy_cq(cq);

    ibv_dealloc_pd(pd);

    return 0;
}

static int rdma_alloc_buffer(struct rdma_cm_id *cm_id, struct ibv_pd *pd) {

    conn_manager_t *cm = (conn_manager_t *)calloc(1, sizeof(conn_manager_t));
    if (!cm) return -1;

    cm->rbuffer = (char *)calloc(1, RDMA_BUFFER_SIZE);
    cm->sbuffer = (char *)calloc(1, RDMA_BUFFER_SIZE);
    if (!cm->rbuffer || !cm->sbuffer) return -1;

    cm->send_mr = ibv_reg_mr(pd, cm->sbuffer, RDMA_BUFFER_SIZE, 0);
    cm->recv_mr = ibv_reg_mr(pd, cm->rbuffer, RDMA_BUFFER_SIZE, IBV_ACCESS_LOCAL_WRITE);
    if (!cm->send_mr || !cm->recv_mr) return -1;

    cm_id->context = cm;
    cm_id->context = cm;
    return 0;
}

static int rdma_free_buffer(struct rdma_cm_id *cm_id) {

    conn_manager_t *cm = cm_id->context;

    ibv_dereg_mr(cm->send_mr);
    ibv_dereg_mr(cm->recv_mr);

    free(cm->rbuffer);
    free(cm->sbuffer);

    return 0;
}


static int rdma_post_recv(struct rdma_cm_id *cm_id) {
    conn_manager_t *cm = (conn_manager_t *) cm_id->context;

    struct ibv_sge sge;
    memset(&sge, 0, sizeof(struct ibv_sge));
    sge.addr = (uintptr_t)cm->rbuffer;
    sge.length = RDMA_BUFFER_SIZE;
    sge.lkey = cm->recv_mr->lkey;

    struct ibv_recv_wr wr, *bad = NULL;
    memset(&wr, 0, sizeof(struct ibv_recv_wr));
    wr.wr_id = (uintptr_t)cm;
    wr.sg_list = &sge;
    wr.num_sge = 1;

    return ibv_post_recv(cm_id->qp, &wr, &bad);
}

static int rdma_post_send(struct rdma_cm_id *cm_id, size_t length) {
    conn_manager_t *cm = cm_id->context;

    if (length == 0 || length > RDMA_BUFFER_SIZE) {
        return -1;
    }

    struct ibv_sge sge = {0};
    sge.addr = (uintptr_t)cm->sbuffer;
    sge.length = (uint32_t)length;
    sge.lkey = cm->send_mr->lkey;

    struct ibv_send_wr wr = {0};
    struct ibv_send_wr *bad = NULL;

    wr.wr_id = (uintptr_t)cm;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;

    return ibv_post_send(cm_id->qp, &wr, &bad);
}

// 轮询 CQ，直到取到指定类型（opcode）的完成事件
// 停等协议下，本端 SEND 的完成和对端回包（READY/ACK）的 RECV 完成
// 落在同一个 CQ 里；遇到类型不匹配的完成事件（自己 SEND 的通知）跳过继续等
/* Preserve completions for the other direction when SEND and RECV share a CQ. */
static struct ibv_wc rdma_pending_wc[RDMA_CQ_DEPTH];
static unsigned rdma_pending_count;

static inline int rdma_next_wc(struct ibv_cq *cq, struct ibv_wc *out) {
    unsigned idle = 0;
    for (;;) {
        int n = ibv_poll_cq(cq, 1, out);
        if (n < 0) return -1;
        if (n == 0) {
            if (++idle == 1024) { sched_yield(); idle = 0; }
            continue;
        }
        if (out->status != IBV_WC_SUCCESS) {
            fprintf(stderr, "[RDMA] WC ERROR: %s opcode=%d wr_id=%llu\n",
                    ibv_wc_status_str(out->status), out->opcode,
                    (unsigned long long)out->wr_id);
            return -1;
        }
        return 0;
    }
}

static int rdma_poll_wc(struct ibv_cq *cq, struct ibv_wc *out, enum ibv_wc_opcode expect) {
    for (unsigned i = 0; i < rdma_pending_count; ++i) {
        if (rdma_pending_wc[i].opcode == expect) {
            if (out) *out = rdma_pending_wc[i];
            memmove(&rdma_pending_wc[i], &rdma_pending_wc[i + 1],
                    (rdma_pending_count - i - 1) * sizeof(rdma_pending_wc[0]));
            --rdma_pending_count;
            return 0;
        }
    }

    while (1) {
        struct ibv_wc wc;
        if (rdma_next_wc(cq, &wc) != 0) return -1;
        if (wc.opcode == expect) { if (out) *out = wc; return 0; }
        if (rdma_pending_count == RDMA_CQ_DEPTH) return -1;
        rdma_pending_wc[rdma_pending_count++] = wc;
    }
}

/* Benchmark pipeline slots; each slot owns its registered buffer. */
typedef struct {
    char *buffer;
    struct ibv_mr *mr;
    size_t capacity;
} rdma_slot_t;

static inline int rdma_slot_init(struct ibv_pd *pd, rdma_slot_t *slot, size_t capacity, int receive) {
    memset(slot, 0, sizeof(*slot));
    slot->buffer = malloc(capacity);
    if (!slot->buffer) return -1;
    slot->mr = ibv_reg_mr(pd, slot->buffer, capacity,
                           receive ? IBV_ACCESS_LOCAL_WRITE : 0);
    if (!slot->mr) { free(slot->buffer); slot->buffer = NULL; return -1; }
    slot->capacity = capacity;
    return 0;
}

static inline void rdma_slot_free(rdma_slot_t *slot) {
    if (slot->mr) ibv_dereg_mr(slot->mr);
    free(slot->buffer);
    memset(slot, 0, sizeof(*slot));
}

static inline int rdma_slot_recv(struct rdma_cm_id *id, rdma_slot_t *slot, uint64_t wr_id) {
    struct ibv_sge sge = {.addr = (uintptr_t)slot->buffer,
                          .length = (uint32_t)slot->capacity, .lkey = slot->mr->lkey};
    struct ibv_recv_wr wr = {.wr_id = wr_id, .sg_list = &sge, .num_sge = 1}, *bad = NULL;
    return ibv_post_recv(id->qp, &wr, &bad);
}

static inline int rdma_slot_send(struct rdma_cm_id *id, rdma_slot_t *slot,
                          size_t length, uint64_t wr_id) {
    struct ibv_sge sge = {.addr = (uintptr_t)slot->buffer,
                          .length = (uint32_t)length, .lkey = slot->mr->lkey};
    struct ibv_send_wr wr = {.wr_id = wr_id, .sg_list = &sge, .num_sge = 1,
                             .opcode = IBV_WR_SEND, .send_flags = IBV_SEND_SIGNALED}, *bad = NULL;
    return ibv_post_send(id->qp, &wr, &bad);
}


#endif


