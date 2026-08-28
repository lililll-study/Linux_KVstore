#include "kvstore.h"

static int sync_fd = -1;

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
    send(fd, header, strlen(header), 0);

    // 发送文件内容
    FILE *fp = fopen(filepath, "rb");
    if (!fp) return -1;

    char buf[SYNC_BUFFER_SIZE];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        send(fd, buf, n, 0);
    }
    fclose(fp);
    return 0;

}

// 主节点
int send_all_files(int fd) {
    // 先保存快照
    printf("Saving snapshots...\n");
    save_snapshots();

    const char *files[] = {
        SNAP_ARRAY_FILE,
        SNAP_RBTREE_FILE,
        SNAP_HASH_FILE,
        SNAP_SKIPTABLE_FILE,
        NULL
    };
    // 发送文件
    for (int i = 0; files[i]; i++) {
        if (access(files[i], R_OK) == 0) {
            send_file(fd, files[i]);
        }
    }
    
    // 发送结束标记
    send(fd, "END\n", 4, 0);
    printf("All files sent\n");
    return 0;
}

// 启动同步服务
int start_sync_server(void) {
    
}