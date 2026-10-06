
CC = gcc
TARGET = build/kvstore
BUILD_DIR = build
SRC_DIR = src
TEST_DIR = test
INC_DIR = include

# 测试用例列表（每个独立编译）
TEST_SRCS = $(TEST_DIR)/5_crc_testcase.c \
			$(TEST_DIR)/3_sync_testcase.c \
			$(TEST_DIR)/2_presist_testcase.c \
			$(TEST_DIR)/1_RESP_testcase.c \
            $(TEST_DIR)/aof_snap_test.c

# ===== 每个测试用例对应的可执行文件 =====
TEST_TARGETS = $(patsubst $(TEST_DIR)/%.c,$(BUILD_DIR)/%,$(TEST_SRCS))

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
	   $(SRC_DIR)/ntyco.c \
	   $(SRC_DIR)/log.c \
	   $(SRC_DIR)/crc.c


OBJS = $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

INC = -I$(INC_DIR) -I./NtyCo/core
# LIBS = -lpthread -ldl
LIBS = -L ./NtyCo/lib/ -lntyco -lpthread -ldl
# 测试端只链接 -lpthread -ldl，不链接 -lntyco
TEST_LIBS = -lpthread -ldl
CFLAGS = -Wall -g $(INC)


# 持久化数据文件
DATA_DIR  = data
AOF_FILE  = $(DATA_DIR)/data_aof.aof
SNAP_FILE = $(DATA_DIR)/data_snap.snap
CRC_AOF   = $(DATA_DIR)/crc_aof.dat
CRC_SNAP  = $(DATA_DIR)/crc_snap.dat

# 日志文件
LOG_DIR  = logs
LOG  = $(LOG_DIR)/kvs.log

# 默认目标：服务端 + 所有测试端
all: dirs $(TARGET) $(TEST_TARGETS)

# 创建必要的目录
dirs:
	@mkdir -p $(BUILD_DIR) data


# ===== 链接服务端 =====
$(TARGET): $(OBJS)
	$(CC) -no-pie -o $@ $(OBJS) $(LIBS)
	@echo "Build complete: $(TARGET)"

# ===== 编译测试端：链接 config.o（读配置用） =====
$(BUILD_DIR)/%: $(TEST_DIR)/%.c $(BUILD_DIR)/config.o
	$(CC) -no-pie $(CFLAGS) -o $@ $< $(BUILD_DIR)/config.o $(TEST_LIBS)
	@echo "Build complete: $@"


# ===== 编译 src/ 下的 .c =====
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@


# ===== kvs_sync.o 专用规则：绕过 NtyCo hook =====
# 注意：必须放在通用规则之后，Make 后定义的规则优先
$(BUILD_DIR)/kvs_sync.o: $(SRC_DIR)/kvs_sync.c
	$(CC) $(CFLAGS) -DSYNC_RAW_IO -c $< -o $@
	@echo "Build complete (raw IO): $@"



clean:
	rm -rf $(BUILD_DIR)
	@mkdir -p $(DATA_DIR)
	@: > $(AOF_FILE)      # 清空 AOF
	@: > $(SNAP_FILE)     # 清空 SNAP
	@: > $(CRC_AOF)       # 清空 CRC-AOF
	@: > $(CRC_SNAP)      # 清空 CRC-SNAP
	@: > $(LOG)      	  # 清空 log
	@echo "Data and log files cleared:"
	@ls -l $(AOF_FILE) $(SNAP_FILE) $(CRC_AOF) $(CRC_SNAP) $(LOG)
	@echo "Clean complete"

.PHONY: all clean cleanall dirs