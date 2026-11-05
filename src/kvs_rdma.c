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
#define RDMA_BUFFER_SIZE    1024 * 1024 * 6 //4096

#if ENABLE_RDMA



typedef struct conn_manager_s {

    struct ibv_mr *recv_mr;
    struct ibv_mr *send_mr;

    char *rbuffer;
    char *sbuffer;
} conn_manager_t;

/* 给RDMA添加READY 和 ACK的同步机制，确保数据传输顺序 */
typedef enum {
    RDMA_MSG_DATA = 1,    // 正文块
    RDMA_MSG_MARKER = 2,      // 结束标记（payload 里放 marker 字符串）
    RDMA_MSG_READY = 3,
    RDMA_MSG_FINISHED = 4,
    RDMA_MSG_ERROR = 5
} rdma_msg_type_t;

typedef struct {
    uint32_t type;
    uint32_t length;      // 本块 payload 有效字节数
} rdma_msg_t;             // 8字节头，后面跟 payload

static struct rdma_cm_id *g_conn_id = NULL;
static struct ibv_pd     *g_conn_pd = NULL;
static struct ibv_cq     *g_conn_cq = NULL;
static conn_manager_t    *g_conn_cm = NULL;
static struct rdma_event_channel *g_conn_channel = NULL;
static struct rdma_cm_id         *g_listen_id    = NULL;
static pthread_mutex_t    g_rdma_mtx = PTHREAD_MUTEX_INITIALIZER;



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
    if (!cm) return -1;

    ibv_dereg_mr(cm->send_mr);
    ibv_dereg_mr(cm->recv_mr);

    free(cm->rbuffer);
    free(cm->sbuffer);
    free(cm);
    cm_id->context = NULL;

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
            LOG_ERROR("[RDMA] poll_cq failed: status=%s(%d), vendor_err=0x%x, opcode=%d, byte_len=%u",
                ibv_wc_status_str(wc.status), wc.status,
                wc.vendor_err, wc.opcode, wc.byte_len);
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
    LOG_INFO("[RDMA] master listening on %s:%d", g_config.master_ip, RDMA_SERVER_PORT);
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

    if (rdma_alloc_buffer(id, pd) != 0) {
        LOG_ERROR("[RDMA] alloc buffer failed");
        return -1;
    }

    /* 提前准备接收从机的第一条 READY */
    if (rdma_post_recv(id) != 0) {
        LOG_ERROR("[RDMA] master initial RECV failed");
        return -1;
    }


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


 #if 1
    g_conn_id = id;
    g_conn_pd = pd;
    g_conn_cq = cq;
    g_conn_cm = (conn_manager_t *)id->context;
    g_conn_channel = channel;      // 事件通道，退出时要销毁
    g_listen_id    = cm_id;        // 监听 id，退出时要销毁
    LOG_INFO("[RDMA] master connection established");
    return 0;
 #else
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
    LOG_INFO("[RDMA] master echo done");

    rdma_free_buffer(id); 
    rdma_destroy_queuepair(id, pd, cq);
    rdma_destroy_id(cm_id);
    rdma_destroy_event_channel(channel);

    return 0;
 #endif
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

    LOG_INFO("rdma connect success\n");

    // struct ibv_wc wc;
    // conn_manager_t *cm = cm_id->context;

 #if 1
    g_conn_id = cm_id;
    g_conn_pd = pd;
    g_conn_cq = cq;
    g_conn_cm = (conn_manager_t *)cm_id->context;
    g_conn_channel = channel;      // 从机只有这一个 id，没有 g_listen_id
    LOG_INFO("[RDMA] slave connection established");

    return 0;
 #else
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
    LOG_INFO("[RDMA] slave got echo: %s", cm->rbuffer);


    rdma_free_buffer(cm_id); 
    rdma_destroy_queuepair(cm_id, pd, cq);

    return 0;
 #endif
}





// 发一个块，阻塞到发送完成
static int rdma_send_block(struct rdma_cm_id *id, struct ibv_cq *cq, uint32_t type, const char *payload, uint32_t len) {
    conn_manager_t *cm = (conn_manager_t *)id->context;
    if (len > RDMA_BUFFER_SIZE - sizeof(rdma_msg_t)) return -1;

    rdma_msg_t *msg = (rdma_msg_t *)cm->sbuffer;
    msg->type = type;
    msg->length = len;
    char *dst = cm->sbuffer + sizeof(rdma_msg_t);
    if (len > 0 && payload != dst) {
        memcpy(dst, payload, len);
    }
    LOG_INFO("[RDMA] posting SEND type=%u len=%u", type, len);
    int ret = rdma_post_send(id, sizeof(rdma_msg_t) + len);

    if (ret != 0) {
        LOG_ERROR("[RDMA] ibv_post_send failed: %d (%s)",
            ret, strerror(ret));
        return -1;
    }
    struct ibv_wc wc;

    if (rdma_poll_wc(cq, &wc) != 0)
        return -1;

    LOG_INFO("[RDMA] SEND completion opcode=%d status=%d",
        wc.opcode, wc.status);

    if (wc.opcode != IBV_WC_SEND)
        return -1;

    LOG_INFO("[RDMA] SEND type=%u, length=%u", type, len);
    return 0;
}

// 接收一个块
static int rdma_recv_block(struct rdma_cm_id *id, struct ibv_cq *cq, struct ibv_wc *out) {
    struct ibv_wc wc;
    if (rdma_poll_wc(cq, &wc) != 0 || wc.opcode != IBV_WC_RECV) return -1;

    if (wc.byte_len < sizeof(rdma_msg_t) || wc.byte_len > RDMA_BUFFER_SIZE) {
        perror("[RDMA] invalid recv size\n");
        return -1;
    }
    *out = wc;
    return 0;
}

// 收发控制的辅助函数
static int rdma_send_control(uint32_t type) {

    return rdma_send_block( g_conn_id, g_conn_cq, type, NULL, 0);

}

static int rdma_wait_control(uint32_t expected) {

    struct ibv_wc wc;

    if (rdma_recv_block( g_conn_id, g_conn_cq, &wc) != 0)
        return -1;

    rdma_msg_t *msg = (rdma_msg_t *)g_conn_cm->rbuffer;

    if (wc.byte_len != sizeof(rdma_msg_t) || msg->length != 0 || msg->type != expected) {

        LOG_ERROR("[RDMA] control mismatch: expected=%u, actual=%u, bytes=%u", expected, msg->type, wc.byte_len);
        return -1;
    }

    return 0;
}


int rdma_send_file(const char *filepath, long total, const char *end_marker) {
    if (!filepath || !end_marker) return -1;

    /* 等连接就绪（主机监听线程是异步起的，最多等 5s） */
    int wait = 0;

    while (g_conn_id == NULL && wait++ < 50) usleep(100 * 1000);

    if (g_conn_id == NULL) {
        LOG_ERROR("[RDMA] connection not ready");
        return -1;
    }

    pthread_mutex_lock(&g_rdma_mtx);

    int rc = -1;
    FILE *fp = NULL;

    struct stat st;
    if (stat(filepath, &st) != 0) {
        perror("stat");
        goto out;
    }

    long to_send = st.st_size;

    if (total > 0 && total < to_send) to_send = total;

    fp = fopen(filepath, "rb");
    if (!fp) {
        perror("fopen");
        goto out;
    }


    /* 直接读进 sbuffer 的 payload 区，避免二次拷贝 */
    char *payload = g_conn_cm->sbuffer + sizeof(rdma_msg_t);

    size_t cap = RDMA_BUFFER_SIZE - sizeof(rdma_msg_t);

    long sent = 0;
    

    while (sent < to_send) {

        /* 等待从机准备好接收 DATA */
        if (rdma_wait_control(RDMA_MSG_READY) != 0)
            goto out;

        /*
         * 在发送当前 DATA 前，先准备接收
         * 下一轮 READY。
         *
         * 避免从机发 READY 时主机还没 post_recv。
         */
        if (rdma_post_recv(g_conn_id) != 0)
            goto out;
        
        size_t remaining = (size_t)(to_send - sent);
        size_t chunk = remaining < cap ? remaining : cap;

        if (fread(payload, 1, chunk, fp) != chunk) {
            LOG_ERROR("[RDMA] fread failed");
            goto out;
        }

        if (rdma_send_block(g_conn_id, g_conn_cq, RDMA_MSG_DATA, payload, (uint32_t)chunk) != 0) {
            goto out;
        }
        sent += (long)chunk;

        LOG_INFO("[RDMA] sent chunk=%zu total=%ld/%ld", chunk, sent, to_send);
    }

    /*
     * 从机处理完最后一块 DATA 后，
     * 会再次发送 READY，表示可以接收 MARKER。
     */
    if (rdma_wait_control(RDMA_MSG_READY) != 0)
        goto out;

    /*
     * 提前准备接收最终 FINISHED。
     */
    if (rdma_post_recv(g_conn_id) != 0)
        goto out;


    LOG_INFO("FINISHED");
    if (rdma_send_block(g_conn_id, g_conn_cq, RDMA_MSG_MARKER, end_marker, (uint32_t)strlen(end_marker)) != 0) {
        goto out;
    }

    /* 等待从机确认整个文件写入成功 */
    if (rdma_wait_control(RDMA_MSG_FINISHED) != 0)
        goto out;

    /* 为下一轮 SNAP 同步提前准备 RECV */
    if (rdma_post_recv(g_conn_id) != 0)
        goto out;

    LOG_INFO("[RDMA] file sync finished: %ld bytes", sent);

    rc = 0;

out:
    if (fp)
        fclose(fp);

    pthread_mutex_unlock(&g_rdma_mtx);
    return rc;
}

/* 从机调用：接收文件写入 filepath，直到收到 end_marker。语义与 recv_file_content 一致 */
int rdma_recv_file(const char *filepath, const char *end_marker) {
    if (!filepath || !end_marker) return -1;
    if (g_conn_id == NULL) return -1;

    pthread_mutex_lock(&g_rdma_mtx);

    int rc = -1;
    FILE *fp = NULL;
    long total = 0;
    int done = 0;

    fp = fopen(filepath, "rb+");

    if (fp == NULL) {
        fp = fopen(filepath, "wb");
    }
    if (!fp) {
        perror("fopen");
        goto out;
    }

    size_t end_len = strlen(end_marker);

    while (!done) {
        // 第一步：先提交请求
        if (rdma_post_recv(g_conn_id) != 0) {
            LOG_ERROR("[RDMA] post_recv failed");
            goto out;
        }

        // 第二步：告诉主机可以发下一条消息
        if (rdma_send_control(RDMA_MSG_READY) != 0) {
            LOG_ERROR("[RDMA] send READY failed");
            goto out;
        }

        // 第三步：等待主机发送DATA | MARKER
        struct ibv_wc wc;
        if (rdma_recv_block(g_conn_id, g_conn_cq, &wc) != 0) goto out;

        rdma_msg_t *msg =(rdma_msg_t *)g_conn_cm->rbuffer;
        char *payload = g_conn_cm->rbuffer + sizeof(rdma_msg_t);
        uint32_t actual_len = wc.byte_len - sizeof(rdma_msg_t);
        LOG_INFO("[RDMA] recv type=%u, wc.byte_len=%u, payload=%u", msg->type, wc.byte_len, msg->length);
        if (msg->length != actual_len) {
            LOG_ERROR("[RDMA] length mismatch: header=%u actual=%u", msg->length, actual_len);
            goto out;
        }

        if (msg->type == RDMA_MSG_DATA) {
            size_t written = fwrite(payload, 1, msg->length, fp);

            if (written != msg->length) {
                LOG_ERROR("[RDMA] fwrite failed");
                goto out;
            }

            total += (long)written;

            LOG_INFO("[RDMA] written=%zu total=%ld", written, total);

        } else if (msg->type == RDMA_MSG_MARKER) {
            if (msg->length != end_len || memcmp(payload, end_marker, end_len) != 0) {
                LOG_ERROR("[RDMA] marker mismatch");
                goto out;
            }

            done = 1;

        } else {
            LOG_ERROR("[RDMA] unknown msg type=%u", msg->type);
            goto out;
        }
        /*
         * 当前消息已经完全处理。
         * 下一轮循环才重新 post_recv，
         * 不会提前覆盖 rbuffer。
         */

    }

    if (fflush(fp) != 0) {
        perror("fflush");
        goto out;
    }

    if (ftruncate(fileno(fp), total) != 0)
        goto out;
        
    if (fsync(fileno(fp)) != 0) {
        perror("fsync");
        goto out;
    }

    if (fclose(fp) != 0) {
        fp = NULL;
        perror("fclose");
        goto out;
    }
    fp = NULL;
    /*
     * 文件写入成功后发送最终确认。
     */

    if (rdma_send_control(RDMA_MSG_FINISHED) != 0) goto out;
    LOG_INFO("[RDMA] received %ld bytes -> %s", total, filepath);
    rc = 0;

out:
    if (fp)
        fclose(fp);

    pthread_mutex_unlock(&g_rdma_mtx);
    return rc;
}

// 主机端的rdma线程
void *master_rdma_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_rdma_mtx);
    if (g_conn_id != NULL) {                    // 已建立过连接，直接返回
        pthread_mutex_unlock(&g_rdma_mtx);
        return NULL;
    }
    pthread_mutex_unlock(&g_rdma_mtx);
    if (0 != master_rdma_start()) {
        LOG_INFO("[RDMA] master handshake failed\n");
    }
    return NULL;
}


/* 程序退出时调用：拆掉常驻连接，释放所有 RDMA 资源 */
void rdma_shutdown(void) {
    pthread_mutex_lock(&g_rdma_mtx);
    if (g_conn_id == NULL) {
        pthread_mutex_unlock(&g_rdma_mtx);
        return;
    }

    /* 1. 先断开：通知对端，同时会在本地触发一个 DISCONNECTED 事件 */
    rdma_disconnect(g_conn_id);

    /* 2. 把这个 DISCONNECTED 事件收掉，否则 rdma_destroy_id 可能一直等事件被 reap */
    if (g_conn_channel) {
        struct rdma_cm_event *ev = NULL;
        if (0 == rdma_get_cm_event(g_conn_channel, &ev)) {
            rdma_ack_cm_event(ev);
        }
    }

    /* 3. 销毁顺序不能乱：dereg MR → destroy QP → destroy CQ → dealloc PD */
    rdma_free_buffer(g_conn_id);                                  /* 里面 dereg send_mr/recv_mr */
    rdma_destroy_queuepair(g_conn_id, g_conn_pd, g_conn_cq);      /* destroy_qp + destroy_cq + dealloc_pd */
    rdma_destroy_id(g_conn_id);

    g_conn_id = NULL;
    g_conn_pd = NULL;
    g_conn_cq = NULL;
    g_conn_cm = NULL;

    /* 4. 最后销毁监听 id 和事件通道 */
    if (g_listen_id) {
        rdma_destroy_id(g_listen_id);
        g_listen_id = NULL;
    }
    if (g_conn_channel) {
        rdma_destroy_event_channel(g_conn_channel);
        g_conn_channel = NULL;
    }

    pthread_mutex_unlock(&g_rdma_mtx);
    LOG_INFO("[RDMA] shutdown done");
}


#endif