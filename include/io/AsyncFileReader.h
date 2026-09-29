#pragma once

// =====================================================================
//  io/AsyncFileReader.h - io_uring 异步顺序文件读取
//
//  一个读取线程：向 io_uring 提交多个 in-flight 读（队列深度 depth），
//  完成的数据块经 SPSC 无锁队列交给消费者线程（解决 A + D：
//  io_uring 真异步 + SpscRingBuffer + BufferPool 落地）。
//
//  仅在找到 liburing 时编译。
// =====================================================================

#if defined(__linux__) && defined(NAS_HAS_IO_URING)

#include "concurrent/SpscRingBuffer.h"
#include "io/BufferPool.h"
#include "io/IoUring.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>

namespace nas {

inline constexpr std::size_t kReaderDoneCapacity = 256;

class AsyncFileReader {
public:
    AsyncFileReader(std::size_t blockSize, unsigned depth);
    ~AsyncFileReader();

    AsyncFileReader(const AsyncFileReader&) = delete;
    AsyncFileReader& operator=(const AsyncFileReader&) = delete;

    bool valid() const noexcept { return ring_.valid(); }

    // 打开文件的 [offset, offset+length) 区间并启动读取线程。
    // length 默认 UINT64_MAX（读到文件尾）。
    bool start(const std::string& path, std::uint64_t offset = 0,
               std::uint64_t length = ~static_cast<std::uint64_t>(0));

    // 阻塞取下一个数据块；读到 EOF 返回空 Buffer（operator bool == false）。
    // 只能由单个消费者线程调用。
    BufferPool::Buffer next();

    // 本次读取的字节总数（区间长度，已按文件大小裁剪）。
    std::uint64_t size() const noexcept { return size_; }

private:
    void loop();

    std::size_t blockSize_;
    unsigned    depth_;
    BufferPool  pool_;
    IoUring     ring_;
    SpscRingBuffer<BufferPool::Buffer, kReaderDoneCapacity> done_;

    int                 fd_   = -1;
    std::uint64_t       size_ = 0;  // 本次区间长度
    std::uint64_t       begin_ = 0; // 区间起点（文件内绝对偏移）
    std::uint64_t       end_   = 0; // 区间终点（不含）
    std::atomic<bool>   running_{false};
    std::thread         thread_;
};

}  // namespace nas

#endif  // __linux__ && NAS_HAS_IO_URING
