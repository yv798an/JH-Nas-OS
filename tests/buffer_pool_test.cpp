// BufferPool 测试：容量、RAII 归还、移动语义、池空。

#include "io/BufferPool.h"

#include <atomic>
#include <cstdio>
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

}  // namespace

int main() {
    std::printf("== BufferPool 测试 ==\n");

    nas::BufferPool pool(64, 4);
    CHECK(pool.bufferSize() == 64);
    CHECK(pool.capacity() == 4);
    CHECK(pool.available() == 4);

    std::vector<nas::BufferPool::Buffer> held;
    for (int i = 0; i < 4; ++i) {
        auto b = pool.acquire();
        CHECK(b.has_value());
        if (!b) continue;
        CHECK(b->size() == 64);
        CHECK(b->data() != nullptr);
        b->setLength(10);
        b->data()[0] = static_cast<char>('A' + i);
        held.push_back(std::move(*b));
    }
    CHECK(held.size() == 4);
    CHECK(pool.available() == 0);
    CHECK(!pool.acquire().has_value());  // 池空

    CHECK(held[0].data() != held[1].data());  // 不同缓冲
    CHECK(held[0].length() == 10);

    // 移动：源失效，目标保有缓冲
    nas::BufferPool::Buffer moved = std::move(held[0]);
    CHECK(!held[0]);
    CHECK(moved);
    CHECK(moved.length() == 10);

    // 析构 3 个仍有效的（held[0] 已 moved-from，不归还）
    held.clear();
    CHECK(pool.available() == 3);

    moved.reset();
    CHECK(pool.available() == 4);

    const int failed = g_failed.load();
    if (failed == 0) {
        std::printf("== 全部通过 ==\n");
        return 0;
    }
    std::printf("== 失败: %d 项 ==\n", failed);
    return 1;
}
