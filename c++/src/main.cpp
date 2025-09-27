
#include "libs/log.h"
#include "libs/Config.h"
#include <yaml-cpp/yaml.h>
#include <iostream>
#include <vector>

int main() {
    // 取得 root logger（LogManager 初始化时已为 root 添加 StdoutLogAppender）
    auto root = Log::LogManager::GetInstance().getRoot();

    // 设置 root level（例如 INFO，低于该等级的日志不会输出）
    root->setLevel(Log::LogLevel::DEBUG); // 按需改为 INFO/WARN...

    // 设置一个简洁的控制台格式：时间 [LEVEL] logger - message
    auto consoleFmt = std::make_shared<Log::LogFormatter>("%d{%H:%M:%S} [%p] %c - %m%n");
    // 将格式器设置到 logger（logger 在输出时会把格式器分配给 appender）
    root->setFormatter(consoleFmt);

    // 确认只有控制台输出（默认 LogManager 构造时已经添加了 StdoutLogAppender）
    LOG_INFO(root) << "Console logger initialized";

    // 示例：创建一些配置项（默认值）
    auto cfg_port = Log::Config::Lookup<int>("server.port", 8080, "server listen port");
    auto cfg_name = Log::Config::Lookup<std::string>("server.name", std::string("myserver"), "server name");
    auto cfg_nodes = Log::Config::Lookup<std::vector<int>>("server.ports", std::vector<int>{80, 443}, "server ports");

    // 打印默认值到控制台
    LOG_INFO(root) << "Defaults -> server.port = " << cfg_port->toString();
    LOG_INFO(root) << "Defaults -> server.name = " << cfg_name->toString();
    LOG_INFO(root) << "Defaults -> server.ports = " << cfg_nodes->toString();

    // 如果存在 config.yaml，则尝试加载（不会输出到文件，仅会触发控制台日志）
    try {
        YAML::Node rootYaml = YAML::LoadFile("config.yaml");
        Log::Config::FromYaml(rootYaml);
        LOG_INFO(root) << "Loaded config.yaml (if present)";
    } catch (const std::exception &e) {
        LOG_WARN(root) << "No config.yaml found or parse failed: " << e.what();
    }

    // 打印加载后的值
    LOG_INFO(root) << "After load -> server.port = " << cfg_port->toString();
    LOG_INFO(root) << "After load -> server.name = " << cfg_name->toString();
    LOG_INFO(root) << "After load -> server.ports = " << cfg_nodes->toString();

    // 示例不同等级日志（都会在控制台打印，取决于 root 的 level）
    LOG_DEBUG(root) << "This is a DEBUG message (visible when level <= DEBUG)";
    LOG_INFO(root) << "This is an INFO message";
    LOG_WARN(root) << "This is a WARN message";
    LOG_ERROR(root) << "This is an ERROR message";

    return 0;
}