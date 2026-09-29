// io_uring vs 同步读取 基准。
//
// 用法：
//   io_bench [--file PATH] [--block KiB] [--size MiB]
// 无 --file 时在 /tmp 生成一个 size MiB 的测试文件。
//
// 基于 Linux + liburing；未启用 io_uring 时打印说明并退出。

#include <cstdio>

#if defined(__linux__) && defined(NAS_HAS_IO_URING)

#include "io/AsyncFileReader.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t fileSize(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) return 0;
    return static_cast<std::uint64_t>(st.st_size);
}

double syncRead(const std::string& path, std::size_t block,
                std::uint64_t& outBytes) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return -1.0;

    std::vector<char> buf(block);
    std::uint64_t     total = 0;

    const auto t0 = Clock::now();
    for (;;) {
        const ssize_t n = ::read(fd, buf.data(), block);
        if (n <= 0) break;
        total += static_cast<std::uint64_t>(n);
    }
    const auto t1 = Clock::now();
    ::close(fd);

    outBytes = total;
    return std::chrono::duration<double>(t1 - t0).count();
}

double uringRead(const std::string& path, std::size_t block, unsigned depth,
                 std::uint64_t& outBytes) {
    nas::AsyncFileReader reader(block, depth);
    if (!reader.valid()) return -1.0;
    if (!reader.start(path)) return -1.0;

    std::uint64_t total = 0;
    const auto    t0    = Clock::now();
    for (;;) {
        nas::BufferPool::Buffer b = reader.next();
        if (!b) break;
        total += b.length();
    }
    const auto t1 = Clock::now();

    outBytes = total;
    return std::chrono::duration<double>(t1 - t0).count();
}

void makeFile(const std::string& path, std::uint64_t bytes) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    std::vector<char> buf(1 << 20);
    for (std::size_t i = 0; i < buf.size(); ++i) {
        buf[i] = static_cast<char>(i * 131 + 7);
    }
    std::uint64_t written = 0;
    while (written < bytes) {
        const std::size_t want = static_cast<std::size_t>(
            std::min<std::uint64_t>(buf.size(), bytes - written));
        const ssize_t n = ::write(fd, buf.data(), want);
        if (n <= 0) break;
        written += static_cast<std::uint64_t>(n);
    }
    ::close(fd);
}

}  // namespace

int main(int argc, char** argv) {
    std::string   path;
    std::size_t   block   = 64 * 1024;
    std::uint64_t sizeMiB = 128;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--file" && i + 1 < argc) {
            path = argv[++i];
        } else if (a == "--block" && i + 1 < argc) {
            block = static_cast<std::size_t>(
                        std::strtoull(argv[++i], nullptr, 10)) * 1024;
        } else if (a == "--size" && i + 1 < argc) {
            sizeMiB = std::strtoull(argv[++i], nullptr, 10);
        }
    }

    if (path.empty()) {
        path = "/tmp/nas_io_bench.bin";
        const std::uint64_t want = sizeMiB * 1024 * 1024;
        if (fileSize(path) != want) makeFile(path, want);
    }

    const std::uint64_t fsize = fileSize(path);
    if (fsize == 0) {
        std::printf("文件不存在或为空: %s\n", path.c_str());
        return 1;
    }

    std::printf("== io_uring 读取基准 ==\n");
    std::printf("file=%s  size=%.1f MiB  block=%zu KiB\n\n", path.c_str(),
                static_cast<double>(fsize) / 1048576.0, block / 1024);

    {
        nas::IoUring probe;
        if (!probe.valid()) {
            std::printf("io_uring 不可用：io_uring_queue_init 失败 errno=%d (%s)\n\n",
                        probe.initErrno(), std::strerror(probe.initErrno()));
        }
    }

    std::uint64_t bytes = 0;
    const double  s     = syncRead(path, block, bytes);
    if (s > 0) {
        std::printf("%-18s %8.1f MiB/s  %8.3f s\n", "sync(read)",
                    static_cast<double>(bytes) / 1048576.0 / s, s);
    }

    for (const unsigned depth : {1u, 4u, 16u, 64u}) {
        const double t = uringRead(path, block, depth, bytes);
        if (t <= 0) {
            std::printf("io_uring QD=%-3u      不可用\n", depth);
            continue;
        }
        const std::string label = "io_uring QD=" + std::to_string(depth);
        std::printf("%-18s %8.1f MiB/s  %8.3f s\n", label.c_str(),
                    static_cast<double>(bytes) / 1048576.0 / t, t);
    }
    return 0;
}

#else

int main() {
    std::printf("io_uring 未启用（未找到 liburing）。\n"
                "Ubuntu: sudo apt install liburing-dev 后重新配置构建。\n");
    return 0;
}

#endif
