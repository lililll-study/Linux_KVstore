// 4_crc_testcase.c
// 按新持久化方案校验 SNAP/AOF 文件自身的 CRC（不再依赖旧独立 CRC 列表文件）：
//   SNAP: 每条 KV 后内联 8 位 hex CRC = CRC32(key + ':' + value)
//   AOF : 文件尾 "# AOF_CRC:xxxxxxxx\r\n"，CRC 覆盖整个命令区全部字节
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "kvstore.h"

#if ENABLE_CRC

#ifndef SNAP_FILE
#define SNAP_FILE "data/data_snap.snap"
#endif
#ifndef AOF_FILE
#define AOF_FILE "data/data_aof.aof"
#endif

#ifndef AOF_CRC_PREFIX
#define AOF_CRC_PREFIX   "# AOF_CRC:"
#define AOF_CRC_PREFIX_LEN 10
#endif
#ifndef AOF_CRC_HEX_LEN
#define AOF_CRC_HEX_LEN  8
#endif
#ifndef AOF_CRC_TAIL_LEN
#define AOF_CRC_TAIL_LEN 20
#endif

#define TAMPER_SUF ".tamper"

/* =============== 本地 CRC32（与 crc.c 算法完全一致） =============== */
static uint32_t crc32_table[256];
static int crc32_table_ready = 0;

static void crc32_init_table(void) {
    if (crc32_table_ready) return;
    uint32_t i;
    for (i = 0; i < 256; i++) {
        uint32_t c = i;
        int j;
        for (j = 0; j < 8; j++)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
    crc32_table_ready = 1;
}

static uint32_t crc32_calc_local(const char *data, size_t len) {
    crc32_init_table();
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    for (i = 0; i < len; i++)
        crc = crc32_table[(crc ^ (unsigned char)data[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* SNAP 每条 KV 的 CRC：CRC32(key) ^ CRC32(":")*31 ^ CRC32(value)*131 */
static uint32_t snap_crc_n(const char *key, long klen,
                           const char *value, long vlen) {
    uint32_t c = crc32_calc_local(key, (size_t)klen);
    c ^= crc32_calc_local(":", 1) * 31u;
    if (value)
        c ^= crc32_calc_local(value, (size_t)vlen) * 131u;
    return c;
}

static void hex8(uint32_t v, char out[9]) {
    snprintf(out, 9, "%08x", v);
}

static int hex_cmp(const char *a, const char *b, int len) {
    int i;
    for (i = 0; i < len; i++) {
        unsigned char x = (unsigned char)a[i];
        unsigned char y = (unsigned char)b[i];
        if (x >= 'A' && x <= 'F') x += 'a' - 'A';
        if (y >= 'A' && y <= 'F') y += 'a' - 'A';
        if (x != y) return 1;
    }
    return 0;
}

/* =============== 文件读写 =============== */
static unsigned char *read_file(const char *path, long *out_size) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size <= 0) { fclose(fp); return NULL; }
    unsigned char *buf = (unsigned char *)malloc((size_t)size);
    if (!buf) { fclose(fp); return NULL; }
    if (fread(buf, 1, (size_t)size, fp) != (size_t)size) {
        free(buf); fclose(fp); return NULL;
    }
    fclose(fp);
    *out_size = size;
    return buf;
}

static int copy_file(const char *src, const char *dst) {
    long size = 0;
    unsigned char *buf = read_file(src, &size);
    if (!buf) return -1;
    FILE *fp = fopen(dst, "wb");
    if (!fp) { free(buf); return -1; }
    fwrite(buf, 1, (size_t)size, fp);
    fclose(fp);
    free(buf);
    return 0;
}

/* 在文件的 1/3 处翻转一个字节 */
static int tamper_file(const char *path) {
    long size = 0;
    unsigned char *buf = read_file(path, &size);
    if (!buf) return -1;
    long at = size / 3;
    buf[at] ^= 0x01;
    FILE *fp = fopen(path, "wb");
    if (!fp) { free(buf); return -1; }
    fwrite(buf, 1, (size_t)size, fp);
    fclose(fp);
    free(buf);
    printf("  tampered byte at offset %ld\n", at);
    return 0;
}

/* =============== SNAP 校验 =============== */
/*
 * 格式:
 *   # hash\n / # skiptable\n                      分节标记
 *   $len\r\nkey\r\n$len\r\nvalue\r\n$8\r\n<hex>\r\n   每条 KV + 内联 CRC
 * 返回: 0 全部通过; 1 存在失败; -1 无法读取/为空
 */
static int verify_snap_file(const char *path) {
    long size = 0;
    unsigned char *buf = read_file(path, &size);
    if (!buf) return -1;

    long pos = 0;
    int entries = 0, fail = 0;

    while (pos < size) {
        /* 分节标记 */
        if (buf[pos] == '#') {
            if (strncmp((char *)buf + pos, "# hash\n", 7) == 0) {
                pos += 7;
                continue;
            }
            if (strncmp((char *)buf + pos, "# skiptable\n", 12) == 0) {
                pos += 12;
                continue;
            }
            printf("[FAIL] unknown marker at %ld\n", pos);
            fail++;
            break;
        }

        /* 读 key / value / crc 三个 bulk token */
        long klen = 0, vlen = 0, clen = 0;
        char *key = NULL, *value = NULL, *crc8 = NULL;

        #define NEXT_TOKEN(var, vlen) do { \
            if (pos >= size || buf[pos] != '$') { var = NULL; break; } \
            long _p = pos + 1; long _l = 0; \
            while (_p < size && buf[_p] >= '0' && buf[_p] <= '9') \
                _l = _l * 10 + (buf[_p++] - '0'); \
            if (_p + 2 > size || buf[_p] != '\r' || buf[_p+1] != '\n') { var = NULL; break; } \
            _p += 2; \
            if (_p + _l + 2 > size || buf[_p + _l] != '\r' || buf[_p + _l + 1] != '\n') { var = NULL; break; } \
            var = (char *)buf + _p; vlen = _l; pos = _p + _l + 2; \
        } while (0)

        NEXT_TOKEN(key, klen);
        NEXT_TOKEN(value, vlen);
        NEXT_TOKEN(crc8, clen);
        #undef NEXT_TOKEN

        if (!key || !value || !crc8 || clen != AOF_CRC_HEX_LEN) {
            printf("[FAIL] token parse error near offset %ld\n", pos);
            fail++;
            break;
        }

        char crc_str[9];
        memcpy(crc_str, crc8, AOF_CRC_HEX_LEN);
        crc_str[AOF_CRC_HEX_LEN] = '\0';

        char expect[9];
        hex8(snap_crc_n(key, klen, value, vlen), expect);
        if (hex_cmp(crc_str, expect, AOF_CRC_HEX_LEN) != 0) {
            if (fail < 10)
                printf("[MISMATCH] entry %d: crc=%s expect=%s\n",
                       entries, crc_str, expect);
            fail++;
        }
        entries++;
    }

    free(buf);
    if (fail == 0) {
        printf("[OK] SNAP verified: %d entries\n", entries);
        return 0;
    }
    printf("[FAIL] SNAP: %d entries, %d mismatches\n", entries, fail);
    return 1;
}

/* =============== AOF 校验 =============== */
/*
 * 格式:
 *   扁平 RESP 命令区（*N\r\n$len\r\n...）
 *   尾部固定 20 字节: # AOF_CRC:xxxxxxxx\r\n
 * CRC 覆盖命令区全部字节（不含尾部标志位本身）
 */
static int verify_aof_file(const char *path) {
    long size = 0;
    unsigned char *buf = read_file(path, &size);
    if (!buf) return -1;
    if (size < AOF_CRC_TAIL_LEN) {
        printf("[FAIL] %s too small (%ld bytes)\n", path, size);
        free(buf);
        return 1;
    }

    long cmd_region = size - AOF_CRC_TAIL_LEN;
    const char *tail = (const char *)buf + cmd_region;

    /* 1. 校验尾部标志位 */
    if (strncmp(tail, AOF_CRC_PREFIX, AOF_CRC_PREFIX_LEN) != 0) {
        printf("[FAIL] missing tail marker\n");
        free(buf);
        return 1;
    }
    if (strncmp(tail + AOF_CRC_PREFIX_LEN + AOF_CRC_HEX_LEN, "\r\n", 2) != 0) {
        printf("[FAIL] bad tail terminator\n");
        free(buf);
        return 1;
    }

    /* 2. 校验命令区可解析（扁平 RESP） */
    long pos = 0, cmds = 0;
    while (pos < cmd_region) {
        if (buf[pos] == '\r' && pos + 1 < cmd_region && buf[pos+1] == '\n') {
            pos += 2;
            continue;
        }
        if (pos >= cmd_region || buf[pos] != '*') {
            printf("[FAIL] bad command region at offset %ld\n", pos);
            free(buf);
            return 1;
        }
        long p = pos + 1;
        long n = 0;
        while (p < cmd_region && buf[p] >= '0' && buf[p] <= '9')
            n = n * 10 + (buf[p++] - '0');
        if (p + 2 > cmd_region || buf[p] != '\r' || buf[p+1] != '\n') {
            printf("[FAIL] bad array header at %ld\n", pos);
            free(buf);
            return 1;
        }
        p += 2;
        long i;
        for (i = 0; i < n; i++) {
            if (p >= cmd_region || buf[p] != '$') {
                printf("[FAIL] bad bulk at %ld\n", p);
                free(buf);
                return 1;
            }
            p++;
            long len = 0;
            while (p < cmd_region && buf[p] >= '0' && buf[p] <= '9')
                len = len * 10 + (buf[p++] - '0');
            if (p + 2 > cmd_region || buf[p] != '\r' || buf[p+1] != '\n') {
                printf("[FAIL] bad bulk header at %ld\n", p);
                free(buf);
                return 1;
            }
            p += 2;
            if (p + len + 2 > cmd_region ||
                buf[p + len] != '\r' || buf[p + len + 1] != '\n') {
                printf("[FAIL] bad bulk body at %ld\n", p);
                free(buf);
                return 1;
            }
            p += len + 2;
        }
        pos = p;
        cmds++;
    }

    /* 3. 全文 CRC 比对 */
    uint32_t crc = crc32_calc_local((const char *)buf, (size_t)cmd_region);
    char expect[9];
    hex8(crc, expect);
    char tail_hex[9];
    memcpy(tail_hex, tail + AOF_CRC_PREFIX_LEN, AOF_CRC_HEX_LEN);
    tail_hex[AOF_CRC_HEX_LEN] = '\0';
    if (hex_cmp(tail_hex, expect, AOF_CRC_HEX_LEN) != 0) {
        printf("[FAIL] AOF full-CRC mismatch: file=%s expect=%s\n",
               tail_hex, expect);
        free(buf);
        return 1;
    }

    printf("[OK] AOF verified: %ld commands, %ld command bytes, tail CRC matched\n",
           cmds, cmd_region);
    free(buf);
    return 0;
}

/* =============== 模式 =============== */
static int mode_verify_snap(void) {
    printf("===== mode 1: verify SNAP inline CRC =====\n");
    int r = verify_snap_file(SNAP_FILE);
    if (r == -1)
        printf("  (skip: %s missing/empty)\n", SNAP_FILE);
    return (r == 0) ? 0 : 1;
}

static int mode_tamper_snap(void) {
    printf("===== mode 2: tamper SNAP copy + expect detection =====\n");
    const char *copy = SNAP_FILE TAMPER_SUF;
    if (copy_file(SNAP_FILE, copy) != 0) {
        printf("[ERR] copy failed (run mode 1 first?)\n");
        return 1;
    }
    tamper_file(copy);
    int r = verify_snap_file(copy);
    remove(copy);
    if (r == 0) {
        printf("==> [FAIL] tamper NOT detected\n");
        return 1;
    }
    printf("==> [OK] tamper detected (verify ret=%d)\n", r);
    return 0;
}

static int mode_verify_aof(void) {
    printf("===== mode 3: verify AOF tail full-CRC =====\n");
    int r = verify_aof_file(AOF_FILE);
    if (r == -1)
        printf("  (skip: %s missing/empty)\n", AOF_FILE);
    return (r == 0) ? 0 : 1;
}

static int mode_tamper_aof(void) {
    printf("===== mode 4: tamper AOF copy + expect detection =====\n");
    const char *copy = AOF_FILE TAMPER_SUF;
    if (copy_file(AOF_FILE, copy) != 0) {
        printf("[ERR] copy failed (run mode 3 first?)\n");
        return 1;
    }
    tamper_file(copy);
    int r = verify_aof_file(copy);
    remove(copy);
    if (r == 0) {
        printf("==> [FAIL] tamper NOT detected\n");
        return 1;
    }
    printf("==> [OK] tamper detected (verify ret=%d)\n", r);
    return 0;
}

static int mode_verify_all(void) {
    printf("===== mode 5: verify SNAP + AOF =====\n");
    int r1 = verify_snap_file(SNAP_FILE);
    int r2 = verify_aof_file(AOF_FILE);
    if (r1 == -1) printf("  (skip: %s missing/empty)\n", SNAP_FILE);
    if (r2 == -1) printf("  (skip: %s missing/empty)\n", AOF_FILE);
    return (r1 == 0 && r2 == 0) ? 0 : 1;
}

static void usage(const char *prog) {
    printf("Usage: %s <mode>\n", prog);
    printf("  mode 1: verify SNAP inline CRC\n");
    printf("  mode 2: tamper SNAP copy + verify detection\n");
    printf("  mode 3: verify AOF tail full-CRC\n");
    printf("  mode 4: tamper AOF copy + verify detection\n");
    printf("  mode 5: verify both SNAP and AOF\n");
}

int main(int argc, char *argv[]) {
    int mode;
    if (argc == 2) {
        mode = atoi(argv[1]);
    } else if (argc == 6) {
        /* 兼容旧参数: <m_ip> <m_port> <s_ip> <s_port> <mode> */
        mode = atoi(argv[5]);
    } else {
        usage(argv[0]);
        return -1;
    }

    printf("===== CRC test =====\n");
    printf("mode : %d\n", mode);

    switch (mode) {
    case 1: return mode_verify_snap();
    case 2: return mode_tamper_snap();
    case 3: return mode_verify_aof();
    case 4: return mode_tamper_aof();
    case 5: return mode_verify_all();
    default:
        usage(argv[0]);
        return -1;
    }
}

#else

int main(void) {
    printf("crc is not work\n");
    return 0;
}

#endif