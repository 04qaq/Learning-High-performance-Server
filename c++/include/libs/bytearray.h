#ifndef __SUNSHINE_BYTEARRAY_H__
#define __SUNSHINE_BYTEARRAY_H__

#include <memory>
#include <string>
#include <vector>
#include <stdint.h>
#include <sys/types.h>
#include <sys/uio.h> // iovec

namespace sunshine {

class ByteArray {
public:
    using ptr = std::shared_ptr<ByteArray>;

    // 内部存储块（链表节点）
    struct Node {
        Node(size_t s);
        Node();
        ~Node();

        char *ptr;   // 数据缓冲区
        Node *next;  // 下一个块
        size_t size; // 本块大小
    };

    // 构造：指定每个块的大小（默认 4KB）
    ByteArray(size_t base_size = 4096);
    ~ByteArray();

    // ---------- 写接口（固定长度整数，固定字节序写法） ----------
    void writeFint8(const int8_t &value);
    void writeFint16(int16_t value);
    void writeFint32(int32_t value);
    void writeFint64(int64_t value);
    void writeFuint8(uint8_t value);
    void writeFuint16(uint16_t value);
    void writeFuint32(uint32_t value);
    void writeFuint64(uint64_t value);

    // 可变长（varint）/zigzag 等示例（名称与语义保持你原来的）
    void writeInt8(int8_t value);
    void writeInt16(int16_t value);
    void writeInt32(int32_t value);
    void writeInt64(int64_t value);
    void writeUint8(uint8_t value);
    void writeUint16(uint16_t value);
    void writeUint32(uint32_t value);
    void writeUint64(uint64_t value);

    void writeFloat(float value);
    void writeDouble(double value);

    // 字符串写：前面带固定长度（16/32/64位）或可变int长度
    void writeStringF16(const std::string &value);
    void writeStringF32(const std::string &value);
    void writeStringF64(const std::string &value);
    void writeStringVint(const std::string &value);

    void writeStringWithoutLength(const std::string &value);

    // ---------- 读接口 ----------
    int8_t readFint8();
    int16_t readFint16();
    int32_t readFint32();
    int64_t readFint64();
    uint8_t readFuint8();
    uint16_t readFuint16();
    uint32_t readFuint32();
    uint64_t readFuint64();

    int8_t readInt8();
    int16_t readInt16();
    int32_t readInt32();
    int64_t readInt64();
    uint8_t readUint8();
    uint16_t readUint16();
    uint32_t readUint32();
    uint64_t readUint64();

    float readFloat();
    double readDouble();

    std::string readStringF16();
    std::string readStringF32();
    std::string readStringF64();
    std::string readStringVint();

    // 清空（保留根节点）
    void clear();

    // 基本读写（读会移动 m_position）
    void write(const void *buf, size_t size);
    void read(void *buf, size_t size);

    // 从特定 position 读取长度为 size 的数据（不会改变 m_position）
    void read(void *buf, size_t size, size_t position);

    // 当前读写位置
    size_t getPosition() const {
        return m_position;
    }
    void setPosition(size_t v);

    bool writeToFile(const std::string name);
    void readFromFile(const std::string name);

    // 可读长度（从 m_position 开始）
    size_t getReadSize() const {
        return m_size - m_position;
    }
    size_t getBaseSize() const {
        return m_baseSize;
    }

    // 字节序（0 = little, 1 = big）
    bool isLittleEndian() const;
    void setIsLittleEndian(bool val);

    // 方便查看（把可读部分转换为 string / hex）
    std::string toString() const;
    std::string toHexString() const;

    // 把可读/可写内存片段转换成 iovec 数组（便于 writev/readv）
    uint64_t getReadBuffers(std::vector<iovec> &buffers, uint64_t len = ~0ull) const;
    uint64_t getReadBuffers(std::vector<iovec> &buffers, uint64_t len, uint64_t position) const;
    uint64_t getWriteBuffers(std::vector<iovec> &buffers, uint64_t len);

private:
    // 确保还有 size 大小的剩余容量，可扩容；返回是否成功
    bool addCapacity(size_t size);
    // 当前剩余可写容量（不改变任何状态）
    size_t getCapacity() const {
        return m_capacity - m_position;
    }

private:
    size_t m_baseSize; // 每个节点大小
    size_t m_position; // 当前操作位置（全局偏移）
    size_t m_size;     // 当前有效数据总长度（已写入长度）
    size_t m_capacity; // 已分配容量（块数 * baseSize）
    int m_endian;      // 字节序标记（0 小端，1 大端）
    Node *m_cur;       // 当前节点指针（包含 m_position 的节点）
    Node *m_root;      // 根节点
};

} // namespace sunshine

#endif // __SUNSHINE_BYTEARRAY_H__
