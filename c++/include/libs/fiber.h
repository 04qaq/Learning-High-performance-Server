
#pragma once

#include <cstddef>
#include <memory>
#include <sys/ucontext.h>
#include <ucontext.h>
#include <functional>
#include <cstdint>

namespace sunshine {

class Fiber : public std::enable_shared_from_this<Fiber> {
public:
    typedef std::shared_ptr<Fiber> ptr;
    enum State {
        INIT, // 准备状态（还没运行过）
        HOLD, // 挂起（协程主动让出）
        EXEC, // 正在运行
        TERM, // 执行完毕
        READY // 就绪（可以被调度运行）
    };

    // 创建一个新协程，传入回调和可选的栈大小
    Fiber(std::function<void()> cb, size_t stacksize = 0);
    ~Fiber();

    // 重置协程（只可用于已经结束的协程，或未使用的协程）
    void reset(std::function<void()> cb);
    // 将当前线程切换到此协程（进入协程执行）
    void swapIn();
    // 将当前协程切回主协程/调度者（保存当前协程上下文并切换）
    void swapOut();

    // 获取当前正在运行的协程（线程局部）
    static Fiber *GetThis();
    // 将当前协程状态设为就绪并切回调度者
    static void YieldToReady();
    // 将当前协程状态设为挂起并切回调度者
    static void YieldToHold();
    // 返回当前线程创建的协程总数
    static uint64_t TotalFibers();

    // 获取状态（调试用）
    State getState() const {
        return m_state;
    }

private:
    // 私有默认构造，用于创建主协程（代表线程本身的上下文）
    Fiber();

    // 协程运行的入口函数（makecontext 指向的静态函数）
    static void MainFunc(uintptr_t);

private:
    uint64_t m_id = 0;          // 协程 id
    uint64_t m_stacksize = 0;   // 协程栈大小
    ucontext_t m_ctx;           // 协程上下文
    State m_state = INIT;       // 当前状态
    void *m_stack = nullptr;    // 协程栈起始地址（向低地址增长）
    std::function<void()> m_cb; // 协程运行的函数入口
};

} // namespace sunshine
