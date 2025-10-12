// POSIX / socket 系统调用
#include <sys/types.h>  // 基本系统类型（有些系统需要）
#include <sys/socket.h> // socket(), bind(), listen(), accept4(), recv(), send()
#include <netinet/in.h> // sockaddr_in
#include <arpa/inet.h>  // htons(), inet_addr(), inet_pton()
#include <unistd.h>     // close(), read(), write()
#include <fcntl.h>      // fcntl(), O_NONBLOCK
#include <errno.h>      // errno
#include <string.h>     // strerror, memset
#include <signal.h>     // signal(), SIGPIPE（可选：忽略 SIGPIPE）

// C++ 标准库
#include <iostream>   // std::cout / std::cerr
#include <memory>     // std::shared_ptr, std::make_shared
#include <functional> // std::function, lambda
#include <string>     // std::string
#include <cassert>    // assert

// 你的工程头（必须）
#include "libs/iomanager.h" // IOManager（包含 Scheduler）
#include "libs/scheduler.h" // 若需要显式使用 Scheduler API
#include "libs/log.h"       // 日志（可选）

using namespace sunshine;

int main() {
    IOManager::ptr iom = std::make_shared<IOManager>(4, true, "ioman");
    iom->start(); // 启动线程，若希望 caller 也参与，请在 caller 线程调用 iom->run() 或在 constructor 用 use_caller=true 并在 caller 调用 run()

    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    // bind/listen 并 setNonBlock(listenfd)...
    // 注册 accept 的可读事件
    iom->addEvent(listenfd, IOManager::READ, [listenfd, iom]() {
        // ET 模式：循环 accept 直到 EAGAIN
        while (true) {
            std::cout << 1 << std::endl;
            int c = accept4(listenfd, nullptr, nullptr, SOCK_NONBLOCK);
            if (c < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                break;
            }
            // 注册 client 可读回调（尽量把实际处理放在协程中）
            iom->addEvent(c, IOManager::READ, [c, iom]() {
                char buf[4096];
                ssize_t n = recv(c, buf, sizeof(buf), 0);
                if (n > 0) {
                    // 处理数据
                } else if (n == 0) {
                    // 客户端关闭
                    iom->cancelAll(c);
                    close(c);
                } else if (errno == EAGAIN) {
                    // 数据读尽, 等待下次
                } else {
                    iom->cancelAll(c);
                    close(c);
                }
            });
        }
    });
}
