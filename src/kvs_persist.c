#include "kvstore.h"
#include "kvs_uring.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
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



// ===== 写入一个 token =====
static int write_token(FILE *fp, const char *data) {
    if (!data) return -1;
    int len = strlen(data);
    fprintf(fp, "$%d\r\n%s\r\n", len, data);
    return 0;
}

/* CRC32 值格式化为 8 位十六进制字符串（方便存入SNAP/AOF文件中） */
static const char *crc32_to_hex(uint32_t crc) {
    static char hexbuf[9];
    snprintf(hexbuf, sizeof(hexbuf), "%08x", (unsigned)crc);
    return hexbuf;
}

/* 底层函数 */
int save_snapshot_hash(const char *filename) {
    FILE *fp = fopen(filename, "a");
    if (!fp) return -1;
    
    fprintf(fp, "# hash\n");
    int i = 0;
    for (i = 0; i < global_hash.max_slots; i++) {
        hashnode_t *node = global_hash.nodes[i];
        while (node != NULL) {
            write_token(fp, node->key);
            write_token(fp, node->value);
            /* 每条 KV 末尾内联 CRC 校验值：$8\r\n<8位hex>\r\n */
            write_token(fp, crc32_to_hex(crc32_calc_snap(node->key, node->value)));
            node = node->next;
        }
    }

    /* 渐进 rehash 期间：新表中的节点也要落盘 */
    for (i = 0; global_hash.rehash_nodes && i < global_hash.rehash_slots; i++) {
        hashnode_t *node = global_hash.rehash_nodes[i];
        while (node != NULL) {
            write_token(fp, node->key);
            write_token(fp, node->value);
            write_token(fp, crc32_to_hex(crc32_calc_snap(node->key, node->value)));
            node = node->next;
        }
    }

    fclose(fp);
    LOG_INFO("Snapshot hash saved to %s\n", filename);
    return 0;
}

int save_snapshot_skiptable(const char *filename) {
    FILE *fp = fopen(filename, "a");
    if (!fp) return -1;
    
    fprintf(fp, "# skiptable\n");
    
    kvs_node_t *curr = global_skiptable.header->forward[0];
    while (curr != NULL) {
        write_token(fp, curr->key);
        write_token(fp, curr->value);
        /* 每条 KV 末尾内联 CRC 校验值 */
        write_token(fp, crc32_to_hex(crc32_calc_snap(curr->key, curr->value)));
        curr = curr->forward[0];
    }
    
    fclose(fp);
    LOG_INFO("Snapshot skiptable saved to %s\n", filename);
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


