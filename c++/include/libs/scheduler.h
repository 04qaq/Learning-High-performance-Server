// file: scheduler.h
#pragma once

// 实现协程控制器（调度器）头文件（工程化版）
// 主要职责：把协程（Fiber）或回调（function）放入内部队列，并在合适的线程上执行它们。
// 支持：线程池（多个 worker threads），任务亲和性（指定线程 id），唤醒/等待，启动/停止等。

#include <cstddef>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <condition_variable>
#include <atomic>
#include <functional>
#include "libs/fiber.h"
#include "libs/log.h"

namespace sunshine {

class Scheduler {
public:
    using ptr = std::shared_ptr<Scheduler>;

    // threads: 工作线程总数（不包含 caller 线程，若 use_caller 为 true，则 caller 可作为第 threads 个 worker）
    // use_caller: 是否允许调用线程成为 worker（若 true，需要用户在 caller 线程调用 run()）
    // name: 调度器名称（用于日志）
    Scheduler(size_t threads = 1, bool use_caller = true, const std::string &name = "");
    ~Scheduler();

    // 启动调度器：创建线程（不阻塞）。
    // 如果 use_caller==true, 此函数不会把 caller 作为后台线程自动运行；若要让 caller 成为 worker，请在 caller 线程显式调用 run()。
    void start();

    // 停止调度器：请求停止并等待所有线程退出（阻塞）。
    void stop();

    std::string GetName() const;

    // 获取当前线程正在使用的 Scheduler（thread-local）
    static Scheduler *GetThis();

    // 获取当前线程/调度器的主协程指针（代表线程自身的上下文）
    static Fiber *GetMainFiber();

    // 单个任务提交（模板）：
    //  - 可以传入 Fiber::ptr 或 std::function<void()>
    //  - thr: 指定任务期望运行的线程 id，默认 std::thread::id() 表示任意线程
    template <class FiberOrCb>
    void scheduler(FiberOrCb fc, std::thread::id thr = std::thread::id()) {
        bool need_tickle = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            need_tickle = schedulerNoLock(fc, thr);
        }
        if (need_tickle) tickle();
    }

    // 批量提交（通过迭代器范围）
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

protected:
    // 保护方法：唤醒 worker（实现使用 condition_variable）
    void tickle();

    // run(): worker 线程/ caller 线程 执行的主循环
    //  - 该函数会持续从任务队列取任务并执行，直到 stop 被请求
    //  - 可由多个线程并发调用（每个线程都在执行自己的 run()）
    void run();

    // 把当前线程绑定到调度器（thread-local）
    void setThis();

private:
    // 在持有 m_mutex 的前提下做实际插入，不做加锁/解锁
    template <class FiberOrCb>
    bool schedulerNoLock(FiberOrCb fc, std::thread::id thr);

    // 检查并弹出一个适合当前线程执行的任务（由 run() 使用）
    bool takeOneTask(Fiber::ptr &out_fiber, std::function<void()> &out_cb);

private:
    struct FiberAndThread {
        std::thread::id threadid; // 期望在哪个线程运行（默认: std::thread::id() 表示任意线程）
        std::function<void()> cb; // 如果以回调提交则放这里
        Fiber::ptr fiber;         // 如果以 Fiber 提交则放这里

        FiberAndThread() = default;
        FiberAndThread(Fiber::ptr f, std::thread::id thr) :
            threadid(thr), fiber(f) {
        }
        FiberAndThread(Fiber::ptr *f, std::thread::id thr) :
            threadid(thr) {
            fiber = move(*f);
        }
        FiberAndThread(const std::function<void()> &f, std::thread::id thr) :
            threadid(thr), cb(f) {
        }
        FiberAndThread(const std::function<void()> *f, std::thread::id thr) :
            threadid(thr) {
            cb = move(*f);
        }
    };

private:
    // worker 管理
    std::vector<std::shared_ptr<std::thread>> m_threads; // 实际启动的线程对象
    std::vector<std::thread::id> threadIds;              // 启动线程的 id 列表（方便亲和判断）
    std::list<FiberAndThread> m_fibers;                  // 任务队列（协程或回调）
    std::string m_name;                                  // 调度器名字（日志）
    std::mutex m_mutex;                                  // 保护队列和状态
    std::condition_variable m_cond;                      // 用于唤醒等待线程

    // 状态计数（用于监控/决策）
    size_t m_threadCount = 0;                // 希望启动的线程数（不包含 caller）
    std::atomic<int> m_activeThreadCount{0}; // 当前正在执行任务的线程数
    std::atomic<int> m_idleThreadCount{0};   // 当前空闲等待的线程数

    // 停止控制
    std::atomic<bool> m_stopping{true}; // 是否正在停止（请求）
    bool m_useCaller = false;           // 是否允许 caller 成为 worker（需要手动调用 run()）
    bool m_autoStop = false;            // 是否自动在所有工作完成后停止（设计选项）

    // root/caller 相关
    std::thread::id m_rootThread; // 如果 use_caller==true，记录 caller 的线程 id（用于亲和判断）
    Fiber::ptr m_rootFiber;       // 当 caller 参与调度时，保存 caller 的 root Fiber（可选，代表 caller 的调度上下文）
};

// 模板成员的实现（放在头文件中）：
template <class FiberOrCb>
bool Scheduler::schedulerNoLock(FiberOrCb fc, std::thread::id thr) {
    bool need_tickle = m_fibers.empty();
    FiberAndThread ft;
    // 判定类型为 Fiber::ptr
    if constexpr (std::is_same_v<std::decay_t<FiberOrCb>, Fiber::ptr>) {
        ft.fiber = fc;
    } else if constexpr (std::is_same_v<std::decay_t<FiberOrCb>, std::function<void()>>) {
        ft.cb = fc;
    } else {
        // 如果传入迭代器的 *it 类型不是上述两种，也尝试隐式转换为 std::function<void()>
        ft.cb = (std::function<void()>)fc;
    }
    ft.threadid = thr;
    if (ft.fiber || ft.cb) {
        m_fibers.push_back(std::move(ft));
    }
    return need_tickle;
}

} // namespace sunshine
