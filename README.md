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
./build/2_presist_testcase 127.0.0.1 2000 1
# 插入完成后，重启服务端
./build/kvstore --config master.conf
# 测试端读取aof加载的数据
./build/2_presist_testcase 127.0.0.1 2000 2
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
./build/2_presist_testcase 127.0.0.1 2000 3
# 插入完成后，重启服务端
./build/kvstore --config master.conf
# 测试端读取aof加载的数据
./build/2_presist_testcase 127.0.0.1 2000 4
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



# 数据校验

```bash
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



# 内存池

```bash
# ===== 步骤 1：无内存池 =====
sed -i 's/#define ENABLE_MEMPOOL.*/#define ENABLE_MEMPOOL 0/' include/kvstore.h
make clean && make
./test/mem_test.sh nopool

# ===== 步骤 2：有内存池 =====
sed -i 's/#define ENABLE_MEMPOOL.*/#define ENABLE_MEMPOOL 1/' include/kvstore.h
make clean && make
./test/mem_test.sh pool

# ===== 步骤 3：jemalloc =====
# 为了对比干净，jemalloc 应该测"无池 + jemalloc"
# 即：先编无池版，再 LD_PRELOAD jemalloc
sed -i 's/#define ENABLE_MEMPOOL.*/#define ENABLE_MEMPOOL 0/' include/kvstore.h
make clean && make
./test/mem_test.sh jemalloc
```



# 性能测试



## 1、CRC性能测试

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
二、开CRC对比数据

------

| 引擎            | 开 CRC（上次） | 关 CRC（本次）  | 差值      | 倍数        |
| ------------- | --------- | ---------- | ------- | --------- |
| **Array**     | 6045      | **6454**   | +409    | 1.07×     |
| **Rbtree**    | 49067     | **149700** | +100633 | **3.05×** |
| **Hash**      | 24764     | **108225** | +83461  | **4.37×** |
| **Skiptable** | 23245     | **66050**  | +42805  | **2.84×** |



## 2、内存池性能测试

| VM/RSS (MB) | 无内存池        | 有内存池        | jemalloc       |
| ----------- | ----------- | ----------- | -------------- |
| 开始状态        | 19.7 / 1.9  | 25.1 / 7.6  | 39.8 / 4.2     |
| 峰值状态        | 62.5 / 45.1 | 80.6 / 62.9 | 71.6 / 33.8    |
| 结束状态        | 62.5 / 45.1 | 80.6 / 62.9 | 71.6 / 7.1     |
| 对比          | RSS复用       | RSS复用       | **RSS归还 -79%** |



| 方案       | 插入QPS | 删除QPS |
| -------- | ----- | ----- |
| nopool   | 20032 | 45182 |
| pool     | 20834 | 52424 |
| jemalloc | 21149 | 48268 |

##### 测试结果说明

**内存表现 （40 万条数据，snap 模式）：**

        开始状态下，无内存池仅占 1.9MB RSS，内存池因首批块预付占 7.6MB，jemalloc 介于两者之间（4.2MB）。峰值状态下，jemalloc 以 33.8MB 最优——其精细的 size-class bin 使单位条目开销最低；内存池为 62.9MB，高出 glibc（45.1MB）约 39%，差额来自 8B/块的管理头与 25% 扩容余量，是池化确定性的结构成本。结束状态最能体现三者策略差异：glibc 与内存池的 RSS 均停留在峰值（前者小块释放无法触发堆收缩，后者主动保留空闲块供复用），唯有 jemalloc 将 RSS 归还 79%（33.8→7.1MB）。

**性能表现 ：**

        三种方案吞吐处于同一水平：插入 QPS 为 20032–21149（极差 5%），删除 QPS 为 45182–52424。内存池删除性能最高（52424），较 glibc 提升 16%，得益于池化分配免去 malloc 元数据查找；jemalloc 并未展现多线程场景的优势——单客户端串行负载下其无锁 arena 无用武之地。

**结论 ：**

        内存池在吞吐不劣于两种动态分配器的前提下（删除路径甚至最优），以峰值内存增加 39% 的代价，换取了 jemalloc 与 glibc 都无法提供的能力—— 启动时即可精确承诺内存上限、超限行为可定义、分配路径 O(1) 确定性 。三种方案的本质区别不在性能，而在内存策略：glibc 保留但不精细，jemalloc 激进归还（适合删多插少），内存池保留复用（适合删了又插、需要水位管控）。对内存上限有硬性要求的部署场景（嵌入式、多租户），池化是唯一选择；无此需求时 jemalloc 是综合最优解。
