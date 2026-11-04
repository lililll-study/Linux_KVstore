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

* 内存管理：仿jemalloc的分级内存池

* 已有数据同步：RDMA

* 实时数据同步：ebpf

# 环境要求

* Ubuntu2404以上版本，Linux 6.8+（依赖 epoll / io _uring / ebpf）
* gcc
* make
* git（构建 ntyco 模式时需要，用于自动拉取 submodule）

| 模式      | 网络架构               | 是否依赖 NtyCo submodule | 适用场景        |
| ------- | ------------------ | -------------------- | ----------- |
| reactor | 单线程 IO 多路复用（epoll） | 否                    | 通用部署，构建路径最短 |
| ntyco   | 同步写法 + 协程（NtyCo 库） | 是                    | 高并发连接场景     |



新系统需要安装的软件包

```bash
#make支持
sudo apt install make
#gcc编译器
sudo apt install -y build-essential
#rdma库
sudo apt install -y librdmacm-dev libibverbs-dev
```

开启RDMA功能需要绑定虚拟网卡

```bash
# 1 创建SoftiWARP设备
sudo rdma link add siw1 type siw netdev ens33
# 2 打印ibv设备
ibv_devices
会看到
device node GUID ------ ---------------- siw1 020c29fffe775881
```





# 编译

## 方式一、reactor 模式（默认，无需克隆 NtyCo）

```bash
git clone git@gitlab.0voice.com:lihongyi/kvs_lhy.git
cd kvs_lhy
make
```

## 方式二、ntyco 协程模式

克隆时导入

```bash
git clone --recurse-submodules git@gitlab.0voice.com:lihongyi/kvs_lhy.git
cd kvs_lhy
make ntyco
```

已经克隆reactor后，git导入协程子模块

```bash
git submodule update --init --recursive
make ntyco
```



# 运行

```bash
主节点
sudo ./build/kvstore --config master.conf

从节点
sudo ./build/kvstore --config slave.conf
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

## 一、管道写入

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
| 6    | 插入 100w（2 引擎 × 50w）                     |
| 7    | 删除 100w（2 引擎 × 50w）                     |
| 10   | 错误命令测试（SSET → SMOD 缺参 → 未知命令 → SGET 校验） |

## 二、持久化

### 1、增量持久化

配置文件master.conf

```bash
# 主节点配置
role=master
master_port=2000
master_ip=192.168.137.139
# snap全量同步
snap.enable=false
# aof增量同步：从节点一般不启动aof
aof.enable=true
# aof全文CRC校验开关
aof.crc=true
# RDMA开关：开启后SNAP文件和aof的初次数据同步走RDMA
rdma.enable=true
# eBPF开关：开启后aof的增量数据通过bpf捕获
ebpf.enable=true
# 内存泄漏检测：仅调试诊断用（有额外内存和性能开销）
leak.enable=false
# 日志级别：debug/info/warn/error/fatal（也可用数字0-4）
log.level=debug
```



测试：插入10w条数据（hash和跳表各5w）

```bash
# 启动主机
sudo ./build/kvstore --config master.conf

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
master_port=2000
master_ip=192.168.137.139
# snap全量同步
snap.enable=true
# aof增量同步：从节点一般不启动aof
aof.enable=false
# aof全文CRC校验开关
aof.crc=false
# RDMA开关：开启后SNAP文件和aof的初次数据同步走RDMA
rdma.enable=true
# eBPF开关：开启后aof的增量数据通过bpf捕获
ebpf.enable=true
# 内存泄漏检测：仅调试诊断用（有额外内存和性能开销）
leak.enable=false
# 日志级别：debug/info/warn/error/fatal（也可用数字0-4）
log.level=debug
```



测试：插入10w条数据（hash和跳表各5w）

```bash
# 启动主机
sudo ./build/kvstore --config master.conf

# SNAP 测试
./build/2_presist_testcase 127.0.0.1 2000 3
# 插入完成后，重启服务端
./build/kvstore --config config/master.conf
# 测试端读取snap加载的数据
./build/2_presist_testcase 127.0.0.1 2000 4
```

## 三、主从同步

```bash
make clean
make
# 1. 启动主机
sudo ./build/kvstore --config master.conf

# 测试端：连接主机并插入5w条数据
./build/3_sync_testcase 192.168.137.139 2000 1

# 2. 启动从机
sudo ./build/kvstore --config slave.conf


# 测试端：再向主机插入5w条数据
./build/3_sync_testcase 192.168.137.139 2000 2


# 测试端：连从机，验证 10w
./build/3_sync_testcase 192.168.137.140 2000 3
```

## 四、数据校验

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
./build/4_crc_testcase 192.168.137.134 2000 192.168.137.135 2001 1


# 4. 修改从机数据
./build/4_crc_testcase 192.168.137.134 2000 192.168.137.135 2001 2

# 5. 再次对比 
./build/4_crc_testcase 192.168.137.134 2000 192.168.137.135 2001 1
```

## 五、内存池

```bash
# ===== 步骤 1：无内存池 =====
make clean 
make
sudo bash ./test/5_mem_test.sh nopool

# ===== 步骤 2：有内存池 =====
make clean 
make
sudo bash ./test/5_mem_test.sh pool

# ===== 步骤 3：jemalloc =====
# 需要关闭内存池
make clean
make
sudo bash ./test/5_mem_test.sh jemalloc
```

> `5_mem_test.sh` 内部通过 `1_RESP_testcase 6/7 -1` 完成 100w（2 引擎 × 50w）的插入与删除，对比内存占用。





# 性能数据

## 1. 加入功能后的性能对比

| 添加功能       | 添加前 QPS | 添加后 QPS | 提升倍数      | 备注             |
| ---------- | ------- | ------- | --------- | -------------- |
| 基础实现（单条写入） | -       | 1.68w   | -         | 阻塞 I/O         |
| 管道化写入（批量）  | 1.68w   | 11.9w   | **7.1x**  | 减少网络 RTT       |
| AOF 异步写入   | 11.9w   | 13.2w   | **1.11x** | 异步AOF降低损耗      |
| 主从同步(aof)  | 13.2w   | 6.64w   | **0.51x** | AOF落盘+主从同步+crc |
| 主从同步(snap) | 11.9w   | 11.4w   | **0.96x** | 几乎不影响          |

-------------------

## 2. 内存池性能测试



| **VM/RSS (MB)** | **无内存池**      | **有内存池**      | **jemalloc** |
| --------------- | ------------- | ------------- | ------------ |
| **开始状态**        | 6.2 / 4.1     | 12.7 / 10.6   | 20.6 / 6.8   |
| **峰值状态**        | 120.3 / 118.3 | 142.5 / 140.5 | 111.6 / 92.3 |
| **结束状态**        | 118.3 / 116.3 | 140.5 / 8.6   | 111.6 / 13.4 |

-------------------



## 3、增量持久化--开启和关闭下的QPS测试

    --kvstore测试
    #  服务启动
    sudo ./build/kvstore --config master.conf
    #  测试端执行
    redis-benchmark -h 127.0.0.1 -p 2000 -c 1 -P 1 -n 100000 PING
    
    --redis测试
    # redis关闭aof
    redis-cli -p 6379 config set appendonly no
    # redis开启aof
    redis-cli -p 6379 config set appendonly yes
    #查看当前aof状态
    redis-cli -p 6379 config get appendonly
    #  测试端执行
    redis-benchmark -h 127.0.0.1 -p 6379 -c 1 -P 1 -n 100000 PING

| 测试项目           | 关闭aof的QPS | 开启aof的QPS |
| -------------- | --------- | --------- |
| kvstore的echo测试 | 8233.16   | 7795.45   |
| redis测试        | 10410.16  | 8277.46   |



-------------------



## 4、全量持久化--不同save间隔下的QPS测试

    # 共100w条数据,单条插入
    
    # 100W/SAVE
    ./build/2_presist_testcase 127.0.0.1 2000 13 h
    # 10W/SAVE
    ./build/2_presist_testcase 127.0.0.1 2000 12 h
    # 1W/SAVE
    ./build/2_presist_testcase 127.0.0.1 2000 11 h
    # 1k/SAVE
    ./build/2_presist_testcase 127.0.0.1 2000 10 h

| Hash引擎 | 100W/SAVE | 10W/SAVE | 1W/SAVE  | 1K/SAVE   |
| ------ | --------- | -------- | -------- | --------- |
| QPS    | 2.25w     | 2.17w    | 1.23w    | 0.436w    |
| 总耗时    | 44.422 s  | 46.068 s | 81.463 s | 229.316 s |

-------------------

## 5、批处理--QPS测试

共插10w条数据。

批量10条，20条，40条，80条，160条的qps

```
# 测试批量10条数据
redis-benchmark -h 127.0.0.1 -p 2000 -c 1 -P 10 -n 100000 -t ping -q

# 测试批量20条数据
redis-benchmark -h 127.0.0.1 -p 2000 -c 1 -P 20 -n 100000 -t ping -q

# 测试批量40条数据
redis-benchmark -h 127.0.0.1 -p 2000 -c 1 -P 40 -n 100000 -t ping -q

# 测试批量80条数据
redis-benchmark -h 127.0.0.1 -p 2000 -c 1 -P 80 -n 100000 -t ping -q

# 测试批量160条数据
redis-benchmark -h 127.0.0.1 -p 2000 -c 1 -P 160 -n 100000 -t ping -q
```



|        | 批量10条 | 批量20条  | 批量40条  | 批量80条  | 批量160条  |
| ------ | ----- | ------ | ------ | ------ | ------- |
| SNAP模式 | 7.84w | 16.81w | 32.79w | 53.48w | 105.26w |
| AOF模式  | 7.07w | 13.94w | 27.55w | 53.76w | 104.17w |


