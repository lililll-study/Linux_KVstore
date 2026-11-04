#ifndef __KVS_LEAK_H__
#define __KVS_LEAK_H__

// 极简泄漏检测：登记/注销/退出报告，三步搞定
#ifndef ENABLE_LEAK
#define ENABLE_LEAK 0
#endif

#if ENABLE_LEAK

void *kvs_leak_malloc(size_t size, const char *file, int line);
void  kvs_leak_free(void *ptr, const char *file, int line);
void  kvs_leak_report(void);
void  kvs_leak_init(void);
void  kvs_leak_set_enable(int on);   // 运行时开关（配置 leak.enable）
int   kvs_leak_enabled(void);

#define kvs_malloc(size)  kvs_leak_malloc((size), __FILE__, __LINE__)
#define kvs_free(ptr)     kvs_leak_free((ptr), __FILE__, __LINE__)

#endif
#endif