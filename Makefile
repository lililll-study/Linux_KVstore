
CC = gcc
TARGET = build/kvstore
TEST_TARGET = build/testcase
BUILD_DIR = build
SRC_DIR = src
TEST_DIR = test
INC_DIR = include

SRCS = $(SRC_DIR)/kvstore.c \
       $(SRC_DIR)/reactor.c \
       $(SRC_DIR)/kvs_array.c \
       $(SRC_DIR)/kvs_rbtree.c \
       $(SRC_DIR)/kvs_hash.c \
       $(SRC_DIR)/kvs_skiptable.c \
       $(SRC_DIR)/kvs_persist.c \
	   $(SRC_DIR)/resp.c \
	   $(SRC_DIR)/kvs_mempool.c \
	   $(SRC_DIR)/config.c \
       $(SRC_DIR)/kvs_sync.c

TEST_SRCS = $(TEST_DIR)/testcase.c


OBJS = $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

INC = -I$(INC_DIR) -I./NtyCo/core
LIBS = -lpthread -ldl
CFLAGS = -Wall -g $(INC)


# 默认目标
all: dirs $(TARGET) $(TEST_TARGET)

# 创建必要的目录
dirs:
	@mkdir -p $(BUILD_DIR) data

# ===== 链接服务端 =====
$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LIBS)
	@echo "Build complete: $(TARGET)"

# ===== 编译测试端（直接编译链接，不生成 .o） =====
$(TEST_TARGET): $(TEST_SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)
	@echo "Build complete: $(TEST_TARGET)"


# ===== 编译 src/ 下的 .c =====
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@


# ===== 运行目标 =====
# 启动主节点
run-master: all
	./$(TARGET) --config config/master.conf

# 启动从节点
run-slave: all
	./$(TARGET) --config config/slave.conf


# 清理
clean:
	rm -rf $(BUILD_DIR)/*.o $(TARGET) $(TEST_TARGET)
	@echo "Clean complete"

cleanall: clean
	rm -rf $(BUILD_DIR)
	@echo "Clean all complete"

.PHONY: all clean cleanall dirs run-master run-slave run-test run-test-batch