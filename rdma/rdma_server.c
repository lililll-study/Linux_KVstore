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
    // 分配事件通道
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
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(RDMA_SERVER_PORT);
    if (0 != rdma_bind_addr(cm_id, (struct sockaddr*)&server_addr)) {
        perror("rdma_bind_addr\n");
        return 0;
    }
    if (0 != rdma_listen(cm_id, 10)) {
        perror("rdma_listen\n");
        return 0;
    }

    struct rdma_cm_event *event = NULL;
    rdma_wait_cm_event(channel, RDMA_CM_EVENT_CONNECT_REQUEST, &event);
    struct rdma_cm_id *id = event->id;
    rdma_ack_cm_event(event);

    struct ibv_pd *pd;
    struct ibv_cq *cq;
    if (0 != rdma_create_queuepair(id, &pd, &cq)) {
        perror("rdma_create_queuepair\n");
        return 0;
    }

    rdma_alloc_buffer(id, pd);

    rdma_post_recv(id);

    struct rdma_conn_param param;
    memset(&param, 0, sizeof(param));
    param.responder_resources = 1;
    param.initiator_depth = 1;
    param.retry_count = 3;


    if (0 != rdma_accept(id, &param)) {
        perror("rdma_accept\n");
        return 0;
    }
    // 连接建立完了，拿到了ID，接着可以进行数据传输
    rdma_wait_cm_event(id->channel, RDMA_CM_EVENT_ESTABLISHED, NULL);

    struct ibv_wc wc;
    if (0 != rdma_poll_wc(cq, &wc) || wc.opcode != IBV_WC_RECV) {
        perror("rdma_poll_wc\n");
        return 0;
    }
    conn_manager_t *cm = id->context;
    printf("recv: %s\n", cm->rbuffer);

    int i=0;
    for (i=0; i<RDMA_BUFFER_SIZE; i++) {
        if (cm->rbuffer[i] == 0x0) break;
        cm->sbuffer[i] = cm->rbuffer[i] - 0x20;
    }

    rdma_post_send(id);
    if (0 != rdma_poll_wc(cq, &wc) || wc.opcode != IBV_WC_SEND) {
        perror("rdma_poll_wc\n");
        return 0;
    }

    getchar();


    // // 必须用 while 循环去拿事件，因为 rdma_get_cm_event 会直接给指针
    // while (rdma_get_cm_event(channel, &event) == 0) {
        
    //     if (event->event == RDMA_CM_EVENT_CONNECT_REQUEST) {
    //         printf("Server: got connect request!\n");

    //         // 1. 获取客户端发来的 ID
    //         struct rdma_cm_id *child_cm_id = event->id;
            
    //         // 2. 为这个新连接创建 QP 和 CQ (复用你的函数)
    //         struct ibv_pd *pd;
    //         struct ibv_cq *cq;
    //         rdma_create_queuepair(child_cm_id, &pd, &cq);

    //         // 3. 准备接受参数
    //         struct rdma_conn_param conn_param;
    //         memset(&conn_param, 0, sizeof(conn_param));
    //         conn_param.responder_resources = 1;
    //         conn_param.initiator_depth = 1;

    //         // 4. 关键步：调用 rdma_accept() 回应客户端
    //         if (rdma_accept(child_cm_id, &conn_param) != 0) {
    //             perror("rdma_accept");
    //         } else {
    //             printf("Server: accepted client\n");
    //         }
    //     }
        
    //     // 释放事件
    //     rdma_ack_cm_event(event);
    // }
    
}