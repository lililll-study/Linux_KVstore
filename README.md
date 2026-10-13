* * *

# 基于C语言的 仿Redis数据库实现（KV存储）

* * *

# 项目目标

* 协议兼容：支持标准RESP协议，兼容redis-cli工具

* 高性能：单机QPS 5w+，持久化、同步及CRC校验功能开启后，仍然保持高性能

* 完整功能：数据引擎、持久化、主从备份、日志、数据校验

# 技术栈

* 语言：C/C++

* 网络：reactor/ntyco

* 协议：RESP

* 数据结构：HASH + SKIPTABLE

* 持久化：AOF + SNAP

* 构建：Makefile

* 数据落盘：io_uring

* 数据加载：mmap

* 内存管理：仿jemalloc实现的字符串分级内存池

# 环境要求

* Linux 5.1+（依赖 epoll / io _uring）
* gcc
* make
* git（构建 ntyco 模式时需要，用于自动拉取 submodule）

| 模式      | 网络架构               | 是否依赖 NtyCo submodule | 适用场景        |
| ------- | ------------------ | -------------------- | ----------- |
| reactor | 单线程 IO 多路复用（epoll） | 否                    | 通用部署，构建路径最短 |
| ntyco   | 同步写法 + 协程（NtyCo 库） | 是                    | 高并发连接场景     |

# 编译

## 方式一、reactor 模式（默认，无需克隆 NtyCo）

```bash
git clone git@gitlab.0voice.com:lihongyi/kvs_lhy.git
cd kvs_lhy

#或直接make
make reactor
```

## 方式二、ntyco 协程模式

克隆时导入

```bash
git clone --recurse-submodules git@gitlab.0voice.com:lihongyi/kvs_lhy.git
cd kvs_lhy
make ntyco
```

已经克隆reactor后导入

```bash
git submodule update --init --recursive
make ntyco
```



# 运行

```bash
主节点
./build/kvstore --config config/master.conf

从节点
./build/kvstore --config config/slave.conf
```

# redis-cli 操作

服务端支持标准 RESP 协议（redis-cli 扁平格式），可直接用 redis-cli 连接操作：

```bash
# 连接（默认端口 2000）
redis-cli -p 2000

# Hash 引擎（HSET 为 SET 语义：HSET <key> <value>）
HSET user 30        # 新插入 → :1；已存在不覆盖 → :0
HGET user           # 命中 → $3\r\n30；未命中 → $-1 (nil)
HEXIST user         # 存在 → :1；不存在 → :0
HMOD user 31        # 修改成功 → +OK；键不存在 → -ERR no such key
HDEL user           # 删除成功 → :1；不存在 → :0

# Skiptable 引擎（S 前缀命令）
SSET rank 1
SGET rank
SEXIST rank
SMOD rank 2
SDEL rank

# 持久化与同步
SAVE                # 仅 snap.enable=true 时生效，成功 → +OK
SYNC                # 从节点向主节点请求同步

# 健康检查
PING                # → +PONG

# 退出
quit
```

### 命令与响应对照

| 命令              | 格式                   | 成功            | 未命中/已存在            | 参数错误                          |
| --------------- | -------------------- | ------------- | ------------------ | ----------------------------- |
| HSET / SSET     | `HSET <key> <value>` | `:1`（新插入）     | `:0`（已存在不覆盖）       | `-ERR invalid arguments`      |
| HGET / SGET     | `HGET <key>`         | `$len<value>` | `$-1`（nil）         | -                             |
| HDEL / SDEL     | `HDEL <key>`         | `:1`（删除成功）    | `:0`（不存在）          | `-ERR invalid arguments`      |
| HMOD / SMOD     | `HMOD <key> <value>` | `+OK`         | `-ERR no such key` | `-ERR invalid arguments`      |
| HEXIST / SEXIST | `HEXIST <key>`       | `:1`（存在）      | `:0`               | -                             |
| SAVE            | `SAVE`               | `+OK`         | -                  | `-ERR SAVE only in SNAP mode` |
| SYNC            | `SYNC`               | -             | -                  | -                             |

> 说明：
> 
> * HSET/SSET 采用 **SET 语义**（`HSET <key> <value>`，两参数），与 Redis 原生的哈希字段语义不同；多余参数会被忽略。
> * 值中若含空格，请在 redis-cli 中用引号包裹，如 `HSET title "hello world"`。
> * 旧版嵌套格式（外层数组包多条命令）仍兼容，可在内部自测中继续使用。



# 清理编译文件和数据

```bash
make clean
```

# 功能测试

## 管道写入

```bash
# 性能对比测试（mode 2 = 单条集中写入，engine -1 = 全部引擎）
./build/1_RESP_testcase 2 -1
# 批量写入 + 校验（每引擎 BATCH_COUNT 条）
./build/1_RESP_testcase 5 -1
```

参数说明（`./build/1_RESP_testcase <mode> <engine>`）：

| 参数     | 说明                         |
| ------ | -------------------------- |
| mode   | 测试模式，见下表                   |
| engine | 0=Hash、1=Skiptable、-1=全部引擎 |

| mode | 说明                                      |
| ---- | --------------------------------------- |
| 1    | 单条收发 + 交叉写入（SET/GET/MOD/EXIST/DEL）      |
| 2    | 单条收发 + 集中写入                             |
| 3    | 管道化 + 交叉写入                              |
| 4    | 管道化 + 集中写入                              |
| 5    | 批量 SET/GET（每引擎 BATCH_COUNT 条）           |
| 6    | 插入 20w（2 引擎 × 10w）                      |
| 7    | 删除 20w（2 引擎 × 10w）                      |
| 10   | 错误命令测试（SSET → SMOD 缺参 → 未知命令 → SGET 校验） |

## 持久化

### 1、增量持久化

配置文件master.conf

```bash
# 主节点配置
role=master
port=2000
# 不启动snap
snap.enable=false
# 启动aof
aof.enable=true
```



测试

```bash
# AOF 测试
./build/2_presist_testcase 127.0.0.1 2000 1
# 插入完成后，重启服务端
./build/kvstore --config config/master.conf
# 测试端读取aof加载的数据
./build/2_presist_testcase 127.0.0.1 2000 2
```



### 2、全量持久化

配置文件master.conf

```bash
# 主节点配置
role=master
port=2000
# 启动snap
snap.enable=true
# 不启动aof
aof.enable=false
```



测试

```bash

# SNAP 测试
./build/2_presist_testcase 127.0.0.1 2000 3
# 插入完成后，重启服务端
./build/kvstore --config config/master.conf
# 测试端读取aof加载的数据
./build/2_presist_testcase 127.0.0.1 2000 4
```

## 主从同步

```bash
make clean
make reactor
# 1. 启动主机
./build/kvstore --config config/master.conf

# 测试端：连接主机并插入5w条数据
./build/3_sync_testcase 192.168.137.134 2000 1

# 2. 启动从机
./build/kvstore --config config/slave.conf


# 测试端：再向主机插入5w条数据
./build/3_sync_testcase 192.168.137.134 2000 2


# 测试端：连从机，验证 10w
./build/3_sync_testcase 192.168.137.135 2001 3
```

## 数据校验

```bash
make clean
make reactor
# 1. 启动主机
./build/kvstore --config config/master.conf

# 测试端：连接主机并插入5条数据
./build/3_sync_testcase 192.168.137.134 2000 10

# 2. 启动从机
./build/kvstore --config config/slave.conf


# 3. 对比主从机的crc列表
./build/5_crc_testcase 192.168.137.134 2000 192.168.137.135 2001 1


# 4. 修改从机数据
./build/5_crc_testcase 192.168.137.134 2000 192.168.137.135 2001 2

# 5. 再次对比 
./build/5_crc_testcase 192.168.137.134 2000 192.168.137.135 2001 1
```

## 内存池

```bash
# ===== 步骤 1：无内存池 =====
make clean 
make
./test/mem_test.sh nopool

# ===== 步骤 2：有内存池 =====
make clean 
make
./test/mem_test.sh pool

# ===== 步骤 3：jemalloc =====
# 需要关闭内存池
make clean
make
./test/mem_test.sh jemalloc
```

> `mem_test.sh` 内部通过 `1_RESP_testcase 6/7 -1` 完成 20w（2 引擎 × 10w）的插入与删除，对比内存占用。

内存池冒烟测试（插入 / 验证 / 删除各 4w，2 引擎 × 2w）：

```bash
# 连接 127.0.0.1:2000，mode 1=插入、2=验证、3=删除
./build/4_mempool_testcase 127.0.0.1 2000 1
./build/4_mempool_testcase 127.0.0.1 2000 2
./build/4_mempool_testcase 127.0.0.1 2000 3
```

# 性能数据

加入功能后的性能对比

| 添加功能       | 添加前 QPS                           | 添加后 QPS | 提升倍数      | 备注          |
| ---------- | --------------------------------- | ------- | --------- | ----------- |
| 基础实现（单条写入） | -                                 | 1.68w   | -         | 阻塞 I/O      |
| 管道化写入（批量）  | 1.68w                             | 11.9w   | **12.6x** | 减少网络 RTT    |
| AOF 异步写入   | 11.9w                             | 13.2w   | **6.4x**  | 异步AOF降低损耗   |
| 主从同步       | 13.2w | 4.0w    | **0.23x** | AOF + 同步双开销 |

一、写入性能对比（无 AOF，无同步）
-------------------

### 混合写入（交叉模式）

| 引擎            | ① 单条混合写入 (QPS) | ③ 管道混合写入 (QPS) | 提升倍数      |
| ------------- | -------------- | -------------- | --------- |
| **Hash**      | 1.4w           | 17.7w          | **12.6x** |
| **Skiptable** | 1.4w           | 15.9w          | **11.2x** |

### 集中写入模式

| 引擎            | ② 单条集中写入 (QPS) | ④ 管道集中写入 (QPS) | 提升倍数      |
| ------------- | -------------- | -------------- | --------- |
| **Hash**      | 1.4w           | 9.8w           | **7.1x**  |
| **Skiptable** | 1.3w           | 13.5w          | **10.1x** |

**结论**：管道模式下批量写入大幅提高性能，混合模式提升 11~13 倍，集中模式提升 7~10 倍。

## 二、开启 AOF 后的写入性能对比

-----------------

| 模式       | AOF 关闭 | AOF 开启 | 变化幅度       |
| -------- | ------ | ------ | ---------- |
| **管道插入** | 27.46w | 9.03w  | **-67.1%** |
| **单条插入** | 1.61w  | 1.57w  | **-2.3%**  |

**结论**：管道模式开启 AOF 后性能下降明显（-67%），单条模式几乎不受影响（-2.3%），但管道写入性能依旧领先。

## 三、主从同步性能测试（AOF + 同步开启）

----------------------

| 场景          | 无 AOF/同步 | 有 AOF+同步 | 变化       |
| ----------- | -------- | -------- | -------- |
| **管道 + 交叉** | 17.1w    | 4.0w     | **-77%** |
| **管道 + 集中** | 10.8w    | 3.0w     | **-72%** |
| **单条 + 交叉** | 1.4w     | 1.9w     | **+36%** |
| **单条 + 集中** | 1.4w     | 1.3w     | **-7%**  |

**结论**：AOF + 主从同步对管道模式打击巨大（-72%~-77%），磁盘 IO 与网络同步成为绝对瓶颈；单条模式因 RTT 主导，开销被掩盖。**管道化提升倍数从 11~13x 降到 1.7~2.8x**，说明开启 AOF+同步后，管道化的优势被磁盘 IO 抵消。



## 四、CRC性能测试

| 引擎            | 开 CRC | 关 CRC      | 差值     | 倍数        |
| ------------- | ----- | ---------- | ------ | --------- |
| **Hash**      | 24764 | **108225** | +83461 | **4.37×** |
| **Skiptable** | 23245 | **66050**  | +42805 | **2.84×** |



## 五、内存池性能测试

| VM/RSS (MB) | 无内存池        | 有内存池        | jemalloc       |
| ----------- | ----------- | ----------- | -------------- |
| 开始状态        | 19.7 / 1.9  | 25.1 / 7.6  | 39.8 / 4.2     |
| 峰值状态        | 62.5 / 45.1 | 80.6 / 62.9 | 71.6 / 33.8    |
| 结束状态        | 62.5 / 45.1 | 80.6 / 62.9 | 71.6 / 7.1     |
| 对比          | RSS复用       | RSS复用       | **RSS归还 -79%** |

##### 测试结果说明**内存表现 （20 万条数据，2 引擎 × 10 万，snap 模式）：**

        开始状态下，无内存池仅占 1.9MB RSS，内存池因首批块预付占 7.6MB，jemalloc 介于两者之间（4.2MB）。峰值状态下，jemalloc 以 33.8MB 最优——其精细的 size-class bin 使单位条目开销最低；内存池为 62.9MB，高出 glibc（45.1MB）约 39%，差额来自 8B/块的管理头与 25% 扩容余量，是池化确定性的结构成本。结束状态最能体现三者策略差异：glibc 与内存池的 RSS 均停留在峰值（前者小块释放无法触发堆收缩，后者主动保留空闲块供复用），唯有 jemalloc 将 RSS 归还 79%（33.8→7.1MB）。
