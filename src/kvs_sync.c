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
    long last_size[1];         // 每个AOF文件的最后大小
    time_t last_heartbeat;     // 上次心跳时间
    pthread_t recv_thread;     // 从节点接收线程
    pthread_t check_thread;    // 主节点检测线程
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


#if 0
// 主节点
int send_all_files(int fd) {
    int i = 0;
    const char *files[] = {
        SNAP_FILE,
        NULL
    };
    // 发送文件
    for (i = 0; files[i]; i++) {
        if (access(files[i], R_OK) == 0) {
            send_file(fd, files[i]);
        }
    }
#if ENABLE_NOBLOCK
    // 发送结束标记（循环发送，处理 EAGAIN）
    const char *end = "END\n";
    size_t end_len = 4;
    size_t sent = 0;
    while (sent < end_len) {
        int n = send(fd, end + sent, end_len - sent, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return -1;
        }
        sent += n;
    }
#else
    // 发送结束标记
    send(fd, "END\n", 4, 0);
#endif
    return 0;
}
#endif

/**
 * @func 主节点收到sync，根据同步模式分发数据
 * aof：
 *      立即同步：每条cmd执行aof_master_immediate
 *      定时同步：5s检查aof文件，并发送
 * snap：
 */

// ===== 主节点：定时同步（everysec） =====
void* master_aof_everysec_thread(void* arg) {
    while (g_aof_ctx.is_active && g_aof_ctx.fd > 0) {
        sleep(5);
        struct stat st;
        if (stat(AOF_FILE, &st) != 0) continue;
        long start = g_aof_ctx.last_size[0];
        long to_send = st.st_size - start;
        if (to_send <= 0) continue;

        FILE* fp = fopen(AOF_FILE, "r");
        if (!fp) continue;
        fseek(fp, start, SEEK_SET);

        char buf[4096];
        long sent = 0;
        size_t n;
        while (sent < to_send &&
               (n = fread(buf, 1,
                   (to_send - sent) < (long)sizeof(buf) ?
                   (size_t)(to_send - sent) : sizeof(buf), fp)) > 0) {
            if (safe_send(g_aof_ctx.fd, buf, n) < 0) {
                fclose(fp);
                goto cleanup;
            }
            sent += n;
        }
        fclose(fp);
        g_aof_ctx.last_size[0] = start + sent;
        SYNC_LOG_M("AOF everysec: sent [%ld, %ld) = %ld bytes", start, start + sent, sent);
    }
cleanup:
    g_aof_ctx.is_active = 0;
    return NULL;
}

// ===== 主节点：立即同步 (immediate) =====
int master_aof_immediate(const char *cmd, const char *key, const char *value) {
    if (g_config.aof_mode != AOF_MODE_IMMEDIATE) return 0;
    if (!g_aof_ctx.is_active || g_aof_ctx.fd <= 0) return 0;

    char buf[4096];
    int len = 0;
    if (value) {
        len = snprintf(buf, sizeof(buf),
                       "*1\r\n*3\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                       strlen(cmd), cmd,
                       strlen(key), key,
                       strlen(value), value);
    } else {
        len = snprintf(buf, sizeof(buf),
                       "*1\r\n*2\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                       strlen(cmd), cmd,
                       strlen(key), key);
    }
    return safe_send(g_aof_ctx.fd, buf, len);
}

// ===== 主节点：判断是不是从机的 SAVE 请求 =====
int is_slave_save(int fd) {
    return (g_snap_ctx.is_active && g_snap_ctx.fd == fd);
}

// ===== 主机：处理 SYNC 请求 =====
int handle_sync_request(int fd) {
    // printf("Handling SYNC request on fd %d\n", fd);

    // 1. 发送模式
    if (g_config.aof_enable) {
        safe_send(fd, "aof\n", 4);
    } else if (g_config.snap_enable) {
        safe_send(fd, "snap\n", 5);
    } else {
        safe_send(fd, "none\n", 5);
        close(fd);
        return -1;
    }

    // 2. 根据模式处理
    if (g_config.aof_enable) {
        SYNC_LOG_M("handle SYNC request on fd=%d, mode=%s", fd,
           g_config.aof_enable ? "aof" : (g_config.snap_enable ? "snap" : "none"));

        init_aof_sync_ctx();
        g_aof_ctx.fd = fd;
        g_aof_ctx.is_active = 1;

        // ★ 记录发送开始时的文件大小，作为快照边界
        struct stat st;
        long snap_size = 0;
        if (stat(AOF_FILE, &st) == 0) snap_size = st.st_size;

        // 只发送 snap_size 字节，末尾加 marker
        if (send_file_content(fd, AOF_FILE, snap_size, "AOF_SYNC_OK\n") < 0) {
            SYNC_ERR("send AOF content failed");
            close(fd);
            return -1;
        }

        // ★ 关键：last_size = snap_size，而不是发送后的文件大小
        g_aof_ctx.last_size[0] = snap_size;

        if (g_config.aof_mode == AOF_MODE_EVERYSEC) {
            pthread_create(&g_aof_ctx.check_thread, NULL, master_aof_everysec_thread, NULL);
            pthread_detach(g_aof_ctx.check_thread);
        }

    } else if (g_config.snap_enable) {
        SYNC_LOG_M("SNAP mode: waiting for slave SAVE");

        // 记录从机 fd，等从机发 SAVE
        g_snap_ctx.fd = fd;
        g_snap_ctx.is_active = 1;

        safe_send(fd, "SNAP_SYNC_OK\n", 13);
    }

    return 0;
}



#if 0
// ===== 从节点：解析并执行一条同步命令 =====
// line: 已去掉 \n 的一条命令字符串，如 "SET lhy2 AAAA..."
static void aof_slave_exec_line(const char *line)
{
    // ===== 使用动态分配，支持大 value =====
    char cmd[64];
    char key[256];
    char *val = NULL;

    size_t line_len = strlen(line);

    // 拷贝到临时缓冲做字段切分
    char *temp = (char*)malloc(line_len + 1);
    if (!temp) {
        printf("[AOF Sync] malloc failed for line (%zu bytes)\n", line_len);
        return;
    }
    memcpy(temp, line, line_len + 1);

    char *space1 = strchr(temp, ' ');
    if (!space1) {
        free(temp);
        return; // 只有一个字段，不是合法命令
    }
    *space1 = '\0';
    strncpy(cmd, temp, sizeof(cmd) - 1);
    cmd[sizeof(cmd) - 1] = '\0';

    char *rest = space1 + 1;
    char *space2 = strchr(rest, ' ');

    if (!space2) {
        // 只有两个字段 (DEL 类命令)
        strncpy(key, rest, sizeof(key) - 1);
        key[sizeof(key) - 1] = '\0';

        char *tokens[4] = {cmd, key, NULL, NULL};
        kvs_filter_protocol(-1, tokens, 2, NULL);
        free(temp);
        return;
    }

    *space2 = '\0';
    strncpy(key, rest, sizeof(key) - 1);
    key[sizeof(key) - 1] = '\0';

    // value 从 space2+1 开始，可能很大
    char *value_start = space2 + 1;
    size_t value_len = strlen(value_start);

    printf("[AOF Sync] cmd=%s, key=%s, value_len=%zu bytes\n",
           cmd, key, value_len);

    // 动态分配 value 内存
    val = (char*)malloc(value_len + 1);
    if (!val) {
        printf("[AOF Sync] malloc failed for value_len=%zu\n", value_len);
        free(temp);
        return;
    }
    memcpy(val, value_start, value_len + 1);

    // 执行命令（append_aof 会在 kvs_filter_protocol 内部完成，写入从机 AOF）
    char *tokens[4] = {cmd, key, val, NULL};
    kvs_filter_protocol(-1, tokens, 3, NULL);

    free(val);
    free(temp);
}
#endif
#if ENABLE_SYNC_MODIFY

/**
 * @func 从节点发起sync同步, 根据master的模式进行数据备份
 * aof：全量接收命令，replay_aof_from加载到内存
 * snap：全量接收数据，落盘，再load加载
 */
// ===== 从节点：接收 AOF 增量数据（修复半包/粘包问题） =====
void* slave_aof_recv_thread(void* arg) {
    aof_sync_ctx_t* ctx = (aof_sync_ctx_t*)arg;
    int fd = ctx->fd;

    char buf[4096];

    LOG_INFO("[AOF Sync] Slave receive thread started on fd %d\n", fd);

    while (ctx->is_active && fd > 0) {
        /* 1 秒超时：有数据就 append，超时后统一 replay + 发心跳 */
        int n = recv_with_timeout(fd, buf, sizeof(buf), 1);

        if (n > 0) {
            /* 只 append，不 replay */
            FILE *fp = fopen(AOF_FILE, "a");
            if (fp) {
                fwrite(buf, 1, n, fp);
                fclose(fp);
            } else {
                LOG_ERROR("[AOF Sync] open %s for append failed\n", AOF_FILE);
            }

        } else if (n == 0) {
            /* 超时：把当前攒下的数据一次性 replay */
            struct stat st;
            if (stat(AOF_FILE, &st) == 0 && st.st_size > ctx->consumed_offset) {
#if ENABLE_CRC
                /*  从 consumed_offset 起，只处理新增部分 */
                long new_offset = replay_aof_from_with_crc(
                    AOF_FILE, ctx->consumed_offset, &g_crc_list_aof);
                if (new_offset > ctx->consumed_offset) {
                    ctx->consumed_offset = new_offset;
                    crc_list_save(&g_crc_list_aof, CRC_LIST_AOF_FILE);
                }
#else
                long new_offset = replay_aof_from(AOF_FILE, ctx->consumed_offset);
                if (new_offset > ctx->consumed_offset) {
                    ctx->consumed_offset = new_offset;
                }
#endif
            }

            /* 发心跳 */
            if (safe_send(fd, HEARTBEAT_CMD, HEARTBEAT_CMD_LEN) < 0) {
                LOG_ERROR("[Heartbeat] Send failed\n");
                break;
            }
        } else {
            LOG_WARN("[AOF Sync] Master connection closed\n");
            break;
        }
    }

    ctx->is_active = 0;
    if (fd > 0) {
        close(fd);
        ctx->fd = -1;
    }
    LOG_INFO("[AOF Sync] Slave receive thread exiting\n");
    return NULL;
}

// ===== 从节点：SNAP 接收线程（每 10s 发 SAVE）=====
void* slave_snap_recv_thread(void* arg) {
    snap_sync_ctx_t* ctx = (snap_sync_ctx_t*)arg;
    int fd = ctx->fd;

    SYNC_LOG_S("SNAP recv thread started, fd=%d", fd);

    while (ctx->is_active && fd > 0) {
        sleep(10);

        // 1. 发 SAVE 给主机（SAVE 本身即心跳）
        const char *save_cmd = "*1\r\n*1\r\n$4\r\nSAVE\r\n";
        if (safe_send(fd, save_cmd, strlen(save_cmd)) < 0) break;


        /* 2. 接收 SNAP 内容并写入 SNAP_FILE */
        if (recv_file_content(fd, SNAP_FILE, "SNAP_END\n") < 0) {
            SYNC_ERR("SNAP recv content failed");
            break;
        }
        load_snapshots();
        SYNC_LOG_S("SNAP loaded");
#if ENABLE_CRC
        crc_rebuild_snap_list();
#endif
    }
    ctx->is_active = 0;
    if (fd > 0) {
        close(fd);
        ctx->fd = -1;
    }
    SYNC_LOG_S("SNAP recv thread exiting");
    return NULL;
}

// ===== 从节点：处理主机发来的aof命令 =====
void slave_handle_aof_mode(int fd) {
    // ===== 1. 接收 AOF 文件（全量） =====
    SYNC_LOG_S("receiving AOF full stream...");

    /* 接收 AOF 内容并写入本地 AOF */
    if (recv_file_content(fd, AOF_FILE, "AOF_SYNC_OK\n") < 0) {
        SYNC_ERR("recv AOF content failed");
        close(fd);
        return;
    }
#if ENABLE_CRC
    /*  从机清空本地 CRC 列表（对齐主机的全量快照点） */
    crc_list_reset(&g_crc_list_aof);
#endif

    SYNC_LOG_S("AOF stream received, replaying...");
#if ENABLE_CRC
    long consumed = replay_aof_from_with_crc(AOF_FILE, 0, &g_crc_list_aof);
    if (consumed >= 0) {
        crc_list_save(&g_crc_list_aof, CRC_LIST_AOF_FILE);
        SYNC_LOG_S("AOF full replay done, crc=%d", g_crc_list_aof.count);
    }
#else
    long consumed = replay_aof_from(AOF_FILE, 0);
#endif
    if (consumed < 0) {
        SYNC_ERR("replay_aof_from failed");
        close(fd);
        return;
    }
    SYNC_LOG_S("AOF replayed, consumed_offset=%ld", consumed);


    // 启动 AOF 增量接收线程（后续主机每 5s 发的新增命令）
    init_aof_sync_ctx();
    g_aof_ctx.fd = fd;
    g_aof_ctx.is_active = 1;
    g_aof_ctx.consumed_offset = consumed;
    pthread_create(&g_aof_ctx.recv_thread, NULL, slave_aof_recv_thread, &g_aof_ctx);
    pthread_detach(g_aof_ctx.recv_thread);

    SYNC_LOG_S("AOF incremental recv thread started");
}

// ===== 从节点：主机发来的snap数据 =====
void slave_handle_snap_mode(int fd) {
    char ack[32] = {0};
    int ack_len = 0;
    while (ack_len < (int)sizeof(ack) - 1) {
        int n = recv(fd, ack + ack_len, 1, 0);
        if (n <= 0) {
            close(fd);
            return;
        }
        if (ack[ack_len] == '\n') {
            break;
        }
        ack_len++;
    }
    if (strcmp(ack, "SNAP_SYNC_OK\n") != 0) {
        SYNC_ERR("SNAP sync rejected: %s", ack);
        close(fd);
        return;
    }

    g_snap_ctx.fd = fd;
    g_snap_ctx.is_active = 1;
    pthread_create(&g_snap_ctx.recv_thread, NULL, slave_snap_recv_thread, &g_snap_ctx);
    pthread_detach(g_snap_ctx.recv_thread);

    SYNC_LOG_S("SNAP recv thread started");
}

// ===== 从节点：建立连接，并发起SYNC同步 ============
int sync_from_master(const char *master_ip, int port) {
    SYNC_LOG_S("connecting to master %s:%d", master_ip, port);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, master_ip, &addr.sin_addr) <= 0) {
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

    // 发送SYNC命令到master，开始同步
    const char *sync_cmd = "*1\r\n*1\r\n$4\r\nSYNC\r\n";
    if (safe_send(fd, sync_cmd, strlen(sync_cmd)) < 0) {
        perror("send SYNC");
        close(fd);
        return -1;
    }

    // 接收 mode（逐字节读，避免吞掉后续的 AOF 内容）
    char mode[16] = {0};
    int mode_len = 0;
    while (mode_len < (int)sizeof(mode) - 1) {
        int n = recv(fd, mode + mode_len, 1, 0);
        if (n <= 0) {
            LOG_ERROR("Failed to receive mode\n");
            close(fd);
            return -1;
        }
        if (mode[mode_len] == '\n') {
            break;
        }
        mode_len++;
    }
    mode[strcspn(mode, "\r\n")] = '\0';
    SYNC_LOG_S("master mode: %s", mode);

    if (strcmp(mode, "aof") == 0) {
        slave_handle_aof_mode(fd);
    } else if (strcmp(mode, "snap") == 0) {
        slave_handle_snap_mode(fd);
    } else {
        SYNC_ERR("unknown mode: %s", mode);
        close(fd);
        return -1;
    }

    return 0;
}

#else
// 主节点----启动同步服务
int handle_sync_request(int fd) {
    printf("Handling SYNC request on fd %d\n", fd);

// 1. 发送 OK

    if (send(fd, "OK\n", 3, 0) < 0) {
        perror("send OK");
        return -1;
    }

    // 2. 保存快照
    if (g_config.snap_enable) {
        printf("Saving snapshots...\n");
        save_snapshots();
        // 3. 清空 AOF（先清空，再发送，避免发送期间新增数据）
        int i = 0;
        for (i = 0; i < 4; i++) {
            FILE *fp = fopen(aof_files[i], "w");
            if (fp) fclose(fp);
        }
        printf("AOF cleared\n");
        // 3. 发送快照文件
        send_all_files(fd);
        printf("SYNC completed for fd %d\n", fd);
    }
    

#if ENABLE_AOF_SYNC
    if (g_config.aof_enable) {
        printf("[AOF Sync] Switching to sync mode...\n");
        // 初始化上下文
        init_aof_sync_ctx();
        g_aof_ctx.fd = fd;
        g_aof_ctx.is_active = 1;
        g_aof_ctx.last_heartbeat = time(NULL);
        int i = 0;
        // 记录当前 AOF 文件大小
        for (i = 0; i < 4; i++) {
            struct stat st;
            if (stat(aof_files[i], &st) == 0) {
                g_aof_ctx.last_size[i] = st.st_size;
            } else {
                g_aof_ctx.last_size[i] = 0;
            }
            // printf("[AOF Sync] %s: %ld bytes\n", aof_files[i], g_aof_ctx.last_size[i]);
        }
        // 发送确认
    safe_send(fd, "AOF_SYNC_OK\n", 12);

        // 启动主节点检测线程（只启动一次）
        static int check_thread_started = 0;
        if (!check_thread_started) {
            pthread_create(&g_aof_ctx.check_thread, NULL, aof_master_check_thread, NULL);
            pthread_detach(g_aof_ctx.check_thread);
            check_thread_started = 1;
            printf("[AOF Sync] Master check thread started\n");
        }
    }  else {
        // 仅snap：发送结束标记，关闭连接
        safe_send(fd, "SYNC_DONE\n", 10);
        printf("Full sync only mode: connection closed\n");
        close(fd);
    }
    return 0;    
#endif
}

// ============ 从节点：发起 SYNC ============
// 全量同步和增量同步，都从从节点发送sync开始
int sync_from_master(const char *master_ip, int port) {

    printf("Connecting to master %s:%d\n", master_ip, port);

    // 连接主节点（作为普通客户端）
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, master_ip, &addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(fd);
        return -1;
    }
    
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }
    
    printf("Connected to master\n");
    
    // 发送 SYNC 命令
    send(fd, "SYNC\n", 5, 0);
    
    // // 接收 OK
    char resp[16];
    int n = recv(fd, resp, sizeof(resp)-1, 0);
    if (n <= 0) {
        printf("Failed to receive response\n");
        close(fd);
        return -1;
    }
    resp[n] = '\0';
    if (strcmp(resp, "OK\n") != 0) {
        printf("Sync rejected: %s\n", resp);
        close(fd);
        return -1;
    }

    // 如果开启了snap
    if (g_config.snap_enable) {
        printf("Receiving files...\n");
        // 接收所有文件
        while (1) {
            int ret = recv_file_from_master(fd, "data");
            if (ret != 0) {
                // 检查是否是 END 标记
                char buf[16];
                n = recv(fd, buf, sizeof(buf), 0);
                if (n > 0 && strcmp(buf, "END\n") == 0) {
                    printf("Received END marker\n");
                    break;
                }
            }
        }
        // 加载快照
        printf("Reloading snapshots from master...\n");
        load_snapshots();
        // ===== 清空从机 AOF =====
        int i;
        for (i = 0; i < 4; i++) {
            FILE *fp = fopen(aof_files[i], "w");
            if (fp) fclose(fp);
        }
        printf("Slave AOF cleared\n");
    }
    
#if ENABLE_AOF_SYNC
    // ===== 关键改动：不关闭连接，等待 AOF 增量同步 =====
    // 接收 AOF_SYNC_OK
    if(g_config.aof_enable) {
        char ack[32];
        n = recv(fd, ack, sizeof(ack)-1, 0);
        if (n <= 0) {
            printf("[AOF Sync] Failed to receive AOF sync ack\n");
            close(fd);
            return -1;
        }
        ack[n] = '\0';
        
        if (strcmp(ack, "AOF_SYNC_OK\n") != 0) {
            printf("[AOF Sync] AOF sync rejected: %s\n", ack);
            close(fd);
            return -1;
        }
        
        printf("[AOF Sync] AOF sync started!\n");
        // 保存连接
        init_aof_sync_ctx();
        g_aof_ctx.fd = fd;
        g_aof_ctx.is_active = 1;
        g_aof_ctx.last_heartbeat = time(NULL);
        
        // 启动从节点接收线程
        pthread_create(&g_aof_ctx.recv_thread, NULL, aof_slave_recv_thread, &g_aof_ctx);
        pthread_detach(g_aof_ctx.recv_thread);
        
        printf("[AOF Sync] Receive thread started\n");
    } else {
        // 仅全量模式
        char done[16];
        n = recv(fd, done, sizeof(done)-1, 0);
        if (n > 0 && strcmp(done, "SYNC_DONE\n") == 0) {
            printf("Full sync completed, connection closed\n");
        }
        close(fd);
    }

    
    return 0;
#else
    close(fd);
    
    // 重新加载数据
    printf("Reloading snap_data from master...\n");
    load_snapshots();
    
    printf("Sync completed\n");
    return 0;

#endif
}


#endif



