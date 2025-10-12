// file: libs/iomanager.h
#pragma once

// 标准库头文件：epoll事件、eventfd、系统调用等
#include "libs/scheduler.h"
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <vector>
#include <memory>
#include <mutex>
#include <functional>
#include <atomic>
#include <errno.h>

namespace sunshine {

// IOManager 类：基于 epoll 的 I/O 多路复用调度器
// 继承自 Scheduler，实现基于事件驱动的 I/O 操作
class IOManager : public Scheduler {
public:
    // 智能指针类型别名
    using ptr = std::shared_ptr<IOManager>;

    // 事件类型枚举（支持读/写事件）
    enum Event : uint32_t {
        NONE = 0x0, // 无事件
        READ = 0x1, // 读事件（EPOLLIN）
        WRITE = 0x4 // 写事件（EPOLLOUT）
    };

    // 构造函数
    // 参数说明：
    // threads: 工作线程数（默认1个，主协程会额外占用1个）
    // use_caller: 是否将调用线程作为主协程（true时主协程在调用线程运行）
    // name: 调度器名称（用于日志标识）
    IOManager(size_t threads = 1, bool use_caller = true, const std::string &name = "");

    // 析构函数（清理 epoll 资源）
    ~IOManager();

    // 添加 I/O 事件监听
    // 参数说明：
    // fd: 文件描述符
    // ev: 事件类型（READ/WRITE）
    // cb: 事件触发时的回调函数
    // 返回值：
    // 0: 成功
    // -1: 失败（设置 errno）
    int addEvent(int fd, Event ev, std::function<void()> cb);

    // 删除事件监听（不触发回调）
    // 参数说明：
    // fd: 文件描述符
    // ev: 事件类型
    // 返回值：是否成功删除
    bool delEvent(int fd, Event ev);

    // 取消事件监听（触发回调）
    // 参数说明：
    // fd: 文件描述符
    // ev: 事件类型
    // 返回值：是否成功取消
    bool cancelEvent(int fd, Event ev);

    // 取消并触发所有注册在 fd 上的事件
    // 参数说明：
    // fd: 文件描述符
    // 返回值：是否成功取消
    bool cancelAll(int fd);

protected:
    // 重写父类 tickle()：使用 eventfd 唤醒 epoll_wait
    void tickle() override;

    // 重写父类 run()：实现 epoll 事件循环
    void run() override;

private:
    // 文件描述符上下文结构体
    // 用于管理单个 fd 的事件注册状态
    struct FdContext {
        using MutexType = std::mutex;

        // 事件上下文（读/写事件各一份）
        struct EventContext {
            Scheduler *scheduler = nullptr; // 关联的调度器
            Fiber::ptr fiber;               // 协程对象
            std::function<void()> cb;       // 回调函数

            // 重置上下文
            void reset() {
                scheduler = nullptr;
                fiber.reset();
                cb = nullptr;
            }
        };

        // 获取指定事件的上下文
        EventContext &getContext(Event ev) {
            if (ev == READ) return read;
            return write;
        }

        // 重置整个上下文
        void reset() {
            read.reset();
            write.reset();
            events = NONE;
        }

        MutexType mutex;          // 保护上下文的互斥锁
        EventContext read, write; // 读/写事件上下文
        int fd = -1;              // 文件描述符
        Event events = NONE;      // 当前注册的事件掩码
    };

    // 确保 m_fdContexts 能索引到指定 fd
    // 扩容容器以容纳 fd（避免索引越界）
    void resizeFdContext(size_t fd);

    // 触发事件回调（由 epoll 事件循环调用）
    // 将事件回调提交到调度器执行
    void triggerEvent(int fd, Event ev);

private:
    int m_epfd{-1};                                       // epoll 文件描述符
    int m_eventfd{-1};                                    // eventfd 文件描述符（用于唤醒 epoll）
    std::vector<std::shared_ptr<FdContext>> m_fdContexts; // fd 上下文容器
    std::mutex m_mutex;                                   // 保护 m_fdContexts 的互斥锁
    std::atomic<size_t> m_pendingEventCount{0};           // 待处理事件计数（用于优化）

    static const int MAX_EVENTS = 1024; // epoll 事件最大数量
};

} // namespace sunshine