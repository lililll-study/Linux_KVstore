/* sync_file_range 属于 GNU 扩展，必须在所有头文件之前定义 */
#define _GNU_SOURCE

/*
 * 11_rdma_sendfile_case.c —— 单文件传输测试（RDMA / sendfile 二选一）
 *
 * 用法：
 *   从机（接收方）: ./11_rdma_sendfile_case <mode> server [输出文件] [profile]
 *   主机（发送方）: ./11_rdma_sendfile_case <mode> client <从机IP> [发送文件] [profile]
 *
 *   mode: 0 = RDMA
 *         1 = sendfile（TCP 零拷贝）
 *
 *   默认发送文件: bench_1000m.bin（1000MB）
 *   生成方式: dd if=/dev/urandom of=bench_1000m.bin bs=1M count=1000
 *
 * 计时口径（两端一致，不含建连）：
 *   起点: 发送方完成 header 发送之后 / 接收方收到 header 之后
 *   终点: 接收方按 profile 完成数据处理并回确认，
 *         发送方收到该确认的时刻
 *
 * profile: 0 = 内存到内存；1 = 文件传输、不强制刷盘；2 = 文件传输并刷盘（默认）
 * RDMA 使用四个 1 MiB 注册缓冲，最多四块在途；接收方每处理完一块返回 credit。
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
#include <sys/statvfs.h>

#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>

#include "../rdma/rdma.h"

#define DEFAULT_FILE        "bench_1000m.bin"
#define HEADER_MAGIC        0x12345678u
#ifndef PIPELINE_DEPTH
#define PIPELINE_DEPTH      16
#endif
#ifndef PIPELINE_CHUNK
#define PIPELINE_CHUNK      (4 * 1024 * 1024)
#endif

/* 边收边刷的粒度：每累计写满这么多字节，就把这段脏页推给磁盘 */
#define SYNC_PUSH_SIZE      (64 * 1024 * 1024)

#define TCP_SERVER_PORT     9002
#define TCP_RECV_BUF_SIZE   (1024 * 1024)   /* 1MB 接收缓冲 */
#define TCP_SOCK_BUF_SIZE   (4 * 1024 * 1024)   /* 4MB socket 缓冲，两侧对称 */

/* header 与 rdma.h 的 file_type_t 字段保持一致，两种传输方式共用同一格式 */
typedef struct {
    uint32_t magic;
    uint32_t profile;
    uint64_t total;
} file_header_t;

/* 0: memory, 1: file without fsync, 2: file with fsync (default). */
static int profile = 2;

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage:\n"
            "  %s 0|1 server [outfile] [profile]\n"
            "  %s 0|1 client <server_ip> [filename] [profile]\n"
            "  mode: 0 = RDMA, 1 = sendfile\n"
            "  profile: 0 = memory, 1 = file without sync, 2 = file with sync (default)\n"
            "  default file: %s\n",
            prog, prog, DEFAULT_FILE);
}

static void print_result(const char *mode, const char *role, size_t total, double sec)
{
    if (sec <= 0.0) {
        sec = 1e-9;   /* 防止除零；正常不会走到 */
    }

    static const char *profiles[] = {"memory", "file-no-sync", "file-sync"};
    printf("[RESULT] mode=%s profile=%s role=%-6s bytes=%zu elapsed=%.3fs throughput=%.2f MiB/s\n",
           profile == 0 && strcmp(mode, "sendfile") == 0 ? "tcp-memory" : mode,
           profiles[profile], role, total, sec,
           total / (1024.0 * 1024.0) / sec);
}

static double time_diff(struct timespec *t0, struct timespec *t1)
{
    return (t1->tv_sec - t0->tv_sec) + (t1->tv_nsec - t0->tv_nsec) / 1e9;
}

static int check_output_space(int fd, size_t total)
{
    struct statvfs fs;
    if (fstatvfs(fd, &fs) != 0) { perror("fstatvfs"); return -1; }
    uint64_t available = (uint64_t)fs.f_bavail * fs.f_frsize;
    if (available < (uint64_t)total + (64u << 20)) {
        fprintf(stderr, "[bench] output filesystem has only %llu free bytes; need file size plus 64 MiB headroom\n",
                (unsigned long long)available);
        return -1;
    }
    return 0;
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
    profile = (int)hdr->profile;
    if (profile < 0 || profile > 2) return -1;
    if (total == 0) {
        fprintf(stderr, "[rdma server] header total is 0\n");
        return -1;
    }

    printf("[rdma server] header: total=%zu bytes\n", total);
    fflush(stdout);

    /* 4. header 已消费，立刻贴下一块数据的 RECV：
     *    open/ftruncate/mmap 比较慢，若等它们做完再贴 RECV，
     *    客户端第一块 SEND 会撞上"对端无接收缓冲"的空窗 */
    rdma_slot_t slots[PIPELINE_DEPTH] = {0}, ack_slots[PIPELINE_DEPTH] = {0};
    for (int i = 0; i < PIPELINE_DEPTH; ++i) {
        if (rdma_slot_init(pd, &slots[i], PIPELINE_CHUNK, 1) != 0 ||
            rdma_slot_init(pd, &ack_slots[i], 32, 0) != 0 ||
            rdma_slot_recv(id, &slots[i], 100 + i) != 0) return -1;
    }

    int fd = -1;
    char *p;
    if (profile == 0) {
        p = malloc(total);
        if (!p) return -1;
        memset(p, 0x5a, total);
    } else {
        fd = open(outfile, O_RDWR | O_CREAT | O_TRUNC, 0666);
        if (fd < 0 || check_output_space(fd, total) != 0 ||
            ftruncate(fd, (off_t)total) != 0) return -1;
        p = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) return -1;
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
    size_t ack_sent = 0;
    size_t synced = 0;      /* 已经推给磁盘的字节数，用于分批发起回写 */

    while (idx < total) {
        if (rdma_poll_wc(cq, &wc, IBV_WC_RECV) != 0) {
            return -1;
        }

        if (wc.wr_id < 100 || wc.wr_id >= 100 + PIPELINE_DEPTH ||
            wc.byte_len == 0 || wc.byte_len > total - idx ||
            wc.byte_len > PIPELINE_CHUNK) {
            fprintf(stderr, "[rdma server] invalid length: %u (idx=%zu)\n",
                    wc.byte_len, idx);
            return -1;
        }

        int slot = (int)(wc.wr_id - 100);
        memcpy(p + idx, slots[slot].buffer, wc.byte_len);
        idx += wc.byte_len;
        chunks++;

        /* 每累计 64MB 就把这段脏页交给磁盘：sync_file_range(WRITE) 只发起回写、
         * 立即返回，磁盘写和后续接收并行；最后的 msync(MS_SYNC) 只需等最后一段 */
        if (profile == 2 && idx - synced >= SYNC_PUSH_SIZE) {
            if (sync_file_range(fd, (off_t)synced, (off_t)(idx - synced),
                                SYNC_FILE_RANGE_WRITE) != 0) {
                perror("sync_file_range");
                return -1;
            }
            synced = idx;
        }

        /* 不是最后一块：先贴好下一块的 RECV，再回 ACK。
         * 顺序不能反：先贴 RECV 才能保证对端发下一块时有接收缓冲，不会触发 RNR */
        if (idx < total) {
            if (rdma_slot_recv(id, &slots[slot], 100 + slot) != 0) {
                return -1;
            }

            /* ACK 走独立环形槽异步发出（signaled=0），不等 SEND 完成，
             * 接收循环保持流动；所有 ACK 内容都是 "ACK"，即使槽位被复用覆盖也不影响语义 */
            int aslot = (int)(ack_sent % PIPELINE_DEPTH);
            strcpy(ack_slots[aslot].buffer, "ACK");
            if (rdma_slot_send(id, &ack_slots[aslot], 4, 400 + aslot, 0) != 0) {
                return -1;
            }
            ++ack_sent;
        }
    }

    struct timespec ts_data_end, ts_sync_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_data_end);
    /* 8. 收尾落盘（与 TCP 的 fsync 对齐）：前面已经分批推过 64MB 段，
     *    这里只等最后一段和异步回写真正完成，所以 sync 时间会明显变短 */
    if (profile == 2 && msync(p, total, MS_SYNC) != 0) return -1;
    clock_gettime(CLOCK_MONOTONIC, &ts_sync_end);
    if (profile == 0) free(p); else { munmap(p, total); close(fd); }

    /* 9. 回最终确认，发送方收到才停止计时。
     *    RC 连接上的 SEND 按提交顺序送达，所以它一定排在此前所有 ACK 之后 */
    strcpy(cm->sbuffer, "sync finished");
    if (rdma_post_send(id, strlen("sync finished") + 1) != 0) {
        return -1;
    }

    if (rdma_poll_wc(cq, &wc, IBV_WC_SEND) != 0) {
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &ts1);

    printf("[rdma server] chunks=%zu, saved to %s\n", chunks, outfile);
    printf("[PHASE] mode=rdma profile=%d data=%.3fs sync=%.3fs\n",
           profile, time_diff(&ts0, &ts_data_end), time_diff(&ts_data_end, &ts_sync_end));
    print_result("rdma", "server", total, time_diff(&ts0, &ts1));

    rdma_destroy_qp(id);
    for (int i = 0; i < PIPELINE_DEPTH; ++i) {
        rdma_slot_free(&slots[i]);
        rdma_slot_free(&ack_slots[i]);
    }
    rdma_free_buffer(id);
    ibv_destroy_cq(cq);
    ibv_dealloc_pd(pd);
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

    char *p = profile == 0 ? malloc(total) :
        (char *)mmap(NULL, total, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED || !p) {
        perror("mmap");
        return -1;
    }
    if (profile == 0) memset(p, 0x5a, total);

    printf("[rdma client] file=%s total=%zu bytes, block=%d MiB, chunks=%zu, depth=%d\n",
           filename, total, PIPELINE_CHUNK / (1024 * 1024),
           (total + PIPELINE_CHUNK - 1) / PIPELINE_CHUNK, PIPELINE_DEPTH);
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

    /* 提前贴一条 RECV 收 READY；数据阶段的 ACK 走下面独立的环形槽缓冲 */
    if (rdma_post_recv(cm_id) != 0) {
        return -1;
    }

    rdma_slot_t sends[PIPELINE_DEPTH] = {0}, acks[PIPELINE_DEPTH] = {0};
    for (int i = 0; i < PIPELINE_DEPTH; ++i) {
        if (rdma_slot_init(pd, &sends[i], PIPELINE_CHUNK, 0) != 0 ||
            rdma_slot_init(pd, &acks[i], 32, 1) != 0 ||
            rdma_slot_recv(cm_id, &acks[i], 200 + i) != 0) return -1;
    }

    /* 1. 发 header（不计入吞吐，但计入总用时起点） */
    file_header_t hdr;
    hdr.magic = HEADER_MAGIC;
    hdr.profile = (uint32_t)profile;
    hdr.total = total;
    memcpy(cm->sbuffer, &hdr, sizeof(file_header_t));

    struct timespec ts0, ts1;
    if (rdma_post_send(cm_id, sizeof(file_header_t)) != 0) {
        return -1;
    }

    if (rdma_poll_wc(cq, &wc, IBV_WC_SEND) != 0) {
        return -1;
    }
    clock_gettime(CLOCK_MONOTONIC, &ts0);

    /* 2. 等 READY：服务端已贴好第一块数据的 RECV */
    if (rdma_poll_wc(cq, &wc, IBV_WC_RECV) != 0) {
        return -1;
    }

    if (wc.byte_len != 6 || memcmp(cm->rbuffer, "READY", 6) != 0) {
        fprintf(stderr, "[rdma client] invalid READY\n");
        return -1;
    }

    /* 3. 分块发送：窗口内的数据块连续 post，用 signaled=0 关掉每块的 SEND 完成事件。
     *    "这一块已被对端收下"由服务端回的 ACK 证明，省掉每块一次 CQ 等待 */
    size_t idx = 0, chunks = 0, outstanding = 0;
    while (idx < total || outstanding) {
        /* 先把窗口填满：连续 post 后立即进入收 ACK，让多个 WR 同时在途 */
        while (idx < total && outstanding < PIPELINE_DEPTH) {
            int slot = (int)(chunks % PIPELINE_DEPTH);
            size_t length = total - idx;
            if (length > PIPELINE_CHUNK) length = PIPELINE_CHUNK;
            memcpy(sends[slot].buffer, p + idx, length);
            if (rdma_slot_send(cm_id, &sends[slot], length, 300 + slot, 0) != 0) {
                return -1;
            }
            idx += length;
            ++chunks;
            ++outstanding;
        }

        /* 窗口满或数据发完：收一个 ACK 释放一个槽位，槽位释放后下一轮才能复用这块缓冲 */
        if (rdma_poll_wc(cq, &wc, IBV_WC_RECV) != 0 ||
            wc.wr_id < 200 || wc.wr_id >= 200 + PIPELINE_DEPTH) return -1;
        int slot = (int)(wc.wr_id - 200);
        if (wc.byte_len == 4 && memcmp(acks[slot].buffer, "ACK", 4) == 0) {
            --outstanding;
            if (rdma_slot_recv(cm_id, &acks[slot], 200 + slot) != 0) return -1;
        } else if (wc.byte_len == sizeof("sync finished") &&
                   memcmp(acks[slot].buffer, "sync finished", wc.byte_len) == 0) {
            outstanding = 0;
            break;
        } else return -1;
    }
    /* 最后一块没有单独的 ACK，由服务端的最终确认代替 */
    if (outstanding != 0) return -1;

    clock_gettime(CLOCK_MONOTONIC, &ts1);

    printf("[rdma client] chunks=%zu, server ACK: sync finished\n", chunks);
    print_result("rdma", "client", total, time_diff(&ts0, &ts1));

    if (profile == 0) free(p); else munmap(p, total);
    close(fd);

    rdma_destroy_qp(cm_id);
    for (int i = 0; i < PIPELINE_DEPTH; ++i) {
        rdma_slot_free(&sends[i]);
        rdma_slot_free(&acks[i]);
    }
    rdma_free_buffer(cm_id);
    ibv_destroy_cq(cq);
    ibv_dealloc_pd(pd);
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
    profile = (int)hdr.profile;
    if (profile < 0 || profile > 2) return -1;

    printf("[sendfile server] header: total=%zu bytes\n", hdr.total);
    fflush(stdout);

    int fd = -1;
    char *memory = NULL;
    if (profile == 0) {
        memory = malloc(hdr.total);
        if (!memory) return -1;
        memset(memory, 0x5a, hdr.total);
    } else {
        fd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd < 0) { perror("open"); return -1; }
        if (check_output_space(fd, hdr.total) != 0) return -1;
    }

    char *buf = (char *)malloc(TCP_RECV_BUF_SIZE);
    if (!buf) {
        perror("malloc");
        return -1;
    }

    /* 准备（open/malloc）已完成，先通知发送端可以开始计时，
     * 避免把接收端的准备时间算进传输时间——两端起点因此严格对齐 */
    if (send_all(conn, "READY", sizeof("READY")) != 0) {
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

        if (profile == 0) memcpy(memory + got, buf, n);
        else {
            ssize_t off = 0;
            while (off < n) {
                ssize_t written = write(fd, buf + off, n - off);
                if (written < 0 && errno == EINTR) continue;
                if (written <= 0) { perror("write"); return -1; }
                off += written;
            }
        }
        got += n;
    }

    struct timespec ts_data_end, ts_sync_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_data_end);
    free(buf);

    /* 3. 收尾落盘后再回 ACK，与 RDMA 的 msync(MS_SYNC) 口径对齐 */
    if (profile == 2 && fsync(fd) != 0) {
        perror("fsync");
        return -1;
    }
    clock_gettime(CLOCK_MONOTONIC, &ts_sync_end);
    if (fd >= 0) close(fd);
    free(memory);

    if (send_all(conn, "sync finished", sizeof("sync finished")) != 0) {
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &ts1);

    printf("[sendfile server] saved to %s\n", outfile);
    printf("[PHASE] mode=sendfile profile=%d data=%.3fs sync=%.3fs\n",
           profile, time_diff(&ts0, &ts_data_end), time_diff(&ts_data_end, &ts_sync_end));
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
    hdr.profile = (uint32_t)profile;
    hdr.total = total;
    if (send_all(sock, &hdr, sizeof(hdr)) != 0) {
        return -1;
    }

    /* 等接收端准备就绪（open/malloc 完成），两端计时起点才对得上 */
    char ready[16];
    memset(ready, 0, sizeof(ready));
    if (recv_all(sock, ready, sizeof("READY")) != 0) {
        return -1;
    }
    if (memcmp(ready, "READY", sizeof("READY")) != 0) {
        fprintf(stderr, "[sendfile client] bad handshake\n");
        return -1;
    }

    /* 2. 计时开始：header 之后，只算数据传输 + 落盘 */
    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);

    /* sendfile 循环：数据从页缓存直接进 socket，不经过用户态。
     * 注意 sendfile 不保证一次发完，必须累计返回值 */
    off_t offset = 0;
    size_t sent = 0;
    char *memory = NULL;
    if (profile == 0) {
        memory = malloc(TCP_RECV_BUF_SIZE);
        if (!memory) return -1;
        memset(memory, 0x5a, TCP_RECV_BUF_SIZE);
    }
    while (sent < total) {
        size_t to_send = total - sent;
        if (to_send > (1u << 30)) {
            to_send = 1u << 30;   /* 单次上限 1GB，防止长度溢出 */
        }

        ssize_t n;
        if (profile == 0) {
            if (to_send > TCP_RECV_BUF_SIZE) to_send = TCP_RECV_BUF_SIZE;
            n = send(sock, memory, to_send, 0);
        } else n = sendfile(sock, file_fd, &offset, to_send);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            perror("sendfile");
            return -1;
        }
        sent += n;
    }
    free(memory);

    /* 3. 等接收端确认已落盘 */
    char ack[64];
    memset(ack, 0, sizeof(ack));
    if (recv_all(sock, ack, sizeof("sync finished")) != 0) {
        return -1;
    }
    if (memcmp(ack, "sync finished", sizeof("sync finished")) != 0) return -1;

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
        profile = argc > 4 ? atoi(argv[4]) : 2;
        if (profile < 0 || profile > 2) { usage(argv[0]); return -1; }
        /* 从机：接收方 */
        const char *outfile = (argc > 3)
                            ? argv[3]
                            : (mode == 0 ? "rdma_recv_1000m.bin" : "tcp_recv_1000m.bin");
        return (mode == 0) ? run_rdma_server(outfile)
                           : run_sendfile_server(outfile);
    }

    if (strcmp(role, "client") == 0) {
        profile = argc > 5 ? atoi(argv[5]) : 2;
        if (profile < 0 || profile > 2) { usage(argv[0]); return -1; }
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
