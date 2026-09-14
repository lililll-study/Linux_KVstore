# 使用说明

```bash

编译
进入项目根目录
make

运行主节点 master
make run


从机运行（可选）
make run-slave


```

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


