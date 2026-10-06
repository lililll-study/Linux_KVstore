#include "nty_coroutine.h"
#include "kvstore.h"
#include <arpa/inet.h>
#include <errno.h>

typedef int (*msg_handler)(int fd, char *msg, int length, char *response);
static msg_handler kvs_handler;


/**
 * 每个协程管理一个fd，功能是负责recv数据，分配给业务层函数，并send
 * 异步处理
 */
void server_reader(void *arg) {
	int fd = *(int *)arg;
    free(arg);
	int ret = 0;

 
	while (1) {
		
		char buf[1024] = {0};
        // 阻塞时自动让出协程，IO 就绪后恢复
		ret = recv(fd, buf, 1024, 0);
		if (ret > 0) {

            kvs_handler(fd, buf, ret, NULL);
            // 获取当前fd的上下文，并从resp_buffer中取出响应
            kvs_conn_ctx_t *ctx = &kvs_conn_ctxs[fd];
            // 如果响应缓冲区中有数据
            while (resp_buf_has_data(ctx)) {
                int data_len = 0;
                char *data = resp_buf_get_data(ctx, &data_len);
                if (data && data_len > 0) {
                    int n = send(fd, data, data_len, 0);   // ntyco hook 版 send
                    if (n <= 0) {
                        LOG_INFO("send failed: %d, %s\n", errno, strerror(errno));
                        close(fd);
                        cleanup_connection(fd);
                        return;
                    }
                    resp_buf_advance(ctx, n);
                }
            }
		} else if (ret == 0) {	
            LOG_INFO("client disconnect: %d\n", fd);
            close(fd);
            cleanup_connection(fd);
            break;
		} else {
            /* recv 错误 */
            LOG_INFO("recv error: %d, %s\n", errno, strerror(errno));
            close(fd);
            cleanup_connection(fd);
            break;
        }
	}
}


/**
 * fd注册服务
 * 在server协程中，循环接收sockfd，然后为每个fd分配一个协程
*/
void server(void *arg) {

	unsigned short port = *(unsigned short *)arg;

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return ;

    /* 端口复用 */
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

	struct sockaddr_in local, remote;
	local.sin_family = AF_INET;
	local.sin_port = htons(port);
	local.sin_addr.s_addr = INADDR_ANY;
	bind(fd, (struct sockaddr*)&local, sizeof(struct sockaddr_in));

	listen(fd, 20);
	// printf("listen port : %d\n", port);


	while (1) {
		socklen_t len = sizeof(struct sockaddr_in);
		int cli_fd = accept(fd, (struct sockaddr*)&remote, &len);
		
        // // 为每个连接初始化一个上下文
        // kvs_ctx_init(&kvs_conn_ctxs[cli_fd]);

        int *pfd = (int *)malloc(sizeof(int));
        *pfd = cli_fd;

		nty_coroutine *read_co;
		nty_coroutine_create(&read_co, server_reader, pfd);

	}
	
}



int ntyco_start(unsigned short port, msg_handler handler) {
    kvs_handler = handler;
    
    // 定义协程
    nty_coroutine *co = NULL;
    // 创建协程：管理fd注册项目
    nty_coroutine_create(&co, server, &port);
    // 允许调度器
    nty_schedule_run();
    return 0;
}