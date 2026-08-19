
#ifndef _SERVER_H_
#define _SERVER_H_


typedef int (*RCALLBACK)(int fd);
#define BUFFER_LENGTH       1024

#define ENABLE_HTTP     0
#define ENABLE_WS       0
#define ENABLE_KVS      1



// 封装IO，读数据写数据内存，以及相应的回调函数
struct conn {
    int fd;

    char rbuffer[BUFFER_LENGTH];
    int rlength;

    char wbuffer[BUFFER_LENGTH];
    int wlength;

    char *payload;  // 解码后的数据
    char mask[4];   // 掩码

    RCALLBACK send_callback; 
    union {// recv和accept是一个或的关系
        RCALLBACK recv_callback;
        RCALLBACK accept_calllback;
    } r_action;

    int status;
    
};


int http_requset(struct conn *c);
int http_response(struct conn *c);
// WebSocket 相关函数声明（在 main 函数之前）
int ws_request(struct conn *c);
int ws_response(struct conn *c);

int kvs_request(struct conn *c);
int kvs_response(struct conn *c);
#endif

