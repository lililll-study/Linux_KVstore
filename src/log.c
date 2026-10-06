#include "kvstore.h"

#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <pthread.h>
#include <stdlib.h>

/* ===== 全局状态 ===== */
static FILE           *g_fp        = NULL;
static pthread_mutex_t g_lock      = PTHREAD_MUTEX_INITIALIZER;
static log_level_t     g_min_level = LOG_LEVEL_INFO;

static const char *g_level_name[] = {
    "DEBUG", "INFO", "WARN", "ERROR", "FATAL"
};

/* 单行日志最大长度（超出截断） */
#define LOG_LINE_MAX 2048

/* ===== 生命周期 ===== */

int kvs_log_init(const char *filename, log_level_t min_level) {
    pthread_mutex_lock(&g_lock);

    g_min_level = min_level;

    if (g_fp) {
        fclose(g_fp);
        g_fp = NULL;
    }

    if (filename) {
        g_fp = fopen(filename, "a");    // 以追加模式打开，不会覆盖旧日志
        if (!g_fp) {
            pthread_mutex_unlock(&g_lock);
            fprintf(stderr, "[LOG] fopen %s failed\n", filename);
            return -1;
        }
        /* 文件日志用行缓冲，避免每条都 fflush 太慢；
         * 但崩溃时可能丢最后几行，调试期可改 _IONBF */
        setvbuf(g_fp, NULL, _IOLBF, 0); // 行缓冲
    }

    pthread_mutex_unlock(&g_lock);
    return 0;
}

void kvs_log_fini(void) {
    pthread_mutex_lock(&g_lock);
    if (g_fp) {
        fflush(g_fp);   // 把缓冲区没落盘的数据刷出去
        fclose(g_fp);
        g_fp = NULL;    // 把文件fp置空
    }
    pthread_mutex_unlock(&g_lock);
}

void kvs_log_set_level(log_level_t level) {
    pthread_mutex_lock(&g_lock);
    g_min_level = level;
    pthread_mutex_unlock(&g_lock);
}

log_level_t kvs_log_get_level(void) {
    log_level_t lv;
    pthread_mutex_lock(&g_lock);
    lv = g_min_level;
    pthread_mutex_unlock(&g_lock);
    return lv;
}

/* ===== 写日志 ===== */

void kvs_log_write(log_level_t level, const char *file, int line, const char *fmt, ...) {
    /* 级别过滤：先无锁读，允许极端情况下的瞬时不一致 */
    if (level < g_min_level) return;
    if (!file) file = "?";
    if (level < LOG_LEVEL_DEBUG) level = LOG_LEVEL_DEBUG;
    if (level > LOG_LEVEL_FATAL) level = LOG_LEVEL_FATAL;

    /* 1. 时间戳（到秒） */
    char timebuf[32];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm);
    /* 2. 拼头部 */
    char linebuf[LOG_LINE_MAX];
    int  n = snprintf(linebuf, sizeof(linebuf),
                      "[%s] [%-5s] [%s:%d] ",
                      timebuf,
                      g_level_name[level],
                      file, line);
    if (n < 0) return;
    if (n >= (int)sizeof(linebuf)) n = sizeof(linebuf) - 1;

    /* 3. 拼正文 */
    va_list ap;         // 用来遍历可变参数
    va_start(ap, fmt);  // 让ap从fmt后面的第一个参数开始
    /**
     * 举例子
     * LOG_INFO("kvstore starting, port=%d role=%s", 2000, "master");
     *                                          文件      行数
     * 展开后：kvs_log_write(LOG_LEVEL_INFO, "kvstore.c", 861, "kvstore starting, port=%d role=%s", 2000, "master");
     */

    // 前面写了长度为n的头，现在要往后拼接正文。
    // vsnprintf，按 fmt 里的格式符（%d、%s 等），依次从 ap 里取参数，格式化后填入 % 的位置。
    int m = vsnprintf(linebuf + n, sizeof(linebuf) - n, fmt, ap);
    va_end(ap);
    if (m < 0) m = 0;
    n += m;
    if (n >= (int)sizeof(linebuf) - 1) n = sizeof(linebuf) - 2;

    /* 4. 补换行 */
    if (n == 0 || linebuf[n - 1] != '\n') {
        linebuf[n++] = '\n';
    }
    linebuf[n] = '\0';

    /* 5. 加锁输出 */
    pthread_mutex_lock(&g_lock);
    if (g_fp) {
        fwrite(linebuf, 1, n, g_fp);
        /* 行缓冲，遇 \n 自动刷；但日志文件还是主动 flush 一下更稳 */
        // fflush(g_fp);
    }
    /* 同时输出到 stderr，方便前台观察 */
    // fwrite(linebuf, 1, n, stderr);
    pthread_mutex_unlock(&g_lock);
}