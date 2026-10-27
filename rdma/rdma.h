
#ifndef __RDMA_TRANSMIT_H__
#define __RDMA_TRANSMIT_H__

#define RDMA_SERVER_PORT    9000
#define RDMA_SERVER_IP      "192.168.137.134"
#define RDMA_CLIENT_PORT    9001
#define RDMA_CLIENT_IP      "192.168.137.137"


#define RDMA_TIMEOUT_MS     2000
#define RDMA_CQ_DEPTH       4096
#define RDMA_BUFFER_SIZE    1024 * 1024

#define RDMA_FILENAME       "a.jpg"

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

    rdma_create_qp(id, *pd, &attr);

}

static int rdma_destroy_queuepair(struct rdma_cm_id *id, struct ibv_pd *pd, struct ibv_cq *cq) {

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

static int rdma_post_send(struct rdma_cm_id *cm_id) {
    conn_manager_t *cm = (conn_manager_t *) cm_id->context;

    struct ibv_sge sge;
    memset(&sge, 0, sizeof(struct ibv_sge));
    sge.addr = (uintptr_t)cm->sbuffer;
    sge.length = RDMA_BUFFER_SIZE;
    sge.lkey = cm->send_mr->lkey;

    struct ibv_send_wr wr, *bad = NULL;
    memset(&wr, 0, sizeof(struct ibv_send_wr));
    wr.wr_id = (uintptr_t)cm;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;

    return ibv_post_send(cm_id->qp, &wr, &bad);
}
// push 一次等待结果返回
static int rdma_poll_wc(struct ibv_cq *cq, struct ibv_wc *out) {

    while (1) {
        struct ibv_wc wc;
        int n = ibv_poll_cq(cq, 1, &wc);
        if (n < 0) return -1;
        else if (n == 0) {
            usleep(0);
            continue;
        }

        if (wc.status != IBV_WC_SUCCESS) {
            perror("ibv_poll_cq\n");
            return -1;
        }
        
        if(out) *out = wc;
        return 0;
    }

}

static int open_file(char *filename) {

}


#endif


