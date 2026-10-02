#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <unistd.h>
#include "kvstore.h"


#define TEST_COUNT      100000      // 总数据量 10w
#define PER_ENGINE      25000       // 每种数据结构 2.5w
#define ENGINE_COUNT    4           // 4 种引擎

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
 * RESP 命令拼接
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

static int append_save(char *buf, int len) {
    len += sprintf(buf + len, "*1\r\n");
    len += sprintf(buf + len, "*1\r\n$4\r\nSAVE\r\n");
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
static const char *engine_set_cmd[ENGINE_COUNT] = {
    "SET",   /* Array */
    "RSET",  /* Rbtree */
    "HSET",  /* Hash */
    "SSET",  /* Skiptable */
};

static const char *engine_get_cmd[ENGINE_COUNT] = {
    "GET",   /* Array */
    "RGET",  /* Rbtree */
    "HGET",  /* Hash */
    "SGET",  /* Skiptable */
};

static const char *engine_name[ENGINE_COUNT] = {
    "Array", "Rbtree", "Hash", "Skiptable"
};

/* ============================================================
 * 阶段1：插入 10w 条 SET（4 引擎 × 2.5w）
 *
 * 每种引擎的数据：
 *   key = "{engine_prefix}_{i}"   i = 0..24999
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

        printf("Insert progress: %d%% (%s done)\n", (e + 1) * 25, engine_name[e]);
        fflush(stdout);
    }

    printf("Total commands: %d, batch len: %d\n", TEST_COUNT, blen);
    send_and_recv(connfd, batch, blen, resp, cap, TEST_COUNT);
    printf("Insert 100k done\n");

    free(batch);
    free(resp);
}

/* ============================================================
 * 阶段2：验证 10w 条 GET（4 引擎 × 2.5w）
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

        printf("Verify progress: %d%% (%s done)\n", (e + 1) * 25, engine_name[e]);
        fflush(stdout);
    }

    send_and_recv(connfd, batch, blen, resp, cap, TEST_COUNT);

    /* 逐条验证 */
    char *p = resp;
    for (e = 0; e < ENGINE_COUNT; e++) {
        for (i = 0; i < PER_ENGINE; i++) {
            char *line_end = strstr(p, "\r\n");
            if (!line_end) { fail++; break; }
            *line_end = '\0';

            char expected[64];
            snprintf(expected, sizeof(expected), "val_%06d", i);

            if (strcmp(p, expected) == 0) pass++;
            else {
                fail++;
                if (fail <= 10)
                    printf("MISMATCH e=%s i=%d: got '%s', expected '%s'\n",
                           engine_name[e], i, p, expected);
            }
            p = line_end + 2;
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
 * 发送 SAVE
 * ============================================================ */
void send_save(int connfd) {
    char buf[32];
    char resp[64] = {0};
    int blen = append_save(buf, 0);
    send(connfd, buf, blen, 0);
    recv(connfd, resp, sizeof(resp) - 1, 0);
    printf("SAVE: %s", resp);
}

/* ============================================================
 * main
 * ============================================================ */
int main(int argc, char *argv[]) {
    if (argc != 4) {
        printf("Usage: %s <ip> <port> <mode>\n", argv[0]);
        printf("  mode 1: AOF  - 插入 10w\n");
        printf("  mode 2: AOF  - 验证 10w\n");
        printf("  mode 3: SNAP - 插入 10w + SAVE\n");
        printf("  mode 4: SNAP - 验证 10w\n");
        return -1;
    }

    char *ip = argv[1];
    int port = atoi(argv[2]);
    mode = atoi(argv[3]);

    /* ============================================================
     * 加载配置
     * ============================================================ */
    if (kvs_config_load("config/master.conf") < 0) {
        printf("Load config failed\n");
        return -1;
    }

    /* ============================================================
     * 根据 mode 校验配置
     * ============================================================ */
    if (mode == 1 || mode == 2) {
        /* AOF 测试：要求 aof.enable = true */
        if (!g_config.aof_enable) {
            printf("[ERROR] mode %d requires aof.enable=true in config/master.conf\n", mode);
            printf("        current aof.enable=false\n");
            return -1;
        }
        printf("[Config] aof.enable=true (OK)\n");
    }
    else if (mode == 3 || mode == 4) {
        /* SNAP 测试：要求 snap.enable = true */
        if (!g_config.snap_enable) {
            printf("[ERROR] mode %d requires snap.enable=true in config/master.conf\n", mode);
            printf("        current snap.enable=false\n");
            return -1;
        }
        printf("[Config] snap.enable=true (OK)\n");
    }
    else {
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

    close(connfd);
    return 0;
}