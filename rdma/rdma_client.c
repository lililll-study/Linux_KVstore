#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>
#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define RDMA_SERVER_PORT    9000
#define RDMA_SERVER_IP      "192.168.137.134"
#define RDMA_TIMEOUT_MS     1000
#define RDMA_CQ_DEPTH     32


// static修饰的，说明在该文件中有效，且函数退出后，变量的值会被保存
static int rdma_wait_cm_event(struct rdma_event_channel *channel, enum rdma_cm_event_type expect) {

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


int main() {
    // 创建channel和id
    struct rdma_event_channel *channel = rdma_create_event_channel();
    if (!channel) {
        perror("rdma_create_event_channel\n");
        return 0;
    }

    struct rdma_cm_id *cm_id;
    if (0 != rdma_create_id(channel, &cm_id, NULL, RDMA_PS_TCP)) {
        perror("rdma_create_id\n");
        return 0;
    }

    struct sockaddr_in dst_addr;
    memset(&dst_addr, 0, sizeof(dst_addr));
    dst_addr.sin_family = AF_INET;
    dst_addr.sin_port = htons(RDMA_SERVER_PORT);
    if (1 != inet_pton(AF_INET, RDMA_SERVER_IP, &dst_addr.sin_addr)) {
        perror("inet_pton\n");
        return 0;
    }

    // 将ip地址解析为rdma地址，如果成功，就把这个RDMA地址绑定到对应的设备上面
    if (0 != rdma_resolve_addr(cm_id, NULL, (struct sockaddr*)&dst_addr, RDMA_TIMEOUT_MS)) {// 解析1s
        perror("rdma_resolve_addr\n");
        return 0;
    }

    rdma_wait_cm_event(channel, RDMA_CM_EVENT_ADDR_RESOLVED);

    // 解析出一个路由器
    if (0 != rdma_resolve_route(cm_id, RDMA_TIMEOUT_MS)) {
        perror("rdma_resolve_route\n");
        return 0;
    }

    rdma_wait_cm_event(channel, RDMA_CM_EVENT_ROUTE_RESOLVED);

    // 构建队列
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    rdma_create_queuepair(cm_id, &pd, &cq);

    // 分配buffer
    struct rdma_conn_param param;
    memset(&param, 0, sizeof(param));
    param.responder_resources = 1;
    param.initiator_depth = 1;
    param.retry_count = 3;
    if (0 != rdma_connect(cm_id, &param)) {
        perror("rdma_connect\n");
        return -1;
    }

    rdma_wait_cm_event(channel, RDMA_CM_EVENT_ESTABLISHED);

    printf("rdma connect success\n");
}




