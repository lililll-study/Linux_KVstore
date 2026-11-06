/*
 * TCP 文件接收端（RDMA 对比基准）
 * 用法: ./tcp_server [outfile]
 * 流程: listen -> accept -> 收 header -> recv 循环写文件 -> 回 ACK -> 输出吞吐
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>

typedef struct file_header_s {
    uint32_t magic;
    size_t total;
} file_header_t;

#define TCP_SERVER_PORT 9002
#define RECV_BUF_SIZE   (1024 * 1024)   /* 1MB 接收缓冲，与 RDMA 块大小一致 */

static int recv_all(int fd, void *buf, size_t len) {
    char *p = (char *)buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("recv");
            return -1;
        }
        if (n == 0) {
            fprintf(stderr, "peer closed\n");
            return -1;
        }
        got += n;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    const char *outfile = (argc > 1) ? argv[1] : "tcp_recv.bin";

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return -1;
    }

    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(TCP_SERVER_PORT);
    if (0 != bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr))) {
        perror("bind");
        return -1;
    }
    if (0 != listen(listen_fd, 10)) {
        perror("listen");
        return -1;
    }
    printf("[tcp server] listening on port %d...\n", TCP_SERVER_PORT);

    int conn = accept(listen_fd, NULL, NULL);
    if (conn < 0) {
        perror("accept");
        return -1;
    }
    printf("[tcp server] client connected\n");

    /* 收 header */
    file_header_t hdr = {0};
    if (0 != recv_all(conn, &hdr, sizeof(hdr))) return -1;
    printf("[tcp server] header: total=%ld, magic=%x\n", hdr.total, hdr.magic);

    int fd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        perror("open");
        return -1;
    }

    /* ===== 计时开始：收到 header 之后（不含建连开销），与 RDMA server 口径一致 ===== */
    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);

    char *buf = malloc(RECV_BUF_SIZE);
    if (!buf) {
        perror("malloc");
        return -1;
    }

    size_t got = 0;
    while (got < hdr.total) {
        ssize_t n = recv(conn, buf, RECV_BUF_SIZE, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("recv");
            return -1;
        }
        if (n == 0) {
            fprintf(stderr, "peer closed early: %zu/%zu\n", got, hdr.total);
            return -1;
        }
        if (write(fd, buf, n) != n) {
            perror("write");
            return -1;
        }
        got += n;
    }

    free(buf);
    close(fd);

    /* 回 ACK（与 RDMA server 相同内容） */
    if (0 != send(conn, "sync finished", sizeof("sync finished"), 0)) {
        perror("send");
        return -1;
    }

    /* ===== 计时结束 ===== */
    clock_gettime(CLOCK_MONOTONIC, &ts1);
    double sec = (ts1.tv_sec - ts0.tv_sec) + (ts1.tv_nsec - ts0.tv_nsec) / 1e9;

    printf("[tcp server] recv %zu bytes in %.3f s -> %.2f MB/s, saved to %s\n",
           got, sec, got / (1024.0 * 1024.0) / sec, outfile);

    close(conn);
    close(listen_fd);
    return 0;
}
