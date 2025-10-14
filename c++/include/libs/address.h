/**
 * @filename    address.h
 * @brief       Address模块（对网络地址（如IPv4、IPv6、Unix）的封装）
 * @author      L-ge (完善版 by ChatGPT)
 * @version     0.2
 * @modify      2025-10-13
 */
#ifndef __SYLAR_ADDRESS_H__
#define __SYLAR_ADDRESS_H__

#include <memory>
#include <string>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>
#include <vector>
#include <map>

namespace sunshine {

class IPAddress;

/**
 * @brief   网络地址的基类（抽象类）
 *
 * 说明：
 * - 派生类应保存对应的 sockaddr_xxx 数据（以网络字节序存储），并实现 getAddr/getAddrLen/insert。
 * - 所有对外公开构造器/工厂函数以“主机字节序”为语义（例如构造时传入的 uint32_t address 为 host-order），
 *   内部在存入 sockaddr 时会转换为网络字节序（使用 htonl/htons）。
 */
class Address {
public:
    typedef std::shared_ptr<Address> ptr;

    /**
     * @brief  通过 sockaddr 指针创建 Address
     * @note   根据 sa_family 返回对应子类（IPv4/IPv6/Unknown）
     */
    static Address::ptr Create(const sockaddr *addr, socklen_t addrlen);

    /**
     * @brief   通过 host 地址返回对应条件的所有 Address
     *
     * @param   result      传出值，保存满足条件的 Address
     * @param   host        域名，服务器名等，例如，www.sylar.top[:80] （方括号为可选内容）
     * @param   family      协议族（AF_INET、AF_INET6、AF_UNSPEC 等）
     * @param   type        socket 类型（SOCK_STREAM、SOCK_DGRAM 等），若为 0 则不限制
     * @param   protocol    协议类型（IPPROTO_TCP、IPPROTO_UDP 等），若为 0 则不限制
     * @return  true 表示至少解析到一个地址并填充 result；false 表示解析失败或无结果
     */
    static bool Lookup(std::vector<Address::ptr> &result,
                       const std::string &host,
                       int family = AF_INET,
                       int type = 0,
                       int protocol = 0);

    /**
     * @brief   通过 host 地址返回对应条件的任意 Address（第一个）
     */
    static Address::ptr LookupAny(const std::string &host,
                                  int family = AF_INET,
                                  int type = 0,
                                  int protocol = 0);

    /**
     * @brief   通过 host 地址返回对应条件的任意 IPAddress（第一个 IPAddress）
     */
    static std::shared_ptr<IPAddress> LookupAnyIPAddress(const std::string &host,
                                                         int family = AF_INET,
                                                         int type = 0,
                                                         int protocol = 0);

    /**
     * @brief  返回本机所有网卡的 <网卡名, (地址, 子网掩码位数)> 列表
     *
     * @param   result  传出参数，保存本机所有地址信息
     * @param   family  协议族（AF_INET/AF_INET6/AF_UNSPEC）
     * @return  true 成功并至少返回一个条目，false 出错
     */
    static bool GetInterfaceAddresses(std::multimap<std::string, std::pair<Address::ptr, uint32_t> > &result,
                                      int family = AF_INET);

    /**
     * @brief   获取指定网卡的地址和子网掩码位数
     *
     * @param   result  传出参数，保存该网卡的所有地址
     * @param   iface   网卡名称（如 "eth0"），特殊值 "*" 或空表示所有接口
     */
    static bool GetInterfaceAddresses(std::vector<std::pair<Address::ptr, uint32_t> > &result,
                                      const std::string &iface,
                                      int family = AF_INET);

    virtual ~Address() {
    }

    /**
     * @brief   返回协议族
     */
    int getFamily() const;

    /**
     * @brief   返回sockaddr指针（只读）
     */
    virtual const sockaddr *getAddr() const = 0;
    virtual sockaddr *getAddr() = 0;

    /**
     * @brief   返回sockaddr的长度（例如 sizeof(sockaddr_in)）
     */
    virtual socklen_t getAddrLen() const = 0;

    /**
     * @brief   将地址以可读形式输出到 ostream（子类实现）
     */
    virtual std::ostream &insert(std::ostream &os) const = 0;

    /**
     * @brief   返回可读性的字符串
     */
    std::string toString() const;

    // 比较函数（用于容器排序等）
    bool operator<(const Address &rhs) const;
    bool operator==(const Address &rhs) const;
    bool operator!=(const Address &rhs) const;
};

/**
 * @brief   IP地址的基类（抽象类）
 */
class IPAddress : public Address {
public:
    typedef std::shared_ptr<IPAddress> ptr;

    /**
     * @brief  通过域名/IP 字符串 创建 IPAddress（只尝试数值地址，不做 DNS，若需 DNS 请调用 Address::Lookup）
     *
     * @param   address 域名/IP/服务器名等（数值地址时有效）
     * @param   port    端口号（主机字节序）
     * @return  失败返回 nullptr
     */
    static IPAddress::ptr Create(const char *address, uint16_t port = 0);

    /**
     * @brief   获取该地址的广播地址（IPv4 有意义，IPv6 不存在广播但可返回子网末地址）
     *
     * @param   prefix_len  子网掩码位数
     */
    virtual IPAddress::ptr broadcastAddress(uint32_t prefix_len) = 0;

    /**
     * @brief   获取该地址的网段（network address）
     *
     * @param   prefix_len  子网掩码位数
     */
    virtual IPAddress::ptr networdAddress(uint32_t prefix_len) = 0;

    /**
     * @brief   获取子网掩码对应的地址
     *
     * @param   prefix_len  子网掩码位数
     */
    virtual IPAddress::ptr subnetMask(uint32_t prefix_len) = 0;

    virtual uint32_t getPort() const = 0;
    virtual void setPort(uint16_t v) = 0;
};

/**
 * @brief   IPv4地址类
 */
class IPv4Address : public IPAddress {
public:
    typedef std::shared_ptr<IPv4Address> ptr;

    /**
     * @brief   使用点分十进制地址创建IPv4Address（文本解析）
     *
     * @param   address 点分十进制地址，例如 "192.168.1.1"
     * @param   port    端口号（主机字节序）
     * @return  失败返回 nullptr
     */
    static IPv4Address::ptr Create(const char *address, uint16_t port = 0);

    IPv4Address(const sockaddr_in &address);

    /**
     * @brief   通过二进制地址构造IPv4Address
     *
     * @param   address 二进制地址（host order）
     * @param   port    端口号（host order）
     */
    IPv4Address(uint32_t address = INADDR_ANY, uint16_t port = 0);

    const sockaddr *getAddr() const override;
    sockaddr *getAddr() override;
    socklen_t getAddrLen() const override;
    std::ostream &insert(std::ostream &os) const override;

    IPAddress::ptr broadcastAddress(uint32_t prefix_len) override;
    IPAddress::ptr networdAddress(uint32_t prefix_len) override;
    IPAddress::ptr subnetMask(uint32_t prefix_len) override;
    uint32_t getPort() const override;
    void setPort(uint16_t v) override;

private:
    sockaddr_in m_addr; // 保证以网络字节序存储
};

/**
 * @brief   IPv6地址类
 */
class IPv6Address : public IPAddress {
public:
    typedef std::shared_ptr<IPv6Address> ptr;

    /**
     * @brief   通过IPv6地址字符串构造IPv6Address（文本解析）
     *
     * @param   address IPv6地址字符串
     * @param   port    端口号（host order）
     * @return  失败返回 nullptr
     */
    static IPv6Address::ptr Create(const char *address, uint16_t port = 0);

    IPv6Address();
    IPv6Address(const sockaddr_in6 &address);

    /**
     * @brief   通过IPv6二进制地址构造IPv6Address
     *
     * @param   address[16] IPv6二进制地址（host order 或 network order 均接受，构造内部会归一化）
     * @param   port        端口号（host order）
     */
    IPv6Address(const uint8_t address[16], uint16_t port = 0);

    const sockaddr *getAddr() const override;
    sockaddr *getAddr() override;
    socklen_t getAddrLen() const override;
    std::ostream &insert(std::ostream &os) const override;

    IPAddress::ptr broadcastAddress(uint32_t prefix_len) override;
    IPAddress::ptr networdAddress(uint32_t prefix_len) override;
    IPAddress::ptr subnetMask(uint32_t prefix_len) override;
    uint32_t getPort() const override;
    void setPort(uint16_t v) override;

private:
    sockaddr_in6 m_addr; // 保证以网络字节序存储
};

/**
 * @brief   Unix Socket地址类
 */
class UnixAddress : public Address {
public:
    typedef std::shared_ptr<UnixAddress> ptr;

    UnixAddress();

    /**
     * @brief   通过路径构造UnixAddress
     *
     * @param   path    UnixSocket路径（长度小于 sun_path 大小）
     */
    UnixAddress(const std::string &path);

    const sockaddr *getAddr() const override;
    sockaddr *getAddr() override;
    socklen_t getAddrLen() const override;
    void setAddrLen(uint32_t v);
    std::string getPath() const;
    std::ostream &insert(std::ostream &os) const override;

private:
    sockaddr_un m_addr;
    socklen_t m_length;
};

/**
 * @brief   未知地址类（占位）
 */
class UnknownAddress : public Address {
public:
    typedef std::shared_ptr<UnknownAddress> ptr;

    UnknownAddress(int family);
    UnknownAddress(const sockaddr &addr);

    const sockaddr *getAddr() const override;
    sockaddr *getAddr() override;
    socklen_t getAddrLen() const override;
    std::ostream &insert(std::ostream &os) const override;

private:
    sockaddr m_addr;
};

/**
 * @brief   流式输出Address
 */
std::ostream &operator<<(std::ostream &os, const Address &addr);

} // namespace sunshine

#endif
