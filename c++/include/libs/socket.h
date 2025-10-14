#pragma once

#include <sys/socket.h>
#include <sys/uio.h> // iovec
#include <memory>
#include <ostream>
#include <cstdint>
#include "libs/address.h"
#include "libs/log.h"

namespace sunshine {

/**
 * @brief Socket 封装类
 *
 * 说明：
 * - 对外以 RAII / shared_ptr 方式管理 socket。内部保存 m_socket、family/type/protocol。
 * - send/recv 系列方法：TCP 可直接 send/recv，UDP 可用 sendTo/recvFrom 处理目标地址。
 * - recvFrom / recvFrom(iovec...) 的第三参数为 Address::ptr &peer（输出），用于返回发送方地址。
 */
class Socket : public std::enable_shared_from_this<Socket> {
public:
    using shared_ptr = std::shared_ptr<Socket>;
    using weak_ptr = std::weak_ptr<Socket>;

    Socket(int family, int type = 0, int protocol = 0);
    ~Socket();

    // 以毫秒为单位的超时时间接口
    int64_t getSendTimeout();
    void setSendTimeOut(int64_t v);

    int64_t getRecvTimeout();
    void setRecvTimeout(int64_t v);

    // getsockopt / setsockopt 封装（使用 socklen_t）
    bool getOption(int level, int option, void *result, socklen_t *len);
    template <class T>
    bool getOption(int level, int option, T &result) {
        socklen_t l = static_cast<socklen_t>(sizeof(T));
        return getOption(level, option, &result, &l);
    }

    bool setOption(int level, int option, const void *result, socklen_t len);
    template <class T>
    bool setOption(int level, int option, const T &result) {
        return setOption(level, option, &result, static_cast<socklen_t>(sizeof(T)));
    }

    Socket::shared_ptr accept(); // 接受客户端连接（服务器端）

    bool init(int sock);                                                       // 由外部句柄创建并接管
    bool bind(const Address::ptr addr);                                        // 绑定
    bool connect(const Address::ptr addr, uint64_t timeout_ms = (uint64_t)-1); // 连接（支持超时）
    bool listen(int backlog = SOMAXCONN);                                      // 监听
    bool close();                                                              // 关闭

    // 发送（TCP / 已连接）
    int send(const void *buff, size_t length, int flags = 0);
    int send(const iovec *buff, size_t length, int flags = 0);

    // 发送（UDP / 无连接，需要目标地址）
    int sendTo(const void *buff, size_t length, const Address::ptr to, int flags = 0);
    int sendTo(const iovec *buff, size_t length, const Address::ptr to, int flags = 0);

    // 接收（TCP / 已连接）
    int recv(void *buff, size_t length, int flags = 0);
    int recv(iovec *buff, size_t length, int flags = 0);

    // 接收（UDP / 无连接，输出发送方地址到 peer）
    int recvFrom(void *buff, size_t length, Address::ptr &peer, int flags = 0);
    int recvFrom(iovec *buff, size_t length, Address::ptr &peer, int flags = 0);

    // 本地/远端地址
    Address::ptr getLocalAddress();
    Address::ptr getRemoteAddress();

    int getFamily() const {
        return m_family;
    }
    int getProtocol() const {
        return m_protocol;
    }
    int getType() const {
        return m_type;
    }
    bool isConnect() const {
        return m_isConnected;
    } // 是否已连接
    int getSocket() const {
        return m_socket;
    } // socket fd

    bool isValid() const; // 是否有效
    int getError();       // 获取 socket 错误（SO_ERROR）

    std::ostream &dump(std::ostream &os) const;

    // 取消 IOManager 上的事件（需要 IOManager 实现）
    bool cancelRead();
    bool cancelWrite();
    bool cancelAccept();
    bool cancelAll();

private:
    void initSocket(); // 初始化 socket（设置 FD_CLOEXEC / SO_REUSEADDR / TCP_NODELAY 等）
    void newSocket();  // 新建 socket（使用 m_family/m_type/m_protocol）

private:
    int m_socket;
    int m_family;
    int m_type;
    int m_protocol;
    bool m_isConnected;

    Address::ptr m_localAddress;
    Address::ptr m_remoteAddress;
};

} // namespace sunshine
