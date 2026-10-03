#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <unistd.h>

#define TEST_COUNT      50000
#define TOTAL_COUNT     100000
#define CHUNK_SIZE      1024

#define TIME_SUB_MS(tv1, tv2)  ((tv1.tv_sec - tv2.tv_sec) * 1000 + \
                                (tv1.tv_usec - tv2.tv_usec) / 1000)

/* ============================================================
 * TCP 连接
 * ============================================================ */
static int connect_tcpserver(const char *ip, unsigned short port) {
    int connfd = socket(AF_INET, SOCK_STREAM, 0);
    if (connfd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(port);

    /* ← 关键：用传入的 ip */
    if (inet_pton(AF_INET, ip, &servaddr.sin_addr) <= 0) {
        perror("inet_pton");
        close(connfd);
        return -1;
    }

    if (connect(connfd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
        perror("connect");
        close(connfd);
        return -1;
    }
    return connfd;
}

/* ============================================================
 * RESP 命令拼接
 * ============================================================ */
static int append_set(char *buf, int len, const char *key, const char *value) {
    len += sprintf(buf + len, "*1\r\n");
    len += sprintf(buf + len,
                   "*3\r\n$3\r\nSET\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n",
                   (int)strlen(key), key,
                   (int)strlen(value), value);
    return len;
}

static int append_get(char *buf, int len, const char *key) {
    len += sprintf(buf + len, "*1\r\n");
    len += sprintf(buf + len,
                   "*2\r\n$3\r\nGET\r\n$%d\r\n%s\r\n",
                   (int)strlen(key), key);
    return len;
}

/* 统计 \r\n 个数（响应条数） */
static int count_responses(const char *buf, int len) {
    int count = 0, i;
    for (i = 0; i < len - 1; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n') { count++; i++; }
    }
    return count;
}

/* ============================================================
 * 边发边收
 * ============================================================ */
static int send_and_recv(int connfd, const char *batch, int batch_len,
                         char *resp, int resp_cap, int expected_count) {
    int sent = 0, total = 0;

    while (sent < batch_len) {
        int chunk = batch_len - sent;
        if (chunk > CHUNK_SIZE) chunk = CHUNK_SIZE;

        int n = send(connfd, batch + sent, chunk, 0);
        if (n <= 0) { perror("send"); return -1; }
        sent += n;

        if (total < resp_cap - 1) {
            int n2 = recv(connfd, resp + total, resp_cap - total - 1, 0);
            if (n2 <= 0) { perror("recv"); return -1; }
            total += n2;
        }
    }

    while (count_responses(resp, total) < expected_count) {
        if (total >= resp_cap - 1) break;
        int n = recv(connfd, resp + total, resp_cap - total - 1, 0);
        if (n <= 0) break;
        total += n;
    }

    resp[total] = '\0';
    return total;
}

/* ============================================================
 * 插入 [start, start+count)
 * ============================================================ */
static int insert_range(int connfd, int start, int count) {
    int cap = count * 64;
    char *batch = malloc(cap);
    char *resp  = malloc(cap);
    if (!batch || !resp) { free(batch); free(resp); return -1; }

    int blen = 0, i;
    for (i = start; i < start + count; i++) {
        char key[32], value[32];
        snprintf(key, sizeof(key), "ps_%06d", i);
        snprintf(value, sizeof(value), "val_%06d", i);
        blen = append_set(batch, blen, key, value);
    }

    struct timeval tv_begin, tv_cur;
    gettimeofday(&tv_begin, NULL);

    int total = send_and_recv(connfd, batch, blen, resp, cap, count);

    gettimeofday(&tv_cur, NULL);
    int time_used = TIME_SUB_MS(tv_cur, tv_begin);

    int ok_count = 0;
    char *p = resp;
    for (i = 0; i < count; i++) {
        char *line_end = strstr(p, "\r\n");
        if (!line_end) break;
        *line_end = '\0';
        if (strcmp(p, "ok") == 0) ok_count++;
        p = line_end + 2;
    }

    printf("[Insert] range=[%d, %d) count=%d, resp=%d bytes, ok=%d, time=%d ms\n",
           start, start + count, count, total, ok_count, time_used);

    free(batch);
    free(resp);
    return ok_count;
}

/* ============================================================
 * 验证 [start, start+count)
 * ============================================================ */
static int verify_range(int connfd, int start, int count) {
    int cap = count * 64;
    char *batch = malloc(cap);
    char *resp  = malloc(cap);
    if (!batch || !resp) { free(batch); free(resp); return -1; }

    int blen = 0, i;
    for (i = start; i < start + count; i++) {
        char key[32];
        snprintf(key, sizeof(key), "ps_%06d", i);
        blen = append_get(batch, blen, key);
    }

    struct timeval tv_begin, tv_cur;
    gettimeofday(&tv_begin, NULL);

    send_and_recv(connfd, batch, blen, resp, cap, count);

    gettimeofday(&tv_cur, NULL);
    int time_used = TIME_SUB_MS(tv_cur, tv_begin);

    int pass = 0, fail = 0;
    char *p = resp;
    for (i = 0; i < count; i++) {
        char *line_end = strstr(p, "\r\n");
        if (!line_end) { fail++; break; }
        *line_end = '\0';

        char expected[32];
        snprintf(expected, sizeof(expected), "val_%06d", start + i);

        if (strcmp(p, expected) == 0) pass++;
        else {
            fail++;
            if (fail <= 10)
                printf("[Verify] MISMATCH key=ps_%06d: got '%s', expected '%s'\n",
                       start + i, p, expected);
        }
        p = line_end + 2;
    }

    printf("[Verify] range=[%d, %d) count=%d, pass=%d, fail=%d, time=%d ms\n",
           start, start + count, count, pass, fail, time_used);

    free(batch);
    free(resp);
    return fail;
}

/* ============================================================
 * main：3 个参数，4 个阶段
 *   mode 1: 连主机，插入前 5w
 *   mode 2: 连主机，插入后 5w
 *   mode 3: 连从机，验证 10w
 *   mode 4: 连任意一端，验证 [0, 5w) 或 [5w, 10w)（可选）
 * ============================================================ */
int main(int argc, char *argv[]) {
    if (argc != 4) {
        printf("Usage: %s <ip> <port> <mode>\n", argv[0]);
        printf("  mode 1: 连主机，插入前 5w 条 (ps_000000 ~ ps_049999)\n");
        printf("  mode 2: 连主机，插入后 5w 条 (ps_050000 ~ ps_099999)\n");
        printf("  mode 3: 连从机，验证 10w 条\n");
        return -1;
    }

    const char *ip = argv[1];
    int port = atoi(argv[2]);
    int mode = atoi(argv[3]);

    int connfd = connect_tcpserver(ip, port);
    if (connfd < 0) {
        printf("connect %s:%d failed\n", ip, port);
        return -1;
    }

    if (mode == 1) {
        printf("===== 阶段1: 向主机 %s:%d 插入前 5w 条 =====\n", ip, port);
        int ok = insert_range(connfd, 0, TEST_COUNT);
        printf("\n>>> 阶段1 完成（ok=%d）。请启动从机，等从机同步完成后执行 mode 2。\n", ok);
    }
    else if (mode == 2) {
        printf("===== 阶段3: 向主机 %s:%d 插入后 5w 条 =====\n", ip, port);
        int ok = insert_range(connfd, TEST_COUNT, TEST_COUNT);
        printf("\n>>> 阶段3 完成（ok=%d）。等从机同步后，执行 mode 3 验证。\n", ok);
    }
    else if (mode == 3) {
        printf("===== 阶段4: 从从机 %s:%d 验证 10w 条 =====\n", ip, port);
        int fail1 = verify_range(connfd, 0, TEST_COUNT);
        int fail2 = verify_range(connfd, TEST_COUNT, TEST_COUNT);

        printf("\n===== 结果 =====\n");
        if (fail1 == 0 && fail2 == 0)
            printf("==> ALL PASS (10w 条一致)\n");
        else
            printf("==> FAILED (前5w fail=%d, 后5w fail=%d)\n", fail1, fail2);
    }
    else {
        printf("Unknown mode: %d\n", mode);
    }

    close(connfd);
    return 0;
}