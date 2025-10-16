#include "libs/bytearray.h"
#include <boost/endian/conversion.hpp>
#include <bit> // std::endian
#include <fstream>
#include <stdexcept>
#include <cstring> // memcpy
#include <sstream>
#include <iomanip>
#include <cmath> // ceil
#include <algorithm>

namespace sunshine {

/* ========================= Node 实现 ========================= */

ByteArray::Node::Node(size_t s) :
    ptr(new char[s]), size(s), next(nullptr) {
}

ByteArray::Node::Node() :
    ptr(nullptr), size(0), next(nullptr) {
}

ByteArray::Node::~Node() {
    if (ptr) {
        delete[] ptr;
        ptr = nullptr;
    }
}

/* ========================= ByteArray 构造/析构 ========================= */

ByteArray::ByteArray(size_t base_size) :
    m_size(0),
    m_capacity(base_size),
    m_baseSize(base_size),
    m_position(0),
    m_endian((std::endian::native == std::endian::little) ? 0 : 1),
    m_root(new Node(base_size)) {
    m_cur = m_root;
    // 构造时分配一个根节点，m_capacity = base_size
}

ByteArray::~ByteArray() {
    Node *tmp = m_root;
    while (tmp) {
        Node *n = tmp;
        tmp = tmp->next;
        delete n;
    }
    m_root = nullptr;
    m_cur = nullptr;
}

/* ========================= 字节序 辅助函数 =========================
   说明：
   - 这个模板函数的目标行为是：当主机字节序与目标端序不同时对整数做字节交换。
   - 该函数对读/写都适用（交换是对称的）。
   - 仅支持常见宽度（1/2/4/8），1 字节无需处理。
*/
template <typename T>
inline T convert_native_to_target_endian_if_needed(T value, int target_endian) noexcept {
    static_assert(std::is_integral<T>::value, "T must be integral");

    if constexpr (sizeof(T) == 1) {
        // 1 字节无需转换
        (void)target_endian;
        return value;
    } else {
        using U = typename std::make_unsigned<T>::type;
        U u = static_cast<U>(value);

        constexpr bool host_is_big = (std::endian::native == std::endian::big);
        int host_flag = host_is_big ? 1 : 0;
        if (host_flag == target_endian) {
            // 目标字节序与宿主一致，不做任何操作
            return value;
        }

        // 目标与主机不同：执行转换（使用 boost.endian 提供的高效函数）
        if (target_endian == 0) { // 目标为 little
            if constexpr (sizeof(U) == 2) {
                u = boost::endian::native_to_little(static_cast<uint16_t>(u));
            } else if constexpr (sizeof(U) == 4) {
                u = boost::endian::native_to_little(static_cast<uint32_t>(u));
            } else if constexpr (sizeof(U) == 8) {
                u = boost::endian::native_to_little(static_cast<uint64_t>(u));
            } else {
                // 罕见宽度的通用字节反转实现
                U tmp = 0;
                unsigned char *src = reinterpret_cast<unsigned char *>(&u);
                unsigned char *dst = reinterpret_cast<unsigned char *>(&tmp);
                for (size_t i = 0; i < sizeof(U); ++i) dst[i] = src[sizeof(U) - 1 - i];
                u = tmp;
            }
        } else { // 目标为 big
            if constexpr (sizeof(U) == 2) {
                u = boost::endian::native_to_big(static_cast<uint16_t>(u));
            } else if constexpr (sizeof(U) == 4) {
                u = boost::endian::native_to_big(static_cast<uint32_t>(u));
            } else if constexpr (sizeof(U) == 8) {
                u = boost::endian::native_to_big(static_cast<uint64_t>(u));
            } else {
                U tmp = 0;
                unsigned char *src = reinterpret_cast<unsigned char *>(&u);
                unsigned char *dst = reinterpret_cast<unsigned char *>(&tmp);
                for (size_t i = 0; i < sizeof(U); ++i) dst[i] = src[sizeof(U) - 1 - i];
                u = tmp;
            }
        }

        return static_cast<T>(u);
    }
}

/* ========================= 写方法实现（固定宽度、varint、浮点、字符串） ========================= */

void ByteArray::writeFint8(const int8_t &value) {
    write(&value, sizeof(value));
}
void ByteArray::writeFuint8(uint8_t value) {
    write(&value, sizeof(value));
}

void ByteArray::writeFint16(int16_t value) {
    value = convert_native_to_target_endian_if_needed(value, m_endian);
    write(&value, sizeof(value));
}
void ByteArray::writeFuint16(uint16_t value) {
    value = convert_native_to_target_endian_if_needed(value, m_endian);
    write(&value, sizeof(value));
}
void ByteArray::writeFint32(int32_t value) {
    value = convert_native_to_target_endian_if_needed(value, m_endian);
    write(&value, sizeof(value));
}
void ByteArray::writeFuint32(uint32_t value) {
    value = convert_native_to_target_endian_if_needed(value, m_endian);
    write(&value, sizeof(value));
}
void ByteArray::writeFint64(int64_t value) {
    value = convert_native_to_target_endian_if_needed(value, m_endian);
    write(&value, sizeof(value));
}
void ByteArray::writeFuint64(uint64_t value) {
    value = convert_native_to_target_endian_if_needed(value, m_endian);
    write(&value, sizeof(value));
}

/* ZigZag 与 varint 编码（用于写可变长度整数） */
static uint32_t EncodeZigzag32(int32_t v) {
    // Zigzag 映射：0->0, -1->1, 1->2, -2->3, 2->4 ...
    return (static_cast<uint32_t>(v) << 1) ^ static_cast<uint32_t>(v >> 31);
}
static uint64_t EncodeZigzag64(int64_t v) {
    return (static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63);
}
static int32_t DecodeZigzag32(uint32_t v) {
    return static_cast<int32_t>((v >> 1) ^ (~(v & 1) + 1));
}
static int64_t DecodeZigzag64(uint64_t v) {
    return static_cast<int64_t>((v >> 1) ^ (~(v & 1) + 1));
}

void ByteArray::writeInt8(int8_t value) {
    writeFint8(value);
}
void ByteArray::writeInt16(int16_t value) {
    writeFint16(value);
}
void ByteArray::writeInt32(int32_t value) {
    writeUint32(EncodeZigzag32(value));
}
void ByteArray::writeInt64(int64_t value) {
    writeUint64(EncodeZigzag64(value));
}

void ByteArray::writeUint8(uint8_t value) {
    writeFuint8(value);
}
void ByteArray::writeUint16(uint16_t value) {
    writeFuint16(value);
}

void ByteArray::writeUint32(uint32_t value) {
    // varint (7-bit per byte)
    uint8_t tmp[5];
    int i = 0;
    while (value >= 0x80) {
        tmp[i++] = static_cast<uint8_t>((value & 0x7f) | 0x80);
        value >>= 7;
    }
    tmp[i++] = static_cast<uint8_t>(value);
    write(tmp, i);
}

void ByteArray::writeUint64(uint64_t value) {
    uint8_t tmp[10];
    int i = 0;
    while (value >= 0x80) {
        tmp[i++] = static_cast<uint8_t>((value & 0x7f) | 0x80);
        value >>= 7;
    }
    tmp[i++] = static_cast<uint8_t>(value);
    write(tmp, i);
}

void ByteArray::writeFloat(float value) {
    uint32_t v;
    std::memcpy(&v, &value, sizeof(value));
    writeFuint32(v);
}
void ByteArray::writeDouble(double value) {
    uint64_t v;
    std::memcpy(&v, &value, sizeof(value));
    writeFuint64(v);
}

void ByteArray::writeStringF16(const std::string &value) {
    writeFuint16(static_cast<uint16_t>(value.size()));
    write(value.data(), value.size());
}
void ByteArray::writeStringF32(const std::string &value) {
    writeFuint32(static_cast<uint32_t>(value.size()));
    write(value.data(), value.size());
}
void ByteArray::writeStringF64(const std::string &value) {
    writeFuint64(static_cast<uint64_t>(value.size()));
    write(value.data(), value.size());
}
void ByteArray::writeStringVint(const std::string &value) {
    writeUint64(static_cast<uint64_t>(value.size()));
    write(value.data(), value.size());
}
void ByteArray::writeStringWithoutLength(const std::string &value) {
    write(value.data(), value.size());
}

/* ========================= 读方法实现 ========================= */

int8_t ByteArray::readFint8() {
    int8_t v;
    read(&v, sizeof(v));
    return v;
}
int16_t ByteArray::readFint16() {
    int16_t v;
    read(&v, sizeof(v));
    v = convert_native_to_target_endian_if_needed(v, m_endian);
    return v;
}
int32_t ByteArray::readFint32() {
    int32_t v;
    read(&v, sizeof(v));
    v = convert_native_to_target_endian_if_needed(v, m_endian);
    return v;
}
int64_t ByteArray::readFint64() {
    int64_t v;
    read(&v, sizeof(v));
    v = convert_native_to_target_endian_if_needed(v, m_endian);
    return v;
}
uint8_t ByteArray::readFuint8() {
    uint8_t v;
    read(&v, sizeof(v));
    return v;
}
uint16_t ByteArray::readFuint16() {
    uint16_t v;
    read(&v, sizeof(v));
    v = convert_native_to_target_endian_if_needed(v, m_endian);
    return v;
}
uint32_t ByteArray::readFuint32() {
    uint32_t v;
    read(&v, sizeof(v));
    v = convert_native_to_target_endian_if_needed(v, m_endian);
    return v;
}
uint64_t ByteArray::readFuint64() {
    uint64_t v;
    read(&v, sizeof(v));
    v = convert_native_to_target_endian_if_needed(v, m_endian);
    return v;
}

int8_t ByteArray::readInt8() {
    return readFint8();
}
int16_t ByteArray::readInt16() {
    return readFint16();
}
int32_t ByteArray::readInt32() {
    return DecodeZigzag32(readFuint32());
}
int64_t ByteArray::readInt64() {
    return DecodeZigzag64(readFuint64());
}

uint8_t ByteArray::readUint8() {
    return readFuint8();
}
uint16_t ByteArray::readUint16() {
    return readFuint16();
}
uint32_t ByteArray::readUint32() {
    uint32_t result = 0;
    for (int i = 0; i < 32; i += 7) {
        uint8_t b = readFuint8();
        if (b < 0x80) {
            result |= (static_cast<uint32_t>(b) << i);
            break;
        } else {
            result |= (static_cast<uint32_t>(b & 0x7f) << i);
        }
    }
    return result;
}
uint64_t ByteArray::readUint64() {
    uint64_t result = 0;
    for (int i = 0; i < 64; i += 7) {
        uint8_t b = readFuint8();
        if (b < 0x80) {
            result |= (static_cast<uint64_t>(b) << i);
            break;
        } else {
            result |= (static_cast<uint64_t>(b & 0x7f) << i);
        }
    }
    return result;
}

float ByteArray::readFloat() {
    uint32_t v = readFuint32();
    float f;
    std::memcpy(&f, &v, sizeof(f));
    return f;
}
double ByteArray::readDouble() {
    uint64_t v = readFuint64();
    double d;
    std::memcpy(&d, &v, sizeof(d));
    return d;
}

std::string ByteArray::readStringF16() {
    uint16_t len = readFuint16();
    // 安全检查：避免出现非常大的长度导致 OOM，可按需在外层配置阈值
    std::string s;
    s.resize(len);
    if (len) read(&s[0], len);
    return s;
}
std::string ByteArray::readStringF32() {
    uint32_t len = readFuint32();
    std::string s;
    s.resize(len);
    if (len) read(&s[0], len);
    return s;
}
std::string ByteArray::readStringF64() {
    uint64_t len = readFuint64();
    std::string s;
    s.resize(len);
    if (len) read(&s[0], len);
    return s;
}
std::string ByteArray::readStringVint() {
    uint64_t len = readUint64();
    std::string s;
    s.resize(len);
    if (len) read(&s[0], len);
    return s;
}

/* ========================= clear / write / read（基于块链） ========================= */

void ByteArray::clear() {
    // 重置位置与已用长度、保留根节点、删除后续节点
    m_position = 0;
    m_size = 0;
    m_capacity = m_baseSize;

    Node *tmp = m_root->next;
    while (tmp) {
        Node *n = tmp;
        tmp = tmp->next;
        delete n;
    }
    m_root->next = nullptr;
    m_cur = m_root;
}

/*
 * write : 把 buf 中 size 字节写入 ByteArray，写入从 m_position 开始并把 m_position 前移
 * 关键点：
 *  - 先调用 addCapacity(size) 确保有足够的剩余空间（会扩容链表）
 *  - 在当前块内做分段 memcpy，写满当前块后移动到下一个块
 */
void ByteArray::write(const void *buf, size_t size) {
    if (buf == nullptr || size == 0) return;

    // 确保有容量
    if (!addCapacity(size)) {
        throw std::runtime_error("ByteArray::write addCapacity failed");
    }

    const uint8_t *src = reinterpret_cast<const uint8_t *>(buf);
    size_t npos = m_position % m_baseSize; // 当前块内偏移
    size_t ncap = m_cur->size - npos;      // 当前块剩余可写字节
    size_t bpos = 0;                       // src 已写偏移

    while (size > 0) {
        if (!m_cur) throw std::runtime_error("ByteArray::write: null current node");

        uint8_t *dst = reinterpret_cast<uint8_t *>(m_cur->ptr) + npos;
        size_t to_copy = std::min(ncap, size);
        std::memcpy(dst, src + bpos, to_copy);

        m_position += to_copy;
        bpos += to_copy;
        size -= to_copy;

        if (to_copy == ncap) {
            // 当前块写满，移动到下一个块
            m_cur = m_cur->next;
            npos = 0;
            ncap = (m_cur ? m_cur->size : 0);
        } else {
            // 仅写入部分当前块，更新块内偏移
            npos += to_copy;
            ncap -= to_copy;
        }
    }

    if (m_position > m_size) {
        m_size = m_position;
    }
}

/*
 * read : 从 m_position 处读取 size 字节到 buf，读取后 m_position 前移
 * 关键点：
 *  - 检查可读长度（m_size - m_position）
 *  - 从当前块开始分段 memcpy 到目标缓冲
 */
void ByteArray::read(void *buf, size_t size) {
    if (buf == nullptr) {
        if (size == 0) return;
        throw std::invalid_argument("ByteArray::read buf is null");
    }
    if (size == 0) return;

    if (size > getReadSize()) {
        throw std::out_of_range("ByteArray::read: not enough data");
    }

    uint8_t *dst = reinterpret_cast<uint8_t *>(buf);
    if (!m_cur) throw std::runtime_error("ByteArray::read: null current node");

    size_t npos = m_position % m_baseSize;
    size_t ncap = m_cur->size - npos;
    size_t bpos = 0;

    while (size > 0) {
        if (!m_cur) throw std::runtime_error("ByteArray::read: null current node inside loop");

        const uint8_t *src = reinterpret_cast<const uint8_t *>(m_cur->ptr) + npos;
        size_t to_copy = std::min(ncap, size);
        std::memcpy(dst + bpos, src, to_copy);

        m_position += to_copy;
        bpos += to_copy;
        size -= to_copy;

        if (to_copy == ncap) {
            m_cur = m_cur->next;
            npos = 0;
            ncap = (m_cur ? m_cur->size : 0);
        } else {
            npos += to_copy;
            ncap -= to_copy;
        }
    }
}

/*
 * 从指定 position 开始读取 size 字节到 buf（不会更改 m_position、m_cur、m_size）
 * 实现说明：
 *  - 首先校验 position 与 size 合法性
 *  - 根据 position 定位到对应的块（从 m_root 开始走 position / baseSize 步）
 *  - 然后像流式读取那样分段 memcpy 到 buf
 */
void ByteArray::read(void *buf, size_t size, size_t position) {
    if (buf == nullptr) {
        if (size == 0) return;
        throw std::invalid_argument("ByteArray::read(position): buf is null");
    }
    if (size == 0) return;

    if (position >= m_size || size > (m_size - position)) {
        throw std::out_of_range("ByteArray::read(position): out of range");
    }

    uint8_t *dst = reinterpret_cast<uint8_t *>(buf);
    size_t npos = position % m_baseSize;
    size_t idx = position / m_baseSize;

    // 从根节点开始定位到包含 position 的节点
    Node *cur = m_root;
    while (idx > 0 && cur) {
        cur = cur->next;
        --idx;
    }
    if (!cur) throw std::out_of_range("ByteArray::read(position): invalid position (no node)");

    size_t ncap = cur->size - npos;
    size_t bpos = 0;

    while (size > 0) {
        if (!cur) throw std::out_of_range("ByteArray::read(position): unexpected end of nodes");

        const uint8_t *src = reinterpret_cast<const uint8_t *>(cur->ptr) + npos;
        size_t to_copy = std::min(ncap, size);
        std::memcpy(dst + bpos, src, to_copy);

        position += to_copy;
        bpos += to_copy;
        size -= to_copy;

        if (to_copy == ncap) {
            cur = cur->next;
            npos = 0;
            ncap = (cur ? cur->size : 0);
        } else {
            npos += to_copy;
            ncap -= to_copy;
        }
    }
}

/* ========== setPosition ========== */
/*
 * setPosition：设置全局 position（类似文件指针）
 * - 允许设置到 [0, m_size]，并把 m_cur 指向包含该位置的节点
 * - 若想禁止跳过已写入范围，可改为检查 v <= m_size 而非 <= m_capacity
 */
void ByteArray::setPosition(size_t v) {
    if (v > m_capacity) {
        throw std::out_of_range("setPosition out of range (beyond capacity)");
    }
    // 更新位置
    m_position = v;
    if (m_position > m_size) {
        m_size = m_position; // 如果设置在当前已写区之外则把 size 扩展到 position（类似文件 seek+truncate behavior）
    }

    // 重新计算 m_cur：从根节点出发，跳过完整的块
    m_cur = m_root;
    size_t pos = v;
    while (m_cur && pos >= m_cur->size) {
        pos -= m_cur->size;
        m_cur = m_cur->next;
    }
    // 如果 pos == m_cur->size，表示位置刚好在当前块末尾，应把 m_cur 指向下一个块（下一次写将从下一块开始）
    if (m_cur && pos == m_cur->size) {
        m_cur = m_cur->next;
    }
}

/* ========================= 文件 IO ========================= */

/*
 * writeToFile: 把从当前 m_position 开始的可读数据写入文件（覆盖写）
 * - 返回是否成功（true 表示写入完成）
 */
bool ByteArray::writeToFile(const std::string name) {
    std::ofstream ofs(name, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;

    // 从当前 m_position 开始写
    uint64_t read_size = getReadSize();
    uint64_t pos = m_position;
    Node *cur = m_cur;

    // 若 m_cur 为 nullptr（例如 position 指向末尾），需要根据 position 定位到对应节点
    if (!cur && read_size > 0) {
        // 定位到 position 对应节点
        size_t idx = pos / m_baseSize;
        cur = m_root;
        while (idx > 0 && cur) {
            cur = cur->next;
            --idx;
        }
        if (!cur && read_size > 0) return false;
    }

    while (read_size > 0 && cur) {
        size_t diff = pos % m_baseSize;
        size_t max_here = cur->size - diff;
        size_t len = (read_size > max_here) ? max_here : read_size;
        ofs.write(cur->ptr + diff, static_cast<std::streamsize>(len));
        if (!ofs) return false;
        pos += len;
        read_size -= len;
        cur = cur->next;
    }
    return true;
}

/*
 * readFromFile: 从文件中读数据并 append（写入到 ByteArray 末尾）
 * - 注意：该函数会把文件全部读入，并把数据写入到当前 ByteArray（增加容量）
 */
void ByteArray::readFromFile(const std::string name) {
    std::ifstream ifs(name, std::ios::binary);
    if (!ifs) {
        throw std::runtime_error("ByteArray::readFromFile open file failed");
    }

    // 临时缓冲用于分块读取
    std::unique_ptr<char[]> buff(new char[m_baseSize]);
    while (!ifs.eof()) {
        ifs.read(buff.get(), static_cast<std::streamsize>(m_baseSize));
        std::streamsize n = ifs.gcount();
        if (n > 0) {
            write(buff.get(), static_cast<size_t>(n));
        }
    }
}

/* ========================= iovec helpers（零拷贝接口） ========================= */

uint64_t ByteArray::getReadBuffers(std::vector<iovec> &buffers, uint64_t len) const {
    // 从当前 m_position 开始最多取 len 字节的可读数据
    uint64_t avail = getReadSize();
    if (len > avail) len = avail;
    if (len == 0) return 0;

    size_t npos = m_position % m_baseSize;
    size_t ncap = (m_cur ? (m_cur->size - npos) : 0);
    Node *cur = m_cur;
    uint64_t ret = len;

    while (len > 0 && cur) {
        iovec iov;
        if (ncap >= len) {
            iov.iov_base = cur->ptr + npos;
            iov.iov_len = len;
            len = 0;
        } else {
            iov.iov_base = cur->ptr + npos;
            iov.iov_len = ncap;
            len -= ncap;
            cur = cur->next;
            if (cur) ncap = cur->size;
            npos = 0;
        }
        buffers.push_back(iov);
    }
    return ret;
}

uint64_t ByteArray::getReadBuffers(std::vector<iovec> &buffers, uint64_t len, uint64_t position) const {
    // 从指定 position 开始取 len 字节（不会修改 m_position）
    if (position >= m_size) return 0;
    uint64_t avail = m_size - position;
    if (len > avail) len = avail;
    if (len == 0) return 0;

    size_t npos = position % m_baseSize;
    size_t idx = position / m_baseSize;
    Node *cur = m_root;
    while (idx > 0 && cur) {
        cur = cur->next;
        --idx;
    }
    if (!cur) return 0;

    uint64_t ret = len;
    size_t ncap = cur->size - npos;
    while (len > 0 && cur) {
        iovec iov;
        if (ncap >= len) {
            iov.iov_base = cur->ptr + npos;
            iov.iov_len = len;
            len = 0;
        } else {
            iov.iov_base = cur->ptr + npos;
            iov.iov_len = ncap;
            len -= ncap;
            cur = cur->next;
            if (cur) ncap = cur->size;
            npos = 0;
        }
        buffers.push_back(iov);
    }
    return ret;
}

uint64_t ByteArray::getWriteBuffers(std::vector<iovec> &buffers, uint64_t len) {
    if (len == 0) return 0;
    // 确保有足够写入空间
    addCapacity(len);

    uint64_t ret = len;
    size_t npos = m_position % m_baseSize;
    size_t ncap = m_cur ? (m_cur->size - npos) : 0;
    Node *cur = m_cur;

    while (len > 0 && cur) {
        iovec iov;
        if (ncap >= len) {
            iov.iov_base = cur->ptr + npos;
            iov.iov_len = len;
            len = 0;
        } else {
            iov.iov_base = cur->ptr + npos;
            iov.iov_len = ncap;
            len -= ncap;
            cur = cur->next;
            if (cur) ncap = cur->size;
            npos = 0;
        }
        buffers.push_back(iov);
    }
    return ret;
}

/* ========================= 扩容实现 ========================= */

/*
 * addCapacity(size):
 * - 确保剩余可写容量 >= size
 * - 若需要扩容则追加 Node，更新 m_capacity
 * - 如果原剩余容量为 0（即当前 m_cur 已满且 m_cur 指向末尾），把 m_cur 指向新追加的第一块
 * 返回值：成功返回 true（若 new 抛出异常则异常外抛）
 */
bool ByteArray::addCapacity(size_t size) {
    if (size == 0) return true;

    size_t old_cap = getCapacity();
    if (old_cap >= size) return true; // 已够用

    size_t need = size - old_cap; // 还需要的字节数
    // 需要的块数（整数上取整）
    size_t count = (need + m_baseSize - 1) / m_baseSize;

    // 找到链表尾部（注意这里没有维护尾指针，遍历一次）
    Node *tail = m_root;
    while (tail->next) tail = tail->next;

    Node *first_new = nullptr;
    for (size_t i = 0; i < count; ++i) {
        Node *n = new Node(m_baseSize);
        tail->next = n;
        tail = n;
        if (!first_new) first_new = n;
        m_capacity += m_baseSize;
    }

    // 如果原来剩余容量为 0，则 m_cur 可能指向末尾（或为 nullptr），
    // 此时把 m_cur 指向新追加的第一块以便写操作直接写入。
    if (old_cap == 0) {
        if (first_new) m_cur = first_new;
    }
    return true;
}

/* ========================= 辅助与转换函数 ========================= */

bool ByteArray::isLittleEndian() const {
    return m_endian == 0;
}
void ByteArray::setIsLittleEndian(bool val) {
    m_endian = val ? 0 : 1;
}

/* toString: 把可读部分（从 m_position 开始）拷贝成 std::string（不会移动 m_position） */
std::string ByteArray::toString() const {
    size_t len = getReadSize();
    if (len == 0) return std::string();

    std::string s;
    s.resize(len);
    // 使用 const_cast 调用非 const 的 read(pos) 实现或者手动实现读取逻辑（下面直接复用 read(pos) 的思路）
    size_t npos = m_position % m_baseSize;
    size_t idx = m_position / m_baseSize;
    Node *cur = m_root;
    // 定位
    while (idx > 0 && cur) {
        cur = cur->next;
        --idx;
    }
    if (!cur) return std::string();

    size_t bpos = 0;
    size_t ncap = cur->size - npos;
    size_t need = len;
    while (need > 0 && cur) {
        size_t to_copy = std::min(ncap, need);
        memcpy(&s[bpos], cur->ptr + npos, to_copy);
        bpos += to_copy;
        need -= to_copy;
        if (to_copy == ncap) {
            cur = cur->next;
            npos = 0;
            if (cur) ncap = cur->size;
        } else {
            npos += to_copy;
            ncap -= to_copy;
        }
    }
    return s;
}

/* toHexString: 把可读部分格式化为 HEX（每 32 字节换行） */
std::string ByteArray::toHexString() const {
    std::string s = toString();
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (size_t i = 0; i < s.size(); ++i) {
        if (i > 0 && (i % 32) == 0) ss << '\n';
        ss << std::setw(2) << (static_cast<int>(static_cast<uint8_t>(s[i])));
        if ((i + 1) % 32 != 0) ss << ' ';
    }
    return ss.str();
}

} // namespace sunshine
