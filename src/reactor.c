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

#if ENABLE_KVS

typedef int (*msg_handler)(int fd, char *msg, int length, char *response);
static msg_handler kvs_handler;

int kvs_request(struct conn *c) {
    // printf("recv %d: %s\n", c->rlength, c->rbuffer);
    printf("===============recved=======================\n");
    c-> wlength = kvs_handler(c->fd, c->rbuffer, c->rlength, c->wbuffer);
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
struct conn conn_list[CONNECTION_SIZE];


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
    // return 0;
}


#if 0
===仿照redis设计的查询缓冲区===
初始化查询缓冲区
int init_querybuf(struct conn *c) {
    c->querybuf_cap = BUFFER_LENGTH * 2;    // 初始化为128kb
    c->querybuf = (char *)malloc(c->querybuf_cap);
    if (!c->querybuf) return -1;
    c->querybuf_len = 0;
    return 0;
}
// 释放查缓冲区
void free_querybuf(struct conn *c) {
    if (c->querybuf) {
        free(c->querybuf);
        c->querybuf = NULL;
    }
    c->querybuf_len = 0;
    c->querybuf_cap = 0;
}
// 动态拓展查询缓冲区
int expand_querybuf(struct conn *c, int need) {
    if (c->querybuf_cap >= need) return 0;

    int new_cap = c->querybuf_cap * 2;
    while (new_cap < need) new_cap *= 2; //@@为什么不直接赋值

    char *new_buf = (char *)realloc(c->querybuf, new_cap);
    if (!new_buf) return -1;
    c->querybuf = new_buf;
    c->querybuf_cap = new_cap;
    return 0;

}


// 从缓冲区拿出已经处理的数据
void querybuf_consume(struct conn *c, int consumed) {
    if (consumed <= 0) return;
    if (consumed >= c->querybuf_len) {
        c->querybuf_len = 0;
        return;
    }
    memmove(c->querybuf, c->querybuf + consumed, c->querybuf_len - consumed); //@@为什么不直接赋值
    c->querybuf_len -= consumed;
}

// 处理缓冲区数据（解析完整命令）
void process_buffer(int fd) {

#if 0
    struct conn *c = &conn_list[fd];
    if (c->querybuf_len == 0) return;

    int cmd_count = 0;
    int i = 0;
    for (i=0; i<c->querybuf_len; i++) {
        if (c->querybuf[i] == '\n') cmd_count++;
    }

    // 没有完整命令
    if (cmd_count == 0) return;

    int batch_size = 1000; // 最多攒100条就发给kvs_requeset处理
    char *last_newline = c->querybuf; // 指向 querybuf 中最后一个换行符 \n 的位置。
    int used_cmds = 0; // 读过的命令数量

    for (i = 0; i<c->querybuf_len && used_cmds < batch_size; i++) {
        if (c->querybuf[i] == '\n') {
            last_newline = c->querybuf + i;
            used_cmds++;
        }
    }

    int batch_cmd_length = last_newline - c->querybuf + 1;  // 批命令的完整长度
    memcpy(c->rbuffer, c->querybuf, batch_cmd_length);// 把数据复制到rbuffer中，方便kvs处理，剩余没处理的数据仍然存储在querybuf中
    // printf("rbuffer_batch: %s\n", c->rbuffer);
    c->rbuffer[batch_cmd_length] = '\0';
    c->rlength = batch_cmd_length;


    kvs_request(c);
    if (batch_cmd_length > 0) {
        querybuf_consume(c, batch_cmd_length);
    }
#else
    struct conn *c = &conn_list[fd];

    while (c->querybuf_len > 0) {

        // 0. wbuffer 快满了，先停下，等 send_cb 发完再继续
        //    （kvs_request 内部也有检查，这里是双保险）
        if (c->wlength >= BUFFER_LENGTH - 256) break;

        // 1. 找完整命令段的边界：第 batch_size 个 \n，且不超过 rbuffer 容量
        int batch_size = 100;               // 每批最多100条，100条响应 ≈ 400B < wbuffer
        char *last_newline = NULL;
        int used_cmds = 0;
        int i = 0;
        for (i = 0; i < c->querybuf_len && used_cmds < batch_size; i++) {
            if (c->querybuf[i] == '\n') {
                last_newline = c->querybuf + i;
                used_cmds++;
            }
        }

        // 没有完整命令（残尾），留在querybuf等下次数据拼接
        if (!last_newline) break;

        // 2. 批长度不能超过 rbuffer 容量（留1字节给'\0'）
        int batch_cmd_length = last_newline - c->querybuf + 1;
        if (batch_cmd_length > BUFFER_LENGTH - 1) {
            // 回退到容量范围内最后一个 \n
            batch_cmd_length = BUFFER_LENGTH - 1;
            while (batch_cmd_length > 0 && c->querybuf[batch_cmd_length - 1] != '\n') {
                batch_cmd_length--;
            }
            if (batch_cmd_length == 0) break;  // 单条命令超过rbuffer，协议限制
        }

        // 3. 拷贝到 rbuffer（精确到 \n，残尾一个字节不多拷）
        memcpy(c->rbuffer, c->querybuf, batch_cmd_length);
        c->rbuffer[batch_cmd_length] = '\0';
        c->rlength = batch_cmd_length;

        // 4. 执行，追加响应到 wbuffer（满了返回-1则跳出，先发送）
        if (kvs_request(c) < 0) break;

    // 移除已经处理的数据
    // if (batch_cmd_length < c->querybuf_len) {
    //     int remain_len = c->querybuf_len - batch_cmd_length;
    //     // 从请求缓冲区batch_cmd_length位置开始，到remain_len个留下的数据，向前移动到缓冲区头部
    //     memmove(c->querybuf, c->querybuf + batch_cmd_length, remain_len);
    //     c->querybuf_len = remain_len;

    // } else {
    //     c->querybuf_len = 0;
    // }
        if (batch_cmd_length > 0) {
        querybuf_consume(c, batch_cmd_length);
        }
    }
#endif

#if 0
    // 查找最后一个换行符
    char *last_newline = memrchr(c->querybuf, '\n', c->querybuf_len);
    if (!last_newline) {
        return;  // 没有任何完整命令
    }
    
    // 计算完整数据的长度（从开头到最后一个换行符）
    int complete_len = last_newline - c->querybuf + 1;
    
    // 复制完整数据到 rbuffer
    memcpy(c->rbuffer, c->querybuf, complete_len);
    c->rbuffer[complete_len] = '\0';
    c->rlength = complete_len;
    
    // 处理完整命令
    kvs_request(c);
    
    // 移除已处理的数据，保留不完整的尾部
    if (complete_len < c->querybuf_len) {
        int remaining = c->querybuf_len - complete_len;
        memmove(c->querybuf, c->querybuf + complete_len, remaining);
        c->querybuf_len = remaining;
    } else {
        c->querybuf_len = 0;
    }
#endif
}
#endif

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
#if ENABLE_NOBLOCK
    // 设置非阻塞
    fcntl(clientfd, F_SETFL, fcntl(clientfd, F_GETFL) | O_NONBLOCK);
#endif
    event_register(clientfd, EPOLLIN);

    if ((clientfd % 1000) == 0) {

        struct timeval current;
        gettimeofday(&current, NULL);

        int time_used = TIME_SUB_MS(current, begin);
        memcpy(&begin, &current, sizeof(struct timeval));

        // printf("accept finished clientfd: %d, time_used: %d\n", clientfd, time_used);

    }
    return 0;
}


int recv_cb(int fd) {
    struct conn *c = &conn_list[fd];
    memset(conn_list[fd].rbuffer, 0, BUFFER_LENGTH );
#if ENABLE_NOBLOCK // 非阻塞
    int total_read = 0;
    int data_len;

    while (1) { // 循环拼接recv中的数据，querybuf动态拓展内存
        memset(c->rbuffer, 0, BUFFER_LENGTH);
        data_len = recv(fd, c->rbuffer, BUFFER_LENGTH, MSG_DONTWAIT);
        // 接收空闲||异常
        if (data_len < 0) {
            // 如果数据读完了，退出循环
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            // 客户端强制关闭连接
            if (errno == ECONNRESET) {
                printf("client closed connection (fd=%d)\n", fd);
                close(fd);
                epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                free_querybuf(c);
                return 0;
            }
            // 接收报错，释放接收缓冲区
            printf("recv error: %s\n", strerror(errno));
            close(fd);
            epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
            free_querybuf(c);
            return -1;
        }
        // 正常关闭，客户端调用close
        if (data_len == 0) {
            printf("client disconnect: %d\n", fd);
            close(fd);
            epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
            free_querybuf(c);
            return 0;
        }
        // 动态拓展请求缓冲区data_len+1大小（预留\0）
        if (expand_querybuf(c, c->querybuf_len + data_len + 1) < 0) {
            printf("ERROR EXpend querybuf\n");
            break;
        }
        // 把rbuffer中的新数据，cp到qbuf的末尾
        memcpy(c->querybuf + c->querybuf_len, c->rbuffer, data_len);
        c->querybuf_len += data_len;
        total_read += data_len;
    }
    // 如果请求buf中有数据，就处理
    if (c->querybuf_len > 0) {
        process_buffer(fd);
    }
    set_event(fd, EPOLLOUT, 0);
    return total_read;
    // data_len = recv(fd, conn_list[fd].rbuffer, BUFFER_LENGTH, 0);
    // if (data_len < 0) {
    //     if (errno == EAGAIN || errno == EWOULDBLOCK) {
    //         // 没有数据，等待epoll通知
    //         return 0;
    //     }
    //     printf("recv error: %s\n", strerror(errno));
    //     close(fd);
    //     epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
    //     return -1;
    // }

    // if (data_len == 0) {
    //     printf("client disconnect: %d\n", fd);
    //     close(fd);
    //     epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
    //     return 0;
    // }

#else // 阻塞写法
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
    printf("recv buffer: %s\n", conn_list[fd].rbuffer);

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
    printf("============waiting for send\n");
    printf("content: %s\n", conn_list[fd].wbuffer);
#endif


#endif
    set_event(fd, EPOLLOUT, 0);
    printf("HELLO IAM DEBUGING======1\n");
    return data_len;
#endif
}



int send_cb(int fd) {
    printf("HELLO IAM DEBUGING======2\n");
#if ENABLE_KVS
    kvs_response(&conn_list[fd]);
#endif
    int send_len = 0;
	if (conn_list[fd].wlength != 0) {
#if ENABLE_NOBLOCK
        send_len = send(fd, conn_list[fd].wbuffer, conn_list[fd].wlength, 0);
        if (send_len < 0) { // 发送失败
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 发送缓冲区还有数据，保持 EPOLLOUT，下次接着发
                set_event(fd, EPOLLOUT, 0);
                return 0;
            }
            perror("send");
            close(fd);
            epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
            return -1;
        }
        // 如果没发完，保持 EPOLLOUT
        if (send_len < conn_list[fd].wlength) {
            // 把剩下的数据移动到缓冲区头部
            memmove(conn_list[fd].wbuffer, 
                    conn_list[fd].wbuffer + send_len, 
                    conn_list[fd].wlength - send_len);
            conn_list[fd].wlength -= send_len;
            // 保持 EPOLLOUT，下次循环继续发剩下的
            set_event(fd, EPOLLOUT, 0);
            return send_len;
        }
        // 数据全部发送完毕，把长度设为 0
        conn_list[fd].wlength = 0;
#else

        printf("wbuffer : %s\n", conn_list[fd].wbuffer);
        printf("==============end===================\n");
		send_len = send(fd, conn_list[fd].wbuffer, conn_list[fd].wlength, 0);
        conn_list[fd].wlength = 0;
        
        // // 发送完毕后，续消费 cmd_buffer 中残留的完整命令（背压后半段）
        // // 直接调用业务层 kvs_ctx_process，无需回调指针
        // kvs_conn_ctx_t *ctx = &kvs_conn_ctxs[fd];
        // int n;
        // while (ENABLE_CMD_BATCH && ctx->cmd_buffer_len > 0) {
        //     n = kvs_ctx_process(fd, ctx, conn_list[fd].wbuffer, BUFFER_LENGTH);
        //     if (n <= 0) break;           // 无残留完整命令可续产
        //     send(fd, conn_list[fd].wbuffer, n, 0);   // 同步发出本轮续产响应
        //     conn_list[fd].wlength = 0;
        // }
	}
	set_event(fd, EPOLLIN, 0);

#endif
    return send_len;
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









