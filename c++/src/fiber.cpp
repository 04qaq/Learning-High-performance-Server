#include "libs/fiber.h"
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <atomic>
#include <bits/types/stack_t.h>
#include <new>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <ucontext.h>
#include <iostream>

namespace sunshine {

// ---------- 线程局部变量 ----------
// 每个线程维护自己的主协程和当前运行协程指针（thread_local）
// t_main_fiber: 指向当前线程的“主协程”（代表线程本身的上下文）
// t_cur_fiber:  指向当前正在执行的协程（可能是主协程或用户创建的协程）
static thread_local Fiber *t_main_fiber = nullptr; // 主协程地址（线程私有）
static thread_local Fiber *t_cur_fiber = nullptr;  // 当前协程地址（线程私有）

// 全局（跨线程安全）协程计数器：用来统计已创建的 Fiber 数量
static std::atomic<uint64_t> s_fiber_count(0);

// 默认协程栈大小（字节），如果用户未指定则使用此值
static const size_t DEFAULT_STACK_SIZE = 128 * 1024; // 128 KB

// ---------- 构造 / 析构 ----------

// 私有无参构造：用于创建“主协程”（代表线程本身）
// 注意：该构造只捕获当前线程的上下文，不分配额外栈
Fiber::Fiber() {
    // 生成唯一 id（简单自增）
    m_id = ++s_fiber_count;

    // 获取当前上下文（保存到 m_ctx），以便后续 swapcontext 能切回来
    // getcontext 通常不会改变寄存器状态，只是把当前 execution context 保存到 m_ctx
    if (getcontext(&m_ctx) != 0) {
        throw std::runtime_error("getcontext failed foe main fiber");
    }

    // 将线程局部的当前协程和主协程指向自己
    // 主协程指代线程的原始执行上下文（非 heap 分配的用户协程）
    t_cur_fiber = this;
    t_main_fiber = this;

    // 说明（未显式设置 m_state）：主协程被视为线程当前在运行的上下文，
    // 在后续 swap 操作中会被作为返回目标。
}

// 普通协程构造：创建一个具有独立栈并将在 makecontext 中绑定 MainFunc 的协程
Fiber::Fiber(std::function<void()> cb, size_t stacksize) :
    m_cb(move(cb)) {
    // 分配 id
    m_id = ++s_fiber_count;

    // 如果用户未指定则使用默认栈大小
    m_stacksize = stacksize;
    if (stacksize == 0) {
        m_stacksize = DEFAULT_STACK_SIZE;
    }

    // 初始状态设为 INIT（尚未执行）
    m_state = INIT;

    // 分配栈（使用 malloc 而非 new[]，因为 ucontext 接口期望裸内存）
    m_stack = malloc(m_stacksize);
    if (!m_stack) {
        throw std::bad_alloc();
    }

    // 初始化上下文
    if (getcontext(&m_ctx) != 0) {
        // 如果 getcontext 失败，释放已分配栈
        std::free(m_stack);
        throw std::runtime_error("getcontext failed");
    }

    // 将分配的栈与上下文关联
    m_ctx.uc_stack.ss_sp = m_stack;       // 栈底（起始地址）
    m_ctx.uc_stack.ss_size = m_stacksize; // 栈大小
    m_ctx.uc_link = nullptr;              // 协程函数返回时的上下文（nullptr 表示没有自动回链）

    // 将协程入口函数 MainFunc 绑定到上下文，并把当前 this 作为参数传入
    // 注意：makecontext 的可变参数需要与 MainFunc 的签名匹配（这里使用 uintptr_t 传指针）
    // 强转写法在大多数 x86_64/linux 下可行，但可移植性需注意。
    makecontext(&m_ctx, (void (*)()) & Fiber::MainFunc, 1, (uintptr_t)this);
}

// 析构：释放为协程分配的栈（如果存在）
Fiber::~Fiber() {
    if (m_stack) {
        free(m_stack);
        m_stack = nullptr;
    }
}

// ---------- 协程复用 / 重置 ----------

// reset: 在协程处于 INIT 或 TERM 时可以复用该对象（重新绑定回调并重建上下文）
// 仅在该 Fiber 有独立栈（即非主协程）时允许 reset
void Fiber::reset(std::function<void()> cb) {
    // 不允许对主协程或没有栈的协程 reset
    if (!m_stack) {
        throw std::logic_error("can't reset a main fiber or a fiber without stack");
    }

    // 仅允许在未运行或已经结束时 reset
    if (m_state != INIT && m_state != TERM) {
        throw std::logic_error("fiber can only be reset in TERM or INIT state");
    }

    // 赋新回调并复位状态
    m_cb = move(cb);
    m_state = INIT;

    // 重新获取上下文并绑定栈（与构造时相同）
    if (getcontext(&m_ctx) != 0) {
        throw std::runtime_error("getcontext failed in reset");
    }

    m_ctx.uc_stack.ss_sp = m_stack;
    m_ctx.uc_stack.ss_size = m_stacksize;
    m_ctx.uc_link = nullptr;

    makecontext(&m_ctx, (void (*)()) & Fiber::MainFunc, 1, (uintptr_t)this);
}

// ---------- 上下文切换接口 ----------

// swapIn: 从当前上下文切换到 this 协程（进入协程执行）
// - 若线程尚未创建主协程，则 first-time 创建主协程（new Fiber()）
// - 如果 this 已经是当前协程则直接返回
void Fiber::swapIn() {
    // 如果主协程不存在（线程第一次使用 Fiber），创建主协程
    if (!t_main_fiber) {
        new Fiber(); // 主协程分配在堆上且不释放：线程结束时 OS 回收
        assert(t_main_fiber);
    }

    // 如果要切换的是当前协程，则无需切换
    if (this == t_cur_fiber) return;

    // 设置目标协程状态为执行中
    m_state = EXEC;

    // 记录先前协程（可能是主协程或其他协程），并把当前协程设为 this
    auto pre = t_cur_fiber;
    t_cur_fiber = this;

    // swapcontext: 保存 pre 的上下文到 pre->m_ctx，并切到 this 的 m_ctx
    // 返回时表示后续有上下文切回到 pre（即其他协程切回了 pre）
    if (swapcontext(&pre->m_ctx, &m_ctx) != 0) {
        throw std::runtime_error("swapcontext swapIn failed");
    }
}

// swapOut: 将当前协程切回主协程（或调度者）
// - 只有当当前协程状态为 EXEC 时，才会修改为 HOLD（以免覆盖调用者已设置的 READY 等状态）
void Fiber::swapOut() {
    if (!t_main_fiber) {
        throw std::logic_error("no main fiber to swap out to");
    }

    // 仅在确实处于执行态时才设置为挂起，避免覆盖调用方预先设置的状态（例如 READY）
    if (m_state == EXEC) {
        m_state = HOLD;
    }

    // 注意：这里使用 t_cur_fiber（当前协程）来做 swapcontext 的源，
    //      并把 t_cur_fiber 切回到主协程指针。
    //      也可以写成 Fiber *self = this; 语义更明确（this 应等于 t_cur_fiber）
    Fiber *self = t_cur_fiber;
    t_cur_fiber = t_main_fiber;

    // 保存当前协程上下文到 self->m_ctx，并切换到主协程上下文
    if (swapcontext(&self->m_ctx, &t_main_fiber->m_ctx) != 0) {
        throw std::runtime_error("swapcontext swapOut failed");
    }
}

// ---------- 辅助静态接口 ----------

// GetThis: 返回当前线程正在执行的协程指针；
// 若尚未初始化主协程，则创建主协程（lazy init）
Fiber *Fiber::GetThis() {
    if (!t_cur_fiber) {
        new Fiber(); // 创建并设置 t_main_fiber / t_cur_fiber
    }
    return t_cur_fiber;
}

// YieldToReady: 将当前协程标记为 READY 并切出（让出 CPU，调度者会把它加入就绪队列）
// 注意：这里先设置状态再 swapOut，是为了让切换发生前状态就绪，避免竞态。
void Fiber::YieldToReady() {
    auto cur = GetThis();
    cur->m_state = READY;
    cur->swapOut();
}

// YieldToHold: 将当前协程标记为 HOLD（挂起）并切出
void Fiber::YieldToHold() {
    auto cur = GetThis();
    cur->m_state = HOLD;
    cur->swapOut();
}

// TotalFibers: 返回已创建的协程总数（含主协程）
// 注意：s_fiber_count 是原子类型，这里直接返回其值（隐式转换），
// 更明确的做法是 return s_fiber_count.load();
uint64_t Fiber::TotalFibers() {
    return s_fiber_count;
}

// ---------- 协程入口函数（由 makecontext 调用） ----------
// MainFunc 的签名接受一个 uintptr_t，用来传递 this 指针（不同平台上需注意调用约定）
void Fiber::MainFunc(uintptr_t raw) {
    // 把传入的整数还原为 Fiber* 指针
    Fiber *f = reinterpret_cast<Fiber *>(raw);

    // 将线程局部当前协程指向 f，并将状态设为 EXEC
    t_cur_fiber = f;
    f->m_state = EXEC;

    try {
        // 调用用户提供的回调函数（协程体）
        if (f->m_cb) {
            f->m_cb(); // !! 这里必须调用函数：f->m_cb()
        }

        // 回调返回，清理回调并标记协程为结束
        f->m_cb = nullptr;
        f->m_state = TERM;
    } catch (const std::exception &e) {
        // 捕获并打印异常（协程内部异常不会导致整个进程终止）
        std::cerr << "Fiber caught exception: " << e.what() << std::endl;
        f->m_state = TERM;
    } catch (...) {
        std::cerr << "Fiber caught unknown exception" << std::endl;
        f->m_state = TERM;
    }

    // 协程结束后，把当前协程指向主协程并切回主协程上下文
    Fiber *self = f;
    t_cur_fiber = t_main_fiber;
    if (swapcontext(&self->m_ctx, &t_main_fiber->m_ctx) != 0) {
        // 如果切换失败，无法恢复到主协程，直接终止程序
        std::terminate();
    }
}

} // namespace sunshine
