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

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>



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
    // printf("recv: %s\n", cm->rbuffer);

#if 0
    int i=0;
    for (i=0; i<RDMA_BUFFER_SIZE; i++) {
        if (cm->rbuffer[i] == 0x0) break;
        cm->sbuffer[i] = cm->rbuffer[i] - 0x20;
    }
#else

    file_type_t *type = (file_type_t*)cm->rbuffer;
    uint32_t magic = type->magic;
    size_t total = type->total;
    printf("total: %ld, magic: %x\n", type->total, type->magic);

    printf("open\n");
    int fd = open(RDMA_FILENAME, O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
        perror("open\n");
        return 0;
    }
    ftruncate(fd, total); // 创建文件指定大小

    printf("mmap\n");
    // 把内容映射进文件
    char *p = (char *)mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        return 0;
    }

    int idx = 0;    // 当前读到文件的位置
    while (idx < total) {   // 没有读到末尾就循环

        printf("Before post_recv, idx=%d\n", idx);

        int ret = rdma_post_recv(id);
        if (ret != 0) {
            fprintf(stderr, "rdma_post_recv failed: %d\n", ret);
            return -1;
        }

        // printf("post_recv success, idx=%d\n", idx);

        if (0 != rdma_poll_wc(cq, &wc) || wc.opcode != IBV_WC_RECV) {
            perror("rdma_poll_wc || wc.opcode\n");
            return -1;
        }

        printf("recv complete: idx=%d, byte_len=%u\n", idx, wc.byte_len);

        size_t remaining = total - idx;

        if (wc.byte_len == 0 || wc.byte_len > remaining) {
            fprintf(stderr, "Invalid received length: %u\n", wc.byte_len);
            return -1;
        }

        memcpy(p + idx, cm->rbuffer, wc.byte_len);
        idx += wc.byte_len;

        printf("write complete: idx=%d\n", idx);
    }
    msync(p, total, MS_SYNC);
    munmap(p, total);
    close(fd);

#endif
    const char *msg = "sync finished";
    strcpy(cm->sbuffer, msg);
    rdma_post_send(id, strlen(msg) + 1);
    if (0 != rdma_poll_wc(cq, &wc) || wc.opcode != IBV_WC_SEND) {
        perror("rdma_poll_wc\n");
        return 0;
    }

    getchar();

}