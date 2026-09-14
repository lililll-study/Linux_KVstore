#include "kvstore.h"

// ===== 全局配置默认值 =====
kvs_config_t g_config = {
    .role = ROLE_MASTER,
    .port = 2000,
    .master_ip = "192.168.137.128",
    .master_port = 2000,
    .snap_enable = 1,      // 默认开启快照同步
    .aof_enable = 0        // 默认关闭 AOF 同步
};

// ===== 去除字符串首尾空白 =====
static char* trim(char *str) {
    while (*str == ' ' || *str == '\t') str++;
    if (*str == '\0') return str;
    char *end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
        *end-- = '\0';
    }
    return str;
}

// ===== 解析布尔值 =====
static int parse_bool(const char *value) {
    if (strcmp(value, "true") == 0)  return 1;
    if (strcmp(value, "false") == 0) return 0;
    if (strcmp(value, "yes") == 0)   return 1;
    if (strcmp(value, "no") == 0)    return 0;
    if (strcmp(value, "on") == 0)    return 1;
    if (strcmp(value, "off") == 0)   return 0;
    return atoi(value);  // "1" / "0"
}

// ===== 加载配置文件 =====
int kvs_config_load(const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        printf("[Config] Cannot open config file: %s\n", filename);
        return -1;
    }
    
    char line[512];
    int line_no = 0;
    
    while (fgets(line, sizeof(line), fp)) {
        line_no++;
        
        // 跳过注释和空行
        char *p = trim(line);
        if (*p == '#' || *p == '\0') continue;
        
        // 解析 key = value
        char *eq = strchr(p, '=');
        if (!eq) {
            printf("[Config] Line %d: invalid format, skip\n", line_no);
            continue;
        }
        
        *eq = '\0';
        char *key = trim(p);
        char *value = trim(eq + 1);
        
        // ===== 解析 key.subkey 格式 =====
        char *subkey = strchr(key, '.');
        if (subkey) {
            *subkey = '\0';
            subkey++;
        }
        
        // ===== 处理各项配置 =====
        if (strcmp(key, "role") == 0) {
            if (strcmp(value, "master") == 0) {
                g_config.role = ROLE_MASTER;
            } else if (strcmp(value, "slave") == 0) {
                g_config.role = ROLE_SLAVE;
            }
        } else if (strcmp(key, "port") == 0) {
            g_config.port = atoi(value);
        } else if (strcmp(key, "master_ip") == 0) {
            strncpy(g_config.master_ip, value, sizeof(g_config.master_ip) - 1);
        } else if (strcmp(key, "master_port") == 0) {
            g_config.master_port = atoi(value);
        }
        // ===== 处理 key.subkey 格式 =====
        else if (subkey && strcmp(key, "snap") == 0) {
            if (strcmp(subkey, "enable") == 0) {
                g_config.snap_enable = parse_bool(value);
            }
        } else if (subkey && strcmp(key, "aof") == 0) {
            if (strcmp(subkey, "enable") == 0) {
                g_config.aof_enable = parse_bool(value);
            }
        } else {
            printf("[Config] Unknown key: %s\n", key);
        }
    }
    
    fclose(fp);
    return 0;
}

// ===== 打印配置 =====
void kvs_config_print(void) {
    printf("========== Config ==========\n");
    printf("  role        : %s\n", g_config.role == ROLE_MASTER ? "master" : "slave");
    printf("  port        : %d\n", g_config.port);
    if (g_config.role == ROLE_SLAVE) {
        printf("  master      : %s:%d\n", g_config.master_ip, g_config.master_port);
    }
    printf("  snap.enable : %s\n", g_config.snap_enable ? "true" : "false");
    printf("  aof.enable  : %s\n", g_config.aof_enable ? "true" : "false");
    printf("============================\n");
}

