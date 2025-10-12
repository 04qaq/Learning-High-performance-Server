// file: libs/scheduler.cpp
#include "libs/scheduler.h"
#include <iostream>

namespace sunshine {

// 线程局部变量：存储当前线程绑定的调度器实例
// 用于实现线程局部存储（TLS），避免全局变量
static thread_local Scheduler *t_scheduler = nullptr;

// 构造函数
// 参数说明：
// threads: 工作线程数量（至少为1）
// use_caller: 是否将调用线程作为主协程（true时主协程在调用线程运行）
// name: 调度器名称（用于日志标识）
Scheduler::Scheduler(size_t threads, bool use_caller, const std::string &name) :
    m_threadCount(threads),
    m_useCaller(use_caller),
    m_name(name) {
    // 确保线程数至少为1（避免0线程）
    if (m_threadCount == 0) m_threadCount = 1;
    // 初始状态：调度器处于停止状态
    m_stopping.store(true);
}

// 析构函数
// 确保调度器安全停止（如果未停止则调用stop）
Scheduler::~Scheduler() {
    if (!m_stopping.load()) stop();
}

// 启动调度器（非阻塞）
// 逻辑流程：
// 1. 检查是否已启动（已启动则直接返回）
// 2. 设置停止标志为false
// 3. 计算实际工作线程数：
//    - 若use_caller=true，则主协程在调用线程，工作线程数 = threads - 1
// 4. 创建工作线程（每个线程执行run()主循环）
// 5. 如果use_caller=true，调用线程将作为主协程（后续会调用run()）
void Scheduler::start() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_stopping.load()) return; // 已启动则返回
    m_stopping.store(false);        // 标记为运行中

    size_t createCount = m_threadCount;
    if (m_useCaller) {
        // 主协程在调用线程，工作线程数减1
        if (createCount > 0) createCount -= 1;
        // 记录主协程线程ID
        m_rootThread = std::this_thread::get_id();
    }

    // 创建工作线程
    for (size_t i = 0; i < createCount; ++i) {
        auto thr = std::make_shared<std::thread>([this]() {
            this->run(); // 虚函数调用，允许子类覆盖（如IOManager）
        });
        m_threads.push_back(thr);
        threadIds.push_back(thr->get_id());
    }

    // 如果使用caller模式，调用线程将作为主协程（后续会调用run()）
    // 注意：此时不会立即执行run()，需等待后续调度
}

// 停止调度器并等待所有线程结束（阻塞）
// 逻辑流程：
// 1. 设置停止标志（m_stopping=true）
// 2. 唤醒所有等待的线程（notify_all）
// 3. 等待所有工作线程结束（join）
void Scheduler::stop() {
    m_stopping.store(true); // 标记停止
    m_cond.notify_all();    // 唤醒所有等待线程

    // 等待工作线程结束
    for (auto &t : m_threads) {
        if (t && t->joinable()) t->join();
    }
    m_threads.clear(); // 清空线程列表
    threadIds.clear(); // 清空线程ID列表
}

// 获取调度器名称
std::string Scheduler::GetName() const {
    return m_name;
}

// 获取当前线程绑定的调度器（线程局部存储）
Scheduler *Scheduler::GetThis() {
    return t_scheduler;
}

// 获取当前调度器的主协程（若use_caller=true时有效）
Fiber *Scheduler::GetMainFiber() {
    // 通过线程局部变量t_scheduler获取当前调度器
    if (t_scheduler) return t_scheduler->m_rootFiber.get();
    return nullptr;
}

// 唤醒策略实现（默认唤醒一个等待线程）
// 用于在任务队列非空时唤醒等待的工作线程
void Scheduler::tickle() {
    m_cond.notify_one(); // 通知一个等待的线程
}

// 设置当前线程绑定的调度器（用于线程局部存储）
void Scheduler::setThis() {
    t_scheduler = this; // 设置线程局部变量
}

// 从任务队列取一个任务（考虑线程绑定）
// 参数说明：
// out_fiber: 输出协程（若任务是协程）
// out_cb: 输出函数对象（若任务是函数）
// 返回值：是否成功取到任务
bool Scheduler::takeOneTask(Fiber::ptr &out_fiber, std::function<void()> &out_cb) {
    std::lock_guard<std::mutex> lock(m_mutex);
    // 任务队列为空则返回false
    if (m_fibers.empty()) return false;

    // 获取当前线程ID
    std::thread::id cur = std::this_thread::get_id();

    // 遍历任务队列，寻找可执行任务：
    // 1. 任务未指定线程（threadid为空）则任意线程可执行
    // 2. 任务指定了线程ID，且与当前线程ID匹配
    for (auto it = m_fibers.begin(); it != m_fibers.end(); ++it) {
        if (it->threadid != std::thread::id() && it->threadid != cur)
            continue; // 任务指定线程不匹配当前线程，跳过

        // 取出任务
        out_fiber = std::move(it->fiber);
        out_cb = std::move(it->cb);
        m_fibers.erase(it); // 从队列移除
        return true;
    }
    return false;
}

// 调度器主循环（核心执行逻辑）
// 逻辑流程：
// 1. 设置当前线程的调度器（setThis）
// 2. 处理主协程（若use_caller=true且当前线程是主协程线程）：
//    - 创建空协程（代表主协程，不封装任务函数）
// 3. 循环执行：
//    a. 尝试取任务（takeOneTask）
//    b. 若取到任务：
//       - 增加活跃线程计数
//       - 执行任务（协程swapIn或函数对象转协程执行）
//       - 捕获异常（避免崩溃）
//       - 减少活跃线程计数
//    c. 若未取到任务：
//       - 增加空闲线程计数
//       - 等待条件变量（新任务或停止）
//       - 减少空闲线程计数
// 4. 停止时清理线程局部变量
void Scheduler::run() {
    setThis(); // 设置当前线程的调度器

    // 处理主协程（当use_caller=true且当前线程是主协程线程时）
    if (m_useCaller && std::this_thread::get_id() == m_rootThread) {
        if (!m_rootFiber) {
            // 创建空协程代表主协程（不封装任务函数）
            m_rootFiber = std::make_shared<Fiber>([]() {});
        }
        // 执行主协程（注意：主协程会阻塞直到任务完成）
        m_rootFiber->swapIn();
    }

    // 主循环：持续执行任务
    while (!m_stopping.load()) {
        Fiber::ptr task_f;
        std::function<void()> task_cb;

        // 尝试获取任务
        if (takeOneTask(task_f, task_cb)) {
            ++m_activeThreadCount; // 标记活跃线程
            try {
                if (task_f) {
                    // 执行协程任务
                    task_f->swapIn();
                } else if (task_cb) {
                    // 将函数对象转为协程执行
                    Fiber::ptr tmp = std::make_shared<Fiber>(std::move(task_cb));
                    tmp->swapIn();
                }
            } catch (const std::exception &e) {
                // 捕获异常并打印（避免崩溃）
                std::cerr << "Scheduler task exception: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "Scheduler task unknown exception" << std::endl;
            }
            --m_activeThreadCount; // 恢复活跃计数
            continue;              // 继续循环
        }

        // 未取到任务：进入等待状态
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_stopping.load()) break; // 停止标志已设置，退出循环
        m_idleThreadCount++;          // 标记空闲线程
        // 等待条件变量（新任务或停止）
        m_cond.wait(lock, [this]() {
            return m_stopping.load() || !m_fibers.empty();
        });
        m_idleThreadCount--; // 恢复空闲计数
    }

    // 停止时清理线程局部变量
    if (t_scheduler == this) t_scheduler = nullptr;
}

} // namespace sunshine