#pragma once
// 日志系统
#include <cstddef>
#include <memory>
#include <cstdint>
#include <string>
#include <list>
#include <sstream>
#include <fstream>
#include <iostream>
#include <vector>

namespace Log {

// 日志事件
class LogEvent {
public:
    typedef std::shared_ptr<LogEvent> ptr;
    LogEvent();

    // 你可以根据需要添加访问器（get/set）或静态创建函数
    const char *m_file = nullptr;
    uint32_t m_elapse = 0;   // 启动到现在运行的时间（毫秒）
    int32_t m_line = 0;      // 行号
    uint32_t m_threadid = 0; // 线程号
    uint32_t m_fiberid = 0;  // 协程号
    uint64_t m_time = 0;     // 时间戳（秒）
    std::string m_context;   // 内容
};

// 日志等级
class LogLevel {
public:
    enum Level {
        DEBUG = 1,
        INFO = 2,
        WARN = 3,
        ERROR = 4,
        FATAL = 5
    };
};

// 日志格式器
class LogFormatter {
public:
    typedef std::shared_ptr<LogFormatter> ptr;
    LogFormatter(const std::string &pattern);
    std::string format(LogEvent::ptr event);
    class FormatItem {
    public:
        typedef std::shared_ptr<FormatItem> ptr;
        virtual ~FormatItem();
        virtual void format(std::ostream &os, LogEvent::ptr even) = 0;
    };

    void init();

private:
    std::vector<FormatItem::ptr> m_items;
    std::string m_pattern;
};

// 日志输出地
class LogAppender {
public:
    typedef std::shared_ptr<LogAppender> ptr;

    virtual ~LogAppender();
    virtual void log(LogLevel::Level level, LogEvent::ptr event) = 0;
    void setLogFormatter(LogFormatter::ptr val) {
        m_formatter = val;
    }
    void setLevel(LogLevel::Level val) {
        m_level = val;
    }

protected:
    LogLevel::Level m_level = LogLevel::DEBUG;
    LogFormatter::ptr m_formatter; // 格式器
};

// 日志器
class Logger {
public:
    typedef std::shared_ptr<Logger> ptr;
    Logger(const std::string &name = "root");
    void log(LogLevel::Level level, LogEvent::ptr event);
    void debug(LogEvent::ptr event);
    void info(LogEvent::ptr event);
    void warn(LogEvent::ptr event);
    void error(LogEvent::ptr event);
    void fatal(LogEvent::ptr event);

    void addAppender(LogAppender::ptr appender); // 添加输出地
    void delAppender(LogAppender::ptr appender); // 删除输出地
    void setLevel(LogLevel::Level val) {
        m_level = val;
    };

protected:
    LogLevel::Level m_level = LogLevel::DEBUG; // 默认级别
    std::string m_name;                        // logger 名称
    std::list<LogAppender::ptr> m_appenders;   // 输出地列表
};

// 将日志输出到控制台的 appender
class StdoutLogAppender : public LogAppender {
public:
    typedef std::shared_ptr<StdoutLogAppender> ptr;
    void log(LogLevel::Level level, LogEvent::ptr event) override;
};

// 将日志输出到文件的 appender
class FileoutAppender : public LogAppender {
public:
    typedef std::shared_ptr<FileoutAppender> ptr;
    FileoutAppender(const std::string &val) {
        m_filename = val;
    }
    bool reopen();
    void log(LogLevel::Level level, LogEvent::ptr event) override;

private:
    std::string m_filename;
    std::ofstream m_filestream;
};

} // namespace Log
