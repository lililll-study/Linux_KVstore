#include "kvstore.h"

#include <inttypes.h>

#if ENABLE_CRC
crc_list_t g_crc_list_snap;
crc_list_t g_crc_list_aof;

static uint32_t crc32_table[256];
static int crc32_table_ready = 0;

// 由于在客户端连接时，以及从机同步线程，还有主线程，都会访问数据引擎，因此需要在数据写入的时候进行加锁

/**
 * 
 * 把CRC32算法中要用到的256个查表值算好，存进全局数组
 * 之后每次算 CRC 就直接查表，而不是逐位运算。
 */
static void crc32_init_table(void) {
    if (crc32_table_ready) return;

    uint32_t i;
    for (i=0; i <256; i++) {
        uint32_t c = i;
        int j;
        for (j=0; j<8; j++) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        crc32_table[i] = c;
    }
    crc32_table_ready = 1;
}

/**
 * 对一段内存计算CRC32 校验值，返回32位无符号数
 */
uint32_t crc32_calc(const char *data, size_t len) {
    crc32_init_table();
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    for (i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ (unsigned char)data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

/* SNAP：CRC32(key + ':' + value) —— 用分隔符消除边界歧义 */
uint32_t crc32_calc_snap(const char *key, const char *value) {
    uint32_t c = crc32_calc(key, strlen(key));
    c ^= crc32_calc(":", 1) * 31u;
    if (value) {
        c ^= crc32_calc(value, strlen(value)) * 131u;
    }
    return c;
}

/* AOF：CRC32(cmd + '|' + key + '|' + value) —— value 可以为 NULL */
uint32_t crc32_calc_aof(const char *cmd, const char *key, const char *value) {
    uint32_t c = crc32_calc(cmd, strlen(cmd));
    c ^= crc32_calc("|", 1) * 31u;
    c ^= crc32_calc(key, strlen(key)) * 131u;
    if (value) {
        c ^= crc32_calc("|", 1) * 313u;
        c ^= crc32_calc(value, strlen(value)) * 617u;
    }
    return c;
}


// 生命周期
int crc_list_create(crc_list_t *list) {
    if (!list) return -1;
    memset(list, 0, sizeof(*list));

    list->cap = 4096;
    list->units = (crc_uint_t *)calloc(list->cap, sizeof(crc_uint_t));  // malloc不会初始化内容为0，relloc会自动扩容
    list->count = 0;
    list->next_seq = 1;

    if (pthread_mutex_init(&list->lock, NULL) != 0) {
        free(list->units);
        list->units = NULL;
        return -1;
    }
    return 0;
}
void crc_list_destroy(crc_list_t *list) {
    if (!list) return;
    if (list->units) {
        free(list->units);
        list->units = NULL;
    }
    pthread_mutex_destroy(&list->lock);
    list->count = 0;
    list->cap = 0;
    list->next_seq = 0;
}
void crc_list_reset(crc_list_t *list) {
    if(!list) return;
    pthread_mutex_lock(&list->lock);
    list->count = 0;
    list->next_seq = 1;
    pthread_mutex_unlock(&list->lock);
}

// 追加记录
/**
 * type : "SNAP" 表示快照条目，其余（如 "SET"/"DEL"）表示 AOF 命令
 * key  : 必填
 * value: 可空（DEL 类命令）
 */
int crc_list_append(crc_list_t *list, const char *type, const char *key, const char *value) {

    if (!list || !type || !key) return -1;
    pthread_mutex_lock(&list->lock);

    // 扩容
    if (list->count >= list->cap) {
        int new_cap = list->cap > 0 ? list->cap *2 : 4096;
        // 把原先的内容扩大，且保留原内容。但新扩大的内容是垃圾值
        crc_uint_t *nu = (crc_uint_t *) realloc(list->units, (size_t)new_cap * sizeof(crc_uint_t));
        if (!nu) {
            pthread_mutex_unlock(&list->lock);
            return -1;
        }
        list->units = nu;
        list->cap = new_cap;
    }
    // 把新单元位置取出，赋值给u。count自增
    crc_uint_t *u = &list->units[list->count++];
    u->seq = list->next_seq++; // 把当前值赋值给新单元，然后自增

    if (strcmp(type, "SNAP") == 0) {
        u->crc = crc32_calc_snap(key, value);
    } else {
        u->crc = crc32_calc_aof(type, key, value);
    }
    pthread_mutex_unlock(&list->lock);
    return 0;
}

// 持久化
#define CRC_MAGIC 0x43524331u   /* "CRC1" */

/**
 * 把整个CRC list以二进制的格式写入文件，用于进程重启后能恢复 CRC 记录，保证主从比对时 seq 不断档。
 */
int crc_list_save(crc_list_t *list, const char *filename) {
    if (!list || !filename) return -1;
    FILE *fp = fopen(filename, "wb");
    if(!fp) {
        LOG_ERROR("[CRC] fopen %s failed\n", filename);
        return -1;
    }
    pthread_mutex_lock(&list->lock);
    uint32_t magic = CRC_MAGIC;
    if (fwrite(&magic,        sizeof(magic),        1, fp) != 1 ||
        fwrite(&list->count,  sizeof(list->count),  1, fp) != 1 ||
        fwrite(&list->next_seq, sizeof(list->next_seq), 1, fp) != 1) {
        pthread_mutex_unlock(&list->lock);
        fclose(fp);
        return -1;
    }

    if (list->count > 0) {
        if (fwrite(list->units, sizeof(crc_uint_t),
                   (size_t)list->count, fp) != (size_t)list->count) {
            pthread_mutex_unlock(&list->lock);
            fclose(fp);
            return -1;
        }
    }

    pthread_mutex_unlock(&list->lock);
    fclose(fp);
    return 0;
}

int crc_list_load(crc_list_t *list, const char *filename) {
    if (!list || !filename) return -1;

    FILE *fp = fopen(filename, "rb");
    if (!fp) return -1;   /* 首次启动没有文件很正常，返回 -1 让调用方忽略 */

    uint32_t magic = 0;
    int      count = 0;
    uint64_t next_seq = 0;

    if (fread(&magic, sizeof(magic), 1, fp) != 1 || magic != CRC_MAGIC) {
        fclose(fp);
        return -1;
    }
    if (fread(&count, sizeof(count), 1, fp) != 1 ||
        fread(&next_seq, sizeof(next_seq), 1, fp) != 1) {
        fclose(fp);
        return -1;
    }
    if (count < 0) { fclose(fp); return -1; }

    pthread_mutex_lock(&list->lock);

    if (count > list->cap) {
        crc_uint_t *nu = (crc_uint_t *)realloc(
            list->units, (size_t)count * sizeof(crc_uint_t));
        if (!nu) {
            pthread_mutex_unlock(&list->lock);
            fclose(fp);
            return -1;
        }
        list->units = nu;
        list->cap   = count;
    }

    if (count > 0 &&
        fread(list->units, sizeof(crc_uint_t), (size_t)count, fp)
            != (size_t)count) {
        pthread_mutex_unlock(&list->lock);
        fclose(fp);
        return -1;
    }

    list->count    = count;
    list->next_seq = next_seq;

    pthread_mutex_unlock(&list->lock);
    fclose(fp);
    return 0;
}



int crc_save_snap(void) { return crc_list_save(&g_crc_list_snap, CRC_LIST_SNAP_FILE); }
int crc_save_aof (void) { return crc_list_save(&g_crc_list_aof,  CRC_LIST_AOF_FILE);  }


#endif