/*
 * TCP + sendfile 文件发送端（RDMA 对比基准）
 * 用法: ./tcp_client <server_ip> [filename]
 * 流程: connect -> 发 header(magic+total) -> sendfile() 零拷贝发送 -> 等 ACK -> 输出吞吐
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
#include <sys/sendfile.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* 与 rdma.h 的 file_type_t 保持一致：header 格式相同，对齐对比口径 */
typedef struct file_header_s {
    uint32_t magic;
    size_t total;
} file_header_t;

#define TCP_SERVER_PORT 9002

/* 循环发送，处理 send/recv 的短写短读 */
static int send_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("send");
            return -1;
        }
        sent += n;
    }
    return 0;
}

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
    if (argc < 2) {
        fprintf(stderr, "usage: %s <server_ip> [filename]\n", argv[0]);
        return -1;
    }
    const char *server_ip = argv[1];
    const char *filename = (argc > 2) ? argv[2] : "bench_100m.bin";

    /* 打开待发送文件 */
    int file_fd = open(filename, O_RDONLY);
    if (file_fd < 0) {
        perror("open");
        return -1;
    }
    struct stat st;
    if (0 != fstat(file_fd, &st)) {
        perror("fstat");
        return -1;
    }
    size_t total = st.st_size;
    printf("[tcp sendfile] file: %s, total: %ld bytes\n", filename, total);

    /* 建连 */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TCP_SERVER_PORT);
    if (1 != inet_pton(AF_INET, server_ip, &addr.sin_addr)) {
        perror("inet_pton");
        return -1;
    }
    if (0 != connect(sock, (struct sockaddr*)&addr, sizeof(addr))) {
        perror("connect");
        return -1;
    }
    printf("[tcp sendfile] connected to %s:%d\n", server_ip, TCP_SERVER_PORT);

    /* 发 header：与 RDMA 端相同格式 */
    file_header_t hdr = {0};
    hdr.magic = 0x12345678;
    hdr.total = total;
    if (0 != send_all(sock, &hdr, sizeof(hdr))) return -1;

    /* ===== 计时开始：header 发出后，只统计数据传输 ===== */
    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);

    /* sendfile 循环：文件数据从页缓存直接进 socket，不经过用户态 */
    off_t offset = 0;
    size_t sent = 0;
    while (sent < total) {
        size_t to_send = total - sent;
        if (to_send > 1 << 30) to_send = 1 << 30;   /* 单次上限 1GB */

        ssize_t n = sendfile(sock, file_fd, &offset, to_send);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            perror("sendfile");
            return -1;
        }
        sent += n;
    }

    /* 等 server ACK */
    char ack[64] = {0};
    if (0 != recv_all(sock, ack, sizeof("sync finished"))) return -1;

    /* ===== 计时结束 ===== */
    clock_gettime(CLOCK_MONOTONIC, &ts1);
    double sec = (ts1.tv_sec - ts0.tv_sec) + (ts1.tv_nsec - ts0.tv_nsec) / 1e9;

    printf("[tcp sendfile] server ACK: %s\n", ack);
    printf("[tcp sendfile] sent %zu bytes in %.3f s -> %.2f MB/s\n",
           total, sec, total / (1024.0 * 1024.0) / sec);

    close(file_fd);
    close(sock);
    return 0;
}
