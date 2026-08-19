CC = gcc
ECHO = echo
AR = ar
ARFLAGS = rcs

SUB_DIR = core/ sample/
ROOT_DIR = $(shell pwd)
OBJS_DIR = $(ROOT_DIR)/objs
BIN_DIR = $(ROOT_DIR)/bin
LIB_DIR = $(ROOT_DIR)/lib

BIN = nty_server nty_client nty_bench nty_server_mulcore nty_http_server nty_mysql_client nty_mysql_oper nty_websocket_server nty_http_server_mulcore ntyco_httpd nty_rediscli
LIB = libntyco.a

# 核心库的目标文件（这些是库的核心）
CORE_OBJS = $(OBJS_DIR)/nty_socket.o $(OBJS_DIR)/nty_coroutine.o $(OBJS_DIR)/nty_epoll.o $(OBJS_DIR)/nty_schedule.o

# 编译标志
FLAG = -lpthread -O3 -lcrypto -lssl -lmysqlclient -lhiredis -ldl -I $(ROOT_DIR)/core  -I /usr/include/mysql/ -I/usr/include/hiredis/

CUR_SOURCE = ${wildcard *.c}
CUR_OBJS = ${patsubst %.c, %.o, %(CUR_SOURCE)}

export CC BIN_DIR OBJS_DIR LIB_DIR ROOT_IDR FLAG BIN ECHO EFLAG AR ARFLAGS

all : $(SUB_DIR) $(LIB) $(BIN)
.PHONY : all

$(SUB_DIR) : ECHO
	make -C $@

ECHO :
	@echo $(SUB_DIR)

# 生成静态库
$(LIB): $(CORE_OBJS)
	mkdir -p $(LIB_DIR)
	$(AR) $(ARFLAGS) $(LIB_DIR)/$@ $^
	@echo "Static library $(LIB_DIR)/$@ created successfully"

# 可执行文件规则
nty_server : $(OBJS_DIR)/nty_socket.o $(OBJS_DIR)/nty_coroutine.o $(OBJS_DIR)/nty_epoll.o $(OBJS_DIR)/nty_schedule.o $(OBJS_DIR)/nty_server.o
	mkdir -p $(BIN_DIR)
	$(CC) -o $(BIN_DIR)/$@ $^ $(FLAG) 

nty_client : $(OBJS_DIR)/nty_socket.o $(OBJS_DIR)/nty_coroutine.o $(OBJS_DIR)/nty_epoll.o $(OBJS_DIR)/nty_schedule.o $(OBJS_DIR)/nty_client.o
	mkdir -p $(BIN_DIR)
	$(CC) -o $(BIN_DIR)/$@ $^ $(FLAG) 

nty_bench : $(OBJS_DIR)/nty_socket.o $(OBJS_DIR)/nty_coroutine.o $(OBJS_DIR)/nty_epoll.o $(OBJS_DIR)/nty_schedule.o $(OBJS_DIR)/nty_bench.o
	mkdir -p $(BIN_DIR)
	$(CC) -o $(BIN_DIR)/$@ $^ $(FLAG) 

nty_server_mulcore : $(OBJS_DIR)/nty_socket.o $(OBJS_DIR)/nty_coroutine.o $(OBJS_DIR)/nty_epoll.o $(OBJS_DIR)/nty_schedule.o $(OBJS_DIR)/nty_server_mulcore.o
	mkdir -p $(BIN_DIR)
	$(CC) -o $(BIN_DIR)/$@ $^ $(FLAG) 

nty_http_server : $(OBJS_DIR)/nty_socket.o $(OBJS_DIR)/nty_coroutine.o $(OBJS_DIR)/nty_epoll.o $(OBJS_DIR)/nty_schedule.o $(OBJS_DIR)/nty_http_server.o
	mkdir -p $(BIN_DIR)
	$(CC) -o $(BIN_DIR)/$@ $^ $(FLAG) 

nty_websocket_server : $(OBJS_DIR)/nty_socket.o $(OBJS_DIR)/nty_coroutine.o $(OBJS_DIR)/nty_epoll.o $(OBJS_DIR)/nty_schedule.o $(OBJS_DIR)/nty_websocket_server.o
	mkdir -p $(BIN_DIR)
	$(CC) -o $(BIN_DIR)/$@ $^ $(FLAG) 

nty_mysql_client : $(OBJS_DIR)/nty_socket.o $(OBJS_DIR)/nty_coroutine.o $(OBJS_DIR)/nty_epoll.o $(OBJS_DIR)/nty_schedule.o $(OBJS_DIR)/nty_mysql_client.o
	mkdir -p $(BIN_DIR)
	$(CC) -o $(BIN_DIR)/$@ $^ $(FLAG)

nty_mysql_oper : $(OBJS_DIR)/nty_socket.o $(OBJS_DIR)/nty_coroutine.o $(OBJS_DIR)/nty_epoll.o $(OBJS_DIR)/nty_schedule.o $(OBJS_DIR)/nty_mysql_oper.o
	mkdir -p $(BIN_DIR)
	$(CC) -o $(BIN_DIR)/$@ $^ $(FLAG)

nty_rediscli : $(OBJS_DIR)/nty_socket.o $(OBJS_DIR)/nty_coroutine.o $(OBJS_DIR)/nty_epoll.o $(OBJS_DIR)/nty_schedule.o $(OBJS_DIR)/nty_rediscli.o
	mkdir -p $(BIN_DIR)
	$(CC) -o $(BIN_DIR)/$@ $^ $(FLAG)

nty_http_server_mulcore : $(OBJS_DIR)/nty_socket.o $(OBJS_DIR)/nty_coroutine.o $(OBJS_DIR)/nty_epoll.o $(OBJS_DIR)/nty_schedule.o $(OBJS_DIR)/nty_http_server_mulcore.o
	mkdir -p $(BIN_DIR)
	$(CC) -o $(BIN_DIR)/$@ $^ $(FLAG) 

ntyco_httpd : $(OBJS_DIR)/nty_socket.o $(OBJS_DIR)/nty_coroutine.o $(OBJS_DIR)/nty_epoll.o $(OBJS_DIR)/nty_schedule.o $(OBJS_DIR)/ntyco_httpd.o
	mkdir -p $(BIN_DIR)
	$(CC) -o $(BIN_DIR)/$@ $^ $(FLAG) 

clean :
	rm -rf $(BIN_DIR)/* $(OBJS_DIR)/* $(LIB_DIR)/*

.PHONY: all clean