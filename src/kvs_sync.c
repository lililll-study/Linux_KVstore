#include "kvstore.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>  
#include <sys/stat.h>  
#include <fcntl.h>     
#include "server.h"
#include <errno.h>
static int sync_fd = 1;


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
                printf("  [DEBUG] send EAGAIN, retry #%d\n", retry_count);  // ← 加这行
                continue;  // 重试
            }
            return -1;
        }
        sent += n;
    }
    if (retry_count > 0) {
        printf("  [DEBUG] header sent with %d retries\n", retry_count);
    }
#else
    send(fd, header, strlen(header), 0);
#endif
    // 发送文件内容
    FILE *fp = fopen(filepath, "rb");
    if (!fp) return -1;

    char buf[SYNC_BUFFER_SIZE];
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
        SNAP_ARRAY_FILE,
        SNAP_RBTREE_FILE,
        SNAP_HASH_FILE,
        SNAP_SKIPTABLE_FILE,
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
// 启动同步服务
int handle_sync_request(int fd) {
    printf("Handling SYNC request on fd %d\n", fd);

// 1. 发送 OK（循环发送，处理 EAGAIN）
#if ENABLE_NOBLOCK
    const char *ok = "OK\n";
    size_t ok_len = 3;
    size_t sent = 0;
    while (sent < ok_len) {
        int n = send(fd, ok + sent, ok_len - sent, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            perror("send OK");
            return -1;
        }
        sent += n;
    }
#else
    if (send(fd, "OK\n", 3, 0) < 0) {
        perror("send OK");
        return -1;
    }
#endif

    // 2. 保存快照
    printf("Saving snapshots...\n");
    save_snapshots();
    // 3. 发送快照文件
    send_all_files(fd);
    // ===== 新增：清空 AOF =====
    FILE *fp = fopen(AOF_ARRAY_FILE, "w");
    if (fp) fclose(fp);
    fp = fopen(AOF_RBTREE_FILE, "w");
    if (fp) fclose(fp);
    fp = fopen(AOF_HASH_FILE, "w");
    if (fp) fclose(fp);
    fp = fopen(AOF_SKIPTABLE_FILE, "w");
    if (fp) fclose(fp);
    printf("SYNC completed for fd %d\n", fd);
}

// ============ 从节点：发起 SYNC ============

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
    send(fd, "SYNC", 4, 0);
    
    // 接收响应
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
    
    printf("Receiving files...\n");

#if ENABLE_NOBLOCK
// 接收所有文件
    int file_count = 0;
    while (1) {
        int ret = recv_file_from_master(fd, "data");
        if (ret == 0) {
            file_count++;
            printf("  Received file #%d\n", file_count);
            continue;
        } else if (ret == -2) {
            // 数据还没到，继续等待
            usleep(10000);
            continue;
        }
        
        // ret == -1，检查是否是 END
        char end_buf[16];
        n = recv(fd, end_buf, sizeof(end_buf)-1, 0);
        if (n > 0) {
            end_buf[n] = '\0';
            if (strcmp(end_buf, "END\n") == 0) {
                printf("Received END marker (total %d files)\n", file_count);
                break;
            }
        }
        break;
    }
#else
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
#endif
    close(fd);
    
    // 重新加载数据
    printf("Reloading snap_data from master...\n");
    load_snapshots();
    
    printf("Sync completed\n");
    return 0;
}