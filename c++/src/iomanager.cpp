// file: libs/iomanager.cpp
#include "libs/iomanager.h"
#include <sys/socket.h>
#include <fcntl.h>
#include <string.h>
#include <iostream>
#include "libs/log.h"

namespace sunshine {

// 设置文件描述符为非阻塞模式
// 返回值：0 成功，-1 失败（设置 errno）
static int setNonBlock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    // 添加 O_NONBLOCK 标志
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) return -1;
    return 0;
}

// 构造函数：初始化 epoll 和 eventfd
// 1. 创建 epoll 实例（epoll_create1）
// 2. 创建 eventfd（用于唤醒 epoll_wait）
// 3. 将 eventfd 注册到 epoll 监听可读事件
// 4. 预分配 fd 上下文容器（128 个元素）
IOManager::IOManager(size_t threads, bool use_caller, const std::string &name) :
    Scheduler(threads, use_caller, name) {
    m_epfd = epoll_create1(EPOLL_CLOEXEC);
    if (m_epfd == -1) {
        perror("epoll_create1");
        throw std::runtime_error("epoll_create1 failed");
    }
    m_eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (m_eventfd == -1) {
        close(m_epfd);
        perror("eventfd");
        throw std::runtime_error("eventfd failed");
    }

    // 注册 eventfd 到 epoll（监听可读事件）
    epoll_event ev{};
    ev.events = EPOLLIN;   // 监听可读事件
    ev.data.ptr = nullptr; // 特殊标记：nullptr 表示 eventfd
    if (epoll_ctl(m_epfd, EPOLL_CTL_ADD, m_eventfd, &ev) == -1) {
        perror("epoll_ctl add eventfd");
        close(m_epfd);
        close(m_eventfd);
        throw std::runtime_error("epoll_ctl add eventfd failed");
    }

    // 预分配 fd 上下文容器（避免动态扩容）
    m_fdContexts.resize(128);
}

// 析构函数：清理 epoll 和 eventfd 资源
// 确保调度器停止后关闭文件描述符
IOManager::~IOManager() {
    stop(); // 先停止调度器
    if (m_epfd != -1) close(m_epfd);
    if (m_eventfd != -1) close(m_eventfd);
}

// 扩容 fd 上下文容器（确保能索引到指定 fd）
// 1. 通过 m_mutex 保护 m_fdContexts
// 2. 如果 fd 超过当前容器大小，则扩容至 2 倍
// 3. 如果当前 fd 上下文不存在，则创建
void IOManager::resizeFdContext(size_t fd) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (fd >= m_fdContexts.size()) {
        size_t newsz = m_fdContexts.size();
        while (fd >= newsz) newsz *= 2; // 扩容至 2 倍
        m_fdContexts.resize(newsz);
    }
    if (!m_fdContexts[fd]) {
        m_fdContexts[fd] = std::make_shared<FdContext>();
        m_fdContexts[fd]->fd = (int)fd;
    }
}

// 添加 I/O 事件监听
// 返回值：0 成功，-1 失败（设置 errno）
int IOManager::addEvent(int fd, Event ev, std::function<void()> cb) {
    if (fd < 0) {
        errno = EINVAL;
        return -1;
    }
    resizeFdContext(fd); // 确保 fd 上下文存在
    auto ctx = m_fdContexts[fd];
    std::lock_guard<std::mutex> lock(ctx->mutex);

    // 检查是否已注册相同事件
    if ((ctx->events & ev) != 0) {
        errno = EEXIST;
        return -1;
    }

    // 确定 epoll 操作类型（ADD 或 MOD）
    int op = ctx->events == NONE ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
    epoll_event epevent{};
    uint32_t events = 0;

    // 当前已注册事件
    if (ctx->events & READ) events |= EPOLLIN;
    if (ctx->events & WRITE) events |= EPOLLOUT;

    // 新增事件
    if (ev & READ) events |= EPOLLIN;
    if (ev & WRITE) events |= EPOLLOUT;

    // 设置 epoll 事件标志（ET 模式 + 错误处理）
    events |= EPOLLET | EPOLLERR | EPOLLHUP;
    epevent.events = events;
    epevent.data.ptr = ctx.get(); // 关联上下文指针

    // 注册到 epoll
    if (epoll_ctl(m_epfd, op, fd, &epevent) == -1) {
        // 处理 epoll_ctl 竞争条件（ADD vs MOD）
        if (op == EPOLL_CTL_ADD && errno == EEXIST) {
            // 改为 MOD 操作
            if (epoll_ctl(m_epfd, EPOLL_CTL_MOD, fd, &epevent) == -1) {
                return -1;
            }
        } else if (op == EPOLL_CTL_MOD && errno == ENOENT) {
            // 改为 ADD 操作
            if (epoll_ctl(m_epfd, EPOLL_CTL_ADD, fd, &epevent) == -1) {
                return -1;
            }
        } else {
            return -1;
        }
    }

    // 更新上下文事件状态
    ctx->events = static_cast<Event>(ctx->events | ev);
    auto &ectx = ctx->getContext(ev);
    ectx.scheduler = this; // 关联调度器
    if (cb)
        ectx.cb = std::move(cb); // 保存回调
    else
        ectx.cb = nullptr;

    // 更新待处理事件计数
    m_pendingEventCount.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

// 删除事件监听（不触发回调）
bool IOManager::delEvent(int fd, Event ev) {
    if (fd < 0) return false;
    if (fd >= (int)m_fdContexts.size()) return false;
    auto ctx = m_fdContexts[fd];
    if (!ctx) return false;

    std::lock_guard<std::mutex> lock(ctx->mutex);
    if ((ctx->events & ev) == 0) return false; // 未注册

    // 计算新事件掩码
    Event newEvents = static_cast<Event>(ctx->events & ~ev);
    uint32_t events = 0;
    if (newEvents & READ) events |= EPOLLIN;
    if (newEvents & WRITE) events |= EPOLLOUT;
    events |= EPOLLET | EPOLLERR | EPOLLHUP;

    epoll_event epevent{};
    epevent.events = events;
    epevent.data.ptr = ctx.get();

    // 确定 epoll 操作
    int op = (newEvents == NONE) ? EPOLL_CTL_DEL : EPOLL_CTL_MOD;
    // 删除操作时传 nullptr
    if (epoll_ctl(m_epfd, op, fd, (op == EPOLL_CTL_DEL) ? nullptr : &epevent) == -1) {
        // 忽略 ENOENT 错误（已删除）
        if (!(op == EPOLL_CTL_DEL && errno == ENOENT)) {
            // 其他错误打印日志（实际可忽略）
        }
    }

    // 更新上下文
    ctx->events = newEvents;
    ctx->getContext(ev).reset(); // 重置上下文
    m_pendingEventCount.fetch_sub(1, std::memory_order_relaxed);
    return true;
}

// 取消事件监听（触发回调）
bool IOManager::cancelEvent(int fd, Event ev) {
    if (fd < 0) return false;
    if (fd >= (int)m_fdContexts.size()) return false;
    auto ctx = m_fdContexts[fd];
    if (!ctx) return false;

    std::function<void()> cb;
    Fiber::ptr f;
    {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        if ((ctx->events & ev) == 0) return false; // 未注册

        // 提取回调和协程
        auto &ectx = ctx->getContext(ev);
        if (ectx.cb) cb = std::move(ectx.cb);
        if (ectx.fiber) f = std::move(ectx.fiber);

        // 更新 epoll 事件
        Event newEvents = static_cast<Event>(ctx->events & ~ev);
        uint32_t events = 0;
        if (newEvents & READ) events |= EPOLLIN;
        if (newEvents & WRITE) events |= EPOLLOUT;
        events |= EPOLLET | EPOLLERR | EPOLLHUP;

        epoll_event epevent{};
        epevent.events = events;
        epevent.data.ptr = ctx.get();
        int op = (newEvents == NONE) ? EPOLL_CTL_DEL : EPOLL_CTL_MOD;
        if (epoll_ctl(m_epfd, op, fd, (op == EPOLL_CTL_DEL) ? nullptr : &epevent) == -1) {
            // 容错处理（忽略部分错误）
        }

        // 更新上下文
        ctx->events = newEvents;
        ctx->getContext(ev).reset();
        m_pendingEventCount.fetch_sub(1, std::memory_order_relaxed);
    }

    // 在锁外触发回调（提交到调度器）
    if (cb)
        scheduler(cb); // 调度器执行回调
    else if (f)
        scheduler(f); // 调度器执行协程
    return true;
}

// 取消并触发所有注册在 fd 上的事件
bool IOManager::cancelAll(int fd) {
    if (fd < 0) return false;
    if (fd >= (int)m_fdContexts.size()) return false;
    auto ctx = m_fdContexts[fd];
    if (!ctx) return false;

    std::function<void()> cb_r, cb_w;
    Fiber::ptr f_r, f_w;
    {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        if (ctx->events == NONE) return false; // 无事件

        // 提取读/写事件的回调和协程
        if (ctx->read.cb) cb_r = std::move(ctx->read.cb);
        if (ctx->read.fiber) f_r = std::move(ctx->read.fiber);
        if (ctx->write.cb) cb_w = std::move(ctx->write.cb);
        if (ctx->write.fiber) f_w = std::move(ctx->write.fiber);

        // 从 epoll 删除所有事件
        if (epoll_ctl(m_epfd, EPOLL_CTL_DEL, fd, nullptr) == -1) {
            // 忽略错误
        }
        ctx->events = NONE;
        ctx->read.reset();
        ctx->write.reset();
        // 更新待处理事件计数（假设两个事件都被移除）
        m_pendingEventCount.fetch_sub(2, std::memory_order_relaxed);
    }

    // 触发所有事件回调
    if (cb_r) scheduler(cb_r);
    if (f_r) scheduler(f_r);
    if (cb_w) scheduler(cb_w);
    if (f_w) scheduler(f_w);
    return true;
}

// 重写 tickle()：使用 eventfd 唤醒 epoll_wait
// 写入 eventfd 触发 epoll_wait 返回
void IOManager::tickle() {
    uint64_t one = 1;
    ssize_t n = write(m_eventfd, &one, sizeof(one));
    (void)n; // 忽略写入结果（通常成功）
    // 同时通知父类条件变量（安全起见）
    Scheduler::tickle();
}

// 触发事件回调（被 epoll 事件循环调用）
// 1. 从上下文提取回调/协程
// 2. 从 epoll 中移除事件
// 3. 提交到调度器执行
void IOManager::triggerEvent(int fd, Event ev) {
    if (fd < 0) return;
    if (fd >= (int)m_fdContexts.size()) return;
    auto ctx = m_fdContexts[fd];
    if (!ctx) return;

    std::function<void()> cb;
    Fiber::ptr f;
    {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        if ((ctx->events & ev) == 0) return; // 事件已被取消

        auto &ectx = ctx->getContext(ev);
        if (ectx.cb) cb = std::move(ectx.cb);
        if (ectx.fiber) f = std::move(ectx.fiber);

        // 更新 epoll 事件
        Event newEvents = static_cast<Event>(ctx->events & ~ev);
        uint32_t events = 0;
        if (newEvents & READ) events |= EPOLLIN;
        if (newEvents & WRITE) events |= EPOLLOUT;
        events |= EPOLLET | EPOLLERR | EPOLLHUP;

        epoll_event epevent{};
        epevent.events = events;
        epevent.data.ptr = ctx.get();
        int op = (newEvents == NONE) ? EPOLL_CTL_DEL : EPOLL_CTL_MOD;
        if (epoll_ctl(m_epfd, op, fd, (op == EPOLL_CTL_DEL) ? nullptr : &epevent) == -1) {
            // 忽略错误
        }
        ctx->events = newEvents;
        ctx->getContext(ev).reset(); // 重置上下文
        m_pendingEventCount.fetch_sub(1, std::memory_order_relaxed);
    }

    // 提交到调度器执行
    if (cb)
        scheduler(cb);
    else if (f)
        scheduler(f);
}

// 重写 run()：实现 epoll 事件循环
void IOManager::run() {
    setThis(); // 设置当前线程的调度器

    // 预分配 epoll 事件缓冲区
    const int MAX_EVENTS_LOCAL = MAX_EVENTS;
    std::vector<epoll_event> events(MAX_EVENTS_LOCAL);

    while (!m_stopping.load()) {
        int timeout_ms = -1; // 无限等待
        int n = epoll_wait(m_epfd, events.data(), MAX_EVENTS_LOCAL, timeout_ms);
        if (n < 0) {
            if (errno == EINTR) continue; // 系统中断
            perror("epoll_wait");
            continue;
        }

        // 处理每个事件
        for (int i = 0; i < n; ++i) {
            epoll_event &e = events[i];
            // 处理 eventfd 事件（tickle 唤醒）
            if (e.data.ptr == nullptr) {
                uint64_t val;
                ssize_t r = read(m_eventfd, &val, sizeof(val));
                (void)r; // 忽略读取结果
                continue;
            }

            // 获取 fd 上下文
            FdContext *ctx = reinterpret_cast<FdContext *>(e.data.ptr);
            int fd = ctx->fd;
            uint32_t revents = e.events;

            // 优先处理错误事件（EPOLLERR/EPOLLHUP）
            if (revents & (EPOLLERR | EPOLLHUP)) {
                // 触发读/写事件（按实际场景）
                triggerEvent(fd, READ);
                triggerEvent(fd, WRITE);
                continue;
            }
            // 处理可读事件
            if (revents & EPOLLIN) {
                triggerEvent(fd, READ);
            }
            // 处理可写事件
            if (revents & EPOLLOUT) {
                triggerEvent(fd, WRITE);
            }
        }
    }
}

} // namespace sunshine