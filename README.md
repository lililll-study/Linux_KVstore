# 使用说明

## 编译运行

```bash
cd kvs_lhy
make

运行项目
./build/kvstore --config master.conf

```



# 批处理



```bash
# 全部 QPS 测试（推荐）

./build/1_RESP_testcase 127.0.0.1 2000 5

# 或者单独跑
./build/1_RESP_testcase 127.0.0.1 2000 1   # 单条SET

./build/1_RESP_testcase 127.0.0.1 2000 2   # 单条 GET

./build/1_RESP_testcase 127.0.0.1 2000 3   # 批量 SET

./build/1_RESP_testcase 127.0.0.1 2000 4   # 批量 GET


错误格式检测
./build/1_RESP_testcase 127.0.0.1 2000 10（存在bug）
批量发送10w条set，最后一条99999号，会无法写入内存
```





# 持久化

## 1 增量持久化

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
./build/2_presist_testcase 192.168.137.128 2000 1
# 插入完成后，重启服务端
./build/kvstore --config master.conf
# 测试端读取aof加载的数据
./build/2_presist_testcase 192.168.137.128 2000 2
```



## 2 全量持久化

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
./build/2_presist_testcase 192.168.137.128 2000 3
# 插入完成后，重启服务端
./build/kvstore --config master.conf
# 测试端读取aof加载的数据
./build/2_presist_testcase 192.168.137.128 2000 4
```





# 主从同步



```bash
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





# 性能测试





不开主从同步，单机批量插入和批量查询：

```bash
lhy@lhy:~/share/kvs_lhy$ ./build/1_RESP_testcase 127.0.0.1 2000 3
===== 批量 SET 5w =====
[Batch  SET] Array      50000: 7675 ms, qps=6514
[Batch  SET] Rbtree     50000: 315 ms, qps=158730
[Batch  SET] Hash       50000: 616 ms, qps=81168
[Batch  SET] Skiptable  50000: 597 ms, qps=83752
lhy@lhy:~/share/kvs_lhy$ ./build/1_RESP_testcase 127.0.0.1 2000 4
===== 批量 GET 5w =====
[Batch  GET] Array      50000: 5867 ms, qps=8522
[Batch  GET] Rbtree     50000: 257 ms, qps=194552
[Batch  GET] Hash       50000: 562 ms, qps=88967
[Batch  GET] Skiptable  50000: 462 ms, qps=108225

```



开启主从同步（定时aof同步），批量插入和查询：

```bash

lhy@lhy:~/share/kvs_lhy$ ./build/1_RESP_testcase 127.0.0.1 2000 3
===== 批量 SET 5w =====
[Batch  SET] Array      50000: 7692 ms, qps=6500
[Batch  SET] Rbtree     50000: 274 ms, qps=182481
[Batch  SET] Hash       50000: 857 ms, qps=58343
[Batch  SET] Skiptable  50000: 852 ms, qps=58685
lhy@lhy:~/share/kvs_lhy$ ./build/1_RESP_testcase 127.0.0.1 2000 4
===== 批量 GET 5w =====
[Batch  GET] Array      50000: 5570 ms, qps=8976
[Batch  GET] Rbtree     50000: 253 ms, qps=197628
[Batch  GET] Hash       50000: 563 ms, qps=88809
[Batch  GET] Skiptable  50000: 434 ms, qps=115207
```

性能几乎不损失



---

# 基于C的仿Redis数据库实现（KV存储）

---

- 源码仓库见：

- 视频讲解：

---



# 项目说明

---

# 1 RESP协议设计

---

        为了支持任意字符的key和value，需要设计一个高可靠的数据解析协议。因此本项目仿照redis的RESP协议设计方案。

Redis 用 5 种类型前缀区分消息--CRLF 边界：

| 前缀  | 类型    | 示例                                                |
| --- | ----- | ------------------------------------------------- |
| `+` | 简单字符串 | `+OK\r\n`                                         |
| `-` | 错误    | `-ERR unknown command\r\n`                        |
| `:` | 整数    | `:10\r\n`                                         |
| `$` | 批量字符串 | `$5\r\nhello\r\n`                                 |
| `*` | 数组    | `*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n` |

客户端请求统一用 **Inline Command**（`SET key value\r\n`）格式。

---

        代码实现上，由于之前采用strtok根据空格分割SET KEY VALUE，因此需要改造为resp parse() 协议解析器。整体实现流程如下：

```c
recv_cb接收数据

kvs_Ctx_append(cmd_buffer)把数据存入缓冲区

kvs_ctx_process()
循环处理cmd_buffer中的数据，只要cmd_buffer_len不为0，就交给resp_parse解析

resp_parse()
从cmd_buf中拿出数据，执行下面解析步骤：
1. 命令都以*2\r\n格式开头，数字表示本次传入的cmd数量
2. 然后开始解析第一个元素，以*3\r\n格式开头，代表第一个元素cmd的token个数
3. 然后解析每个元素，以$3\r\n开头，\r\n结尾，内容长度由开头数字表示
4. 每解析完一个元素，即count>0, 就调用kvs_filter_protocol执行，否则继续解析



命令1: SET a 1
RESP: *3\r\n$3\r\nSET\r\n$1\r\na\r\n$1\r\n1\r\n

命令2: GET a
RESP: *2\r\n$3\r\nGET\r\n$1\r\na\r\n


// 解析器收到的数据
*2\r\n                          // ← 外层数组，2个元素
*3\r\n $3\r\nSET\r\n $1\r\na\r\n $1\r\n1\r\n  // ← 元素1：数组
*2\r\n $3\r\nGET\r\n $1\r\na\r\n             // ← 元素2：数组

// 解析结果
外层数组: [cmd1, cmd2]
  cmd1: ["SET", "a", "1"]
  cmd2: ["GET", "a"]

// 执行
for each cmd in 外层数组:
    execute(cmd)
```

        用这种数组嵌套式写法，即使kv中存在\r\n也不会报错，因为解析根据校验头$len来判定kv的长度。



# 2 协程和IO_uring应用

## 基于coroutine的网络框架



## 基于uring的持久化数据落盘

问题

```bash
原先的aof数据落盘：
每次 SET/MOD/DEL 都会同步执行文件 I/O，每条写命令 open → fprintf → close，高频同步阻塞
```

改造方法

```bash
把aof改为持久化请求队列，append_aof()变为：
构造 AOF record    // 定义一条 AOF 记录
      ↓
加入 queue
      ↓
唤醒 persistence thread
      ↓
io_uring 提交


真正做法：做到高并发异步 I/O，实现批量提交
       ┌── request 1 ──┐
       ├── request 2 ──┤
queue ─┼── request 3 ──┼→ SQ
       ├── request 4 ──┤
       └── request 5 ──┘
                       ↓
                  kernel I/O
                       ↓
       ┌── CQE 1 ──────┐
       ├── CQE 2 ──────┤
       ├── CQE 3 ──────┤
       └── CQE 4 ──────┘


而且需要保证aof的顺序，不能让多个业务线程随意直接操作同一个ring，故uring只由一个线程负责
```



修改业务流程

业务请求线程只负责把 AOF 数据放进内存队列，不直接执行磁盘写；后台持久化线程负责批量提交 io_uring。

```c
客户端
  ↓
ntyco 协程
  ↓
kvs_handler()
  ↓
SET / DEL / MOD
  ↓
append_aof()
  ↓
内存 FIFO 队列
  ↓
Persistence Worker
  ↓
io_uring SQ
  ↓
IORING_OP_WRITE
  ↓
AOF
```

同时需要确保SAVA的一致性：

```c
SAVE
 ↓
persistence_flush()
 ↓
等待所有 AOF 写完成
 ↓
save_snapshots()
 ↓
清空 AOF

```



设计方案

```bash
kvs_uring_t          ← 对外实例（含 worker 线程、队列、计数）
    └── kvs_ring_t   ← io_uring 裸封装（mmap 区域指针）
            └── io_uring_sqe / io_uring_cqe  ← 内核数据结构


业务线程: kvs_uring_append → 构造 req → 入队 → signal(cond)
                                                  ↓
worker线程: cond_wait → 批量取 64 个 → submit_requests
              → ring_get_sqe × N → ring_submit → reap_one × N
              → free_req → completed++
```



## 基于mmap的持久化数据加载

当前代码的 `load_snapshots()` 和 `replay_aof()` 都是 `fopen + fgetc/fread，效率低

改造方案

```c
AOF/SNAP 文件
      ↓
open()
      ↓
fstat()
      ↓
mmap(PROT_READ, MAP_PRIVATE)
      ↓
内存中的 char *data
      ↓
基于指针 offset 顺序解析
      ↓
KV 引擎恢复
      ↓
munmap()
```



代码实现

1 mmap把文件映射到进程虚拟地址空间

要映射到虚拟地址空间的原因：CPU不能直接读磁盘文件，所以需要先把磁盘内容加载进内存。传统方式：read拷贝。内核从磁盘读文件到page cache（物理内存），然后把数据从pc拷贝进用户态缓冲区，CPU通过用户态缓冲区的虚拟地址访问数据。

    问题是从磁盘→pc，再从pc->用户态空间，需要两次拷贝。

mmap的方式：直接映射，char *p = mmap(...);内核建立虚拟地址->pc的页表映射，CPU通过p的虚拟地址直接访问pc，少了一次拷贝。关键：<mark>p是虚拟地址，页表把p映射到page cache的物理页，cpu访问p的时候直接访问pagecache。</mark>

```c
mf->data = mmap(NULL, mf->size, PROT_READ, MAP_PRIVATE, mf->fd, 0);
         让内核自己选映射地址、映射多少字节、只读、只读场景写时复制

一次拷贝
磁盘
  │ ① DMA 拷贝（磁盘 → page cache）
  ↓
page cache
  │ ② 页表映射（虚拟地址 → page cache）
  ↓
进程虚拟地址空间（p）
  │ ③ CPU 访问
  ↓
CPU 读到数据


两次拷贝
磁盘
  │ ① DMA 拷贝（磁盘 → page cache）
  ↓
page cache
  │ ② CPU 拷贝（page cache → 用户态 buf）
  ↓
用户态 buf
  │ ③ CPU 访问
  ↓
CPU 读到数据
```





# 3 config配置

通常情况下全量和增量只会选择一个开启，因此需要把全量同步和增量同步功能独立出来，然后通过config文件进行配置，比如说./kvstore --config master.conf启动主节点，./kvstore --config slave.conf启动从节点。



主从备份：

主节点

```c
# 主节点配置
role = master
port = 2000


# snap快照同步
snap.enable=true
# 从节点一般不启动aof
aof.enable=true
```



从节点

```c
# 从节点配置
role=slave
port=2001
master_ip=192.168.137.128
master_port=2000

# snap快照同步
snap.enable=true
# 从节点一般不启动aof
aof.enable=false
```



# 4 持久化

## 增量持久化



## 全量持久化



# 5 主从同步

为什么要主从同步，使用场景：

| 目的       | 说明              |
| -------- | --------------- |
| **高可用**  | 主机挂了，从机顶上       |
| **读扩展**  | 读请求分发到从机，减轻主机压力 |
| **数据备份** | 从机是主机数据的副本      |
| **异地容灾** | 从机在异地，主机故障时接管   |
| **数据分析** | 从机跑分析任务，不影响主机   |

**Redis、MySQL、Kafka 都有主从**。



## 同步流程

SAVE指令只在snap模式下有效

aof有两种同步方式：

1：每5s同步一次（通过xxljob等定时平台调用）

2：立即同步    （每个写操作立即同步）

snap有一种同步方式：

执行一次save同步一次（使用频率低）



开启从机，自动连接主机，连接后发送sync。主机收到sync，把当前的持久化模式发送给从机。

从机收到mode，如果是aof，就把命令接收完后批量执行，然后执行save（从机为snap模式）。如果是snap，就把data直接写入snap。

主机操作：

如果主机是aof模式，如果是定时同步，就会检测aof文件，有增加就发送。如果是立即同步，每个写操作就立即同步。

如果主机是snap模式，那么从机每10s发一次save命令，把snap文件全量发送。



### master

作为主机可选aof或snap



### slave

作为从机，一般选择snap，此时不知道主机的模式，只管接收主机来的数据。



如果a发生宕机，那么b需要选择开启aof或snap，然后重启。这时，功能和主节点相同。

当a恢复后，b重启并执行cmd把数据同步给a。



## 数据校验

也就是说，写数据在主机上，需要给写入的数据加入CRC校验值，原因

1：能避免传输过程中出现的数据错误问题

2：能够校验数据是否被修改



实现流程：

1：A每次同步给B数据，会在本地维护一个CRC列表 crc_list_a，并会记录同步到B的数据量

2：A宕机后，B接管，B同样维护crc_list_b

3：A恢复后，从B同步数据，先对比备份内容的crc_list，如果有不同，会把这些数据罗列出来，并暂停插入，同时把不同的数据写入临时文件中

4：如果检查完无误后，执行命令在把临时文件的插入主机data。





# 6 日志等级

日志格式：

```c
[时间] [级别] [文件:行] 消息

[2024-01-01 12:00:00] [INFO] [kvstore.c:100] AOF started, offset=0
[2024-01-01 12:00:01] [ERROR] [kvs_persist.c:200] open data/data_aof.aof failed: No such file

时间格式：YYYY-MM-DD HH:MM:SS
```



另外在config中加入一个是否为调试状态的开关

打开后，终端有输出


