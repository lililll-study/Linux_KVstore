
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <errno.h>      // ← 添加这行
#include <unistd.h>

#define MAX_MSG_LEN 4096
#define TIME_SUB_MS(tv1, tv2)  ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)
int mode = 0;

int send_msg(int connfd, char *msg, int length) {
    printf("send: %s\n", msg);
    int res = send(connfd, msg, length, 0);

    if (res < 0) {
        perror("send");
        exit(1);
    }
    return res;
}

int recv_msg(int connfd, char *msg, int length) {

    int res = recv(connfd, msg, length, 0);
    if (res < 0) {
        perror("recv");
        exit(1);
    }
    return res;
}

void testcase(int connfd, char *msg, char *pattern, char *casename) {

    if (!msg || !pattern || !casename) return ;

    send_msg(connfd, msg, strlen(msg));
    
    char result[MAX_MSG_LEN] = {0};
    recv_msg(connfd, result, MAX_MSG_LEN);

    // 比对result和pattern是否一致
    if (strcmp(result, pattern) == 0) {
        // printf("==> PASS -> %s\n", casename);
    } else {
        printf("==> FAILED -> %s, '%s' != '%s'\n", casename, result, pattern);
    }
}

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

void array_testcase_1w (int connfd) {
    int count = 10000;
    int i = 0;
    struct timeval tv_begin;
	gettimeofday(&tv_begin, NULL);

    for (i = 0; i< count; i++) {
        testcase(connfd, "SET Teacher LHY\n",     "ok\r\n", "SET-Teacher");
        testcase(connfd, "GET Teacher\n",          "LHY\r\n", "GET-Teacher");
        testcase(connfd, "MOD Teacher Lililihy\n", "ok\r\n", "MOD-Teacher");
        testcase(connfd, "GET Teacher\n",          "Lililihy\r\n", "GET-Teacher");
        testcase(connfd, "EXIST Teacher\n",        "exist\r\n", "EXIST-Teacher");
        testcase(connfd, "DEL Teacher\n",         "ok\r\n", "DEL-Teacher");
        testcase(connfd, "GET Teacher\n",          "no exist\r\n", "GET-Teacher");
        testcase(connfd, "MOD Teacher lhy\n",     "no exist\r\n", "MOD-Teacher");
        testcase(connfd, "EXIST Teacher\n",        "no exist\r\n", "EXIST-Teacher");
    }
    struct timeval tv_cur;
    gettimeofday(&tv_cur, NULL);

    int time_used = TIME_SUB_MS(tv_cur, tv_begin); //ms
    printf("array_testcase -- > time used: %d, qps: %d\n", time_used, 9* count * 1000 / time_used);
}

void rbtree_testcase (int connfd) {
    
    testcase(connfd, "RSET Teacher LHY\n",     "ok\r\n", "RSET-Teacher");
    testcase(connfd, "RGET Teacher\n",          "LHY\r\n", "RGET-Teacher");
    testcase(connfd, "RMOD Teacher Lililihy\n", "ok\r\n", "RMOD-Teacher");
    testcase(connfd, "RGET Teacher\n",          "Lililihy\r\n", "RGET-Teacher");
    testcase(connfd, "REXIST Teacher\n",        "exist\r\n", "REXIST-Teacher");
    testcase(connfd, "RDEL Teacher\n",         "ok\r\n", "RDEL-Teacher");
    testcase(connfd, "RGET Teacher\n",          "no exist\r\n", "RGET-Teacher");
    testcase(connfd, "RMOD Teacher lhy\n",     "no exist\r\n", "RMOD-Teacher");
    testcase(connfd, "REXIST Teacher\n",        "no exist\r\n", "REXIST-Teacher");

}

void hash_testcase (int connfd) {
    
    testcase(connfd, "HSET Teacher LHY\n",     "ok\r\n", "HSET-Teacher");
    testcase(connfd, "HGET Teacher\n",          "LHY\r\n", "HGET-Teacher");
    testcase(connfd, "HMOD Teacher Lililihy\n", "ok\r\n", "HMOD-Teacher");
    testcase(connfd, "HGET Teacher\n",          "Lililihy\r\n", "HGET-Teacher");
    testcase(connfd, "HEXIST Teacher\n",        "exist\r\n", "HEXIST-Teacher");
    testcase(connfd, "HDEL Teacher\n",         "ok\r\n", "HDEL-Teacher");
    testcase(connfd, "HGET Teacher\n",          "no exist\r\n", "HGET-Teacher");
    testcase(connfd, "HMOD Teacher lhy\n",     "no exist\r\n", "HMOD-Teacher");
    testcase(connfd, "HEXIST Teacher\n",        "no exist\r\n", "HEXIST-Teacher");

}
void rbtree_testcase_1w (int connfd) {
    int count = 10000;
    int i = 0;
    struct timeval tv_begin;
	gettimeofday(&tv_begin, NULL);

    for (i = 0; i< count; i++) {
        testcase(connfd, "RSET Teacher LHY",     "ok\r\n", "RSET-Teacher");
        testcase(connfd, "RGET Teacher",          "LHY\r\n", "RGET-Teacher");
        testcase(connfd, "RMOD Teacher Lililihy", "ok\r\n", "RMOD-Teacher");
        testcase(connfd, "RGET Teacher",          "Lililihy\r\n", "RGET-Teacher");
        testcase(connfd, "REXIST Teacher",        "exist\r\n", "REXIST-Teacher");
        testcase(connfd, "RDEL Teacher",         "ok\r\n", "RDEL-Teacher");
        testcase(connfd, "RGET Teacher",          "no exist\r\n", "RGET-Teacher");
        testcase(connfd, "RMOD Teacher lhy",     "no exist\r\n", "RMOD-Teacher");
        testcase(connfd, "REXIST Teacher",        "no exist\r\n", "REXIST-Teacher");
    }
    struct timeval tv_cur;
    gettimeofday(&tv_cur, NULL);

    int time_used = TIME_SUB_MS(tv_cur, tv_begin); //ms
    printf("rbtree_testcase_1w -- > time used: %d, qps: %d\n", time_used, count * 9 * 1000 / time_used);
}

void rbtree_testcase_3w_0_mix (int connfd) {
    int count = 10000;
    int i = 0;
    struct timeval tv_begin;
	gettimeofday(&tv_begin, NULL);

    for (i = 0; i< count; i++) {
        char cmd[128] = {0};
        snprintf(cmd, 128, "RSET Teacher%d King%d\n", i, i);
        testcase(connfd, cmd,     "ok\r\n", "RSET-Teacher");
    }
    for (i = 0; i< count; i++) {
        char cmd[128] = {0};
        snprintf(cmd, 128, "RGET Teacher%d\n", i);

        char result[128] = {0};
        snprintf(result, 128, "King%d\r\n", i);
        testcase(connfd, cmd,     result, "RGET-Teacher");
    }
    for (i = 0; i< count; i++) {
        char cmd[128] = {0};
        snprintf(cmd, 128, "RMOD Teacher%d Lililihy%d\n", i, i);
        testcase(connfd, cmd,     "ok\r\n", "RMOD-Teacher");
    }

    struct timeval tv_cur;
    gettimeofday(&tv_cur, NULL);

    int time_used = TIME_SUB_MS(tv_cur, tv_begin); //ms
    printf("rbtree_testcase_1w -- > time used: %d, qps: %d\n", time_used, count * 3 * 1000 / time_used);
}

void skiptable_testcase (int connfd) {
    
    testcase(connfd, "SSET Teacher LHY\n",     "ok\r\n", "SSET-Teacher");
    testcase(connfd, "SGET Teacher\n",          "LHY\r\n", "SGET-Teacher");
    testcase(connfd, "SMOD Teacher Lililihy\n", "ok\r\n", "SMOD-Teacher");
    testcase(connfd, "SGET Teacher\n",          "Lililihy\r\n", "SGET-Teacher");
    testcase(connfd, "SEXIST Teacher\n",        "exist\r\n", "SEXIST-Teacher");
    testcase(connfd, "SDEL Teacher\n",         "ok\r\n", "SDEL-Teacher");
    testcase(connfd, "SGET Teacher\n",          "no exist\r\n", "SGET-Teacher");
    testcase(connfd, "SMOD Teacher lhy\n",     "no exist\r\n", "SMOD-Teacher");
    testcase(connfd, "SEXIST Teacher\n",        "no exist\r\n", "SEXIST-Teacher");

}

void skiptable_testcase_3w_0_mix (int connfd) {
    int count = 10000;
    int i = 0;
    struct timeval tv_begin;
	gettimeofday(&tv_begin, NULL);

    for (i = 0; i< count; i++) {
        char cmd[128] = {0};
        snprintf(cmd, 128, "SSET Teacher%d King%d\n", i, i);
        testcase(connfd, cmd,     "ok\r\n", "SSET-Teacher");
    }
    for (i = 0; i< count; i++) {
        char cmd[128] = {0};
        snprintf(cmd, 128, "SGET Teacher%d\n", i);

        char result[128] = {0};
        snprintf(result, 128, "King%d\r\n", i);
        testcase(connfd, cmd,     result, "SGET-Teacher");
    }
    for (i = 0; i< count; i++) {
        char cmd[128] = {0};
        snprintf(cmd, 128, "SMOD Teacher%d Lililihy%d\n", i, i);
        testcase(connfd, cmd,     "ok\r\n", "SMOD-Teacher");
    }

    struct timeval tv_cur;
    gettimeofday(&tv_cur, NULL);

    int time_used = TIME_SUB_MS(tv_cur, tv_begin); //ms
    printf("skiptable_testcase_1w -- > time used: %d, qps: %d\n", time_used, count * 3 * 1000 / time_used);
}

void array_testcase_3w_0_mix (int connfd) {
    int count = 10000;
    int i = 0;
    struct timeval tv_begin;
	gettimeofday(&tv_begin, NULL);

    for (i = 0; i< count; i++) {
        char cmd[128] = {0};
        snprintf(cmd, 128, "SET Teacher%d King%d\n", i, i);
        testcase(connfd, cmd,     "ok\r\n", "SET-Teacher");
    }
    for (i = 0; i< count; i++) {
        char cmd[128] = {0};
        snprintf(cmd, 128, "GET Teacher%d\n", i);

        char result[128] = {0};
        snprintf(result, 128, "King%d\r\n", i);
        testcase(connfd, cmd,     result, "GET-Teacher");
    }
    for (i = 0; i< count; i++) {
        char cmd[128] = {0};
        snprintf(cmd, 128, "MOD Teacher%d Lililihy%d\n", i, i);
        testcase(connfd, cmd,     "ok\r\n", "MOD-Teacher");
    }

    struct timeval tv_cur;
    gettimeofday(&tv_cur, NULL);

    int time_used = TIME_SUB_MS(tv_cur, tv_begin); //ms
    printf("array_testcase_1w -- > time used: %d, qps: %d\n", time_used, count * 3 * 1000 / time_used);
}
void hash_testcase_3w_0_mix (int connfd) {
    int count = 10000;
    int i = 0;
    struct timeval tv_begin;
	gettimeofday(&tv_begin, NULL);

    for (i = 0; i< count; i++) {
        char cmd[128] = {0};
        snprintf(cmd, 128, "HSET Teacher%d King%d\n", i, i);
        testcase(connfd, cmd,     "ok\r\n", "HSET-Teacher");
    }
    for (i = 0; i< count; i++) {
        char cmd[128] = {0};
        snprintf(cmd, 128, "HGET Teacher%d\n", i);

        char result[128] = {0};
        snprintf(result, 128, "King%d\r\n", i);
        testcase(connfd, cmd,     result, "HGET-Teacher");
    }
    for (i = 0; i< count; i++) {
        char cmd[128] = {0};
        snprintf(cmd, 128, "HMOD Teacher%d Lililihy%d\n", i, i);
        testcase(connfd, cmd,     "ok\r\n", "HMOD-Teacher");
    }

    struct timeval tv_cur;
    gettimeofday(&tv_cur, NULL);

    int time_used = TIME_SUB_MS(tv_cur, tv_begin); //ms
    printf("hash_testcase_1w -- > time used: %d, qps: %d\n", time_used, count * 3 * 1000 / time_used);
}


/**
 * 统计响应缓冲区中的响应条数
 * 每条响应以 \r\n 结尾
 */
static int count_responses(const char *buffer, int length) {
    if (!buffer || length <= 0) return 0;
    
    int count = 0;
    int i = 0;
    for (i = 0; i < length - 1; i++) {
        if (buffer[i] == '\r' && buffer[i+1] == '\n') {
            count++;
            i++;  // 跳过 \n
        }
    }
    return count;
}
/**
 * 发送批量命令并接收所有响应
 * @param fd          socket 文件描述符
 * @param batch       批量命令数据
 * @param batch_size  命令总长度
 * @param result      输出：接收到的响应数据
 * @param expected_count 期望收到的响应条数（用于判断是否收完）
 * @return 0: 成功, -1: 失败
 */
int send_recv_batch_msg(int fd, const char *batch, int batch_size,
    char *result, int expected_count) {
    if (!batch || !result) return -1;
    int total = 0;
    int sent = 0;
    int MAX_RESP_SIZE = 65535;  // 响应缓冲区大小
    // ===== 阶段1：边发边收 =====
    while (sent < batch_size) {
        int chunk = batch_size - sent;
        if (chunk > 1024) chunk = 1024;
        
        int n = send(fd, batch + sent, chunk, 0);
        if (n <= 0) {
            perror("send");
            return -1;
        }
        sent += n;
        
        // 立即接收一次，腾出接收窗口
        int n2 = recv(fd, result + total, MAX_RESP_SIZE - total - 1, 0);
        if (n2 > 0) {
            total += n2;
        } else if (n2 < 0) {
            perror("recv");
            return -1;
        }
    }

    // ===== 阶段2：收完剩余响应 =====
    while (1) {
        int resp_count = count_responses(result, total);
        if (resp_count >= expected_count) break;
        
        int n = recv(fd, result + total, MAX_RESP_SIZE - total - 1, 0);
        if (n <= 0) {
            if (n < 0) perror("recv");
            break;
        }
        total += n;
    }
    
    result[total] = '\0';
    return 0;
}

void batch_test(int connfd) {
    int count = 1000;
    int i;
    struct timeval tv_begin, tv_cur;
    char batch[500 * 1024] = {0};  // 100KB，足够 500 条
    int batch_size = 0;
    // 预期响应 1500条 22500字节
    char expected[65536] = {0};
    if (mode == 21) {
        // 组装批量命令
        for (i = 0; i < count; i++) {
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "SSET Teacher%d King%d\n", i, i);
            strcat(batch, cmd);
        }
        for (i = 0; i < count; i++) {
            char cmd[32];
            snprintf(cmd, sizeof(cmd), "SGET Teacher%d\n", i);
            strcat(batch, cmd);
        }
        for (i = 0; i < count; i++) {
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "SMOD Teacher%d Lililihy%d\n", i, i);
            strcat(batch, cmd);
        }
        batch_size = strlen(batch);

        for (i = 0; i < count; i++) {
            strcat(expected, "ok\r\n");
        }
        for (i = 0; i < count; i++) {
            char resp[256];
            snprintf(resp, sizeof(resp), "King%d\r\n", i);
            strcat(expected, resp);
        }
        for (i = 0; i < count; i++) {
            strcat(expected, "ok\r\n");
        }
    } else if (mode == 22) {
        // 组装批量命令
        for (i = 0; i < count; i++) {
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "RSET Teacher%d King%d\n", i, i);
            strcat(batch, cmd);
        }
        for (i = 0; i < count; i++) {
            char cmd[32];
            snprintf(cmd, sizeof(cmd), "RGET Teacher%d\n", i);
            strcat(batch, cmd);
        }
        for (i = 0; i < count; i++) {
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "RMOD Teacher%d Lililihy%d\n", i, i);
            strcat(batch, cmd);
        }
        batch_size = strlen(batch);

        for (i = 0; i < count; i++) {
            strcat(expected, "ok\r\n");
        }
        for (i = 0; i < count; i++) {
            char resp[256];
            snprintf(resp, sizeof(resp), "King%d\r\n", i);
            strcat(expected, resp);
        }
        for (i = 0; i < count; i++) {
            strcat(expected, "ok\r\n");
        }
    } else if (mode == 23) {
        // 组装批量命令
        for (i = 0; i < count; i++) {
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "HSET Teacher%d King%d\n", i, i);
            strcat(batch, cmd);
        }
        for (i = 0; i < count; i++) {
            char cmd[32];
            snprintf(cmd, sizeof(cmd), "HGET Teacher%d\n", i);
            strcat(batch, cmd);
        }
        for (i = 0; i < count; i++) {
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "HMOD Teacher%d Lililihy%d\n", i, i);
            strcat(batch, cmd);
        }
        batch_size = strlen(batch);

        for (i = 0; i < count; i++) {
            strcat(expected, "ok\r\n");
        }
        for (i = 0; i < count; i++) {
            char resp[256];
            snprintf(resp, sizeof(resp), "King%d\r\n", i);
            strcat(expected, resp);
        }
        for (i = 0; i < count; i++) {
            strcat(expected, "ok\r\n");
        }
    } else if (mode == 24) {
        // 组装批量命令
        for (i = 0; i < count; i++) {
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "SET Teacher%d King%d\n", i, i);
            strcat(batch, cmd);
        }
        for (i = 0; i < count; i++) {
            char cmd[32];
            snprintf(cmd, sizeof(cmd), "GET Teacher%d\n", i);
            strcat(batch, cmd);
        }
        for (i = 0; i < count; i++) {
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "MOD Teacher%d Lililihy%d\n", i, i);
            strcat(batch, cmd);
        }
        batch_size = strlen(batch);

        for (i = 0; i < count; i++) {
            strcat(expected, "ok\r\n");
        }
        for (i = 0; i < count; i++) {
            char resp[256];
            snprintf(resp, sizeof(resp), "King%d\r\n", i);
            strcat(expected, resp);
        }
        for (i = 0; i < count; i++) {
            strcat(expected, "ok\r\n");
        }
    }
    int expected_count = count * 3;  // 期待的响应数量

    // 发送批量命令并接收响应
    char result[65536] = {0};

    gettimeofday(&tv_begin, NULL);
#if 1
    int ret = send_recv_batch_msg(connfd, batch, batch_size,
                                         result, expected_count);
#else
    // 仿 echo_test 的边发边收：分块发送命令，每发一块就收走已到达的响应，
    // 让客户端接收窗口持续腾空，避免与服务端形成双向阻塞死锁
    int cmd_total = strlen(batch);  //计算批次数据总长度
    int sent = 0;
    while (sent < cmd_total) {  // 如果batch数据没发完就一直发
        int chunk = cmd_total - sent;   // 一个块的大小是还没发的数据总量，如果还没发的数据大于rbuffer长度
        if (chunk > 1024) chunk = 1024;      // 客户端的发送块大小，防止内核wmem满
        int n = send(connfd, batch + sent, chunk, 0); // 待发送数据起始地址，发送一个块的数据过去
        if (n <= 0) { perror("send"); break; }
        sent += n;

        // 发完一个chunk，立即接收，收一次即可腾出接收窗口，剩余响应由收尾循环收满 （缓冲区剩余空间）
        int n2 = recv(connfd, result + total, sizeof(result) - total - 1, 0);
        if (n2 > 0) total += n2;
        else if (n2 < 0) perror("recv");
    }

    // 接收并统计所有响应直到收满预期的响应条数。
    while (1) {
        // 统计 result 缓冲区中有多少个 \r\n 结尾的响应。
        int resp_count = 0;
        int j;
        for (j = 0; j < total - 1; j++) {
            if (result[j] == '\r' && result[j+1] == '\n') {
                resp_count++;
                j++; // 跳过 \n
            }
        }
        if (resp_count >= expected_count) break;  // 收满了

        //此处还需要接收的原因是，第一阶段接受不完所有响应
        // 第一阶段只负责发完，在发完的时段内把缓冲区的数据接收
        // 但是发完后，可能因为客户端拥塞窗口原因，导致还有数据积攒在服务端发送缓冲区，所以还要继续接收
        int n = recv(connfd, result + total, sizeof(result) - total - 1, 0);
        if (n <= 0) {
            if (n < 0) perror("recv");
            break; // 连接关闭或出错
        }
        total += n;
    }
    result[total] = '\0';

#if 0
    char result[65536] = {0};
    recv_msg(connfd, result, sizeof(result));
#endif

#endif
    gettimeofday(&tv_cur, NULL);
    if (ret < 0) {
        printf("send_recv_batch failed\n");
        return;
    }

    int time_used = TIME_SUB_MS(tv_cur, tv_begin);
    printf("%d_batch_1500 -- > time used: %d, qps: %d\n", mode,
           time_used, count * 3 * 1000 / time_used);
    // 验证结果
    if (strcmp(result, expected) == 0) {
        printf("PASS\n");
    } else {
        printf("FAILED\n");
    }
}


// 纯 echo 测试：一次性发送 1..2048 个数字（模拟批量大数据），
// 服务端不做命令解析、原样返回，客户端收全后校验一致性。
void echo_test(int connfd) {
    int count = 10000;
    int cap = 256 * 1024;
    char *sendbuf = (char *)malloc(cap);
    char *recvbuf = (char *)malloc(cap);
    if (!sendbuf || !recvbuf) {
        printf("echo_test: malloc failed\n");
        if (sendbuf) free(sendbuf);
        if (recvbuf) free(recvbuf);
        return;
    }

    // 构造 "1\n2\n3\n...\n2048\n"
    int len = 0;
    int i;
    for (i = 1; i <= count; i++) {
        len += snprintf(sendbuf + len, cap - len, "%d\n", i);
    }

    struct timeval tv_begin, tv_cur;
    gettimeofday(&tv_begin, NULL);

    int sent = 0, total = 0;
    while (sent < len) {
        int chunk = len - sent;
        if (chunk > 1024) chunk = 1024;      // 和服务端 rbuffer 对齐
        int n = send(connfd, sendbuf + sent, chunk, 0);
        if (n <= 0) { perror("send"); break; }
        sent += n;
        // 发多少就收多少回显
        int got = 0;
        while (got < n) {
            int r = recv(connfd, recvbuf + total + got, n - got, 0);
            if (r <= 0) break;
            got += r;
        }
        total += got;
    }

    gettimeofday(&tv_cur, NULL);
    int time_used = TIME_SUB_MS(tv_cur, tv_begin);
    printf("echo_test -- > sent: %d bytes, recv: %d bytes, time used: %d ms\n",
           len, total, time_used);

    if (total == len && memcmp(sendbuf, recvbuf, len) == 0) {
        printf("echo_test -- > PASS\n");
    } else {
        printf("echo_test -- > FAILED (recv %d / %d)\n", total, len);
    }

    free(sendbuf);
    free(recvbuf);
}


void array_large_value_test(int connfd) {
    char key[] = "lhy2w";
    
    // ===== 1. 构建 2048 字节的 value =====
    const int value_len = 20000;
    char *value = (char *)malloc(value_len + 1);
    if (!value) {
        printf("malloc failed\n");
        return;
    }
    memset(value, 'A', value_len);
    value[value_len] = '\0';
    printf("Value size: %d bytes (%.2f KB)\n", value_len, value_len / 1024.0);
    
    // ===== 2. SET =====
    char *cmd = (char *)malloc(value_len + 64);
    int cmd_len = snprintf(cmd, value_len + 64, "SET %s ", key);
    memcpy(cmd + cmd_len, value, value_len);
    cmd_len += value_len;
    cmd[cmd_len++] = '\n';
    cmd[cmd_len] = '\0';
    
    char result[1024] = {0};
    
    send(connfd, cmd, cmd_len, 0);
    recv(connfd, result, sizeof(result), 0);
    printf("SET response: %s", result);
    
    // ===== 3. GET =====
    char get_cmd[32];
    snprintf(get_cmd, sizeof(get_cmd), "GET %s\n", key);
    
    char *get_result = (char *)malloc(value_len + 256);
    memset(get_result, 0, value_len + 256);
    send(connfd, get_cmd, strlen(get_cmd), 0);
    
    int total_recv = 0;
    while (1) {
        int ret = recv(connfd, get_result + total_recv, value_len + 256 - total_recv - 1, 0);
        if (ret <= 0) break;
        total_recv += ret;
        if (total_recv >= 2 && get_result[total_recv-2] == '\r' && get_result[total_recv-1] == '\n') {
            break;
        }
    }
    get_result[total_recv] = '\0';
    
    printf("Total received: %d bytes\n", total_recv);
    printf("Full response:\n%s\n", get_result);
    
    // ===== 4. 验证 =====
    // 去掉末尾的 \r\n
    int len = strlen(get_result);
    if (len >= 2 && get_result[len-2] == '\r' && get_result[len-1] == '\n') {
        get_result[len-2] = '\0';
    }

    if (strcmp(get_result, value) == 0) {
        printf("PASS\n");
    } else {
        printf("FAILED\n");

    }
    printf("Expected len: %d, Received len: %d\n", 
        (int)strlen(value), (int)strlen(get_result));
    free(value);
    free(cmd);
    free(get_result);
}

void hash_large_value_test(int connfd) {
    char key[] = "lhy2w";
    
    // ===== 1. 构建 2048 字节的 value =====
    const int value_len = 20000;
    char *value = (char *)malloc(value_len + 1);
    if (!value) {
        printf("malloc failed\n");
        return;
    }
    memset(value, 'H', value_len);
    value[value_len] = '\0';
    printf("Value size: %d bytes (%.2f KB)\n", value_len, value_len / 1024.0);
    
    // ===== 2. SET =====
    char *cmd = (char *)malloc(value_len + 64);
    int cmd_len = snprintf(cmd, value_len + 64, "HSET %s ", key);
    memcpy(cmd + cmd_len, value, value_len);
    cmd_len += value_len;
    cmd[cmd_len++] = '\n';
    cmd[cmd_len] = '\0';
    
    char result[1024] = {0};
    
    send(connfd, cmd, cmd_len, 0);
    recv(connfd, result, sizeof(result), 0);
    printf("HSET response: %s", result);
    
    // ===== 3. GET =====
    char get_cmd[32];
    snprintf(get_cmd, sizeof(get_cmd), "HGET %s\n", key);
    
    char *get_result = (char *)malloc(value_len + 256);
    memset(get_result, 0, value_len + 256);
    send(connfd, get_cmd, strlen(get_cmd), 0);
    
    int total_recv = 0;
    while (1) {
        int ret = recv(connfd, get_result + total_recv, value_len + 256 - total_recv - 1, 0);
        if (ret <= 0) break;
        total_recv += ret;
        if (total_recv >= 2 && get_result[total_recv-2] == '\r' && get_result[total_recv-1] == '\n') {
            break;
        }
    }
    get_result[total_recv] = '\0';
    
    printf("Total received: %d bytes\n", total_recv);
    printf("Full response:\n%s\n", get_result);
    
    // ===== 4. 验证 =====
    // 去掉末尾的 \r\n
    int len = strlen(get_result);
    if (len >= 2 && get_result[len-2] == '\r' && get_result[len-1] == '\n') {
        get_result[len-2] = '\0';
    }

    if (strcmp(get_result, value) == 0) {
        printf("PASS\n");
    } else {
        printf("FAILED\n");

    }
    printf("Expected len: %d, Received len: %d\n", 
        (int)strlen(value), (int)strlen(get_result));
    free(value);
    free(cmd);
    free(get_result);
}

void rbtree_large_value_test(int connfd) {
    char key[] = "lhy2w";
    
    // ===== 1. 构建 2048 字节的 value =====
    const int value_len = 20000;
    char *value = (char *)malloc(value_len + 1);
    if (!value) {
        printf("malloc failed\n");
        return;
    }
    memset(value, 'R', value_len);
    value[value_len] = '\0';
    printf("Value size: %d bytes (%.2f KB)\n", value_len, value_len / 1024.0);
    
    // ===== 2. SET =====
    char *cmd = (char *)malloc(value_len + 64);
    int cmd_len = snprintf(cmd, value_len + 64, "RSET %s ", key);
    memcpy(cmd + cmd_len, value, value_len);
    cmd_len += value_len;
    cmd[cmd_len++] = '\n';
    cmd[cmd_len] = '\0';
    
    char result[1024] = {0};
    
    send(connfd, cmd, cmd_len, 0);
    recv(connfd, result, sizeof(result), 0);
    printf("RSET response: %s", result);
    
    // ===== 3. GET =====
    char get_cmd[32];
    snprintf(get_cmd, sizeof(get_cmd), "RGET %s\n", key);
    
    char *get_result = (char *)malloc(value_len + 256);
    memset(get_result, 0, value_len + 256);
    send(connfd, get_cmd, strlen(get_cmd), 0);
    
    int total_recv = 0;
    while (1) {
        int ret = recv(connfd, get_result + total_recv, value_len + 256 - total_recv - 1, 0);
        if (ret <= 0) break;
        total_recv += ret;
        if (total_recv >= 2 && get_result[total_recv-2] == '\r' && get_result[total_recv-1] == '\n') {
            break;
        }
    }
    get_result[total_recv] = '\0';
    
    printf("Total received: %d bytes\n", total_recv);
    printf("Full response:\n%s\n", get_result);
    
    // ===== 4. 验证 =====
    // 去掉末尾的 \r\n
    int len = strlen(get_result);
    if (len >= 2 && get_result[len-2] == '\r' && get_result[len-1] == '\n') {
        get_result[len-2] = '\0';
    }

    if (strcmp(get_result, value) == 0) {
        printf("PASS\n");
    } else {
        printf("FAILED\n");

    }
    printf("Expected len: %d, Received len: %d\n", 
        (int)strlen(value), (int)strlen(get_result));
    free(value);
    free(cmd);
    free(get_result);
}


void skiptable_large_value_test(int connfd) {
    char key[] = "lhy2w";
    
    // ===== 1. 构建 2048 字节的 value =====
    const int value_len = 20000;
    char *value = (char *)malloc(value_len + 1);
    if (!value) {
        printf("malloc failed\n");
        return;
    }
    memset(value, 'S', value_len);
    value[value_len] = '\0';
    printf("Value size: %d bytes (%.2f KB)\n", value_len, value_len / 1024.0);
    
    // ===== 2. SET =====
    char *cmd = (char *)malloc(value_len + 64);
    int cmd_len = snprintf(cmd, value_len + 64, "SSET %s ", key);
    memcpy(cmd + cmd_len, value, value_len);
    cmd_len += value_len;
    cmd[cmd_len++] = '\n';
    cmd[cmd_len] = '\0';
    
    char result[1024] = {0};
    
    send(connfd, cmd, cmd_len, 0);
    recv(connfd, result, sizeof(result), 0);
    printf("SSET response: %s", result);
    
    // ===== 3. GET =====
    char get_cmd[32];
    snprintf(get_cmd, sizeof(get_cmd), "SGET %s\n", key);
    
    char *get_result = (char *)malloc(value_len + 256);
    memset(get_result, 0, value_len + 256);
    send(connfd, get_cmd, strlen(get_cmd), 0);
    
    int total_recv = 0;
    while (1) {
        int ret = recv(connfd, get_result + total_recv, value_len + 256 - total_recv - 1, 0);
        if (ret <= 0) break;
        total_recv += ret;
        if (total_recv >= 2 && get_result[total_recv-2] == '\r' && get_result[total_recv-1] == '\n') {
            break;
        }
    }
    get_result[total_recv] = '\0';
    
    printf("Total received: %d bytes\n", total_recv);
    printf("Full response:\n%s\n", get_result);
    
    // ===== 4. 验证 =====
    // 去掉末尾的 \r\n
    int len = strlen(get_result);
    if (len >= 2 && get_result[len-2] == '\r' && get_result[len-1] == '\n') {
        get_result[len-2] = '\0';
    }

    if (strcmp(get_result, value) == 0) {
        printf("PASS\n");
    } else {
        printf("FAILED\n");

    }
    printf("Expected len: %d, Received len: %d\n", 
        (int)strlen(value), (int)strlen(get_result));
    free(value);
    free(cmd);
    free(get_result);
}


void test_resp_batch(int connfd) {
    // 发送 RESP 批量命令
    const char *cmd = 
        "*10\r\n"
        "*3\r\n$3\r\nSET\r\n$6\r\nlhyyyy\r\n$9\r\n123456789\r\n"
        "*3\r\n$3\r\nSET\r\n$6\r\nlhyyyy\r\n$10\r\n1234567891\r\n"
        "*2\r\n$3\r\nGET\r\n$6\r\nlhyyyy\r\n"
        "*3\r\n$3\r\nMOD\r\n$6\r\nlhyyyy\r\n$8\r\nLILILILI\r\n"
        "*2\r\n$3\r\nGET\r\n$6\r\nlhyyyy\r\n"
        "*2\r\n$5\r\nEXIST\r\n$6\r\nlhyyyy\r\n"
        "*2\r\n$3\r\nDEL\r\n$6\r\nlhyyyy\r\n"
        "*2\r\n$3\r\nGET\r\n$6\r\nlhyyyy\r\n"
        "*3\r\n$3\r\nMOD\r\n$6\r\nlhyyyy\r\n$9\r\nLILILILI2\r\n"
        "*2\r\n$5\r\nEXIST\r\n$6\r\nlhyyyy\r\n";
    
    printf("Sending: %s\n", cmd);
    send(connfd, cmd, strlen(cmd), 0);
    
    // 接收响应
    char result[1024] = {0};
    int n = recv(connfd, result, sizeof(result) - 1, 0);
    if (n > 0) {
        result[n] = '\0';
        printf("Response: %s\n", result);
    } else {
        printf("recv failed\n");
    }
}


// testcase 192.168.137.128 2000
int main(int argc, char *argv[]) {

    if (argc != 4) {
         printf("argc error\n");
         return -1;
    }

    char *ip = argv[1];
    int port = atoi(argv[2]);
    mode = atoi(argv[3]);
    int connfd = connect_tcpserver(ip, port);

    // rbtree_testcase(connfd);
    
    // 基本插入测试
    if (mode == 1) {
        array_testcase_1w(connfd);  // 正常
    } else if (mode ==2) {
        rbtree_testcase(connfd);    // 正常
    } else if (mode ==3) {
        hash_testcase(connfd);      // 正常
    } else if (mode == 4) {
        skiptable_testcase(connfd); // 正常

    // 单条发送的命令测试
    } else if (mode == 11) {
        rbtree_testcase_3w_0_mix(connfd);   // 正常 QPS:5000
    } else if (mode == 12) {
        skiptable_testcase_3w_0_mix(connfd);    // 正常 QPS:4700
    } else if (mode == 13) {
        array_testcase_3w_0_mix(connfd);
    } else if (mode == 14) {
        hash_testcase_3w_0_mix(connfd);
    
// 批数据测试      跳表         红黑树          哈希        数组
    } else if (mode == 21 || mode == 22 || mode == 23 || mode == 24) {    
        batch_test(connfd); 
    }
    
    else if (mode == 31) {    // echo 收发完整性测试
        echo_test(connfd);
    }
    
    // 测试超长kv通过性
    else if (mode == 41) {    
        array_large_value_test(connfd);
    } else if (mode == 42) {   
        hash_large_value_test(connfd);
    } else if (mode == 43) {    
        rbtree_large_value_test(connfd);
    } else if (mode == 44) {   
        skiptable_large_value_test(connfd);
    }
    else if (mode == 51) {   
        test_resp_batch(connfd);
    }

    
    return 0;

}