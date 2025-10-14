#include "libs/socket.h"
#include "libs/address.h"
#include "libs/iomanager.h"
#include "libs/log.h"

#include <cstring>
#include <memory>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h> // TCP_NODELAY
#include <fcntl.h>       // fcntl, FD_CLOEXEC
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include <net/if.h> // IF_NAMESIZE, if_indextoname

// 可移植的 likely/unlikely hints 宏
#if defined(__has_builtin)
#if __has_builtin(__builtin_expect)
#define SUNSHINE_LIKELY(x) (__builtin_expect(!!(x), 1))
#define SUNSHINE_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#else
#define SUNSHINE_LIKELY(x) (x)
#define SUNSHINE_UNLIKELY(x) (x)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define SUNSHINE_LIKELY(x) (__builtin_expect(!!(x), 1))
#define SUNSHINE_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#else
#define SUNSHINE_LIKELY(x) (x)
#define SUNSHINE_UNLIKELY(x) (x)
#endif

namespace sunshine {

// logger（替换为你的日志实例构造方式）
static Logger::ptr g_logger = std::make_shared<Logger>("system");

// ----------------------------
// 辅助：把 sockaddr_storage -> Address::ptr
// ----------------------------
static Address::ptr createAddressFromSockaddr(const sockaddr *sa, socklen_t salen) {
    if (!sa) return nullptr;
    return Address::Create(sa, salen);
}

// ----------------------------
// 构造 / 析构
// ----------------------------
Socket::Socket(int family, int type, int protocol) :
    m_socket(-1), m_family(family), m_type(type), m_protocol(protocol), m_isConnected(false) {
}

Socket::~Socket() {
    close();
}

// ----------------------------
// 超时：SO_SNDTIMEO / SO_RCVTIMEO（毫秒）
// ----------------------------
int64_t Socket::getSendTimeout() {
    if (!isValid()) return -1;
    struct timeval tv;
    socklen_t len = static_cast<socklen_t>(sizeof(tv));
    if (::getsockopt(m_socket, SOL_SOCKET, SO_SNDTIMEO, &tv, &len) != 0) {
        return -1;
    }
    return static_cast<int64_t>(tv.tv_sec) * 1000 + (tv.tv_usec / 1000);
}

void Socket::setSendTimeOut(int64_t v) {
    if (!isValid()) return;
    struct timeval tv;
    if (v < 0) {
        tv.tv_sec = 0;
        tv.tv_usec = 0;
    } else {
        tv.tv_sec = static_cast<time_t>(v / 1000);
        tv.tv_usec = static_cast<suseconds_t>((v % 1000) * 1000);
    }
    ::setsockopt(m_socket, SOL_SOCKET, SO_SNDTIMEO, &tv, static_cast<socklen_t>(sizeof(tv)));
}

int64_t Socket::getRecvTimeout() {
    if (!isValid()) return -1;
    struct timeval tv;
    socklen_t len = static_cast<socklen_t>(sizeof(tv));
    if (::getsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, &len) != 0) {
        return -1;
    }
    return static_cast<int64_t>(tv.tv_sec) * 1000 + (tv.tv_usec / 1000);
}

void Socket::setRecvTimeout(int64_t v) {
    if (!isValid()) return;
    struct timeval tv;
    if (v < 0) {
        tv.tv_sec = 0;
        tv.tv_usec = 0;
    } else {
        tv.tv_sec = static_cast<time_t>(v / 1000);
        tv.tv_usec = static_cast<suseconds_t>((v % 1000) * 1000);
    }
    ::setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, static_cast<socklen_t>(sizeof(tv)));
}

// ----------------------------
// getsockopt / setsockopt 封装
// ----------------------------
bool Socket::getOption(int level, int option, void *result, socklen_t *len) {
    if (!isValid()) return false;
    if (::getsockopt(m_socket, level, option, result, len) == 0) {
        return true;
    }
    return false;
}

bool Socket::setOption(int level, int option, const void *result, socklen_t len) {
    if (!isValid()) return false;
    if (::setsockopt(m_socket, level, option, result, len) == 0) {
        return true;
    }
    return false;
}

// ----------------------------
// accept：接收客户端连接并返回已初始化的 Socket::shared_ptr
// ----------------------------
Socket::shared_ptr Socket::accept() {
    if (!isValid()) return nullptr;
    sockaddr_storage ss;
    socklen_t slen = static_cast<socklen_t>(sizeof(ss));
    int newsock = ::accept(m_socket, reinterpret_cast<sockaddr *>(&ss), &slen);
    if (newsock == -1) {
        LOG_ERROR(g_logger) << "accept error errno=" << errno << " strerr=" << strerror(errno);
        return nullptr;
    }
    Socket::shared_ptr sock = std::make_shared<Socket>(m_family, m_type, m_protocol);
    if (sock->init(newsock)) {
        return sock;
    } else {
        ::close(newsock);
        return nullptr;
    }
}

// ----------------------------
// init：接管外部 fd，读取状态并初始化（保留阻塞/非阻塞语义）
// ----------------------------
bool Socket::init(int sock) {
    if (sock < 0) return false;

    // 初始化成员为安全默认值
    m_socket = -1;
    m_family = AF_UNSPEC;
    m_type = 0;
    m_protocol = 0;
    m_isConnected = false;
    m_localAddress.reset();
    m_remoteAddress.reset();

    m_socket = sock;

    // 获取 socket type（SOCK_STREAM / SOCK_DGRAM）
    int so_type = 0;
    socklen_t optlen = static_cast<socklen_t>(sizeof(so_type));
    if (::getsockopt(m_socket, SOL_SOCKET, SO_TYPE, &so_type, &optlen) == 0) {
        m_type = so_type;
    } else {
        m_type = 0;
    }

    // getsockname 获取本地地址与 family（如果未 bind，某些实现仍能返回 family）
    sockaddr_storage local_ss;
    socklen_t local_len = static_cast<socklen_t>(sizeof(local_ss));
    if (::getsockname(m_socket, reinterpret_cast<sockaddr *>(&local_ss), &local_len) == 0) {
        m_family = reinterpret_cast<sockaddr *>(&local_ss)->sa_family;
        m_localAddress = createAddressFromSockaddr(reinterpret_cast<sockaddr *>(&local_ss), local_len);
        // 对 Unix 域地址需特别处理其长度（见 getLocalAddress）
    } else {
        m_family = AF_UNSPEC;
    }

    // getpeername 判断是否已连接（对 TCP 有意义）
    sockaddr_storage remote_ss;
    socklen_t remote_len = static_cast<socklen_t>(sizeof(remote_ss));
    if (::getpeername(m_socket, reinterpret_cast<sockaddr *>(&remote_ss), &remote_len) == 0) {
        m_isConnected = true;
        m_remoteAddress = createAddressFromSockaddr(reinterpret_cast<sockaddr *>(&remote_ss), remote_len);
    } else {
        m_isConnected = false;
    }

    // protocol 无通用方法可取，保留为 m_protocol（若构造时提供则已设置）
    // 通用初始化（设置 FD_CLOEXEC, SO_REUSEADDR, TCP_NODELAY）
    initSocket();
    return true;
}

// ----------------------------
// bind（检查 family 后 bind）
// ----------------------------
bool Socket::bind(const Address::ptr addr) {
    if (!addr) return false;
    if (!isValid()) {
        newSocket();
        if (SUNSHINE_UNLIKELY(!isValid())) {
            LOG_ERROR(g_logger) << "bind newSocket failed";
            return false;
        }
    }

    if (SUNSHINE_UNLIKELY(m_family != addr->getFamily())) {
        LOG_ERROR(g_logger) << "bind socket.family(" << m_family
                            << ") addr.family(" << addr->getFamily()
                            << ") not equal, addr=" << addr->toString();
        return false;
    }

    if (::bind(m_socket, addr->getAddr(), addr->getAddrLen()) != 0) {
        LOG_ERROR(g_logger) << "bind error errno=" << errno
                            << " strerr=" << strerror(errno)
                            << " addr=" << addr->toString();
        return false;
    }

    // 更新本地地址缓存
    getLocalAddress();
    return true;
}

// ----------------------------
// connect_with_timeout：辅助函数（非成员）
// - 临时把 socket 设为非阻塞（若原来是阻塞），发起 connect，poll 等待可写 / 超时
// - 成功后恢复原有 socket flags
// 返回 0 成功，-1 失败（errno 被设置）
// ----------------------------
static int connect_with_timeout(int sockfd, const struct sockaddr *addr, socklen_t addrlen, int timeout_ms) {
    // 获取原 flags
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags == -1) return -1;
    bool is_nonblock = (flags & O_NONBLOCK) != 0;
    int rc = 0;

    if (!is_nonblock) {
        if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1) {
            return -1;
        }
    }

    int n = ::connect(sockfd, addr, addrlen);
    if (n == 0) {
        rc = 0;
        goto restore;
    } else {
        if (errno == EINPROGRESS) {
            struct pollfd pfd;
            pfd.fd = sockfd;
            pfd.events = POLLOUT | POLLERR | POLLHUP;
            int pret;
            while ((pret = poll(&pfd, 1, timeout_ms)) < 0 && errno == EINTR) {
                continue;
            }
            if (pret == 0) {
                errno = ETIMEDOUT;
                rc = -1;
                goto restore;
            } else if (pret < 0) {
                rc = -1;
                goto restore;
            } else {
                int err = 0;
                socklen_t elen = static_cast<socklen_t>(sizeof(err));
                if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0) {
                    rc = -1;
                    goto restore;
                }
                if (err != 0) {
                    errno = err;
                    rc = -1;
                    goto restore;
                }
                rc = 0;
                goto restore;
            }
        } else {
            if (errno == EISCONN) {
                rc = 0;
                goto restore;
            } else {
                rc = -1;
                goto restore;
            }
        }
    }

restore:
    if (!is_nonblock) {
        (void)fcntl(sockfd, F_SETFL, flags);
    }
    return rc;
}

// ----------------------------
// connect：客户端主动连接（支持可选超时）
// ----------------------------
bool Socket::connect(const Address::ptr addr, uint64_t timeout_ms) {
    if (!addr) return false;

    if (!isValid()) {
        newSocket();
        if (SUNSHINE_UNLIKELY(!isValid())) {
            LOG_ERROR(g_logger) << "connect newSocket failed";
            return false;
        }
    }

    if (SUNSHINE_UNLIKELY(m_family != addr->getFamily())) {
        LOG_ERROR(g_logger) << "connect socket.family(" << m_family
                            << ") addr.family(" << addr->getFamily()
                            << ") not equal, addr=" << addr->toString();
        return false;
    }

    int rt = 0;
    if (timeout_ms == (uint64_t)-1) {
        rt = ::connect(m_socket, addr->getAddr(), addr->getAddrLen());
    } else {
        rt = connect_with_timeout(m_socket, addr->getAddr(), addr->getAddrLen(), static_cast<int>(timeout_ms));
    }

    if (rt != 0) {
        LOG_ERROR(g_logger) << "socket=" << m_socket << " connect(" << addr->toString()
                            << ") error errno=" << errno << " strerr=" << strerror(errno);
        return false;
    }

    m_isConnected = true;
    // 更新远端/本地地址缓存
    getRemoteAddress();
    getLocalAddress();
    return true;
}

// ----------------------------
// listen / close
// ----------------------------
bool Socket::listen(int backlog) {
    if (!isValid()) {
        LOG_ERROR(g_logger) << "listen error socket invalid";
        return false;
    }
    if (::listen(m_socket, backlog) != 0) {
        LOG_ERROR(g_logger) << "listen error errno=" << errno << " strerr=" << strerror(errno);
        return false;
    }
    return true;
}

bool Socket::close() {
    if (m_socket == -1) {
        m_isConnected = false;
        return true; // 已关闭，视为成功
    }
    m_isConnected = false;
    if (::close(m_socket) == 0) {
        m_socket = -1;
        return true;
    } else {
        LOG_ERROR(g_logger) << "close error errno=" << errno << " strerr=" << strerror(errno);
        m_socket = -1;
        return false;
    }
}

// ----------------------------
// 发送（TCP）
// ----------------------------
int Socket::send(const void *buffer, size_t length, int flags) {
    if (!m_isConnected) return -1;
    return ::send(m_socket, buffer, length, flags);
}

int Socket::send(const iovec *buffers, size_t length, int flags) {
    if (!m_isConnected) return -1;
    msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = const_cast<iovec *>(buffers);
    msg.msg_iovlen = static_cast<size_t>(length);
    return ::sendmsg(m_socket, &msg, flags);
}

// ----------------------------
// 发送（UDP）
// ----------------------------
int Socket::sendTo(const void *buffer, size_t length, const Address::ptr to, int flags) {
    if (!isValid() || !to) return -1;
    return ::sendto(m_socket, buffer, length, flags, to->getAddr(), to->getAddrLen());
}

int Socket::sendTo(const iovec *buffers, size_t length, const Address::ptr to, int flags) {
    if (!isValid() || !to) return -1;
    msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = const_cast<iovec *>(buffers);
    msg.msg_iovlen = static_cast<size_t>(length);
    msg.msg_name = const_cast<void *>(reinterpret_cast<const void *>(to->getAddr()));
    msg.msg_namelen = to->getAddrLen();
    return ::sendmsg(m_socket, &msg, flags);
}

// ----------------------------
// 接收（TCP）
// ----------------------------
int Socket::recv(void *buffer, size_t length, int flags) {
    if (!m_isConnected) return -1;
    return ::recv(m_socket, buffer, length, flags);
}

int Socket::recv(iovec *buffers, size_t length, int flags) {
    if (!m_isConnected) return -1;
    msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = buffers;
    msg.msg_iovlen = static_cast<size_t>(length);
    return ::recvmsg(m_socket, &msg, flags);
}

// ----------------------------
// 接收（UDP）：输出发送方地址到 peer（引用）
// ----------------------------
/*
使用方法展示：
Address::ptr peer;
int n = sock->recvFrom(buf, len, peer);
if (n > 0 && peer) {
    std::cout << "from: " << peer->toString() << "\n";
}
*/
int Socket::recvFrom(void *buffer, size_t length, Address::ptr &peer, int flags) {
    if (!isValid()) return -1;
    sockaddr_storage ss;
    socklen_t slen = static_cast<socklen_t>(sizeof(ss));
    ssize_t n = ::recvfrom(m_socket, buffer, length, flags, reinterpret_cast<sockaddr *>(&ss), &slen);
    if (n >= 0) {
        peer = createAddressFromSockaddr(reinterpret_cast<sockaddr *>(&ss), slen);
    }
    return static_cast<int>(n);
}

int Socket::recvFrom(iovec *buffers, size_t length, Address::ptr &peer, int flags) {
    if (!isValid()) return -1;
    msghdr msg;
    memset(&msg, 0, sizeof(msg));
    sockaddr_storage ss;
    memset(&ss, 0, sizeof(ss));
    msg.msg_iov = buffers;
    msg.msg_iovlen = static_cast<size_t>(length);
    msg.msg_name = &ss;
    msg.msg_namelen = static_cast<socklen_t>(sizeof(ss));
    ssize_t n = ::recvmsg(m_socket, &msg, flags);
    if (n >= 0) {
        peer = createAddressFromSockaddr(reinterpret_cast<sockaddr *>(&ss), static_cast<socklen_t>(msg.msg_namelen));
    }
    return static_cast<int>(n);
}

// ----------------------------
// 本地/远端地址查询
// ----------------------------
Address::ptr Socket::getLocalAddress() {
    if (m_localAddress) return m_localAddress;
    Address::ptr result;
    switch (m_family) {
    case AF_INET:
        result.reset(new IPv4Address());
        break;
    case AF_INET6:
        result.reset(new IPv6Address());
        break;
    case AF_UNIX:
        result.reset(new UnixAddress());
        break;
    default:
        result.reset(new UnknownAddress(m_family));
        break;
    }
    socklen_t addrlen = result->getAddrLen();
    if (::getsockname(m_socket, const_cast<sockaddr *>(result->getAddr()), &addrlen) != 0) {
        LOG_ERROR(g_logger) << "getsockname error socket=" << m_socket
                            << " errno=" << errno << " strerr=" << strerror(errno);
        return Address::ptr(new UnknownAddress(m_family));
    }
    if (m_family == AF_UNIX) {
        auto addr = std::dynamic_pointer_cast<UnixAddress>(result);
        if (addr) addr->setAddrLen(addrlen);
    }
    m_localAddress = result;
    return m_localAddress;
}

Address::ptr Socket::getRemoteAddress() {
    if (m_remoteAddress) return m_remoteAddress;
    Address::ptr result;
    switch (m_family) {
    case AF_INET:
        result.reset(new IPv4Address());
        break;
    case AF_INET6:
        result.reset(new IPv6Address());
        break;
    case AF_UNIX:
        result.reset(new UnixAddress());
        break;
    default:
        result.reset(new UnknownAddress(m_family));
        break;
    }
    socklen_t addrlen = result->getAddrLen();
    if (::getpeername(m_socket, const_cast<sockaddr *>(result->getAddr()), &addrlen) != 0) {
        LOG_ERROR(g_logger) << "getpeername error socket=" << m_socket
                            << " errno=" << errno << " strerr=" << strerror(errno);
        return Address::ptr(new UnknownAddress(m_family));
    }
    if (m_family == AF_UNIX) {
        auto addr = std::dynamic_pointer_cast<UnixAddress>(result);
        if (addr) addr->setAddrLen(addrlen);
    }
    m_remoteAddress = result;
    return m_remoteAddress;
}

// ----------------------------
// 状态 / 错误 / dump
// ----------------------------
bool Socket::isValid() const {
    return m_socket != -1;
}

int Socket::getError() {
    if (!isValid()) return errno;
    int err = 0;
    socklen_t len = static_cast<socklen_t>(sizeof(err));
    if (::getsockopt(m_socket, SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
        return errno;
    }
    return err;
}

std::ostream &Socket::dump(std::ostream &os) const {
    os << "[Socket=" << m_socket
       << " isConnect=" << m_isConnected
       << " family=" << m_family
       << " type=" << m_type
       << " protocol=" << m_protocol;
    if (m_localAddress) os << " localAddress=" << m_localAddress->toString();
    if (m_remoteAddress) os << " remoteAddress=" << m_remoteAddress->toString();
    os << "]";
    return os;
}

// ----------------------------
// 取消事件（依赖 IOManager 实现）
// ----------------------------
bool Socket::cancelRead() {
    return IOManager::GetThis()->cancelEvent(m_socket, IOManager::READ);
}
bool Socket::cancelWrite() {
    return IOManager::GetThis()->cancelEvent(m_socket, IOManager::WRITE);
}
bool Socket::cancelAccept() {
    return IOManager::GetThis()->cancelEvent(m_socket, IOManager::READ);
}
bool Socket::cancelAll() {
    return IOManager::GetThis()->cancelAll(m_socket);
}

// ----------------------------
// initSocket/newSocket：通用初始化与创建
// ----------------------------
void Socket::initSocket() {
    if (m_socket <= 0) return;

    // FD_CLOEXEC
    int flags = ::fcntl(m_socket, F_GETFD, 0);
    if (flags != -1) {
        flags |= FD_CLOEXEC;
        ::fcntl(m_socket, F_SETFD, flags);
    }

    // SO_REUSEADDR
    int on = 1;
    ::setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, &on, static_cast<socklen_t>(sizeof(on)));

#ifdef SO_REUSEPORT
    ::setsockopt(m_socket, SOL_SOCKET, SO_REUSEPORT, &on, static_cast<socklen_t>(sizeof(on)));
#endif

    // TCP_NODELAY for stream sockets
    if (m_type == SOCK_STREAM) {
#ifdef TCP_NODELAY
        int val = 1;
        ::setsockopt(m_socket, IPPROTO_TCP, TCP_NODELAY, &val, static_cast<socklen_t>(sizeof(val)));
#endif
    }
}

void Socket::newSocket() {
    m_socket = ::socket(m_family, m_type, m_protocol);
    if (SUNSHINE_LIKELY(m_socket != -1)) {
        initSocket();
    } else {
        LOG_ERROR(g_logger) << "socket(" << m_family << ", " << m_type << ", " << m_protocol
                            << ") error=" << errno << " errstr=" << strerror(errno);
    }
}

} // namespace sunshine
