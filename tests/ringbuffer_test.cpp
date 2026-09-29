// SpscRingBuffer 正确性测试靶子。
//
// 这个文件只依赖 SpscRingBuffer.h 暴露的公共接口（push/pop/size），
// 不关心你内部怎么实现。你要做的就是让这些测试全绿。
//
// 在 Ubuntu x86 上跑（带 ThreadSanitizer）：
//     cmake -S . -B build-host && cmake --build build-host -j
//     ctest --test-dir build-host --output-on-failure
// 或直接： ./scripts/test.sh

#include "concurrent/SpscRingBuffer.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace {

std::atomic<int> g_failed{0};

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failed.fetch_add(1, std::memory_order_relaxed);              \
        }                                                                  \
    } while (0)

constexpr std::size_t kCap = 1024;

// 1) 单线程 FIFO 基本次序
void testFifoOrder() {
    std::printf("[1] FIFO 次序\n");
    nas::SpscRingBuffer<std::uint64_t, kCap> q;

    std::uint64_t pushed = 0;
    while (pushed < kCap && q.push(pushed)) {
        ++pushed;
    }
    CHECK(pushed >= 1);     // 至少能存一个元素
    CHECK(pushed <= kCap);  // 不能超过容量

    std::uint64_t expected = 0;
    std::uint64_t value = 0;
    std::uint64_t guard = 0;
    while (guard++ <= kCap + 1 && q.pop(value)) {
        CHECK(value == expected);
        ++expected;
    }
    CHECK(expected == pushed);
}

// 2) 满 / 空边界（反复 push 失败与 pop 失败都不得改变状态）
void testCapacityBoundary() {
    std::printf("[2] 满/空边界\n");
    nas::SpscRingBuffer<std::uint64_t, kCap> q;
    CHECK(q.size() == 0);

    std::uint64_t value = 0;
    CHECK(q.pop(value) == false);  // 空队列 pop 失败

    std::uint64_t count = 0;
    while (q.push(count)) {
        ++count;
    }
    CHECK(count >= 1 && count <= kCap);
    CHECK(q.size() == count);

    // 再 push 一次仍应失败，且大小不变
    CHECK(q.push(count) == false);
    CHECK(q.size() == count);

    std::uint64_t expected = 0;
    std::uint64_t guard = 0;
    while (guard++ <= kCap + 1 && q.pop(value)) {
        CHECK(value == expected);
        ++expected;
    }
    CHECK(expected == count);
    CHECK(q.size() == 0);
}

// 3) 环绕：用极小容量制造大量索引回绕，检验位掩码/取模是否正确
void testWrapAround() {
    std::printf("[3] 环绕\n");
    nas::SpscRingBuffer<std::uint64_t, 8> q;

    std::uint64_t push_seq = 0;
    std::uint64_t pop_seq = 0;
    for (int i = 0; i < 10000; ++i) {
        CHECK(q.push(push_seq));
        ++push_seq;

        std::uint64_t value = 0;
        CHECK(q.pop(value));
        CHECK(value == pop_seq);
        ++pop_seq;
    }
}

// 4) 并发：不丢、不重、不乱序
void testConcurrentOrder() {
    std::printf("[4] 并发 不丢/不重/不乱序\n");
    constexpr std::uint64_t kItems = 2000000;
    nas::SpscRingBuffer<std::uint64_t, 4096> q;

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kItems;) {
            if (q.push(i)) {
                ++i;
            }
        }
    });

    std::thread consumer([&] {
        std::uint64_t expected = 0;
        std::uint64_t value = 0;
        std::uint64_t spins = 0;
        while (expected < kItems) {
            if (q.pop(value)) {
                CHECK(value == expected);
                ++expected;
                spins = 0;
            } else if (++spins > (1ull << 36)) {
                CHECK(false && "消费者空转过久：可能漏元素或死锁");
                return;
            }
        }
    });

    producer.join();
    consumer.join();
}

// 5) 并发：撕裂检测。两个字段必须始终成对，若缺乏正确的内存序约束，
//    TSan 或这里的断言会把它抓出来。
void testConcurrentNoTearing() {
    std::printf("[5] 并发 撕裂检测\n");
    struct Pair {
        std::uint64_t seq;
        std::uint64_t inv;
    };

    constexpr std::uint64_t kItems = 1000000;
    nas::SpscRingBuffer<Pair, 4096> q;

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kItems;) {
            if (q.push(Pair{i, ~i})) {
                ++i;
            }
        }
    });

    std::thread consumer([&] {
        std::uint64_t expected = 0;
        Pair value{};
        std::uint64_t spins = 0;
        while (expected < kItems) {
            if (q.pop(value)) {
                CHECK(value.seq == expected);
                CHECK(value.inv == ~value.seq);
                ++expected;
                spins = 0;
            } else if (++spins > (1ull << 36)) {
                CHECK(false && "消费者空转过久：可能漏元素或死锁");
                return;
            }
        }
    });

    producer.join();
    consumer.join();
}

}  // namespace

int main() {
    std::printf("== SpscRingBuffer 测试 ==\n");
    testFifoOrder();
    testCapacityBoundary();
    testWrapAround();
    testConcurrentOrder();
    testConcurrentNoTearing();

    const int failed = g_failed.load();
    if (failed == 0) {
        std::printf("== 全部通过 ==\n");
        return 0;
    }
    std::printf("== 失败: %d 项 ==\n", failed);
    return 1;
}
