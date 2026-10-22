#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <unistd.h>
#include "kvstore.h"


#define TEST_COUNT      100000      // 总数据量 10w
#define PER_ENGINE      50000       // 每种数据结构 5w
#define ENGINE_COUNT    2           // 2 种引擎 (Hash / Skiptable)

#define CHUNK_SIZE 1024

int mode = 0;

int connect_tcpserver(const char *ip, unsigned short port) {
    int connfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(struct sockaddr_in));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(port);
    if (0 != connect(connfd, (struct sockaddr*)&servaddr, sizeof(struct sockaddr_in))) {
        perror("connect");
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

static int append_save(char *buf, int len) {
    len += sprintf(buf + len, "*1\r\n$4\r\nSAVE\r\n");
    return len;
}


/* ============================================================
 * mode 10~13：全量持久化(SAVE)不同频率下的 QPS
 *   共插入 TOTAL_INSERT 条 HSET，每 interval 条触发一次 SAVE
 *   mode 10: 1k/次   mode 11: 1w/次
 *   mode 12: 10w/次  mode 13: 100w/次(仅末尾一次)
 *   QPS(total) = 总条数 / (写入耗时 + SAVE 耗时)
 * ============================================================ */
#define TOTAL_INSERT    1000000     /* 100w */
#define BENCH_BATCH     1000        /* 每轮发送并收满响应的命令数 */

#define TIME_SUB_US(tv1, tv2) \
    ((double)((tv1.tv_sec - tv2.tv_sec) * 1000000LL + \
              (tv1.tv_usec - tv2.tv_usec)))

static int send_all(int connfd, const char *buf, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(connfd, buf + sent, len - sent, 0);
        if (n <= 0) { perror("send"); return -1; }
        sent += n;
    }
    return 0;
}

/* 接收一条完整响应 */
static int recv_one_resp(int connfd, char *out, int cap) {
    char buf[256];
    int len = 0;
    while (len < (int)sizeof(buf) - 1) {
        int n = recv(connfd, buf + len, sizeof(buf) - 1 - len, 0);
        if (n <= 0) { perror("recv"); return -1; }
        len += n;
        buf[len] = '\0';
        if (resp_parse_one(buf, buf + len, out, cap) != NULL)
            return 0;
    }
    return -1;
}

/* 收满 need 条响应；边解析边消费，防止缓冲区溢出；ok 累计 ":1" 条数 */
static int drain_responses(int connfd, char *resp, int cap, int *rlen,
                           int need, int *ok) {
    int got = 0;
    while (got < need) {
        if (*rlen >= cap - 1) return -1;
        int n = recv(connfd, resp + *rlen, cap - 1 - *rlen, 0);
        if (n <= 0) { perror("recv"); return -1; }
        *rlen += n;

        const char *p = resp, *end = resp + *rlen;
        char out[64];
        while (got < need) {
            const char *next = resp_parse_one(p, end, out, sizeof(out));
            if (!next) break;
            if (strcmp(out, "1") == 0) (*ok)++;
            got++;
            p = next;
        }
        int consumed = (int)(p - resp);
        if (consumed > 0) {
            memmove(resp, resp + consumed, *rlen - consumed);
            *rlen -= consumed;
            resp[*rlen] = '\0';
        }
    }
    return got;
}

/* 边发边收：每次 send CHUNK_SIZE 字节，发完立刻 recv */
static int send_and_recv(int connfd, const char *batch, int batch_len,
                         char *resp, int resp_cap, int expected_count) {
    int sent = 0;
    int total = 0;

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
static const char *engine_set_cmd[ENGINE_COUNT] = {
    "HSET",  /* Hash */
    "SSET",  /* Skiptable */
};

static const char *engine_get_cmd[ENGINE_COUNT] = {
    "HGET",  /* Hash */
    "SGET",  /* Skiptable */
};

static const char *engine_name[ENGINE_COUNT] = {
    "Hash", "Skiptable"
};

/* ============================================================
 * 阶段1：插入 10w 条 SET（2 引擎 × 5w）
 *
 * 每种引擎的数据：
 *   key = "{engine_prefix}_{i}"   i = 0..49999
 *   val = "val_{i}"
 * ============================================================ */
void insert_100k(int connfd) {
    int cap = TEST_COUNT * 80;
    char *batch = (char *)malloc(cap);
    char *resp  = (char *)malloc(cap);
    int blen = 0;
    int i, e;

    for (e = 0; e < ENGINE_COUNT; e++) {
        for (i = 0; i < PER_ENGINE; i++) {
            char key[64], value[64];
            snprintf(key, sizeof(key), "%s_%06d", engine_name[e], i);
            snprintf(value, sizeof(value), "val_%06d", i);
            blen = append_resp_cmd(batch, blen, engine_set_cmd[e], key, value);
        }

        printf("Insert progress: %d%% (%s done)\n", (e + 1) * 50, engine_name[e]);
        fflush(stdout);
    }

    printf("Total commands: %d, batch len: %d\n", TEST_COUNT, blen);
    send_and_recv(connfd, batch, blen, resp, cap, TEST_COUNT);
    printf("Insert 100k done\n");

    free(batch);
    free(resp);
}

/* ============================================================
 * 阶段2：验证 10w 条 GET（2 引擎 × 5w）
 * ============================================================ */
void verify_100k(int connfd) {
    int cap = TEST_COUNT * 80;
    char *batch = (char *)malloc(cap);
    char *resp  = (char *)malloc(cap);
    int blen = 0;
    int i, e;
    int pass = 0, fail = 0;

    for (e = 0; e < ENGINE_COUNT; e++) {
        for (i = 0; i < PER_ENGINE; i++) {
            char key[64];
            snprintf(key, sizeof(key), "%s_%06d", engine_name[e], i);
            blen = append_resp_cmd(batch, blen, engine_get_cmd[e], key, NULL);
        }

        printf("Verify progress: %d%% (%s done)\n", (e + 1) * 50, engine_name[e]);
        fflush(stdout);
    }

    send_and_recv(connfd, batch, blen, resp, cap, TEST_COUNT);

    /* 逐条验证（bulk 响应：$len\r\nval\r\n） */
    const char *p = resp, *end = resp + strlen(resp);
    char got[256];
    for (e = 0; e < ENGINE_COUNT; e++) {
        for (i = 0; i < PER_ENGINE; i++) {
            const char *next = resp_parse_one(p, end, got, sizeof(got));
            if (!next) { fail += PER_ENGINE - i; break; }

            char expected[64];
            snprintf(expected, sizeof(expected), "val_%06d", i);

            if (strcmp(got, expected) == 0) pass++;
            else {
                fail++;
                if (fail <= 10)
                    printf("MISMATCH e=%s i=%d: got '%s', expected '%s'\n",
                           engine_name[e], i, got, expected);
            }
            p = next;
        }
    }

    printf("Verify done: pass=%d, fail=%d\n", pass, fail);
    if (fail == 0 && pass == TEST_COUNT)
        printf("==> TEST PASS\n");
    else
        printf("==> TEST FAILED\n");

    free(batch);
    free(resp);
}

/* ============================================================
 * 发送 SAVE（期望响应 +OK）
 * ============================================================ */
void send_save(int connfd) {
    char buf[32];
    char resp[64] = {0};
    int blen = append_save(buf, 0);
    send(connfd, buf, blen, 0);
    recv(connfd, resp, sizeof(resp) - 1, 0);
    printf("SAVE: %s", resp);
}

void bench_save_qps(int connfd, int interval, const char *set_cmd) {
    int cap  = BENCH_BATCH * 96;
    int rcap = 65536;
    char *batch = (char*)malloc(cap);
    char *resp  = (char*)malloc(rcap);
    if (!batch || !resp) { free(batch); free(resp); return; }

    int rlen = 0, ok = 0, inserted = 0;
    long saves = 0;
    double insert_us = 0, save_us = 0;
    struct timeval ta0, ta1, t0, t1, ts, te;

    gettimeofday(&ta0, NULL);

    while (inserted < TOTAL_INSERT) {
        int n = BENCH_BATCH;
        if (inserted + n > TOTAL_INSERT) n = TOTAL_INSERT - inserted;

        int blen = 0, i;
        for (i = 0; i < n; i++) {
            char key[32], value[32];
            snprintf(key, sizeof(key), "snap_%07d", inserted + i);
            snprintf(value, sizeof(value), "val_%07d", inserted + i);
            
            // 【修改点】使用传入的 set_cmd (HSET 或 SSET)
            blen = append_resp_cmd(batch, blen, set_cmd, key, value);
        }

        gettimeofday(&t0, NULL);
        if (send_all(connfd, batch, blen) < 0) break;
        if (drain_responses(connfd, resp, rcap, &rlen, n, &ok) < 0) break;
        gettimeofday(&t1, NULL);
        insert_us += TIME_SUB_US(t1, t0);
        inserted += n;

        if (inserted % 100000 == 0)
            printf("  progress: inserted=%d (%d%%)\n",
                   inserted, inserted / (TOTAL_INSERT / 100));

        if (inserted % interval == 0 || inserted == TOTAL_INSERT) {
            char sbuf[32], out[64];
            int slen = append_save(sbuf, 0);
            gettimeofday(&ts, NULL);
            if (send_all(connfd, sbuf, slen) < 0) break;
            if (recv_one_resp(connfd, out, sizeof(out)) < 0) break;
            gettimeofday(&te, NULL);
            if (strcmp(out, "OK") != 0) {
                printf("[ERROR] SAVE rejected: %s (snap.enable=true?)\n", out);
                free(batch); free(resp);
                return;
            }
            save_us += TIME_SUB_US(te, ts);
            saves++;
        }
    }

    gettimeofday(&ta1, NULL);
    double total_us = TIME_SUB_US(ta1, ta0);

    // 精简后的打印
    printf("\n===== 测试结果 =====\n");
    printf("引擎: %s | SAVE间隔: %d\n", set_cmd, interval);
    printf("写入耗时: %.3f s | 总耗时: %.3f s\n", insert_us / 1000000.0, total_us / 1000000.0);
    printf("SAVE次数: %ld | 平均SAVE耗时: %.2f ms\n", saves, saves ? save_us / saves / 1000.0 : 0.0);
    printf("QPS(写入): %.0f req/s\n", inserted * 1000000.0 / insert_us);
    printf("QPS(总计): %.0f req/s\n", inserted * 1000000.0 / total_us);

    // 异常提示保留
    if (ok < inserted) {
        printf(">>> 警告: %d 条返回非 :1（key 已存在，请清数据后重测）\n", inserted - ok);
    }

    free(batch);
    free(resp);
}


/* ============================================================
 * main
 * ============================================================ */
int main(int argc, char *argv[]) {
     if (argc != 4 && argc != 5) { 
        printf("Usage: %s <ip> <port> <mode> [h|s]\n", argv[0]);
        printf("  [h|s]: h 代表 Hash(HSET), s 代表 Skiptable(SSET), 默认 h\n");
        printf("  mode 1: AOF  - 插入 10w\n");
        printf("  mode 2: AOF  - 验证 10w\n");
        printf("  mode 3: SNAP - 插入 10w + SAVE\n");
        printf("  mode 4: SNAP - 验证 10w\n");
        printf("  mode 10: 插入 100w，每 1k 条 SAVE 一次，统计 QPS\n");
        printf("  mode 11: 插入 100w，每 1w 条 SAVE 一次，统计 QPS\n");
        printf("  mode 12: 插入 100w，每 10w 条 SAVE 一次，统计 QPS\n");
        printf("  mode 13: 插入 100w，末尾 SAVE 一次，统计 QPS\n");
        return -1;
    }

    char *ip = argv[1];
    int port = atoi(argv[2]);
    mode = atoi(argv[3]);

    // 【修改点】解析第 4 个参数控制数据结构
    const char *set_cmd = "HSET"; // 默认 Hash
    if (argc == 5) {
        if (argv[4][0] == 's' || argv[4][0] == 'S') {
            set_cmd = "SSET"; // 跳表
        } else if (argv[4][0] == 'h' || argv[4][0] == 'H') {
            set_cmd = "HSET";
        } else {
            printf("[ERROR] 未知的数据结构参数: %s，请使用 h 或 s\n", argv[4]);
            return -1;
        }
    }

    if ((mode < 1 || mode > 4) && (mode < 10 || mode > 13)) {
        printf("[ERROR] Unknown mode: %d\n", mode);
        return -1;
    }

    /* ============================================================
     * 连接服务端
     * ============================================================ */
    int connfd = connect_tcpserver(ip, port);
    if (connfd < 0) {
        printf("Connect failed\n");
        return -1;
    }

    if (mode == 1) {
        printf("===== AOF 阶段1: 插入 10w =====\n");
        insert_100k(connfd);
        printf(">>> 杀掉服务端，重启后执行 mode 2\n");
    }
    else if (mode == 2) {
        printf("===== AOF 阶段2: 验证 10w =====\n");
        verify_100k(connfd);
    }
    else if (mode == 3) {
        printf("===== SNAP 阶段1: 插入 10w + SAVE =====\n");
        insert_100k(connfd);
        send_save(connfd);
        printf(">>> 杀掉服务端，重启后执行 mode 4\n");
    }
    else if (mode == 4) {
        printf("===== SNAP 阶段2: 验证 10w =====\n");
        verify_100k(connfd);
    }
    else if (mode >= 10 && mode <= 13) {
        static const int intervals[4] = {1000, 10000, 100000, 1000000};
        int interval = intervals[mode - 10];
        
        // 【修改点】把 set_cmd 传进去
        printf("===== mode %d: 引擎 %s, 每 %d 条 SAVE 一次, 共插入 %d 条 =====\n",
               mode, set_cmd, interval, TOTAL_INSERT);
        bench_save_qps(connfd, interval, set_cmd);
    }
    close(connfd);
    return 0;
}
