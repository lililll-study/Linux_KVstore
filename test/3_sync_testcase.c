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
    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(struct sockaddr_in));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(port);
    if (0 != connect(connfd, (struct sockaddr*)&servaddr, sizeof(struct sockaddr_in))) {
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
 * 向指定连接插入 [start, start+count) 条数据
 * ============================================================ */
static int insert_range(int connfd, int start, int count) {
    int cap = count * 48;
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

    /* 统计 ok 个数 */
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
 * 从指定连接读取 [start, start+count) 条数据并校验
 * ============================================================ */
static int verify_range(int connfd, int start, int count) {
    int cap = count * 48;
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
 * 探测从机是否已完成同步：尝试 GET 第一条 key
 * 返回 1 表示同步完成（能拿到值），0 表示还没同步好
 * ============================================================ */
static int probe_slave_synced(int connfd, int timeout_sec) {
    char cmd[128];
    char resp[128];
    int cmd_len = snprintf(cmd, sizeof(cmd),
                           "*1\r\n*2\r\n$3\r\nGET\r\n$9\r\nps_000000\r\n");

    int waited = 0;
    while (waited < timeout_sec * 10) {
        send(connfd, cmd, cmd_len, 0);

        int total = 0;
        while (total < (int)sizeof(resp) - 1) {
            int n = recv(connfd, resp + total, sizeof(resp) - 1 - total, 0);
            if (n <= 0) break;
            total += n;
            resp[total] = '\0';
            if (strstr(resp, "\r\n")) break;
        }
        if (total > 0) {
            resp[strcspn(resp, "\r\n")] = '\0';
            if (strncmp(resp, "val_", 4) == 0) {
                return 1;   /* 同步完成 */
            }
        }
        usleep(100 * 1000);  /* 100ms */
        waited++;
    }
    return 0;
}

/* ============================================================
 * main
 * ============================================================ */
int main(int argc, char *argv[]) {
    if (argc != 6) {
        printf("Usage: %s <master_ip> <master_port> <slave_ip> <slave_port> <mode>\n", argv[0]);
        printf("  mode 1: 完整流程（插5w -> 等你启动从机 -> 插5w -> 验从机10w）\n");
        printf("  mode 2: 只插前5w（然后手动启动从机）\n");
        printf("  mode 3: 只插后5w\n");
        printf("  mode 4: 只验证从机10w\n");
        return -1;
    }

    const char *master_ip = argv[1];
    int master_port = atoi(argv[2]);
    const char *slave_ip  = argv[3];
    int slave_port  = atoi(argv[4]);
    int mode = atoi(argv[5]);

    if (mode == 1) {
        /* ---------- 1. 连接主机，插入前 5w ---------- */
        int conn_master = connect_tcpserver(master_ip, master_port);
        if (conn_master < 0) { printf("connect master failed\n"); return -1; }

        printf("===== 阶段1: 向主机插入前 5w 条 =====\n");
        insert_range(conn_master, 0, TEST_COUNT);

        printf("\n>>> 现在请启动从机，从机会自动同步。\n");
        printf(">>> 等待 10 秒后继续（可按回车提前继续）...\n");
        sleep(10);

        /* ---------- 2. 再插入后 5w ---------- */
        printf("\n===== 阶段2: 向主机插入后 5w 条 =====\n");
        insert_range(conn_master, TEST_COUNT, TEST_COUNT);

        printf("\n>>> 再等 5 秒，让从机同步后 5w...\n");
        sleep(5);

        close(conn_master);

        /* ---------- 3. 连接从机，验证 10w ---------- */
        printf("\n===== 阶段3: 连接从机，验证 10w 条 =====\n");
        int conn_slave = connect_tcpserver(slave_ip, slave_port);
        if (conn_slave < 0) { printf("connect slave failed\n"); return -1; }

        /* 先探测从机是否同步好 */
        printf(">>> 探测从机同步状态...\n");
        if (probe_slave_synced(conn_slave, 30)) {
            printf(">>> 从机已同步\n");
        } else {
            printf(">>> 警告：从机 30 秒内未同步好，仍然继续验证\n");
        }

        int fail1 = verify_range(conn_slave, 0, TEST_COUNT);
        int fail2 = verify_range(conn_slave, TEST_COUNT, TEST_COUNT);

        printf("\n===== 结果 =====\n");
        if (fail1 == 0 && fail2 == 0)
            printf("==> ALL PASS (10w 条一致)\n");
        else
            printf("==> FAILED (前5w fail=%d, 后5w fail=%d)\n", fail1, fail2);

        close(conn_slave);
    }
    else if (mode == 2) {
        int conn_master = connect_tcpserver(master_ip, master_port);
        if (conn_master < 0) return -1;
        printf("===== 插入前 5w =====\n");
        insert_range(conn_master, 0, TEST_COUNT);
        close(conn_master);
        printf(">>> 现在启动从机，等同步好后跑 mode 3\n");
    }
    else if (mode == 3) {
        int conn_master = connect_tcpserver(master_ip, master_port);
        if (conn_master < 0) return -1;
        printf("===== 插入后 5w =====\n");
        insert_range(conn_master, TEST_COUNT, TEST_COUNT);
        close(conn_master);
        printf(">>> 等从机同步好，跑 mode 4\n");
    }
    else if (mode == 4) {
        int conn_slave = connect_tcpserver(slave_ip, slave_port);
        if (conn_slave < 0) return -1;
        printf("===== 验证从机 10w =====\n");
        int fail1 = verify_range(conn_slave, 0, TEST_COUNT);
        int fail2 = verify_range(conn_slave, TEST_COUNT, TEST_COUNT);
        if (fail1 == 0 && fail2 == 0)
            printf("==> ALL PASS\n");
        else
            printf("==> FAILED (fail1=%d, fail2=%d)\n", fail1, fail2);
        close(conn_slave);
    }

    return 0;
}