#pragma once

// =====================================================================
//  nas::Logger - asynchronous, thread-safe logger (C++17)
//
//  Front-end (per logging thread)
//  ------------------------------
//   * LOG_* macros format into a per-thread 64 KiB buffer and return;
//     the common path does not block on the backend.
//   * Double buffering: each thread keeps an "active" buffer being
//     written and a "standby" one, so publishing a full buffer is an
//     O(1) pointer swap.
//
//  Back-end (single flusher thread)
//  --------------------------------
//   * Drains the queue in batches, writes to disk, rotates at N MiB.
//   * Wakes at least every kAutoFlushMs (200 ms) and flushes every
//     thread's active buffer, so an idle thread's last line still
//     reaches disk promptly.
//
//  Robustness guarantees
//  ---------------------
//   * No log line is lost on thread exit: each thread registers a
//     pthread_key destructor that flushes its tail and recycles its
//     buffers.
//   * No log line is lost on shutdown()/init(): all live thread
//     contexts are flushed before the backend is stopped.
//   * Every ThreadContext registers itself in a global registry, so the
//     backend (timer) and shutdown() can safely flush them under a
//     per-context mutex.
//   * The buffer pool is capped; under pathological back-pressure new
//     lines are dropped and counted (Logger::droppedCount()) instead of
//     growing memory without bound.
//
//  This logger is intentionally NOT lock-free: correctness and
//  testability come first. The queue seam can later be replaced by
//  per-thread SPSC ring buffers (see include/concurrent/SpscRingBuffer.h)
//  without touching callers.
// =====================================================================

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace nas {

enum class LogLevel : int {
    TRACE = 0,
    DEBUG = 1,
    INFO  = 2,
    WARN  = 3,
    ERROR = 4,
    FATAL = 5,
    OFF   = 6
};

namespace detail {

// Fixed-size, pre-allocated log buffer. Never grows, never reallocates.
struct Buffer {
    static constexpr std::size_t kCapacity = 64 * 1024;
    char        data[kCapacity];
    std::size_t len = 0;

    void        reset() noexcept { len = 0; }
    std::size_t size() const noexcept { return len; }
    std::size_t available() const noexcept { return kCapacity - len; }
};

} // namespace detail

class Logger {
public:
    // Configure and (re)start the logger backend. Safe to call repeatedly;
    // a previous run is flushed and stopped first.
    static void init(const std::string& filepath,
                     LogLevel level = LogLevel::INFO,
                     std::size_t maxFileSizeBytes = 10 * 1024 * 1024,
                     int maxFiles = 5);

    // Flush every live thread's buffer, then stop the backend gracefully.
    static void shutdown();

    // Push the calling thread's pending buffer to the backend.
    static void flush();

    static void setLevel(LogLevel l) noexcept {
        s_level.store(l, std::memory_order_relaxed);
    }
    static LogLevel level() noexcept {
        return s_level.load(std::memory_order_relaxed);
    }

    // Cheap guard used by the macros to skip the whole call when filtered.
    static bool shouldLog(LogLevel l) noexcept {
        return static_cast<int>(l) >=
               static_cast<int>(s_level.load(std::memory_order_relaxed));
    }

    // Number of log lines dropped because the buffer pool was exhausted.
    static std::uint64_t droppedCount() noexcept;

    // Backend entry point (normally invoked through the LOG_* macros).
    static void log(LogLevel l, const char* file, int line, const char* func,
                    const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
        __attribute__((format(printf, 5, 6)))
#endif
        ;

    static const char* baseName(const char* path) noexcept;

private:
    static std::atomic<LogLevel> s_level;
};

} // namespace nas

// ---------------------------------------------------------------------
//  Front-end macros: non-blocking, level-filtered.
// ---------------------------------------------------------------------
#define NAS_LOG_IMPL(level_, ...)                                                 \
    do {                                                                          \
        if (::nas::Logger::shouldLog(level_)) {                                   \
            ::nas::Logger::log(level_, __FILE__, __LINE__, __func__, __VA_ARGS__); \
        }                                                                         \
    } while (0)

#define LOG_TRACE(...) NAS_LOG_IMPL(::nas::LogLevel::TRACE, __VA_ARGS__)
#define LOG_DEBUG(...) NAS_LOG_IMPL(::nas::LogLevel::DEBUG, __VA_ARGS__)
#define LOG_INFO(...)  NAS_LOG_IMPL(::nas::LogLevel::INFO,  __VA_ARGS__)
#define LOG_WARN(...)  NAS_LOG_IMPL(::nas::LogLevel::WARN,  __VA_ARGS__)
#define LOG_ERROR(...) NAS_LOG_IMPL(::nas::LogLevel::ERROR, __VA_ARGS__)
#define LOG_FATAL(...) NAS_LOG_IMPL(::nas::LogLevel::FATAL, __VA_ARGS__)
