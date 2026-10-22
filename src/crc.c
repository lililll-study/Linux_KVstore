#include "kvstore.h"

#include <inttypes.h>

/**
 * 整体作用：
 * 给一段内存中的数据，生成32bit的指纹
 * 以检测数据在存储或传输过程中有没有损坏
 */


#if ENABLE_CRC

static uint32_t crc32_table[256];
static int crc32_table_ready = 0;


/**
 * @name 初始化查表
 * @fn  预计算256个CRC32查表值，存到全局数组crc32_table中
 *      因为CRC原始算法需要逐位运算很慢，提前算好表之后每个字节只需要一次查表+异或
 * @attention 这里的crc值是怎么计算的？为什么需要256个值？为什么用CRC32？不用16
 */
static void crc32_init_table(void) {
    if (crc32_table_ready) return;

    uint32_t i;
    for (i=0; i <256; i++) {// 一个字节有258种可能
        uint32_t c = i;
        int j;
        for (j=0; j<8; j++) {// 循环8次处理一个bit
            // 看最低位是0还是1， 如果最低位是1：那么右移一位后，再异或多项式，如果是0：只右移，不异或
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        crc32_table[i] = c;
    }
    crc32_table_ready = 1;
}

/**
 * @name 计算一段内存的CRC32
 * @fn  对一段内存（data，长度 len）计算 CRC32 校验值
 *      先以 0xFFFFFFFF 为初始值调用 crc32_update
 *      最后再 ^ 0xFFFFFFFF 做最终处理
 * @attention 为什么是这个流程？：CRC特性：初值取反 + 结果取反
 */
uint32_t crc32_calc(const char *data, size_t len) {
    // 以 0xFFFFFFFF 为初始 CRC 值
    // 把 data 的 len 个字节全部“喂”进 crc32_update，把最终结果再 ^ 0xFFFFFFFF（按位取反）
    return crc32_update(0xFFFFFFFFu, data, len) ^ 0xFFFFFFFFu;
}

/**
 * @name 增量累计 CRC32
 * @fn  把一段数据“喂”进 CRC 状态机，返回更新后的 CRC 值。
 *      处理分段数据（如 AOF 日志逐条追加写入时，
 *      不需要每次重算整个文件，只需在已有 CRC 基础上继续累计）
 * @attention 怎么实现这个功能的？
 */
uint32_t crc32_update(uint32_t crc, const char *data, size_t len) {
    crc32_init_table();
    size_t i;
    for (i = 0; i < len; i++) {
        // 取当前字节强转unsigned char
        // 当前 CRC 的低 8 位与当前字节异或（用异或是因为CRC是当前状态+新字节的混合运算）
        // 只保留低 8 位，作为查表索引（0~255），因为当前字节的 8 位已经被“消耗”掉了，CRC 状态需要“腾出”低 8 位给下一个字节。
        crc = crc32_table[(crc ^ (unsigned char)data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc;
}

/**
 * @name 计算snap中 key+value 的快照 CRC
 * @fn  对 key 和 value 的组合计算一个 CRC 值
 * @attention 为什么要加：分隔符和系数
 *            加了分隔符，用于区分k和v，避免算出相同的crc。加系数，进一步区分crc
 */
uint32_t crc32_calc_snap(const char *key, const char *value) {
    uint32_t c = crc32_calc(key, strlen(key));
    c ^= crc32_calc(":", 1) * 31u;
    if (value) {
        c ^= crc32_calc(value, strlen(value)) * 131u;
    }
    return c;
}

#endif