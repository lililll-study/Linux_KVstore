
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <errno.h>      // ← 添加这行
#include <unistd.h>

#define MAX_MSG_LEN 4096
#define TIME_SUB_MS(tv1, tv2)  ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)


int send_msg(int connfd, char *msg, int length) {
    
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

        // testcase(connfd, "RMOD Teacher Lililihy", "ok\r\n", "RMOD-Teacher");
        // testcase(connfd, "RGET Teacher",          "Lililihy\r\n", "RGET-Teacher");
        // testcase(connfd, "REXIST Teacher",        "exist\r\n", "REXIST-Teacher");
        // testcase(connfd, "RDEL Teacher",         "ok\r\n", "RDEL-Teacher");
        // testcase(connfd, "RGET Teacher",          "no exist\r\n", "RGET-Teacher");
        // testcase(connfd, "RMOD Teacher lhy",     "no exist\r\n", "RMOD-Teacher");
        // testcase(connfd, "REXIST Teacher",        "no exist\r\n", "REXIST-Teacher");



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


void skiptable_batch_1500(int connfd) {
    int count = 80;
    int i;
    struct timeval tv_begin, tv_cur;
    // 组装批量命令
    // 估算：每条命令最多 64 字节，500 条 × 3 个循环 = 1500 条 最多96KB
    char batch[500 * 1024] = {0};  // 100KB，足够 500 条

    for (i = 0; i < count; i++) {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "SSET Teacher%d King%d\n", i, i);
        strcat(batch, cmd);
    }
    for (i = 0; i < count; i++) {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "SGET Teacher%d\n", i);
        strcat(batch, cmd);
    }
    for (i = 0; i < count; i++) {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "SMOD Teacher%d Lililihy%d\n", i, i);
        strcat(batch, cmd);
    }

    
    // 预期响应 1500条 22500字节
    char expected[65536] = {0};
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
    
    // 发送批量命令并接收响应
    gettimeofday(&tv_begin, NULL);
    send_msg(connfd, batch, strlen(batch));

#if 1
    char result[65536] = {0};
    int total = 0;
    int expected_count = count * 3;  // 150条响应

    while (1) {
        // 统计当前已收到的响应条数（每条以 \r\n 结尾）
        int resp_count = 0;
        int j;
        for (j = 0; j < total - 1; j++) {
            if (result[j] == '\r' && result[j+1] == '\n') {
                resp_count++;
                j++; // 跳过 \n
            }
        }
        if (resp_count >= expected_count) break;  // 收满了

        int n = recv(connfd, result + total, sizeof(result) - total - 1, 0);
        if (n <= 0) {
            if (n < 0) perror("recv");
            break; // 连接关闭或出错
        }
        total += n;
    }
    result[total] = '\0';

#else
    char result[65536] = {0};
    recv_msg(connfd, result, sizeof(result));


#endif

    gettimeofday(&tv_cur, NULL);
    
    printf("\n--- result ---\n");
    printf("%s", result);
    printf("\n--- expected ---\n");
    printf("%s", expected);
    // 验证结果
    if (strcmp(result, expected) == 0) {
        printf("PASS\n");
    } else {
        printf("FAILED\n");
    }
    
    int time_used = TIME_SUB_MS(tv_cur, tv_begin);
    printf("skiptable_batch_1500 -- > time used: %d, qps: %d\n", 
           time_used, count * 3 * 1000 / time_used);
}


// testcase 192.168.137.128 2000
int main(int argc, char *argv[]) {

    if (argc != 4) {
         printf("argc error\n");
         return -1;
    }

    char *ip = argv[1];
    int port = atoi(argv[2]);
    int mode = atoi(argv[3]);
    int connfd = connect_tcpserver(ip, port);

    // rbtree_testcase(connfd);
    
    // 普通功能测试
    if (mode == 1) {
        array_testcase_1w(connfd);  // 正常
    } else if (mode ==2) {
        rbtree_testcase(connfd);    // 正常
    } else if (mode ==3) {
        hash_testcase(connfd);      // 正常
    } else if (mode == 4) {
        skiptable_testcase(connfd); // 正常

    // 插入量测试  
    } else if (mode == 11) {
        rbtree_testcase_3w_0_mix(connfd);   // 正常 QPS:5000
    } else if (mode == 12) {
        skiptable_testcase_3w_0_mix(connfd);    // 正常 QPS:4700
    
    // 批数据测试    
    } else if (mode == 21) {
        skiptable_batch_1500(connfd); 
    }

    
    return 0;

}