# ebpf技术详解

# 1、situation

bpf（berkeley packet filter）伯克利包过滤器。能够实现如下功能：

1. tcp连接的生命时长

2. MYSQL 执行一条语句的时间

3. Redis执行命令的时间

4. 对文件进行一次 读或者写的时间
   
   

这是一种探测技术



bpftrace语法结构：

tracepoint:syscalls:sys_enter_accept

/ comm == "networkio" /

{

        printf("accept %s\n", comm);

}

结构

event                    事件

/ condition /        条件

{

        action            动作

}



需要5.10以后的内核版本（Ubuntu 23.04）



传统的增量同步中，业务耦合的太深了，每加入一个新命令都要硬编码同步逻辑，而且同步逻辑是阻塞的，如果网络有波动，会拖慢主线程的处理速度。为了彻底解耦并提升性能，我在aof实时数据同步的基础上引入了ebpf uprobe技术，我是把ebpf程序直接挂载到append_aof函数上，相当于给函数加入一个无侵入的探针，只要调用了写aof，ebpf就能自动抓取命令并通过ringbuf异步给用户态再由用户态转发到从机





# 2、task



1 首先用ebpf的uprobe技术，完成对aof实时同步函数append_aof的监测，有一条数据写入aof，内核就跳去执行BPF程序，执行完再回到原函数。

2 然后把每条命令的cmd key value完整搬入用户态并逐条打印。

3 把ebpf作为kvstore的一部分，将uprobe挂载到append_aof函数上，原有的io_uring写入data_aof.aof逻辑不不变化。同时将探测到的命令写入内核ringbuf，由消费线程读取后，执行aof_sync_send_cmd()发送到从节点





# 3、action

可以分为 **内核态捕获** 和 **用户态消费与转发** 两条线：

一、内核态捕获流

1：首选需要创建一个内核态的捕获流(aof_capture.bpf.c)

2：然后主线程调用 append_aof()触发挂载在内核的uprobe

3：接着target_pid 取出目标的pid，如果PT_REGS_PARM1/2/3获取三个字符串指针（对应x86的rdi，rsi，rdx）

4：调用bpf_ringbuf_reserve申请固定大小的aof_event结构体。

5：使用bpf_probe_read_user_str将字符串从用户态拷贝到事件体，并计算真实长度，打上truncated标记

6：提交，调用bpf_ringbuf_submit，时间进入内核的环形队列等待用户态读取



二、用户态消费与转发

1. **启动与挂载**：`ebpf_capture_start` 被主节点调用，加载骨架（skeleton），绑定 PID，挂载 uprobe，并创建一个独立的轮询线程 `poll_thread`。

2. **拉取事件**：`poll_thread` 内部循环调用 `ring_buffer__poll(g_rb, 100)`，一旦有数据，回调 `handle_event`。

3. **异步缓存**：在 `handle_event` 中，通过 `master_aof_format_cmd` 将事件解析为标准的 RESP 协议，追加到 `g_batch` 缓冲区。

4. **批量化发送**：如果 `g_batch_len` 超过 64KB，或者轮询周期结束，就调用 `batch_flush()` 通过 `master_aof_send_raw` 发给从机。

5. **全量同步屏障**：当从机发起 `SYNC` 请求时：
   
   * 主节点先关闭 eBPF 转发（此时产生的命令会被丢弃，因为已经在全量里面了）。
   
   * 执行 `persistence_flush()` 保证 `io_uring` 里的尾巴落盘。
   
   * 将当前文件大小 `shot_size` 发给从机。
   
   * 发送完毕，再打开 eBPF 转发。



为了在面试中把“具体实现流程”讲清楚，你需要把这条链路讲成一个**完整的、有状态的、闭环的流水线**。不能只讲“抓到了、发过去”，还要讲清楚**从机连上来的前后，主节点是怎么无缝切换全量与增量的**，以及**怎么避免重复和漏发**。

以下是可以直接用于面试回答的**端到端具体实现流程**：

---

## 一、 启动与初始化阶段（`main` 与 `ebpf_capture_start`）

1. **拉起主进程**：`main` 函数加载配置文件，确认当前是主节点（`g_is_master = 1`），并初始化 `io_uring` 异步 AOF 写入引擎（`persistence_init()`）。
2. **启动 eBPF 捕获**：
   - 通过 `readlink("/proc/self/exe")` 获取自身的二进制路径。
   - 调用 `ebpf_capture_start(bin_path, getpid())`。
   - 内部流程：
     1. 调用 `aof_capture_bpf__open_and_load()` 把 BPF 字节码加载进内核。
     2. 更新 `target_pid` Map，填入当前进程 PID，用于内核态过滤。
     3. 通过 `bpf_program__attach_uprobe_opts()`，把 `on_append_aof` 程序挂载到 `<kvstore_binary>` 的 `append_aof` 符号上。
     4. 创建 `ring_buffer__new()` 消费者，并启动独立的轮询线程 `poll_thread`。
   - **关键点**：此时 `g_forward = 0`（旁路模式），增量转发尚未开启。

---

## 二、 正常运行阶段（内核捕获 + 用户态批处理）

每当主节点处理客户端写请求（如 `HSET k v`）时：

1. **业务层调用**：`kvstore.c` 执行完毕后调用 `append_aof("HSET", k, v)`，试图写入本地 AOF。
2. **内核探针触发（内核侧）**：
   - `on_append_aof` 被触发，读 `target_pid` 过滤，确认是自己。
   - 通过 `PT_REGS_PARM1/2/3`（`rdi/rsi/rdx`）取到 `cmd`、`key`、`value` 的用户态指针。
   - 调用 `bpf_ringbuf_reserve()` 申请固定大小的 `aof_event`，用 `bpf_probe_read_user_str()` 安全拷贝字符串。
   - 调用 `bpf_ringbuf_submit()` 提交事件。
3. **用户态消费（用户态侧）**：
   - `poll_thread` 执行 `ring_buffer__poll(g_rb, 100)`，回调 `handle_event()`。
   - `handle_event` 先判断 `g_forward`，**旁路期间直接丢弃**（比如正在做全量同步）。
   - 正常转发时，调用 `master_aof_format_cmd()` 把事件拼接成标准 RESP 协议（如 `*3\r\n$4\r\nHSET\r\n...`）。
   - 将 RESP 包追加到 `g_batch[64KB]` 缓冲区。**攒批策略**：如果超过 64KB 就调用 `batch_flush()` 一次性 `send()` 给从机；否则等本轮 `poll` 结束再 flush。
4. **从机接收（`slave_aof_recv_thread`）**：
   - 从机用 `recv_with_timeout` 收包，有数据就追加到本地 AOF 文件。
   - 每秒触发一次 `replay_aof_from()`，从本地 AOF 的 `consumed_offset` 往后逐条解析、执行命令，更新 `consumed_offset`。
   - 顺手发一个 `HEARTBEAT_CMD` 保活。

> 这个过程是**全异步**的：业务主线程继续处理客户端请求，eBPF 和 poll 线程负责吞吐同步，从机通过 TCP 或 RDMA 高速消费。

---

## 三、 从机发起 SYNC 时的“全量 + 增量无缝衔接”流程（核心逻辑）

当从机发起 `SYNC` 请求，主节点进入 `handle_sync_request(fd)`，这是这套系统**最容易出 bug 也最能体现功力**的地方。

### 1. 暂停增量转发（全量窗口开启）

```c
int ebpf_live = (g_config.ebpf_enable && ebpf_capture_is_running());
if (ebpf_live) {
    ebpf_capture_set_forward(0); // 旁路
    persistence_flush();         // ★ 强制刷盘：把 io_uring 队列中未落盘的 AOF 刷出去
}
```

- **目的**：此刻若继续转发 eBPF 事件，这些命令也会被塞在全量 AOF 里，从机就会**重复执行**。
- **`persistence_flush()` 必须做**：因为 `append_aof` 只是把写请求丢进 `io_uring` 队列，还没真正落盘。不刷盘直接 `stat` 文件大小，会导致全量快照漏掉还在内核队列里的命令，从机就**永久丢数据**。

### 2. 记录快照边界

```c
struct stat st;
long shot_size = 0;
if (stat(AOF_FILE, &st) == 0) shot_size = st.st_size;
```

- 只发送 `shot_size` 字节给从机，这就是全量数据的**绝对截止线**。

### 3. 发送全量（阻塞发送）

```c
if (send_file_content(fd, AOF_FILE, shot_size, "AOF_SYNC_OK\n") < 0) { ... }
```

- 发送指定的 `shot_size` 字节，末尾追加标记 `"AOF_SYNC_OK\n"`。
- 使用 RDMA 时走 `rdma_send_file()`，保证传输吞吐。
- 此时从机 `recv_file_content()` 边收边写本地 AOF，收满后截断标记，然后 `replay_aof_from(AOF_FILE, 0)` 全量加载到内存。
- 从机记下 `consumed_offset = shot_size`，**意味着接下来的增量从这里开始**。

### 4. 恢复增量转发（全量窗口关闭）

```c
if (ebpf_live) {
    ebpf_capture_set_forward(1);
}
```

- 此时主线程刚才**一直阻塞在这个发送函数里**，不会产生新命令。
- 恢复 `g_forward=1` 之后，新的 `append_aof` 会被 eBPF 捕获、被 `handle_event` 转发，从机只需要从 `consumed_offset` 开始追加即可，**既不重也不漏**。

---

## 四、 退出阶段（资源清理）

1. 主节点 `reactor_start()` 收到退出信号，跳出事件循环。
2. 调用 `ebpf_capture_stop()`：
   - 先 `batch_flush()` 刷掉残余缓冲。
   - 设置 `g_stop=1`，`pthread_join` 收割 `poll_thread`。
   - 打印统计：`[EBPF] 共捕获 X 条，转发 Y 条，丢弃 Z 条`（Z 从 `lost_count` map 读）。
   - 依次销毁 `ring_buffer`、`bpf_link`、`skel`。
3. 调用 `rdma_shutdown()` 拆 RDMA 连接。
4. 调用 `persistence_shutdown()` 和 `dest_kvengine()` 刷盘退出。

---

## 五、 面试时如何一图流总结（可以直接背）

> “整体流程可以总结为**两次状态切换，三条并行流水线**：
> 
> **两次状态切换：**
> 
> 1. **旁路模式 -> 转发模式**：主节点启动 eBPF 时 `g_forward=0`，等从机连上，先刷盘、做全量快照，发完再切 `g_forward=1`，避免全量与增量重复。
> 2. **转发模式 -> 旁路模式**：退出时先 flush 缓冲，再停线程，保证不留尾巴。
> 
> **三条并行流水线：**
> 
> 1. **内核捕获流**：uprobe → 过滤 PID → 读用户态字符串 → Ringbuf 入队；
> 2. **用户态处理流**：poll 线程收 Ringbuf → 拼 RESP → 64KB 攒批 → `send`/RDMA 出去；
> 3. **从机接收流**：`recv` → append 本地 AOF → 每秒 `replay_aof_from` 执行 → 心跳保活。
> 
> 这整个过程中，主业务线程完全不知道同步的存在，性能没有下降；从机通过 `consumed_offset` 精准追赶，保证主从数据严格一致。”





# 4、result





**面试官可能问：** “网络延迟或者 eBPF 丢包了怎么办？怎么保证不丢数据？”  
**你的回答：**

1. **Ringbuf 丢弃机制**：我在内核态维护了 `lost_count`。由于 `io_uring` 刷盘很快，同时我把 Ringbuf 设置了 64MB，大概率能扛住突发峰值。如果超过阈值，`lost_count` 会累加。

2. **监控告警**：用户态 `poll_thread` 会定时检查 `lost_count`，一旦发现被丢弃，立刻打印 `ERROR` 级别的日志：“Ringbuf 丢弃 X 条，请调大 Ringbuf 或降低写入速率”。

3. **网络发送缓冲**：如果发送失败（比如从机连接断了），`batch_flush()` 会返回错误，日志会记录，并且当前批次的数据不会被破坏。

4. **数据一致性保障**：在全量同步时，我特意调用 `persistence_flush()` 把内核队列里的写请求 flush 到文件，防止全量同步拿到的 `shot_size` 是残缺的，导致后续增量衔接不上。

**面试官：** “你的程序如果大 key、大 value 怎么办？”  
**你的回答：**  
“在 `aof_capture.h` 里我定义了 `AOF_CMD_MAX=16`, `AOF_KEY_MAX=256`, `AOF_VALUE_MAX=1024`。因为 Ringbuf 必须是固定长度的结构体，所以我用 `bpf_probe_read_user_str` 进行拷贝。如果超过了设定长度，该字段会被自动截断，同时结构体里的 `truncated` 标志位会被置 1。用户态收到后，如果是 `truncated==1`，会打印 WARN 日志，提示开发者。对于超大的 Value，截断意味着同步过去的数据是残缺的，需要业务层面使用大 Value 分块写入机制，或者调整 eBPF 结构体大小。”


