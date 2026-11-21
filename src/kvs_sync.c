#include "kvstore.h"
#include "server.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>  
#include <fcntl.h>     
#include <errno.h>

/* ===== 当定义 SYNC_RAW_IO 时，绕过 NtyCo hook，直接走系统调用 ===== */
#ifdef SYNC_RAW_IO
#include <sys/syscall.h>

#define socket(d, t, p)         syscall(SYS_socket, d, t, p)
#define connect(f, a, l)        syscall(SYS_connect, f, a, l)
#define send(f, b, l, g)        syscall(SYS_sendto, f, b, l, g, NULL, 0)
#define recv(f, b, l, g)        syscall(SYS_recvfrom, f, b, l, g, NULL, NULL)
#define select(n, r, w, e, t)   syscall(SYS_select, n, r, w, e, t)
#define close(f)                syscall(SYS_close, f)
#endif


#if ENABLE_AOF_SYNC
#define AOF_FILE_COUNT 1

// AOF 增量同步上下文（长连接）
typedef struct {
    int fd;                    // 同步连接fd
    int is_active;             // 是否活跃
    time_t last_heartbeat;     // 上次心跳时间
    pthread_t recv_thread;     // 从节点接收线程
    long consumed_offset;      // ← 新增：从机已 replay 到的 AOF 偏移
} aof_sync_ctx_t;

static aof_sync_ctx_t g_aof_ctx = {0};


// 初始化 AOF 上下文
void init_aof_sync_ctx(void) {
    memset(&g_aof_ctx, 0, sizeof(g_aof_ctx));
    g_aof_ctx.fd = -1;
    g_aof_ctx.is_active = 0;
    g_aof_ctx.last_heartbeat = time(NULL);
}
#endif

static int g_slave_incr_fd = -1;   /* 主机侧：从机复用的增量连接 fd（SYNC 命令所在连接）
                                    * ⚠ 与 kvs_ebpf.c 的 g_slave_fd 指向同一条连接：
                                    *   两处各自独立登记（都在收到 SYNC 命令时），
                                    *   改动任意一处的登记逻辑需同步评估另一处 */

int safe_send(int fd, const char* data, int len) {
    int total = 0;
    while (total < len) {
        int n = send(fd, data + total, len - total, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(10);
                continue;      // 流控，正常，不打印
            }
            SYNC_ERR("safe_send fd=%d errno=%d (%s)", fd, errno, strerror(errno));
            return -1;
        }
        if (n == 0) {
            SYNC_ERR("safe_send fd=%d peer closed", fd);
            return -1;
        }
        total += n;
    }
    return total;
}

#if ENABLE_FWD_QUEUE
/**
 * 网络转发异步队列（仅 ebpf.enable=false 的直发路径使用）
 * 模型：单生产者（主业务线程），单消费者（发送线程）
 *   - 生产：malloc 节点 + memcpy 字节串 + 挂尾，临界区仅此，微秒级
 *   - 消费：取头节点 → safe_send → free
 *   - 保序：FIFO 保证主从命令顺序一致
 */
#define SEND_QUEUE_MAX (32u << 20)      // 32MB队列上限

typedef struct fwd_node {
    struct fwd_node *next;
    int     len;
    char    data[];
} fwd_node_t;


static fwd_node_t *fwd_head = NULL;
static fwd_node_t *fwd_tail = NULL;
static unsigned int fwd_bytes = 0;

static pthread_mutex_t fwd_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fwd_cond = PTHREAD_COND_INITIALIZER;
static pthread_t fwd_tid;
static int fwd_started = 0; //  1表示线程已经创建
static int fwd_run = 0; //  1表示线程已经运行


/**
 * 后台发送线程：FIFO取节点，safe_send发出，释放节点
 */
static void *fwd_send_thread(void *arg) {

    (void)arg;

    while(1) {
        pthread_mutex_lock(&fwd_mutex);
        // 如果线程在运行且队列头节点位空，那么线程等待
        while (fwd_run && fwd_head == NULL) {
            pthread_cond_wait(&fwd_cond, &fwd_mutex);
        }
        /* 退出条件：收到 stop 且队列已排空 */
        if (!fwd_run && fwd_head == NULL) {
            pthread_mutex_unlock(&fwd_mutex);
            break;
        }

        fwd_node_t *node = fwd_head;
        fwd_head = node->next;
        if (fwd_head == NULL) fwd_tail = NULL;
        fwd_bytes -= (unsigned int)node->len;
        pthread_mutex_unlock(&fwd_mutex);

        if (g_slave_incr_fd > 0) {

            safe_send(g_slave_incr_fd, node->data, node->len);
        }

        free(node);
    }
    return NULL;
}

// 启动线程：第一次有命令要转发时才创建线程
static void fwd_thread_start(void) {

    if (fwd_started) return;

    fwd_run = 1;
    if (pthread_create(&fwd_tid, NULL, fwd_send_thread, NULL) != 0) {
        LOG_ERROR("[SYNC] 创建转发线程失败，回退为命令线程内同步发送");
        fwd_run = 0;
        return;
    }
    fwd_started = 1;
}

/* ===== 主机：网络转发模式——命令线程把写命令拼成 RESP 直发从机 =====
 * 与 eBPF 转发自动二选一：ebpf.enable=false 时走本路径（增量同步总得有通道）。
 * 全量同步窗口期间从机接收线程尚未就绪，数据会先堆在 socket 缓冲，
 * 就绪后被自动消费——语义上等价于 eBPF 模式的"窗口缓存补发"。 */
void sync_net_forward_cmd(const char *cmd, const char *key, const char *value) {
    if (g_config.ebpf_enable || !g_is_master || g_slave_incr_fd <= 0) return;

    static char fwd_buf[512];   /* 命令线程单线程写，入队前已拷走，安全 */
    int n;
    if (value) {
        n = snprintf(fwd_buf, sizeof(fwd_buf),
                     "*3\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                     strlen(cmd), cmd, strlen(key), key, strlen(value), value);
    } else if (key) {
        n = snprintf(fwd_buf, sizeof(fwd_buf),
                     "*2\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                     strlen(cmd), cmd, strlen(key), key);
    } else {
        /* 无参命令（SAVE）：*1 单元素数组 */
        n = snprintf(fwd_buf, sizeof(fwd_buf),
                     "*1\r\n$%zu\r\n%s\r\n",
                     strlen(cmd), cmd);
    }
    if (n <= 0 || n >= (int)sizeof(fwd_buf)) return;


    fwd_thread_start();
    /* 线程没起来（创建失败）：退化为原来的同步发送，保证功能不丢 */
    if (!fwd_started) {
        safe_send(g_slave_incr_fd, fwd_buf, n);
        return;
    }

    // RESP数据入队
    pthread_mutex_lock(&fwd_mutex);

    /* 队列超限：从机长期不读，主从已脱节，丢本条（等重同步） */
    if (fwd_bytes + (unsigned int)n > SEND_QUEUE_MAX) {
        pthread_mutex_unlock(&fwd_mutex);
        LOG_WARN("[SYNC] 转发队列满（%u 字节），丢弃一条命令", SEND_QUEUE_MAX);
        return;
    }

    fwd_node_t *node = (fwd_node_t *)malloc(sizeof(fwd_node_t) + (size_t)n);
    if (node == NULL) {
        pthread_mutex_unlock(&fwd_mutex);
        return;
    }
    node->len  = n;
    node->next = NULL;
    memcpy(node->data, fwd_buf, (size_t)n);

    if (fwd_tail) 
        fwd_tail->next = node;
    else            
        fwd_head = node;

    fwd_tail = node;
    fwd_bytes += (unsigned int)n;
    pthread_cond_signal(&fwd_cond);

    pthread_mutex_unlock(&fwd_mutex);
}

/* 停止转发线程：置退出标志 → join（线程会把队列排空后退出）→ 清残留。
 * 幂等，可重复调用。 */
void sync_net_forward_stop(void)
{
    if (!fwd_started) return;

    pthread_mutex_lock(&fwd_mutex);
    fwd_run = 0;
    pthread_cond_signal(&fwd_cond);
    pthread_mutex_unlock(&fwd_mutex);

    pthread_join(fwd_tid, NULL);
    fwd_started = 0;
}

#else

/* ===== 主机：网络转发模式——命令线程把写命令拼成 RESP 直发从机 =====
 * 与 eBPF 转发自动二选一：ebpf.enable=false 时走本路径（增量同步总得有通道）。
 * 全量同步窗口期间从机接收线程尚未就绪，数据会先堆在 socket 缓冲，
 * 就绪后被自动消费——语义上等价于 eBPF 模式的"窗口缓存补发"。 */
void sync_net_forward_cmd(const char *cmd, const char *key, const char *value) {
    if (g_config.ebpf_enable || !g_is_master || g_slave_incr_fd <= 0) return;

    static char fwd_buf[512];   // 单条命令缓冲（本工程 key/value 均为短串）
    int n;
    if (value) {
        n = snprintf(fwd_buf, sizeof(fwd_buf),
                     "*3\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                     strlen(cmd), cmd, strlen(key), key, strlen(value), value);
    } else if (key) {
        n = snprintf(fwd_buf, sizeof(fwd_buf),
                     "*2\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                     strlen(cmd), cmd, strlen(key), key);
    } else {
        /* 无参命令（SAVE）：*1 单元素数组 */
        n = snprintf(fwd_buf, sizeof(fwd_buf),
                     "*1\r\n$%zu\r\n%s\r\n",
                     strlen(cmd), cmd);
    }
    if (n > 0 && n < (int)sizeof(fwd_buf)) {
        safe_send(g_slave_incr_fd, fwd_buf, n);
    }
}

#endif



// ===== 从节点接收aof--超时5s自动发心跳 =====
static int recv_with_timeout(int fd, void *buf, size_t len, int timeout_sec) {
// 返回：
//   > 0 ：收到数据
//   = 0 ：超时
//   < 0 ：错误
    fd_set rfds;
    struct timeval tv;

    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;

    int ret = select(fd + 1, &rfds, NULL, NULL, &tv);
    if (ret < 0) {
        // select 错误
        return -1;
    } else if (ret == 0) {
        // 超时
        return 0;
    }

    // 有数据
    return recv(fd, buf, len, 0);
}

// ===== 主节点： fd 是否为同步（复制）连接 =====
int is_sync_peer(int fd) {
    if (fd < 0) return 0;
    #if ENABLE_AOF_SYNC
    if (g_aof_ctx.is_active && g_aof_ctx.fd == fd) return 1;
    #endif
    if (g_is_master && g_slave_incr_fd == fd) return 1;   /* 主机侧：从机增量连接 */
    return 0;
}





// ===== 1. 全量阶段，主节点handle调用：内存数据全量放入tmp中 ==== 
int master_save_tmp(void)
{
    unlink(TEMP_FILE);                        /* 清掉上次同步可能残留的 .tmp */
    if (save_snapshots_to(TEMP_FILE) != 0) {
        LOG_ERROR("save full data to %s failed", TEMP_FILE);
        return -1;
    }
    LOG_INFO("[TEMP] full data saved to %s", TEMP_FILE);
    return 0;
}

// ===== 2. 全量阶段，主节点handle调用：tmp文件发送线程 ==== 
static void *master_send_temp_thread(void *arg) {

    (void)arg;

    /* 从机要先发起 RDMA client 连接，这里最多等 10s */
    if (rdma_wait_client(10000) != 0) {
        LOG_ERROR("rdma client not connected in 10s, full sync abort");
        return NULL;
    }

    if (rdma_send_file(TEMP_FILE, -1, "AOF_SYNC_OK\n") < 0) {
        LOG_ERROR("rdma send %s failed", TEMP_FILE);
        return NULL;
    }
    LOG_INFO("[RDMA] Full data sync: %s pushed by rdma success !", TEMP_FILE);
    return NULL;
}

// ===== 主机：收到从机的SYNC命令 =====
int handle_sync_request(int fd) {

    g_slave_incr_fd = fd;            /* 记住这条连接：从机之后用它收增量/发心跳 */

    /* 1: 把内存全量数据保存进temp文件 */
    if (0 != master_save_tmp()) {
        LOG_INFO("[TEMP] Save full data to tmp file failed");
        close(fd);
        return -1;
    }
    LOG_INFO("[TEMP] Save full data to tmp file success !");
    safe_send(fd, "ok\n", 3);            /* 主机tmp保存完成，发送ok给从机 */

    /* 2: 启动RDMA server，后台线程推送.tmp到从机 */
 #if ENABLE_RDMA
    if (g_config.rdma_enable) {

        /* 启动rdma server 线程*/
        pthread_t tid;
        if (pthread_create(&tid, NULL, master_rdma_thread, NULL) != 0) {
            LOG_INFO("create rdma thread failed\n");
            close(fd);
            return -1;
        }
        pthread_detach(tid);

        /* 推送线程：先等待从机rdma连接，然后发.tmp文件 */
        pthread_t send_tid;
        if (pthread_create(&send_tid, NULL, master_send_temp_thread, (void *)(long)fd) != 0) {
            SYNC_ERR("create full sync send thread failed");
            close(fd);
            return -1;
        }
        pthread_detach(send_tid);
        return 0;
    }
 #endif

    SYNC_ERR("rdma disabled, sync unsupported");
    close(fd);
    return -1;
}

// ===== 主机：收到从机 SYNC-FINISHED，全量同步结束 =====
int handle_sync_finished(int fd) {
    (void)fd;

    unlink(TEMP_FILE);
    LOG_INFO("[SYNC] SYNC-FINISHED received, %s removed", TEMP_FILE);

#if ENABLE_RDMA
    if (g_config.rdma_enable) {
        rdma_server_destroy();                 /* 一次性同步：server 用完即毁 */
    }
#endif
    return 0;
}


static const char *SYNC_CMD = "*1\r\n$4\r\nSYNC\r\n";
static const char *fin = "*1\r\n$13\r\nSYNC-FINISHED\r\n";   /* $后是命令字符串长度：13 */

// ===== 从机：接收主机 eBPF 转发的增量命令（RESP 流，直接执行，不落 AOF） =====
static void *slave_sync_recv_thread(void *arg) {
    aof_sync_ctx_t *ctx = (aof_sync_ctx_t *)arg;
    int fd = ctx->fd;

    char buf[4096];
    SYNC_LOG_S("incremental recv thread started on fd %d", fd);

    while (ctx->is_active && fd > 0) {
        int n = recv_with_timeout(fd, buf, sizeof(buf), 1);

        if (n > 0) {
            /* 直接走协议解析执行（与普通客户端同一入口） */
            kvs_protocol(fd, buf, n, NULL);
        } else if (n == 0) {
            /* 超时发心跳 */
            if (safe_send(fd, HEARTBEAT_CMD, HEARTBEAT_CMD_LEN) < 0) break;
        } else {
            SYNC_LOG_S("master connection closed");
            break;
        }
    }

    ctx->is_active = 0;
    if (fd > 0) { close(fd); ctx->fd = -1; }
    return NULL;
}


// ===== 从节点：连接主机 ============
static int slave_connect_to_master(const char *ip, int port) {
    SYNC_LOG_S("connecting to master %s:%d", ip, port);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }

    SYNC_LOG_S("TCP Connection established !");
    return fd;
}

// ===== 从机：通知主机全量加载完成（新建命令连接发 SYNC-FINISHED） =====
static int slave_temp_sync_finished(const char *ip, int port) {

    int fd = slave_connect_to_master(ip, port);      /* 复用已有连接函数 */
    if (fd <= 0) return -1;

    safe_send(fd, fin, strlen(fin));
    close(fd);
    return 0;
}

// ===== 从机：接收全量数据并加载 =====
static void slave_recv_temp(int fd) {

#if ENABLE_RDMA
    if (g_config.rdma_enable) {

        if (slave_rdma_start() != 0) {
            SYNC_ERR("rdma client connect failed");
            close(fd);
            return;
        }
        /* 本机是 rdma client（slave_rdma_start 已建连），收全量到 TEMP_FILE */
        if (rdma_recv_file(TEMP_FILE, "AOF_SYNC_OK\n") < 0) {
            SYNC_ERR("rdma recv full data failed");
            close(fd);
            return;
        }
        SYNC_LOG_S("full data received, saved to %s", TEMP_FILE);
    } else {

        SYNC_ERR("rdma disabled, sync unsupported");
        close(fd);
        return;
    }
#endif


    /* 加载全量数据：.tmp 与快照格式相同，改名为本地快照后直接加载
     * 副作用是从机留下一份 SNAP_FILE 本地快照，重启也能恢复 */
    /* 重新同步会用新文件替换 SNAP：先停掉旧的保存通道，
     * 否则 io_uring 的 fd 还指向被 rename 掉的旧文件，后续保存全写丢 */
    persist_snap_stop();

    if (rename(TEMP_FILE, SNAP_FILE) != 0) {
        SYNC_ERR("rename %s to %s failed", TEMP_FILE, SNAP_FILE);
        close(fd);
        return;
    }
    if (load_snapshots() != 0) {
        SYNC_ERR("load full data from %s failed", SNAP_FILE);
        close(fd);
        return;
    }
    SYNC_LOG_S("full data loaded from %s", SNAP_FILE);

    /* 全量数据已就绪：重建 SNAP 落盘通道（fd 指向新快照文件，
     * 之后主机转发来的 SAVE 命令由此通道落盘）
     * 放在 load 之后、增量线程之前：避免把空表/半截数据存成快照 */
    if (persist_snap_start() != 0) {
        SYNC_ERR("[SNAP] async save start failed");
    }

    /* 先起增量接收线程：保证主机收到 FINISHED 后立刻补发的窗口缓存有人读
     * （eBPF 在收到 SYNC-FINISHED 的瞬间就会 flush，若从机还没在 recv 会压 TCP 缓冲） */
    init_aof_sync_ctx();
    g_aof_ctx.fd = fd;
    g_aof_ctx.is_active = 1;
    pthread_create(&g_aof_ctx.recv_thread, NULL, slave_sync_recv_thread, &g_aof_ctx);
    pthread_detach(g_aof_ctx.recv_thread);

    /* 再通知主机全量完成（触发主机 eBPF 补发缓存 + 开启正常转发） */
    if (slave_temp_sync_finished(g_config.master_ip, g_config.master_port) != 0) {
        SYNC_ERR("notify SYNC-FINISHED failed");
        return;
    }

#if ENABLE_RDMA
    if (g_config.rdma_enable) {
        rdma_client_destroy();           /* 从机销毁 rdma client 资源 */
    }
#endif

    SYNC_LOG_S("incremental recv thread started");
}
// ===== 从节点：建立连接，并发起SYNC同步 ============
int sync_from_master(const char *master_ip, int port) {

    /* 1：从机连接主机，并发SYNC同步标志*/
    int fd = slave_connect_to_master(master_ip, port);
    if (fd <= 0) {
        LOG_ERROR("connect to master failed");
    }
    if (safe_send(fd, SYNC_CMD, strlen(SYNC_CMD)) < 0) {
        LOG_ERROR("send SYNC error");
        close(fd);
        return -1;
    }

    /* 2：等主机回ok */
    char ack[8] = {0};
    int ack_len = 0;
    while (ack_len < 3) {
        int n = recv(fd, ack + ack_len, 3 - ack_len, 0);
        if (n <= 0) {
            SYNC_ERR("no ack from master");
            close(fd);
            return -1;
        }
        ack_len += n;
    }
    if (strncmp(ack, "ok", 2) != 0) {
        SYNC_ERR("sync rejected by master: %s", ack);
        close(fd);
        return -1;
    }

    /* 3：启动rdma client，接收tmp文件*/
    slave_recv_temp(fd);
    return 0;
}







