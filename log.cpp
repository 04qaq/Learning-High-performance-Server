#include "log.h"
#include <cstdio>
#include <unordered_map>
#include <functional>
#include <chrono>
#include <ctime>
#include <thread>
#include <iomanip>

namespace Log {
// ---------------------- LogEvent ----------------------
LogEvent::LogEvent() {
    m_time = static_cast<uint64_t>(std::time(nullptr)); // seconds since epoch
    // 使用线程 id 的哈希值作为 uint32_t 的线程标识（简单处理）
    std::hash<std::thread::id> hasher;
    m_threadid = static_cast<uint32_t>(hasher(std::this_thread::get_id()) & 0xffffffffu);
    m_elapse = 0;
    m_line = 0;
    m_fiberid = 0;
    m_file = nullptr;
    m_context.clear();
}

// ---------------------- Logger ----------------------
Logger::Logger(const std::string &name) :
    m_name(name) {
}

void Logger::log(LogLevel::Level level, LogEvent::ptr event) {
    if (level >= m_level) { // 大于等于时输出
        for (auto &it : m_appenders) {
            if (it) it->log(level, event);
        }
    }
}
void Logger::debug(LogEvent::ptr event) {
    std::cout << "debug\n";
    log(LogLevel::DEBUG, event);
}
void Logger::info(LogEvent::ptr event) {
    log(LogLevel::INFO, event);
}
void Logger::warn(LogEvent::ptr event) {
    log(LogLevel::WARN, event);
}
void Logger::error(LogEvent::ptr event) {
    log(LogLevel::ERROR, event);
}
void Logger::fatal(LogEvent::ptr event) {
    log(LogLevel::FATAL, event);
}

void Logger::addAppender(LogAppender::ptr appender) {
    m_appenders.push_back(appender);
}
void Logger::delAppender(LogAppender::ptr appender) {
    m_appenders.remove(appender);
}

// ---------------------- LogAppender ----------------------
LogAppender::~LogAppender() = default;

// ---------------------- StdoutLogAppender ----------------------
void StdoutLogAppender::log(LogLevel::Level level, LogEvent::ptr event) {
    if (level >= m_level && m_formatter) {
        std::cout << m_formatter->format(event);
    }
}

// ---------------------- FileoutAppender ----------------------
bool FileoutAppender::reopen() {
    if (m_filestream.is_open()) {
        m_filestream.close();
    }
    m_filestream.open(m_filename, std::ios::app);
    return m_filestream.is_open();
}
void FileoutAppender::log(LogLevel::Level level, LogEvent::ptr event) {
    if (level >= m_level && m_formatter) {
        if (!m_filestream.is_open()) {
            reopen();
        }
        if (m_filestream.is_open()) {
            m_filestream << m_formatter->format(event);
        }
    }
}

// ---------------------- LogFormatter ----------------------
// 需要实现 FormatItem 的析构和若干具体 Item（在 cpp 中定义以减少头文件暴露）

LogFormatter::FormatItem::~FormatItem() = default;

LogFormatter::LogFormatter(const std::string &pattern) :
    m_pattern(pattern) {
    init();
}

std::string LogFormatter::format(LogEvent::ptr event) {
    std::ostringstream ss;
    for (auto &it : m_items) {
        if (it) it->format(ss, event);
    }
    return ss.str();
}

// 下面在 cpp 中定义具体的 FormatItem 子类（仅在实现文件可见）
// String (literal text)
class StringFormatItem : public LogFormatter::FormatItem {
public:
    StringFormatItem(const std::string &str) :
        m_string(str) {
    }
    void format(std::ostream &os, LogEvent::ptr) override {
        os << m_string;
    }

private:
    std::string m_string;
};

// Message (we use m_context as content)
class MessageFormatItem : public LogFormatter::FormatItem {
public:
    void format(std::ostream &os, LogEvent::ptr ev) override {
        if (ev) os << ev->m_context;
    }
};

// Level - print numeric for now (you can map to names)
class LevelFormatItem : public LogFormatter::FormatItem {
public:
    void format(std::ostream &os, LogEvent::ptr ev) override {
        if (!ev) return;
        // 简单根据枚举值输出文本
        // 如果你想输出字符串名称，可由 Logger/LogLevel 提供映射
        // 这里仅示例常见映射
        int lvl = 0;
        // 没有直接保存 level 在 LogEvent 中，若需要可扩展 LogEvent 增加 level 字段
        // 这里默认不输出
        (void)lvl;
    }
};

// Elapse (m_elapse)
class ElapseFormatItem : public LogFormatter::FormatItem {
public:
    void format(std::ostream &os, LogEvent::ptr ev) override {
        if (ev) os << ev->m_elapse;
    }
};

// Logger name (not stored in LogEvent currently)
class NameFormatItem : public LogFormatter::FormatItem {
public:
    void format(std::ostream &os, LogEvent::ptr) override {
        // 若需要 Logger 名称，请把 Logger 名称放进 LogEvent 或在 format() 外部拼接
    }
};

// Date format item, supports strftime-style format string
class DateFormatItem : public LogFormatter::FormatItem {
public:
    DateFormatItem(const std::string &fmt = "%Y-%m-%d %H:%M:%S") :
        m_fmt(fmt.empty() ? "%Y-%m-%d %H:%M:%S" : fmt) {
    }
    void format(std::ostream &os, LogEvent::ptr ev) override {
        if (!ev) return;
        std::time_t t = static_cast<std::time_t>(ev->m_time);
        std::tm tm;
#if defined(_WIN32) || defined(_WIN64)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char buf[256] = {0};
        if (std::strftime(buf, sizeof(buf), m_fmt.c_str(), &tm)) {
            os << buf;
        } else {
            os << "InvalidDate";
        }
    }

private:
    std::string m_fmt;
};

// Thread id
class ThreadIdFormatItem : public LogFormatter::FormatItem {
public:
    void format(std::ostream &os, LogEvent::ptr ev) override {
        if (ev) os << ev->m_threadid;
    }
};

// File
class FileFormatItem : public LogFormatter::FormatItem {
public:
    void format(std::ostream &os, LogEvent::ptr ev) override {
        if (ev && ev->m_file) os << ev->m_file;
    }
};

// Line
class LineFormatItem : public LogFormatter::FormatItem {
public:
    void format(std::ostream &os, LogEvent::ptr ev) override {
        if (ev) os << ev->m_line;
    }
};

// Newline
class NewLineFormatItem : public LogFormatter::FormatItem {
public:
    void format(std::ostream &os, LogEvent::ptr) override {
        os << std::endl;
    }
};

// init 实现（简洁、工程化，基于工厂表）
void LogFormatter::init() {
    m_items.clear();
    std::unordered_map<char, std::function<FormatItem::ptr(const std::string &)>> factories = {
        {'m', [](const std::string &) { return std::make_shared<MessageFormatItem>(); }},
        {'p', [](const std::string &) { return std::make_shared<LevelFormatItem>(); }},
        {'r', [](const std::string &) { return std::make_shared<ElapseFormatItem>(); }},
        {'c', [](const std::string &) { return std::make_shared<NameFormatItem>(); }},
        {'d', [this](const std::string &fmt) { return std::make_shared<DateFormatItem>(fmt); }},
        {'t', [](const std::string &) { return std::make_shared<ThreadIdFormatItem>(); }},
        {'f', [](const std::string &) { return std::make_shared<FileFormatItem>(); }},
        {'l', [](const std::string &) { return std::make_shared<LineFormatItem>(); }},
        {'n', [](const std::string &) { return std::make_shared<NewLineFormatItem>(); }},
    };

    std::string text_buf;
    const std::string &p = m_pattern;
    for (size_t i = 0; i < p.size(); ++i) {
        if (p[i] != '%') {
            text_buf.push_back(p[i]);
            continue;
        }
        // handle %%
        if (i + 1 < p.size() && p[i + 1] == '%') {
            text_buf.push_back('%');
            ++i;
            continue;
        }
        // push text buffer
        if (!text_buf.empty()) {
            m_items.push_back(std::make_shared<StringFormatItem>(std::move(text_buf)));
            text_buf.clear();
        }
        if (i + 1 >= p.size()) {
            // trailing %
            m_items.push_back(std::make_shared<StringFormatItem>("%"));
            break;
        }
        char spec = p[++i];
        std::string fmt;
        // parse optional { ... }
        if (i + 1 < p.size() && p[i + 1] == '{') {
            size_t j = i + 2;
            size_t end = j;
            while (end < p.size() && p[end] != '}') ++end;
            if (end < p.size() && p[end] == '}') {
                fmt = p.substr(j, end - j);
                i = end; // move to '}'
            }
        }
        auto it = factories.find(spec);
        if (it != factories.end()) {
            m_items.push_back(it->second(fmt));
        } else {
            // unknown spec: output as literal "%x"
            std::string s = "%";
            s.push_back(spec);
            m_items.push_back(std::make_shared<StringFormatItem>(s));
        }
    }
    if (!text_buf.empty()) {
        m_items.push_back(std::make_shared<StringFormatItem>(std::move(text_buf)));
        text_buf.clear();
    }
}

} // namespace Log
