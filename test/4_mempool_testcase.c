#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <unistd.h>

/* 内存池冒烟测试（redis-cli 扁平 RESP 格式）
 *
 * 通过 Hash / Skiptable 两个引擎做一轮 插 → 查 → 删 的完整循环，
 * 用于验证内存池在大量键值分配/释放场景下的正确性。
 * 命令：HSET/SSET key value、HGET/SGET key、HDEL/SDEL key
 */

#define PER_ENGINE      20000       /* 每引擎 2w */
#define TOTAL           (PER_ENGINE * 2)
#define ENGINE_COUNT    2
#define CHUNK_SIZE      1024

/* ============================================================
 * TCP 连接
 * ============================================================ */
static int connect_tcpserver(const char *ip, unsigned short port) {
    int connfd = socket(AF_INET, SOCK_STREAM, 0);
    if (connfd < 0) { perror("socket"); return -1; }

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(port);
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
 * RESP 响应解析（redis-cli 扁平格式）
 * ============================================================ */
static const char *resp_parse_one(const char *p, const char *end,
                                  char *out, int cap) {
    if (!p || p >= end) return NULL;
    switch (*p) {
    case '+':
    case '-':
    case ':': {
        const char *e = strstr(p, "\r\n");
        if (!e || e + 2 > end) return NULL;
        int n = (int)(e - p) - 1;
        if (n > cap - 1) n = cap - 1;
        memcpy(out, p + 1, n);
        out[n] = '\0';
        return e + 2;
    }
    case '$': {
        const char *e = strstr(p, "\r\n");
        if (!e || e + 2 > end) return NULL;
        int len = atoi(p + 1);
        if (len < 0) {                    /* $-1：空值 */
            strcpy(out, "nil");
            return e + 2;
        }
        const char *data = e + 2;
        if (data + len + 2 > end) return NULL;   /* 数据未收完 */
        if (len > cap - 1) len = cap - 1;
        memcpy(out, data, len);
        out[len] = '\0';
        return data + len + 2;
    }
    default:
        return NULL;
    }
}

/* 统计缓冲区中完整 RESP 响应的条数 */
static int resp_count(const char *buf, int len) {
    int count = 0;
    const char *p = buf, *end = buf + len;
    char tmp[1024];
    while (p && p < end) {
        const char *next = resp_parse_one(p, end, tmp, sizeof(tmp));
        if (!next) break;
        count++;
        p = next;
    }
    return count;
}

/* ============================================================
 * RESP 命令拼接（redis-cli 扁平格式：一条命令 = 一个数组）
 * ============================================================ */
static int append_resp_cmd(char *buf, int len, const char *cmd,
                           const char *key, const char *value) {
    if (value) {
        len += sprintf(buf + len,
                       "*3\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n",
                       (int)strlen(cmd), cmd,
                       (int)strlen(key), key,
                       (int)strlen(value), value);
    } else {
        len += sprintf(buf + len,
                       "*2\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n",
                       (int)strlen(cmd), cmd,
                       (int)strlen(key), key);
    }
    return len;
}

/* 边发边收 */
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

    while (resp_count(resp, total) < expected_count) {
        if (total >= resp_cap - 1) break;
        int n = recv(connfd, resp + total, resp_cap - total - 1, 0);
        if (n <= 0) break;
        total += n;
    }

    resp[total] = '\0';
    return total;
}

/* ============================================================
 * 引擎命令前缀（仅 Hash / Skiptable）
 * ============================================================ */
static const char *engine_set_cmd[ENGINE_COUNT] = { "HSET", "SSET" };
static const char *engine_get_cmd[ENGINE_COUNT] = { "HGET", "SGET" };
static const char *engine_del_cmd[ENGINE_COUNT] = { "HDEL", "SDEL" };
static const char *engine_name[ENGINE_COUNT]    = { "Hash", "Skiptable" };

/* ============================================================
 * 阶段 1：插入（HSET/SSET），期望 :1
 * ============================================================ */
static int smoke_insert(int connfd) {
    int cap = TOTAL * 80;
    char *batch = malloc(cap);
    char *resp  = malloc(cap);
    int blen = 0, e, i, pass = 0, fail = 0;

    for (e = 0; e < ENGINE_COUNT; e++) {
        for (i = 0; i < PER_ENGINE; i++) {
            char key[64], value[64];
            snprintf(key, sizeof(key), "mp_%s_%06d", engine_name[e], i);
            snprintf(value, sizeof(value), "val_%06d", i);
            blen = append_resp_cmd(batch, blen, engine_set_cmd[e], key, value);
        }
    }

    send_and_recv(connfd, batch, blen, resp, cap, TOTAL);

    const char *p = resp, *end = resp + strlen(resp);
    char got[64];
    for (i = 0; i < TOTAL; i++) {
        const char *next = resp_parse_one(p, end, got, sizeof(got));
        if (!next) { fail += TOTAL - i; break; }
        if (strcmp(got, "1") == 0) pass++;
        else fail++;
        p = next;
    }

    printf("[Mempool Smoke] insert %d: pass=%d fail=%d\n", TOTAL, pass, fail);
    free(batch);
    free(resp);
    return fail;
}

/* ============================================================
 * 阶段 2：验证（HGET/SGET），期望 val_%06d
 * ============================================================ */
static int smoke_verify(int connfd) {
    int cap = TOTAL * 80;
    char *batch = malloc(cap);
    char *resp  = malloc(cap);
    int blen = 0, e, i, pass = 0, fail = 0;

    for (e = 0; e < ENGINE_COUNT; e++) {
        for (i = 0; i < PER_ENGINE; i++) {
            char key[64];
            snprintf(key, sizeof(key), "mp_%s_%06d", engine_name[e], i);
            blen = append_resp_cmd(batch, blen, engine_get_cmd[e], key, NULL);
        }
    }

    send_and_recv(connfd, batch, blen, resp, cap, TOTAL);

    const char *p = resp, *end = resp + strlen(resp);
    char got[64], exp[64];
    for (e = 0; e < ENGINE_COUNT; e++) {
        for (i = 0; i < PER_ENGINE; i++) {
            const char *next = resp_parse_one(p, end, got, sizeof(got));
            if (!next) { fail += PER_ENGINE - i; break; }
            snprintf(exp, sizeof(exp), "val_%06d", i);
            if (strcmp(got, exp) == 0) pass++;
            else fail++;
            p = next;
        }
    }

    printf("[Mempool Smoke] verify %d: pass=%d fail=%d\n", TOTAL, pass, fail);
    free(batch);
    free(resp);
    return fail;
}

/* ============================================================
 * 阶段 3：删除（HDEL/SDEL），期望 :1
 * ============================================================ */
static int smoke_delete(int connfd) {
    int cap = TOTAL * 80;
    char *batch = malloc(cap);
    char *resp  = malloc(cap);
    int blen = 0, e, i, pass = 0, fail = 0;

    for (e = 0; e < ENGINE_COUNT; e++) {
        for (i = 0; i < PER_ENGINE; i++) {
            char key[64];
            snprintf(key, sizeof(key), "mp_%s_%06d", engine_name[e], i);
            blen = append_resp_cmd(batch, blen, engine_del_cmd[e], key, NULL);
        }
    }

    send_and_recv(connfd, batch, blen, resp, cap, TOTAL);

    const char *p = resp, *end = resp + strlen(resp);
    char got[64];
    for (i = 0; i < TOTAL; i++) {
        const char *next = resp_parse_one(p, end, got, sizeof(got));
        if (!next) { fail += TOTAL - i; break; }
        if (strcmp(got, "1") == 0) pass++;
        else fail++;
        p = next;
    }

    printf("[Mempool Smoke] delete %d: pass=%d fail=%d\n", TOTAL, pass, fail);
    free(batch);
    free(resp);
    return fail;
}

/* ============================================================
 * main
 * ============================================================ */
int main(int argc, char *argv[]) {
    if (argc != 4) {
        printf("Usage: %s <ip> <port> <mode>\n", argv[0]);
        printf("  mode 1: 插入 %d 条 (2 引擎 x %d)\n", TOTAL, PER_ENGINE);
        printf("  mode 2: 验证 %d 条\n", TOTAL);
        printf("  mode 3: 删除 %d 条\n", TOTAL);
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

    int fail;
    if (mode == 1) {
        printf("===== Mempool Smoke: insert =====\n");
        fail = smoke_insert(connfd);
    }
    else if (mode == 2) {
        printf("===== Mempool Smoke: verify =====\n");
        fail = smoke_verify(connfd);
    }
    else if (mode == 3) {
        printf("===== Mempool Smoke: delete =====\n");
        fail = smoke_delete(connfd);
    }
    else {
        printf("Unknown mode: %d\n", mode);
        close(connfd);
        return -1;
    }

    close(connfd);
    printf(fail == 0 ? "==> TEST PASS\n" : "==> TEST FAILED\n");
    return fail == 0 ? 0 : 1;
}
