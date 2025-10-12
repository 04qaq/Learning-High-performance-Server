// file: libs/scheduler.h
#pragma once

// 标准库头文件：基础类型、容器、线程同步工具等
#include <cstddef>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <string>
// 自定义头文件：协程和日志模块
#include "libs/fiber.h"
#include "libs/log.h"

namespace sunshine {

// 调度器类：管理多线程协程调度的核心组件
class Scheduler {
public:
    // 智能指针类型别名，方便对象管理
    using ptr = std::shared_ptr<Scheduler>;

    // 构造函数
    // 参数说明：
    // threads: 线程池大小（默认1个线程，主协程会作为额外线程）
    // use_caller: 是否将当前调用线程作为主协程（true时主协程在调用线程运行）
    // name: 调度器名称（用于日志和调试）
    Scheduler(size_t threads = 1, bool use_caller = true, const std::string &name = "");

    // 析构函数
    virtual ~Scheduler();

    // 启动调度器（非阻塞）
    // 1. 创建指定数量的工作线程
    // 2. 启动线程执行run()主循环
    void start();

    // 停止调度器并等待所有线程结束（阻塞）
    // 1. 标记停止状态
    // 2. 唤醒所有等待的线程
    // 3. 等待工作线程退出
    void stop();

    // 获取调度器名称
    std::string GetName() const;

    // 获取当前线程绑定的调度器（若未绑定则返回nullptr）
    // 用于线程局部存储，避免全局变量
    static Scheduler *GetThis();

    // 获取当前调度器的主协程（若use_caller=true时有效）
    // 主协程是调度器启动时绑定的初始协程（通常在调用线程）
    static Fiber *GetMainFiber();

    // 提交单个任务（支持协程或函数对象）
    // 参数说明：
    // fc: 任务（Fiber::ptr或std::function<void()>）
    // thr: 指定执行线程ID（默认在任意线程执行）
    // 逻辑：
    // 1. 加锁确保线程安全
    // 2. 调用无锁提交函数
    // 3. 若队列为空则唤醒工作线程
    template <class FiberOrCb>
    void scheduler(FiberOrCb fc, std::thread::id thr = std::thread::id()) {
        bool need_tickle = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            need_tickle = schedulerNoLock(fc, thr); // 无锁提交
        }
        if (need_tickle) tickle(); // 唤醒等待的线程
    }

    // 批量提交任务（迭代器范围）
    // 逻辑同单任务提交，但支持批量处理
    template <class InputIterator>
    void scheduler(InputIterator begin, InputIterator end, std::thread::id thr = std::thread::id()) {
        bool need_tickle = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            while (begin != end) {
                need_tickle = schedulerNoLock(*begin, thr) || need_tickle;
                ++begin;
            }
        }
        if (need_tickle) tickle();
    }

    // 状态查询（原子操作，线程安全）
    int getIdleCount() const {
        return m_idleThreadCount.load();
    } // 空闲线程数
    int getActiveCount() const {
        return m_activeThreadCount.load();
    }                             // 活跃线程数（正在执行任务）
    size_t getTaskCount() const { // 任务队列长度
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_fibers.size();
    }

protected:
    // 唤醒策略（子类可重载，如IOManager需要特殊唤醒）
    // 默认实现：唤醒等待的条件变量
    virtual void tickle();

    // 主循环（虚函数，允许子类覆盖如IOManager）
    // 工作线程的执行入口：
    // 1. 标记活跃线程
    // 2. 循环从任务队列取任务执行
    // 3. 空闲时等待条件变量
    virtual void run();

    // 绑定当前线程到调度器（用于主协程）
    void setThis();

private:
    // 无锁提交任务（关键内部函数）
    // 返回值：是否需要唤醒线程（队列从空变非空）
    template <class FiberOrCb>
    bool schedulerNoLock(FiberOrCb fc, std::thread::id thr);

    // 从队列取一个任务（原子操作）
    // 输出参数：
    // out_fiber: 协程对象（若任务是协程）
    // out_cb: 函数对象（若任务是函数）
    bool takeOneTask(Fiber::ptr &out_fiber, std::function<void()> &out_cb);

private:
    // 任务结构体：保存任务和所属线程ID
    struct FiberAndThread {
        std::thread::id threadid; // 任务指定执行的线程ID
        std::function<void()> cb; // 函数式任务
        Fiber::ptr fiber;         // 协程任务

        // 默认构造
        FiberAndThread() = default;
        // 协程构造
        FiberAndThread(Fiber::ptr f, std::thread::id thr) :
            threadid(thr), fiber(f) {
        }
        // 函数对象构造
        FiberAndThread(const std::function<void()> &f, std::thread::id thr) :
            threadid(thr), cb(f) {
        }
    };

protected:
    // 工作线程列表（管理线程生命周期）
    std::vector<std::shared_ptr<std::thread>> m_threads;
    // 工作线程ID列表（用于线程绑定检查）
    std::vector<std::thread::id> threadIds;
    // 任务队列：存储FiberAndThread结构体
    std::list<FiberAndThread> m_fibers;
    // 调度器名称（用于日志标识）
    std::string m_name;
    // 任务队列互斥锁（保护m_fibers）
    mutable std::mutex m_mutex;
    // 条件变量：工作线程等待任务
    std::condition_variable m_cond;

    // 线程池配置
    size_t m_threadCount = 0; // 实际工作线程数
    // 线程状态计数器（原子操作，避免锁开销）
    std::atomic<int> m_activeThreadCount{0}; // 正在执行任务的线程数
    std::atomic<int> m_idleThreadCount{0};   // 空闲线程数

    // 停止状态
    std::atomic<bool> m_stopping{true}; // 是否正在停止
    bool m_useCaller = false;           // 是否使用调用线程作为主协程
    bool m_autoStop = false;            // 是否自动停止（用于主协程）

    // 主协程相关
    std::thread::id m_rootThread; // 主协程所在线程ID
    Fiber::ptr m_rootFiber;       // 主协程对象
};

// 模板函数实现：无锁提交任务
template <class FiberOrCb>
bool Scheduler::schedulerNoLock(FiberOrCb fc, std::thread::id thr) {
    bool need_tickle = m_fibers.empty(); // 任务队列是否为空（用于判断是否需要唤醒线程）
    FiberAndThread ft;                   // 创建任务结构体

    // 类型判断：根据任务类型填充结构体
    if constexpr (std::is_same_v<std::decay_t<FiberOrCb>, Fiber::ptr>) {
        ft.fiber = fc; // 任务是协程
    } else if constexpr (std::is_same_v<std::decay_t<FiberOrCb>, std::function<void()>>) {
        ft.cb = fc; // 任务是函数
    } else {
        ft.cb = (std::function<void()>)fc; // 其他类型转换为函数对象
    }
    ft.threadid = thr; // 记录任务指定线程ID

    // 有效任务才入队
    if (ft.fiber || ft.cb) {
        m_fibers.push_back(std::move(ft));
    }
    return need_tickle; // 返回是否需要唤醒（队列从空变非空）
}

} // namespace sunshine