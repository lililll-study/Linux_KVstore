#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>
#include "../rdma/rdma.h"

/* Continuous memory-to-memory throughput. No file I/O occurs in the timed path. */
#define TCP_PORT 9002
#define WARMUP_SECONDS 2
#define MEASURE_SECONDS 5
#define BLOCK_SIZE (1024 * 1024)
#define WINDOW 8
#define MAGIC 0x4b565331u
enum { MSG_DATA = 1, MSG_START = 2, MSG_STOP = 3,
       MSG_ACK = 4, MSG_START_ACK = 5, MSG_RESULT = 6 };

struct message { uint32_t magic, type, length, reserved; uint64_t bytes, nanos; };
static volatile uint64_t observed;

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}
static void result(const char *transport, const char *role, uint64_t bytes, uint64_t ns) {
    double sec = ns / 1e9;
    printf("[RESULT] transport=%s role=%s warmup=%ds duration=%.3fs bytes=%" PRIu64
           " throughput=%.2f MiB/s\n", transport, role, WARMUP_SECONDS,
           sec, bytes, sec > 0 ? bytes / 1048576.0 / sec : 0.0);
    fflush(stdout);
}
static int send_all(int fd, const void *data, size_t len) {
    const char *p = data;
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; len -= (size_t)n;
    }
    return 0;
}
static int recv_all(int fd, void *data, size_t len) {
    char *p = data;
    while (len) {
        ssize_t n = recv(fd, p, len, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; len -= (size_t)n;
    }
    return 0;
}
static int tcp_frame(int fd, uint32_t type, const char *payload, uint32_t length) {
    struct message m = {.magic = MAGIC, .type = type, .length = length};
    return send_all(fd, &m, sizeof(m)) ||
           (length && send_all(fd, payload, length));
}
static int tcp_server(void) {
    int listener = socket(AF_INET, SOCK_STREAM, 0), fd = -1, rc = -1;
    char *buf = NULL;
    if (listener < 0) return -1;
    int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(TCP_PORT),
                               .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(listener, (void *)&addr, sizeof(addr)) || listen(listener, 1)) goto out;
    printf("[tcp] listening on %d\n", TCP_PORT); fflush(stdout);
    fd = accept(listener, NULL, NULL);
    if (fd < 0 || !(buf = malloc(BLOCK_SIZE))) goto out;
    int measuring = 0;
    uint64_t start = 0, bytes = 0;
    for (;;) {
        struct message m;
        if (recv_all(fd, &m, sizeof(m)) || m.magic != MAGIC) goto out;
        if (m.type == MSG_DATA && m.length == BLOCK_SIZE) {
            if (recv_all(fd, buf, m.length)) goto out;
            observed += (unsigned char)buf[0];
            if (measuring) bytes += m.length;
        } else if (m.type == MSG_START && m.length == 0) {
            bytes = 0; measuring = 1; start = now_ns();
            char ack = 1;
            if (send_all(fd, &ack, 1)) goto out;
        } else if (m.type == MSG_STOP && m.length == 0 && measuring) {
            uint64_t elapsed = now_ns() - start;
            struct message reply = {.magic = MAGIC, .type = MSG_RESULT,
                                    .bytes = bytes, .nanos = elapsed};
            if (send_all(fd, &reply, sizeof(reply))) goto out;
            result("tcp", "server", bytes, elapsed);
            rc = 0; break;
        } else goto out;
    }
out:
    if (rc) perror("tcp server");
    free(buf); if (fd >= 0) close(fd); close(listener); return rc;
}
static int tcp_client(const char *ip) {
    int fd = socket(AF_INET, SOCK_STREAM, 0), rc = -1;
    char *buf = NULL;
    if (fd < 0) return -1;
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(TCP_PORT)};
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1 ||
        connect(fd, (void *)&addr, sizeof(addr))) goto out;
    if (!(buf = malloc(BLOCK_SIZE))) goto out;
    memset(buf, 0x5a, BLOCK_SIZE);
    uint64_t until = now_ns() + WARMUP_SECONDS * 1000000000ull;
    while (now_ns() < until) if (tcp_frame(fd, MSG_DATA, buf, BLOCK_SIZE)) goto out;
    if (tcp_frame(fd, MSG_START, NULL, 0)) goto out;
    char ack;
    if (recv_all(fd, &ack, 1) || ack != 1) goto out;
    until = now_ns() + MEASURE_SECONDS * 1000000000ull;
    while (now_ns() < until) if (tcp_frame(fd, MSG_DATA, buf, BLOCK_SIZE)) goto out;
    if (tcp_frame(fd, MSG_STOP, NULL, 0)) goto out;
    struct message reply;
    if (recv_all(fd, &reply, sizeof(reply)) || reply.magic != MAGIC ||
        reply.type != MSG_RESULT || reply.nanos == 0) goto out;
    result("tcp", "client", reply.bytes, reply.nanos);
    rc = 0;
out:
    if (rc) perror("tcp client");
    free(buf); close(fd); return rc;
}

#define SF_SOURCE "/tmp/sendfile_src.bin"

/* 从文件描述符 file 的起始位置发出 len 字节；每次调用都把 offset 归零，
 * 因此源文件大小必须 >= len（本测试里文件正好等于一个数据块） */
static int sendfile_all(int sock, int file, size_t len) {
    off_t off = 0;
    while (len) {
        ssize_t n = sendfile(sock, file, &off, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        len -= (size_t)n;
    }
    return 0;
}

/*
 * sendfile 模式的数据阶段不再逐帧加头，而是以整块为单位传输：
 *   普通块 = BLOCK_SIZE 字节数据
 *   MARK 块 / STOP 块 = 块首写入 struct message{magic, type}，其余为 0
 * 这样发送端可以连续 sendfile，接收端按固定块长读取并识别控制块。
 */
#define CTRL_TYPE(ptr) (((struct message *)(ptr))->magic == MAGIC ? ((struct message *)(ptr))->type : 0)

static int sf_server(void) {
    int listener = socket(AF_INET, SOCK_STREAM, 0), fd = -1, rc = -1;
    char *buf = NULL;
    if (listener < 0) return -1;
    int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(TCP_PORT),
                               .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(listener, (void *)&addr, sizeof(addr)) || listen(listener, 1)) goto out;
    printf("[sendfile] listening on %d\n", TCP_PORT); fflush(stdout);
    fd = accept(listener, NULL, NULL);
    if (fd < 0 || !(buf = malloc(BLOCK_SIZE))) goto out;
    int sockbuf = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sockbuf, sizeof(sockbuf));

    int measuring = 0;
    uint64_t start = 0, bytes = 0;
    for (;;) {
        if (recv_all(fd, buf, BLOCK_SIZE)) goto out;
        uint32_t type = CTRL_TYPE(buf);
        if (type == MSG_START) {
            /* 计时起点：到此为止的热身数据不计入 */
            bytes = 0; measuring = 1; start = now_ns();
            continue;
        }
        if (type == MSG_STOP) break;
        if (measuring) bytes += BLOCK_SIZE;
    }
    uint64_t elapsed = now_ns() - start;
    struct message reply = {.magic = MAGIC, .type = MSG_RESULT,
                            .bytes = bytes, .nanos = elapsed};
    if (send_all(fd, &reply, sizeof(reply))) goto out;
    result("sendfile", "server", bytes, elapsed);
    rc = 0;
out:
    if (rc) perror("sendfile server");
    free(buf); if (fd >= 0) close(fd); close(listener); return rc;
}

static int sf_client(const char *ip) {
    int fd = socket(AF_INET, SOCK_STREAM, 0), file = -1, rc = -1;
    char *ctrl = NULL;
    if (fd < 0) return -1;
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(TCP_PORT)};
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1 ||
        connect(fd, (void *)&addr, sizeof(addr))) goto out;
    int sockbuf = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sockbuf, sizeof(sockbuf));

    /* 源文件不存在就先创建一个 BLOCK_SIZE 大小的文件，避免手工准备 */
    file = open(SF_SOURCE, O_RDONLY);
    if (file < 0) {
        char *tmp = malloc(BLOCK_SIZE);
        file = open(SF_SOURCE, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (!tmp || file < 0) { free(tmp); goto out; }
        memset(tmp, 0x5a, BLOCK_SIZE);
        if (write(file, tmp, BLOCK_SIZE) != (ssize_t)BLOCK_SIZE) {
            free(tmp); close(file); file = -1; goto out;
        }
        free(tmp); close(file);
        file = open(SF_SOURCE, O_RDONLY);
        if (file < 0) goto out;
    }
    if (!(ctrl = calloc(1, BLOCK_SIZE))) goto out;

    /* 2 秒热身：接收端收到但不计入统计 */
    uint64_t until = now_ns() + WARMUP_SECONDS * 1000000000ull;
    while (now_ns() < until) if (sendfile_all(fd, file, BLOCK_SIZE)) goto out;

    /* 计时起点标记 */
    struct message *m = (void *)ctrl;
    m->magic = MAGIC; m->type = MSG_START;
    if (send_all(fd, ctrl, BLOCK_SIZE)) goto out;

    until = now_ns() + MEASURE_SECONDS * 1000000000ull;
    while (now_ns() < until) if (sendfile_all(fd, file, BLOCK_SIZE)) goto out;

    /* 结束标记 */
    m->type = MSG_STOP;
    if (send_all(fd, ctrl, BLOCK_SIZE)) goto out;

    struct message reply;
    if (recv_all(fd, &reply, sizeof(reply)) || reply.magic != MAGIC ||
        reply.type != MSG_RESULT || reply.nanos == 0) goto out;
    result("sendfile", "client", reply.bytes, reply.nanos);
    rc = 0;
out:
    if (rc) perror("sendfile client");
    free(ctrl); if (file >= 0) close(file); close(fd); return rc;
}

static int wait_event(struct rdma_event_channel *ec, enum rdma_cm_event_type expected,
                      struct rdma_cm_id **id) {
    struct rdma_cm_event *ev;
    if (rdma_get_cm_event(ec, &ev)) return -1;
    int ok = ev->event == expected;
    if (id) *id = ev->id;
    if (!ok) fprintf(stderr, "CM event %s, expected %s\n",
                     rdma_event_str(ev->event), rdma_event_str(expected));
    rdma_ack_cm_event(ev);
    return ok ? 0 : -1;
}
static int rdma_reply(struct rdma_cm_id *id, struct ibv_cq *cq, struct message *m) {
    conn_manager_t *cm = id->context;
    memcpy(cm->sbuffer, m, sizeof(*m));
    struct ibv_wc wc;
    return rdma_post_send(id, sizeof(*m)) || rdma_poll_wc(cq, &wc, IBV_WC_SEND);
}
static int rdma_server(void) {
    int rc = -1, measuring = 0;
    struct rdma_event_channel *ec = rdma_create_event_channel();
    struct rdma_cm_id *listen_id = NULL, *id = NULL;
    struct ibv_pd *pd = NULL;
    struct ibv_cq *cq = NULL;
    rdma_slot_t slots[WINDOW] = {0}, ack_slots[WINDOW] = {0};
    if (!ec || rdma_create_id(ec, &listen_id, NULL, RDMA_PS_TCP)) goto out;
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(RDMA_SERVER_PORT),
                               .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (rdma_bind_addr(listen_id, (void *)&addr) || rdma_listen(listen_id, 1)) goto out;
    printf("[rdma] listening on %d\n", RDMA_SERVER_PORT); fflush(stdout);
    if (wait_event(ec, RDMA_CM_EVENT_CONNECT_REQUEST, &id) ||
        rdma_create_queuepair(id, &pd, &cq) || rdma_alloc_buffer(id, pd)) goto out;
    for (int i = 0; i < WINDOW; ++i)
        if (rdma_slot_init(pd, &slots[i], sizeof(struct message) + BLOCK_SIZE, 1) ||
            rdma_slot_init(pd, &ack_slots[i], sizeof(struct message), 0) ||
            rdma_slot_recv(id, &slots[i], 100 + i)) goto out;
    struct rdma_conn_param param = {.retry_count = 7};
    if (rdma_accept(id, &param) || wait_event(ec, RDMA_CM_EVENT_ESTABLISHED, NULL)) goto out;
    uint64_t start = 0, bytes = 0, ack_sent = 0;
    for (;;) {
        struct ibv_wc wc;
        if (rdma_poll_wc(cq, &wc, IBV_WC_RECV) ||
            wc.wr_id < 100 || wc.wr_id >= 100 + WINDOW) goto out;
        int slot = (int)(wc.wr_id - 100);
        struct message *m = (void *)slots[slot].buffer;
        if (wc.byte_len < sizeof(*m) || m->magic != MAGIC) goto out;
        uint32_t type = m->type, length = m->length;
        if (type == MSG_DATA && length == BLOCK_SIZE &&
            wc.byte_len == sizeof(*m) + length) {
            observed += (unsigned char)slots[slot].buffer[sizeof(*m)];
            if (measuring) bytes += length;
        } else if (type == MSG_START && length == 0) {
            bytes = 0; measuring = 1; start = now_ns();
        } else if (type != MSG_STOP || length != 0 || !measuring) goto out;

        /* 先把空槽还给客户端，避免后面的 ACK 发送阻塞后续接收 */
        if (rdma_slot_recv(id, &slots[slot], 100 + slot)) goto out;

        if (type == MSG_DATA) {
            /* 数据 ACK 用独立环形槽异步发出：不等 SEND 完成，接收循环保持流动 */
            struct message ack = {.magic = MAGIC, .type = MSG_ACK};
            int aslot = (int)(ack_sent % WINDOW);
            memcpy(ack_slots[aslot].buffer, &ack, sizeof(ack));
            if (rdma_slot_send(id, &ack_slots[aslot], sizeof(ack), 400 + aslot, 0)) goto out;
            ++ack_sent;
            continue;
        }

        /* 控制消息（START_ACK / RESULT）数据量极小，沿用阻塞发送 */
        struct message reply = {.magic = MAGIC, .type = MSG_START_ACK};
        if (type == MSG_STOP) {
            reply.type = MSG_RESULT;
            reply.bytes = bytes; reply.nanos = now_ns() - start;
        }
        if (rdma_reply(id, cq, &reply)) goto out;
        if (type == MSG_STOP) {
            result("rdma", "server", bytes, reply.nanos);
            rc = 0; break;
        }
    }
out:
    if (rc) fprintf(stderr, "rdma server failed\n");
    if (id && id->qp) rdma_destroy_qp(id);
    for (int i = 0; i < WINDOW; ++i) { rdma_slot_free(&slots[i]); rdma_slot_free(&ack_slots[i]); }
    if (id && id->context) rdma_free_buffer(id);
    if (cq) ibv_destroy_cq(cq);
    if (pd) ibv_dealloc_pd(pd);
    if (id) rdma_destroy_id(id);
    if (listen_id) rdma_destroy_id(listen_id);
    if (ec) rdma_destroy_event_channel(ec);
    return rc;
}

static int rdma_ack(struct rdma_cm_id *id, struct ibv_cq *cq,
                    rdma_slot_t acks[WINDOW], uint32_t expected, struct message *result_msg) {
    struct ibv_wc wc;
    if (rdma_poll_wc(cq, &wc, IBV_WC_RECV) ||
        wc.wr_id < 200 || wc.wr_id >= 200 + WINDOW ||
        wc.byte_len != sizeof(struct message)) return -1;
    int slot = (int)(wc.wr_id - 200);
    struct message m;
    memcpy(&m, acks[slot].buffer, sizeof(m));
    if (m.magic != MAGIC || m.type != expected) return -1;
    if (result_msg) *result_msg = m;
    return rdma_slot_recv(id, &acks[slot], 200 + slot);
}
static int rdma_control(struct rdma_cm_id *id, struct ibv_cq *cq, uint32_t type) {
    conn_manager_t *cm = id->context;
    struct message m = {.magic = MAGIC, .type = type};
    memcpy(cm->sbuffer, &m, sizeof(m));
    struct ibv_wc wc;
    return rdma_post_send(id, sizeof(m)) || rdma_poll_wc(cq, &wc, IBV_WC_SEND);
}
static int rdma_data(struct rdma_cm_id *id, struct ibv_cq *cq,
                     rdma_slot_t sends[WINDOW], rdma_slot_t acks[WINDOW],
                     uint64_t deadline) {
    uint64_t sent = 0, outstanding = 0;
    while (now_ns() < deadline || outstanding) {
        /* 先把窗口填满：连续 post 后立即返回，不等任何完成，让多个 WR 同时在途 */
        while (now_ns() < deadline && outstanding < WINDOW) {
            int slot = (int)(sent % WINDOW);
            if (rdma_slot_send(id, &sends[slot], sizeof(struct message) + BLOCK_SIZE,
                               300 + slot, 0)) return -1;
            ++sent; ++outstanding;
        }
        /* 窗口满或计时到：收一个 ACK 释放一个槽位。
         * 收到 ACK 说明对端已收下这块数据，源缓冲可以安全复用 */
        if (outstanding) {
            if (rdma_ack(id, cq, acks, MSG_ACK, NULL)) return -1;
            --outstanding;
        }
    }
    return 0;
}
static int rdma_client(const char *ip) {
    int rc = -1;
    struct rdma_event_channel *ec = rdma_create_event_channel();
    struct rdma_cm_id *id = NULL;
    struct ibv_pd *pd = NULL;
    struct ibv_cq *cq = NULL;
    rdma_slot_t sends[WINDOW] = {0}, acks[WINDOW] = {0};
    if (!ec || rdma_create_id(ec, &id, NULL, RDMA_PS_TCP)) goto out;
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(RDMA_SERVER_PORT)};
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1 ||
        rdma_resolve_addr(id, NULL, (void *)&addr, RDMA_TIMEOUT_MS) ||
        wait_event(ec, RDMA_CM_EVENT_ADDR_RESOLVED, NULL) ||
        rdma_resolve_route(id, RDMA_TIMEOUT_MS) ||
        wait_event(ec, RDMA_CM_EVENT_ROUTE_RESOLVED, NULL) ||
        rdma_create_queuepair(id, &pd, &cq) || rdma_alloc_buffer(id, pd)) goto out;
    for (int i = 0; i < WINDOW; ++i) {
        if (rdma_slot_init(pd, &sends[i], sizeof(struct message) + BLOCK_SIZE, 0) ||
            rdma_slot_init(pd, &acks[i], sizeof(struct message), 1) ||
            rdma_slot_recv(id, &acks[i], 200 + i)) goto out;
        struct message m = {.magic = MAGIC, .type = MSG_DATA, .length = BLOCK_SIZE};
        memcpy(sends[i].buffer, &m, sizeof(m));
        memset(sends[i].buffer + sizeof(m), 0x5a, BLOCK_SIZE);
    }
    struct rdma_conn_param param = {.retry_count = 7};
    if (rdma_connect(id, &param) || wait_event(ec, RDMA_CM_EVENT_ESTABLISHED, NULL)) goto out;
    if (rdma_data(id, cq, sends, acks, now_ns() + WARMUP_SECONDS * 1000000000ull) ||
        rdma_control(id, cq, MSG_START) ||
        rdma_ack(id, cq, acks, MSG_START_ACK, NULL) ||
        rdma_data(id, cq, sends, acks, now_ns() + MEASURE_SECONDS * 1000000000ull) ||
        rdma_control(id, cq, MSG_STOP)) goto out;
    struct message reply;
    if (rdma_ack(id, cq, acks, MSG_RESULT, &reply) || reply.nanos == 0) goto out;
    result("rdma", "client", reply.bytes, reply.nanos);
    rc = 0;
out:
    if (rc) fprintf(stderr, "rdma client failed\n");
    if (id && id->qp) rdma_destroy_qp(id);
    for (int i = 0; i < WINDOW; ++i) { rdma_slot_free(&sends[i]); rdma_slot_free(&acks[i]); }
    if (id && id->context) rdma_free_buffer(id);
    if (cq) ibv_destroy_cq(cq);
    if (pd) ibv_dealloc_pd(pd);
    if (id) rdma_destroy_id(id);
    if (ec) rdma_destroy_event_channel(ec);
    return rc;
}

int main(int argc, char **argv) {
    if (argc < 3 ||
        (strcmp(argv[1], "tcp") && strcmp(argv[1], "sendfile") && strcmp(argv[1], "rdma")) ||
        (strcmp(argv[2], "server") && strcmp(argv[2], "client")) ||
        (!strcmp(argv[2], "client") && argc != 4) ||
        (!strcmp(argv[2], "server") && argc != 3)) {
        fprintf(stderr, "usage: %s tcp|sendfile|rdma server | %s tcp|sendfile|rdma client SERVER_IP\n",
                argv[0], argv[0]);
        return 2;
    }
    /* 0 = 普通 send/recv，1 = 内核零拷贝 sendfile，2 = RDMA */
    int mode = !strcmp(argv[1], "tcp") ? 0 : !strcmp(argv[1], "sendfile") ? 1 : 2;
    int server = !strcmp(argv[2], "server");
    int rc;
    if (mode == 0) {
        rc = server ? tcp_server() : tcp_client(argv[3]);
    } else if (mode == 1) {
        rc = server ? sf_server() : sf_client(argv[3]);
    } else {
        rc = server ? rdma_server() : rdma_client(argv[3]);
    }
    return rc ? 1 : 0;
}
