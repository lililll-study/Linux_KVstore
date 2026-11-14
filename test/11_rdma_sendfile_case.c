/*
 * 11_rdma_sendfile_case.c —— 单文件传输测试（RDMA / sendfile 二选一）
 *
 * 用法：
 *   从机（接收方）: ./11_rdma_sendfile_case <mode> server [输出文件]
 *   主机（发送方）: ./11_rdma_sendfile_case <mode> client <从机IP> [发送文件]
 *
 *   mode: 0 = RDMA
 *         1 = sendfile（TCP 零拷贝）
 *
 *   默认发送文件: bench_1000m.bin（1000MB）
 *   生成方式: dd if=/dev/urandom of=bench_1000m.bin bs=1M count=1000
 *
 * 计时口径（两端一致，不含建连）：
 *   起点: 发送方把 header 交给内核之前 / 接收方收到 header 之后
 *   终点: 接收方数据真正落盘（RDMA msync(MS_SYNC) / TCP fsync()）并回确认，
 *         发送方收到该确认的时刻
 *
 * 块大小与同步模式沿用 ../rdma/rdma.h：
 *   RDMA_BUFFER_SIZE = 200MB（1000MB 文件正好 5 块）
 *   ENABLE_RDMA_ACK  = 0（连发模式：只等首块 READY 与最终落盘确认）
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>

#include "../rdma/rdma.h"

#define DEFAULT_FILE        "bench_1000m.bin"
#define HEADER_MAGIC        0x12345678u

#define TCP_SERVER_PORT     9002
#define TCP_RECV_BUF_SIZE   (1024 * 1024)   /* 1MB 接收缓冲 */
#define TCP_SOCK_BUF_SIZE   (4 * 1024 * 1024)   /* 4MB socket 缓冲，两侧对称 */

/* header 与 rdma.h 的 file_type_t 字段保持一致，两种传输方式共用同一格式 */
typedef file_type_t file_header_t;

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage:\n"
            "  %s 0|1 server [outfile]\n"
            "  %s 0|1 client <server_ip> [filename]\n"
            "  mode: 0 = RDMA, 1 = sendfile\n"
            "  default file: %s\n",
            prog, prog, DEFAULT_FILE);
}

static void print_result(const char *mode, const char *role, size_t total, double sec)
{
    if (sec <= 0.0) {
        sec = 1e-9;   /* 防止除零；正常不会走到 */
    }

    printf("[RESULT] mode=%s role=%-6s bytes=%zu elapsed=%.3fs throughput=%.2f MiB/s\n",
           mode, role, total, sec,
           total / (1024.0 * 1024.0) / sec);
}

static double time_diff(struct timespec *t0, struct timespec *t1)
{
    return (t1->tv_sec - t0->tv_sec) + (t1->tv_nsec - t0->tv_nsec) / 1e9;
}

/* ============================ TCP 收发小工具 ============================ */

static int send_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("send");
            return -1;
        }
        sent += n;
    }
    return 0;
}

static int recv_all(int fd, void *buf, size_t len)
{
    char *p = (char *)buf;
    size_t got = 0;

    while (got < len) {
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
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

/* ================================ RDMA ================================ */

/* 客户端：解析地址 + 解析路由，拿到可 connect 的 cm_id */
static struct rdma_cm_id *rdma_client_create_id(struct rdma_event_channel *channel,
                                                const char *ip, unsigned short port)
{
    struct rdma_cm_id *cm_id = NULL;

    if (rdma_create_id(channel, &cm_id, NULL, RDMA_PS_TCP) != 0) {
        perror("rdma_create_id");
        return NULL;
    }

    struct sockaddr_in dst_addr;
    memset(&dst_addr, 0, sizeof(dst_addr));
    dst_addr.sin_family = AF_INET;
    dst_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &dst_addr.sin_addr) != 1) {
        perror("inet_pton");
        return NULL;
    }

    struct sockaddr_in src_addr;
    memset(&src_addr, 0, sizeof(src_addr));
    src_addr.sin_family = AF_INET;
    src_addr.sin_port = htons(RDMA_CLIENT_PORT);
    src_addr.sin_addr.s_addr = htonl(INADDR_ANY);   /* 源地址由 CM 自动选择 */

    if (rdma_resolve_addr(cm_id, (struct sockaddr *)&src_addr,
                          (struct sockaddr *)&dst_addr, RDMA_TIMEOUT_MS) != 0) {
        perror("rdma_resolve_addr");
        return NULL;
    }

    if (rdma_wait_cm_event(channel, RDMA_CM_EVENT_ADDR_RESOLVED, NULL) != 0) {
        return NULL;
    }

    if (rdma_resolve_route(cm_id, RDMA_TIMEOUT_MS) != 0) {
        perror("rdma_resolve_route");
        return NULL;
    }

    if (rdma_wait_cm_event(channel, RDMA_CM_EVENT_ROUTE_RESOLVED, NULL) != 0) {
        return NULL;
    }

    return cm_id;
}

static int run_rdma_server(const char *outfile)
{
    struct rdma_event_channel *channel = rdma_create_event_channel();
    if (!channel) {
        perror("rdma_create_event_channel");
        return -1;
    }

    struct rdma_cm_id *listen_id = NULL;
    if (rdma_create_id(channel, &listen_id, NULL, RDMA_PS_TCP) != 0) {
        perror("rdma_create_id");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(RDMA_SERVER_PORT);

    if (rdma_bind_addr(listen_id, (struct sockaddr *)&addr) != 0) {
        perror("rdma_bind_addr");
        return -1;
    }

    if (rdma_listen(listen_id, 1) != 0) {
        perror("rdma_listen");
        return -1;
    }

    printf("[rdma server] listening on port %d ...\n", RDMA_SERVER_PORT);
    fflush(stdout);

    /* 1. 等连接请求 */
    struct rdma_cm_event *event = NULL;
    if (rdma_wait_cm_event(channel, RDMA_CM_EVENT_CONNECT_REQUEST, &event) != 0) {
        return -1;
    }

    struct rdma_cm_id *id = event->id;
    rdma_ack_cm_event(event);

    /* 2. 建 QP / 注册内存 / 先贴一条 RECV 收 header */
    struct ibv_pd *pd = NULL;
    struct ibv_cq *cq = NULL;
    if (rdma_create_queuepair(id, &pd, &cq) != 0) {
        return -1;
    }

    if (rdma_alloc_buffer(id, pd) != 0) {
        return -1;
    }

    if (rdma_post_recv(id) != 0) {
        return -1;
    }

    struct rdma_conn_param param;
    memset(&param, 0, sizeof(param));
    param.responder_resources = 1;
    param.initiator_depth = 1;
    param.retry_count = 7;

    if (rdma_accept(id, &param) != 0) {
        perror("rdma_accept");
        return -1;
    }

    if (rdma_wait_cm_event(id->channel, RDMA_CM_EVENT_ESTABLISHED, NULL) != 0) {
        return -1;
    }

    conn_manager_t *cm = id->context;
    struct ibv_wc wc;

    /* 3. 收 header：magic + 文件总大小 */
    if (rdma_poll_wc(cq, &wc, IBV_WC_RECV) != 0) {
        return -1;
    }

    file_header_t *hdr = (file_header_t *)cm->rbuffer;
    if (hdr->magic != HEADER_MAGIC) {
        fprintf(stderr, "[rdma server] bad magic: 0x%x\n", hdr->magic);
        return -1;
    }

    size_t total = hdr->total;
    if (total == 0) {
        fprintf(stderr, "[rdma server] header total is 0\n");
        return -1;
    }

    printf("[rdma server] header: total=%zu bytes\n", total);
    fflush(stdout);

    /* 4. header 已消费，立刻贴下一块数据的 RECV：
     *    open/ftruncate/mmap 比较慢，若等它们做完再贴 RECV，
     *    客户端第一块 SEND 会撞上"对端无接收缓冲"的空窗 */
    if (rdma_post_recv(id) != 0) {
        return -1;
    }

    int fd = open(outfile, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        perror("open");
        return -1;
    }

    if (ftruncate(fd, (off_t)total) != 0) {
        perror("ftruncate");
        return -1;
    }

    char *p = (char *)mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        return -1;
    }

    /* 5. 计时开始：文件已准备好，只算传输 + 落盘 */
    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);

    /* 6. 通知发送方可以发数据 */
    strcpy(cm->sbuffer, "READY");
    if (rdma_post_send(id, 6) != 0) {
        return -1;
    }

    if (rdma_poll_wc(cq, &wc, IBV_WC_SEND) != 0) {
        return -1;
    }

    /* 7. 收数据块，直接写进 mmap 的文件 */
    size_t idx = 0;
    size_t chunks = 0;

    while (idx < total) {
        if (rdma_poll_wc(cq, &wc, IBV_WC_RECV) != 0) {
            return -1;
        }

        if (wc.byte_len == 0 || wc.byte_len > total - idx) {
            fprintf(stderr, "[rdma server] invalid length: %u (idx=%zu)\n",
                    wc.byte_len, idx);
            return -1;
        }

        memcpy(p + idx, cm->rbuffer, wc.byte_len);
        idx += wc.byte_len;
        chunks++;

        /* 下一块的 RECV 先准备好 */
        if (idx < total && rdma_post_recv(id) != 0) {
            return -1;
        }
    }

    /* 8. 强制落盘（与 TCP 的 fsync 对齐） */
    msync(p, total, MS_SYNC);
    munmap(p, total);
    close(fd);

    /* 9. 回最终确认，发送方收到才停止计时 */
    strcpy(cm->sbuffer, "sync finished");
    if (rdma_post_send(id, strlen("sync finished") + 1) != 0) {
        return -1;
    }

    if (rdma_poll_wc(cq, &wc, IBV_WC_SEND) != 0) {
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &ts1);

    printf("[rdma server] chunks=%zu, saved to %s\n", chunks, outfile);
    print_result("rdma", "server", total, time_diff(&ts0, &ts1));

    rdma_free_buffer(id);
    rdma_destroy_queuepair(id, pd, cq);
    return 0;
}

static int run_rdma_client(const char *server_ip, const char *filename)
{
    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        perror("open");
        fprintf(stderr, "先生成测试文件: dd if=/dev/urandom of=%s bs=1M count=1000\n",
                filename);
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        perror("fstat");
        return -1;
    }

    size_t total = (size_t)st.st_size;
    if (total == 0) {
        fprintf(stderr, "[rdma client] %s is empty\n", filename);
        return -1;
    }

    char *p = (char *)mmap(NULL, total, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        return -1;
    }

    printf("[rdma client] file=%s total=%zu bytes, block=%d MB, chunks=%zu\n",
           filename, total, RDMA_BUFFER_SIZE / (1024 * 1024),
           (total + RDMA_BUFFER_SIZE - 1) / RDMA_BUFFER_SIZE);
    fflush(stdout);

    struct rdma_event_channel *channel = rdma_create_event_channel();
    if (!channel) {
        perror("rdma_create_event_channel");
        return -1;
    }

    struct rdma_cm_id *cm_id = rdma_client_create_id(channel, server_ip, RDMA_SERVER_PORT);
    if (!cm_id) {
        return -1;
    }

    struct ibv_pd *pd = NULL;
    struct ibv_cq *cq = NULL;
    if (rdma_create_queuepair(cm_id, &pd, &cq) != 0) {
        return -1;
    }

    if (rdma_alloc_buffer(cm_id, pd) != 0) {
        return -1;
    }

    struct rdma_conn_param param;
    memset(&param, 0, sizeof(param));
    param.responder_resources = 1;
    param.initiator_depth = 1;
    param.retry_count = 7;

    if (rdma_connect(cm_id, &param) != 0) {
        perror("rdma_connect");
        return -1;
    }

    if (rdma_wait_cm_event(channel, RDMA_CM_EVENT_ESTABLISHED, NULL) != 0) {
        printf("[rdma client] connect failed\n");
        return -1;
    }

    printf("[rdma client] connected to %s:%d\n", server_ip, RDMA_SERVER_PORT);
    fflush(stdout);

    conn_manager_t *cm = cm_id->context;
    struct ibv_wc wc;

    /* 提前贴一条 RECV：先用来收 READY，之后复用为收最终确认 */
    if (rdma_post_recv(cm_id) != 0) {
        return -1;
    }

    /* 1. 发 header（不计入吞吐，但计入总用时起点） */
    file_header_t hdr;
    hdr.magic = HEADER_MAGIC;
    hdr.total = total;
    memcpy(cm->sbuffer, &hdr, sizeof(file_header_t));

    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);

    if (rdma_post_send(cm_id, sizeof(file_header_t)) != 0) {
        return -1;
    }

    if (rdma_poll_wc(cq, &wc, IBV_WC_SEND) != 0) {
        return -1;
    }

    /* 2. 等 READY：服务端已贴好第一块数据的 RECV */
    if (rdma_poll_wc(cq, &wc, IBV_WC_RECV) != 0) {
        return -1;
    }

    if (wc.byte_len != 6 || memcmp(cm->rbuffer, "READY", 6) != 0) {
        fprintf(stderr, "[rdma client] invalid READY\n");
        return -1;
    }

    /* READY 消耗了之前提交的 RECV，重新提交一条用于收最终确认 */
    if (rdma_post_recv(cm_id) != 0) {
        return -1;
    }

    /* 3. 分块发送 */
    size_t idx = 0;
    size_t chunks = 0;

    while (idx < total) {
        size_t length = total - idx;
        if (length > RDMA_BUFFER_SIZE) {
            length = RDMA_BUFFER_SIZE;
        }

        memcpy(cm->sbuffer, p + idx, length);

        if (rdma_post_send(cm_id, length) != 0) {
            fprintf(stderr, "[rdma client] rdma_post_send failed\n");
            return -1;
        }

        /* 等本地 SEND 完成，才能安全复用 sbuffer */
        if (rdma_poll_wc(cq, &wc, IBV_WC_SEND) != 0) {
            return -1;
        }

        idx += length;
        chunks++;
    }

    /* 4. 等最终确认：服务端数据已全部落盘 */
    if (rdma_poll_wc(cq, &wc, IBV_WC_RECV) != 0) {
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &ts1);

    if (wc.byte_len == 0 ||
        memchr(cm->rbuffer, '\0', wc.byte_len) == NULL) {
        fprintf(stderr, "[rdma client] invalid final ACK\n");
        return -1;
    }

    printf("[rdma client] chunks=%zu, server ACK: %s\n", chunks, cm->rbuffer);
    print_result("rdma", "client", total, time_diff(&ts0, &ts1));

    munmap(p, total);
    close(fd);

    rdma_free_buffer(cm_id);
    rdma_destroy_queuepair(cm_id, pd, cq);
    return 0;
}

/* ============================== sendfile ============================== */

static int run_sendfile_server(const char *outfile)
{
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

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        return -1;
    }

    if (listen(listen_fd, 1) != 0) {
        perror("listen");
        return -1;
    }

    printf("[sendfile server] listening on port %d ...\n", TCP_SERVER_PORT);
    fflush(stdout);

    int conn = accept(listen_fd, NULL, NULL);
    if (conn < 0) {
        perror("accept");
        return -1;
    }

    printf("[sendfile server] client connected\n");
    fflush(stdout);

    /* 4MB 接收缓冲：与发送端 SO_SNDBUF 对称，窗口太小会被误判成链路慢 */
    int rcvbuf = TCP_SOCK_BUF_SIZE;
    setsockopt(conn, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    /* 1. 收 header */
    file_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    if (recv_all(conn, &hdr, sizeof(hdr)) != 0) {
        return -1;
    }

    if (hdr.magic != HEADER_MAGIC || hdr.total == 0) {
        fprintf(stderr, "[sendfile server] bad header: magic=0x%x total=%zu\n",
                hdr.magic, hdr.total);
        return -1;
    }

    printf("[sendfile server] header: total=%zu bytes\n", hdr.total);
    fflush(stdout);

    int fd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        perror("open");
        return -1;
    }

    char *buf = (char *)malloc(TCP_RECV_BUF_SIZE);
    if (!buf) {
        perror("malloc");
        return -1;
    }

    /* 2. 计时开始：收到 header 之后（不含建连） */
    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);

    size_t got = 0;
    while (got < hdr.total) {
        ssize_t n = recv(conn, buf, TCP_RECV_BUF_SIZE, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("recv");
            return -1;
        }
        if (n == 0) {
            fprintf(stderr, "[sendfile server] peer closed early: %zu/%zu\n",
                    got, hdr.total);
            return -1;
        }

        if (write(fd, buf, n) != n) {
            perror("write");
            return -1;
        }
        got += n;
    }

    free(buf);

    /* 3. 强制落盘后再回 ACK，与 RDMA 的 msync(MS_SYNC) 口径对齐 */
    if (fsync(fd) != 0) {
        perror("fsync");
        return -1;
    }
    close(fd);

    if (send_all(conn, "sync finished", sizeof("sync finished")) != 0) {
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &ts1);

    printf("[sendfile server] saved to %s\n", outfile);
    print_result("sendfile", "server", got, time_diff(&ts0, &ts1));

    close(conn);
    close(listen_fd);
    return 0;
}

static int run_sendfile_client(const char *server_ip, const char *filename)
{
    int file_fd = open(filename, O_RDONLY);
    if (file_fd < 0) {
        perror("open");
        fprintf(stderr, "先生成测试文件: dd if=/dev/urandom of=%s bs=1M count=1000\n",
                filename);
        return -1;
    }

    struct stat st;
    if (fstat(file_fd, &st) != 0) {
        perror("fstat");
        return -1;
    }

    size_t total = (size_t)st.st_size;
    if (total == 0) {
        fprintf(stderr, "[sendfile client] %s is empty\n", filename);
        return -1;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TCP_SERVER_PORT);
    if (inet_pton(AF_INET, server_ip, &addr.sin_addr) != 1) {
        perror("inet_pton");
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("connect");
        return -1;
    }

    printf("[sendfile client] connected to %s:%d, file=%s total=%zu bytes\n",
           server_ip, TCP_SERVER_PORT, filename, total);
    fflush(stdout);

    /* 4MB 发送缓冲：与接收端 SO_RCVBUF 对称 */
    int sndbuf = TCP_SOCK_BUF_SIZE;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    /* 1. 发 header（与 RDMA 端格式相同） */
    file_header_t hdr;
    hdr.magic = HEADER_MAGIC;
    hdr.total = total;
    if (send_all(sock, &hdr, sizeof(hdr)) != 0) {
        return -1;
    }

    /* 2. 计时开始：header 之后，只算数据传输 + 落盘 */
    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);

    /* sendfile 循环：数据从页缓存直接进 socket，不经过用户态。
     * 注意 sendfile 不保证一次发完，必须累计返回值 */
    off_t offset = 0;
    size_t sent = 0;
    while (sent < total) {
        size_t to_send = total - sent;
        if (to_send > (1u << 30)) {
            to_send = 1u << 30;   /* 单次上限 1GB，防止长度溢出 */
        }

        ssize_t n = sendfile(sock, file_fd, &offset, to_send);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            perror("sendfile");
            return -1;
        }
        sent += n;
    }

    /* 3. 等接收端确认已落盘 */
    char ack[64];
    memset(ack, 0, sizeof(ack));
    if (recv_all(sock, ack, sizeof("sync finished")) != 0) {
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &ts1);

    printf("[sendfile client] server ACK: %s\n", ack);
    print_result("sendfile", "client", total, time_diff(&ts0, &ts1));

    close(file_fd);
    close(sock);
    return 0;
}

/* ================================ main ================================ */

int main(int argc, char *argv[])
{
    if (argc < 3) {
        usage(argv[0]);
        return -1;
    }

    int mode = atoi(argv[1]);
    if (mode != 0 && mode != 1) {
        fprintf(stderr, "mode must be 0 (rdma) or 1 (sendfile)\n");
        usage(argv[0]);
        return -1;
    }

    const char *role = argv[2];

    if (strcmp(role, "server") == 0) {
        /* 从机：接收方 */
        const char *outfile = (argc > 3)
                            ? argv[3]
                            : (mode == 0 ? "rdma_recv_1000m.bin" : "tcp_recv_1000m.bin");
        return (mode == 0) ? run_rdma_server(outfile)
                           : run_sendfile_server(outfile);
    }

    if (strcmp(role, "client") == 0) {
        /* 主机：发送方 */
        if (argc < 4) {
            fprintf(stderr, "client 需要指定从机 IP\n");
            usage(argv[0]);
            return -1;
        }
        const char *server_ip = argv[3];
        const char *filename = (argc > 4) ? argv[4] : DEFAULT_FILE;
        return (mode == 0) ? run_rdma_client(server_ip, filename)
                           : run_sendfile_client(server_ip, filename);
    }

    fprintf(stderr, "role must be 'client' or 'server'\n");
    usage(argv[0]);
    return -1;
}
