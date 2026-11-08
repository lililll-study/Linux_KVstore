#include "kvstore.h"
#include "kvs_uring.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

extern kvs_hash_t   global_hash;
extern kvs_skiptable_t global_skiptable;


//================================
/*aof and snap 基础框架定义*/
//================================

// snap快照统一调度接口
typedef int (*snapshot_save_fn)(const char *filename);


int save_snapshot_hash(const char *filename) ;
int save_snapshot_skiptable(const char *filename) ;

// 使用结构体+函数指针解决不同引擎独立保存和加载复杂的问题
typedef struct engine_persist_s {
    const char *name;
    snapshot_save_fn save_fn;
} engine_persist_t;



// 引擎配置，和函数接口相对应
static engine_persist_t engine_persist_table[] = {
    {"hash",        save_snapshot_hash},
    {"skiptable",   save_snapshot_skiptable},
    {NULL, NULL}
};

//================================
/* mmap持久化数据加载 */
//================================
// mmap文件结构
typedef struct {
    int fd;     // 文件描述符
    size_t size; // 保存文件的大小
    char *data; // mmap返回的文件首地址
} mmap_file_t;


// 对内提供5个API接口
#if ENABLE_MMAP
// 创建 aof文件到pagecache的映射，返回一个虚拟地址mf->data（指针）
static int mmap_file_open(const char *filename, mmap_file_t *mf)
{
    struct stat st;

    memset(mf, 0, sizeof(*mf));
    mf->fd = -1;

    mf->fd = open(filename, O_RDONLY | O_CLOEXEC);
    if (mf->fd < 0)
        return -errno;

    if (fstat(mf->fd, &st) < 0) {
        int ret = -errno;
        close(mf->fd);
        mf->fd = -1;
        return ret;
    }

    mf->size = (size_t)st.st_size;

    if (mf->size == 0)
        return 0;
    // 把文件映射到进程的虚拟地址空间，返回
    // 数据加载，采用只读模式
    mf->data = mmap(NULL, mf->size, PROT_READ, MAP_PRIVATE, mf->fd, 0);
    if (mf->data == MAP_FAILED) {
        int ret = -errno;
        mf->data = NULL;
        close(mf->fd);
        mf->fd = -1;
        return ret;
    }

    return 0;
}
// 解除内存映射
static void mmap_file_close(mmap_file_t *mf)
{
    if (!mf)
        return;

    if (mf->data && mf->size)
        munmap(mf->data, mf->size); // 解除映射，从addr开始，长度len的内存映射

    if (mf->fd >= 0)
        close(mf->fd);

    mf->data = NULL;
    mf->size = 0;
    mf->fd = -1;
}

// 从mmap返回的虚拟地址拿数据，和从数组中取数据相同
// 解析$len\r\nvalue\r\n
static char *mmap_read_token(const char *data, size_t size, size_t *offset)
{
    size_t pos;
    size_t len = 0;
    char *value;
    if (!data || !offset || *offset >= size || data[*offset] != '$') return NULL;

    // 跳过$解析长度值
    pos = *offset + 1;
    while (pos < size && data[pos] >= '0' && data[pos] <= '9') {
        len = len * 10 + (data[pos] - '0');

        if (len > size) return NULL;
        pos++;
    }

    if (pos + 1 >= size || data[pos] != '\r' || data[pos + 1] != '\n') return NULL;
    pos += 2;  // 跳过\r\n
    // 此时应该到达value位置，value大小为len，size-len为value长度
    if (len > size - pos) return NULL;
    value = malloc(len + 1);
    if (!value) return NULL;
    memcpy(value, data + pos, len);
    value[len] = '\0';
    pos += len;

    if (pos + 1 >= size || data[pos] != '\r' || data[pos + 1] != '\n') {
        free(value);
        return NULL;
    }

    *offset = pos + 2; // 跳过\r\n
    return value;
}

/**
 * mmap_read_line：从 mmap 区域读取一行文本。
 * 主要用于读取 Snapshot 中的 # array、# rbtree、# hash、# skiptable 标记。
 */
static int mmap_read_line(const char *data, size_t size,
                          size_t *offset, char *line, size_t line_size)
{
    size_t len = 0;

    while (*offset < size &&
           data[*offset] != '\r' &&
           data[*offset] != '\n') {

        if (len + 1 < line_size)
            line[len++] = data[*offset];

        (*offset)++;
    }

    line[len] = '\0';

    while (*offset < size &&
           (data[*offset] == '\r' || data[*offset] == '\n')) {
        (*offset)++;
    }

    return 0;
}

static int mmap_parse_resp_command(const char *data, size_t size,
                                   size_t *offset,
                                   char **tokens, int *token_count) {
    size_t start = *offset;
    size_t p = *offset;
    char line[256];

    /* 辅助宏：从 p 读一行（到 \r 或 \n），跳过行尾 \r\n */
#define READ_LINE(dst, dst_size) do {                                   \
        size_t li = 0;                                                  \
        while (p < size && data[p] != '\r' && data[p] != '\n') {        \
            if (li + 1 < (dst_size)) dst[li++] = data[p];               \
            p++;                                                        \
        }                                                               \
        if (p >= size) return 0;      /* 数据不够 */                    \
        dst[li] = '\0';                                                 \
        if (p + 1 < size && data[p] == '\r' && data[p+1] == '\n') {     \
            p += 2;                                                     \
        } else if (data[p] == '\n') {                                   \
            p += 1;                                                     \
        } else {                                                        \
            return -1;                                                  \
        }                                                               \
    } while (0)

    /* 扁平 RESP：*M\r\n 数组本身即一条命令（AOF 记录格式） */
    if (p >= size) return 0;
    if (data[p] != '*') return -1;
    READ_LINE(line, sizeof(line));
    int inner = atoi(line + 1);
    if (inner <= 0 || inner > KVS_MAX_TOKENS) return -1;

    /* 3. 读 M 个 token */
    int i;
    for (i = 0; i < inner; i++) {
        if (p >= size) goto not_enough;
        if (data[p] != '$') goto format_err;
        READ_LINE(line, sizeof(line));
        int len = atoi(line + 1);
        if (len < 0) goto format_err;

        /* 剩余够不够 len + 2（\r\n） */
        if (p + (size_t)len + 2 > size) goto not_enough;

        char *tok = malloc((size_t)len + 1);
        if (!tok) goto format_err;
        memcpy(tok, data + p, (size_t)len);
        tok[len] = '\0';
        p += (size_t)len;

        if (data[p] != '\r' || data[p+1] != '\n') {
            free(tok);
            goto format_err;
        }
        p += 2;

        tokens[i] = tok;
    }

    *token_count = inner;
    *offset = p;
    return (int)(p - start);

not_enough:
    for (i = 0; i < KVS_MAX_TOKENS; i++) {
        if (tokens[i]) { free(tokens[i]); tokens[i] = NULL; }
    }
    return 0;

format_err:
    for (i = 0; i < KVS_MAX_TOKENS; i++) {
        if (tokens[i]) { free(tokens[i]); tokens[i] = NULL; }
    }
    return -1;

#undef READ_LINE
}

#endif



/* ===== 快照格式化的内存缓冲 =====
 * 异步保存时先把整份快照拼进内存，再一次性交给 io_uring 写盘，
 * 这样遍历内存表和磁盘 IO 不会交错进行 */
typedef struct {
    char   *data;
    size_t  len;
    size_t  cap;
} snap_buf_t;

/* 确保缓冲剩余空间够 need 字节，不够就按 2 倍扩容 */
static int snap_buf_reserve(snap_buf_t *b, size_t need)
{
    if (b->len + need <= b->cap) return 0;

    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->len + need) cap <<= 1;

    char *p = (char *)realloc(b->data, cap);
    if (!p) return -1;

    b->data = p;
    b->cap = cap;
    return 0;
}

/* 追加一行原始文本（段落标记用，如 "# hash\n"） */
static int snap_buf_line(snap_buf_t *b, const char *line)
{
    size_t len = strlen(line);
    if (snap_buf_reserve(b, len) < 0) return -1;

    memcpy(b->data + b->len, line, len);
    b->len += len;
    return 0;
}

/* 追加一个 token：$len\r\ndata\r\n（带显式长度，供锁外格式化打包副本用） */
static int snap_buf_token_n(snap_buf_t *b, const char *data, size_t len)
{
    if (!data) return -1;

    /* 预留够：$ + 长度数字 + \r\n + 内容 + \r\n */
    if (snap_buf_reserve(b, len + 32) < 0) return -1;

    int n = snprintf(b->data + b->len, b->cap - b->len, "$%zu\r\n", len);
    if (n < 0) return -1;
    b->len += (size_t)n;

    memcpy(b->data + b->len, data, len);
    b->len += len;
    b->data[b->len++] = '\r';
    b->data[b->len++] = '\n';
    return 0;
}

/* ===== 锁内打包：临界区里只做 memcpy，格式化/CRC 全部推迟到锁外 =====
 * 打包格式：条目的 key/value 以"原文 + 结尾 NUL"紧凑排列，
 * 锁外靠 NUL 顺序解析，天然自描述，无需长度头 */

/* 锁内拷贝一个字符串（含结尾 NUL），无任何格式化 */
static int snap_pack_str(snap_buf_t *b, const char *s)
{
    if (!s) return -1;

    size_t len = strlen(s) + 1;             /* 连结尾 NUL 一起拷 */
    if (snap_buf_reserve(b, len) < 0) return -1;

    memcpy(b->data + b->len, s, len);
    b->len += len;
    return 0;
}


/* CRC32 值格式化为 8 位十六进制字符串（方便存入SNAP/AOF文件中） */
static const char *crc32_to_hex(uint32_t crc) {
    static char hexbuf[9];
    snprintf(hexbuf, sizeof(hexbuf), "%08x", (unsigned)crc);
    return hexbuf;
}

/* 锁内遍历 hash 表，把条目原文拷进打包缓冲（不格式化、不算 CRC）
 * 调用方需持 g_data_lock 读锁 */
static int snap_pack_hash(snap_buf_t *b) {
    int i = 0;
    for (i = 0; i < global_hash.max_slots; i++) {
        hashnode_t *node = global_hash.nodes[i];
        while (node != NULL) {
            if (snap_pack_str(b, node->key) < 0 ||
                snap_pack_str(b, node->value) < 0) {
                return -1;
            }
            node = node->next;
        }
    }

    /* 渐进 rehash 期间：新表中的节点也要落盘 */
    for (i = 0; global_hash.rehash_nodes && i < global_hash.rehash_slots; i++) {
        hashnode_t *node = global_hash.rehash_nodes[i];
        while (node != NULL) {
            if (snap_pack_str(b, node->key) < 0 ||
                snap_pack_str(b, node->value) < 0) {
                return -1;
            }
            node = node->next;
        }
    }
    return 0;
}

/* 锁外：把打包缓冲（NUL 分隔的条目原文流）解析并格式化成 token 文本
 * [p, end) 为一个引擎段的打包数据；CRC 在这里算（读的是锁内拷出的副本） */
static int snap_fmt_packed(snap_buf_t *b, const char *p, const char *end)
{
    while (p < end) {
        const char *key = p;
        p += strlen(p) + 1;                     /* 跳过 key 原文 */
        if (p >= end) return -1;                /* 缺 value，数据损坏 */
        const char *val = p;
        p += strlen(p) + 1;                     /* 跳过 value 原文 */
        if (p > end) return -1;

        if (snap_buf_token_n(b, key, strlen(key)) < 0 ||
            snap_buf_token_n(b, val, strlen(val)) < 0 ||
            snap_buf_token_n(b, crc32_to_hex(crc32_calc_snap(key, val)), 8) < 0) {
            return -1;
        }
    }
    return 0;
}

/* 把 hash 全量格式化进内存缓冲（save_snapshot_* 单线程路径用）
 * 内部同样走"先打包再格式化"，遍历逻辑只保留一份 */
static int snap_format_hash(snap_buf_t *b) {
    snap_buf_t packed = {0};
    if (snap_pack_hash(&packed) != 0) {
        free(packed.data);
        return -1;
    }

    int ret = (snap_buf_line(b, "# hash\n") != 0) ? -1 :
              snap_fmt_packed(b, packed.data, packed.data + packed.len);
    free(packed.data);
    return ret;
}

/* 底层函数 */
int save_snapshot_hash(const char *filename) {
    snap_buf_t b = {0};
    if (snap_format_hash(&b) != 0) {
        free(b.data);
        return -1;
    }

    FILE *fp = fopen(filename, "a");
    if (!fp) {
        free(b.data);
        return -1;
    }
    if (b.len > 0) fwrite(b.data, 1, b.len, fp);
    fclose(fp);
    free(b.data);

    LOG_INFO("[PERSIST] Snapshot hash saved to %s\n", filename);
    return 0;
}

/* 锁内遍历 skiptable，条目原文拷进打包缓冲
 * 调用方需持 g_data_lock 读锁 */
static int snap_pack_skiptable(snap_buf_t *b) {
    kvs_node_t *curr = global_skiptable.header->forward[0];
    while (curr != NULL) {
        if (snap_pack_str(b, curr->key) < 0 ||
            snap_pack_str(b, curr->value) < 0) {
            return -1;
        }
        curr = curr->forward[0];
    }
    return 0;
}

/* 把 skiptable 全量格式化进内存缓冲（save_snapshot_* 单线程路径用） */
static int snap_format_skiptable(snap_buf_t *b) {
    snap_buf_t packed = {0};
    if (snap_pack_skiptable(&packed) != 0) {
        free(packed.data);
        return -1;
    }

    int ret = (snap_buf_line(b, "# skiptable\n") != 0) ? -1 :
              snap_fmt_packed(b, packed.data, packed.data + packed.len);
    free(packed.data);
    return ret;
}

int save_snapshot_skiptable(const char *filename) {
    snap_buf_t b = {0};
    if (snap_format_skiptable(&b) != 0) {
        free(b.data);
        return -1;
    }

    FILE *fp = fopen(filename, "a");
    if (!fp) {
        free(b.data);
        return -1;
    }
    if (b.len > 0) fwrite(b.data, 1, b.len, fp);
    fclose(fp);
    free(b.data);

    LOG_INFO("[PERSIST] Snapshot skiptable saved to %s\n", filename);
    return 0;
}




/* io_uring 功能封装*/

// aof文件的fd，进程启动打开一次，且常开
static int g_aof_fd = -1;
// uring封装实例，管理写入队列和worker线程
static kvs_uring_t *g_aof_uring = NULL;

#if ENABLE_CRC
// AOF 命令区全文 CRC 累计中间值（初值 0xFFFFFFFF），以及命令区末尾偏移
static uint32_t g_aof_full_crc = 0xFFFFFFFFu;
static off_t    g_aof_cmd_end = 0;

#endif
// 1. 构建写入aof文件的resp数据
static int build_aof_record(const char *cmd, const char *key, const char *value, char **out_buf, size_t *out_len) {
    if (!cmd || !key || !out_buf || !out_len)
        return -EINVAL;

    int need;
    // 如果没有value表示是删除del等cmd，有value表示是set等
    if (value) {
        // 标准扁平 RESP：*3\r\n$LEN(cmd)\r\nCMD\r\n$LEN(key)\r\nKEY\r\n$LEN(value)\r\nVALUE\r\n
        need = snprintf(NULL, 0,
                        "*3\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                        strlen(cmd), cmd,
                        strlen(key), key,
                        strlen(value), value);
    } else {
        // 标准扁平 RESP：*2\r\n$LEN(cmd)\r\nCMD\r\n$LEN(key)\r\nKEY\r\n
        need = snprintf(NULL, 0,
                        "*2\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                        strlen(cmd), cmd,
                        strlen(key), key);
    }

    if (need < 0)
        return -EINVAL;
    // 给返回数据分配内存，且末尾保留\0：因为snprintf总是再末尾加入\0，如果不写，会被截断
    char *buf = (char *)malloc((size_t)need + 1);
    if (!buf)
        return -ENOMEM;
    // 按照标准扁平 RESP 协议格式进行拼接
    if (value) {
        snprintf(buf, (size_t)need + 1,
                "*3\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                strlen(cmd), cmd,
                strlen(key), key,
                strlen(value), value);
    } else {
        snprintf(buf, (size_t)need + 1,
                "*2\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                strlen(cmd), cmd,
                strlen(key), key);
    }

    *out_buf = buf;
    *out_len = (size_t)need;
    return 0;
}


#if ENABLE_MMAP && ENABLE_CRC
/* 扫描已有 AOF 文件：恢复命令区末尾偏移与全文 CRC 累计值（截掉旧尾部） */
static void aof_scan_existing(void)
{
    g_aof_full_crc = 0xFFFFFFFFu;
    g_aof_cmd_end = 0;

    mmap_file_t mf;
    if (mmap_file_open(AOF_FILE, &mf) < 0)
        return;
    if (mf.size == 0) {
        mmap_file_close(&mf);
        return;
    }

    size_t offset = 0;
    while (offset < mf.size) {
        /* 跳过空行 */
        while (offset < mf.size &&
               (mf.data[offset] == '\r' || mf.data[offset] == '\n'))
            offset++;
        if (offset >= mf.size) break;

        /* 读到 # 标志位：命令区结束 */
        if (mf.data[offset] == '#') break;

        size_t start = offset;
        char *tokens[KVS_MAX_TOKENS] = {0};
        int token_count = 0;
        int consumed = mmap_parse_resp_command(mf.data, mf.size, &offset,
                                               tokens, &token_count);
        int i;
        for (i = 0; i < token_count; i++) free(tokens[i]);
        if (consumed <= 0) break;   /* 半条/损坏，命令区到此为止 */

        g_aof_full_crc = crc32_update(g_aof_full_crc,
                                      mf.data + start, (size_t)consumed);
        g_aof_cmd_end = (off_t)offset;
    }
    mmap_file_close(&mf);
}
#endif

// 2. 进程启动时调用一次，初始化aof的uring环境
// 调用方：init_kvsengine时
int persistence_init(void)
{
    if (!g_config.aof_enable) return 0;

    if (g_aof_uring) return 0;
    // 打开aof文件获得fd    读写模式    不存在则创建 exec 时自动关闭
    g_aof_fd = open(AOF_FILE, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (g_aof_fd < 0) {
        LOG_ERROR("[AOF] open %s failed: %s\n", AOF_FILE, strerror(errno));
        return -errno;
    }

    struct stat st;
    // 获取文件大小，作为首次写入offset
    if (fstat(g_aof_fd, &st) < 0) {
        int err = errno;
        close(g_aof_fd);
        g_aof_fd = -1;
        return -err;
    }
    long base_offset = st.st_size; // 将当前文件大小作为基础偏移
#if ENABLE_MMAP && ENABLE_CRC
    /* 如果config中开启了crc，那么扫描已有命令区，计算全文CRC和命令区的末尾偏移 */
    if (g_config.aof_crc_enable) {
        aof_scan_existing();
    } else if (base_offset > 0) {
        /* crc 关闭：截掉上次开启时遗留的 CRC 尾，保持文件为纯命令区 */
        aof_scan_existing();
        if (g_aof_cmd_end < base_offset) {
            if (ftruncate(g_aof_fd, g_aof_cmd_end) == 0) {
                base_offset = g_aof_cmd_end;
            }
        }
        g_aof_full_crc = 0xFFFFFFFFu;
    }
    
#endif

    // 初始化uring子系统，失败就清理fd
    int ret = kvs_uring_init(&g_aof_uring, g_aof_fd, base_offset);
    if (ret < 0) {
        LOG_ERROR("[AOF] io_uring init failed: %s\n",
               strerror(-ret));
        close(g_aof_fd);
        g_aof_fd = -1;
        return ret;
    }

    LOG_INFO("[AOF] io_uring writer started, offset=%lld\n", (long long)st.st_size);

    return 0;
}

/**
 * 3. aof的异步写入
 * 业务层调用，异步写aof
 * 构造 AOF 字节串 → 入队到 io_uring → 立刻返回（不阻塞）
 */
int append_aof(const char *cmd, const char *key, const char *value)
{
    if (!g_config.aof_enable) return 0;
    if (!g_aof_uring) return -ESHUTDOWN;    //返回-108：未初始化

    char *record = NULL;
    size_t record_len = 0;

    // 把cmd拼接为aof格式
    int ret = build_aof_record(cmd, key, value, &record, &record_len);
    if (ret < 0) return ret;

#if ENABLE_CRC
    if (g_config.aof_crc_enable) {
        /* 增量累计全文 CRC，构造文件尾校验值，一次性截断并重写文件尾 */
        g_aof_full_crc = crc32_update(g_aof_full_crc, record, record_len);
        uint32_t final_crc = g_aof_full_crc ^ 0xFFFFFFFFu;
        char tail[AOF_CRC_TAIL_LEN + 1];
        snprintf(tail, sizeof(tail), "# AOF_CRC:%08x\r\n", (unsigned)final_crc);
        ret = kvs_uring_append_cmd(g_aof_uring, record, record_len,
                                   tail, AOF_CRC_TAIL_LEN, g_aof_cmd_end);
        g_aof_cmd_end += (off_t)record_len;
        free(record);   // 释放
        return ret;
    }
#endif

    ret = kvs_uring_append(g_aof_uring, record, record_len);
    free(record);   // 释放

    return ret;
}

// 等所有已经入队的 AOF 写请求完成。
// 返回0：AOF 未启用，没有请求要等；io_uring 未初始化，没有队列
// 上级调用：SAVE命令中
int persistence_flush(void)
{
    if (!g_config.aof_enable || !g_aof_uring) return 0;
    return kvs_uring_flush(g_aof_uring);
}
// 清空aof
// 清空文件+重置uring的offset
// 上级调用：SAVE 命令生成快照后
int persistence_reset_aof(void)
{
    if (!g_config.aof_enable || !g_aof_uring) return 0;
    int ret = kvs_uring_reset(g_aof_uring);
#if ENABLE_CRC
    g_aof_full_crc = 0xFFFFFFFFu;
    g_aof_cmd_end = 0;
#endif
    return ret;
}

// 退出清理：进程退出时清理 AOF 子系统。
void persistence_shutdown(void)
{   
    persist_snap_stop();          /* 先关 SNAP 落盘通道并写完最后一批 */

    // 先关闭uring
    if (g_aof_uring) {
        kvs_uring_shutdown(g_aof_uring);
        g_aof_uring = NULL;
    }
    // 再关闭fd
    if (g_aof_fd >= 0) {
        close(g_aof_fd);
        g_aof_fd = -1;
    }
}


/* =============================================================
 * SNAP 异步保存通道：由 SAVE 命令触发，fork 子进程落盘（COW 快照）
 * ============================================================= */

static pthread_t      g_snap_tid;               /* 保存线程：fork/等子进程全在这 */
static volatile int   g_snap_run = 0;           /* 保存线程运行标志 */
static volatile int   g_snap_saving = 0;        /* 1=有一轮保存未完成（防重入） */
static pthread_mutex_t g_snap_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_snap_cond = PTHREAD_COND_INITIALIZER;

/* 全局数据锁：写命令持写锁，读命令持读锁 */
pthread_rwlock_t g_data_lock = PTHREAD_RWLOCK_INITIALIZER;

/* ===== 子进程侧：流式写盘（静态缓冲，全程禁 malloc/LOG/io_uring） =====
 * fork 出的子进程持有 fork 瞬间的 COW 冻结镜像：
 *   - 读 global_hash/global_skiptable 不用加锁（子进程单线程，内存不再变化）
 *   - 禁 malloc/realloc：fork 时父进程其他线程可能正持 malloc 内部锁 → 子进程死锁
 *   - 禁 LOG/stdio：同理，glibc 内部锁
 *   - 禁 io_uring：ring 与父进程共享，子进程提交会互相踩
 *   - 落盘用裸 write()，出错不 LOG，只置错误标志，由父进程统一报告 */

#define SNAP_CHILD_BUF_SIZE (256 * 1024)

static char   g_snap_cbuf[SNAP_CHILD_BUF_SIZE]; /* 子进程写盘缓冲（静态内存） */
static size_t g_snap_clen = 0;                  /* 已占字节数 */
static int    g_snap_cfd = -1;
static int    g_snap_cerr = 0;                  /* 出错标志 */

/* 把缓冲内数据全部写入文件（循环写，处理部分写入） */
static void snap_child_flush(void)
{
    size_t off = 0;
    while (off < g_snap_clen) {
        ssize_t n = write(g_snap_cfd, g_snap_cbuf + off, g_snap_clen - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            g_snap_cerr = 1;
            return;
        }
        off += (size_t)n;
    }
    g_snap_clen = 0;
}

/* 追加字节流，缓冲满先落盘 */
static void snap_child_write(const char *data, size_t len)
{
    if (g_snap_cerr) return;
    if (len > sizeof(g_snap_cbuf)) {            /* 单条超过整缓冲：防御 */
        g_snap_cerr = 1;
        return;
    }
    if (g_snap_clen + len > sizeof(g_snap_cbuf)) {
        snap_child_flush();
        if (g_snap_cerr) return;
    }
    memcpy(g_snap_cbuf + g_snap_clen, data, len);
    g_snap_clen += len;
}

/* 输出一个 token：$len\r\ndata\r\n（格式与 snap_buf_token_n 完全一致） */
static void snap_child_token(const char *s)
{
    if (g_snap_cerr) return;
    char head[32];
    int n = snprintf(head, sizeof(head), "$%zu\r\n", strlen(s));
    if (n < 0) {
        g_snap_cerr = 1;
        return;
    }
    snap_child_write(head, (size_t)n);
    snap_child_write(s, strlen(s));
    snap_child_write("\r\n", 2);
}

/* 子进程遍历 hash 表并直接格式化输出（含渐进 rehash 表） */
static void snap_child_save_hash(void)
{
    int i = 0;
    for (i = 0; i < global_hash.max_slots; i++) {
        hashnode_t *node = global_hash.nodes[i];
        while (node != NULL) {
            snap_child_token(node->key);
            snap_child_token(node->value);
            snap_child_token(crc32_to_hex(crc32_calc_snap(node->key, node->value)));
            node = node->next;
        }
    }

    for (i = 0; global_hash.rehash_nodes && i < global_hash.rehash_slots; i++) {
        hashnode_t *node = global_hash.rehash_nodes[i];
        while (node != NULL) {
            snap_child_token(node->key);
            snap_child_token(node->value);
            snap_child_token(crc32_to_hex(crc32_calc_snap(node->key, node->value)));
            node = node->next;
        }
    }
}

/* 子进程遍历 skiptable 并直接格式化输出 */
static void snap_child_save_skiptable(void)
{
    kvs_node_t *curr = global_skiptable.header->forward[0];
    while (curr != NULL) {
        snap_child_token(curr->key);
        snap_child_token(curr->value);
        snap_child_token(crc32_to_hex(crc32_calc_snap(curr->key, curr->value)));
        curr = curr->forward[0];
    }
}

/* 子进程入口：冻结镜像 → 流式写 SNAP 文件 → _exit 带回退出码 */
static void snap_child_save(void)
{
    int i;

    /* 关闭从父进程继承的所有 fd（socket/epoll 等子进程用不到）：
     * 不关的话，子进程会拖着这些 socket 的引用计数，
     * 父进程关闭连接后 epoll 注册无法自动摘除，会残留出 recv EBADF 之类的怪问题。
     * 0/1/2 保留，3 以上全部关闭；子进程随后自己 open 快照文件 */
    for (i = 3; i < 1024; i++) {
        close(i);
    }

    /* 子进程不响应退出信号：父进程收 Ctrl+C 时不能把写一半的保存一起带走 */
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);

    g_snap_cfd = open(SNAP_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (g_snap_cfd < 0) _exit(1);

    snap_child_write("# hash\n", strlen("# hash\n"));
    snap_child_save_hash();
    snap_child_write("# skiptable\n", strlen("# skiptable\n"));
    snap_child_save_skiptable();

    if (!g_snap_cerr) snap_child_flush();
    if (!g_snap_cerr && fsync(g_snap_cfd) < 0) g_snap_cerr = 1;

    close(g_snap_cfd);
    _exit(g_snap_cerr ? 1 : 0);
}

/* 完整一轮保存：fork 子进程在 COW 冻结镜像上打包落盘
 * 命令线程全程不受读写锁影响（只有 fork 瞬间毫秒级页表复制），
 * 期间新 SAVE 请求按防重入语义跳过（后一轮快照天然更新） */
static int snap_do_save(void)
{
    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERROR("[SNAP] fork failed: %s\n", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        snap_child_save();      /* 子进程：内部 _exit，不会返回 */
    }

    /* 保存线程阻塞等子进程结束（后台线程，不阻塞命令线程） */
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        LOG_ERROR("[SNAP] waitpid failed: %s\n", strerror(errno));
        return -1;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        LOG_DEBUG("[SNAP] fork 保存完成\n");
        return 0;
    }
    LOG_ERROR("[SNAP] 子进程保存失败 status=%d\n", status);
    return -1;
}

/* 保存线程：SAVE 命令只负责唤醒，拷贝/拼接/落盘全部在这里执行
 * 命令线程因此完全不被保存工作占用 */
static void *snap_save_worker(void *arg)
{
    (void)arg;

    while (g_snap_run) {
        /* 等 SAVE 命令唤醒（g_snap_saving 已由命令线程置位）
         * while 循环防虚假唤醒，同时兼顾退出标志 */
        pthread_mutex_lock(&g_snap_mutex);
        while (g_snap_run && !g_snap_saving) {
            pthread_cond_wait(&g_snap_cond, &g_snap_mutex);
        }
        pthread_mutex_unlock(&g_snap_mutex);
        if (!g_snap_run) break;

        snap_do_save();     /* 完整一轮：锁内 pack → 锁外 fmt → uring 落盘 */

        pthread_mutex_lock(&g_snap_mutex);
        g_snap_saving = 0;  /* 本轮结束，放行下一次 SAVE 请求 */
        pthread_mutex_unlock(&g_snap_mutex);
    }
    return NULL;
}

/**
 * @name 初始化 SNAP 落盘通道（启动保存线程）
 * 主机/从机：snap 模式下 init 阶段调用；
 * 从机全量同步（.tmp 加载）后需 stop→start 重建。
 * 每轮保存 fork 子进程执行（子进程自己 open 快照文件），SAVE 命令只发信号唤醒。
 */
int persist_snap_start(void)
{
    if (!g_config.snap_enable) return 0;   /* 非 SNAP 模式不启用 */
    if (g_snap_run) return 0;              /* 已经启动过 */

    /* 保存线程：fork/等待子进程全在后台 */
    g_snap_run = 1;
    if (pthread_create(&g_snap_tid, NULL, snap_save_worker, NULL) != 0) {
        LOG_ERROR("[SNAP] create save thread failed\n");
        g_snap_run = 0;
        return -1;
    }

    LOG_INFO("[SNAP] 落盘通道已就绪（fork 子进程执行保存，SAVE 仅唤醒）\n");
    return 0;
}

/* 关闭 SNAP 落盘通道：停保存线程（等它做完手头这轮，含子进程落盘）
 * 从机全量同步 rename 前必须调用：否则可能有子进程还在写 SNAP 文件 */
void persist_snap_stop(void)
{
    if (g_snap_run) {
        g_snap_run = 0;
        pthread_mutex_lock(&g_snap_mutex);
        pthread_cond_signal(&g_snap_cond);  /* 唤醒线程使其看到退出标志 */
        pthread_mutex_unlock(&g_snap_mutex);
        /* join：若此刻线程正在保存，会等 fork 子进程完整写完 */
        pthread_join(g_snap_tid, NULL);
    }
}

/* SAVE 命令入口：只占任务槽 + 唤醒保存线程，立即返回
 * 返回 0 = 已受理（后台保存中），不保证此刻已落盘
 * 后台忙（上一轮未完）则跳过本轮——后一轮快照天然更新，无损失 */
int persist_snap_request(void)
{
    if (!g_config.snap_enable) return 0;
    if (!g_snap_run) return -1;

    pthread_mutex_lock(&g_snap_mutex);
    if (g_snap_saving) {
        pthread_mutex_unlock(&g_snap_mutex);
        LOG_DEBUG("[SNAP] 上一次保存尚未结束，跳过本次请求\n");
        return 0;
    }
    g_snap_saving = 1;                      /* 占住任务槽 */
    pthread_cond_signal(&g_snap_cond);      /* 唤醒保存线程 */
    pthread_mutex_unlock(&g_snap_mutex);
    return 0;
}


long replay_aof_from(const char *filename, long start_offset) {
    mmap_file_t mf;
    size_t offset = (size_t)start_offset;
    int count = 0;
    long ret = start_offset;
    uint32_t crc = 0xFFFFFFFFu;
    /* 仅全量重放（从 0 开始）且 aof.crc 开启时对文件尾全文 CRC 做校验 */
    int verify = (start_offset == 0) && g_config.aof_crc_enable;
    int tail_found = 0;

    int open_ret = mmap_file_open(filename, &mf);
    if (open_ret < 0) {
        if (open_ret == -ENOENT) {
            /* 首次运行：AOF 尚未创建，无需重放 */
            LOG_INFO("[AOF] no aof file yet, nothing to replay: %s\n", filename);
            return 0;
        }
        LOG_ERROR("[AOF] mmap_file_open failed: %s (errno=%d: %s)\n",
                  filename, -open_ret, strerror(-open_ret));
        return -1;
    }

    if (mf.size == 0) {
        mmap_file_close(&mf);
        return start_offset;
    }

    if (offset > mf.size) {
        mmap_file_close(&mf);
        return -1;
    }

    while (offset < mf.size) {
        /* 跳过空行 / 空白 */
        while (offset < mf.size &&
               (mf.data[offset] == '\r' || mf.data[offset] == '\n' ||
                mf.data[offset] == ' ' || mf.data[offset] == '\t'))
            offset++;

        if (offset >= mf.size)
            break;

        /* 标志位：# AOF_CRC: —— 文件尾 CRC，解析最后 8 位 hex 校验值 */
        if (mf.data[offset] == '#') {
            uint32_t stored = 0;
            int ok = 0;

            if (mf.size - offset >= (size_t)AOF_CRC_TAIL_LEN &&
                strncmp(mf.data + offset, AOF_CRC_PREFIX,
                        AOF_CRC_PREFIX_LEN) == 0) {
                char hex[AOF_CRC_HEX_LEN + 1];
                memcpy(hex, mf.data + offset + AOF_CRC_PREFIX_LEN,
                       AOF_CRC_HEX_LEN);
                hex[AOF_CRC_HEX_LEN] = '\0';
                stored = (uint32_t)strtoul(hex, NULL, 16);
                ok = 1;
            }
            if (ok) tail_found = 1;
            if (verify) {
                if (!ok || stored != (crc ^ 0xFFFFFFFFu)) {
                    LOG_ERROR("[AOF] CRC MISMATCH: stored=%08x, calc=%08x\n",
                              ok ? stored : 0, crc ^ 0xFFFFFFFFu);
                    ret = -1;
                    break;
                }
                LOG_INFO("[AOF] CRC OK: %08x, %d entries\n",
                         crc ^ 0xFFFFFFFFu, count);
            } else {
                /* 增量重放：跳过该尾部行，继续解析后续命令 */
                char line[64];
                mmap_read_line(mf.data, mf.size, &offset, line, sizeof(line));
                continue;
            }

            ret = (long)offset;
            break;
        }

        char *tokens[KVS_MAX_TOKENS] = {0};
        int token_count = 0;
        size_t before = offset;

        int consumed = mmap_parse_resp_command(mf.data, mf.size, &offset,
                                               tokens, &token_count);
        if (consumed < 0) {
            LOG_ERROR("[AOF] FORMAT ERROR at offset=%zu\n", offset);
            break;
        }
        if (consumed == 0) {
            /* 末尾半条命令，停在这里 */
            break;
        }

        /* 累计全文 CRC：该命令完整 RESP 字节 */
        crc = crc32_update(crc, mf.data + before, (size_t)consumed);

        /* 执行命令（fd=-1：不写 AOF、不回响应） */
        kvs_filter_protocol(-1, tokens, token_count, NULL);

        int i;
        for (i = 0; i < token_count; i++) free(tokens[i]);
        count++;
        ret = (long)offset;
    }

    mmap_file_close(&mf);

    if (verify && ret >= 0 && !tail_found) {
        LOG_WARN("[AOF] CRC tail missing (file written with aof.crc=false?), skip verification\n");
    }
    
    LOG_INFO("Replayed %d entries, offset %ld -> %ld\n",
           count, start_offset, ret);
    return ret;
}

/**
 * replay_aof：兼容旧接口，从 0 开始 replay。
 */
int replay_aof(const char *filename) {
    long ret = replay_aof_from(filename, 0);
    return (ret < 0) ? -1 : 0;
}

int save_snapshots() {
    // 先清空文件
    FILE *fp = fopen(SNAP_FILE, "w");
    if (!fp) return -1;
    fclose(fp);

    // 依次调用各引擎的保存函数
    int ret = 0, i = 0;
    for (i = 0; engine_persist_table[i].name != NULL; i++) {
        if (engine_persist_table[i].save_fn(SNAP_FILE) != 0) {
            LOG_ERROR("failed to save %s snap\n", engine_persist_table[i].name);
            ret = -1;
        }
    }

    return ret;
}

/* 把内存数据全量保存到指定文件（save_snapshots 的参数化版本）
 * 复用引擎持久化表，格式与快照完全一致（含每条记录的内联 CRC） */
int save_snapshots_to(const char *filename)
{
    // 先清空文件
    FILE *fp = fopen(filename, "w");
    if (!fp) return -1;
    fclose(fp);

    // 依次调用各引擎的保存函数（hash / skiptable 追加写同一文件）
    int ret = 0, i = 0;
    for (i = 0; engine_persist_table[i].name != NULL; i++) {
        if (engine_persist_table[i].save_fn(filename) != 0) {
            LOG_ERROR("failed to save %s snap to %s\n",
                      engine_persist_table[i].name, filename);
            ret = -1;
        }
    }

    return ret;
}

#if ENABLE_MMAP

/**
 * snap_resync_line：解析失败后从 from 起跳到下一个换行之后，
 * 重新对齐记录边界，使后续记录仍可继续解析。
 */
static size_t snap_resync_line(const char *data, size_t size, size_t from)
{
    size_t p = from;
    while (p < size && data[p] != '\n')
        p++;
    if (p < size)
        p++;
    return p;
}

/**
 * load_snapshots：使用 mmap 一次性映射 Snapshot 文件，
 * 根据引擎标记解析 key/value，并恢复到对应 KV 引擎。
 * 单条记录 CRC 不匹配或解析失败时仅跳过该条（记录偏移与 key），
 * 继续加载其余记录，结尾统一汇总。
 */
int load_snapshots()
{
    mmap_file_t mf;
    size_t offset = 0;
    int count = 0;
    int corrupt = 0;
    int current_engine = -1;

    if (mmap_file_open(SNAP_FILE, &mf) < 0) {
        LOG_INFO("No snapshot file: %s\n", SNAP_FILE);
        return -1;
    }

    if (mf.size == 0) {
        mmap_file_close(&mf);
        return 0;
    }

    while (offset < mf.size) {
        while (offset < mf.size &&
               (mf.data[offset] == '\r' ||
                mf.data[offset] == '\n' ||
                mf.data[offset] == ' ' ||
                mf.data[offset] == '\t')) {
            offset++;
        }

        if (offset >= mf.size)
            break;

        /* 读取当前引擎 */
        if (mf.data[offset] == '#') {
            char line[64];

            mmap_read_line(mf.data, mf.size,
                           &offset, line, sizeof(line));

            if (strstr(line, "hash"))
                current_engine = 2;
            else if (strstr(line, "skiptable"))
                current_engine = 3;

            continue;
        }

        /* 读取 key/value/CRC */
        if (mf.data[offset] == '$') {
            size_t rec_start = offset;
            char *key = mmap_read_token(
                mf.data, mf.size, &offset);

            if (!key) {
                LOG_ERROR("[Snap] FORMAT ERROR at offset=%zu: failed to read key, skip record\n",
                          rec_start);
                corrupt++;
                offset = snap_resync_line(mf.data, mf.size, rec_start);
                continue;
            }

            char *value = mmap_read_token(mf.data, mf.size, &offset);

            if (!value) {
                LOG_ERROR("[Snap] FORMAT ERROR at offset=%zu: failed to read value, skip record\n", rec_start);
                free(key);
                corrupt++;
                offset = snap_resync_line(mf.data, mf.size, rec_start);
                continue;
            }

            /* 读取每条 KV 末尾的内联 CRC 校验值 */
            char *hex = mmap_read_token(
                mf.data, mf.size, &offset);

            if (!hex) {
                LOG_ERROR("[Snap] FORMAT ERROR at offset=%zu: failed to read crc, skip record\n", rec_start);
                free(key);
                free(value);
                corrupt++;
                offset = snap_resync_line(mf.data, mf.size, rec_start);
                continue;
            }

            /* 计算并比对 */
            uint32_t calc   = crc32_calc_snap(key, value);
            uint32_t stored = (uint32_t)strtoul(hex, NULL, 16);

            if (stored != calc) {
                LOG_ERROR("[Snap] CRC MISMATCH at offset=%zu key='%s': stored=%08x, calc=%08x, skip record\n",
                          rec_start, key, stored, calc);
                free(key);
                free(value);
                free(hex);
                corrupt++;
                continue;
            }


            switch (current_engine) {
            case 2:
                kvs_hash_set(&global_hash, key, value);
                break;

            case 3:
                kvs_skiptable_set(
                    &global_skiptable, key, value);
                break;

            default:
                LOG_WARN("[Snap] ERROR: unknown engine at offset=%zu, skip record\n", rec_start);
                free(key);
                free(value);
                free(hex);
                corrupt++;
                continue;
            }

            free(key);
            free(value);
            free(hex);
            count++;
        } else {
            offset++;
        }
    }

    mmap_file_close(&mf);

    LOG_INFO("[Snap] Loaded %d entries, skipped %d corrupt entries from %s\n", count, corrupt, SNAP_FILE);

    return 0;
}

#else
int load_snapshots() {
    FILE *fp = fopen(SNAP_FILE, "r");
    if (!fp) {
        printf("No snapshot file: %s\n", SNAP_FILE);
        return -1;
    }
    
    int count = 0;
    int current_engine = -1;
    int ch;
    
    while ((ch = fgetc(fp)) != EOF) {
        // 跳过换行符和空格
        if (ch == '\n' || ch == '\r' || ch == ' ' || ch == '\t') continue;
        
        // ===== 检测引擎标记 =====
        if (ch == '#') {
            char line[64];
            int i = 0;
            while ((ch = fgetc(fp)) != EOF && ch != '\n') {
                if (i < sizeof(line) - 1) line[i++] = ch;
            }
            line[i] = '\0';
            line[strcspn(line, "\r\n")] = '\0';
            
            if (strstr(line, "hash"))           current_engine = 2;
            else if (strstr(line, "skiptable")) current_engine = 3;
            continue;
        }
        
        // ===== 读取 $ 开头的 token =====
        if (ch == '$') {
            ungetc(ch, fp);
            
            char *key = read_token(fp);
            if (!key) {
                printf("[Snap] ERROR: failed to read key\n");
                break;
            }
            
            char *value = read_token(fp);
            if (!value) {
                printf("[Snap] ERROR: failed to read value\n");
                free(key);
                break;
            }
            
            /* 读取每条 KV 末尾的内联 CRC 校验值 */
            char *hex = read_token(fp);
            if (!hex) {
                printf("[Snap] ERROR: failed to read crc\n");
                free(key);
                free(value);
                break;
            }

            uint32_t calc   = crc32_calc_snap(key, value);
            uint32_t stored = (uint32_t)strtoul(hex, NULL, 16);
            if (stored != calc) {
                printf("[Snap] CRC MISMATCH key='%s': stored=%08x, calc=%08x\n",
                       key, stored, calc);
                free(key);
                free(value);
                free(hex);
                break;
            }

            // ===== 根据当前引擎分发 =====
            switch (current_engine) {
                case 2: kvs_hash_set(&global_hash, key, value); break;
                case 3: kvs_skiptable_set(&global_skiptable, key, value); break;
            }
            
            free(key);
            free(value);
            free(hex);
            count++;
        }
    }
    
    fclose(fp);
    printf("Loaded %d entries from %s\n", count, SNAP_FILE);
    return 0;
}
#endif


