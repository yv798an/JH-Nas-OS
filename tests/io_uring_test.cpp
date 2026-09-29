// AsyncFileReader (io_uring) 端到端测试：写一个已知内容的文件，异步读回校验。
// io_uring 不可用时（内核/权限限制）自动跳过。

#include <cstdio>

#if defined(__linux__) && defined(NAS_HAS_IO_URING)

#include "io/AsyncFileReader.h"

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <string>

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
    std::printf("== AsyncFileReader 测试 ==\n");

    const std::size_t kSize = 300 * 1024 + 123;  // 非 block 整数倍
    std::string       data(kSize, '\0');
    for (std::size_t i = 0; i < kSize; ++i) {
        data[i] = static_cast<char>((i * 7 + 3) & 0xFF);
    }

    const char* path = "/tmp/nas_uring_test.bin";
    const int   fd   = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        std::printf("  open 失败，跳过\n");
        return 0;
    }
    const ssize_t w = ::write(fd, data.data(), kSize);
    ::close(fd);
    if (w != static_cast<ssize_t>(kSize)) {
        std::printf("  write 失败，跳过\n");
        return 0;
    }

    nas::AsyncFileReader reader(64 * 1024, 8);
    if (!reader.valid()) {
        nas::IoUring probe;
        std::printf("  io_uring 不可用 (errno=%d)，跳过\n", probe.initErrno());
        return 0;
    }
    if (!reader.start(path)) {
        std::printf("  start 失败，跳过\n");
        return 0;
    }

    std::string got;
    got.reserve(kSize);
    for (;;) {
        nas::BufferPool::Buffer b = reader.next();
        if (!b) break;  // EOF 哨兵
        got.append(b.data(), b.length());
    }

    CHECK(got.size() == kSize);
    CHECK(got == data);

    ::unlink(path);

    const int failed = g_failed.load();
    if (failed == 0) {
        std::printf("== 全部通过 ==\n");
        return 0;
    }
    std::printf("== 失败: %d 项 ==\n", failed);
    return 1;
}

#else

int main() {
    std::printf("io_uring 未启用，跳过 AsyncFileReader 测试。\n");
    return 0;
}

#endif
