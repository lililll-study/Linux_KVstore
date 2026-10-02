
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <unistd.h>



#define TEST_COUNT 100000
#define CHUNK_SIZE 1024


int mode = 0;

int connect_tcpserver(const char *ip, unsigned short port) {
    int connfd = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(struct sockaddr_in));

    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY); //0.0.0.0
    servaddr.sin_port = htons(port);//0-1023不能用，要大于1024

    if (0 != connect(connfd, (struct sockaddr*)&servaddr, sizeof(struct sockaddr_in))) {
        perror("connect");
        return -1;
    }

    return connfd;
}


/* 追加 RESP SET 到 buf，外层包 *1 */
static int append_set(char *buf, int len, const char *key, const char *value) {
    len += sprintf(buf + len, "*1\r\n");       // ← 外层 *1，表示 1 条命令
    len += sprintf(buf + len, "*3\r\n$3\r\nSET\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n",
                   (int)strlen(key), key, (int)strlen(value), value);
    return len;
}

static int append_get(char *buf, int len, const char *key) {
    len += sprintf(buf + len, "*1\r\n");
    len += sprintf(buf + len, "*2\r\n$3\r\nGET\r\n$%d\r\n%s\r\n",
                   (int)strlen(key), key);
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

    /* 收完剩余响应 */
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
 * 阶段1：插入 10w 条 SET
 * ============================================================ */
void insert_100k(int connfd) {
    /* 所有 cmd 先攒进内存 batch */
    int cap = TEST_COUNT * 48;
    char *batch = (char *)malloc(cap);
    char *resp  = (char *)malloc(cap);
    int blen = 0;
    int i;

    for (i = 0; i < TEST_COUNT; i++) {
        char key[32], value[32];
        snprintf(key, sizeof(key), "key%06d", i);
        snprintf(value, sizeof(value), "val%06d", i);
        blen = append_set(batch, blen, key, value);
    }

    send_and_recv(connfd, batch, blen, resp, cap, TEST_COUNT);
    printf("Insert 100k done\n");

    free(batch);
    free(resp);
}

/* ============================================================
 * 阶段2：验证 10w 条
 * ============================================================ */
void verify_100k(int connfd) {
    int cap = TEST_COUNT * 48;
    char *batch = (char *)malloc(cap);
    char *resp  = (char *)malloc(cap);
    int blen = 0;
    int i;
    int pass = 0, fail = 0;

    for (i = 0; i < TEST_COUNT; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%06d", i);
        blen = append_get(batch, blen, key);
    }

    send_and_recv(connfd, batch, blen, resp, cap, TEST_COUNT);

    /* 逐行解析 */
    char *p = resp;
    for (i = 0; i < TEST_COUNT; i++) {
        char *line_end = strstr(p, "\r\n");
        if (!line_end) { fail++; break; }
        *line_end = '\0';

        char expected[32];
        snprintf(expected, sizeof(expected), "val%06d", i);

        if (strcmp(p, expected) == 0) pass++;
        else {
            fail++;
            if (fail <= 10)
                printf("MISMATCH i=%d: got '%s', expected '%s'\n", i, p, expected);
        }
        p = line_end + 2;
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



// testcase 192.168.137.128 2000
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
    int connfd = connect_tcpserver(ip, port);

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