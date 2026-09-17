#include "kvstore.h"
#include "server.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>  
#include <fcntl.h>     
#include <errno.h>


#if ENABLE_AOF_SYNC


// AOF 增量同步上下文（长连接）
typedef struct {
    int fd;                    // 同步连接fd
    int is_active;             // 是否活跃
    long last_size[4];         // 每个AOF文件的最后大小
    time_t last_heartbeat;     // 上次心跳时间
    pthread_t recv_thread;     // 从节点接收线程
    pthread_t check_thread;    // 主节点检测线程
} aof_sync_ctx_t;

static aof_sync_ctx_t g_aof_ctx = {0};

// AOF 文件列表（与 kvs_persist.c 保持一致）
static const char* aof_files[] = {
    AOF_FILE
};

// 初始化 AOF 上下文
void init_aof_sync_ctx(void) {
    memset(&g_aof_ctx, 0, sizeof(g_aof_ctx));
    g_aof_ctx.fd = -1;
    g_aof_ctx.is_active = 0;
    g_aof_ctx.last_heartbeat = time(NULL);
}

#endif


static int safe_send(int fd, const char* data, int len) {
    int total = 0;
    while (total < len) {
        int n = send(fd, data + total, len - total, 0);
        if (n <= 0) return -1;
        total += n;
    }
    return total;
}

// 发送单个文件
int send_file(int fd, const char *filepath) {
    
    if (!filepath) return -1;
    struct stat st;
    if (stat(filepath, &st) != 0) {
        perror("stat");
        return -1;
    }

    size_t file_size = st.st_size;
    printf("Sending: %s -- %zu bytes\n", filepath, file_size);

    // 发送文件名和大小
    char header[512];
    snprintf(header, sizeof(header), "%s|%zu\n", filepath, file_size);
#if ENABLE_NOBLOCK
    size_t sent = 0;
    size_t header_len = strlen(header);
    int retry_count = 0;
    while (sent < header_len) {
        int n = send(fd, header + sent, header_len - sent, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                retry_count++;
                // printf("  [DEBUG] send EAGAIN, retry #%d\n", retry_count);  // ← 加这行
                continue;  // 重试
            }
            return -1;
        }
        sent += n;
    }
    if (retry_count > 0) {
        // printf("  [DEBUG] header sent with %d retries\n", retry_count);
    }
#else
    send(fd, header, strlen(header), 0);
#endif
    // 发送文件内容
    FILE *fp = fopen(filepath, "rb");
    if (!fp) return -1;

    char buf[1024];
    size_t n;
#if ENABLE_NOBLOCK
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        size_t written = 0;
        while (written < n) {
            int ret = send(fd, buf + written, n - written, 0);
            if (ret < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                perror("send data");
                fclose(fp);
                return -1;
            }
            written += ret;
        }
    }
#else
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        send(fd, buf, n, 0);
    }
#endif
    fclose(fp);
    return 0;

}

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
// 接收文件
int recv_file(int fd, const char *save_dir) {
    char header[512];
    char buf[8192];
    
    int n = recv(fd, header, sizeof(header)-1, 0);
    if (n <= 0) return -1;
    header[n] = '\0';
    
    char *name = header;
    char *size_str = strchr(header, '|');
    if (!size_str) return -1;
    *size_str = '\0';
    size_str++;
    size_t size = atol(size_str);
    
    char *base = strrchr(name, '/');
    if (base) base++; else base = name;
    
    char save_path[512];
    snprintf(save_path, sizeof(save_path), "%s/%s", save_dir, base);
    
    printf("Receiving: %s (%zu bytes)\n", save_path, size);
    
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", save_dir);
    system(cmd);
    
    FILE *fp = fopen(save_path, "wb");
    if (!fp) return -1;
    
    size_t received = 0;
    while (received < size) {
        size_t to_read = (size - received > sizeof(buf)) ? sizeof(buf) : size - received;
        n = recv(fd, buf, to_read, 0);
        if (n <= 0) break;
        fwrite(buf, 1, n, fp);
        received += n;
    }
    
    fclose(fp);
    return (received == size) ? 0 : -1;
}

#if ENABLE_AOF_SYNC

// ===== 主节点：检测 AOF 变化并发送 =====
void* aof_master_check_thread(void* arg) {
    // char buffer[2000];
    
    while (g_aof_ctx.is_active && g_aof_ctx.fd > 0) {
        sleep(5);
        
        // 心跳
        if (safe_send(g_aof_ctx.fd, "PING\n", 5) < 0) {
            printf("[AOF Sync] Send heartbeat failed\n");
            g_aof_ctx.is_active = 0;
            close(g_aof_ctx.fd);
            g_aof_ctx.fd = -1;
            break;
        }
        int i = 0;
        // 检查4个AOF文件
        for (i = 0; i < 4; i++) {
            struct stat st;
            if (stat(aof_files[i], &st) != 0) continue;
            if (st.st_size <= g_aof_ctx.last_size[i]) continue;
            
            FILE* fp = fopen(aof_files[i], "r");
            if (!fp) continue;
            
            fseek(fp, g_aof_ctx.last_size[i], SEEK_SET);
            
            char* line = NULL;
            size_t line_len = 0;
            ssize_t read;
            int sent_count = 0;
            
            while ((read = getline(&line, &line_len, fp)) > 0) {
                if (safe_send(g_aof_ctx.fd, line, read) < 0) {
                    free(line);
                    fclose(fp);
                    goto cleanup;
                }
                sent_count++;
            }
            
            free(line);
            fclose(fp);
            
            if (sent_count > 0) {
                g_aof_ctx.last_size[i] = st.st_size;
                printf("[AOF Sync] Sent %d lines from %s\n", sent_count, aof_files[i]);
            }
        }
    }
    
cleanup:
    g_aof_ctx.is_active = 0;
    if (g_aof_ctx.fd > 0) {
        close(g_aof_ctx.fd);
        g_aof_ctx.fd = -1;
    }
    printf("[AOF Sync] Master check thread exiting\n");
    return NULL;
}

// ===== 从节点：接收 AOF 增量数据 =====

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

// ===== 从节点：接收 AOF 增量数据（修复半包/粘包问题） =====
void* aof_slave_recv_thread(void* arg)
{
    aof_sync_ctx_t* ctx = (aof_sync_ctx_t*)arg;
    int fd = ctx->fd;

    // 跨 recv 的累积缓冲：不完整的行保留下次继续拼接
    static const int ACCUM_CAP = 128 * 1024;   // 单条命令最大 128KB
    char *accum = (char*)malloc(ACCUM_CAP);
    if (!accum) {
        printf("[AOF Sync] accum malloc failed\n");
        ctx->is_active = 0;
        return NULL;
    }
    int accum_len = 0;

    printf("[AOF Sync] Slave receive thread started on fd %d\n", fd);

    while (ctx->is_active && fd > 0) {
        // 1. 尽量多收数据，追加到累积缓冲末尾（不清空旧数据）
        int n = recv(fd, accum + accum_len, ACCUM_CAP - accum_len - 1, 0);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                usleep(1000);
                continue;
            }
            printf("[AOF Sync] Master connection closed (recv=%d)\n", n);
            break;
        }
        accum_len += n;
        accum[accum_len] = '\0';
        ctx->last_heartbeat = time(NULL);

        // 2. 处理心跳：只在"整段数据开头"是 PING 行时处理
        if (accum_len >= 5 && strncmp(accum, "PING\n", 5) == 0) {
            safe_send(fd, "PONG\n", 5);
            memmove(accum, accum + 5, accum_len - 5);
            accum_len -= 5;
            accum[accum_len] = '\0';
        }

        // 3. 处理所有完整行，保留不完整的尾巴
        char *line = accum;
        char *nl;
        while ((nl = memchr(line, '\n', accum_len - (line - accum))) != NULL) {
            *nl = '\0';
            if (strlen(line) > 0) {
                aof_slave_exec_line(line);
            }
            line = nl + 1;
        }

        // 4. 把残留的半包移到缓冲区开头，等待下次 recv 拼接
        int remain = accum_len - (line - accum);
        if (remain > 0) {
            memmove(accum, line, remain);
        }
        accum_len = remain;
        accum[accum_len] = '\0';

        // 5. 防御：半包超过缓冲区容量，说明数据异常，丢弃重置
        if (accum_len >= ACCUM_CAP - 1) {
            printf("[AOF Sync] Warning: oversize line dropped (%d bytes)\n", accum_len);
            accum_len = 0;
            accum[0] = '\0';
        }
    }

    free(accum);
    ctx->is_active = 0;
    if (fd > 0) {
        close(fd);
        ctx->fd = -1;
    }
    printf("[AOF Sync] Slave receive thread exiting\n");
    return NULL;
}


#endif // ENABLE_AOF_SYNC



// 从节点专用的接收函数（非阻塞）
int recv_file_from_master(int fd, const char *save_dir) {
    char header[512];
    char buf[8192];
    
#if ENABLE_NOBLOCK
    // 先 peek 一下，看是不是 END（非阻塞）
    char peek_buf[4];
    int n = recv(fd, peek_buf, 4, MSG_PEEK | MSG_DONTWAIT);
    if (n == 4 && strncmp(peek_buf, "END\n", 4) == 0) {
        return -1;  // 是 END
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return -2;  // 数据还没到，下次再试
    }
    // 读取 header（逐字节读取，避免粘包）
    int header_len = 0;
    char ch;
    while (header_len < sizeof(header) - 1) {
        n = recv(fd, &ch, 1, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return -2;  // 数据还没到，下次再试
            }
            return -1;
        }
        if (n == 0) return -1;
        if (ch == '\n') break;
        header[header_len++] = ch;
    }
    header[header_len] = '\0';
    if (header_len == 0) {
    return -1;
}
#else
    
    int n = recv(fd, header, 4, MSG_PEEK);
    if (n == 4 && strncmp(header, "END\n", 4) == 0) {
        // 是 END，让调用者处理
        return -1;
    }
    
    // 接收 header
    n = recv(fd, header, sizeof(header)-1, 0);
    if (n <= 0) return -1;
    header[n] = '\0';
#endif
// 解析 header
    char *name = header;
    char *size_str = strchr(header, '|');
    if (!size_str) return -1;
    *size_str = '\0';
    size_str++;
    size_t size = atol(size_str);
    
    char *base = strrchr(name, '/');
    if (base) base++; else base = name;
    
    char save_path[512];
    snprintf(save_path, sizeof(save_path), "%s/%s", save_dir, base);
    
    printf("Receiving: %s (%zu bytes)\n", save_path, size);
    
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", save_dir);
    system(cmd);
    
    FILE *fp = fopen(save_path, "wb");
    if (!fp) return -1;
    
    size_t received = 0;
#if ENABLE_NOBLOCK
    while (received < size) {
        size_t to_read = (size - received > sizeof(buf)) ? sizeof(buf) : size - received;
        n = recv(fd, buf, to_read, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 数据还没到，等待一下再试
                usleep(1000);
                continue;
            }
            break;
        }
        if (n == 0) break;
        fwrite(buf, 1, n, fp);
        received += n;
    }
#else
    while (received < size) {
        size_t to_read = (size - received > sizeof(buf)) ? sizeof(buf) : size - received;
        n = recv(fd, buf, to_read, 0);
        if (n <= 0) break;
        fwrite(buf, 1, n, fp);
        received += n;
    }
#endif
    fclose(fp);
    return (received == size) ? 0 : -1;
}
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



