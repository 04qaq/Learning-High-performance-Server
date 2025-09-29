// thread.cpp
#include "libs/thread.h"
#include <libs/log.h>
#include <iostream>
#include <stdexcept>
#include <atomic>
#include <cstring> // for strncpy
#include <functional>
#ifdef __linux__
#include <sys/syscall.h> // syscall, SYS_gettid
#include <unistd.h>
#endif

namespace sunshine {

// 线程局部变量：当前线程对应的 Thread*（供 Thread::GetThis() 使用）
static thread_local Thread *t_thread = nullptr;
// 线程局部变量：当前线程名（供 Thread::SetName() / 调试使用）
static thread_local std::string t_thread_name = "UNKNOWN";

// 构造函数：接收回调和线程名，创建 pthread 并启动线程
Thread::Thread(std::function<void()> cb, std::string &name) :
    m_cb(std::move(cb)), m_name(name), m_thread(0), m_id(0) {
    // 在创建 pthread 之前，确保回调有效
    if (!m_cb) {
        throw std::invalid_argument("Thread callback is empty");
    }

    // 创建 pthread，传入 this 指针作为参数
    // pthread_create 会在成功返回后把 pthread id 写入 m_thread
    int rt = pthread_create(&m_thread, nullptr, &Thread::run, this);
    if (rt != 0) {
        // 若创建失败，抛出异常并让调用方处理
        throw std::runtime_error(std::string("pthread_create failed: ") + std::strerror(rt));
    }

    // 注意：此时线程已经开始执行 run(), 但 m_id 由 run() 内设置（线程自己设置）
}

// 析构函数：如果线程仍然可 join 且未 join，选择 detach（避免 std::terminate）
// 也可以根据需要改为强制 join（可能阻塞析构），这里选择 detach 更安全
Thread::~Thread() {
    // 如果线程句柄有效且不是主线程的 t_thread（避免误 detach 主线程）：
    if (m_thread) {
        // 尝试 detach：如果线程已被 join，则 pthread_detach 会返回 ESRCH 或 EINVAL
        int rt = pthread_detach(m_thread);
        // 我们不抛异常，因为析构不能抛出；把错误打印出来以便调试
        if (rt != 0 && rt != ESRCH && rt != EINVAL) {
            LOG_ERROR(Log::LogManager::GetInstance().getRoot())
                << "Thread::~Thread: pthread_detach failed: "
                << std::strerror(rt) << "\n";
        }
        m_thread = 0; // 避免重复操作
    }
}

// 获取线程 id（内核线程 id 或由实现决定）
// 在 Linux 下用 syscall(SYS_gettid) 获取真实 tid；在其它平台用 hash(pthread_t)
uint32_t Thread::getId() {
    return m_id;
}

// 阻塞等待线程结束
void Thread::join() {
    if (!m_thread) {
        return; // 线程尚未创建或已被处理
    }
    int rt = pthread_join(m_thread, nullptr);
    if (rt != 0) {
        // join 失败，抛异常让调用方知晓
        throw std::runtime_error(std::string("pthread_join failed: ") + std::strerror(rt));
    }
    // join 成功后清理
    m_thread = 0;
}

// 返回当前线程对应的 Thread*，若不是由本类创建的线程则返回 nullptr
Thread *Thread::GetThis() {
    return t_thread;
}

// 设置当前线程名字（仅影响调用该函数的线程）
// 此函数也尝试同步到 pthread 名称（Linux: pthread_setname_np）
void Thread::SetName(const std::string name) {
    t_thread_name = name;
#ifdef __linux__
    // pthread_setname_np 名字长度限制为 16（包含终止符）
    // 因此我们截断为 15 个字符
    char buf[16];
    std::strncpy(buf, name.c_str(), sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    // 作用于当前线程
    pthread_setname_np(pthread_self(), buf);
#endif
}

// 静态入口：pthread_create 需要的函数签名
// 这里把 arg 转回 Thread*，然后在托管线程中执行用户回调
void *Thread::run(void *arg) {
    Thread *thread_obj = static_cast<Thread *>(arg);
    // 在新线程中设置线程局部变量，使 GetThis() 可用
    t_thread = thread_obj;
    t_thread_name = thread_obj->m_name;

    // 如果需要设置内核 tid 到对象中（让主线程可以通过 getId() 读取）
#ifdef __linux__
    // Linux: 获取真实线程 id（tid）
    pid_t tid = static_cast<pid_t>(::syscall(SYS_gettid));
    thread_obj->m_id = static_cast<uint32_t>(tid);
#else
    // 非 Linux 平台：用 pthread_t 的 hash 值作为 id
    std::hash<pthread_t> hasher;
    thread_obj->m_id = static_cast<uint32_t>(hasher(pthread_self()));
#endif

    // 尝试设置 pthread 名称（便于调试）
#ifdef __linux__
    {
        char buf[16];
        std::strncpy(buf, thread_obj->m_name.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        // 忽略返回值：设置失败通常不影响功能
        pthread_setname_np(pthread_self(), buf);
    }
#endif

    // 执行用户回调，捕获异常以避免异常逃出线程边界（会导致 terminate）
    try {
        thread_obj->m_cb();
    } catch (const std::exception &ex) {
        LOG_ERROR(Log::LogManager::GetInstance().getRoot())
            << "Thread caught std::exception: "
            << ex.what() << "\n";
    } catch (...) {
        LOG_ERROR(Log::LogManager::GetInstance().getRoot())
            << "Thread caught unknown exception"
            << "\n";
    }

    // 线程函数返回前清理线程局部变量（可选）
    t_thread = nullptr;
    t_thread_name = "UNKNOWN";

    return nullptr;
}
const std::string &Thread::getName() const {
    return m_name;
}
} // namespace sunshine
