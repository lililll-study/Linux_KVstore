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

* 数据结构：ARRAY + RBTREE + HASH + SKIPTABLE

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
git clone --recurse-submodules git@gitlab.0voice.com :lihongyi/kvs_lhy.git
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
./build/kvstore --config master.conf

从节点
./build/kvstore --config config/slave.conf
```



# 清理编译文件和数据

```bash
make clean
```

# 功能测试

## 管道写入

```bash
# 单条写入和管道写入的性能对比
./build/1_RESP_testcase 127.0.0.1 2000 5
```

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
./build/kvstore --config master.conf
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
./build/kvstore --config master.conf
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

# 性能数据

性能优化对比

| 优化项                | 优化前 QPS | 优化后 QPS | 提升倍数      | 备注               |
| ------------------ | ------- | ------- | --------- | ---------------- |
| 基础实现（单条写入）         | 1.2w    | -       | -         | 阻塞 I/O + 同步 AOF  |
| 管道化写入（批量）          | 1.4w    | 17.7w   | **12.6x** | 减少网络 RTT         |
| 管道 + 集中写入          | 1.4w    | 13.5w   | **9.6x**  | 批量发送，顺序处理        |
| AOF 异步写入（io_uring） | 1.4k    | 9.0w    | **64x**   | AOF 模式下的巨大提升     |
| 主从同步（管道模式）         | 17.1w   | 4.0w    | **0.23x** | AOF + 同步双开销      |
| 主从同步（单条模式）         | 1.4w    | 1.9w    | **1.36x** | RTT 主导，AOF 影响被掩盖 |

一、写入性能对比（无 AOF，无同步）
-------------------

### 混合写入（交叉模式）

| 引擎            | ① 单条混合写入 (QPS) | ③ 管道混合写入 (QPS) | 提升倍数      |
| ------------- | -------------- | -------------- | --------- |
| **Array**     | 1.2w           | 17.7w          | **14.5x** |
| **Rbtree**    | 1.5w           | 17.2w          | **11.2x** |
| **Hash**      | 1.4w           | 17.7w          | **12.6x** |
| **Skiptable** | 1.4w           | 15.9w          | **11.2x** |

### 集中写入模式

| 引擎            | ② 单条集中写入 (QPS) | ④ 管道集中写入 (QPS) | 提升倍数      |
| ------------- | -------------- | -------------- | --------- |
| **Array**     | 1.0w           | 3.2w           | **3.1x**  |
| **Rbtree**    | 1.7w           | 16.6w          | **10.0x** |
| **Hash**      | 1.4w           | 9.8w           | **7.1x**  |
| **Skiptable** | 1.3w           | 13.5w          | **10.1x** |

**结论**：管道模式下批量写入大幅提高性能，混合模式提升 11~15 倍，集中模式提升 3~10 倍。

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

**结论**：AOF + 主从同步对管道模式打击巨大（-72%~-77%），磁盘 IO 与网络同步成为绝对瓶颈；单条模式因 RTT 主导，开销被掩盖。**管道化提升倍数从 11~15x 降到 1.7~2.8x**，说明开启 AOF+同步后，管道化的优势被磁盘 IO 抵消。



## 四、CRC性能测试

| 引擎            | 开 CRC | 关 CRC      | 差值      | 倍数        |
| ------------- | ----- | ---------- | ------- | --------- |
| **Array**     | 6045  | **6454**   | +409    | 1.07×     |
| **Rbtree**    | 49067 | **149700** | +100633 | **3.05×** |
| **Hash**      | 24764 | **108225** | +83461  | **4.37×** |
| **Skiptable** | 23245 | **66050**  | +42805  | **2.84×** |



## 五、内存池性能测试

| VM/RSS (MB) | 无内存池        | 有内存池        | jemalloc       |
| ----------- | ----------- | ----------- | -------------- |
| 开始状态        | 19.7 / 1.9  | 25.1 / 7.6  | 39.8 / 4.2     |
| 峰值状态        | 62.5 / 45.1 | 80.6 / 62.9 | 71.6 / 33.8    |
| 结束状态        | 62.5 / 45.1 | 80.6 / 62.9 | 71.6 / 7.1     |
| 对比          | RSS复用       | RSS复用       | **RSS归还 -79%** |

##### 测试结果说明**内存表现 （40 万条数据，snap 模式）：**

        开始状态下，无内存池仅占 1.9MB RSS，内存池因首批块预付占 7.6MB，jemalloc 介于两者之间（4.2MB）。峰值状态下，jemalloc 以 33.8MB 最优——其精细的 size-class bin 使单位条目开销最低；内存池为 62.9MB，高出 glibc（45.1MB）约 39%，差额来自 8B/块的管理头与 25% 扩容余量，是池化确定性的结构成本。结束状态最能体现三者策略差异：glibc 与内存池的 RSS 均停留在峰值（前者小块释放无法触发堆收缩，后者主动保留空闲块供复用），唯有 jemalloc 将 RSS 归还 79%（33.8→7.1MB）。
