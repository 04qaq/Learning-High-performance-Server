#include "libs/log.h"
#include <chrono>
#include <ctime>
#include <functional>
#include <thread>
#include <iomanip>

namespace Log {

// ---------------- LogEvent ----------------
LogEvent::LogEvent(const std::string &logger_name,
                   uint32_t thread_id,
                   const char *file,
                   int32_t line,
                   uint64_t time,
                   uint32_t elapse,
                   const std::string &ctx,
                   int32_t fiberid,
                   int level) :
    m_file(file),
    m_elapse(elapse),
    m_line(line),
    m_threadid(thread_id),
    m_fiberid(fiberid),
    m_time(time),
    m_context(ctx),
    m_loggerName(logger_name),
    m_level(level) {
}

// ---------------- LogFormatter ----------------
LogFormatter::FormatItem::~FormatItem() = default;

LogFormatter::LogFormatter(const std::string &pattern) :
    m_pattern(pattern) {
    init();
}

std::string LogFormatter::format(std::shared_ptr<LogEvent> event) {
    std::ostringstream ss;
    for (auto &it : m_items) {
        if (it) it->format(ss, event);
    }
    return ss.str();
}

// 一些 FormatItem 实现（仅在 cpp 内部可见）
class StringFormatItem : public LogFormatter::FormatItem {
public:
    StringFormatItem(const std::string &str) :
        m_string(str) {
    }
    void format(std::ostream &os, std::shared_ptr<LogEvent>) override {
        os << m_string;
    }

private:
    std::string m_string;
};

class MessageFormatItem : public LogFormatter::FormatItem {
public:
    void format(std::ostream &os, std::shared_ptr<LogEvent> ev) override {
        if (ev) {
            // 优先使用流中内容，否则使用 m_context
            std::string s = ev->getContent();
            if (s.empty()) s = ev->m_context;
            os << s;
        }
    }
};

class LevelFormatItem : public LogFormatter::FormatItem {
public:
    void format(std::ostream &os, std::shared_ptr<LogEvent> ev) override {
        if (!ev) return;
        os << LogLevel::toString(static_cast<LogLevel::Level>(ev->m_level));
    }
};

class ElapseFormatItem : public LogFormatter::FormatItem {
public:
    void format(std::ostream &os, std::shared_ptr<LogEvent> ev) override {
        if (ev) os << ev->m_elapse;
    }
};

class NameFormatItem : public LogFormatter::FormatItem {
public:
    void format(std::ostream &os, std::shared_ptr<LogEvent> ev) override {
        if (ev) os << ev->m_loggerName;
    }
};

class DateFormatItem : public LogFormatter::FormatItem {
public:
    DateFormatItem(const std::string &fmt = "%Y-%m-%d %H:%M:%S") :
        m_fmt(fmt.empty() ? "%Y-%m-%d %H:%M:%S" : fmt) {
    }
    void format(std::ostream &os, std::shared_ptr<LogEvent> ev) override {
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

class ThreadIdFormatItem : public LogFormatter::FormatItem {
public:
    void format(std::ostream &os, std::shared_ptr<LogEvent> ev) override {
        if (ev) os << ev->m_threadid;
    }
};

class FileFormatItem : public LogFormatter::FormatItem {
public:
    void format(std::ostream &os, std::shared_ptr<LogEvent> ev) override {
        if (ev && ev->m_file) os << ev->m_file;
    }
};

class LineFormatItem : public LogFormatter::FormatItem {
public:
    void format(std::ostream &os, std::shared_ptr<LogEvent> ev) override {
        if (ev) os << ev->m_line;
    }
};

class NewLineFormatItem : public LogFormatter::FormatItem {
public:
    void format(std::ostream &os, std::shared_ptr<LogEvent>) override {
        os << std::endl;
    }
};

// init: 解析 pattern
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
        if (i + 1 < p.size() && p[i + 1] == '%') {
            text_buf.push_back('%');
            ++i;
            continue;
        }
        if (!text_buf.empty()) {
            m_items.push_back(std::make_shared<StringFormatItem>(std::move(text_buf)));
            text_buf.clear();
        }
        if (i + 1 >= p.size()) {
            m_items.push_back(std::make_shared<StringFormatItem>("%"));
            break;
        }
        char spec = p[++i];
        std::string fmt;
        if (i + 1 < p.size() && p[i + 1] == '{') {
            size_t j = i + 2;
            size_t end = j;
            while (end < p.size() && p[end] != '}') ++end;
            if (end < p.size() && p[end] == '}') {
                fmt = p.substr(j, end - j);
                i = end;
            }
        }
        auto it = factories.find(spec);
        if (it != factories.end()) {
            m_items.push_back(it->second(fmt));
        } else {
            std::string s = "%";
            s.push_back(spec);
            m_items.push_back(std::make_shared<StringFormatItem>(s));
        }
    }
    if (!text_buf.empty()) {
        m_items.push_back(std::make_shared<StringFormatItem>(std::move(text_buf)));
    }
}

// ---------------- LogAppender impl ----------------
LogAppender::~LogAppender() = default;

void StdoutLogAppender::log(LogLevel::Level level, std::shared_ptr<LogEvent> event) {
    if (level < m_level) return;
    std::lock_guard<std::mutex> lk(m_mutex);
    LogFormatter::ptr f = m_formatter;
    if (!f && event) {
        static LogFormatter::ptr s_def = std::make_shared<LogFormatter>();
        f = s_def;
    }
    if (f) {
        std::cout << f->format(event);
    } else {
        std::cout << event->getContent() << std::endl;
    }
}

FileoutAppender::FileoutAppender(const std::string &filename) :
    m_filename(filename) {
    reopen();
}

bool FileoutAppender::reopen() {
    std::lock_guard<std::mutex> lk(m_file_mutex);
    if (m_filestream.is_open()) m_filestream.close();
    m_filestream.open(m_filename, std::ios::app);
    return m_filestream.is_open();
}

void FileoutAppender::log(LogLevel::Level level, std::shared_ptr<LogEvent> event) {
    if (level < m_level) return;
    std::lock_guard<std::mutex> lk(m_file_mutex);
    if (!m_filestream.is_open()) {
        if (!reopen()) return;
    }
    LogFormatter::ptr f = m_formatter;
    if (!f) {
        static LogFormatter::ptr s_def = std::make_shared<LogFormatter>();
        f = s_def;
    }
    if (f) {
        m_filestream << f->format(event);
    } else {
        m_filestream << event->getContent() << std::endl;
    }
}

// ---------------- Logger ----------------
Logger::Logger(const std::string &name) :
    m_name(name) {
    m_formatter = std::make_shared<LogFormatter>(); // 默认格式器
}

void Logger::log(LogLevel::Level level, std::shared_ptr<LogEvent> event) {
    if (level < m_level) return;
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto &app : m_appenders) {
        if (!app) continue;
        if (!app->getFormatter()) {
            app->setFormatter(m_formatter);
        }
        app->log(level, event);
    }
}

void Logger::debug(std::shared_ptr<LogEvent> event) {
    log(LogLevel::DEBUG, event);
}
void Logger::info(std::shared_ptr<LogEvent> event) {
    log(LogLevel::INFO, event);
}
void Logger::warn(std::shared_ptr<LogEvent> event) {
    log(LogLevel::WARN, event);
}
void Logger::error(std::shared_ptr<LogEvent> event) {
    log(LogLevel::ERROR, event);
}
void Logger::fatal(std::shared_ptr<LogEvent> event) {
    log(LogLevel::FATAL, event);
}

void Logger::addAppender(LogAppender::ptr appender) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_appenders.push_back(appender);
}
void Logger::delAppender(LogAppender::ptr appender) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_appenders.remove(appender);
}

// ---------------- LogEventWrap ----------------
LogEventWrap::LogEventWrap(Logger::ptr logger, std::shared_ptr<LogEvent> event) :
    m_logger(logger), m_event(event) {
}

LogEventWrap::~LogEventWrap() {
    if (m_logger && m_event) {
        // 将流内容复制到 m_context（以兼容旧 formatter）
        m_event->m_context = m_event->getContent();
        m_logger->log(static_cast<LogLevel::Level>(m_event->m_level), m_event);
    }
}

// ---------------- LogManager ----------------
LogManager::LogManager() {
    m_root = std::make_shared<Logger>("root");
    // root 默认添加一个 stdout appender
    auto stdout_app = std::make_shared<StdoutLogAppender>();
    m_root->addAppender(stdout_app);
    m_loggers[m_root->getName()] = m_root;
}

LogManager &LogManager::GetInstance() {
    static LogManager s_inst;
    return s_inst;
}

Logger::ptr LogManager::getRoot() {
    return m_root;
}

Logger::ptr LogManager::getLogger(const std::string &name) {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_loggers.find(name);
    if (it != m_loggers.end()) return it->second;
    auto logger = std::make_shared<Logger>(name);
    logger->setFormatter(m_root->getFormatter()); // 继承 root 的 formatter
    m_loggers[name] = logger;
    return logger;
}

} // namespace Log