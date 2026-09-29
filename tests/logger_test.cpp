// Logger 健壮性测试靶子。
//
// 覆盖点：
//   [1] init 之前调用不崩溃、静默丢弃
//   [2] 基本功能：行数不丢
//   [3] 并发多线程：不丢行（回归 shutdown 只 flush 调用线程的 bug）
//   [4] 短命线程：退出即落盘（回归析构不 flush 的 bug）
//   [5] 空闲线程：靠后端定时刷新在 ~200ms 内落盘
//   [6] FATAL 立即落盘
//   [7] 反复 init/shutdown 不崩溃、不串文件
//
// 在 Ubuntu x86 上带 TSan 跑：
//     ./scripts/test.sh
#include "common/Logger.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<int> g_failed{0};

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failed.fetch_add(1, std::memory_order_relaxed);              \
        }                                                                  \
    } while (0)

std::string readAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int countLines(const std::string& s) {
    int n = 0;
    for (char c : s) if (c == '\n') ++n;
    return n;
}

int countOccurrences(const std::string& s, const std::string& needle) {
    if (needle.empty()) return 0;
    int n = 0;
    std::size_t pos = 0;
    while ((pos = s.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

void resetFile(const std::string& path) { std::remove(path.c_str()); }

// [1]
void testBeforeInit() {
    std::printf("[1] init 之前调用\n");
    nas::Logger::shutdown(); // 确保处于关闭态
    LOG_INFO("this line is dropped (%d)", 1);
    LOG_ERROR("this too %s", "silently");
    CHECK(nas::Logger::droppedCount() == 0);
}

// [2]
void testBasicNotLost() {
    std::printf("[2] 基本：行数不丢\n");
    const std::string path = "logger_test_basic.log";
    resetFile(path);

    const int kN = 1000;
    const std::uint64_t d0 = nas::Logger::droppedCount();
    nas::Logger::init(path, nas::LogLevel::TRACE, 1u << 30, 5);
    for (int i = 0; i < kN; ++i) LOG_INFO("basic line %d", i);
    nas::Logger::flush();
    nas::Logger::shutdown();

    const std::string s = readAll(path);
    CHECK(countLines(s) == kN);
    CHECK(nas::Logger::droppedCount() == d0);
    resetFile(path);
}

// [3]
void testConcurrentNotLost() {
    std::printf("[3] 并发：不丢/不重\n");
    const std::string path = "logger_test_concurrent.log";
    resetFile(path);

    const unsigned kThreads = 8;
    const int      kPer     = 5000;
    const std::uint64_t d0  = nas::Logger::droppedCount();

    nas::Logger::init(path, nas::LogLevel::TRACE, 1u << 30, 5);

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (unsigned t = 0; t < kThreads; ++t) {
        threads.emplace_back([t, kPer] {
            for (int j = 0; j < kPer; ++j) LOG_INFO("t%02u line %06d", t, j);
        });
    }
    for (auto& th : threads) th.join();

    nas::Logger::shutdown(); // 必须 flush 所有线程

    const std::string s = readAll(path);
    CHECK(countLines(s) == static_cast<int>(kThreads) * kPer);
    CHECK(nas::Logger::droppedCount() == d0);
    resetFile(path);
}

// [4]
void testShortLivedThread() {
    std::printf("[4] 短命线程：退出即落盘\n");
    const std::string path = "logger_test_shortlived.log";
    resetFile(path);

    nas::Logger::init(path, nas::LogLevel::TRACE, 1u << 30, 5);
    std::thread([] { LOG_WARN("short-lived-marker"); }).join();
    nas::Logger::flush();
    nas::Logger::shutdown();

    CHECK(countOccurrences(readAll(path), "short-lived-marker") == 1);
    resetFile(path);
}

// [5]
void testIdleThreadTimerFlush() {
    std::printf("[5] 空闲线程：定时刷新\n");
    const std::string path = "logger_test_idle.log";
    resetFile(path);

    nas::Logger::init(path, nas::LogLevel::TRACE, 1u << 30, 5);

    std::thread t([] {
        LOG_WARN("idle-marker");
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
    });

    // 线程仍在睡眠（不会自己 flush），只等后端定时器。
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(countOccurrences(readAll(path), "idle-marker") == 1);

    t.join();
    nas::Logger::shutdown();
    CHECK(countOccurrences(readAll(path), "idle-marker") == 1);
    resetFile(path);
}

// [6]
void testFatalFlushes() {
    std::printf("[6] FATAL 立即落盘\n");
    const std::string path = "logger_test_fatal.log";
    resetFile(path);

    nas::Logger::init(path, nas::LogLevel::TRACE, 1u << 30, 5);
    LOG_FATAL("fatal-marker");
    // 不显式 flush，只等后端把 FATAL 触发的刷写落盘。
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(countOccurrences(readAll(path), "fatal-marker") == 1);
    nas::Logger::shutdown();
    resetFile(path);
}

// [7]
void testReinit() {
    std::printf("[7] 反复 init/shutdown\n");
    for (int k = 0; k < 3; ++k) {
        const std::string path =
            "logger_test_reinit_" + std::to_string(k) + ".log";
        resetFile(path);

        nas::Logger::init(path, nas::LogLevel::INFO, 1u << 30, 5);
        LOG_INFO("reinit run %d", k);
        // 故意不 flush，检验 shutdown 会兜底。
        nas::Logger::shutdown();

        const std::string s = readAll(path);
        CHECK(countOccurrences(s, "reinit run " + std::to_string(k)) == 1);
        CHECK(countLines(s) == 1);
        resetFile(path);
    }
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("== Logger 测试 ==\n");
    testBeforeInit();
    testBasicNotLost();
    testConcurrentNotLost();
    testShortLivedThread();
    testIdleThreadTimerFlush();
    testFatalFlushes();
    testReinit();

    const int failed = g_failed.load();
    if (failed == 0) {
        std::printf("== 全部通过 ==\n");
        return 0;
    }
    std::printf("== 失败: %d 项 ==\n", failed);
    return 1;
}
