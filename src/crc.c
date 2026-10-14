#include "kvstore.h"

#include <inttypes.h>

#if ENABLE_CRC

static uint32_t crc32_table[256];
static int crc32_table_ready = 0;


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
    return crc32_update(0xFFFFFFFFu, data, len) ^ 0xFFFFFFFFu;
}

/**
 * CRC32 增量累计：把 data 的 len 个字节喂给 crc 状态机。
 * crc 初值应为 0xFFFFFFFF，全部字节喂完后结果需 ^ 0xFFFFFFFF 才是最终校验值。
 * 用于 AOF 全文 CRC：追加命令时逐段累计，无需每次全量重算。
 */
uint32_t crc32_update(uint32_t crc, const char *data, size_t len) {
    crc32_init_table();
    size_t i;
    for (i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ (unsigned char)data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc;
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





#endif