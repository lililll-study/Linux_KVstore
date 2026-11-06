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
 * RESP 响应解析（redis-cli 扁平格式）
 *
 * resp_parse_one 解析一个完整 RESP 响应：
 *   p   指向响应起始（'+' / '-' / ':' / '$'）
 *   end 指向缓冲区末尾（不含）
 *   out 回填响应值（去掉类型前缀与结尾 \r\n；$-1 回填 "nil"）
 *   返回完整响应后的下一个位置；数据不完整时返回 NULL
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
#define ENGINE_COUNT   2
static const char *engine_set_cmd[ENGINE_COUNT]   = { "HSET", "SSET" };
static const char *engine_get_cmd[ENGINE_COUNT]   = { "HGET", "SGET" };
static const char *engine_del_cmd[ENGINE_COUNT]   = { "HDEL", "SDEL" };
static const char *engine_mod_cmd[ENGINE_COUNT]   = { "HMOD", "SMOD" };
static const char *engine_exist_cmd[ENGINE_COUNT] = { "HEXIST", "SEXIST" };
static const char *engine_name[ENGINE_COUNT]      = { "Hash", "Skiptable" };

/* ============================================================
 * Part 1：四种模式(单条/管道 x 交叉/集中)
 * ============================================================ */
#define MIX_LOOP      20000
#define MIX_CMDS      5
#define MIX_TOTAL     (MIX_LOOP * MIX_CMDS)
#define STAGE_COUNT   20000
#define VERIFY_CHUNK  1000

/* 各操作的标准 RESP 期望值：
 *   SET(新插入) → :1 ；GET → $len\r\nval... ；MOD(已存在) → +OK
 *   EXIST(存在) → :1 ；DEL(删除成功) → :1
 */
static void expected_of(int op, int i, char *out, int n) {
    if (op == 0)      snprintf(out, n, "1");            /* SET */
    else if (op == 1) snprintf(out, n, "val_%06d", i);  /* GET */
    else if (op == 2) snprintf(out, n, "OK");           /* MOD */
    else if (op == 3) snprintf(out, n, "1");            /* EXIST */
    else              snprintf(out, n, "1");            /* DEL */
}

static int verify_resp_lines(char *resp, int op, int i_start, int count,
                             int *pass, int *fail) {
    const char *p = resp, *end = resp + strlen(resp);
    char got[256], exp[64];
    int k;
    for (k = 0; k < count; k++) {
        const char *next = resp_parse_one(p, end, got, sizeof(got));
        if (!next) { *fail += count - k; break; }
        expected_of(op, i_start + k, exp, sizeof(exp));
        if (strcmp(got, exp) == 0) (*pass)++;
        else {
            (*fail)++;
            if (*fail <= 5)
                printf("[Verify] MISMATCH op=%d i=%d: got '%s', exp '%s'\n",
                       op, i_start + k, got, exp);
        }
        p = next;
    }
    return 0;
}

/* ============================================================
 * 工具：循环 recv 直到收到一个完整的 RESP 响应
 * 返回收到的字节数，-1 表示连接断开
 * ============================================================ */
static int recv_one_resp(int connfd, char *buf, int cap, char *out, int outcap) {
    int total = 0;
    while (total < cap - 1) {
        int n = recv(connfd, buf + total, cap - 1 - total, 0);
        if (n <= 0) {
            buf[total] = '\0';
            return -1;
        }
        total += n;
        buf[total] = '\0';
        if (resp_parse_one(buf, buf + total, out, outcap)) return total;
    }
    return -1;
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
    const char *p = resp, *end = resp + strlen(resp);
    char got[256], exp[64];
    int k;
    for (k = 0; k < total; k++) {
        const char *next = resp_parse_one(p, end, got, sizeof(got));
        if (!next) { *fail += total - k; break; }

        int op  = k % MIX_CMDS;
        int idx = r_start + k / MIX_CMDS;

        expected_of(op, idx, exp, sizeof(exp));
        if (strcmp(got, exp) == 0) {
            (*pass)++;
        } else {
            (*fail)++;
            if (*fail <= 5)
                printf("[Verify] MISMATCH op=%d idx=%d: got '%s', exp '%s'\n",
                       op, idx, got, exp);
        }
        p = next;
    }
    return 0;
}

static int verify_one_resp(char *resp, int op, int i, int *pass, int *fail) {
    char exp[64], got[256];
    expected_of(op, i, exp, sizeof(exp));
    if (!resp_parse_one(resp, resp + strlen(resp), got, sizeof(got))) {
        (*fail)++;
        if (*fail <= 5)
            printf("[Verify] PARSE-FAIL op=%d i=%d: resp='%.64s'\n", op, i, resp);
        return 0;
    }
    if (strcmp(got, exp) == 0) {
        (*pass)++;
        return 0;
    }
    (*fail)++;
    if (*fail <= 5)
        printf("[Verify] MISMATCH op=%d i=%d: got '%s', exp '%s'\n",
               op, i, got, exp);
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
        return sprintf(buf, "*3\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n",
                       (int)strlen(cmdname), cmdname,
                       (int)strlen(key), key,
                       (int)strlen(op == 2 ? value2 : value),
                       op == 2 ? value2 : value);
    return sprintf(buf, "*2\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n",
                   (int)strlen(cmdname), cmdname,
                   (int)strlen(key), key);
}


/* mode1：单条收发 + 交叉 */
void mixed_single(int connfd, int engine_idx, int loop) {
    int r, op;
    char cmd[256];
    char resp[256];
    char got[256];
    int pass = 0, fail = 0;

    struct timeval tv_begin, tv_cur;
    gettimeofday(&tv_begin, NULL);

    for (r = 0; r < loop; r++) {
        for (op = 0; op < MIX_CMDS; op++) {
            int cmd_len = build_one_cmd(cmd, engine_idx, op, r);
            send(connfd, cmd, cmd_len, 0);

            /* ★ 循环收到一个完整 RESP 响应 */
            if (recv_one_resp(connfd, resp, sizeof(resp), got, sizeof(got)) < 0) {
                printf("[Mixed single] recv failed at r=%d op=%d\n", r, op);
                goto done;
            }
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
    char got[256];
    int pass = 0, fail = 0;

    struct timeval tv_begin, tv_cur;
    gettimeofday(&tv_begin, NULL);

    for (op = 0; op < MIX_CMDS; op++) {
        for (i = 0; i < count; i++) {
            int cmd_len = build_one_cmd(cmd, engine_idx, op, i);
            send(connfd, cmd, cmd_len, 0);

            /* ★ 循环收到一个完整 RESP 响应 */
            if (recv_one_resp(connfd, resp, sizeof(resp), got, sizeof(got)) < 0) {
                printf("[Staged single] recv failed at op=%d i=%d\n", op, i);
                goto done;
            }
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
 * Part 2：错误格式测试（Skiptable 引擎）
 * ============================================================ */

/* 阶段 1：正确 SSET */
void error_test_phase1_correct_sset(int connfd, int count) {
    int cap = count * 80;
    char *batch = malloc(cap);
    char *resp  = malloc(cap);
    int blen = 0;
    int i;

    for (i = 0; i < count; i++) {
        char key[64], value[64];
        snprintf(key, sizeof(key), "rt_%06d", i);
        snprintf(value, sizeof(value), "val_%06d", i);
        blen = append_resp_cmd(batch, blen, "SSET", key, value);
    }

    struct timeval tv_begin, tv_cur;
    gettimeofday(&tv_begin, NULL);

    send_and_recv(connfd, batch, blen, resp, cap, count);

    gettimeofday(&tv_cur, NULL);
    int time_used = TIME_SUB_MS(tv_cur, tv_begin);

    /* SSET 新插入 → :1 */
    int ok_count = 0;
    const char *p = resp, *end = resp + strlen(resp);
    char got[64];
    for (i = 0; i < count; i++) {
        const char *next = resp_parse_one(p, end, got, sizeof(got));
        if (!next) break;
        if (strcmp(got, "1") == 0) ok_count++;
        p = next;
    }

    printf("[Error Test Phase1] SSET %d: %d ms, ok=%d\n",
           count, time_used, ok_count);

    free(batch);
    free(resp);
}


/* 阶段 2：参数缺失（SMOD 只给 key 不给 value）→ -ERR invalid arguments */
void error_test_phase2_bad_resp_smod(int connfd, int count) {
    int cap = count * 64;
    char *batch = malloc(cap);
    char *resp  = malloc(cap);
    int blen = 0;
    int i;

    for (i = 0; i < count; i++) {
        char key[64];
        snprintf(key, sizeof(key), "rt_%06d", i);
        blen += sprintf(batch + blen, "*2\r\n$4\r\nSMOD\r\n$%d\r\n%s\r\n",
                        (int)strlen(key), key);
    }

    struct timeval tv_begin, tv_cur;
    gettimeofday(&tv_begin, NULL);

    send_and_recv(connfd, batch, blen, resp, cap, count);

    gettimeofday(&tv_cur, NULL);
    int time_used = TIME_SUB_MS(tv_cur, tv_begin);

    /* 每条响应都应为 -ERR invalid arguments */
    int ok_count = 0;
    const char *p = resp, *end = resp + strlen(resp);
    char got[256];
    for (i = 0; i < count; i++) {
        const char *next = resp_parse_one(p, end, got, sizeof(got));
        if (!next) break;
        if (strncmp(got, "ERR", 3) == 0) ok_count++;
        p = next;
    }

    printf("[Error Test Phase2] SMOD(缺value) %d: %d ms, err_count=%d\n",
           count, time_used, ok_count);

    free(batch);
    free(resp);
}

/* 阶段 3：未知命令 SDELX → -ERR unknown command 'SDELX' */
void error_test_phase3_bad_cmd_del(int connfd, int count) {
    int cap = count * 64;
    char *batch = malloc(cap);
    char *resp  = malloc(cap);
    int blen = 0;
    int i;

    for (i = 0; i < count; i++) {
        char key[64];
        snprintf(key, sizeof(key), "rt_%06d", i);
        blen += sprintf(batch + blen, "*2\r\n$5\r\nSDELX\r\n$%d\r\n%s\r\n",
                        (int)strlen(key), key);
    }

    struct timeval tv_begin, tv_cur;
    gettimeofday(&tv_begin, NULL);

    send_and_recv(connfd, batch, blen, resp, cap, count);

    gettimeofday(&tv_cur, NULL);
    int time_used = TIME_SUB_MS(tv_cur, tv_begin);

    int ok_count = 0;
    const char *p = resp, *end = resp + strlen(resp);
    char got[256];
    for (i = 0; i < count; i++) {
        const char *next = resp_parse_one(p, end, got, sizeof(got));
        if (!next) break;
        if (strstr(got, "unknown command")) ok_count++;
        p = next;
    }

    printf("[Error Test Phase3] SDELX(未知命令) %d: %d ms, err_count=%d\n",
           count, time_used, ok_count);

    free(batch);
    free(resp);
}

/* 阶段 4：SGET 校验 phase1 插入的数据（bulk 响应解析） */
void error_test_phase4_verify(int connfd, int count) {
    int cap = count * 64;
    char *batch = malloc(cap);
    char *resp  = malloc(cap);
    int blen = 0;
    int i;

    for (i = 0; i < count; i++) {
        char key[64];
        snprintf(key, sizeof(key), "rt_%06d", i);
        blen = append_resp_cmd(batch, blen, "SGET", key, NULL);
    }

    struct timeval tv_begin, tv_cur;
    gettimeofday(&tv_begin, NULL);

    send_and_recv(connfd, batch, blen, resp, cap, count);

    gettimeofday(&tv_cur, NULL);
    int time_used = TIME_SUB_MS(tv_cur, tv_begin);

    int pass = 0, fail = 0;
    const char *p = resp, *end = resp + strlen(resp);
    char got[256], exp[64];
    for (i = 0; i < count; i++) {
        const char *next = resp_parse_one(p, end, got, sizeof(got));
        if (!next) { fail += count - i; break; }
        snprintf(exp, sizeof(exp), "val_%06d", i);
        if (strcmp(got, exp) == 0) pass++;
        else {
            fail++;
            if (fail <= 5)
                printf("[Verify] MISMATCH i=%d: got '%s', exp '%s'\n",
                       i, got, exp);
        }
        p = next;
    }

    printf("[Error Test Phase4] SGET校验 %d: %d ms, pass=%d fail=%d\n",
           count, time_used, pass, fail);

    free(batch);
    free(resp);
}

/* 单键 SGET：拼接扁平命令,解析 bulk 值 */
static void verify_single_key(int connfd, const char *key, const char *expect) {
    char cmd[128], resp[1024], got[256];
    int cmd_len = append_resp_cmd(cmd, 0, "SGET", key, NULL);
    send(connfd, cmd, cmd_len, 0);

    if (recv_one_resp(connfd, resp, sizeof(resp), got, sizeof(got)) < 0) {
        printf("[Verify] SGET %s recv failed\n", key);
        return;
    }
    if (strcmp(got, expect) == 0)
        printf("[Verify] SGET %s = %s (PASS)\n", key, got);
    else
        printf("[Verify] SGET %s = %s, exp '%s' (FAIL)\n", key, got, expect);
}

/* ============================================================
 * Part 3：100 万数据（2 引擎 × 50 万）
 * ============================================================ */
#define PER_ENGINE_500K  500000

void insert_1m(int connfd) {
    int e;
    for (e = 0; e < ENGINE_COUNT; e++) {
        int cap = PER_ENGINE_500K * 80;
        char *batch = malloc(cap);
        char *resp  = malloc(cap);
        int blen = 0;
        int i;

        for (i = 0; i < PER_ENGINE_500K; i++) {
            char key[64], value[64];
            snprintf(key, sizeof(key), "%s_%06d", engine_name[e], i);
            snprintf(value, sizeof(value), "val_%06d", i);
            blen = append_resp_cmd(batch, blen, engine_set_cmd[e], key, value);
        }

        struct timeval tv_begin, tv_cur;
        gettimeofday(&tv_begin, NULL);

        send_and_recv(connfd, batch, blen, resp, cap, PER_ENGINE_500K);

        gettimeofday(&tv_cur, NULL);
        int time_used = TIME_SUB_MS(tv_cur, tv_begin);

        printf("[Insert 500k] %-10s %d: %d ms, qps=%d\n",
               engine_name[e], PER_ENGINE_500K, time_used,
               PER_ENGINE_500K * 1000 / (time_used > 0 ? time_used : 1));

        free(batch);
        free(resp);
    }
}

void delete_1m(int connfd) {
    int e;
    for (e = 0; e < ENGINE_COUNT; e++) {
        int cap = PER_ENGINE_500K * 80;
        char *batch = malloc(cap);
        char *resp  = malloc(cap);
        int blen = 0;
        int i;

        for (i = 0; i < PER_ENGINE_500K; i++) {
            char key[64];
            snprintf(key, sizeof(key), "%s_%06d", engine_name[e], i);
            blen = append_resp_cmd(batch, blen, engine_del_cmd[e], key, NULL);
        }

        struct timeval tv_begin, tv_cur;
        gettimeofday(&tv_begin, NULL);

        send_and_recv(connfd, batch, blen, resp, cap, PER_ENGINE_500K);

        gettimeofday(&tv_cur, NULL);
        int time_used = TIME_SUB_MS(tv_cur, tv_begin);

        printf("[Delete 500k] %-10s %d: %d ms, qps=%d\n",
               engine_name[e], PER_ENGINE_500K, time_used,
               PER_ENGINE_500K * 1000 / (time_used > 0 ? time_used : 1));

        free(batch);
        free(resp);
    }
}

/* ============================================================
 * Part 4：echo 写入测试（README §3：10w 条插入，单条收发）
 * 与 redis-benchmark -c 1 -P 1 -n 100000 SET 对齐：
 * 发一条命令、等一条响应，量出真实往返的 echo 性能
 * ============================================================ */
#define ECHO_COUNT  100000

void echo_insert(int connfd, int engine_idx, int count) {
    int i;
    char cmd[256];
    char resp[256];
    char got[256];
    int pass = 0, fail = 0;

    struct timeval tv_begin, tv_cur;
    gettimeofday(&tv_begin, NULL);

    for (i = 0; i < count; i++) {
        /* op 0 = SET：key/value 沿用 Hash_i / val_i 格式 */
        int cmd_len = build_one_cmd(cmd, engine_idx, 0, i);
        send(connfd, cmd, cmd_len, 0);

        /* ★ 一条命令一个往返：循环收满一个完整 RESP 响应 */
        if (recv_one_resp(connfd, resp, sizeof(resp), got, sizeof(got)) < 0) {
            printf("[Echo insert] recv failed at i=%d\n", i);
            goto done;
        }
        /* 新插入回 :1，覆盖旧数据回 +OK，两者都算成功 */
        if (strcmp(got, "1") == 0 || strcmp(got, "OK") == 0) {
            pass++;
        } else {
            fail++;
            if (fail <= 5)
                printf("[Echo insert] MISMATCH i=%d: got '%s'\n", i, got);
        }
    }

done:
    gettimeofday(&tv_cur, NULL);
    int time_used = TIME_SUB_MS(tv_cur, tv_begin);

    printf("[Echo insert] %-10s %d: %d ms, qps=%d, pass=%d fail=%d\n",
           engine_name[engine_idx], count, time_used,
           count * 1000 / (time_used > 0 ? time_used : 1), pass, fail);
}

/* ============================================================
 * main：测试入口
 * ============================================================ */
int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("usage: %s <mode> <engine>\n", argv[0]);
        printf("  mode:\n");
        printf("    1  mixed_single   单条收发 + 交叉 (SET/GET/MOD/EXIST/DEL)\n");
        printf("    2  staged_single  单条收发 + 集中\n");
        printf("    3  mixed_ops      管道化 + 交叉\n");
        printf("    4  staged_ops     管道化 + 集中\n");
        printf("    5  批量 SET/GET (每引擎 BATCH_COUNT 条)\n");
        printf("    6  插入 100w (2 引擎 x 50w)\n");
        printf("    7  删除 100w (2 引擎 x 50w)\n");
        printf("    8  echo_insert    echo 写入测试 (10w 条单条收发)\n");
        printf("   10  错误格式测试 (SSET → SMOD缺参 → SDELX未知命令 → SGET校验)\n");
        printf("  engine: 0=Hash 1=Skiptable (-1=全部引擎)\n");
        return 0;
    }

    mode = atoi(argv[1]);
    int engine = atoi(argv[2]);

    int connfd = connect_tcpserver("127.0.0.1", 2000);
    if (connfd < 0) {
        printf("connect to 127.0.0.1:2000 failed\n");
        return -1;
    }

    int e;
    switch (mode) {
    case 1:
    case 2:
    case 3:
    case 4:
        if (engine < 0)
            for (e = 0; e < ENGINE_COUNT; e++) {
                if (mode == 1) mixed_single(connfd, e, MIX_LOOP);
                else if (mode == 2) staged_single(connfd, e, STAGE_COUNT);
                else if (mode == 3) mixed_ops(connfd, e, MIX_LOOP);
                else staged_ops(connfd, e, STAGE_COUNT);
            }
        else {
            if (mode == 1) mixed_single(connfd, engine, MIX_LOOP);
            else if (mode == 2) staged_single(connfd, engine, STAGE_COUNT);
            else if (mode == 3) mixed_ops(connfd, engine, MIX_LOOP);
            else staged_ops(connfd, engine, STAGE_COUNT);
        }
        break;
    case 5:
        if (engine < 0)
            for (e = 0; e < ENGINE_COUNT; e++) {
                batch_set(connfd, e, BATCH_COUNT);
                batch_get(connfd, e, BATCH_COUNT);
            }
        else {
            batch_set(connfd, engine, BATCH_COUNT);
            batch_get(connfd, engine, BATCH_COUNT);
        }
        break;
    case 6:
        insert_1m(connfd);
        break;
    case 7:
        delete_1m(connfd);
        break;
    case 8:
        if (engine < 0)
            for (e = 0; e < ENGINE_COUNT; e++)
                echo_insert(connfd, e, ECHO_COUNT);
        else
            echo_insert(connfd, engine, ECHO_COUNT);
        break;
    case 10:
        error_test_phase1_correct_sset(connfd, 10000);
        error_test_phase2_bad_resp_smod(connfd, 10000);
        error_test_phase3_bad_cmd_del(connfd, 10000);
        error_test_phase4_verify(connfd, 10000);
        verify_single_key(connfd, "rt_000000", "val_000000");
        break;
    default:
        printf("unknown mode %d\n", mode);
        break;
    }

    close(connfd);
    return 0;
}
