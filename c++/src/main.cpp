// main.cpp - 测试 sunshine::Thread
#include "libs/log.h"
#include <iostream>
#include <vector>
#include <memory>
#include <string>
#include <chrono>
#include <thread>        // 用于主线程 sleep
#include <libs/thread.h> // 请根据你项目的实际路径调整

int main() {
    using namespace sunshine;
    LOG_DEBUG(LogManager::GetInstance().getRoot()) << "This is DEBUG" << '\n';
}
