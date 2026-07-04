#pragma once

//   ____                _ _
//  / __ \              (_) |
// | |  | |___ _ __ ___  _| |     ___   __ _  __ _  ___ _ __
// | |  | / __| '_ ` _ \| | |    / _ \ / _` |/ _` |/ _ \ '__|
// | |__| \__ \ | | | | | | |___| (_) | (_| | (_| |  __/ |
//  \____/|___/_| |_| |_|_|______\___/ \__, |\__, |\___|_|
//                                      __/ | __/ |
//                                     |___/ |___/
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>


enum class Level : std::uint8_t {
    TRACE = 0,
    DEBUG,
    INFO,
    WARN,
    ERROR,
    FATAL,
    OFF
};

namespace logger_detail {

inline const char* level_str(Level l) noexcept {
    switch (l) {
        case Level::TRACE: return "TRACE";
        case Level::DEBUG: return "DEBUG";
        case Level::INFO:  return "INFO ";
        case Level::WARN:  return "WARN ";
        case Level::ERROR: return "ERROR";
        case Level::FATAL: return "FATAL";
        default:           return "?    ";
    }
}

inline const char* level_color(Level l) noexcept {
    switch (l) {
        case Level::TRACE: return "\033[36m";
        case Level::DEBUG: return "\033[36m";
        case Level::INFO:  return "\033[32m";
        case Level::WARN:  return "\033[33m";
        case Level::ERROR: return "\033[31m";
        case Level::FATAL: return "\033[35;1m";
        default:           return "\033[0m";
    }
}

inline std::string make_timestamp() {
    using namespace std::chrono;
    const auto now  = system_clock::now();
    const auto ns = duration_cast<nanoseconds>(now.time_since_epoch()).count();
    const auto secs = ns / 1'000'000'000LL;
    const auto frac = ns % 1'000'000'000LL;

    const std::time_t t = static_cast<std::time_t>(secs);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);

    char out[48];
    std::snprintf(out, sizeof(out), "%s.%09lld", buf,
                  static_cast<long long>(frac));
    return {out};
}



inline void format_into(std::ostringstream& oss, const char* fmt) {
    oss << fmt;   // no args left
}

template <typename T, typename... Rest>
inline void format_into(std::ostringstream& oss, const char* fmt,
                        T&& first, Rest&&... rest) {
    while (*fmt) {
        if (fmt[0] == '{' && fmt[1] == '}') {
            oss << std::forward<T>(first);
            format_into(oss, fmt + 2, std::forward<Rest>(rest)...);
            return;
        }
        oss << *fmt++;
    }
}

template <typename... Args>
inline std::string format(const char* fmt, Args&&... args) {
    std::ostringstream oss;
    format_into(oss, fmt, std::forward<Args>(args)...);
    return oss.str();
}

struct LogEntry {
    std::string timestamp;
    Level level;
    std::string message;
    std::uint32_t thread_id;
};

inline uint32_t current_thread_id() {
    static thread_local const std::uint32_t id = []() -> std::uint32_t {
        std::ostringstream oss;
        oss << std::this_thread::get_id();
        return static_cast<std::uint32_t>(std::hash<std::string>{}(oss.str()));
    }();
    return id;
}

//unoptimmized
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity)
        : capacity_(capacity), buf_(capacity), head_(0), tail_(0) {}

    bool push(LogEntry entry) {
        std::lock_guard<std::mutex> lk(mtx_);
        const size_t next = (head_ + 1) % capacity_;
        if (next == tail_) return false;
        buf_[head_] = std::move(entry);
        head_= next;
        cv_.notify_one();
        return true;
    }

    bool pop(LogEntry& out, bool wait) {
        std::unique_lock<std::mutex> lk(mtx_);
        if (wait) {
            cv_.wait(lk, [this] { return head_ != tail_ || stop_; });
        }
        if (head_ == tail_) return false;
        out  = std::move(buf_[tail_]);
        tail_ = (tail_ + 1) % capacity_;
        return true;
    }

    void signal_stop() {
        std::lock_guard<std::mutex> lk(mtx_);
        stop_ = true;
        cv_.notify_all();
    }

    bool stopping() const noexcept { return stop_; }

private:
    size_t capacity_;
    std::vector<LogEntry> buf_;
    size_t head_;
    size_t tail_;
    bool stop_{false};
    std::mutex mtx_;
    std::condition_variable cv_;
};

}


class OsmiLogger {
public:

    explicit OsmiLogger(const std::string& path,
                    Level min_level = Level::TRACE,
                    size_t buf_size  = 65536,
                    bool color_stderr = true)
        : min_level_(min_level),
          color_stderr_(color_stderr),
          buffer_(std::make_unique<logger_detail::RingBuffer>(buf_size))
    {
        if (!path.empty()) {
            file_.open(path, std::ios::app);
            if (!file_.is_open())
                std::cerr << "[OsmiLogger] could not open log file: "
                          << path << "\n";
        }
        worker_ = std::thread(&OsmiLogger::drain, this);
    }

    ~OsmiLogger() {
        buffer_->signal_stop();
        if (worker_.joinable()) worker_.join();
        //drain buffer
        logger_detail::LogEntry e;
        while (buffer_->pop(e, false)) write_entry(e);
    }

    //ro3
    OsmiLogger(const OsmiLogger&) = delete;
    OsmiLogger& operator=(const OsmiLogger&) = delete;


    template <typename... Args>
    void log(Level level, const char* fmt, Args&&... args) {
        if (level < min_level_ || min_level_ == Level::OFF) return;

        logger_detail::LogEntry e;
        e.timestamp = logger_detail::make_timestamp();
        e.level = level;
        e.message = logger_detail::format(fmt, std::forward<Args>(args)...);
        e.thread_id = logger_detail::current_thread_id();

        if (!buffer_->push(std::move(e))) {
            // buffer full
            ++dropped_;
        }
    }

    void trace(const char* msg) { log(Level::TRACE, msg); }
    void debug(const char* msg) { log(Level::DEBUG, msg); }
    void info (const char* msg) { log(Level::INFO,  msg); }
    void warn (const char* msg) { log(Level::WARN,  msg); }
    void error(const char* msg) { log(Level::ERROR, msg); }
    void fatal(const char* msg) { log(Level::FATAL, msg); }

    void set_level(Level l) noexcept { min_level_ = l; }
    Level get_level() const noexcept { return min_level_; }
    std::uint64_t dropped_count() const noexcept { return dropped_.load(); }


    void flush() {

        using namespace std::chrono_literals;

        for (int i = 0; i < 1000; ++i) {
            std::this_thread::sleep_for(1ms);
            logger_detail::LogEntry dummy;

        }
        if (file_.is_open()) file_.flush();
    }

private:
    void drain() {
        logger_detail::LogEntry e;
        while (!buffer_->stopping()) {
            if (buffer_->pop(e, true))
                write_entry(e);
        }
        while (buffer_->pop(e, false))
            write_entry(e);
    }

    void write_entry(const logger_detail::LogEntry& e) {

        std::ostringstream line;
        line << '[' << e.timestamp << ']' << " [0sm1L0gg3r ^_^] "
             << " [" << logger_detail::level_str(e.level) << ']'
             << " [tid:" << std::hex << std::setw(8) << std::setfill('0')
             << e.thread_id << std::dec << "] "
             << e.message << '\n';

        const std::string plain = line.str();

        if (file_.is_open())
            file_ << plain;

        if (color_stderr_) {
            std::cerr << logger_detail::level_color(e.level)
                      << plain
                      << "\033[0m";
        } else {
            std::cerr << plain;
        }
    }

    Level min_level_;
    bool color_stderr_;
    std::unique_ptr<logger_detail::RingBuffer> buffer_;
    std::ofstream file_;
    std::thread worker_;
    std::atomic<std::uint64_t> dropped_{0};
};
