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

        将RDMA功能，应用到KVSTORE中的主从数据同步功能中。





# 三、Action



1 创建SoftiWARP设备

sudo rdma link add siw1 type siw netdev ens33

2 打印ibv设备

ibv_devices

    会看到

device node GUID ------ ---------------- siw1 020c29fffe775881

3 启动服务

gcc -o rdma_server rdma_server.c -libverbs -lrdmacm







# 四、Result
