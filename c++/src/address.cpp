#include "libs/address.h"
#include "libs/log.h"
#include <memory>
#include <sstream>
#include <netdb.h>
#include <ifaddrs.h>
#include <stddef.h>
#include <cstring>
#include <errno.h>
#include <stdexcept>
#include <algorithm>
#include <net/if.h>

namespace sunshine {

// 获取 logger（假设你已有日志模块）
static Logger::ptr g_logger = std::make_shared<Logger>("system");

/////////////////////////////////////////////////////////////////
// 辅助模板与函数
/////////////////////////////////////////////////////////////////

/**
 * @brief 创建掩码：返回高 bits 位为 1，其余为 0 的字面值（host-order）
 *
 * 例如：sizeof(T)=4 字节（32 位），bits=24 -> 0xFFFFFF00
 */
template <class T>
static T CreateMask(uint32_t bits) {
    const uint32_t total = sizeof(T) * 8;
    if (bits == 0) return static_cast<T>(0);
    if (bits >= total) return static_cast<T>(~(T)0);
    // 先构造全部为 1 的值，再左移得到高 bits 为 1
    return static_cast<T>((~(T)0) << (total - bits));
}

/**
 * @brief 统计 value 中二进制 1 的个数（Brian Kernighan 算法）
 */
template <class T>
static uint32_t CountBytes(T value) {
    uint32_t result = 0;
    while (value) {
        value &= value - 1;
        ++result;
    }
    return result;
}

/////////////////////////////////////////////////////////////////
// Address::Create：从 sockaddr 构造对应 Address 子类
/////////////////////////////////////////////////////////////////
Address::ptr Address::Create(const sockaddr *addr, socklen_t addrlen) {
    if (addr == nullptr) {
        return nullptr;
    }
    Address::ptr result;
    switch (addr->sa_family) {
    case AF_INET:
        result.reset(new IPv4Address(*(const sockaddr_in *)addr));
        break;
    case AF_INET6:
        result.reset(new IPv6Address(*(const sockaddr_in6 *)addr));
        break;
    default:
        result.reset(new UnknownAddress(*addr));
        break;
    }
    return result;
}

/////////////////////////////////////////////////////////////////
// Address::Lookup
// 使用 getaddrinfo 解析 host（支持 "host" / "host:port" / "[ipv6]:port" 等格式）
// family/type/protocol 用于 hints（可传 AF_UNSPEC/0/0 表示不限定）
// 解析到的每个 ai_addr 都转换成对应的 Address 并 push 到 result。
/////////////////////////////////////////////////////////////////
bool Address::Lookup(std::vector<Address::ptr> &result,
                     const std::string &host,
                     int family,
                     int type,
                     int protocol) {
    result.clear();
    if (host.empty()) {
        return false;
    }

    // 准备 hints
    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = family;     // AF_INET / AF_INET6 / AF_UNSPEC
    hints.ai_socktype = type;     // SOCK_STREAM / SOCK_DGRAM / 0
    hints.ai_protocol = protocol; // IPPROTO_TCP / IPPROTO_UDP / 0
    // 这两个 flags 是常用的组合：AI_ADDRCONFIG 根据本机配置返回地址，AI_V4MAPPED 在需要 IPv6 时允许返回 IPv4 映射
    hints.ai_flags = AI_ADDRCONFIG | AI_V4MAPPED;

    // 处理 host 是否包含 service（端口）
    std::string node;
    const char *service = nullptr;

    // 支持 [ipv6]:port 形式
    if (!host.empty() && host.front() == '[') {
        const char *endipv6 = (const char *)memchr(host.c_str() + 1, ']', host.size() - 1);
        if (endipv6) {
            if (*(endipv6 + 1) == ':') {
                service = endipv6 + 2;
            }
            node = host.substr(1, endipv6 - host.c_str() - 1);
        }
    }

    // 如果不是 [ipv6], 检查是否含有单个 ':' 表示 host:port（但排除 IPv6 多个 ':' 的情况）
    if (node.empty()) {
        const char *pos = (const char *)memchr(host.c_str(), ':', host.size());
        if (pos) {
            // 判断后面是否还有 ':'，若没有则认为是 host:port
            if (!memchr(pos + 1, ':', host.c_str() + host.size() - pos - 1)) {
                node = host.substr(0, pos - host.c_str());
                service = pos + 1;
            }
        }
    }

    if (node.empty()) node = host;

    addrinfo *results = nullptr;
    int error = getaddrinfo(node.c_str(), service, &hints, &results);
    if (error) {
        LOG_DEBUG(g_logger) << "Address::Lookup getaddrinfo(" << host
                            << ") error=" << error << " errmsg=" << gai_strerror(error);
        return false;
    }

    // 遍历链表并转换
    for (addrinfo *ai = results; ai; ai = ai->ai_next) {
        if (ai->ai_addr == nullptr) continue;
        result.push_back(Create(ai->ai_addr, (socklen_t)ai->ai_addrlen));
    }

    freeaddrinfo(results);
    return !result.empty();
}

/////////////////////////////////////////////////////////////////
// LookupAny / LookupAnyIPAddress（便利封装）
/////////////////////////////////////////////////////////////////
Address::ptr Address::LookupAny(const std::string &host, int family, int type, int protocol) {
    std::vector<Address::ptr> addrs;
    if (!Lookup(addrs, host, family, type, protocol)) {
        return nullptr;
    }
    return addrs[0];
}

std::shared_ptr<IPAddress> Address::LookupAnyIPAddress(const std::string &host, int family, int type, int protocol) {
    std::vector<Address::ptr> addrs;
    if (!Lookup(addrs, host, family, type, protocol)) {
        return nullptr;
    }
    for (auto &a : addrs) {
        auto ip = std::dynamic_pointer_cast<IPAddress>(a);
        if (ip) return ip;
    }
    return nullptr;
}

/////////////////////////////////////////////////////////////////
// GetInterfaceAddresses：枚举本机网卡与地址
// - 使用 getifaddrs 获取接口链表
// - 对每个地址根据 family 计算掩码位数并记录
/////////////////////////////////////////////////////////////////
bool Address::GetInterfaceAddresses(std::multimap<std::string, std::pair<Address::ptr, uint32_t> > &result, int family) {
    result.clear();
    struct ifaddrs *next = nullptr;
    struct ifaddrs *results = nullptr;
    if (getifaddrs(&results) != 0) {
        LOG_DEBUG(g_logger) << "Address::GetInterfaceAddresses getifaddrs error="
                            << errno << " errmsg=" << strerror(errno);
        return false;
    }

    try {
        for (next = results; next; next = next->ifa_next) {
            if (!next->ifa_addr) continue;

            if (family != AF_UNSPEC && next->ifa_addr->sa_family != family) continue;

            Address::ptr addr;
            uint32_t prefix_len = ~0u;

            if (next->ifa_addr->sa_family == AF_INET) {
                addr = Create(next->ifa_addr, sizeof(sockaddr_in));
                // netmask 存在时使用 netmask 计算前缀长度（注意 netmask 在 sockaddr_in 中为网络字节序）
                if (next->ifa_netmask) {
                    uint32_t netmask = ntohl(((sockaddr_in *)next->ifa_netmask)->sin_addr.s_addr);
                    prefix_len = CountBytes<uint32_t>(netmask);
                }
            } else if (next->ifa_addr->sa_family == AF_INET6) {
                addr = Create(next->ifa_addr, sizeof(sockaddr_in6));
                if (next->ifa_netmask) {
                    in6_addr mask = ((sockaddr_in6 *)next->ifa_netmask)->sin6_addr;
                    prefix_len = 0;
                    for (int i = 0; i < 16; ++i) {
                        prefix_len += CountBytes<uint8_t>(mask.s6_addr[i]);
                    }
                }
            } else {
                continue;
            }

            if (addr) {
                result.insert(std::make_pair(std::string(next->ifa_name), std::make_pair(addr, prefix_len)));
            }
        }
    } catch (...) {
        freeifaddrs(results);
        LOG_DEBUG(g_logger) << "Address::GetInterfaceAddresses exception";
        return false;
    }

    freeifaddrs(results);
    return !result.empty();
}

/**
 * @brief 获取指定网卡的地址（名字过滤）
 */
bool Address::GetInterfaceAddresses(std::vector<std::pair<Address::ptr, uint32_t> > &result,
                                    const std::string &iface,
                                    int family) {
    result.clear();
    if (iface.empty() || iface == "*") {
        // 特殊值表示“任意”或通配符，返回任意协议族的任意地址占位
        if (family == AF_INET || family == AF_UNSPEC) {
            result.push_back(std::make_pair(Address::ptr(new IPv4Address()), 0u));
        }
        if (family == AF_INET6 || family == AF_UNSPEC) {
            result.push_back(std::make_pair(Address::ptr(new IPv6Address()), 0u));
        }
        return true;
    }

    std::multimap<std::string, std::pair<Address::ptr, uint32_t> > results;
    if (!GetInterfaceAddresses(results, family)) {
        return false;
    }

    auto its = results.equal_range(iface);
    for (auto it = its.first; it != its.second; ++it) {
        result.push_back(it->second);
    }
    return !result.empty();
}

/////////////////////////////////////////////////////////////////
// Address 基类其他方法
/////////////////////////////////////////////////////////////////
int Address::getFamily() const {
    return getAddr()->sa_family;
}

std::string Address::toString() const {
    std::stringstream ss;
    insert(ss);
    return ss.str();
}

bool Address::operator<(const Address &rhs) const {
    socklen_t minlen = std::min(getAddrLen(), rhs.getAddrLen());
    int r = memcmp(getAddr(), rhs.getAddr(), minlen);
    if (r < 0) return true;
    if (r > 0) return false;
    if (getAddrLen() < rhs.getAddrLen()) return true;
    return false;
}

bool Address::operator==(const Address &rhs) const {
    return getAddrLen() == rhs.getAddrLen()
           && memcmp(getAddr(), rhs.getAddr(), getAddrLen()) == 0;
}
bool Address::operator!=(const Address &rhs) const {
    return !(*this == rhs);
}

/////////////////////////////////////////////////////////////////
// IPAddress::Create（只解析数值文本地址，不做 DNS）
// - 使用 getaddrinfo + AI_NUMERICHOST：只解析数值文本地址
// - 返回的 IPAddress 已设置端口（若可设置）
/////////////////////////////////////////////////////////////////
IPAddress::ptr IPAddress::Create(const char *address, uint16_t port) {
    if (!address) return nullptr;
    addrinfo hints, *results = nullptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_flags = AI_NUMERICHOST; // 仅接受数值地址文本（不做 DNS）
    hints.ai_family = AF_UNSPEC;

    int error = getaddrinfo(address, nullptr, &hints, &results);
    if (error) {
        LOG_DEBUG(g_logger) << "IPAddress::Create(" << address << ", " << port
                            << ") getaddrinfo error=" << error << " " << gai_strerror(error);
        return nullptr;
    }

    IPAddress::ptr result = nullptr;
    try {
        Address::ptr a = Address::Create(results->ai_addr, (socklen_t)results->ai_addrlen);
        result = std::dynamic_pointer_cast<IPAddress>(a);
        if (result) result->setPort(port);
    } catch (...) {
        freeaddrinfo(results);
        return nullptr;
    }
    freeaddrinfo(results);
    return result;
}

/////////////////////////////////////////////////////////////////
// IPv4Address 实现
/////////////////////////////////////////////////////////////////
IPv4Address::IPv4Address(const sockaddr_in &address) {
    m_addr = address; // 直接拷贝（注意 sockaddr_in 中字段为网络字节序）
}

IPv4Address::IPv4Address(uint32_t address, uint16_t port) {
    // address / port 是主机字节序（API 约定），在写入 m_addr 时转换为网络字节序
    memset(&m_addr, 0, sizeof(m_addr));
    m_addr.sin_family = AF_INET;
    m_addr.sin_port = htons(port);
    m_addr.sin_addr.s_addr = htonl(address);
}

IPv4Address::ptr IPv4Address::Create(const char *address, uint16_t port) {
    if (!address) return nullptr;
    IPv4Address::ptr rt(new IPv4Address);
    rt->m_addr.sin_family = AF_INET;
    rt->m_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, address, &rt->m_addr.sin_addr) != 1) {
        LOG_DEBUG(g_logger) << "IPv4Address::Create invalid address=" << address
                            << " errno=" << errno << " errstr=" << strerror(errno);
        return nullptr;
    }
    return rt;
}

const sockaddr *IPv4Address::getAddr() const {
    return (const sockaddr *)&m_addr;
}
sockaddr *IPv4Address::getAddr() {
    return (sockaddr *)&m_addr;
}
socklen_t IPv4Address::getAddrLen() const {
    return static_cast<socklen_t>(sizeof(m_addr));
}

std::ostream &IPv4Address::insert(std::ostream &os) const {
    // inet_ntop 更健壮，但这里按你原有风格也可以自定义格式化
    char buf[INET_ADDRSTRLEN] = {0};
    if (inet_ntop(AF_INET, &m_addr.sin_addr, buf, sizeof(buf))) {
        os << buf << ":" << ntohs(m_addr.sin_port);
    } else {
        os << "<invalid-ipv4>";
    }
    return os;
}

static inline uint32_t ipv4_prefix_to_mask(uint32_t prefix_len) {
    if (prefix_len > 32) return 0;
    if (prefix_len == 0) return 0;
    return (prefix_len == 32) ? 0xFFFFFFFFu : (~0u << (32 - prefix_len));
}

IPAddress::ptr IPv4Address::broadcastAddress(uint32_t prefix_len) {
    if (prefix_len > 32) return nullptr;
    uint32_t host_addr = ntohl(m_addr.sin_addr.s_addr);
    uint32_t mask = ipv4_prefix_to_mask(prefix_len);
    uint32_t net = host_addr & mask;
    uint32_t bcast = net | (~mask);
    uint16_t port = ntohs(m_addr.sin_port);
    return std::make_shared<IPv4Address>(bcast, port);
}

IPAddress::ptr IPv4Address::networdAddress(uint32_t prefix_len) {
    if (prefix_len > 32) return nullptr;
    uint32_t host_addr = ntohl(m_addr.sin_addr.s_addr);
    uint32_t mask = ipv4_prefix_to_mask(prefix_len);
    uint32_t net = host_addr & mask;
    uint16_t port = ntohs(m_addr.sin_port);
    return std::make_shared<IPv4Address>(net, port);
}

IPAddress::ptr IPv4Address::subnetMask(uint32_t prefix_len) {
    if (prefix_len > 32) return nullptr;
    uint32_t mask = ipv4_prefix_to_mask(prefix_len);
    // 子网掩码通常没有端口，传入 0
    return std::make_shared<IPv4Address>(mask, 0);
}

uint32_t IPv4Address::getPort() const {
    return static_cast<uint32_t>(ntohs(m_addr.sin_port));
}
void IPv4Address::setPort(uint16_t v) {
    m_addr.sin_port = htons(v);
}

/////////////////////////////////////////////////////////////////
// IPv6Address 实现
/////////////////////////////////////////////////////////////////
IPv6Address::IPv6Address() {
    memset(&m_addr, 0, sizeof(m_addr));
    m_addr.sin6_family = AF_INET6;
    m_addr.sin6_port = 0;
}

IPv6Address::IPv6Address(const sockaddr_in6 &address) {
    m_addr = address; // 直接拷贝（字段已为网络字节序）
}

IPv6Address::IPv6Address(const uint8_t address[16], uint16_t port) {
    memset(&m_addr, 0, sizeof(m_addr));
    m_addr.sin6_family = AF_INET6;
    m_addr.sin6_port = htons(port);
    // 假定 address 为 network-order 字节数组，直接拷贝
    memcpy(&m_addr.sin6_addr.s6_addr, address, 16);
}

IPv6Address::ptr IPv6Address::Create(const char *address, uint16_t port) {
    if (!address) return nullptr;
    IPv6Address::ptr rt(new IPv6Address);
    rt->m_addr.sin6_family = AF_INET6;
    rt->m_addr.sin6_port = htons(port);
    if (inet_pton(AF_INET6, address, &rt->m_addr.sin6_addr) != 1) {
        LOG_DEBUG(g_logger) << "IPv6Address::Create invalid address=" << address
                            << " errno=" << errno << " errstr=" << strerror(errno);
        return nullptr;
    }
    return rt;
}

const sockaddr *IPv6Address::getAddr() const {
    return (const sockaddr *)&m_addr;
}
sockaddr *IPv6Address::getAddr() {
    return (sockaddr *)&m_addr;
}
socklen_t IPv6Address::getAddrLen() const {
    return static_cast<socklen_t>(sizeof(m_addr));
}

std::ostream &IPv6Address::insert(std::ostream &os) const {
    char buf[INET6_ADDRSTRLEN] = {0};
    if (inet_ntop(AF_INET6, &m_addr.sin6_addr, buf, sizeof(buf)) == nullptr) {
        os << "<invalid-ipv6>";
        return os;
    }
    os << "[" << buf;
    // 若有 scope id，尝试把索引转换为接口名并追加
    if (m_addr.sin6_scope_id != 0) {
        char ifname[IF_NAMESIZE] = {0};
        if (if_indextoname(m_addr.sin6_scope_id, ifname) != nullptr) {
            os << "%" << ifname;
        } else {
            os << "%" << m_addr.sin6_scope_id;
        }
    }
    os << "]:" << ntohs(m_addr.sin6_port);
    return os;
}

// IPv6 辅助：根据 prefix_len 生成 16 字节掩码（network-order）
static inline void ipv6_prefix_to_mask_bytes(uint8_t mask[16], uint32_t prefix_len) {
    memset(mask, 0, 16);
    if (prefix_len == 0) return;
    if (prefix_len > 128) prefix_len = 128;
    uint32_t full_bytes = prefix_len / 8;
    uint32_t rem_bits = prefix_len % 8;
    for (uint32_t i = 0; i < full_bytes; ++i) mask[i] = 0xFF;
    if (rem_bits) {
        mask[full_bytes] = static_cast<uint8_t>(0xFF << (8 - rem_bits));
    }
}

IPAddress::ptr IPv6Address::broadcastAddress(uint32_t prefix_len) {
    if (prefix_len > 128) return nullptr;
    uint8_t addr[16];
    memcpy(addr, &m_addr.sin6_addr, 16);
    uint8_t mask[16];
    ipv6_prefix_to_mask_bytes(mask, prefix_len);
    uint8_t net[16], bcast[16];
    for (int i = 0; i < 16; ++i) {
        net[i] = addr[i] & mask[i];
        bcast[i] = net[i] | (~mask[i]);
    }
    sockaddr_in6 baddr;
    memset(&baddr, 0, sizeof(baddr));
    baddr.sin6_family = AF_INET6;
    memcpy(&baddr.sin6_addr.s6_addr, bcast, 16);
    baddr.sin6_port = m_addr.sin6_port;
    baddr.sin6_scope_id = m_addr.sin6_scope_id;
    return std::make_shared<IPv6Address>(baddr);
}

IPAddress::ptr IPv6Address::networdAddress(uint32_t prefix_len) {
    if (prefix_len > 128) return nullptr;
    uint8_t addr[16];
    memcpy(addr, &m_addr.sin6_addr, 16);
    uint8_t mask[16];
    ipv6_prefix_to_mask_bytes(mask, prefix_len);
    uint8_t net[16];
    for (int i = 0; i < 16; ++i) net[i] = addr[i] & mask[i];
    sockaddr_in6 naddr;
    memset(&naddr, 0, sizeof(naddr));
    naddr.sin6_family = AF_INET6;
    memcpy(&naddr.sin6_addr.s6_addr, net, 16);
    naddr.sin6_port = m_addr.sin6_port;
    naddr.sin6_scope_id = m_addr.sin6_scope_id;
    return std::make_shared<IPv6Address>(naddr);
}

IPAddress::ptr IPv6Address::subnetMask(uint32_t prefix_len) {
    if (prefix_len > 128) return nullptr;
    uint8_t mask[16];
    ipv6_prefix_to_mask_bytes(mask, prefix_len);
    sockaddr_in6 saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin6_family = AF_INET6;
    memcpy(&saddr.sin6_addr.s6_addr, mask, 16);
    saddr.sin6_port = 0;
    saddr.sin6_scope_id = 0;
    return std::make_shared<IPv6Address>(saddr);
}

uint32_t IPv6Address::getPort() const {
    return static_cast<uint32_t>(ntohs(m_addr.sin6_port));
}
void IPv6Address::setPort(uint16_t v) {
    m_addr.sin6_port = htons(v);
}

/////////////////////////////////////////////////////////////////
// UnixAddress / UnknownAddress
/////////////////////////////////////////////////////////////////
static const size_t UNIX_PATH_MAX_LEN = sizeof(((sockaddr_un *)0)->sun_path);

UnixAddress::UnixAddress() {
    memset(&m_addr, 0, sizeof(m_addr));
    m_addr.sun_family = AF_UNIX;
    m_length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path));
}

UnixAddress::UnixAddress(const std::string &path) {
    memset(&m_addr, 0, sizeof(m_addr));
    m_addr.sun_family = AF_UNIX;

    // 支持抽象命名空间（Linux）：第一个字节为 '\0'
    if (path.size() > UNIX_PATH_MAX_LEN) {
        throw std::logic_error("Unix socket path too long");
    }

    if (!path.empty() && path[0] == '\0') {
        // 抽象命名空间：复制全部字节（不需要末尾 NUL）
        memcpy(m_addr.sun_path, path.data(), path.size());
        m_length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size());
    } else {
        // 文件系统路径：复制并以 NUL 终止
        strncpy(m_addr.sun_path, path.c_str(), sizeof(m_addr.sun_path) - 1);
        m_length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + strlen(m_addr.sun_path) + 1);
    }
}

const sockaddr *UnixAddress::getAddr() const {
    return (const sockaddr *)&m_addr;
}
sockaddr *UnixAddress::getAddr() {
    return (sockaddr *)&m_addr;
}
socklen_t UnixAddress::getAddrLen() const {
    return m_length;
}
void UnixAddress::setAddrLen(uint32_t v) {
    m_length = v;
}

std::string UnixAddress::getPath() const {
    std::stringstream ss;
    if (m_length > offsetof(sockaddr_un, sun_path) && m_addr.sun_path[0] == '\0') {
        // 抽象命名空间：显示以 "\0" 开头的字符串（可视化）
        ss << "\\0" << std::string(m_addr.sun_path + 1, m_length - offsetof(sockaddr_un, sun_path) - 1);
    } else {
        ss << m_addr.sun_path;
    }
    return ss.str();
}

std::ostream &UnixAddress::insert(std::ostream &os) const {
    if (m_length > offsetof(sockaddr_un, sun_path) && m_addr.sun_path[0] == '\0') {
        os << "\\0" << std::string(m_addr.sun_path + 1, m_length - offsetof(sockaddr_un, sun_path) - 1);
    } else {
        os << m_addr.sun_path;
    }
    return os;
}

UnknownAddress::UnknownAddress(int family) {
    memset(&m_addr, 0, sizeof(m_addr));
    m_addr.sa_family = static_cast<unsigned short>(family);
}
UnknownAddress::UnknownAddress(const sockaddr &addr) {
    m_addr = addr;
}

const sockaddr *UnknownAddress::getAddr() const {
    return (const sockaddr *)&m_addr;
}
sockaddr *UnknownAddress::getAddr() {
    return (sockaddr *)&m_addr;
}
socklen_t UnknownAddress::getAddrLen() const {
    return static_cast<socklen_t>(sizeof(m_addr));
}

std::ostream &UnknownAddress::insert(std::ostream &os) const {
    os << "[UnknownAddress family=" << m_addr.sa_family << "]";
    return os;
}

std::ostream &operator<<(std::ostream &os, const Address &addr) {
    return addr.insert(os);
}

} // namespace sunshine
