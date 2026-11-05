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

// ===== 统一的同步上下文（含心跳） =====
typedef struct {
    int fd;
    int is_active;
    pthread_t recv_thread;
} snap_sync_ctx_t;
static snap_sync_ctx_t g_snap_ctx  = {0};

int safe_send(int fd, const char* data, int len) {
    int total = 0;
    while (total < len) {
        int n = send(fd, data + total, len - total, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
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

// ===== 主节点：把本地文件内容循环发送给 fd，末尾追加 end_marker =====
int send_file_content(int fd, const char *filepath, long max_bytes, const char *end_marker) {
    if (!filepath || !end_marker) return -1;

    struct stat st;
    if (stat(filepath, &st) != 0) {
        SYNC_ERR("stat %s failed", filepath);
        return -1;
    }

    long to_send = st.st_size;
    if (max_bytes > 0 && max_bytes < to_send) to_send = max_bytes;
    if (to_send > 0) {
        FILE *fp = fopen(filepath, "rb");
        if (!fp) { SYNC_ERR("open %s failed", filepath); return -1; }

        char buf[1024];
        long sent_total = 0;
        size_t n;
        while (sent_total < to_send &&
               (n = fread(buf, 1, (to_send - sent_total) < (long)sizeof(buf)
                                 ? (size_t)(to_send - sent_total) : sizeof(buf), fp)) > 0) {
            if (safe_send(fd, buf, n) < 0) {
                SYNC_ERR("send content failed");
                fclose(fp);
                return -1;
            }
            sent_total += n;
        }
        fclose(fp);
        SYNC_LOG_M("Sent %ld bytes from %s (limit=%ld)", sent_total, filepath, to_send);
    }

    if (safe_send(fd, end_marker, strlen(end_marker)) < 0) {
        SYNC_ERR("send marker failed");
        return -1;
    }
    return 0;
}

// ===== 从节点：循环接收内容写入 filepath，遇到 end_marker 停止并截断 =====
int recv_file_content(int fd, const char *filepath, const char *end_marker) {
    if (!filepath || !end_marker) return -1;

    FILE *fp = fopen(filepath, "wb");
    if (!fp) {
        SYNC_ERR("open %s failed", filepath);
        return -1;
    }

    char buf[1024];
    int total = 0;
    int end_received = 0;
    char tail[64] = {0};
    int tail_len = 0;
    int end_len = strlen(end_marker);

    if (end_len >= (int)sizeof(tail)) {
        SYNC_ERR("end_marker too long");
        fclose(fp);
        return -1;
    }

    while (!end_received) {
        int n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            SYNC_ERR("recv failed or peer closed");
            fclose(fp);
            return -1;
        }

        fwrite(buf, 1, n, fp);
        total += n;

        int i;
        for (i = 0; i < n; i++) {
            if (tail_len < (int)sizeof(tail) - 1) {
                tail[tail_len++] = buf[i];
                tail[tail_len] = '\0';
            } else {
                memmove(tail, tail + 1, sizeof(tail) - 2);
                tail[sizeof(tail) - 2] = buf[i];
                tail[sizeof(tail) - 1] = '\0';
            }
            if (tail_len >= end_len &&
                strcmp(tail + tail_len - end_len, end_marker) == 0) {
                end_received = 1;
                break;
            }
        }
    }

    fclose(fp);

    /* 截掉末尾的 end_marker */
    struct stat st;
    if (stat(filepath, &st) == 0 && st.st_size >= end_len) {
        truncate(filepath, st.st_size - end_len);
    }

    SYNC_LOG_S("Received %d bytes, saved to %s", total - end_len, filepath);
    return 0;
}

// ===== 主节点：判断是不是从机的 SAVE 请求 =====
int is_slave_save(int fd) {
    return (g_snap_ctx.is_active && g_snap_ctx.fd == fd);
}

// ===== 主节点： fd 是否为同步（复制）连接 =====
int is_sync_peer(int fd) {
    if (fd < 0) return 0;
    #if ENABLE_AOF_SYNC
    if (g_aof_ctx.is_active && g_aof_ctx.fd == fd) return 1;
    #endif
    if (g_snap_ctx.is_active && g_snap_ctx.fd == fd) return 1;
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
    LOG_INFO("full data saved to %s", TEMP_FILE);
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
    LOG_INFO("full sync: %s pushed via rdma", TEMP_FILE);
    return NULL;
}

// ===== 主机：收到从机的SYNC命令 =====
int handle_sync_request(int fd) {
    
    /* 1: 把内存全量数据保存进temp文件 */
    if (0 != master_save_tmp()) {
        LOG_INFO("save all mem data to tmp file failed");
        close(fd);
        return -1;
    }
    LOG_INFO("save all mem data to tmp file success");
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
    LOG_INFO("SYNC-FINISHED received, %s removed", TEMP_FILE);

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

    SYNC_LOG_S("connected to master");
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

    /* 给主机发送全量完成标志SYNC-FINISHED */
    if (slave_temp_sync_finished(g_config.master_ip, g_config.master_port) != 0) {
        SYNC_ERR("notify SYNC-FINISHED failed");
        return;
    }

#if ENABLE_RDMA
    if (g_config.rdma_enable) {
        rdma_client_destroy();           /* 从机销毁 rdma client 资源 */
    }
#endif

    /* 增量接收：复用这条连接，收 eBPF 转发的 RESP 命令流直接执行 */
    init_aof_sync_ctx();
    g_aof_ctx.fd = fd;
    g_aof_ctx.is_active = 1;
    pthread_create(&g_aof_ctx.recv_thread, NULL, slave_sync_recv_thread, &g_aof_ctx);
    pthread_detach(g_aof_ctx.recv_thread);
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







