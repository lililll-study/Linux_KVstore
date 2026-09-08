#include <stdio.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/time.h>
#include <stdlib.h>

#include "server.h"
#include "kvstore.h"

#include <openssl/sha.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <openssl/evp.h>

#include <fcntl.h>
/* 
连接大小：读缓存1K，写缓存1K，2K* = 2M
*/
#define CONNECTION_SIZE     1024 // 1024^2
#define MAX_PORTs           1
#define ENABLE_NETWORK_TEST 0

#define TIME_SUB_MS(tv1, tv2)  ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)

int epfd = 0;
struct timeval begin;

// struct conn conn_list[CONNECTION_SIZE] = {0};
// 优化为动态分配内存
struct conn conn_list[CONNECTION_SIZE];


int accept_cb(int fd);
int recv_cb(int fd);
int send_cb(int fd);
int event_register(int fd, int event);
int set_event(int fd, int event, int flag);

#if ENABLE_KVS

typedef int (*msg_handler)(int fd, char *msg, int length, char *response);
static msg_handler kvs_handler;

int kvs_request(struct conn *c) {

    c-> wlength = kvs_handler(c->fd, c->rbuffer, c->rlength, c->wbuffer);
    // printf("%d\n", c->wlength);
    return 0;
}

#if ENABLE_REPBUF

int kvs_response(struct conn *c) {
    kvs_conn_ctx_t *ctx = &kvs_conn_ctxs[c->fd];
    // printf("debug 3: 准备回发响应\n");
    
    // 检查是否有数据待发送
    if (resp_buf_has_data(ctx)) {
        int data_len = 0;
        // printf("debug 4: 检测到回发数据\n");
        char *data = resp_buf_get_data(ctx, &data_len);
        
        if (data && data_len > 0) {
            int chunk = data_len > 2000 ? 2000 : data_len;
            int n = send(c->fd, data, chunk, 0);
            // printf("debug 5: 回发成功, 发送了 %d 字节\n", n);
            if (n < 0) {
                perror("send");
                close(c->fd);
                epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
                cleanup_connection(c->fd);
                return -1;
            }
            // 标记数据已发送
            resp_buf_advance(ctx, n);
            
            // 如果还有数据未发送，保持 EPOLLOUT 事件
            if (resp_buf_has_data(ctx)) {
                // printf("debug 6: 还有数据未发完，保持EPOLLOUT\n");
                set_event(c->fd, EPOLLOUT, 0);
            } else {
                // 数据全部发送完成，重置状态，切换回 EPOLLIN
                ctx->send_pending = 0;
                set_event(c->fd, EPOLLIN, 0);
            }
            return n;
        }
    }
    
    // 没有数据待发送，重置状态并切换回 EPOLLIN
    ctx->send_pending = 0;
    set_event(c->fd, EPOLLIN, 0);
    return 0;
}

#endif
#endif




int set_event(int fd, int event, int flag) {

    if (flag) { // 1:添加
        struct epoll_event ev;
        ev.events = event;
        ev.data.fd = fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    } else {
        struct epoll_event ev;
        ev.events = event;
        ev.data.fd = fd;
        epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
    }
    return 0;
}


int event_register(int fd, int event) {

    if (fd < 0) return -1;
    // 为新连接设置不同回调函数以及信息
    conn_list[fd].fd = fd;
    conn_list[fd].r_action.recv_callback = recv_cb;
    conn_list[fd].send_callback = send_cb;

    memset(conn_list[fd].rbuffer, 0, BUFFER_LENGTH);
    conn_list[fd].rlength = 0;

#if ENABLE_REPBUF
     // 不用wbuffer，用resp缓冲区替代wbuffer
#else
    memset(conn_list[fd].wbuffer, 0, BUFFER_LENGTH);
    conn_list[fd].wlength = 0;
    return 0;

#endif
    set_event(fd, event, 1);
    return 0;
}


// listenfd(sockfd) --> EPOLLIN --> accept_cb
int accept_cb(int fd) {

    struct sockaddr_in client_addr;
    memset(&client_addr, 0, sizeof(struct sockaddr_in));
    socklen_t client_len = sizeof(client_addr);

    int clientfd = accept(fd, (struct sockaddr*)&client_addr, &client_len);
    // printf("accept finished clientfd: %d\n", clientfd);
    if (clientfd < 0) {
        printf("accept error: %d --> %s\n", errno, strerror(errno));
        return -1;
    }
#if ENABLE_NOBLOCK
    // 设置非阻塞
    fcntl(clientfd, F_SETFL, fcntl(clientfd, F_GETFL) | O_NONBLOCK);
#endif
    event_register(clientfd, EPOLLIN);

    if ((clientfd % 1000) == 0) {

        struct timeval current;
        gettimeofday(&current, NULL);

        // int time_used = TIME_SUB_MS(current, begin);
        memcpy(&begin, &current, sizeof(struct timeval));

        // printf("accept finished clientfd: %d, time_used: %d\n", clientfd, time_used);

    }
    return 0;
}


int recv_cb(int fd) {
    // struct conn *c = &conn_list[fd];
    memset(conn_list[fd].rbuffer, 0, BUFFER_LENGTH );

    // 阻塞写法
    int data_len = recv(fd, conn_list[fd].rbuffer, BUFFER_LENGTH, 0);
    if (data_len == 0) { // 断开连接了
        // close(clientfd);
        printf("client disconnect: %d\n", fd);
        close(fd);
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);   // unfinished 
        cleanup_connection(fd);
        return 0;
    } else if (data_len < 0) {
        printf("count: %d, errno: %d, %s\n", data_len, errno, strerror(errno));
        close(fd);
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);

        return 0;
    }
    if (data_len > 0) {// 确保字符以\0结尾
        conn_list[fd].rbuffer[data_len] = '\0';
    }
    conn_list[fd].rlength = data_len;
    // printf("recv buffer: %s\n", conn_list[fd].rbuffer);

#if 0 // echo
    conn_list[fd].wlength = conn_list[fd].rlength;
    memcpy(conn_list[fd].wbuffer, conn_list[fd].rbuffer, conn_list[fd].wlength);
    printf("[%d]recv: %s\n", conn_list[fd].rlength, conn_list[fd].rbuffer);
#endif



#if ENABLE_KVS

#if ENABLE_NETWORK_TEST
    // echo 模式：不做命令解析，把收到的数据原样返回
    conn_list[fd].wlength = conn_list[fd].rlength;
    memcpy(conn_list[fd].wbuffer, conn_list[fd].rbuffer, conn_list[fd].wlength);
    conn_list[fd].wbuffer[conn_list[fd].wlength] = '\0';
#else
    kvs_request(&conn_list[fd]);
    // printf("============waiting for send\n");
    // printf("content: %s\n", conn_list[fd].wbuffer);
#endif


#endif
    set_event(fd, EPOLLOUT, 0);
    return data_len;
}

int send_cb(int fd) {

#if ENABLE_REPBUF
    return kvs_response(&conn_list[fd]);
#else

#if ENABLE_KVS
#endif
    int send_len = 0;
	if (conn_list[fd].wlength != 0) {

        printf("wbuffer : %s\n", conn_list[fd].wbuffer);
        // printf("==============end===================\n");
		send_len = send(fd, conn_list[fd].wbuffer, conn_list[fd].wlength, 0);
        conn_list[fd].wlength = 0;
        
	}

    return send_len;
#endif
}


int init_server(unsigned short port) {

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
// 设置端口复用，允许立即重用端口
    int reuse = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        printf("setsockopt SO_REUSEADDR failed: %s\n", strerror(errno));
    }
    struct sockaddr_in servaddr;
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(port);//0-1023不能用，要大于1024
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY); //0.0.0.0

    if (-1 == bind(sockfd, (struct sockaddr*)&servaddr, sizeof(struct sockaddr_in))) {
        printf("bind faild: %s\n", strerror(errno));
    }

    listen(sockfd, 1024);
    // printf("listen finished sockfd: %d\n", sockfd);

    return sockfd;
}


int reactor_start(unsigned short port, msg_handler handler) {
    // unsigned short port = 2000;
    // 传入业务函数kvs_protocol作为函数指针，给kvs_request调用
    /*
    这里写成函数参数有三点原因：
        1 让reactor成为通用组件，如果直接extern定义函数，就写死了，那么 reactor.c 就变成了“KV 存储服务器专用”的网络库。
        2 用函数指针后，reactor.c 根本不知道 kvs_protocol 的存在。它只知道有一个类型叫 msg_handler 的回调。
        3 在软件架构中，有一个原则：高层模块不应依赖低层模块，二者都应依赖其抽象。
        通过函数指针 msg_handler，我们定义了一个“抽象接口”。两者互不包含，完全隔离。

    */
    kvs_handler = handler;
    epfd = epoll_create(1);


    int i = 0;
// =============
    // 初始化所有连接
    for (i = 0; i < CONNECTION_SIZE; i++) {
        memset(&conn_list[i], 0, sizeof(struct conn));
        conn_list[i].fd = -1;
    }
    
    // 注意：fd 可能大于 CONNECTION_SIZE，所以还需要检查
    int sockfd = init_server(port);
    if (sockfd < 0) {
        printf("Failed to initialize server on port %d\n", port);
        return -1;
    }
    if (sockfd >= CONNECTION_SIZE) {
        printf("fd %d >= CONNECTION_SIZE %d\n", sockfd, CONNECTION_SIZE);
        return -1;
    }

    // conn_list = (struct conn *)malloc(CONNECTION_SIZE * sizeof(struct conn));
// =============



    for (i = 0; i < MAX_PORTs; i++){
        // int sockfd = init_server(port + i); 
        conn_list[sockfd].fd = sockfd;
        conn_list[sockfd].r_action.recv_callback = accept_cb;

        set_event(sockfd, EPOLLIN, 1);
    }

    gettimeofday(&begin, NULL);


    while(1) { // mainloop

        struct epoll_event events[1024];
        memset(events, 0, sizeof(events));
        int nready = epoll_wait(epfd, events, 1024, -1);

        int i = 0;
        for (i=0; i<nready; i++) {
            int connectionfd = events[i].data.fd;

            // 不用else if是因为，IO可能同时存在IN和OUT事件
            if (events[i].events & EPOLLIN) {
                conn_list[connectionfd].r_action.recv_callback(connectionfd);
            }
            if (events[i].events & EPOLLOUT) {
                conn_list[connectionfd].send_callback(connectionfd);
            }
        }
    }
}









