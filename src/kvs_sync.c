#include "kvstore.h"
#include "server.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>  
#include <fcntl.h>     
#include <errno.h>
#include <poll.h>


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

                struct pollfd pfd = {
                    .fd = fd,
                    .events = POLLOUT
                };

                int ret;

                do {
                    ret = poll(&pfd, 1, -1);
                } while (ret < 0 && errno == EINTR);

                if (ret <= 0 ||
                    (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                    return -1;
                }

                continue;
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

/* =====================================================================
 * 异步转发 P0 方案：预分配内存池 + FIFO 节点链表
 *
 * 主线程（生产者）：接收已成功执行的写命令 → 从内存池取节点 →
 *                   只复制 cmd/key/value 原文入队（不拼 RESP）→ 解锁返回
 * 发送线程（消费者）：等队列非空 → 一次摘取一批 → 锁外统一拼接 RESP 进
 *                   64KB 缓冲 → 批量 safe_send → 成功后整批回收节点；
 *                   失败则记录数量/内容/原因，标记连接失效，回收但不静默丢弃
 * 统一由内存池管理命令节点、队列顺序和空闲内存，运行期零 malloc
 * ===================================================================== */

#define FWD_POOL_SIZE     (64u << 20)   /* 内存池总大小：64MB，启动时一次分配 */
#define FWD_SLOT_DATA     1024          /* 单节点数据区：cmd\0key\0val 紧挨存放 */
#define FWD_BATCH_SIZE    (64u << 10)   /* 发送线程单批拼包上限 64KB */

typedef struct fwd_node {
    struct fwd_node *next;
    int cmd_len;                        /* 各字段长度均含末尾 '\0' */
    int key_len;
    int val_len;                        /* -1 表示无 value（如 HDEL），拼 *2 包 */
    char data[FWD_SLOT_DATA];
} fwd_node_t;

#define FWD_SLOT_COUNT  (FWD_POOL_SIZE / (int)sizeof(fwd_node_t))

static fwd_node_t *fwd_pool       = NULL;   /* 内存池首地址（进程生命周期） */
static fwd_node_t *fwd_free_list  = NULL;   /* 空闲节点链（回收的节点挂回这里） */
static fwd_node_t *fwd_head       = NULL;   /* FIFO 队头（消费者摘取端） */
static fwd_node_t *fwd_tail       = NULL;   /* FIFO 队尾（生产者追加端） */
static unsigned int fwd_cnt       = 0;      /* 队列当前条数 */

/* 运行统计 */
static unsigned long fwd_stat_enqueued     = 0;
static unsigned long fwd_stat_sent         = 0;
static unsigned long fwd_stat_drop_pool    = 0;   /* 内存池耗尽丢弃 */
static unsigned long fwd_stat_drop_fail    = 0;   /* 发送失败丢弃 */

static int fwd_run = 0;
static int fwd_started = 0;
static int fwd_failed = 0;

static pthread_mutex_t fwd_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t fwd_start_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_cond_t fwd_cond = PTHREAD_COND_INITIALIZER;

static pthread_t fwd_tid;


/* 启动时一次性分配内存池，并把所有节点串成空闲链 */
static int fwd_pool_init(void)
{
    if (fwd_pool != NULL)
        return 0;

    fwd_pool = (fwd_node_t *)malloc(FWD_POOL_SIZE);

    if (fwd_pool == NULL) {
        LOG_ERROR("[SYNC] forward pool malloc %u bytes failed", FWD_POOL_SIZE);
        return -1;
    }

    fwd_free_list = NULL;

    for (int i = FWD_SLOT_COUNT - 1; i >= 0; i--) {
        fwd_node_t *node = &fwd_pool[i];
        node->next = fwd_free_list;
        fwd_free_list = node;
    }

    LOG_INFO("[SYNC] forward pool ready: %u MB, %d slots",
             FWD_POOL_SIZE >> 20, FWD_SLOT_COUNT);
    return 0;
}


/* 后台发送线程：等非空 → 摘一批 → 锁外拼 RESP → 批量发送 → 回收 */
static void *fwd_send_thread(void *arg)
{
    (void)arg;

    char batch[FWD_BATCH_SIZE];

    while (1) {

        pthread_mutex_lock(&fwd_mutex);

        /* 队列空则等待；停止且已排空才退出 */
        while (fwd_run && fwd_cnt == 0) {
            pthread_cond_wait(&fwd_cond, &fwd_mutex);
        }

        if (!fwd_run && fwd_cnt == 0) {
            pthread_mutex_unlock(&fwd_mutex);
            break;
        }

        /* 一次摘取整批链表，立刻放锁（拼包和发送都在锁外） */
        fwd_node_t *chain = fwd_head;
        fwd_node_t *rest  = NULL;       /* 64KB 装不下的剩余部分 */
        fwd_head = NULL;
        fwd_tail = NULL;

        unsigned int batch_cnt = fwd_cnt;
        fwd_cnt = 0;

        pthread_mutex_unlock(&fwd_mutex);

        /* ---- 锁外统一拼接 RESP ---- */
        size_t len = 0;
        unsigned int fmt_cnt = 0;
        fwd_node_t *it = chain;

        while (it != NULL) {

            char *p = it->data;
            const char *cmd = p;    p += it->cmd_len;
            const char *key = p;    p += it->key_len;

            int n;
            if (it->val_len > 0) {
                n = snprintf(batch + len, FWD_BATCH_SIZE - len,
                    "*3\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n",
                    it->cmd_len - 1, cmd,
                    it->key_len - 1, key,
                    it->val_len - 1, p);
            } else {
                n = snprintf(batch + len, FWD_BATCH_SIZE - len,
                    "*2\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n",
                    it->cmd_len - 1, cmd,
                    it->key_len - 1, key);
            }

            if (n < 0 || n >= (int)(FWD_BATCH_SIZE - len))
                break;                      /* 本批装满，剩余留给下一轮 */

            len += (size_t)n;
            fmt_cnt++;
            it = it->next;
        }
        rest = it;

        /* ---- 批量发送 ---- */
        int fd = g_slave_incr_fd;
        int send_ok = 1;

        if (len > 0) {
            if (fd < 0 || safe_send(fd, batch, (int)len) < 0) {
                send_ok = 0;
            }
        }

        if (send_ok) {

            fwd_stat_sent += fmt_cnt;

            /* 发送成功的节点整批回收进空闲链（锁内，与生产者取节点互斥） */
            pthread_mutex_lock(&fwd_mutex);

            while (chain != rest) {
                fwd_node_t *next = chain->next;
                chain->next = fwd_free_list;
                fwd_free_list = chain;
                chain = next;
            }

            /* 装不下的剩余部分放回队头（它仍是完整链表，需修复队尾） */
            fwd_head = rest;

            if (rest != NULL) {
                fwd_node_t *t = rest;
                while (t->next != NULL) t = t->next;
                fwd_tail = t;
                fwd_cnt = batch_cnt - fmt_cnt;
            }

            pthread_mutex_unlock(&fwd_mutex);

        } else {

            /* 发送失败：记录数量、首条命令内容、失败原因，再回收——不静默丢弃 */
            const char *reason = (fd < 0) ? "no connection"
                                : "send failed";
            SYNC_ERR("[SYNC] forward batch FAILED: reason=%s, "
                     "batch=%u cmds %zu bytes, first_cmd=%s",
                     reason, batch_cnt, len,
                     chain != NULL ? chain->data : "?");

            fwd_stat_drop_fail += batch_cnt;

            /* 标记本次复制连接失败：停止生产者入队，等从机重新 SYNC 全量恢复 */
            pthread_mutex_lock(&fwd_mutex);
            fwd_failed = 1;
            g_slave_incr_fd = -1;
            pthread_mutex_unlock(&fwd_mutex);

            /* 回收整批节点（含未拼接的剩余），数据随重同步恢复 */
            while (chain != NULL) {
                fwd_node_t *next = chain->next;
                chain->next = fwd_free_list;
                fwd_free_list = chain;
                chain = next;
            }
        }
    }

    return NULL;
}


/* 启动发送线程（首次调用分配内存池；断线重连后可重复调用） */
static int fwd_thread_start(void)
{
    pthread_mutex_lock(&fwd_start_mutex);

    if (!fwd_started) {

        if (fwd_pool_init() != 0) {
            pthread_mutex_unlock(&fwd_start_mutex);
            return -1;
        }

        pthread_mutex_lock(&fwd_mutex);
        fwd_run = 1;
        fwd_failed = 0;
        pthread_cond_broadcast(&fwd_cond);
        pthread_mutex_unlock(&fwd_mutex);

        if (pthread_create(&fwd_tid, NULL,
                           fwd_send_thread, NULL) != 0) {

            pthread_mutex_lock(&fwd_mutex);
            fwd_run = 0;
            pthread_mutex_unlock(&fwd_mutex);

            pthread_mutex_unlock(&fwd_start_mutex);
            return -1;
        }

        fwd_started = 1;

    } else {
        /* 线程仍在运行（上次发送失败标记了 failed）：从机重新 SYNC 后恢复转发 */
        pthread_mutex_lock(&fwd_mutex);
        fwd_run = 1;
        fwd_failed = 0;
        pthread_mutex_unlock(&fwd_mutex);
    }

    pthread_mutex_lock(&fwd_mutex);

    int ok = fwd_run && !fwd_failed;

    pthread_mutex_unlock(&fwd_mutex);
    pthread_mutex_unlock(&fwd_start_mutex);

    return ok ? 0 : -1;
}


/* 主节点：已成功执行的写命令进入异步转发队列（生产者，只拷贝原文） */
void sync_net_forward_cmd(const char *cmd,
                          const char *key,
                          const char *value)
{
    if (g_config.ebpf_enable ||
        !g_is_master ||
        g_slave_incr_fd < 0)
        return;

    int cmd_len = (int)strlen(cmd) + 1;
    int key_len = (int)strlen(key) + 1;
    int val_len = (value != NULL) ? (int)strlen(value) + 1 : -1;

    int total = cmd_len + key_len + (val_len > 0 ? val_len : 0);

    /* 单条超长（本工程 key≤128/value≤512，正常到不了这里），防御性丢弃 */
    if (total > FWD_SLOT_DATA) {
        fwd_stat_drop_pool++;
        LOG_WARN("[SYNC] cmd too long (%d bytes) for slot, dropped", total);
        return;
    }

    pthread_mutex_lock(&fwd_mutex);

    if (!fwd_run || fwd_failed) {
        pthread_mutex_unlock(&fwd_mutex);
        return;
    }

    /* 内存池耗尽：从机长期不消费，丢弃并计数（等重同步恢复） */
    fwd_node_t *node = fwd_free_list;

    if (node == NULL) {
        pthread_mutex_unlock(&fwd_mutex);
        fwd_stat_drop_pool++;
        LOG_WARN("[SYNC] forward pool exhausted (%d slots), one cmd dropped",
                 FWD_SLOT_COUNT);
        return;
    }

    fwd_free_list = node->next;

    /* 只复制原文，RESP 拼接留给发送线程 */
    char *p = node->data;
    memcpy(p, cmd, (size_t)cmd_len);    p += cmd_len;
    memcpy(p, key, (size_t)key_len);    p += key_len;
    if (val_len > 0) {
        memcpy(p, value, (size_t)val_len);
    }
    node->cmd_len = cmd_len;
    node->key_len = key_len;
    node->val_len = val_len;
    node->next = NULL;

    /* 入队 + cnt 统计 */
    int was_empty = (fwd_cnt == 0);

    if (fwd_tail != NULL)
        fwd_tail->next = node;
    else
        fwd_head = node;

    fwd_tail = node;
    fwd_cnt++;
    fwd_stat_enqueued++;

    /* 只在队列空变非空时通知消费者 */
    if (was_empty) {
        pthread_cond_signal(&fwd_cond);
    }

    pthread_mutex_unlock(&fwd_mutex);
}


/* 停止发送线程：先唤醒排空队列再退出（锁外发送失败则按失败路径回收） */
void sync_net_forward_stop(void)
{
    pthread_mutex_lock(&fwd_start_mutex);

    if (!fwd_started) {
        pthread_mutex_unlock(&fwd_start_mutex);
        return;
    }

    pthread_mutex_lock(&fwd_mutex);

    fwd_run = 0;

    pthread_cond_broadcast(&fwd_cond);

    pthread_mutex_unlock(&fwd_mutex);

    pthread_join(fwd_tid, NULL);

    /* 线程已退出，把队列残留节点还回空闲链（连接已不可用，数据由重同步恢复） */
    pthread_mutex_lock(&fwd_mutex);

    unsigned int left = fwd_cnt;

    while (fwd_head != NULL) {
        fwd_node_t *next = fwd_head->next;
        fwd_head->next = fwd_free_list;
        fwd_free_list = fwd_head;
        fwd_head = next;
    }
    fwd_tail = NULL;
    fwd_cnt = 0;
    fwd_failed = 0;

    pthread_mutex_unlock(&fwd_mutex);

    if (left > 0) {
        LOG_WARN("[SYNC] forward stop: %u cmds still queued, recycled", left);
    }

    fwd_started = 0;

    pthread_mutex_unlock(&fwd_start_mutex);
}


/* 队列运行统计：容量 / 已入队 / 已发送 / 丢弃数 */
void sync_net_forward_stats(void)
{
    pthread_mutex_lock(&fwd_mutex);
    unsigned int in_queue = fwd_cnt;
    pthread_mutex_unlock(&fwd_mutex);

    LOG_INFO("[SYNC] forward stats: capacity=%d slots, in_queue=%u, "
             "enqueued=%lu, sent=%lu, drop_pool=%lu, drop_fail=%lu",
             FWD_SLOT_COUNT, in_queue,
             fwd_stat_enqueued, fwd_stat_sent,
             fwd_stat_drop_pool, fwd_stat_drop_fail);
}

/* 主从增量连接断开时调用 */
void sync_net_forward_on_disconnect(int fd)
{
    /* 不是当前主从同步连接，不处理 */
    if (fd != g_slave_incr_fd)
        return;

    /* 停止线程并排空队列 */
    sync_net_forward_stop();

    /* 清空连接标记 */
    g_slave_incr_fd = -1;

    LOG_INFO("[SYNC] forward connection disconnected");
}

#else


/* ===== 主机：网络转发模式——攒批同步直发 =====
 * 与 eBPF 转发自动二选一：ebpf.enable=false 时走本路径（增量同步总得有通道）。
 * 攒批策略：命令拼进静态缓冲，攒满 FWD_SYNC_BATCH 条或缓冲放不下时一次 send；
 * 一次 send 发多条可摊薄系统调用开销，且没有发送线程的调度抖动。
 * 注意：单条收发模式下最后不满一批的命令会随下一次写入顺带发出，
 * 从机侧最多滞后 (批大小-1) 条，属于攒批的固有语义。 */
#define FWD_SYNC_BATCH_MAX  5       /* 攒满 5 条统一 send */
#define FWD_SYNC_BUF_SIZE   4096    /* 攒批缓冲：5 条命令（单条上限约 700B）绰绰有余 */

static char fwd_sync_buf[FWD_SYNC_BUF_SIZE];
static int  fwd_sync_len = 0;       /* 缓冲中已积累的字节数 */
static int  fwd_sync_cnt = 0;       /* 缓冲中已积累的命令条数 */

/* 把缓冲里攒的命令一次性发给从机 */
static void fwd_sync_flush(void) {
    if (fwd_sync_len > 0 && g_slave_incr_fd > 0) {
        safe_send(g_slave_incr_fd, fwd_sync_buf, fwd_sync_len);
    }
    fwd_sync_len = 0;
    fwd_sync_cnt = 0;
}

void sync_net_forward_cmd(const char *cmd, const char *key, const char *value) {
    if (g_config.ebpf_enable || !g_is_master || g_slave_incr_fd <= 0) return;

    /* 无参命令（SAVE）：必须先冲掉攒批里的命令保住顺序，SAVE 自身立即发 */
    if (key == NULL) {
        fwd_sync_flush();
        char buf[128];
        int n = snprintf(buf, sizeof(buf), "*1\r\n$%zu\r\n%s\r\n",
                         strlen(cmd), cmd);
        if (n > 0 && n < (int)sizeof(buf)) {
            safe_send(g_slave_incr_fd, buf, n);
        }
        return;
    }

    static char one[768];   // 单条命令缓冲（key≤128 + value≤512 + RESP 头）
    int n;
    if (value) {
        n = snprintf(one, sizeof(one),
                     "*3\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                     strlen(cmd), cmd, strlen(key), key, strlen(value), value);
    } else {
        n = snprintf(one, sizeof(one),
                     "*2\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                     strlen(cmd), cmd, strlen(key), key);
    }
    if (n <= 0 || n >= (int)sizeof(one)) return;

    /* 缓冲放不下这一条：先冲掉已攒的，再入缓冲 */
    if (fwd_sync_len + n > (int)sizeof(fwd_sync_buf)) {
        fwd_sync_flush();
    }

    memcpy(fwd_sync_buf + fwd_sync_len, one, (size_t)n);
    fwd_sync_len += n;
    fwd_sync_cnt++;

    /* 攒满一批：统一 send */
    if (fwd_sync_cnt >= FWD_SYNC_BATCH_MAX) {
        fwd_sync_flush();
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

    int buf_size = 4 * 1024 * 1024;

    setsockopt(fd, SOL_SOCKET, SO_SNDBUF,
               &buf_size, sizeof(buf_size));

    setsockopt(fd, SOL_SOCKET, SO_RCVBUF,
               &buf_size, sizeof(buf_size));

#if ENABLE_FWD_QUEUE
    /* 只在普通网络转发模式启动后台线程 */
    if (!g_config.ebpf_enable) {

        if (fwd_thread_start() != 0) {
            LOG_ERROR("[SYNC] forward thread start failed");
            close(fd);
            return -1;
        }
    }
#endif
    /* 线程启动成功后，再发布增量同步连接 */
    g_slave_incr_fd = fd;

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







