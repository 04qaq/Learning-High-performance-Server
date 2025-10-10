------

# Socket

## 1. 什么是 Socket？

**Socket（套接字）** 是操作系统提供的 **网络通信接口**。
 应用程序通过 Socket 调用操作系统内核的 **TCP/IP 协议栈**，实现跨主机的进程间通信。

可以把 socket 理解为 **进程在网络中的通信端点**。

------

## 2. Socket 的基本分类

### （1）按协议族

- **AF_INET**：IPv4 网络协议（最常用）。
- **AF_INET6**：IPv6 网络协议。
- **AF_UNIX/AF_LOCAL**：本地进程间通信（IPC，基于文件系统）。

### （2）按传输方式

- **SOCK_STREAM**
  - 面向连接（TCP）。
  - 数据可靠、有序、无差错、无重复。
  - 适合大多数应用（HTTP、FTP、SSH…）。
- **SOCK_DGRAM**
  - 无连接（UDP）。
  - 数据报形式，可能丢包、乱序。
  - 适合实时性要求高的应用（视频通话、游戏、DNS）。
- **SOCK_RAW**
  - 原始套接字，可以自己构造 IP 包头，用于底层协议研究（比如抓包工具、协议分析）。

------

## 3. 常见函数介绍

### （1）创建套接字

```c
#include <sys/socket.h>
int sockfd = socket(AF_INET, SOCK_STREAM, 0);
```

- **第一个参数**：协议族，例如 `AF_INET` 表示 IPv4，`AF_INET6` 表示 IPv6。
- **第二个参数**：传输方式，例如 `SOCK_STREAM` 表示流式（TCP），`SOCK_DGRAM` 表示数据报（UDP）。
- **第三个参数**：协议。通常写 `0`，表示由系统根据前两个参数自动选择；也可以指定为 `IPPROTO_TCP` 或 `IPPROTO_UDP`。

------

### （2）绑定地址

```c++
#include <arpa/inet.h>
#include <cstring>

struct sockaddr_in serv_addr;                  // IPv4 专用 socket 地址
bzero(&serv_addr, sizeof(serv_addr));          // 初始化置零
serv_addr.sin_family = AF_INET;                // 协议族：IPv4
serv_addr.sin_addr.s_addr = inet_addr("127.0.0.1"); // IP 地址（转成网络字节序）
serv_addr.sin_port = htons(8888);              // 端口号（转成网络字节序）

bind(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)); // 绑定 socket 和地址
```

注意事项：

- **为什么定义用 `sockaddr_in`，但 `bind()` 要强转成 `sockaddr\*`？**
   因为 `bind()` 定义的是通用接口，接受所有协议族的 `sockaddr*`。`sockaddr_in` 只是 IPv4 专用结构，需要强转。
- **为什么要用 `inet_addr()`、`htons()`？**
   不同 CPU 有大端/小端差异，IETF 规定网络上传输一律使用 **大端序（网络字节序）**。
  - `inet_addr("127.0.0.1")`：字符串 → 32 位网络字节序的 IP。
  - `htons(8888)`：16 位主机字节序（端口） → 网络字节序。
  - 对应的反向函数是 `ntohl()`、`ntohs()`。

------

### （3）监听

```c++
const int SOMAXCONN = 128;
listen(sockfd, SOMAXCONN);
```

- **第一个参数**：监听的套接字。
- **第二个参数**：最大等待队列长度（Linux 推荐值为 128）。

------

### （4）接受客户端连接

```c++
struct sockaddr_in clnt_addr;
socklen_t clnt_addr_len = sizeof(clnt_addr);
bzero(&clnt_addr, sizeof(clnt_addr));

int clnt_sockfd = accept(sockfd, (struct sockaddr*)&clnt_addr, &clnt_addr_len);
printf("new client fd %d! IP: %s Port: %d\n",
       clnt_sockfd,
       inet_ntoa(clnt_addr.sin_addr),     // 转回点分十进制 IP
       ntohs(clnt_addr.sin_port));        // 转回主机字节序端口
```

注意事项：

- `bind()` 的第三个参数只需要传结构体大小；
- `accept()` 需要传入 `socklen_t*`，让内核把客户端地址长度写回去。
- `accept()` 会阻塞，直到有客户端连接成功。

------

### （5）客户端连接

```c++
int sockfd = socket(AF_INET, SOCK_STREAM, 0);

struct sockaddr_in serv_addr;
bzero(&serv_addr, sizeof(serv_addr));
serv_addr.sin_family = AF_INET;
serv_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
serv_addr.sin_port = htons(8888);

connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
```

客户端主动调用 `connect()` 发起三次握手，连接成功后就能用 `send()/recv()` 或 `write()/read()` 通信。

------

## 4. Socket 通信流程总结（以 TCP 为例）

### 服务端：

1. `socket()` → 创建监听套接字
2. `bind()` → 绑定 IP 和端口
3. `listen()` → 开始监听
4. `accept()` → 等待并接受客户端连接
5. `send()/recv()` → 收发数据
6. `close()` → 关闭连接

### 客户端：

1. `socket()` → 创建套接字
2. `connect()` → 主动连接服务器
3. `send()/recv()` → 收发数据
4. `close()` → 关闭连接

------

## 5. 常见应用场景

- Web 服务器（Nginx、Apache）
- 数据库服务（MySQL、PostgreSQL）
- 聊天软件（QQ、微信）
- 网络游戏
- 分布式系统（RPC、消息队列）

------

一句话总结：
**Socket 就是进程间通过网络通信的统一接口。`sockaddr_in` 方便设置地址，`sockaddr` 作为通用类型提供多态接口，而字节序转换函数保证了不同硬件架构之间的正确通信。**
