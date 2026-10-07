# 一、背景

（1）学完了网络要把所有知识点串起来，就要用到这个项目。知识点如下

0. 一请求一线程

1. select、poll、epoll

2. ntyco

3. dpdk封装TCP/IP

4. IO_Uring

5. iocp

        kv存储主要构建的是key-value的键值对存储，而MySQL中存的是标准的表格，每一条数据都有，是关系型数据库。**而kv是用key去查找value**。

1. redis

2. MongoDB

3. Memcached

        **既然有了这些方案为什么要做kv存储呢？**

1. 把业务做简单，就做几个功能（两个功能），发现自己做也能驾驭

2. 把性能做到极致，对比性能和redis
   
   

**（3）kv store怎么用？**

        比如在抖音，有抖音ID，对应用户名，两者有关联关系。或者在图床中，用链接访问png图片，形成了短连接和长连接的映射关系。

        在淘宝的商品链接很长，但是点分享后会有一个很短的字符，你放入浏览器就可以访问这个商品，就是短链接和长连接的映射。

        这是一个基础设施，用起来是无感知的。

        再比如说webserver实现一个问卷调查，访问前端发来问卷，可能做了30分钟，然后再发回去，服务端保存了Sation，问卷有一个cookie，两者对应。

        是一种数据组织的方式。



分包和粘包情况：

为什么会造成这种情况？  
原因是 TCP 的流式传输特性。

具体到 底层，粘包 / 分包的出现，主要来自三个环节：  
**1. 发送端：TCP 的优化机制导致粘包**

* Nagle 算法：TCP 为了减少网络小包的传输开销，会默认开启 Nagle 算法 —— 它会把多个小的send请求攒起来，等数据量达到 MSS（最大报文段长度），或者等之前的包收到 ACK 确认后，再一次性发出去。你连续 3 次send小数据包，TCP 可能一次就全发出去了，自然就粘包了。
* 发送缓冲区机制：调用send的时候，并不是直接把数据发到网络上，只是把数据拷贝到了内核的 TCP 发送缓冲区里。什么时候发、一次发多少，是 TCP 内核协议栈说了算，不是你调用几次send就发几次。

**2. 接收端：缓冲区读取机制导致粘包 + 分包**

* 粘包成因：TCP 收到的数据，会先放到内核的接收缓冲区里。如果你的业务代码读取不及时，缓冲区里就会攒下客户端多次发送的数据包，你调用一次recv，就会把缓冲区里所有能读的数据都读出来，自然就粘包了。
* 分包成因：你调用recv的时候，传入的 buffer 大小是有限的（比如你定义了buffer[1024]），如果此时缓冲区里有 2000 字节的数据，你一次只能读 1024 字节，剩下的 976 字节只能下次recv再读，一个完整的包就被拆分了，也就是分包。

**3. 网络层：MTU 限制导致分包**  
以太网的 MTU（最大传输单元）默认是 1500 字节，如果你发送的数据包超过了 MTU 大小，IP 层会把这个数据包拆分成多个分片传输，TCP 会在接收端重组这些分片。但如果分片还没全部到达接收缓冲区，你就调用了recv，就只能读到部分数据，导致分包。

在应用层自己定义一个规则，即定义一个消息边界，让接收端能准确区分 “一个完整的消息从哪开始、到哪结束”。  
**方案一：固定包头 + 包体**  
给每个业务消息，都定义一个固定长度的包头，包头里只放一个字段：包体的长度。通用格式：[固定长度的包头][业务包体]

* 包头：通常用 2 字节 或 4 字节，根据你的业务最大消息长度选择；

* 包体：就是你的实际业务数据（比如 KV 的 SET/GET 命令）。  
  那对于`SET Key Value\r\n`，包体长度是 15 字节，用 2 字节包头的话，完整的消息就是：
    [15][SET Key Value\r\n]

**方案二：特殊分隔符方案**  
加一个业务中绝对不会出现的特殊分隔符，接收端通过找分隔符来区分消息边界。  
最典型的就是 HTTP 协议，用\r\n\r\n作为请求头的结束符；还有很多文本型的命令行协议，用\r\n（换行符）作为每条命令的结束符。

# 二、拆分功能

1. 独立运行（独立的进程）

2. 通过网络访问（通过node访问，两者间是基于TCP的），会有短连接和长连接的映射关系

<mark>不用公开的库，用自己封装的库来做，用reactor.c</mark>



✅️设计模式

从下面的代码中可以看出，

1. 过滤器模式：可以在来到数据的时候解析，然后分发给特定的接口。

2. 观察者模式：来了事件通知三个接口，丢给每个处理。

```c
#if 0 // echo
    conn_list[fd].wlength = conn_list[fd].rlength;
    memcpy(conn_list[fd].wbuffer, conn_list[fd].rbuffer, conn_list[fd].wlength);
    printf("[%d]recv: %s\n", conn_list[fd].rlength, conn_list[fd].rbuffer);

#elif ENABLE_HTTP
// 接受完数据后做request请求
    http_requset(&conn_list[fd]);

#elif ENABLE_WS
    ws_request(&conn_list[fd]);

#elif ENABLE_KVS

    kvs_request(&conn_list[fd]);
```

能使用reactor网络的框架完成数据的收发，后续的开发功能就和网络没关系了。

1. reactor来做底层的网络框架

2. 用协程ntyco来做：把解析放在server_reader的while循环中，用协程封装的底层非阻塞API来recv

3. io_uring的做法

4. iocp也可以

底层的网络框架是可以跨平台的



当前代码：

    main -> 网络框架里

    协议处理 -> kvstore.c

请注意，主循环与业务相关，所以main应该放入kvstore.c中



作业：在Windows上兼容iocp



通信协议设计：解决tcp分包与粘包的问题

1. recv();

            先接收的就是对方先发送的（保证接收顺序）；接收完的数据中间不会出现丢数据（保证数据必达性）

            因此在数据包前面加上包长

            每次先接收2个字节的数据，然后再接收剩余的数据

```c
连续接收两次
short length = 0;
recv(fd, &length, 2, 0);
recv(fd, buffer, length, 0);
```

2. ET和LT都能做这个功能，但是对方可能一次发多个包，两者分别怎么实现？

3. redis的话，拆分为token分发，比如发GET Teacher

发2\r\n -> 3\r\n -> GET\r\n -> 7\r\n -> Teacher\r\n,那就需要自己实现readline，然后把\r\n去掉。

首先tokens=atoi(buffer)，然后循环tokens次readline



# 三、代码设计

## 1 kv存储+reactor实现

迭代的过程 产生架构

代码都是分层设计的



自己封装malloc这些，使得不直接使用系统调用。



## 2 kv引擎实现

### 2.1 测试用例实现

1. tcp客户端,建立连接
2. 发送协议,
3. 接收服务端返回的数据
4. 预期数据 与 服务端返回数据对比。
   
   

测试QPS时，发现效果。当关闭打印信息后，qps大幅提升，说明IO操作是非常耗时的：

```bash
带打印：
lhy@ubuntu:~/share/linux_first/12_1$ ./testcase 192.168.137.128 2000
array_testcase -- > time used: 2604, qps: 3456
不带打印
lhy@ubuntu:~/share/linux_first/12_1$ ./testcase 192.168.137.128 2000
array_testcase -- > time used: 49, qps: 183673
lhy@ubuntu:~/share/linux_first/12_1$ ./testcase 192.168.137.128 2000
array_testcase -- > time used: 51, qps: 176470

```

作业： 客户端如何实现多线程、多进程，来提高qps

### 2.1 引擎实现

接口统一，测试方案统一

```c
引擎层        array    rbtree    skiptable (5cmd + 2(create&des))


接口层        5+2        5+2        5+2

适配层        适配       适配        适配接口层

核心层                    kvstore

协议层        协议设计  - SET GET DEL MOD EXIST


网络层        reactor ntyco io_uring
```

编译命令

```bash
gcc -o kvstore kvstore.c reactor.c kvs_array.c
```



⚠️

面试题：

1. kvs为什么不用多线程而是用单线程？
* 因为kv存储本身就是解决的在内存中快速读写共享数据这一场景，单线程能更好地利用更低的复杂度换来很高的性能和更低的延迟。更深入来说就是进行一次单线程的kv存储的cpu周期很短，并发带来的总管理成本比一次kv存储本身还贵。
1. 如果要实现范围查询，需要用什么数据结构实现kv引擎？

rbtree，b+ tree

可以用红黑树来实现范围查询，因为当插入的是有序数据时，红黑树可以通过中序遍历来实现范围查询。

```c



// 函数调用：range_query(root, 30, 70)

// 步骤1：在根节点判断
// root = user:50，在范围内 [30, 70] ✅
// 继续遍历左右子树

// 步骤2：遍历左子树 (user:30)
// node = user:30，在范围内 [30, 70] ✅
// 继续遍历左右子树

// 步骤3：遍历 user:30 的左子树 (user:10)
// node = user:10，小于30 ❌
// 只遍历右子树（因为左子树更小）

// 步骤4：遍历 user:10 的右子树
// user:10 没有右子树

// 步骤5：处理 user:30
// user:30 在范围内 ✅ 收集 "C"

// 步骤6：遍历 user:30 的右子树 (user:40)
// user:40 在范围内 ✅ 收集 "D"

// 步骤7：回到根节点 user:50
// user:50 在范围内 ✅ 收集 "F"

// 步骤8：遍历右子树 (user:70)
// user:70 在范围内 ✅ 收集 "H"

// 步骤9：遍历 user:70 的右子树 (user:90)
// user:90 大于70 ❌
// 不收集，停止

// 结果：["C", "D", "F", "H"]
// 对应 keys: [30, 40, 50, 70]
```

1. 为什么会用到kvs_malloc，频繁的使用kvs_malloc，kv存储的内存管理如何做？（内存池会讲）
   
   

# 四、作业



## 1 跳表引擎实现

### 1.1 为什么要跳表，而不是使用二分查找？

因为二分查找的前提是，数据必须存储在数组中，并且始终保持有序和不变。数组要求底层是连续内存，并且插入和删除的复杂度都是O(n)。

而跳表可以实现所有操作在动态数据的基础上实现，并且时间复杂度都是O(log n)，且分布在分散的内存上。

**一句话：二分查找需要数组支持，数组插入太慢；跳表用链表 + 多层索引，实现了同样快的查找，但插入和删除快得多。**

### 1.2 跳表代码实现

本次跳表的实现主要涉及到：跳表的定义，接口层适配函数，接口函数。

定义跳表包括定义跳表中的每个节点，以及跳表的表结构体

跳表节点包括key, value, forward。forward[i]用来表示每个节点在第i层的下一个节点（二级指针）

跳表表结构包括跳表层级level和头节点

关键点：增删改查

1. 节点设计

2. 跳表设计

3. 随机层高

4. 实现查询、插入、删除
   
   

#### （1）节点设计

这里把每一层的对应节点的后继节点，设计为二级指针forward，因为每个节点node是一个指针，而指向每个指针就设计为二级指针。

```c
typedef struct kvs_node {
    char *key;
    char *value;
    struct kvs_node** forward;
} kvs_node_t;
```



#### （2）跳表设计

每个跳表的头节点（哨兵节点），拥有最高层数，并且不存储数据。

```c
typedef struct kvs_skiptable {
    int level;
    kvs_node_t* header;
} kvs_skiptable_t;

头节点 (header):
┌─────────────────────────────┐
│ key = NULL                  │
│ value = NULL                │
│ forward[0] → 第一个实际节点  │ (第0层)
│ forward[1] → 第1层的第一个节点│
│ forward[2] → 第2层的第一个节点│
│ forward[3] → 第3层的第一个节点│
│ forward[4] → 第4层的第一个节点│
│ forward[5] → 第5层的第一个节点│
│ forward[6] → 第6层的第一个节点│
└─────────────────────────────┘
```

// 定义跳表最大层数，每个节点可以出现在多个层级中，6表示最高允许建到第五层

// 这样查找时最多跳跃6次就能找到目标

// 一般取log2(n)作为最大层数，比如100w节点，log2(100w)=20



#### （3）随机层高

随机生成跳表节点的层数

每个节点至少在第0层，抛硬币决定是否增加层数。

<mark>生成0到max之间的随机数，小于max/2的概率是50%（redis是1/4）</mark>

```c
int random_level() {
    int level = 0;
    while ((rand() < RAND_MAX/2) && (level < MAX_LEVEL)) {
        level ++;
    }
    return level;
}
```



#### （4）节点创建和释放

三元运算符，如果`key != NULL`，调用 `strdup(key)` 复制字符串。

这里不直接复制key的原因是，能够使得newnode拥有独立的内存，使得key被修改，node的key不会被修改，内存安全。

```c
kvs_node_t* createNode(int level, char *key, char *value) {
    kvs_node_t* newNode = (kvs_node_t*)malloc(sizeof(kvs_node_t));
    newNode->key = key ? strdup(key) : NULL;
    newNode->value = value ? strdup(value) : NULL;
    newNode->forward = (kvs_node_t**)malloc((level + 1) * sizeof(kvs_node_t*));
    return newNode;
}

void free_node(kvs_node_t *node) {
    if (node == NULL) return;

    if (node->key) kvs_free(node->key);
    if (node->value) kvs_free(node->value);
    if (node->forward) kvs_free(node->forward);
    kvs_free(node);
}
```



（5）搜索，插入，删除，修改

搜索：自顶层向下搜索

```c
kvs_node_t* search(kvs_skiptable_t* skipList, char* key) {
    if (skipList == NULL || key == NULL) return NULL;

    kvs_node_t* current = skipList->header;
    int i = 0;

    for (i = skipList->level; i >= 0; --i) {
        while (current->forward[i] != NULL && strcmp(current->forward[i]->key, key) < 0)
            current = current->forward[i];
    }

    current = current -> forward[0];

    if(current && strcmp(current->key, key) == 0) {
        // printf("Key %d found with value %d\n", key, current->value);
        return current;
    }else{
        // printf("Key %d not found\n", key);
        return NULL;
    }
}


int kvs_skiptable_mod(kvs_skiptable_t *inst, char *key, char *value) {
    if (inst == NULL || key == NULL || value == NULL) return -1;

    // 先检查key是否存在
    if (search(inst, key) == NULL) return 1; // key不存在

    // update表示在第i层，新节点应该插入在哪个节点之后
    // 1. 初始化查找update和current
    kvs_node_t *update[MAX_LEVEL + 1];
    kvs_node_t *current = inst->header;

    int i = 0;
    // 从最高层向下寻找要插入的位置
    // 2. 查找插入位置
    for (i = inst->level; i>=0; i--) {
        // 在每一层向右移动，直到找到key大于等于当前key的位置
        while (current->forward[i] != NULL && strcmp(current->forward[i]->key, key) <0) {
                current = current->forward[i];
            }
            update[i] = current;// 记录每层的前驱节点
    }
    // 找到位置
    current = current->forward[0];

    // 3. 更新value
    if (current == NULL || strcmp(current->key, key) != 0) {
        // 释放旧的value 替换为新的
        return -1;
    }
    kvs_free(current->value);
    current->value = strdup(value);
    return 0;
}


```

插入：

在比较的时候，需要把每一层的待插入位置的前驱结点update记录下来，逻辑是：当current->forward[i]比key大，那么就把当前current保存到update[i]，后续只要插入在update后面即可。然后找到current在第0层的位置。

然后判断level是不是比当前跳表层数高，如果新节点层数 > 当前跳表最高层，那么需要把update数组补齐，把新增的层的头节点分配给前驱update

最后插入新节点，从底层开始遍历，新节点指向update的下一个节点，然后把新节点赋值给update的下一个节点

```c
int kvs_skiptable_set(kvs_skiptable_t *inst, char *key, char *value) {
    if (inst == NULL || key == NULL || value == NULL) return -1;

    if (search(inst, key) != NULL) return 1; // key已经存在了


    // update表示在第i层，新节点应该插入在哪个节点之后
    // 1. 初始化查找update和current
    kvs_node_t *update[MAX_LEVEL + 1];
    kvs_node_t *current = inst->header;
    int i = 0;
    // 从最高层向下寻找要插入的位置
    // 2. 查找插入位置
    for (i = inst->level; i>=0; i--) {
        // 在每一层向右移动，直到找到key大于等于当前key的位置
        while (current->forward[i] != NULL && strcmp(current->forward[i]->key, key) <0) {
                current = current->forward[i];
            }
            update[i] = current;// 记录每层的前驱节点
    }
    current = current->forward[0];


    // 4. 先分配层数
    int level = random_level();
    if (level > inst->level) {
        // 如果插入层数高于当前跳表层数，补齐update
        for (i = inst->level +1; i<=level; i++) {
            update[i] = inst->header;
        }
        inst->level = level;
    }
    // 5. 创建并插入节点
    // 创建新节点
    kvs_node_t *new_node = createNode(level, key, value);
    if (new_node == NULL) return -1;

    // 在各层插入新节点
    for (i=0; i<=level; i++) {
        new_node->forward[i] = update[i]->forward[i]; 
        update[i]->forward[i] = new_node;
    }
    return 0;
}
```



删除

先搜索节点是否存在，然后初始化前驱的数组，接着在各层把current删除，并释放current，最后更新跳表层数

```c
int kvs_skiptable_del(kvs_skiptable_t *inst, char *key) {
    if (inst == NULL || key == NULL) return -1;

    // 先检查key是否存在
    if (search(inst, key) == NULL) return 1; // key不存在

    kvs_node_t *update[MAX_LEVEL + 1];
    kvs_node_t *current = inst->header;
    int i = 0;

    // 初始化 update 数组
    for (i = 0; i <= MAX_LEVEL; i++) {
        update[i] = NULL;
    }

    // 查找要删除的节点
    for (i = inst->level; i >= 0; i--) {
        while (current->forward[i] != NULL && 
               strcmp(current->forward[i]->key, key) < 0) {
            current = current->forward[i];
        }
        update[i] = current;
    }

    // 移动到目标节点
    current = current->forward[0];

    // 在各层删除节点
    for (i = 0; i <= inst->level; i++) {
        if (update[i] != NULL && update[i]->forward[i] == current) {
            update[i]->forward[i] = current->forward[i];
        }
    }

    // 释放节点
    free_node(current);

    // 更新跳表层数（如果最高层没有节点了，降低 level）
    while (inst->level > 0 && 
           inst->header->forward[inst->level] == NULL) {
        inst->level--;
    }

    return 0;
}
```



关键实现：找到目标节点key

        从最高节点的头开始，每层向右查询。如果curr-key比forward-key大，那么就把curr向右移动一格。

        如果移动到比curr-key大的节点，或者为null，则退出while，并记录这个curr到update。

        直到找到最底层，最后把最底层的curr后继赋值给curr，即为找到的key位置。

```c
获取头节点
kvs_node_t *curr = inst->header;
for (i=inst->level; i>=0; i--) {
    从最高层向下遍历
    while (curr->forward[i] != NULL && strcmp(curr->forward[i]->key, key)<0) {
    curr = curr->forward[i];
    }
    update[i] = curr;
}
curr = curr->forward[0];
```

forward：每个节点通过forward指向各层的下一个节点

update：记录新节点一个插入在哪些节点之后







## 2 持久化存储

分为全量存储和增量存储。全量存储用快照snap的形式，使用命令行调用SAVE时，把所有引擎的数据遍历存储进磁盘文件中，下一次启动引擎时加载读取。增量存储记录set、del、mod成功的命令，并把命令写入磁盘文件。如果执行SAVE则删除aof，没有执行则在启动加载全量数据后，再把增量操作执行。这样就能恢复到上一次关闭前的数据状态。

全量：

红黑树遍历：中序遍历

```c

红黑树结构（二叉搜索树）：
        B
       / \
      A   C
         / \
        B   D

中序遍历顺序：左子树 → 当前节点 → 右子树
结果：A, B, C, D（按 key 排序）

递归思路：
1. 先递归遍历左子树（所有比当前节点小的）
2. 保存当前节点
3. 再递归遍历右子树（所有比当前节点大的）
```



哈希遍历

```c

哈希表结构：
nodes[0]: → node1 → node2 → NULL    ← 桶0的链表
nodes[1]: → NULL                     ← 空桶
nodes[2]: → node3 → NULL            ← 桶2的链表
nodes[3]: → node4 → node5 → node6 → NULL  ← 桶3的链表（冲突）
...

遍历思路：
外层循环：遍历所有桶（0 到 max_slots-1）
内层循环：遍历每个桶的链表（处理哈希冲突）

```



跳表遍历

```c
只要沿着第0层走一遍就行了
从头结点的forward[0]开始，依次访问
```



## 3 内存碎片管理

### 3.1 碎片存在的原因

内存碎片会存在内部碎片和外部碎片

        内部碎片指的是在分配内存时，因为<mark>内存对齐</mark>的原因，导致使用到的内存只占用了分配内存的一部分，<mark>使得另一部分未使用的处于空闲状态被浪费。</mark>例如：<mark>使用malloc(17)分配了17字节，但是由于内存对齐可能分配了32字节，剩余的15字节被浪费。</mark>

        <mark>外部碎片指的是分配的内存块之间存在空闲内存间隙，这些间隙无法满足新的内存请求，尽管总的空闲内存量看起来可能足够满足40字节的请求，但由于空闲块是零散分布且大小都小于40字节，所以无法满足这一分配需求，从而形成了外部碎片。</mark>



### 3.2 内存池的设计思路：

        将内存拆分为一个一个chunk块，然后用链表连接起来。然后在每个块的前8个字节用来存储下一个空闲块的地址。（注意这8个字节不算入可用空间）

然后在调用不同存储引擎时，会将数据存入不同内存池，做到内存池分类。因为每种数据的大小都是不同的，做到分区管理。而且系统大小的对象做到连续释放，能够提高缓存命中率。



**问题：**

1、为什么使用8个字节？

**CPU 访问对齐内存更快**，特别是指针操作。如果块地址不是 8 的倍数，CPU 需要两次内存访问才能读取一个 8 字节的指针。



2、这里的内存设计如何支持任意大小KV的？

在kvs_mempool_alloc中，会先计算用户可用空间，如果本次请求大小超过了块的容量，那么直接用malloc，不会出现溢出情况。

好处：小数据用池（快），大数据用 malloc（灵活）



### 3.3 内存池代码实现



```c
typedef struct mempool_s{
    size_t block_size;       // 每个内存块的总大小
    size_t total_blocks;    // 内存池总块数
    size_t free_blocks;     // 当前空闲块数量
    void* free_list;        // 空闲链表头指针
    char data[];        // 柔性数组
} mempool_t;
```

```c
    size_t actual_block_size = user_block_size + sizeof(void *); // 块大小
    size_t pool_size = sizeof(mempool_t) + actual_block_size * num_blocks; // 池大小

```

sizeof(void *)：表示内存块在空闲的时候，需要用前8个字节来记录下一个空闲块在哪。



**假设要创建5个块，每个块能用100字节：**

第一步，计算大小

```c
actual_block_size = 100 + 8 = 108  // 每个块实际大小
pool_size = sizeof(mempool_t) + 108 * 5  // 管理结构 + 5个块

malloc 后内存布局：
┌─────────────┬────────┬────────┬────────┬────────┬────────┐
│ mempool_t   │ 块0    │ 块1    │ 块2    │ 块3    │ 块4    │
│ (管理信息)   │ 108B   │ 108B   │ 108B   │ 108B   │ 108B   │
└─────────────┴────────┴────────┴────────┴────────┴────────┘
     ↑           ↑
   pool      pool->data 指向这里
```



第二步，初始化链表

1 找到 block[i]的起始地址

2 在block[i]的前8字节写入pool->free_list（i=0时写入null）

3 让free->list指向block[i]

```c
size_t i = 0;
    for (i=0; i<num_blocks; ++i) {
        // 找到 block[i]的起始地址
        void* block = pool->data + i * actual_block_size; 
        // 得到块起始地址处存储的内容 ,这一段就是把pool->free_list的值（开始为null）写入块起始地址
        *((void**)block) = pool->free_list; // 此时block0存储null
        pool->free_list = block;    // free_list = block0
    }
```

循环下去，空闲列表如下，每个空闲块的前8字节都存着下一个空闲块的地址:

```c
块2: [块1的地址] → 块1: [块0的地址] → 块0: [NULL]
free_list → 块2 → 块1 → 块0 → NULL
```



第三步，分配块malloc

1 检查pool是否有效，计算用户可用容量

2 小对象继续，大对象用malloc

3 从空闲表，取出链表第一个块

4 读取块的前8字节， 得到下一个空闲块

5 返回用户可用的地址（跳过前8字节）



第四步，释放块

1 计算块起始地址：block = (char*)ptr - sizeof(void*)

2 判断 ptr 是否在池内：在池内 → 放回空闲链表 ： 不在池内 → 直接free

3 放回空闲链表 (头插法)：// 块指向旧链表头 -> 块成为新链表头





## 4 批处理

实现思路：

<img src="file:///U:/kvs_lhy/screen/32badd17-31a1-48a8-b9f8-2eaae02585d4.png" title="" alt="32badd17-31a1-48a8-b9f8-2eaae02585d4" style="zoom:50%;"><img src="file:///U:/kvs_lhy/screen/e71cda89-0188-4642-a99a-c56fcd1b0f0c.png" title="" alt="e71cda89-0188-4642-a99a-c56fcd1b0f0c" style="zoom:50%;">

        主要做法是，输入端命令需要有\n结尾，作为识别符。底层网络回调接收批量数据后，kvs业务层把传入的数据直接append进一块内存缓冲区cmd_buf，然后循环处理缓冲区中的数据。包括查找\n符号分割cmd，接着把解析到的命令存入cmd_temp_buf这个临时缓冲区。如果有完整命令，就调用协议解析处理。如果没有，就等下一次触发接收。cmd_buf用于累计所有数据，cmd_temp_buf用于累计单条命令，处理完后立即释放。

        如果遇到长命令，if (!line_end) break;则说明没有检测到完整行，然后计算命令长度int cmd_len = line_end - line_start + 1，把数据复制到cmd_temp_buf，最后退出循环，下一次接着用cmd_temp_buf中的数据。

9-4号流程2点解决该问题，因为服务端和客户端的LINUX内核wmem都是1024到2048，所以当客户端一次send大批量数据到server会出现分包发送的情况，大量数据还没到达服务端，在客户端的内核发送缓冲区等待。服务端处理完数据并把响应回发，由于客户端send是阻塞的，所以还没到接收流程。服务端的回发响应就卡在发送缓冲区。当发送缓冲区满了，就无法继续消费客户端发来的数据了，因为阻塞窗口长度变为0，会停止接收数据。

解决方法：修改客户端为分块发送，一个chunk大小为1024，每次发送1024到服务端后立即recv，这样窗口就不会堵塞了，使得双方不会死锁。

测试得到各类数据类型的qps如下，大约在2w到5w之间，差距较大，分析原因应该是网络连接建立导致的。但这依然大幅超过了单条发送的4kpqs

属于大幅提升了写入性能

```bash

    lhy@ubuntu:~/share/kvs_lhy$ ./testcase 192.168.137.128 2000 21
    skiptable_batch_1500 -- > time used: 58, qps: 51724
    PASS
    lhy@ubuntu:~/share/kvs_lhy$ ./testcase 192.168.137.128 2000 22
    skiptable_batch_1500 -- > time used: 101, qps: 29702
    PASS
    lhy@ubuntu:~/share/kvs_lhy$ ./testcase 192.168.137.128 2000 23
    skiptable_batch_1500 -- > time used: 149, qps: 20134
    PASS
    lhy@ubuntu:~/share/kvs_lhy$ ./testcase 192.168.137.128 2000 24
    skiptable_batch_1500 -- > time used: 65, qps: 46153
```



## 5 主从备份

```c
┌─────────────────────────────────────────────────────────────────┐
│                      主从备份架构                              │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌─────────────────────────┐    TCP长连接    ┌────────────────┐│
│  │      主节点 (Master)    │◄───────────────►│  从节点 (Slave) ││
│  │                         │                  │                ││
│  │  • 处理读写请求         │   ① SYNC命令    │  • 只读        ││
│  │  • 写入AOF文件          │   ② 快照文件    │  • 接收增量    ││
│  │  • 检测AOF变化          │   ③ AOF增量     │  • 执行AOF     ││
│  │  • 发送增量数据         │   ④ PING/PONG   │  • 心跳维持    ││
│  └─────────────────────────┘                  └────────────────┘│
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 全量备份

1. **从节点发SYNC，主节点收到** → 执行 `save_snapshots()`清空aof并回应ok

2. **主节点发送snap**，发完会有END\n标志

3. 主节点进入增量备份模式--启动检测线程pthread

4. 从节点清空aof，接收snap，并写入data

5. 从节点启动接收线程

### 增量备份

1. 全量备份结束后，两者间维护TCP长连接

2. PING/PONG心跳维持

3. 主机有aof写入，则同步到从机
   
   

#### 1. 主节点检测线程 (`aof_master_check_thread`)

```c
void* aof_master_check_thread(void* arg) {
    while (g_aof_ctx.is_active && g_aof_ctx.fd > 0) {
        sleep(5);  // 每5秒检查一次

        // 1. 发送心跳 PING
        safe_send(g_aof_ctx.fd, "PING\n", 5);

        // 2. 检查4个AOF文件
        for (i = 0; i < 4; i++) {
            struct stat st;
            stat(aof_files[i], &st);

            // 文件变大 → 有新增内容
            if (st.st_size > g_aof_ctx.last_size[i]) {
                // 读取增量内容
                fseek(fp, g_aof_ctx.last_size[i], SEEK_SET);
                while (getline(&line, &line_len, fp) > 0) {
                    // 发送到从节点
                    safe_send(g_aof_ctx.fd, line, read);
                }
                g_aof_ctx.last_size[i] = st.st_size;
            }
        }
    }
}
```

**核心逻辑**：

1. 使用 `stat()` 获取文件大小

2. 比较当前大小和上次记录的大小

3. 如果有增长，从上次位置读取新增内容

4. 发送给从节点

#### 2. 从节点接收线程 (`aof_slave_recv_thread`)

```c
void* aof_slave_recv_thread(void* arg) {
    while (ctx->is_active && fd > 0) {
        // 1. 接收数据
        n = recv(fd, buffer, sizeof(buffer)-1, 0);

        // 2. 更新心跳时间
        ctx->last_heartbeat = time(NULL);

        // 3. 处理 PING → 回复 PONG
        if (strncmp(buffer, "PING", 4) == 0) {
            safe_send(fd, "PONG\n", 5);
            continue;
        }

        // 4. 按行解析并执行 AOF 命令
        while ((next = strchr(line, '\n'))) {
            *next = '\0';
            sscanf(line, "%s %s %s", cmd, key, val);
            kvs_filter_protocol(-1, tokens, cnt, NULL);
        }
    }
}
```



### 心跳实现

```c
┌─────────────────────────────────────────────────────────────────┐
│                      心跳机制原理                              │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  主节点 (Master)                    从节点 (Slave)             │
│      |                                  |                       │
│      | ① 每5秒发送 "PING\n"            |                       │
│      |--------------------------------->|                       │
│      |                                  | ② 收到 PING          │
│      |                                  | ③ 回复 "PONG\n"      │
│      |<---------------------------------|                       │
│      | ④ 收到 PONG，连接正常           | ④ 收到 PONG，连接正常│
│      |                                  |                       │
│      | ⑤ 如果发送失败 → 断开连接        | ⑤ 如果 recv 返回0   │
│      |                                  |    → 断开连接        │
│      |                                  |                       │
└─────────────────────────────────────────────────────────────────┘
```



### 数据传输

#### 全量同步数据传输

stat获取文件信息后，发送文件名和大小

接着分块发送文件内容，每次fread读取1KB的数据并发送，循环发完



增量同步数据传输

主节点调用线程aof_master_check_thread，每5s检测一次。if (st.st_size > g_aof_ctx.last_size[i])判断当前文件大小后最后一次记录大小，如果增大，就用fseek跳到上次读取的位置，接着逐行读取新增的内容，并发送。发完后更新记录的大小。

从节点调用线程aof_slave_recv_thread，并设置一个缓冲区accum，然后循环接收数据，在这期间会接收心跳数据并回复。然后解析并执行AOF命令。如果有不完整的数据就保存到accum中。



## 6 任意字符大小

#### 1. 内存池分配函数 (`kvs_mempool_alloc`)

大数据 → 自动降级到 malloc

* 小数据（≤ 248 字节）→ 从内存池分配（快）

* 大数据（> 248 字节）→ 自动降级到 `malloc`（灵活）
  
  

#### 2. 数据结构层都使用指针，而不是固定数组



### 3.传输层协议处理无长度限制

主从同步的协议解析是动态分配的：aof_slave_exec_line



批处理中针对cmd_buf和resp_buf是动态扩容的：kvs_ctx_expand





* * *

# 基于C的仿Redis数据库实现（KV存储）

* * *

* 源码仓库见：

* 视频讲解：

* * *

# 项目说明

* * *

# 1 RESP协议设计

* * *

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

* * *

        代码实现上，由于之前采用strtok根据空格分割SET KEY VALUE，因此需要改造为resp parse() 协议解析器。整体实现流程如下：
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

        用这种数组嵌套式写法，即使kv中存在\r\n也不会报错，因为解析根据校验头$len来判定kv的长度。

# 2 协程和IO_uring应用

## 基于coroutine的网络框架

## 基于uring的持久化数据落盘

问题
    原先的aof数据落盘：
    每次 SET/MOD/DEL 都会同步执行文件 I/O，每条写命令 open → fprintf → close，高频同步阻塞

改造方法
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

修改业务流程

业务请求线程只负责把 AOF 数据放进内存队列，不直接执行磁盘写；后台持久化线程负责批量提交 io_uring。
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

同时需要确保SAVA的一致性：
    SAVE
     ↓
    persistence_flush()
     ↓
    等待所有 AOF 写完成
     ↓
    save_snapshots()
     ↓
    清空 AOF

设计方案
    kvs_uring_t          ← 对外实例（含 worker 线程、队列、计数）
        └── kvs_ring_t   ← io_uring 裸封装（mmap 区域指针）
                └── io_uring_sqe / io_uring_cqe  ← 内核数据结构


    业务线程: kvs_uring_append → 构造 req → 入队 → signal(cond)
                                                      ↓
    worker线程: cond_wait → 批量取 64 个 → submit_requests
                  → ring_get_sqe × N → ring_submit → reap_one × N
                  → free_req → completed++

## 基于mmap的持久化数据加载

当前代码的 `load_snapshots()` 和 `replay_aof()` 都是 `fopen + fgetc/fread，效率低

改造方案
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

代码实现

1 mmap把文件映射到进程虚拟地址空间

要映射到虚拟地址空间的原因：CPU不能直接读磁盘文件，所以需要先把磁盘内容加载进内存。传统方式：read拷贝。内核从磁盘读文件到page cache（物理内存），然后把数据从pc拷贝进用户态缓冲区，CPU通过用户态缓冲区的虚拟地址访问数据。

    问题是从磁盘→pc，再从pc->用户态空间，需要两次拷贝。

mmap的方式：直接映射，char *p = mmap(...);内核建立虚拟地址->pc的页表映射，CPU通过p的虚拟地址直接访问pc，少了一次拷贝。关键：<mark>p是虚拟地址，页表把p映射到page cache的物理页，cpu访问p的时候直接访问pagecache。</mark>
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

# 3 config配置

通常情况下全量和增量只会选择一个开启，因此需要把全量同步和增量同步功能独立出来，然后通过config文件进行配置，比如说./kvstore --config master.conf启动主节点，./kvstore --config slave.conf启动从节点。

主从备份：

主节点
    # 主节点配置
    role = master
    port = 2000


    # snap快照同步
    snap.enable=true
    # 从节点一般不启动aof
    aof.enable=true

从节点
    # 从节点配置
    role=slave
    port=2001
    master_ip=192.168.137.128
    master_port=2000

    # snap快照同步
    snap.enable=true
    # 从节点一般不启动aof
    aof.enable=false

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

A是主机，B是从机

1：A每次同步给B数据，会在本地维护一个CRC列表 crc_list_a，如果是snap模式，在每次执行save后，重新计算每条信息的CRC。aof需要CRC校验吗？因为aof是记录命令操作，可以溯源的。

2：A宕机后，B接管，B同样维护crc_list_b

3：A恢复后，从B同步数据，先对比备份内容的crc_list，如果有不同，会把这些数据罗列出来，并暂停插入，同时把不同的数据写入临时文件中

4：如果检查完无误后，执行命令在把临时文件的插入主机data。

# 6 日志等级

日志格式：
    [时间] [级别] [文件:行] 消息

    [2024-01-01 12:00:00] [INFO] [kvstore.c:100] AOF started, offset=0
    [2024-01-01 12:00:01] [ERROR] [kvs_persist.c:200] open data/data_aof.aof failed: No such file

    时间格式：YYYY-MM-DD HH:MM:SS

另外在config中加入一个是否为调试状态的开关

打开后，终端有输出

# 7 内存管理

![620a828c0f294373b923f7d3fecd4864](file:///V:/kvs_lhy/620a828c-0f29-4373-b923-f7d3fecd4864.png?msec=1791356321264)

图为32位用户的虚拟空间内存结构图，由上到下分别是：

1. 只读段：包括代码和常量等；

2. 数据段：包括全局变量等；

3. 堆：包括动态分配的内存，从低地址开始向上增长；

4. 文件映射段：包括动态库、共享内存等，从高地址开始向下增长；

5. 栈：包括局部变量和函数调用的上下文等。

其中**堆**与**文件映射段**是我们讨论的重点，它们的内存都是动态分配的。比如说，使用 C 标准库的 **malloc()** 或者 **mmap()** ，就可以分别在**堆**和**文件映射段**动态分配内存。

**malloc()** 是 C 标准库提供的内存分配函数，对应到系统调用上，有两种实现方式，即 **brk()** 和 **mmap()**。

**1. brk 方式**

* 对于小块内存（<128K），C 标准库使用 **brk()** 来分配。也就是通过移动堆顶的位置来分配内存。这些内存释放后并不会立刻归还系统，而是被缓存起来，重复使用。
* 优缺点：brk() 方式可以减少缺页异常的发生，提高内存访问效率。不过，由于这些内存没有归还系统，所以在内存工作繁忙时，频繁的内存分配和释放会造成内存碎片。

**2. mmap 匿名映射方式**

* 对于大块内存（>128K），C 标准库使用 **mmap()** 来分配，也就是在文件映射段找一块空闲内存分配出去。**mmap()** 方式分配的内存，会在释放时直接归还系统，所以每次 mmap 都会发生缺页异常。
* 优缺点：**mmap()** 方式可以将内存及时返回给系统，避免 OOM。但是工作繁忙时，频繁的内存分配会导致大量的缺页异常，使内核的管理负担增大。这也是 malloc 只对大块内存使用 mmap 的原因。

所谓的**缺页异常**是指进程申请内存后，只分配了虚拟内存。这些所申请的虚拟内存，只有在首次访问时才会分配真正的物理内存，也就是通过缺页异常进入内核中，再由**内核**来分配物理内存（本质就是建立虚拟内存与物理内存的地址映射）。

* brk() 方式申请的**堆内存**由于释放内存后并不会归还给系统，所以下次申请内存时，并不需要发生缺页异常。
* mmap() 方式申请的动态内存会在释放内存后直接归还系统，所以下次申请内存时，会发生缺页异常（增加内核态 CPU 开销）。

C 语言跟内存申请相关的函数主要有 calloc, malloc, realloc 等。

* malloc：根据内存申请大小，选择在堆或文件映射段中分配连续内存，但是不会初始化内存，一般会再通过 memset 函数来初始化这块内存。
* calloc：与 malloc 类似，只不过会自动初始化这块内存空间，每个字节置为 0。
* realloc：可以对已申请的内存进行大小调整，同 malloc 一样新申请的内存也是未初始化的。

上述所说的是 Linux 进程通过 C 标准库中的内存分配函数 malloc 向系统申请内存，但是到真正与内核交互之间，其实还隔了一层，即**内存分配管理器(memory allocator)**。常见的内存分配器包括：ptmalloc(Glibc)、tcmalloc(Google)、jemalloc(FreeBSD)。MySQL 默认使用的是 glibc 的 ptmalloc 作为内存分配器。

![7mj3vh74lg](https://ask.qcloudimg.com/http-save/yehe-7053949/7mj3vh74lg.png)
