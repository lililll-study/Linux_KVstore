# RDMA技术应用

# 一、Situation

## 1.1 背景

        在多态服务器协同工作的时候，节点之间频繁交换数据，RDMA（remote direct memory access）远程直接访问内存技术，允许一台机器直接访问另一台机器的内存，而无需操作系统的接入。这种直接访问通过专门硬件——俗称为RDMA网卡来实现，数据传输过程绕过了 CPU 和内核协议栈，由网卡硬件直接完成数据的读取和写入操作，实现零拷贝的数据传输。

        零拷贝特性意味着应用程序的数据不需要在用户空间和内核空间之间反复复制。在传统网络通信中，数据从应用缓冲区到网卡发送缓冲区需要经过至少两次拷贝操作，而 RDMA 将这些拷贝完全消除。对于大规模的梯度同步操作，这节省了大量的内存带宽和 CPU 周期。

## 1.2 实现方式

        当前主要由三种方式实现：infiniBand是专为高性能计算设计的，提供极高的吞吐量和极低的延迟，但需要专用的交换机和网卡，成本较高。RoCE 即 RDMA over Converged Ethernet，允许在以太网上运行 RDMA 协议，其中 RoCE v2 基于 UDP/IP 可以跨三层网络路由，是目前数据中心的主流选择。iWARP 基于 TCP/IP 协议栈实现 RDMA，对网络设备要求较低但性能通常不如前两者。



**三大优势：**

——极低延迟

——极低CPU开销

——高带宽

**适用场景**：大数据、AI训练等数据密集型应用的需求



# 二、Task

    使用RDMA功能实现数据互传

使用RDMA实现文件互传：服务端如何确定文件的SIZE大小？

        将RDMA功能，应用到KVSTORE中的主从数据同步功能中。





# 三、Action

Linux操作流程：

1 创建SoftiWARP设备

sudo rdma link add siw1 type siw netdev ens33

2 打印ibv设备

ibv_devices

    会看到

device node GUID ------ ---------------- siw1 020c29fffe775881

3 启动服务

gcc -o rdma_server rdma_server.c -libverbs -lrdmacm

./rdma_server

gcc -o rdma_client rdma_client.c -libverbs -lrdmacm

./rdma_client



查看端口服务

netstat -anop | grep 9000



WARP代码实现流程：

server端

rdma_create_event_channel();

rdma_create_id();    创建通道和id，然后把id绑定一个本地IP

rdma_bind_addr(); 使得它进入listen的状态

rdma_listen();    ——rdma_wait_cm_event();

rdma_accept();    ——rdma_wait_cm_event();

相较于TCP server而言，TCP是阻塞在accept()处，而RDMA是阻塞在rdma_wait_cm_event处,也就是说，在RDMA中accept时，连接已经建立了。



把数据放进用户空间的buffer后，会mmap映射到内核中的mr区域，然后这块区域通过RDMA自动映射到其他机器中，这个过程是把数据放进去就自动映射的。

```c
typedef struct conn_manager_s {

    struct ibv_mr *recv_mr;
    struct ibv_mr *send_mr;

    char *rbuffer;
    char *sbuffer;
}
```

也就是我们把数据放到sbuffer中，对方的rbuffer就会收到数据。把数据放到rbuffer中，对方的sbuffer就会收到数据

rdma具有三个队列（queue）：sq、rq、cq，完成的数据都是放在cq中的，也就说rdma是一个异步的，类似io_uring，都是高性能io组件。

wr：work request  ——>sq、rq

wc：work complete——>cq





这里可以类比为，channel是epoll，然后id是fd，可以创建多个id，由channel进行统一管理

queuepair实际上是两个队列，send_q和recv_q



RDMA用于已有数据的同步，准确来说，主机是AOF模式时，只在第一次备份用RDMA同步，后续的实时同步暂时不变。

在SNAP中，SAVE保存的文件同步也用RDMA

当前的代码逻辑是：

先判断主机模式

主机SNAP：那么收到SYNC后，记录从机sockfd，g_snap_ctx.is_active置位，然后判断从机是否发送SAVE指令，收到SAVE后，调用

send_file_content把文件内容发送过去

主机AOF：收到SYNC后，g_aof_ctx.fd = fd; g_aof_ctx.is_active = 1;置位，然后直接把当前的AOF文件内容send_file_content发送到对端



# 四、Result



问题：

客户端发起

ibv_poll_cq: No space left on device
rdma_poll_wc: No space left on device

异步情况，客户端一直在发数据，但是服务端没取



解法：

文件发送时，可以一块一块的发送

而RDMA发送时，可以一次发一个G，本端mr开1G，对端mr就有1G，是一个内存共享的方式，不用考虑多次的方式，
