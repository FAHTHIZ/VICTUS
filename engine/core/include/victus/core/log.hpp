// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace victus::log {

enum class Level : std::uint8_t { Trace, Debug, Info, Warn, Error, Fatal };
[[nodiscard]] const char* to_string(Level level) noexcept;
[[nodiscard]] bool parse_level(std::string_view text, Level& out) noexcept;

struct Record {
    Level level{Level::Info};
    std::string category;
    std::string message;
    std::chrono::system_clock::time_point time{};
    std::uint64_t thread_hash{0};
};

class Sink {
public:
    virtual ~Sink() = default;
    virtual void write(const Record& record) = 0;
    virtual void flush() {}
};

// Writes human-readable lines to stderr (errors) / stdout (others).
class ConsoleSink final : public Sink {
public:
    explicit ConsoleSink(bool color = true) : color_(color) {}
    void write(const Record& record) override;
    void flush() override;

private:
    bool color_;
};

// Appends to a log file; truncated on open.
class FileSink final : public Sink {
public:
    explicit FileSink(const std::string& path);
    ~FileSink() override;
    FileSink(const FileSink&) = delete;
    FileSink& operator=(const FileSink&) = delete;
    [[nodiscard]] bool is_open() const noexcept { return file_ != nullptr; }
    void write(const Record& record) override;
    void flush() override;

private:
    std::FILE* file_{nullptr};
};

// Bounded in-memory history used by the editor console and diagnostics dumps.
class RingSink final : public Sink {
public:
    explicit RingSink(std::size_t capacity = 2048) : capacity_(capacity) {}
    void write(const Record& record) override;
    [[nodiscard]] std::vector<Record> snapshot() const;
    [[nodiscard]] std::uint64_t total_written() const;
    void clear();

private:
    mutable std::mutex mutex_;
    std::vector<Record> records_;
    std::size_t capacity_;
    std::size_t head_{0};
    std::uint64_t total_{0};
};

class Logger {
public:
    void add_sink(std::shared_ptr<Sink> sink);
    void remove_sink(const std::shared_ptr<Sink>& sink);
    void clear_sinks();
    void set_level(Level level) noexcept { level_ = level; }
    [[nodiscard]] Level level() const noexcept { return level_; }
    [[nodiscard]] bool enabled(Level level) const noexcept { return level >= level_; }
    void write(Level level, std::string_view category, std::string message);
    void flush();
    [[nodiscard]] std::uint64_t count(Level level) const noexcept;

private:
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<Sink>> sinks_;
    Level level_{Level::Info};
    std::uint64_t counts_[6]{};
};

// Process logger. Engine code writes through this; applications configure sinks.
Logger& logger();

template <typename... Args>
void write(Level level, std::string_view category, std::format_string<Args...> fmt, Args&&... args) {
    Logger& l = logger();
    if (!l.enabled(level)) return;
    l.write(level, category, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace victus::log

#define VICTUS_LOG_TRACE(cat, ...) ::victus::log::write(::victus::log::Level::Trace, cat, __VA_ARGS__)
#define VICTUS_LOG_DEBUG(cat, ...) ::victus::log::write(::victus::log::Level::Debug, cat, __VA_ARGS__)
#define VICTUS_LOG_INFO(cat, ...) ::victus::log::write(::victus::log::Level::Info, cat, __VA_ARGS__)
#define VICTUS_LOG_WARN(cat, ...) ::victus::log::write(::victus::log::Level::Warn, cat, __VA_ARGS__)
#define VICTUS_LOG_ERROR(cat, ...) ::victus::log::write(::victus::log::Level::Error, cat, __VA_ARGS__)
