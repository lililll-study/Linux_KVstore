#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>
#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <unistd.h>
#include "rdma.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>


int main(int argc, char *argv[]) {
    /* 用法: ./rdma_server [outfile]，输出耗时与吞吐 */
    const char *outfile = (argc > 1) ? argv[1] : RDMA_FILENAME;

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
    printf("[RDMA server] listening on port %d...\n", RDMA_SERVER_PORT);

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

    // 收 header：magic + 文件总大小
    file_type_t *type = (file_type_t*)cm->rbuffer;
    uint32_t magic = type->magic;
    size_t total = type->total;
    printf("[RDMA server] header: total=%ld, magic=%x\n", type->total, type->magic);

    /* header 已消费，立刻贴上第一块 chunk 的 RECV：
     * open/ftruncate/mmap 耗时较长，若等它们做完才贴 RECV，
     * 客户端第一块 SEND 会撞上“对端无接收缓冲”空窗，触发 RNR 报错 */
    if (rdma_post_recv(id) != 0) {
        fprintf(stderr, "rdma_post_recv failed\n");
        return -1;
    }

    int fd = open(outfile, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        perror("open\n");
        return 0;
    }
    ftruncate(fd, total); // 创建文件指定大小

    // 把内容映射进文件
    char *p = (char *)mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        return 0;
    }

    /* ===== 计时开始：收到 header 之后（不含建连开销）===== */
    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);

    size_t idx = 0;    // 当前写到文件的位置
    while (idx < total) {   // 没有读到末尾就循环

        if (0 != rdma_poll_wc(cq, &wc) || wc.opcode != IBV_WC_RECV) {
            perror("rdma_poll_wc || wc.opcode\n");
            return -1;
        }

        size_t remaining = total - idx;

        if (wc.byte_len == 0 || wc.byte_len > remaining) {
            fprintf(stderr, "Invalid received length: %u\n", wc.byte_len);
            return -1;
        }

        /* 收完一块后先贴下一块 RECV 再 memcpy：
         * memcpy 期间对端可能正在发下一块，提前补上接收缓冲避免 RNR 空窗；
         * 最后一块不再贴（保持一发一收配对：初始1 + 预贴1 + 循环补99 = 101） */
        if (idx + wc.byte_len < total) {
            if (rdma_post_recv(id) != 0) {
                fprintf(stderr, "rdma_post_recv failed\n");
                return -1;
            }
        }

        memcpy(p + idx, cm->rbuffer, wc.byte_len);
        idx += wc.byte_len;
    }

    msync(p, total, MS_SYNC);
    munmap(p, total);
    close(fd);

    // 回 ACK
    const char *msg = "sync finished";
    strcpy(cm->sbuffer, msg);
    rdma_post_send(id, strlen(msg) + 1);
    if (0 != rdma_poll_wc(cq, &wc) || wc.opcode != IBV_WC_SEND) {
        perror("rdma_poll_wc\n");
        return 0;
    }

    /* ===== 计时结束：ACK 发出 ===== */
    clock_gettime(CLOCK_MONOTONIC, &ts1);
    double sec = (ts1.tv_sec - ts0.tv_sec) + (ts1.tv_nsec - ts0.tv_nsec) / 1e9;

    printf("[RDMA server] recv %zu bytes in %.3f s -> %.2f MB/s, saved to %s\n",
           total, sec, total / (1024.0 * 1024.0) / sec, outfile);

    rdma_free_buffer(id);
    rdma_destroy_queuepair(id, pd, cq);
    return 0;
}
