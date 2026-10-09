#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <unistd.h>

#define CHUNK_SIZE      1024
#define MAX_RESP        (32 * 1024 * 1024)
#define BATCH_COUNT     50000

#define TIME_SUB_MS(tv1, tv2)  ((tv1.tv_sec - tv2.tv_sec) * 1000 + \
                                (tv1.tv_usec - tv2.tv_usec) / 1000)

int mode = 0;

/* ============================================================
 * TCP 连接
 * ============================================================ */
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
 * RESP 命令拼接(外层 *1)
 * ============================================================ */
static int append_resp_cmd(char *buf, int len, const char *cmd,
                           const char *key, const char *value) {
    len += sprintf(buf + len, "*1\r\n");
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

/* 统计响应条数 */
static int count_responses(const char *buf, int len) {
    int count = 0, i;
    for (i = 0; i < len - 1; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n') { count++; i++; }
    }
    return count;
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
 * 引擎命令前缀
 * ============================================================ */
static const char *engine_set_cmd[4]   = { "SET",  "RSET", "HSET", "SSET" };
static const char *engine_get_cmd[4]   = { "GET",  "RGET", "HGET", "SGET" };
static const char *engine_del_cmd[4]   = { "DEL",  "RDEL", "HDEL", "SDEL" };
static const char *engine_mod_cmd[4]   = { "MOD",  "RMOD", "HMOD", "SMOD" };
static const char *engine_exist_cmd[4] = { "EXIST", "REXIST", "HEXIST", "SEXIST" };
static const char *engine_name[4]      = { "Array", "Rbtree", "Hash", "Skiptable" };

/* ============================================================
 * Part 1：四种模式(单条/管道 x 交叉/集中)
 * ============================================================ */
#define MIX_LOOP      20000
#define MIX_CMDS      5
#define MIX_TOTAL     (MIX_LOOP * MIX_CMDS)
#define STAGE_COUNT   20000
#define VERIFY_CHUNK  1000

static void expected_of(int op, int i, char *out, int n) {
    if (op == 0)      snprintf(out, n, "ok");            /* SET */
    else if (op == 1) snprintf(out, n, "val_%06d", i);   /* GET */
    else if (op == 2) snprintf(out, n, "ok");            /* MOD */
    else if (op == 3) snprintf(out, n, "exist");         /* EXIST */
    else              snprintf(out, n, "ok");            /* DEL */
}

static int verify_resp_lines(char *resp, int op, int i_start, int count,
                             int *pass, int *fail) {
    char *p = resp;
    int k;
    for (k = 0; k < count; k++) {
        char *e = strstr(p, "\r\n");
        if (!e) { *fail += count - k; break; }
        *e = '\0';
        if (p[0] != '$' && p[0] != '+' && p[0] != '-' &&
            p[0] != ':' && p[0] != '*') {
            char exp[64];
            expected_of(op, i_start + k, exp, sizeof(exp));
            if (strcmp(p, exp) == 0) (*pass)++;
            else {
                (*fail)++;
                if (*fail <= 5)
                    printf("[Verify] MISMATCH op=%d i=%d: got '%s', exp '%s'\n",
                           op, i_start + k, p, exp);
            }
        }
        p = e + 2;
    }
    return 0;
}

/* ============================================================
 * 工具：循环 recv 直到收到完整一行（含 \r\n）
 * 返回收到的字节数，-1 表示连接断开
 * ============================================================ */
static int recv_one_line(int connfd, char *resp, int cap) {
    int total = 0;
    while (total < cap - 1) {
        int n = recv(connfd, resp + total, cap - 1 - total, 0);
        if (n <= 0) {
            resp[total] = '\0';
            return -1;
        }
        total += n;
        resp[total] = '\0';
        if (strstr(resp, "\r\n")) break;
    }
    return total;
}

/* ============================================================
 * 工具：按"发送顺序"校验响应
 *   resp 里的响应顺序 = 发送顺序：
 *     k = 0..total-1
 *     op  = k % MIX_CMDS
 *     idx = r_start + k / MIX_CMDS
 * ============================================================ */
static int verify_resp_seq(char *resp, int r_start, int total,
                           int *pass, int *fail) {
    char *p = resp;
    int k;
    for (k = 0; k < total; k++) {
        char *e = strstr(p, "\r\n");
        if (!e) { *fail += total - k; break; }
        *e = '\0';

        int op  = k % MIX_CMDS;
        int idx = r_start + k / MIX_CMDS;

        /* 跳过 RESP 类型前缀（$ + - : *） */
        if (p[0] != '$' && p[0] != '+' && p[0] != '-' &&
            p[0] != ':' && p[0] != '*') {
            char exp[64];
            expected_of(op, idx, exp, sizeof(exp));
            if (strcmp(p, exp) == 0) {
                (*pass)++;
            } else {
                (*fail)++;
                if (*fail <= 5)
                    printf("[Verify] MISMATCH op=%d idx=%d: got '%s', exp '%s'\n",
                           op, idx, p, exp);
            }
        }
        p = e + 2;
    }
    return 0;
}

static int verify_one_resp(char *resp, int op, int i, int *pass, int *fail) {
    char exp[64];
    expected_of(op, i, exp, sizeof(exp));
    if (op == 1) {
        if (strncmp(resp, "val_", 4) == 0) (*pass)++;
        else goto mismatch;
    } else if (op == 2) {
        if (strncmp(resp, "mod_", 4) == 0 || strcmp(resp, "ok") == 0) (*pass)++;
        else goto mismatch;
    } else if (op == 3) {
        if (strcmp(resp, "exist") == 0 || strcmp(resp, "1") == 0) (*pass)++;
        else goto mismatch;
    } else {
        if (strcmp(resp, "ok") == 0) (*pass)++;
        else goto mismatch;
    }
    return 0;
mismatch:
    (*fail)++;
    if (*fail <= 5)
        printf("[Verify] MISMATCH op=%d i=%d: got '%.32s', exp '%s'\n",
               op, i, resp, exp);
    return 0;
}

static int build_one_cmd(char *buf, int engine_idx, int op, int i) {
    char key[64], value[64], value2[64];
    const char *cmdname;
    int nargs;

    snprintf(key, sizeof(key), "%s_%06d", engine_name[engine_idx], i);
    snprintf(value, sizeof(value), "val_%06d", i);
    snprintf(value2, sizeof(value2), "mod_%06d", i);

    switch (op) {
    case 0: cmdname = engine_set_cmd[engine_idx];   nargs = 3; break;
    case 1: cmdname = engine_get_cmd[engine_idx];   nargs = 2; break;
    case 2: cmdname = engine_mod_cmd[engine_idx];   nargs = 3; break;
    case 3: cmdname = engine_exist_cmd[engine_idx]; nargs = 2; break;
    default: cmdname = engine_del_cmd[engine_idx];  nargs = 2; break;
    }

    if (nargs == 3)
        return sprintf(buf, "*1\r\n*3\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n",
                       (int)strlen(cmdname), cmdname,
                       (int)strlen(key), key,
                       (int)strlen(op == 2 ? value2 : value),
                       op == 2 ? value2 : value);
    return sprintf(buf, "*1\r\n*2\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n",
                   (int)strlen(cmdname), cmdname,
                   (int)strlen(key), key);
}


/* mode1：单条收发 + 交叉 */
void mixed_single(int connfd, int engine_idx, int loop) {
    int r, op;
    char cmd[256];
    char resp[256];
    int pass = 0, fail = 0;

    struct timeval tv_begin, tv_cur;
    gettimeofday(&tv_begin, NULL);

    for (r = 0; r < loop; r++) {
        for (op = 0; op < MIX_CMDS; op++) {
            int cmd_len = build_one_cmd(cmd, engine_idx, op, r);
            send(connfd, cmd, cmd_len, 0);

            /* ★ 循环收到完整一行 */
            if (recv_one_line(connfd, resp, sizeof(resp)) < 0) {
                printf("[Mixed single] recv failed at r=%d op=%d\n", r, op);
                goto done;
            }
            resp[strcspn(resp, "\r\n")] = '\0';
            verify_one_resp(resp, op, r, &pass, &fail);
        }
    }

done:
    gettimeofday(&tv_cur, NULL);
    int time_used = TIME_SUB_MS(tv_cur, tv_begin);
    int total = loop * MIX_CMDS;

    printf("[Mixed  single] %-10s total=%d: %d ms, qps=%d, pass=%d fail=%d\n",
           engine_name[engine_idx], total, time_used,
           total * 1000 / (time_used > 0 ? time_used : 1), pass, fail);
}
/* mode2：单条收发 + 集中 */
void staged_single(int connfd, int engine_idx, int count) {
    int op, i;
    char cmd[256];
    char resp[256];
    int pass = 0, fail = 0;

    struct timeval tv_begin, tv_cur;
    gettimeofday(&tv_begin, NULL);

    for (op = 0; op < MIX_CMDS; op++) {
        for (i = 0; i < count; i++) {
            int cmd_len = build_one_cmd(cmd, engine_idx, op, i);
            send(connfd, cmd, cmd_len, 0);

            /* ★ 循环收到完整一行 */
            if (recv_one_line(connfd, resp, sizeof(resp)) < 0) {
                printf("[Staged single] recv failed at op=%d i=%d\n", op, i);
                goto done;
            }
            resp[strcspn(resp, "\r\n")] = '\0';
            verify_one_resp(resp, op, i, &pass, &fail);
        }
    }

done:
    gettimeofday(&tv_cur, NULL);
    int time_used = TIME_SUB_MS(tv_cur, tv_begin);
    int total = count * MIX_CMDS;

    printf("[Staged single] %-10s total=%d: %d ms, qps=%d, pass=%d fail=%d\n",
           engine_name[engine_idx], total, time_used,
           total * 1000 / (time_used > 0 ? time_used : 1), pass, fail);
}
/* mode3：管道化 + 交叉（分块防死锁，逐块按顺序校验） */
void mixed_ops(int connfd, int engine_idx, int loop) {
    int r, op, k;
    int total = loop * MIX_CMDS;
    int pass = 0, fail = 0;
    int cap = VERIFY_CHUNK * MIX_CMDS * 80;
    char *batch = malloc(cap);
    char *resp  = malloc(cap);
    if (!batch || !resp) { free(batch); free(resp); return; }

    struct timeval tv_begin, tv_cur;
    gettimeofday(&tv_begin, NULL);

    for (r = 0; r < loop; r += VERIFY_CHUNK) {
        int n = loop - r < VERIFY_CHUNK ? loop - r : VERIFY_CHUNK;
        int blen = 0;

        /* ★ 发送顺序：k 轮内 op = 0..4，即 SET, GET, MOD, EXIST, DEL 交错 */
        for (k = 0; k < n; k++)
            for (op = 0; op < MIX_CMDS; op++)
                blen += build_one_cmd(batch + blen, engine_idx, op, r + k);

        send_and_recv(connfd, batch, blen, resp, cap, n * MIX_CMDS);

        /* ★ 响应顺序 = 发送顺序：k % 5 = op，k / 5 = 轮内偏移 */
        int pc = 0, fc = 0;
        verify_resp_seq(resp, r, n * MIX_CMDS, &pc, &fc);
        pass += pc; fail += fc;
    }

    gettimeofday(&tv_cur, NULL);
    int time_used = TIME_SUB_MS(tv_cur, tv_begin);

    printf("[Mixed  5ops] %-10s loop=%d total=%d: %d ms, qps=%d, pass=%d fail=%d\n",
           engine_name[engine_idx], loop, total, time_used,
           total * 1000 / (time_used > 0 ? time_used : 1), pass, fail);

    free(batch);
    free(resp);
}
/* mode4：管道化 + 集中（分块防死锁，逐块校验） */
void staged_ops(int connfd, int engine_idx, int count) {
    int op, i, k;
    int total = count * MIX_CMDS;
    int pass = 0, fail = 0;
    int cap = VERIFY_CHUNK * 80;
    char *batch = malloc(cap);
    char *resp  = malloc(cap);
    if (!batch || !resp) { free(batch); free(resp); return; }

    struct timeval tv_begin, tv_cur;
    gettimeofday(&tv_begin, NULL);

    for (op = 0; op < MIX_CMDS; op++) {
        for (i = 0; i < count; i += VERIFY_CHUNK) {
            int n = count - i < VERIFY_CHUNK ? count - i : VERIFY_CHUNK;
            int blen = 0;
            for (k = 0; k < n; k++)
                blen += build_one_cmd(batch + blen, engine_idx, op, i + k);

            send_and_recv(connfd, batch, blen, resp, cap, n);

            /* op 固定，idx 从 i 开始，n 条连续 */
            int pc = 0, fc = 0;
            verify_resp_lines(resp, op, i, n, &pc, &fc);
            pass += pc; fail += fc;
        }
    }

    gettimeofday(&tv_cur, NULL);
    int time_used = TIME_SUB_MS(tv_cur, tv_begin);

    printf("[Staged 5ops] %-10s %d x %d: %d ms, qps=%d, pass=%d fail=%d\n",
           engine_name[engine_idx], MIX_CMDS, count, time_used,
           total * 1000 / (time_used > 0 ? time_used : 1), pass, fail);

    free(batch);
    free(resp);
}


/* ============================================================
 * Part 1：批量命令测试
 * ============================================================ */
void batch_set(int connfd, int engine_idx, int count) {
    int cap = count * 80;
    char *batch = malloc(cap);
    char *resp  = malloc(cap);
    int blen = 0;
    int i;

    for (i = 0; i < count; i++) {
        char key[64], value[64];
        snprintf(key, sizeof(key), "%s_%06d", engine_name[engine_idx], i);
        snprintf(value, sizeof(value), "val_%06d", i);
        blen = append_resp_cmd(batch, blen, engine_set_cmd[engine_idx], key, value);
    }

    struct timeval tv_begin, tv_cur;
    gettimeofday(&tv_begin, NULL);

    send_and_recv(connfd, batch, blen, resp, cap, count);

    gettimeofday(&tv_cur, NULL);
    int time_used = TIME_SUB_MS(tv_cur, tv_begin);

    printf("[Batch  SET] %-10s %d: %d ms, qps=%d\n",
           engine_name[engine_idx], count, time_used,
           count * 1000 / (time_used > 0 ? time_used : 1));

    free(batch);
    free(resp);
}

void batch_get(int connfd, int engine_idx, int count) {
    int cap = count * 80;
    char *batch = malloc(cap);
    char *resp  = malloc(cap);
    int blen = 0;
    int i;

    for (i = 0; i < count; i++) {
        char key[64];
        snprintf(key, sizeof(key), "%s_%06d", engine_name[engine_idx], i);
        blen = append_resp_cmd(batch, blen, engine_get_cmd[engine_idx], key, NULL);
    }

    struct timeval tv_begin, tv_cur;
    gettimeofday(&tv_begin, NULL);

    send_and_recv(connfd, batch, blen, resp, cap, count);

    gettimeofday(&tv_cur, NULL);
    int time_used = TIME_SUB_MS(tv_cur, tv_begin);

    printf("[Batch  GET] %-10s %d: %d ms, qps=%d\n",
           engine_name[engine_idx], count, time_used,
           count * 1000 / (time_used > 0 ? time_used : 1));

    free(batch);
    free(resp);
}

/* ============================================================
 * Part 2：错误格式测试
 * ============================================================ */

/* 阶段 1：正确 SSET */
void error_test_phase1_correct_rset(int connfd, int count) {
    int cap = count * 80;
    char *batch = malloc(cap);
    char *resp  = malloc(cap);
    int blen = 0;
    int i;

    for (i = 0; i < count; i++) {
        char key[64], value[64];
        snprintf(key, sizeof(key), "rt_%06d", i);
        snprintf(value, sizeof(value), "val_%06d", i);
        blen = append_resp_cmd(batch, blen, "RSET", key, value);
    }

    struct timeval tv_begin, tv_cur;
    gettimeofday(&tv_begin, NULL);

    send_and_recv(connfd, batch, blen, resp, cap, count);

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

    printf("[Error Test Phase1] RSET %d: %d ms, ok=%d\n",
           count, time_used, ok_count);

    free(batch);
    free(resp);
}

/* ===== 通用：边发边收,但只统计响应,不校验内容 ===== */
static void send_and_drain(int connfd, const char *batch, int batch_len,
                           int expected_count) {
    // char resp_buf[CHUNK_SIZE];
    int sent = 0;
    // int total_resp = 0;
    char *big_resp = malloc(expected_count * 64);   // 大缓冲
    int big_len = 0;

    while (sent < batch_len) {
        int chunk = batch_len - sent;
        if (chunk > CHUNK_SIZE) chunk = CHUNK_SIZE;

        int n = send(connfd, batch + sent, chunk, 0);
        if (n <= 0) { perror("send"); break; }
        sent += n;

        int n2 = recv(connfd, big_resp + big_len,
                      expected_count * 64 - big_len - 1, 0);
        if (n2 > 0) big_len += n2;
    }

    /* 用 count_responses 收完剩余 */
    while (count_responses(big_resp, big_len) < expected_count) {
        fd_set rfds;
        struct timeval tv = { 1, 0 };
        FD_ZERO(&rfds);
        FD_SET(connfd, &rfds);
        if (select(connfd + 1, &rfds, NULL, NULL, &tv) <= 0) break;
        int n = recv(connfd, big_resp + big_len,
                     expected_count * 64 - big_len - 1, 0);
        if (n <= 0) break;
        big_len += n;
    }

    free(big_resp);
}

/* 阶段 2：发 10w 条 RESP 格式错误的 SMOD */
void error_test_phase2_bad_resp_rmod(int connfd, int count) {
    char *batch = malloc(count * 64);
    int blen = 0;
    int i;

    for (i = 0; i < count; i++) {
        char key[64];
        snprintf(key, sizeof(key), "rt_%06d", i);

        /* 错误格式：*2 RMOD key(缺 value) */
        blen += sprintf(batch + blen, "*1\r\n");
        blen += sprintf(batch + blen,
                        "*2\r\n$4\r\nRMOD\r\n$%d\r\n%s\r\n",
                        (int)strlen(key), key);
    }

    printf("[Error Test Phase2] Sending %d bad RESP RMOD...\n", count);
    fflush(stdout);

    /* ===== 边发边收 ===== */
    send_and_drain(connfd, batch, blen, count);

    printf("[Error Test Phase2] Done\n");

    free(batch);
}

/* 阶段 3：发 10w 条命令错误的 DEL */
void error_test_phase3_bad_cmd_del(int connfd, int count) {
    char *batch = malloc(count * 64);
    int blen = 0;
    int i;

    for (i = 0; i < count; i++) {
        char key[64];
        snprintf(key, sizeof(key), "rt_%06d", i);

        blen += sprintf(batch + blen, "*1\r\n");
        blen += sprintf(batch + blen,
                        "*2\r\n$5\r\nRDELX\r\n$%d\r\n%s\r\n",
                        (int)strlen(key), key);
    }

    printf("[Error Test Phase3] Sending %d bad cmd RDEL...\n", count);
    fflush(stdout);

    /* ===== 边发边收 ===== */
    send_and_drain(connfd, batch, blen, count);

    printf("[Error Test Phase3] Done\n");

    free(batch);
}

/* 阶段 4：正确 SGET,验证数据完整 */
void error_test_phase4_verify(int connfd, int count) {
    int cap = count * 80;
    char *batch = malloc(cap);
    char *resp  = malloc(cap);
    int blen = 0;
    int i;

    for (i = 0; i < count; i++) {
        char key[64];
        snprintf(key, sizeof(key), "rt_%06d", i);
        blen = append_resp_cmd(batch, blen, "RGET", key, NULL);
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

        char expected[64];
        snprintf(expected, sizeof(expected), "val_%06d", i);

        if (strcmp(p, expected) == 0) pass++;
        else {
            fail++;
            if (fail <= 10)
                printf("[Error Test] MISMATCH i=%d: got '%s', expected '%s'\n",
                       i, p, expected);
        }
        p = line_end + 2;
    }

    printf("[Error Test Phase4] RGET  %d: %d ms, pass=%d, fail=%d\n",
           count, time_used, pass, fail);

    if (fail == 0 && pass == count)
        printf("[Error Test] ==> PASS (data intact)\n");
    else
        printf("[Error Test] ==> FAILED (data corrupted)\n");

    free(batch);
    free(resp);
}
void verify_single_key(int connfd, const char *key) {
    char cmd[256];
    char resp[256];
    int total = 0;

    int cmd_len = snprintf(cmd, sizeof(cmd),
                           "*1\r\n*2\r\n$4\r\nRGET\r\n$%d\r\n%s\r\n",
                           (int)strlen(key), key);

    send(connfd, cmd, cmd_len, 0);

    /* 一直读到出现 \r\n */
    while (total < (int)sizeof(resp) - 1) {
        int n = recv(connfd, resp + total, sizeof(resp) - 1 - total, 0);
        if (n <= 0) break;
        total += n;
        resp[total] = '\0';
        if (strstr(resp, "\r\n")) break;
    }

    if (total <= 0) {
        printf("[Verify] recv failed for key '%s'\n", key);
        return;
    }
    resp[strcspn(resp, "\r\n")] = '\0';

    printf("[Verify] RGET %s => '%s'\n", key, resp);
    if (strncmp(resp, "val_", 4) == 0)
        printf("[Verify] ==> key EXISTS in memory engine (skiptable)\n");
    else
        printf("[Verify] ==> key MISSING in memory engine\n");
}

/* ============================================================
 * Part 3：内存测试(4 引擎 × 10w = 40w)
 * ============================================================ */
#define PER_ENGINE_100K   100000

/* 插 40w：4 引擎 × 10w */
static void insert_400k(int connfd) {
    int total = PER_ENGINE_100K * 4;
    int cap = total * 80;
    char *batch = malloc(cap);
    char *resp  = malloc(cap);
    if (!batch || !resp) { free(batch); free(resp); return; }

    int blen = 0, e, i;
    for (e = 0; e < 4; e++) {
        for (i = 0; i < PER_ENGINE_100K; i++) {
            char key[64], value[64];
            snprintf(key, sizeof(key), "%s_%06d", engine_name[e], i);
            snprintf(value, sizeof(value), "val_%06d", i);
            blen = append_resp_cmd(batch, blen, engine_set_cmd[e], key, value);
        }
        printf("[Insert] %-10s done (%d)\n", engine_name[e], PER_ENGINE_100K);
        fflush(stdout);
    }

    struct timeval tv_begin, tv_cur;
    gettimeofday(&tv_begin, NULL);
    int got = send_and_recv(connfd, batch, blen, resp, cap, total);
    gettimeofday(&tv_cur, NULL);
    int ms = TIME_SUB_MS(tv_cur, tv_begin);

    printf("[Insert 400k] total=%d resp=%d bytes, time=%d ms, qps=%d\n",
           total, got, ms, total * 1000 / (ms ? ms : 1));

    free(batch);
    free(resp);
}

/* 删 40w：4 引擎 × 10w,key 和插入时一致 */
static void delete_400k(int connfd) {
    int total = PER_ENGINE_100K * 4;
    int cap = total * 80;
    char *batch = malloc(cap);
    char *resp  = malloc(cap);
    if (!batch || !resp) { free(batch); free(resp); return; }

    int blen = 0, e, i;
    for (e = 0; e < 4; e++) {
        for (i = 0; i < PER_ENGINE_100K; i++) {
            char key[64];
            snprintf(key, sizeof(key), "%s_%06d", engine_name[e], i);
            blen = append_resp_cmd(batch, blen, engine_del_cmd[e], key, NULL);
        }
        printf("[Delete] %-10s done (%d)\n", engine_name[e], PER_ENGINE_100K);
        fflush(stdout);
    }

    struct timeval tv_begin, tv_cur;
    gettimeofday(&tv_begin, NULL);
    int got = send_and_recv(connfd, batch, blen, resp, cap, total);
    gettimeofday(&tv_cur, NULL);
    int ms = TIME_SUB_MS(tv_cur, tv_begin);

    printf("[Delete 400k] total=%d resp=%d bytes, time=%d ms, qps=%d\n",
           total, got, ms, total * 1000 / (ms ? ms : 1));

    free(batch);
    free(resp);
}

/* ============================================================
 * main
 * ============================================================ */
int main(int argc, char *argv[]) {
    if (argc != 4) {
        printf("Usage: %s <ip> <port> <mode>\n", argv[0]);
        printf("  ===== Part 1: 四种模式对比 =====\n");
        printf("  mode 1: 单条收发 + 交叉(set/get/mod/exist/del x 2w轮,4 引擎共 40w)\n");
        printf("  mode 2: 单条收发 + 集中(每阶段 2w 条 x 5 阶段)\n");
        printf("  mode 3: 管道化 + 交叉\n");
        printf("  mode 4: 管道化 + 集中\n");
        printf("  mode 5: 全部四种模式(1+2+3+4)\n");
        printf("  ===== Part 2: 错误格式测试 =====\n");
        printf("  mode 10: 全部 4 阶段\n");
        printf("  ===== Part 3: 内存测试 =====\n");
        printf("  mode 6: 插 40w(4 引擎 × 10w)\n");
        printf("  mode 7: 删 40w(4 引擎 × 10w)\n");
        return -1;
    }

    char *ip = argv[1];
    int port = atoi(argv[2]);
    mode = atoi(argv[3]);

    int connfd = connect_tcpserver(ip, port);
    if (connfd < 0) {
        printf("Connect failed\n");
        return -1;
    }

    int e;

    switch (mode) {
    case 1:
        printf("===== mode 1: 单条收发 + 交叉 =====\n");
        for (e = 0; e < 4; e++) mixed_single(connfd, e, MIX_LOOP);
        break;

    case 2:
        printf("===== mode 2: 单条收发 + 集中 =====\n");
        for (e = 0; e < 4; e++) staged_single(connfd, e, STAGE_COUNT);
        break;

    case 3:
        printf("===== mode 3: 管道化 + 交叉 =====\n");
        for (e = 0; e < 4; e++) mixed_ops(connfd, e, MIX_LOOP);
        break;

    case 4:
        printf("===== mode 4: 管道化 + 集中 =====\n");
        for (e = 0; e < 4; e++) staged_ops(connfd, e, STAGE_COUNT);
        break;

    case 5:
        printf("===== Part 1: 四种模式对比 =====\n\n");

        printf("--- 1. 单条收发 + 交叉 ---\n");
        for (e = 0; e < 4; e++) mixed_single(connfd, e, MIX_LOOP);

        printf("\n--- 2. 单条收发 + 集中 ---\n");
        for (e = 0; e < 4; e++) staged_single(connfd, e, STAGE_COUNT);

        printf("\n--- 3. 管道化 + 交叉 ---\n");
        for (e = 0; e < 4; e++) mixed_ops(connfd, e, MIX_LOOP);

        printf("\n--- 4. 管道化 + 集中 ---\n");
        for (e = 0; e < 4; e++) staged_ops(connfd, e, STAGE_COUNT);

        printf("\n===== 对比测试完成 =====\n");
        break;
    case 6:
        printf("===== mode 6: 插 40w (4 引擎 × 10w) =====\n");
        insert_400k(connfd);
        break;

    case 7:
        printf("===== mode 7: 删 40w (4 引擎 × 10w) =====\n");
        delete_400k(connfd);
        break;
    case 10:
        printf("===== Part 2: 错误格式测试(Skiptable) =====\n\n");

        printf("--- 阶段1: 发 10w 条正确的 SSET ---\n");
        error_test_phase1_correct_rset(connfd, 100000);
        printf("\n--- 阶段1.5: 直接验证 et_099999 是否在内存中 ---\n");
        verify_single_key(connfd, "et_099999");
        verify_single_key(connfd, "et_099998");
        verify_single_key(connfd, "et_000000");

        printf("\n--- 阶段2: 发 10w 条 RESP 格式错误的 SMOD(数据不被修改)---\n");
        error_test_phase2_bad_resp_rmod(connfd, 100000);

        printf("\n--- 阶段3: 发 10w 条命令错误的 DEL(数据不被删除)---\n");
        error_test_phase3_bad_cmd_del(connfd, 100000);

        printf("\n--- 阶段4: 发 10w 条正确的 SGET,验证数据完整 ---\n");
        error_test_phase4_verify(connfd, 100000);

        printf("\n===== 错误格式测试完成 =====\n");
        break;

    default:
        printf("Unknown mode: %d\n", mode);
        break;
    }

    close(connfd);
    return 0;
}