// main.cpp - 测试 sunshine::Thread
#include "libs/log.h"
#include "libs/fiber.h"
#include <iostream>
#include <vector>
#include <memory>
#include <string>
#include <chrono>
#include <thread>        // 用于主线程 sleep
#include <libs/thread.h> // 请根据你项目的实际路径调整
static std::string stateName(sunshine::Fiber::State s) {
    switch (s) {
    case sunshine::Fiber::INIT: return "INIT";
    case sunshine::Fiber::HOLD: return "HOLD";
    case sunshine::Fiber::EXEC: return "EXEC";
    case sunshine::Fiber::TERM: return "TERM";
    case sunshine::Fiber::READY: return "READY";
    default: return "UNKNOWN";
    }
}

int main() {
    using sunshine::Fiber;
    std::cout << "TotalFibers before creation: " << Fiber::TotalFibers() << "\n";

    // 创建一个协程，内部会先 YieldToReady，然后 YieldToHold，最后结束
    auto f = std::make_shared<Fiber>([]() {
        std::cout << "[fiber] started\n";

        std::cout << "[fiber] -> YieldToReady()\n";
        // 期望：把自己标记为 READY，然后切出到主协程（调度者）
        Fiber::YieldToReady();

        std::cout << "[fiber] resumed after YieldToReady\n";

        std::cout << "[fiber] -> YieldToHold()\n";
        // 期望：把自己标记为 HOLD，然后切出到主协程（调度者）
        Fiber::YieldToHold();

        std::cout << "[fiber] resumed after YieldToHold, finishing\n";
        // 返回则会把状态设为 TERM 并切回主协程
    });

    std::cout << "Fiber created. state = " << stateName(f->getState()) << " (expect INIT)\n";

    // 第一次 swapIn：进入协程，执行到 YieldToReady() 并切回这里
    std::cout << "main -> swapIn() #1\n";
    f->swapIn();
    std::cout << "main <- returned from swapIn #1, state = " << stateName(f->getState()) << " (expect READY)\n";
    if (f->getState() == Fiber::READY)
        std::cout << "[TEST] READY OK\n";
    else
        std::cout << "[TEST] READY FAIL\n";

    // 第二次 swapIn：继续运行协程到 YieldToHold() 并切回
    std::cout << "main -> swapIn() #2\n";
    f->swapIn();
    std::cout << "main <- returned from swapIn #2, state = " << stateName(f->getState()) << " (expect HOLD)\n";
    if (f->getState() == Fiber::HOLD)
        std::cout << "[TEST] HOLD OK\n";
    else
        std::cout << "[TEST] HOLD FAIL\n";

    // 第三次 swapIn：继续运行直到协程结束（TERM）
    std::cout << "main -> swapIn() #3\n";
    f->swapIn();
    std::cout << "main <- returned from swapIn #3, state = " << stateName(f->getState()) << " (expect TERM)\n";
    if (f->getState() == Fiber::TERM)
        std::cout << "[TEST] TERM OK\n";
    else
        std::cout << "[TEST] TERM FAIL\n";

    std::cout << "TotalFibers after: " << Fiber::TotalFibers() << "\n";

    return 0;
}
