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

/* 
连接大小：读缓存1K，写缓存1K，2K* = 2M
*/


#define CONNECTION_SIZE     1024 // 1024^2
#define MAX_PORTs           1

#define TIME_SUB_MS(tv1, tv2)  ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)

#if ENABLE_KVS

typedef int (*msg_handler)(char *msg, int length, char *response);

static msg_handler kvs_handler;

int kvs_request(struct conn *c) {

    // printf("recv %d: %s\n", c->rlength, c->rbuffer);
    c-> wlength = kvs_handler(c->rbuffer, c->rlength, c->wbuffer);
    return 0;

}


int kvs_response(struct conn *c) {

    return 0;
}


#endif



int accept_cb(int fd);
int recv_cb(int fd);
int send_cb(int fd);
int event_register(int fd, int event);



int epfd = 0;
struct timeval begin;




// struct conn conn_list[CONNECTION_SIZE] = {0};
// 优化为动态分配内存
struct conn conn_list[CONNECTION_SIZE] = {0};


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

}


int event_register(int fd, int event) {

    if (fd < 0) return -1;
    // 为新连接设置不同回调函数以及信息
    conn_list[fd].fd = fd;
    conn_list[fd].r_action.recv_callback = recv_cb;
    conn_list[fd].send_callback = send_cb;

    memset(conn_list[fd].rbuffer, 0, BUFFER_LENGTH);
    conn_list[fd].rlength = 0;

    memset(conn_list[fd].wbuffer, 0, BUFFER_LENGTH);
    conn_list[fd].wlength = 0;

    set_event(fd, event, 1);
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

    event_register(clientfd, EPOLLIN);

    if ((clientfd % 1000) == 0) {

        struct timeval current;
        gettimeofday(&current, NULL);

        int time_used = TIME_SUB_MS(current, begin);
        memcpy(&begin, &current, sizeof(struct timeval));

        printf("accept finished clientfd: %d, time_used: %d\n", clientfd, time_used);

    }
    return 0;
}


int recv_cb(int fd) {

    memset(conn_list[fd].rbuffer, 0, BUFFER_LENGTH);
    int data_len = recv(fd, conn_list[fd].rbuffer, BUFFER_LENGTH, 0);

    if (data_len == 0) { // 断开连接了
        // close(clientfd);
        printf("client disconnect: %d\n", fd);
        close(fd);
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);   // unfinished 

        return 0;
    } else if (data_len < 0) {
        printf("count: %d, errno: %d, %s\n", data_len, errno, strerror(errno));
        close(fd);
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);

        return 0;
    }

    conn_list[fd].rlength = data_len;
    // printf("recv buffer: %s\n", conn_list[fd].rbuffer);

#if 0 // echo
    conn_list[fd].wlength = conn_list[fd].rlength;
    memcpy(conn_list[fd].wbuffer, conn_list[fd].rbuffer, conn_list[fd].wlength);
    printf("[%d]recv: %s\n", conn_list[fd].rlength, conn_list[fd].rbuffer);

#elif ENABLE_HTTP
// 接受完数据后做request请求
    http_requset(&conn_list[fd]);

#elif ENABLE_WS
    ws_request(&conn_list[fd]);

#elif ENABLE_KVS

    kvs_request(&conn_list[fd]);

#endif
    set_event(fd, EPOLLOUT, 0);

    return data_len;
}

//1 不同事件触发不同回调
//2 rbuffer, wbuffer





int send_cb(int fd) {



#if ENABLE_HTTP
// 发送数据之前做response响应
    http_response(&conn_list[fd]);

#elif ENABLE_WS
    ws_response(&conn_list[fd]);

#elif ENABLE_KVS

    kvs_response(&conn_list[fd]);

#endif

    int send_len = 0;

#if 0

    if (conn_list[fd].status == 1) {
        // printf("send buffer: %s\n", conn_list[fd].wbuffer);
        send_len = send(fd, conn_list[fd].wbuffer, conn_list[fd].wlength, 0);
        set_event(fd, EPOLLOUT, 0);
    } else if (conn_list[fd].status == 2) {
        set_event(fd, EPOLLOUT, 0);
    } else if (conn_list[fd].status == 0) {
        set_event(fd, EPOLLIN, 0);
    }

#else

	if (conn_list[fd].wlength != 0) {
		send_len = send(fd, conn_list[fd].wbuffer, conn_list[fd].wlength, 0);
	}
	
	set_event(fd, EPOLLIN, 0);

#endif

    return send_len;
}


int init_server(unsigned short port) {

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);

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

    // conn_list = (struct conn *)malloc(CONNECTION_SIZE * sizeof(struct conn));
    if (!conn_list) {
        printf("malloc conn_list failed\n");
        return -1;
    }

    int i = 0;

    for (i = 0; i < MAX_PORTs; i++){
        int sockfd = init_server(port + i); 
        conn_list[sockfd].fd = sockfd;
        conn_list[sockfd].r_action.recv_callback = accept_cb;

        set_event(sockfd, EPOLLIN, 1);
    }

    gettimeofday(&begin, NULL);


    while(1) { // mainloop

        struct epoll_event events[1024] = {0};
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









