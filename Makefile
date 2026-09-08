
CC = gcc
TARGET = kvstore
BUILD_DIR = build
SRC_DIR = src
INC_DIR = include

SRCS = $(SRC_DIR)/kvstore.c \
       $(SRC_DIR)/reactor.c \
       $(SRC_DIR)/kvs_array.c \
       $(SRC_DIR)/kvs_rbtree.c \
       $(SRC_DIR)/kvs_hash.c \
       $(SRC_DIR)/kvs_skiptable.c \
       $(SRC_DIR)/kvs_persist.c \
	   $(SRC_DIR)/kvs_batch.c \
	   $(SRC_DIR)/kvs_mempool.c \
       $(SRC_DIR)/kvs_sync.c

OBJS = $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

INC = -I$(INC_DIR) -I./NtyCo/core
LIBS = -lpthread -ldl
CFLAGS = -Wall -g $(INC)


# 默认目标
all: dirs $(TARGET)

# 创建必要的目录
dirs:
	@mkdir -p $(BUILD_DIR) data

# 链接
$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LIBS)
	@echo "Build complete: $(TARGET)"

# 编译 .c -> .o
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) $(DEFINES) -c $< -o $@

# 清理
clean:
	rm -rf $(BUILD_DIR)/*.o $(TARGET)
	@echo "Clean complete"

# 完全清理
cleanall: clean
	rm -rf $(BUILD_DIR)
	@echo "Clean all complete"


# 运行（主节点）
run: $(TARGET)
	./$(TARGET) 2000 --master

# 运行（从节点）
run-slave: $(TARGET)
	./$(TARGET) 2001 --slave

.PHONY: all clean cleanall dirs run run-slave