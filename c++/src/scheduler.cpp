
#include "libs/scheduler.h"
#include <utility>
#include <iostream>

namespace sunshine {

// 线程局部的当前 Scheduler 指针（每个 worker 线程都会绑定一个 Scheduler*）
static thread_local Scheduler *t_scheduler = nullptr;

Scheduler::Scheduler(size_t threads, bool use_caller, const std::string &name) :
    m_threadCount(threads),
    m_useCaller(use_caller),
    m_name(name) {
    if (m_threadCount == 0) m_threadCount = 1;
    // 如果 caller 参与，则期望的后台线程数是 threads-1（但实际线程创建在 start() 中）
    m_stopping.store(true);
}

Scheduler::~Scheduler() {
    // 析构时确保已停止
    if (!m_stopping.load()) {
        stop();
    }
}

void Scheduler::start() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_stopping.load()) {
        // 已经启动过
        return;
    }
    m_stopping.store(false);

    // 创建 worker 线程（如果 use_caller 为 true，caller 负责自己调用 run()）
    size_t createCount = m_threadCount;
    if (m_useCaller) {
        // 如果 caller 参与，这里我们不为 caller 启动新的 std::thread
        // m_threadCount 表示期望的 worker 总数（包含 caller），因此实际创建 = m_threadCount - 1
        if (createCount > 0) createCount -= 1;
        // 记录 caller 线程 id（在 caller 调用 run() 时会设置）
        m_rootThread = std::this_thread::get_id();
    }

    for (size_t i = 0; i < createCount; ++i) {
        // 创建线程并把 run() 作为线程函数
        auto thr = std::make_shared<std::thread>([this]() {
            this->run();
        });
        threadIds.push_back(thr->get_id());
        m_threads.push_back(thr);
    }
}

// stop: 请求停止并等待所有线程退出
void Scheduler::stop() {
    m_stopping.store(true);
    // 唤醒所有等待中的线程
    m_cond.notify_all();

    // 如果 caller 作为 worker 并且当前线程正自己在 run() 中，stop 可能由 run 发起
    // join 所有 std::thread
    for (auto &t : m_threads) {
        if (t && t->joinable()) {
            t->join();
        }
    }
    m_threads.clear();
    threadIds.clear();
}

// 返回调度器名字
std::string Scheduler::GetName() const {
    return m_name;
}

Scheduler *Scheduler::GetThis() {
    return t_scheduler;
}

Fiber *Scheduler::GetMainFiber() {
    if (t_scheduler) {
        return t_scheduler->m_rootFiber.get();
    }
    return nullptr;
}

// tickle: 简单实现为唤醒 condition_variable
void Scheduler::tickle() {
    // 唤醒一个等待线程（或所有，根据策略）
    m_cond.notify_one();
}

// 把当前线程绑定到调度器（设置 thread-local 指针）
void Scheduler::setThis() {
    t_scheduler = this;
}

// takeOneTask: 在持有 m_mutex 的外部调用，用于从队列中取出一个适合当前线程的任务
bool Scheduler::takeOneTask(Fiber::ptr &out_fiber, std::function<void()> &out_cb) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_fibers.empty()) return false;

    // 当前线程 id
    std::thread::id cur = std::this_thread::get_id();

    for (auto it = m_fibers.begin(); it != m_fibers.end(); ++it) {
        // 如果任务指定了具体 threadid，则只有在匹配或为 caller（root）时才取
        if (it->threadid != std::thread::id() && it->threadid != cur) {
            continue;
        }
        // 找到任务，移动出返回
        out_fiber = std::move(it->fiber);
        out_cb = std::move(it->cb);
        m_fibers.erase(it);
        return true;
    }
    return false;
}

// run: worker 主循环，供 worker 线程和（可选）caller 线程调用
void Scheduler::run() {
    // 绑定此线程到调度器（用于 GetThis）
    setThis();

    // 如果 caller 参与，并且当前线程是 caller，则需要设置 m_rootFiber（若尚未）
    if (m_useCaller && std::this_thread::get_id() == m_rootThread) {
        // 用一个 Fiber 将 run 包裹（可选，如果想从协程视角管理 caller）
        if (!m_rootFiber) {
            m_rootFiber = std::make_shared<Fiber>(std::bind(&Scheduler::run, this));
        }
    }

    // 循环直到停止
    while (!m_stopping.load()) {
        Fiber::ptr task_f;
        std::function<void()> task_cb;

        // 尝试取任务
        if (takeOneTask(task_f, task_cb)) {
            // 有任务
            ++m_activeThreadCount;
            try {
                if (task_f) {
                    // 如果是 Fiber，直接切入执行
                    task_f->swapIn();
                    // 协程返回后，可能为 HOLD/READY/TERM 等，根据需要处理（此处不再自动重插）
                } else if (task_cb) {
                    // 如果是简单回调，包装成临时 Fiber 执行（确保协程环境）
                    Fiber::ptr tmp = std::make_shared<Fiber>(std::move(task_cb));
                    tmp->swapIn();
                }
            } catch (const std::exception &e) {
                std::cerr << "Scheduler task exception: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "Scheduler task unknown exception" << std::endl;
            }
            --m_activeThreadCount;
            continue; // 继续循环，不休眠
        }

        // 没有直接可执行的任务，进入等待
        std::unique_lock<std::mutex> lock(m_mutex);
        // 如果停止就退出
        if (m_stopping.load()) break;

        // 等待直到队列有新任务或停止
        m_idleThreadCount++;
        m_cond.wait(lock, [this]() { return !m_stopping.load() && !m_fibers.empty(); });
        m_idleThreadCount--;
        // loop 继续尝试 takeOneTask
    }
    // 线程结束前解绑 t_scheduler（可选）
    if (t_scheduler == this) t_scheduler = nullptr;
}

} // namespace sunshine
