
CC = gcc
TARGET = build/kvstore
TESTCASE_TARGET = build/testcase
SNAP_AOF_TARGET = build/snap_aof_test
BUILD_DIR = build
SRC_DIR = src
TEST_DIR = test
INC_DIR = include

#       $(SRC_DIR)/reactor.c 
SRCS = $(SRC_DIR)/kvstore.c \
       $(SRC_DIR)/kvs_eg_array.c \
       $(SRC_DIR)/kvs_eg_rbtree.c \
       $(SRC_DIR)/kvs_eg_hash.c \
       $(SRC_DIR)/kvs_eg_skiptable.c \
       $(SRC_DIR)/kvs_persist.c \
	   $(SRC_DIR)/uring.c \
	   $(SRC_DIR)/resp.c \
	   $(SRC_DIR)/kvs_mempool.c \
	   $(SRC_DIR)/config.c \
	   $(SRC_DIR)/reactor.c \
       $(SRC_DIR)/kvs_sync.c \
	   $(SRC_DIR)/ntyco.c 


OBJS = $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

INC = -I$(INC_DIR) -I./NtyCo/core
# LIBS = -lpthread -ldl
LIBS = -L ./NtyCo/lib/ -lntyco -lpthread -ldl
# 测试端只链接 -lpthread -ldl，不链接 -lntyco
TEST_LIBS = -lpthread -ldl
CFLAGS = -Wall -g $(INC)

# 默认目标
all: dirs $(TARGET) $(TESTCASE_TARGET) $(SNAP_AOF_TARGET)

# 创建必要的目录
dirs:
	@mkdir -p $(BUILD_DIR) data


# ===== 链接服务端 =====
$(TARGET): $(OBJS)
	$(CC) -no-pie -o $@ $(OBJS) $(LIBS)
	@echo "Build complete: $(TARGET)"

# ===== 编译测试端 1：testcase =====
$(TESTCASE_TARGET): $(TEST_DIR)/testcase.c
	$(CC) -no-pie $(CFLAGS) -o $@ $< $(TEST_LIBS)
	@echo "Build complete: $(TESTCASE_TARGET)"

# ===== 编译测试端 2：snap_aof_test =====
$(SNAP_AOF_TARGET): $(TEST_DIR)/snap_aof_test.c
	$(CC) -no-pie $(CFLAGS) -o $@ $< $(TEST_LIBS)
	@echo "Build complete: $(SNAP_AOF_TARGET)"


# ===== 编译 src/ 下的 .c =====
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@


# 清理
clean:
	rm -rf $(OBJS) $(TARGET) $(TESTCASE_TARGET) $(SNAP_AOF_TARGET)
	@echo "Clean complete"

cleanall: clean
	rm -rf $(BUILD_DIR)
	@echo "Clean all complete"

.PHONY: all clean cleanall dirs $(SUBDIR)