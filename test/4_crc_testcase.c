// 5_crc_testcase.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "kvstore.h"

#if ENABLE_CRC

#define CRC_MAGIC 0x43524331u

/* ---------------- CRC 文件加载 ---------------- */

typedef struct {
    uint32_t *crcs;
    uint64_t *seqs;
    int       count;
    uint64_t  next_seq;
} crc_dump_t;

/*
 * 返回：
 *   0  成功
 *  -1  文件不存在 / 打开失败
 *  -2  文件为空（0 字节）
 *  -3  文件存在但格式不对（bad magic / 读取失败）
 */
static int crc_load_file(const char *path, crc_dump_t *out) {
    memset(out, 0, sizeof(*out));

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        printf("[WARN] cannot open %s (skip)\n", path);
        return -1;
    }

    /* 先看文件大小 */
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fsize <= 0) {
        printf("[WARN] %s is empty (skip)\n", path);
        fclose(fp);
        return -2;
    }

    uint32_t magic = 0;
    int      count = 0;
    uint64_t next_seq = 0;

    if (fread(&magic, sizeof(magic), 1, fp) != 1 || magic != CRC_MAGIC) {
        printf("[WARN] %s: bad magic 0x%08x (skip)\n", path, magic);
        fclose(fp);
        return -3;
    }
    if (fread(&count, sizeof(count), 1, fp) != 1 ||
        fread(&next_seq, sizeof(next_seq), 1, fp) != 1) {
        printf("[WARN] %s: header read failed (skip)\n", path);
        fclose(fp);
        return -3;
    }
    if (count < 0 || count > 100000000) {
        printf("[WARN] %s: bad count %d (skip)\n", path, count);
        fclose(fp);
        return -3;
    }

    out->count    = count;
    out->next_seq = next_seq;

    if (count == 0) {
        fclose(fp);
        return 0;
    }

    out->crcs = (uint32_t *)malloc(sizeof(uint32_t) * count);
    out->seqs = (uint64_t *)malloc(sizeof(uint64_t) * count);
    if (!out->crcs || !out->seqs) {
        free(out->crcs); free(out->seqs);
        printf("[WARN] %s: oom (skip)\n", path);
        fclose(fp);
        return -3;
    }

    int i;
    for (i = 0; i < count; i++) {
        crc_uint_t u;
        if (fread(&u, sizeof(u), 1, fp) != 1) {
            printf("[WARN] %s: read unit %d failed (skip)\n", path, i);
            free(out->crcs); free(out->seqs);
            fclose(fp);
            return -3;
        }
        out->crcs[i] = u.crc;
        out->seqs[i] = u.seq;
    }

    fclose(fp);
    return 0;
}

static void crc_dump_free(crc_dump_t *d) {
    if (d->crcs) free(d->crcs);
    if (d->seqs) free(d->seqs);
    d->crcs = NULL;
    d->seqs = NULL;
    d->count = 0;
}

/* ---------------- 对比 ---------------- */

static int crc_compare(const char *name,
                       const crc_dump_t *a, const crc_dump_t *b) {
    printf("\n----- compare %s -----\n", name);
    printf("  master: count=%d next_seq=%llu\n",
           a->count, (unsigned long long)a->next_seq);
    printf("  slave : count=%d next_seq=%llu\n",
           b->count, (unsigned long long)b->next_seq);

    if (a->count != b->count) {
        printf("  [FAIL] count mismatch (%d vs %d)\n", a->count, b->count);
        return -1;
    }

    int mismatch = 0;
    int i;
    for (i = 0; i < a->count; i++) {
        if (a->crcs[i] != b->crcs[i]) {
            if (mismatch < 10) {
                printf("  [MISMATCH] idx=%d master.crc=0x%08x slave.crc=0x%08x\n",
                       i, a->crcs[i], b->crcs[i]);
            }
            mismatch++;
        }
        if (a->seqs[i] != b->seqs[i]) {
            if (mismatch < 10) {
                printf("  [MISMATCH] idx=%d master.seq=%llu slave.seq=%llu\n",
                       i,
                       (unsigned long long)a->seqs[i],
                       (unsigned long long)b->seqs[i]);
            }
            mismatch++;
        }
    }

    if (mismatch == 0) {
        printf("  [OK] %s identical (%d entries)\n", name, a->count);
        return 0;
    }
    printf("  [FAIL] %s: %d mismatches\n", name, mismatch);
    return -1;
}

/*
 * 对比一对文件：
 *   返回  0  = 一致
 *   返回 -1  = 不一致
 *   返回 -2  = 跳过（文件空/缺/坏）
 */
static int compare_one(const char *name,
                       const char *m_path, const char *s_path) {
    crc_dump_t md = {0}, sd = {0};
    int rm = crc_load_file(m_path, &md);
    int rs = crc_load_file(s_path, &sd);
    int ret;

    if (rm != 0 || rs != 0) {
        /* 任意一边加载失败，跳过 */
        printf("\n----- %s: SKIP (m_ret=%d s_ret=%d) -----\n",
               name, rm, rs);
        ret = -2;
        goto cleanup;
    }

    ret = crc_compare(name, &md, &sd);

cleanup:
    crc_dump_free(&md);
    crc_dump_free(&sd);
    return ret;
}

/* ---------------- mode 处理 ---------------- */

static int mode_compare(void) {
    printf("===== mode 1: compare master/slave CRC =====\n");

    /*
     * 本场景是 AOF 模式：
     *   主机 data/crc_aof.dat  ←  实时写入
     *   从机 test/crc_aof.dat  ←  你拷过来的从机副本
     * SNAP 文件为空，不对比。
     */
    int r_aof = compare_one("crc_aof.dat",
                            "data/crc_aof.dat",
                            "test/crc_aof.dat");

    printf("\n===== summary =====\n");
    if (r_aof == 0)
        printf("  crc_aof.dat : IDENTICAL\n");
    else if (r_aof == -2)
        printf("  crc_aof.dat : SKIPPED\n");
    else
        printf("  crc_aof.dat : DIFFERENT\n");

    return (r_aof == 0) ? 0 : 1;
}

/*
 * mode 2: 修改从机数据（本地副本）
 * 直接改 test/crc_aof.dat 中间的 2 条记录，制造不一致。
 */
static int mode_tamper(void) {
    printf("===== mode 2: tamper slave crc_aof.dat (local copy) =====\n");

    const char *path = "test/crc_aof.dat";
    FILE *fp = fopen(path, "r+b");
    if (!fp) {
        printf("[ERR] cannot open %s for tamper\n", path);
        return -1;
    }

    uint32_t magic = 0;
    int      count = 0;
    uint64_t next_seq = 0;
    if (fread(&magic, sizeof(magic), 1, fp) != 1 ||
        fread(&count, sizeof(count), 1, fp) != 1 ||
        fread(&next_seq, sizeof(next_seq), 1, fp) != 1) {
        printf("[ERR] read header failed\n");
        fclose(fp);
        return -1;
    }
    if (magic != CRC_MAGIC || count <= 0) {
        printf("[ERR] bad file (magic=0x%08x count=%d)\n", magic, count);
        fclose(fp);
        return -1;
    }

    /* 中间位置 */
    int mid = count / 2;
    if (mid < 1) mid = 1;

    int targets[2] = { mid - 1, mid };
    int k;
    for (k = 0; k < 2; k++) {
        int idx = targets[k];
        if (idx < 0 || idx >= count) continue;

        long off = sizeof(magic) + sizeof(count) + sizeof(next_seq)
                 + (long)sizeof(crc_uint_t) * idx;
        if (fseek(fp, off, SEEK_SET) != 0) {
            printf("[ERR] seek idx=%d failed\n", idx);
            fclose(fp);
            return -1;
        }
        crc_uint_t u;
        if (fread(&u, sizeof(u), 1, fp) != 1) {
            printf("[ERR] read idx=%d failed\n", idx);
            fclose(fp);
            return -1;
        }

        uint32_t old_crc = u.crc;
        u.crc ^= 0xDEADBEEFu;   /* 翻转一些位 */

        if (fseek(fp, off, SEEK_SET) != 0) {
            printf("[ERR] seek-back idx=%d failed\n", idx);
            fclose(fp);
            return -1;
        }
        if (fwrite(&u, sizeof(u), 1, fp) != 1) {
            printf("[ERR] write idx=%d failed\n", idx);
            fclose(fp);
            return -1;
        }
        printf("  tampered idx=%d: crc 0x%08x -> 0x%08x\n",
               idx, old_crc, u.crc);
    }

    fflush(fp);
    fclose(fp);
    printf("  slave crc_aof.dat tampered (%d entries, 2 modified)\n", count);
    return 0;
}

static void usage(const char *prog) {
    printf("Usage: %s <m_ip> <m_port> <s_ip> <s_port> <mode>\n", prog);
    printf("\n");
    printf("  mode 1: compare data/crc_aof.dat vs test/crc_aof.dat\n");
    printf("  mode 2: tamper test/crc_aof.dat (modify 2 entries)\n");
    printf("  mode 3: same as mode 1 (compare again)\n");
    printf("\n");
    printf("Before running:\n");
    printf("  cp <slave>/data/crc_aof.dat test/crc_aof.dat\n");
}

int main(int argc, char *argv[]) {
    if (argc != 6) { usage(argv[0]); return -1; }

    /* ip/port 参数保留，当前只读本地文件 */
    const char *m_ip   = argv[1];
    int         m_port = atoi(argv[2]);
    const char *s_ip   = argv[3];
    int         s_port = atoi(argv[4]);
    int         mode   = atoi(argv[5]);

    printf("===== CRC test =====\n");
    printf("master: %s:%d\n", m_ip, m_port);
    printf("slave : %s:%d\n", s_ip, s_port);
    printf("mode  : %d\n", mode);

    switch (mode) {
    case 1:
        return mode_compare() == 0 ? 0 : 1;
    case 2:
        return mode_tamper() == 0 ? 0 : 1;
    case 3:
        printf("(mode 3 -> compare again)\n");
        return mode_compare() == 0 ? 0 : 1;
    default:
        usage(argv[0]);
        return -1;
    }
}

#else

int main() {
    printf("crc is not work\n");
}

#endif