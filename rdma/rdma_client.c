#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>
#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <unistd.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>

#include "rdma.h"

static struct rdma_cm_id *rdma_create_cm_id(struct rdma_event_channel *channel, const char *ip, unsigned short port) {

    struct rdma_cm_id *cm_id; 
    if (0 != rdma_create_id(channel, &cm_id, NULL, RDMA_PS_TCP)) {
        perror("rdma_create_id\n");
        return 0;
    }

    struct sockaddr_in dst_addr;
    memset(&dst_addr, 0, sizeof(dst_addr));
    dst_addr.sin_family = AF_INET;
    dst_addr.sin_port = htons(port);
    if (1 != inet_pton(AF_INET, ip, &dst_addr.sin_addr)) {
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

    return cm_id;
}


int main() {
    // 创建channel和id
    struct rdma_event_channel *channel = rdma_create_event_channel();
    if (!channel) {
        perror("rdma_create_event_channel\n");
        return 0;
    }

    struct rdma_cm_id *cm_id = rdma_create_cm_id(channel, RDMA_SERVER_IP, RDMA_SERVER_PORT);

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
    // sprintf(cm->sbuffer, "abcdefghijklmnopqrstuvwxyz");

// openfile
    int fd = open(RDMA_FILENAME, O_RDWR);
    if (fd < 0) {
        perror("open\n");
        return 0;
    }
    // 判断文件大小的方法：
    // 1：libc fseek();
    // 2：POSIX API
    struct stat st;
    if(0 != fstat(fd, &st)) {
        perror("fstat\n");
        return 0;
    }

    size_t total = st.st_size;
    printf("total: %ld\n", total);


// readfile
    char *p = (char *)mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        return 0;
    }



// 把p的内容放进sendbuffer中
    int idx = 0;    // 当前读到文件的位置
    struct ibv_wc wc;

    file_type_t type = {0};
    type.magic = 0x12345678;
    type.total = total;
    memcpy(cm->sbuffer, &type, sizeof(file_type_t));
    rdma_post_send(cm_id);

    if (0 != rdma_poll_wc(cq, &wc) || wc.opcode != IBV_WC_SEND) {
        perror("rdma_poll_wc\n");
        return 0;
    }

    while (idx < total) {   // 没有读到末尾就循环
        if (idx + RDMA_BUFFER_SIZE < total) { // 如果剩余内容超过512字节，就读满512字节
            memcpy(cm->sbuffer, p + idx, RDMA_BUFFER_SIZE);
            idx += RDMA_BUFFER_SIZE;
        } else {    // 如果剩余内容小于512字节，就把剩下的都读掉
            memcpy(cm->sbuffer, p + idx, total - idx);
            idx = total;
        }
        printf("idx: %d\n", idx);
        rdma_post_send(cm_id);
        if (0 != rdma_poll_wc(cq, &wc) || wc.opcode != IBV_WC_SEND) {
            perror("rdma_poll_wc\n");
            return 0;
        }
    }

    munmap(p, total);
    close(fd);

    rdma_post_recv(cm_id);
    if (0 != rdma_poll_wc(cq, &wc) || wc.opcode != IBV_WC_RECV) {
        perror("rdma_poll_wc\n");
        return 0;
    }
    printf("Recv: %s\n", cm->rbuffer);



    getchar();

    rdma_free_buffer(cm_id); 
    rdma_destroy_queuepair(cm_id, pd, cq);
}




