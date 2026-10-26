
#include <rdma/rdma_cma.h>
#include <errno.h>
#include <stdio.h>

#define RDMA_SERVER_PORT 9000

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

    rdma_wait_cm_event(channel, RDMA_CM_EVENT_CONNECT_REQUEST);

    
    getchar();
    
}