
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/time.h>


#define MAX_MSG_LEN 1024
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
        printf("==> PASS -> %s\n", casename);
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
        testcase(connfd, "SET Teacher LHY",     "ok\r\n", "SET-Teacher");
        testcase(connfd, "GET Teacher",          "LHY\r\n", "GET-Teacher");
        testcase(connfd, "MOD Teacher Lililihy", "ok\r\n", "MOD-Teacher");
        testcase(connfd, "GET Teacher",          "Lililihy\r\n", "GET-Teacher");
        testcase(connfd, "EXIST Teacher",        "exist\r\n", "EXIST-Teacher");
        testcase(connfd, "DEL Teacher",         "ok\r\n", "DEL-Teacher");
        testcase(connfd, "GET Teacher",          "no exist\r\n", "GET-Teacher");
        testcase(connfd, "MOD Teacher lhy",     "no exist\r\n", "MOD-Teacher");
        testcase(connfd, "EXIST Teacher",        "no exist\r\n", "EXIST-Teacher");
    }
    struct timeval tv_cur;
    gettimeofday(&tv_cur, NULL);

    int time_used = TIME_SUB_MS(tv_cur, tv_begin); //ms
    printf("array_testcase -- > time used: %d, qps: %d\n", time_used, 9* count * 1000 / time_used);
}

void rbtree_testcase (int connfd) {
    
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

void hash_testcase (int connfd) {
    
    testcase(connfd, "HSET Teacher LHY",     "ok\r\n", "HSET-Teacher");
    testcase(connfd, "HGET Teacher",          "LHY\r\n", "HGET-Teacher");
    testcase(connfd, "HMOD Teacher Lililihy", "ok\r\n", "HMOD-Teacher");
    testcase(connfd, "HGET Teacher",          "Lililihy\r\n", "HGET-Teacher");
    testcase(connfd, "HEXIST Teacher",        "exist\r\n", "HEXIST-Teacher");
    testcase(connfd, "HDEL Teacher",         "ok\r\n", "HDEL-Teacher");
    testcase(connfd, "HGET Teacher",          "no exist\r\n", "HGET-Teacher");
    testcase(connfd, "HMOD Teacher lhy",     "no exist\r\n", "HMOD-Teacher");
    testcase(connfd, "HEXIST Teacher",        "no exist\r\n", "HEXIST-Teacher");

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
        snprintf(cmd, 128, "RSET Teacher%d King%d", i, i);
        testcase(connfd, cmd,     "ok\r\n", "RSET-Teacher");
    }
    for (i = 0; i< count; i++) {
        char cmd[128] = {0};
        snprintf(cmd, 128, "RGET Teacher%d", i);

        char result[128] = {0};
        snprintf(result, 128, "King%d\r\n", i);
        testcase(connfd, cmd,     result, "RGET-Teacher");
    }
    for (i = 0; i< count; i++) {
        char cmd[128] = {0};
        snprintf(cmd, 128, "RMOD Teacher%d Lililihy%d", i, i);
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
    
    // 9w次往返数据
    if (mode == 0) {
        rbtree_testcase_1w(connfd);
    } else if (mode ==1) {
        array_testcase_1w(connfd);
    } else if (mode ==2) {
        rbtree_testcase_3w_0_mix(connfd);
    } else if (mode == 3) {
        hash_testcase(connfd);
    }

    
    return 0;

}