
#include "kvstore.h"

// 启动命令
// gcc -o kvstore kvstore.c reactor.c ntyco.c  -I ./NtyCo/core  -L ./NtyCo/lib  -lntyco -lpthread -ldl
// gcc -o kvstore kvstore.c reactor.c

#if ENABLE_ARRAY
extern kvs_array_t global_array;
#endif

#if ENABLE_RBTREE
extern kvs_rbtree_t global_rbtree;
#endif

#if ENABLE_HASH
extern kvs_hash_t global_hash;
#endif


void *kvs_malloc (size_t size) {
    return malloc(size);
}

void kvs_free(void *ptr) {
    return free(ptr);
}


const char *command[] = {
    "SET", "GET", "DEL", "MOD", "EXIST",
    "RSET", "RGET", "RDEL", "RMOD", "REXIST",
    "HSET", "HGET", "HDEL", "HMOD", "HEXIST"

};

enum {
    KVS_CMD_START = 0,
    //array
    KVS_CMD_SET = KVS_CMD_START,
    KVS_CMD_GET,
    KVS_CMD_DEL,
    KVS_CMD_MOD,
    KVS_CMD_EXIST,
    // rbtree
    KVS_CMD_RSET,
    KVS_CMD_RGET,
    KVS_CMD_RDEL,
    KVS_CMD_RMOD,
    KVS_CMD_REXIST,
	// hash
	KVS_CMD_HSET,
	KVS_CMD_HGET,
	KVS_CMD_HDEL,
	KVS_CMD_HMOD,
	KVS_CMD_HEXIST,
	
	KVS_CMD_COUNT,

};

const char *response[] = {};






int kvs_split_token(char *msg, char *tokens[]) {

    if (msg == NULL || tokens == NULL)return -1;
    int idx = 0;
    char *token = strtok(msg, " ");

    while (token != NULL) {
        // printf("idx: %d, %s\n", idx, token);
        tokens[idx ++] = token;
        token = strtok(NULL, " ");
    }

    return idx;

}

// SET KEY VALUE
// tokens[0]: SET ; tokens[1]: KEY; tokens[0]: VALUE
// 进行协议数据过滤
int kvs_filter_protocol(char **tokens, int count, char *response) {

    if (tokens == NULL || count == 0 || response == NULL) return -1;

    int cmd = KVS_CMD_START;
    for (cmd = KVS_CMD_START; cmd < KVS_CMD_COUNT; cmd++) {
        if (strcmp(tokens[0], command[cmd]) == 0 ) {
            break;
        }
    }

    int len = 0;
    int ret = 0;
    char *key = tokens[1];
    char *value = tokens[2];
    char *result = 0;

    switch (cmd) {
#if ENABLE_ARRAY
        case KVS_CMD_SET:
            ret = kvs_array_set(&global_array, key, value);
            if (ret < 0) {
                len = sprintf(response, "set error\r\n");
            } else if (ret == 0) {
                len = sprintf(response, "ok\r\n");
            } else {
                len = sprintf(response, "exist\r\n");
            }
            break;

        case KVS_CMD_GET:{
            result = kvs_array_get(&global_array, key);
            if (result == NULL) {
                len = sprintf(response, "no exist\r\n");
            } else {
                len = sprintf(response, "%s\r\n", result);
            }
            break;
        }
        case KVS_CMD_DEL:
            ret = kvs_array_del(&global_array, key);
            if (ret < 0) {
                len = sprintf(response, "error\r\n");
            } else if (ret == 0) {
                len = sprintf(response, "ok\r\n");
            } else {
                len = sprintf(response, "no exist\r\n");
            }
            break;

        case KVS_CMD_MOD:
            ret = kvs_array_mod(&global_array, key, value);
            if (ret < 0) {
                len = sprintf(response, "error\r\n");
            } else if (ret == 0) {
                len = sprintf(response, "ok\r\n");
            } else {
                len = sprintf(response, "no exist\r\n");
            }
            break;
        case KVS_CMD_EXIST:
            ret = kvs_array_exist(&global_array, key);
            if (ret == 0) {
                len = sprintf(response, "exist\r\n");
            } else {
                len = sprintf(response, "no exist\r\n");
            }
            break;
#endif
        // rbtree
#if ENABLE_RBTREE
        case KVS_CMD_RSET:
            ret = kvs_rbtree_set(&global_rbtree, key, value);
            if (ret < 0) {
                len = sprintf(response, "set error\r\n");
            } else if (ret == 0) {
                len = sprintf(response, "ok\r\n");
            } else {
                len = sprintf(response, "exist\r\n");
            }
            break;

        case KVS_CMD_RGET:{
            char *result = kvs_rbtree_get(&global_rbtree, key);
            if (result == NULL) {
                len = sprintf(response, "no exist\r\n");
            } else {
                len = sprintf(response, "%s\r\n", result);
            }
            break;
        }
        case KVS_CMD_RDEL:
            ret = kvs_rbtree_del(&global_rbtree, key);
            if (ret < 0) {
                len = sprintf(response, "error\r\n");
            } else if (ret == 0) {
                len = sprintf(response, "ok\r\n");
            } else {
                len = sprintf(response, "no exist\r\n");
            }
            break;

        case KVS_CMD_RMOD:
            ret = kvs_rbtree_mod(&global_rbtree, key, value);
            if (ret < 0) {
                len = sprintf(response, "error\r\n");
            } else if (ret == 0) {
                len = sprintf(response, "ok\r\n");
            } else {
                len = sprintf(response, "no exist\r\n");
            }
            break;
        case KVS_CMD_REXIST:
            ret = kvs_rbtree_exist(&global_rbtree, key);
            if (ret == 0) {
                len = sprintf(response, "exist\r\n");
            } else {
                len = sprintf(response, "no exist\r\n");
            }
            break;
#endif
#if ENABLE_HASH
        case KVS_CMD_HSET:
            ret = kvs_hash_set(&global_hash ,key, value);
            if (ret < 0) {
                len = sprintf(response, "error\r\n");
            } else if (ret == 0) {
                len = sprintf(response, "ok\r\n");
            } else {
                len = sprintf(response, "exist\r\n");
            } 
            
            break;
        case KVS_CMD_HGET: {
            char *result = kvs_hash_get(&global_hash, key);
            if (result == NULL) {
                len = sprintf(response, "no exist\r\n");
            } else {
                len = sprintf(response, "%s\r\n", result);
            }
            break;
        }
        case KVS_CMD_HDEL:
            ret = kvs_hash_del(&global_hash ,key);
            if (ret < 0) {
                len = sprintf(response, "error\r\n");
            } else if (ret == 0) {
                len = sprintf(response, "ok\r\n");
            } else {
                len = sprintf(response, "no exist\r\n");
            }
            break;
        case KVS_CMD_HMOD:
            ret = kvs_hash_mod(&global_hash ,key, value);
            if (ret < 0) {
                len = sprintf(response, "error\r\n");
            } else if (ret == 0) {
                len = sprintf(response, "ok\r\n");
            } else {
                len = sprintf(response, "no exist\r\n");
            }
            break;
        case KVS_CMD_HEXIST:
            ret = kvs_hash_exist(&global_hash ,key);
            if (ret == 0) {
                len = sprintf(response, "exist\r\n");
            } else {
                len = sprintf(response, "no exist\r\n");
            }
            break;
    #endif

        default:
            assert(0);
    }

    return len;

}

/*
msg: request message
length:length of request msg
response: need to send
@return : len of response
协议解析：拆分命令 + 协议处理 = 返回响应
*/
int kvs_protocol(char *msg, int length, char *response) {

    if (msg == NULL || length <= 0 || response == NULL) return -1;
    // printf("kvstore.c recv %d: %s\n", length, msg);

    char *tokens[KVS_MAX_TOKENS] = {0};

    int count = kvs_split_token(msg, tokens);
    if (count == -1)return -1;

    // memcpy(response, msg, length);
    return kvs_filter_protocol(tokens, count, response);

}


int init_kvengine(void) {

#if ENABLE_ARRAY
    memset(&global_array, 0, sizeof(kvs_array_t));
    kvs_array_create(&global_array);
#endif
#if ENABLE_RBTREE
    memset(&global_rbtree, 0, sizeof(kvs_rbtree_t));
    kvs_rbtree_create(&global_rbtree);
#endif
#if ENABLE_HASH
	memset(&global_hash, 0, sizeof(kvs_hash_t));
	kvs_hash_create(&global_hash);
#endif

    return 0;
}

void dest_kvengine(void) {
#if ENABLE_ARRAY
    kvs_array_destory(&global_array);
#endif
#if ENABLE_RBTREE
    kvs_rbtree_destory(&global_rbtree);
#endif
#if ENABLE_HASH
	kvs_hash_destory(&global_hash);
#endif

}


// 对于一个服务而言，传入一个port，以及kvs_protocol就行了
// 底层网络完全抽象隔离，剩下就是改造网络框架成想要的
int main(int argc, char *argv[]) {

    if (argc != 2) return -1;

    int port = atoi(argv[1]);

    // 用于创建kv存储全局单例内存块global_array
    init_kvengine();

    // 功能入口函数，第二个参数为函数指针msg_handler，把kvs_protocol传入作为函数
    // 间接
#if (NET_SELECT == NET_REACTOR)
    reactor_start(port, kvs_protocol);
#elif (NET_SELECT == NET_NTYCO)
    ntyco_start(port, kvs_protocol);
#elif (NET_SELECT == NET_PROACTOR)
    proactor_start(port, kvs_protocol);

#endif
    dest_kvengine();
}




