#include <thread>
#include <memory>
#include <functional>
#include <pthread.h>
#include <string>
namespace sunshine {
class Thread {
public:
    typedef std::shared_ptr<Thread> ptr;
    Thread(std::function<void()> cb, std::string &name);
    ~Thread();
    uint32_t getId();
    const std::string &getName() const;
    void join(); //阻塞线程，等待这个线程结束
    static Thread *GetThis();
    static void SetName(const std::string name);

protected:
    Thread(const Thread &) = delete;
    Thread(const Thread &&) = delete;
    Thread &operator=(const Thread &) = delete;
    static void *run(void *arg);

private:
    uint32_t m_id = -1;         //线程id
    pthread_t m_thread = 0;     //线程
    std::string m_name;         //线程名
    std::function<void()> m_cb; //方法
};
} // namespace sunshine