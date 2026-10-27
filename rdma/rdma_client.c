#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>
#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <unistd.h>
#include "rdma.h"


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

    struct sockaddr_in src_addr;
    memset(&src_addr, 0, sizeof(src_addr));
    src_addr.sin_family = AF_INET;
    src_addr.sin_port = htons(RDMA_CLIENT_PORT);
    src_addr.sin_addr.s_addr = inet_addr(RDMA_CLIENT_IP);

    // 将ip地址解析为rdma地址，如果成功，就把这个RDMA地址绑定到对应的设备上面
    if (0 != rdma_resolve_addr(cm_id, (struct sockaddr*)&src_addr, (struct sockaddr*)&dst_addr, RDMA_TIMEOUT_MS)) {// 解析1s
        perror("rdma_resolve_addr\n");
        return 0;
    }

    rdma_wait_cm_event(channel, RDMA_CM_EVENT_ADDR_RESOLVED, NULL);

    // 解析出一个路由器
    if (0 != rdma_resolve_route(cm_id, RDMA_TIMEOUT_MS)) {
        perror("rdma_resolve_route\n");
        return 0;
    }

    rdma_wait_cm_event(channel, RDMA_CM_EVENT_ROUTE_RESOLVED, NULL);

    // 构建队列
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    rdma_create_queuepair(cm_id, &pd, &cq);

    rdma_alloc_buffer(cm_id, pd);

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

    if (rdma_wait_cm_event(channel, RDMA_CM_EVENT_ESTABLISHED, NULL) != 0) {
        printf("Connect failed!\n");
        return -1;
    }

    printf("rdma connect success\n");

    conn_manager_t *cm = cm_id->context;
    sprintf(cm->sbuffer, "abcdefghijklmnopqrstuvwxyz");
    rdma_post_send(cm_id);

    struct ibv_wc wc;
    if (0 != rdma_poll_wc(cq, &wc) || wc.opcode != IBV_WC_SEND) {
        perror("rdma_poll_wc\n");
        return 0;
    }

    rdma_post_recv(cm_id);
    if (0 != rdma_poll_wc(cq, &wc) || wc.opcode != IBV_WC_RECV) {
        perror("rdma_poll_wc\n");
        return 0;
    }
    printf("Recv: %s\n", cm->rbuffer);



    getchar();

}




