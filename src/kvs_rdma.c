#include "kvstore.h"
#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>
#include <arpa/inet.h>

#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>

// 主机只需要port参数即可
#define RDMA_SERVER_PORT    (g_config.master_port + 1)

// 从机需要 主机ip, 主机port，从机IP，从机port
#define RDMA_CLIENT_PORT    (g_config.slave_port + 1)
#define RDMA_CLIENT_IP      (g_config.slave_ip)
#define RDMA_SERVER_IP     (g_config.master_ip)

#define RDMA_TIMEOUT_MS     2000
#define RDMA_CQ_DEPTH       32
#define RDMA_BUFFER_SIZE    1024 //4096

#if ENABLE_RDMA



typedef struct conn_manager_s {

    struct ibv_mr *recv_mr;
    struct ibv_mr *send_mr;

    char *rbuffer;
    char *sbuffer;
} conn_manager_t;

// RDMA API

// 等待事件
static int rdma_wait_cm_event(struct rdma_event_channel *channel, enum rdma_cm_event_type expect, struct rdma_cm_event **out) {

    int ret = 0;
    struct rdma_cm_event *event;
    if (0 != rdma_get_cm_event(channel, &event)) {
        perror("rdma_get_cm_event\n");
        return -1;
    }

    if (event->event != expect) {   // 失败
        fprintf(stderr, "unexpect RDMA CM event %s, expected = %s\n", rdma_event_str(event->event), rdma_event_str(expect));
        ret = -1;
        goto finished;
    }

    if (out) {
        *out = event;
        return 0;
    }

 finished:
    rdma_ack_cm_event(event);
    return ret;
}

// 创建queuepair
static int rdma_create_queuepair(struct rdma_cm_id *id, struct ibv_pd **pd, struct ibv_cq **cq) {
    *pd = ibv_alloc_pd(id->verbs);
    if (NULL == *pd) {
        perror("ibv_alloc_pd\n");
        return -1;
    }
    // 创建一个complete queue
    *cq = ibv_create_cq(id->verbs, RDMA_CQ_DEPTH, NULL, NULL, 0);
    if (NULL == *cq) {
        perror("ibv_create_cq\n");
        return -1;
    }

    struct ibv_qp_init_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.send_cq = *cq;
    attr.recv_cq = *cq;
    attr.qp_type = IBV_QPT_RC;
    attr.cap.max_send_wr = RDMA_CQ_DEPTH;
    attr.cap.max_recv_wr = RDMA_CQ_DEPTH;
    attr.cap.max_send_sge = 1;
    attr.cap.max_recv_sge = 1;

    int ret = rdma_create_qp(id, *pd, &attr);

    if (ret != 0) {
        fprintf(stderr, "rdma_create_qp failed: %d\n", ret);
        return -1;
    }

    return 0;

}

static int rdma_destroy_queuepair(struct rdma_cm_id *id, struct ibv_pd *pd, struct ibv_cq *cq) {

    rdma_destroy_qp(id);

    ibv_destroy_cq(cq);

    ibv_dealloc_pd(pd);

    return 0;
}

// 给id的sbuffer、rbuffer和send_mr、recv_mr分配内存
static int rdma_alloc_buffer(struct rdma_cm_id *cm_id, struct ibv_pd *pd) {

    conn_manager_t *cm = (conn_manager_t *)calloc(1, sizeof(conn_manager_t));
    if (!cm) return -1;

    cm->rbuffer = (char *)calloc(1, RDMA_BUFFER_SIZE);
    cm->sbuffer = (char *)calloc(1, RDMA_BUFFER_SIZE);
    if (!cm->rbuffer || !cm->sbuffer) return -1;

    cm->send_mr = ibv_reg_mr(pd, cm->sbuffer, RDMA_BUFFER_SIZE, 0);
    cm->recv_mr = ibv_reg_mr(pd, cm->rbuffer, RDMA_BUFFER_SIZE, IBV_ACCESS_LOCAL_WRITE);
    if (!cm->send_mr || !cm->recv_mr) return -1;

    cm_id->context = cm;
    return 0;

}

static int rdma_free_buffer(struct rdma_cm_id *cm_id) {

    conn_manager_t *cm = cm_id->context;

    ibv_dereg_mr(cm->send_mr);
    ibv_dereg_mr(cm->recv_mr);

    free(cm->rbuffer);
    free(cm->sbuffer);

    return 0;
}

// 启动接收数据（可以提前开启，启动接收数据不代表接收到了数据）
static int rdma_post_recv(struct rdma_cm_id *cm_id) {
    conn_manager_t *cm = (conn_manager_t *) cm_id->context;

    struct ibv_sge sge;
    memset(&sge, 0, sizeof(struct ibv_sge));
    sge.addr = (uintptr_t)cm->rbuffer;
    sge.length = RDMA_BUFFER_SIZE;
    sge.lkey = cm->recv_mr->lkey;

    struct ibv_recv_wr wr, *bad = NULL;
    memset(&wr, 0, sizeof(struct ibv_recv_wr));
    wr.wr_id = (uintptr_t)cm;
    wr.sg_list = &sge;
    wr.num_sge = 1;

    return ibv_post_recv(cm_id->qp, &wr, &bad);
}

static int rdma_post_send(struct rdma_cm_id *cm_id, size_t length) {
    conn_manager_t *cm = cm_id->context;

    if (length == 0 || length > RDMA_BUFFER_SIZE) {
        return -1;
    }

    struct ibv_sge sge = {0};
    sge.addr = (uintptr_t)cm->sbuffer;
    sge.length = (uint32_t)length;
    sge.lkey = cm->send_mr->lkey;

    struct ibv_send_wr wr = {0};
    struct ibv_send_wr *bad = NULL;

    wr.wr_id = (uintptr_t)cm;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;

    return ibv_post_send(cm_id->qp, &wr, &bad);
}

// push 一次等待结果返回
static int rdma_poll_wc(struct ibv_cq *cq, struct ibv_wc *out) {

    while (1) {
        struct ibv_wc wc;
        int n = ibv_poll_cq(cq, 1, &wc);
        if (n < 0) return -1;
        else if (n == 0) {
            usleep(0);
            continue;
        }

        if (wc.status != IBV_WC_SUCCESS) {
            perror("ibv_poll_cq\n");
            return -1;
        }
        
        if(out) *out = wc;
        return 0;
    }
}





// 主节点建立RDMA连接
int master_rdma_start() {

    struct rdma_event_channel *channel = rdma_create_event_channel();
    if (!channel) {
        perror("rdma_create_event_channel\n");
        return 0;
    }
    struct rdma_cm_id *cm_id;
    if (0 != rdma_create_id(channel, &cm_id, NULL, RDMA_PS_TCP)) {
        perror("rdma_create_id\n");
        return 0;
    }
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(RDMA_SERVER_PORT);     // @@@服务端的port通过config读取
    if (0 != rdma_bind_addr(cm_id, (struct sockaddr*)&server_addr)) {
        perror("rdma_bind_addr\n");
        return 0;
    }
    if (0 != rdma_listen(cm_id, 10)) {
        perror("rdma_listen\n");
        return 0;
    }
    // 此时连接已经建立
    struct rdma_cm_event *event = NULL;
    rdma_wait_cm_event(channel, RDMA_CM_EVENT_CONNECT_REQUEST, &event);
    struct rdma_cm_id *id = event->id;
    rdma_ack_cm_event(event);

    struct ibv_pd *pd;
    struct ibv_cq *cq;
    if (0 != rdma_create_queuepair(id, &pd, &cq)) {
        perror("rdma_create_queuepair\n");
        return 0;
    }

    rdma_alloc_buffer(id, pd);
    rdma_post_recv(id); // 提前启动接收数据

    // 初始化连接参数
    struct rdma_conn_param param;
    memset(&param, 0, sizeof(param));
    param.responder_resources = 1;
    param.initiator_depth = 1;
    param.retry_count = 3;

    // accept双向连接建立完成
    if (0 != rdma_accept(id, &param)) {
        perror("rdma_accept\n");
        return 0;
    }
    // 连接建立完了，拿到了ID，接着可以进行数据传输
    rdma_wait_cm_event(id->channel, RDMA_CM_EVENT_ESTABLISHED, NULL);

    struct ibv_wc wc;
    if (0 != rdma_poll_wc(cq, &wc) || wc.opcode != IBV_WC_RECV) {
        perror("rdma_poll_wc\n");
        return 0;
    }
    conn_manager_t *cm = id->context;

    /* 测试代码，客户端发来小写字母，服务端回大写字母 */
    printf("recv: %s\n", cm->rbuffer);
    int i=0;
    for (i=0; i<RDMA_BUFFER_SIZE; i++) {
        if (cm->rbuffer[i] == 0x0) break;
        cm->sbuffer[i] = cm->rbuffer[i] - 0x20;
    }
    rdma_post_send(id, strlen(cm->sbuffer));
    printf("[RDMA] master echo done\n");

    rdma_free_buffer(cm_id); 
    rdma_destroy_queuepair(cm_id, pd, cq);

    return 0;
}

// 从节点创建id的过程
static struct rdma_cm_id *rdma_create_cm_id(struct rdma_event_channel *channel, const char *ip, unsigned short port) {

    struct rdma_cm_id *cm_id; 
    if (0 != rdma_create_id(channel, &cm_id, NULL, RDMA_PS_TCP)) {
        perror("rdma_create_id\n");
        return 0;
    }

    struct sockaddr_in dst_addr;
    memset(&dst_addr, 0, sizeof(dst_addr));
    dst_addr.sin_family = AF_INET;
    dst_addr.sin_port = htons(port);
    if (1 != inet_pton(AF_INET, ip, &dst_addr.sin_addr)) {
        perror("inet_pton\n");
        return 0;
    }

    struct sockaddr_in src_addr;
    memset(&src_addr, 0, sizeof(src_addr));
    src_addr.sin_family = AF_INET;
    src_addr.sin_port = htons(RDMA_CLIENT_PORT);
    src_addr.sin_addr.s_addr = inet_addr(RDMA_CLIENT_IP);

    // 将ip地址解析为rdma地址，如果成功，就把这个RDMA地址绑定到对应的设备上面
    if (0 != rdma_resolve_addr(cm_id, (struct sockaddr*)&src_addr, (struct sockaddr*)&dst_addr, RDMA_TIMEOUT_MS)) {// 解析1s
        perror("rdma_resolve_addr\n");
        return 0;
    }

    rdma_wait_cm_event(channel, RDMA_CM_EVENT_ADDR_RESOLVED, NULL);

    // 解析出一个路由器
    if (0 != rdma_resolve_route(cm_id, RDMA_TIMEOUT_MS)) {
        perror("rdma_resolve_route\n");
        return 0;
    }

    rdma_wait_cm_event(channel, RDMA_CM_EVENT_ROUTE_RESOLVED, NULL);

    return cm_id;
}

// 从节点建立连接
int slave_rdma_start() {

    // 创建channel和id
    struct rdma_event_channel *channel = rdma_create_event_channel();
    if (!channel) {
        perror("rdma_create_event_channel\n");
        return 0;
    }

    struct rdma_cm_id *cm_id = rdma_create_cm_id(channel, RDMA_SERVER_IP, RDMA_SERVER_PORT);

    // 构建队列
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    rdma_create_queuepair(cm_id, &pd, &cq);

    rdma_alloc_buffer(cm_id, pd);

    // 分配buffer
    struct rdma_conn_param param;
    memset(&param, 0, sizeof(param));
    param.responder_resources = 1;
    param.initiator_depth = 1;
    param.retry_count = 3;

    if (0 != rdma_connect(cm_id, &param)) {
        perror("rdma_connect\n");
        return -1;
    }
    if (rdma_wait_cm_event(channel, RDMA_CM_EVENT_ESTABLISHED, NULL) != 0) {
        printf("Connect failed!\n");
        return -1;
    }

    printf("rdma connect success\n");

    struct ibv_wc wc;
    conn_manager_t *cm = cm_id->context;
    

    // 提前提交接收请求，等待服务端最终确认
    if (rdma_post_recv(cm_id) != 0) {
        fprintf(stderr, "rdma_post_recv failed\n");
        return -1;
    }
    printf("Client RECV posted, waiting for server ACK\n");


    sprintf(cm->sbuffer, "abcdefghijklmnopqrstuvwxyz");
    rdma_post_send(cm_id, strlen(cm->sbuffer));
    if (0 != rdma_poll_wc(cq, &wc) || wc.opcode != IBV_WC_SEND) {
        perror("rdma_poll_wc\n");
        return 0;
    }

    /* 等主机的 echo 回来 */

    if (rdma_poll_wc(cq, &wc) != 0 || wc.opcode != IBV_WC_RECV) {
        perror("rdma_poll_wc\n");
        return 0;
    }
    printf("[RDMA] slave got echo: %s\n", cm->rbuffer);


    rdma_free_buffer(cm_id); 
    rdma_destroy_queuepair(cm_id, pd, cq);

    return 0;
}


// 主机端的rdma线程
void *master_rdma_thread(void *arg) {
    (void)arg;
    if (master_rdma_start() != 0) {
        LOG_INFO("[RDMA] master handshake failed\n");
    }
    return NULL;
}


#endif