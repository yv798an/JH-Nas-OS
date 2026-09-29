// io_uring vs 同步读取 基准。
//
// 用法：
//   io_bench [--file PATH] [--block KiB] [--size MiB]
//            [--repeat N] [--drop-cache]
// 无 --file 时在 /tmp 生成一个 size MiB 的测试文件。
//
// --repeat N      每种方案重复 N 次，报告 median/min/max（默认 1）
// --drop-cache    每次计时前 drop page cache（需 root；用于测冷盘）
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
#include <functional>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t fileSize(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) return 0;
    return static_cast<std::uint64_t>(st.st_size);
}

// drop 三档 page cache（需 root）。用于让每次计时都从冷盘开始。
bool dropCaches() {
    ::sync();
    const int fd = ::open("/proc/sys/vm/drop_caches", O_WRONLY);
    if (fd < 0) return false;
    const ssize_t n = ::write(fd, "3\n", 2);
    ::close(fd);
    return n == 2;
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

using ReadFn = std::function<double(std::uint64_t&)>;

// 重复跑同一方案；可每次前 drop cache。返回每次的吞吐 MiB/s。
std::vector<double> benchRepeat(bool drop, int repeat, const ReadFn& run,
                                std::uint64_t& outBytes) {
    std::vector<double> tput;
    tput.reserve(static_cast<std::size_t>(repeat));
    for (int i = 0; i < repeat; ++i) {
        if (drop) dropCaches();
        std::uint64_t b = 0;
        const double  t = run(b);
        if (t <= 0) {
            tput.push_back(-1.0);
            continue;
        }
        outBytes = b;
        tput.push_back(static_cast<double>(b) / 1048576.0 / t);
    }
    return tput;
}

void printStats(const std::string& label, std::vector<double> v) {
    if (v.empty() ||
        std::any_of(v.begin(), v.end(), [](double x) { return x < 0; })) {
        std::printf("%-18s 不可用\n", label.c_str());
        return;
    }
    std::sort(v.begin(), v.end());
    std::printf("%-18s median %8.1f  min %8.1f  max %8.1f  MiB/s (n=%zu)\n",
                label.c_str(), v[v.size() / 2], v.front(), v.back(), v.size());
}

}  // namespace

int main(int argc, char** argv) {
    std::string   path;
    std::size_t   block     = 64 * 1024;
    std::uint64_t sizeMiB   = 128;
    int           repeat    = 1;
    bool          dropCache = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--file" && i + 1 < argc) {
            path = argv[++i];
        } else if (a == "--block" && i + 1 < argc) {
            block = static_cast<std::size_t>(
                        std::strtoull(argv[++i], nullptr, 10)) * 1024;
        } else if (a == "--size" && i + 1 < argc) {
            sizeMiB = std::strtoull(argv[++i], nullptr, 10);
        } else if (a == "--repeat" && i + 1 < argc) {
            repeat = std::atoi(argv[++i]);
        } else if (a == "--drop-cache") {
            dropCache = true;
        }
    }
    if (repeat < 1) repeat = 1;

    if (path.empty()) path = "/tmp/nas_io_bench.bin";
    // 文件不存在或为空时按 --size 生成；已存在的非空文件原样使用（避免误截断真实文件）。
    if (fileSize(path) == 0) {
        makeFile(path, sizeMiB * 1024 * 1024);
        if (fileSize(path) == 0) {
            std::printf("无法创建测试文件（目录不存在或只读？）: %s\n",
                        path.c_str());
            return 1;
        }
    }

    const std::uint64_t fsize = fileSize(path);
    if (fsize == 0) {
        std::printf("文件不存在或为空: %s\n", path.c_str());
        return 1;
    }

    std::printf("== io_uring 读取基准 ==\n");
    std::printf(
        "file=%s  size=%.1f MiB  block=%zu KiB  repeat=%d  drop_cache=%s\n\n",
        path.c_str(), static_cast<double>(fsize) / 1048576.0, block / 1024,
        repeat, dropCache ? "on" : "off");

    {
        nas::IoUring probe;
        if (!probe.valid()) {
            std::printf("io_uring 不可用：io_uring_queue_init 失败 errno=%d (%s)\n\n",
                        probe.initErrno(), std::strerror(probe.initErrno()));
        }
    }

    std::uint64_t bytes = 0;

    {
        const ReadFn run = [&](std::uint64_t& b) {
            return syncRead(path, block, b);
        };
        printStats("sync(read)", benchRepeat(dropCache, repeat, run, bytes));
    }

    for (const unsigned depth : {1u, 4u, 16u, 64u}) {
        const ReadFn run = [&](std::uint64_t& b) {
            return uringRead(path, block, depth, b);
        };
        printStats("io_uring QD=" + std::to_string(depth),
                   benchRepeat(dropCache, repeat, run, bytes));
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
