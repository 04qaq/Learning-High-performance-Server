#pragma once
// 工程化日志系统 - header
#include <cstddef>
#include <memory>
#include <cstdint>
#include <string>
#include <list>
#include <sstream>
#include <fstream>
#include <iostream>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <thread>
namespace Log {

// 前向声明
class Logger;
class LogFormatter;
class LogAppender;

//
// LogEvent: 表示一次日志（包含用于格式化的信息和用于流式写入的 stringstream）
//
class LogEvent {
public:
    typedef std::shared_ptr<LogEvent> ptr;
    //构造函数
    LogEvent(const std::string &logger_name,
             uint32_t thread_id,
             const char *file,
             int32_t line,
             uint64_t time,
             uint32_t elapse,
             const std::string &ctx,
             int32_t fiberid,
             int level);

    // 直接通过流拼接消息（流式风格）
    std::ostringstream &getSS() {
        return m_ss;
    }
    std::string getContent() const {
        return m_ss.str();
    }

    // 基本字段（public 便于 formatter 访问）
    const char *m_file = nullptr;
    uint32_t m_elapse = 0;    //计时器，用于记录启动到现在多久了
    int32_t m_line = 0;       //行号
    uint32_t m_threadid = 0;  //线程id
    uint32_t m_fiberid = 0;   //协程id
    uint64_t m_time = 0;      //时间戳
    std::string m_context;    // 可选上下文字段（额外）
    std::string m_loggerName; //对应的日志器名称
    int m_level = 0;          // LogLevel::Level
private:
    std::ostringstream m_ss;
};

//
// LogLevel
//
class LogLevel {
public:
    enum Level {
        DEBUG = 1,
        INFO = 2,
        WARN = 3,
        ERROR = 4,
        FATAL = 5
    };
    //返回等级的string
    static const char *toString(Level level) {
        switch (level) {
        case DEBUG: return "DEBUG";
        case INFO: return "INFO";
        case WARN: return "WARN";
        case ERROR: return "ERROR";
        case FATAL: return "FATAL";
        default: return "UNKNOWN";
        }
    }
};

//
// LogFormatter: 支持类似 "%d{%Y-%m-%d %H:%M:%S} [%p] %c %t %f:%l %m%n" 的 pattern
//
class LogFormatter {
public:
    typedef std::shared_ptr<LogFormatter> ptr;
    LogFormatter(const std::string &pattern = "%d{%Y-%m-%d %H:%M:%S} [%p] %c %t %f:%l %m%n");
    std::string format(std::shared_ptr<LogEvent> event);
    class FormatItem {
    public:
        typedef std::shared_ptr<FormatItem> ptr;
        virtual ~FormatItem();
        virtual void format(std::ostream &os, std::shared_ptr<LogEvent> event) = 0;
    };

    //解析日志内容
    void init();

private:
    std::vector<FormatItem::ptr> m_items; //有哪些日志格式类型
    std::string m_pattern;
};

//
// LogAppender: 输出目标（控制台 / 文件），每个 appender 可有自己的 formatter
//
class LogAppender {
public:
    typedef std::shared_ptr<LogAppender> ptr;
    virtual ~LogAppender();
    virtual void log(LogLevel::Level level, std::shared_ptr<LogEvent> event) = 0;

    void setFormatter(LogFormatter::ptr val) {
        m_formatter = val;
    }
    LogFormatter::ptr getFormatter() const {
        return m_formatter;
    }
    void setLevel(LogLevel::Level val) {
        m_level = val;
    }
    LogLevel::Level getLevel() const {
        return m_level;
    }

protected:
    std::mutex m_mutex;
    LogLevel::Level m_level = LogLevel::DEBUG;
    LogFormatter::ptr m_formatter;
};

//
// Logger: 管理多个 appender，线程安全
//
class Logger : public std::enable_shared_from_this<Logger> {
public:
    typedef std::shared_ptr<Logger> ptr;
    Logger(const std::string &name = "root"); //默认构造一个名为root的日志器

    void log(LogLevel::Level level, std::shared_ptr<LogEvent> event);

    // 便利函数
    void debug(std::shared_ptr<LogEvent> event);
    void info(std::shared_ptr<LogEvent> event);
    void warn(std::shared_ptr<LogEvent> event);
    void error(std::shared_ptr<LogEvent> event);
    void fatal(std::shared_ptr<LogEvent> event);

    void addAppender(LogAppender::ptr appender);
    void delAppender(LogAppender::ptr appender);

    void setLevel(LogLevel::Level val) {
        m_level = val;
    }
    LogLevel::Level getLevel() const {
        return m_level;
    }

    void setFormatter(LogFormatter::ptr val) {
        m_formatter = val;
    }
    LogFormatter::ptr getFormatter() const {
        return m_formatter;
    }

    const std::string &getName() const {
        return m_name;
    }

private:
    LogLevel::Level m_level = LogLevel::DEBUG;
    std::string m_name;
    std::list<LogAppender::ptr> m_appenders;
    LogFormatter::ptr m_formatter; // logger 默认格式器（appender 没有则使用它）
    mutable std::mutex m_mutex;
};

//
// Stdout 和 File appender 实现
//
class StdoutLogAppender : public LogAppender {
public:
    typedef std::shared_ptr<StdoutLogAppender> ptr;
    void log(LogLevel::Level level, std::shared_ptr<LogEvent> event) override;
};

class FileoutAppender : public LogAppender {
public:
    typedef std::shared_ptr<FileoutAppender> ptr;
    FileoutAppender(const std::string &filename);
    bool reopen();
    void log(LogLevel::Level level, std::shared_ptr<LogEvent> event) override;

private:
    std::string m_filename;     //文件地址
    std::ofstream m_filestream; //文件流
    std::mutex m_file_mutex;    //线程锁
};

//
// LogEventWrap: RAII 帮助类，支持流式写法，析构时把内容发给 Logger
// 用法： LOG_DEBUG(logger) << "x=" << x;
//
class LogEventWrap {
public:
    LogEventWrap(Logger::ptr logger, std::shared_ptr<LogEvent> event);
    ~LogEventWrap();

    //返回event的文本内容
    std::ostream &getSS() {
        return m_event->getSS();
    }

private:
    Logger::ptr m_logger;
    std::shared_ptr<LogEvent> m_event;
};

//
// LogManager: 全局管理器，获取 root 或指定名称的 Logger
//
class LogManager {
public:
    static LogManager &GetInstance();               //构造一个static修饰的全局管理器
    Logger::ptr getLogger(const std::string &name); //返回名为name的logger
    Logger::ptr getRoot();                          //返回默认日志器

private:
    LogManager();
    std::unordered_map<std::string, Logger::ptr> m_loggers;
    Logger::ptr m_root;
    std::mutex m_mutex;
};

#define LOG_EVENT(logger_ptr, level)                                                                                                                 \
    Log::LogEventWrap((logger_ptr), std::make_shared<Log::LogEvent>((logger_ptr)->getName(),                                                         \
                                                                    static_cast<uint32_t>(std::hash<std::thread::id>()(std::this_thread::get_id())), \
                                                                    __FILE__, __LINE__, static_cast<uint64_t>(time(nullptr)), 0, "", 0, level))      \
        .getSS()

#define LOG_DEBUG(logger) LOG_EVENT(logger, Log::LogLevel::DEBUG)
#define LOG_INFO(logger) LOG_EVENT(logger, Log::LogLevel::INFO)
#define LOG_WARN(logger) LOG_EVENT(logger, Log::LogLevel::WARN)
#define LOG_ERROR(logger) LOG_EVENT(logger, Log::LogLevel::ERROR)
#define LOG_FATAL(logger) LOG_EVENT(logger, Log::LogLevel::FATAL)

} // namespace Log