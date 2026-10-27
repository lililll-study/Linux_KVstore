
#ifndef __RDMA_TRANSMIT_H__
#define __RDMA_TRANSMIT_H__

#define RDMA_SERVER_PORT    9000
#define RDMA_SERVER_IP      "192.168.137.134"
#define RDMA_CLIENT_PORT    9001
#define RDMA_CLIENT_IP      "192.168.137.137"


#define RDMA_TIMEOUT_MS     1000
#define RDMA_CQ_DEPTH     32


// static修饰的，说明在该文件中有效，且函数退出后，变量的值会被保存
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


#endif


