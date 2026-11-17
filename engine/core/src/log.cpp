// SPDX-License-Identifier: Apache-2.0
#include "victus/core/log.hpp"

#if defined(__ANDROID__)
#include <android/log.h>
#endif

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <thread>

namespace victus::log {

const char* to_string(Level level) noexcept {
    switch (level) {
    case Level::Trace: return "trace";
    case Level::Debug: return "debug";
    case Level::Info: return "info";
    case Level::Warn: return "warn";
    case Level::Error: return "error";
    case Level::Fatal: return "fatal";
    }
    return "?";
}

bool parse_level(std::string_view text, Level& out) noexcept {
    for (int i = 0; i <= static_cast<int>(Level::Fatal); ++i) {
        if (text == to_string(static_cast<Level>(i))) {
            out = static_cast<Level>(i);
            return true;
        }
    }
    return false;
}

namespace {
std::string format_time(std::chrono::system_clock::time_point tp) {
    const auto t = std::chrono::system_clock::to_time_t(tp);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count() % 1000;
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms));
    return buf;
}

std::string format_line(const Record& r) {
    return std::format("[{}] [{}] [{}] {}\n", format_time(r.time), to_string(r.level), r.category, r.message);
}
} // namespace

void ConsoleSink::write(const Record& record) {
#if defined(__ANDROID__)
    // stdout/stderr go nowhere on Android; the console is logcat (`adb logcat -s VICTUS`).
    int prio = ANDROID_LOG_INFO;
    switch (record.level) {
    case Level::Trace: prio = ANDROID_LOG_VERBOSE; break;
    case Level::Debug: prio = ANDROID_LOG_DEBUG; break;
    case Level::Warn: prio = ANDROID_LOG_WARN; break;
    case Level::Error: prio = ANDROID_LOG_ERROR; break;
    case Level::Fatal: prio = ANDROID_LOG_FATAL; break;
    default: break;
    }
    __android_log_write(prio, "VICTUS", format_line(record).c_str());
    return;
#endif
    std::FILE* out = record.level >= Level::Error ? stderr : stdout;
    const std::string line = format_line(record);
    if (color_) {
        const char* code = "";
        switch (record.level) {
        case Level::Trace:
        case Level::Debug: code = "\x1b[90m"; break;
        case Level::Warn: code = "\x1b[33m"; break;
        case Level::Error:
        case Level::Fatal: code = "\x1b[31m"; break;
        default: break;
        }
        if (*code) {
            std::fprintf(out, "%s%s\x1b[0m", code, line.c_str());
            return;
        }
    }
    std::fputs(line.c_str(), out);
}

void ConsoleSink::flush() {
    std::fflush(stdout);
    std::fflush(stderr);
}

FileSink::FileSink(const std::string& path) {
#if defined(_WIN32)
    fopen_s(&file_, path.c_str(), "wb");
#else
    file_ = std::fopen(path.c_str(), "wb");
#endif
}

FileSink::~FileSink() {
    if (file_) std::fclose(file_);
}

void FileSink::write(const Record& record) {
    if (!file_) return;
    const std::string line = format_line(record);
    std::fwrite(line.data(), 1, line.size(), file_);
    if (record.level >= Level::Error) std::fflush(file_);
}

void FileSink::flush() {
    if (file_) std::fflush(file_);
}

void RingSink::write(const Record& record) {
    std::lock_guard lock(mutex_);
    ++total_;
    if (capacity_ == 0) return;
    if (records_.size() < capacity_) {
        records_.push_back(record);
    } else {
        records_[head_] = record;
        head_ = (head_ + 1) % capacity_;
    }
}

std::vector<Record> RingSink::snapshot() const {
    std::lock_guard lock(mutex_);
    std::vector<Record> out;
    out.reserve(records_.size());
    for (std::size_t i = 0; i < records_.size(); ++i) out.push_back(records_[(head_ + i) % records_.size()]);
    return out;
}

std::uint64_t RingSink::total_written() const {
    std::lock_guard lock(mutex_);
    return total_;
}

void RingSink::clear() {
    std::lock_guard lock(mutex_);
    records_.clear();
    head_ = 0;
}

void Logger::add_sink(std::shared_ptr<Sink> sink) {
    std::lock_guard lock(mutex_);
    sinks_.push_back(std::move(sink));
}

void Logger::remove_sink(const std::shared_ptr<Sink>& sink) {
    std::lock_guard lock(mutex_);
    sinks_.erase(std::remove(sinks_.begin(), sinks_.end(), sink), sinks_.end());
}

void Logger::clear_sinks() {
    std::lock_guard lock(mutex_);
    sinks_.clear();
}

void Logger::write(Level level, std::string_view category, std::string message) {
    Record record{level, std::string(category), std::move(message), std::chrono::system_clock::now(),
                  std::hash<std::thread::id>{}(std::this_thread::get_id())};
    std::lock_guard lock(mutex_);
    ++counts_[static_cast<int>(level)];
    for (auto& sink : sinks_) sink->write(record);
}

void Logger::flush() {
    std::lock_guard lock(mutex_);
    for (auto& sink : sinks_) sink->flush();
}

std::uint64_t Logger::count(Level level) const noexcept {
    std::lock_guard lock(mutex_);
    return counts_[static_cast<int>(level)];
}

Logger& logger() {
    static Logger instance;
    return instance;
}

} // namespace victus::log
