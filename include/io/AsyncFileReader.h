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
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace nas {

inline constexpr std::size_t kReaderDoneCapacity = 256;

// 队列交接方式：
//   Yield   - 历史实现，空/满时 std::this_thread::yield() 忙等（基线）；
//   CondVar - 互斥量 + 条件变量阻塞等待，避免消费者在空队列上自旋。
enum class Handoff { Yield, CondVar };

// 用户态时间分解（板端无 PMU 时使用）：
//   submitNs   - 读取线程在 submitAndWait() 中的等待（提交 + I/O 等待）
//   handoffNs  - 消费者在 next() 中等待/取数据的绝对时间
//   queueNs    - 数据块从入队到被取出的驻留时间（handoffNs 的子集）
//   readerCpuNs- 读取线程自身的 CPU 时间（RUSAGE_THREAD）
struct TimingStats {
    std::uint64_t submitNs = 0;
    std::uint64_t handoffNs = 0;
    std::uint64_t queueNs = 0;
    std::uint64_t readerCpuNs = 0;
    std::uint64_t blocks = 0;
};

class AsyncFileReader {
public:
    AsyncFileReader(std::size_t blockSize, unsigned depth,
                    Handoff handoff = Handoff::Yield);
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

    // 等待读取线程结束（消费到 EOF 后调用，确保 timing() 已完整）。
    void waitFinished();

    const TimingStats& timing() const noexcept { return timing_; }

private:
    struct DoneItem {
        BufferPool::Buffer buf;
        std::uint64_t      stampNs = 0;

        DoneItem() = default;
        DoneItem(BufferPool::Buffer&& b, std::uint64_t stamp) noexcept
            : buf(std::move(b)), stampNs(stamp) {}
    };

    void loop();
    bool pushDone(DoneItem&& item);

    std::size_t blockSize_;
    unsigned    depth_;
    Handoff     handoff_;
    BufferPool  pool_;
    IoUring     ring_;
    SpscRingBuffer<DoneItem, kReaderDoneCapacity> done_;
    TimingStats timing_;

    std::mutex              handoffMutex_;
    std::condition_variable cvData_;
    std::condition_variable cvSpace_;

    int                 fd_   = -1;
    std::uint64_t       size_ = 0;  // 本次区间长度
    std::uint64_t       begin_ = 0; // 区间起点（文件内绝对偏移）
    std::uint64_t       end_   = 0; // 区间终点（不含）
    std::atomic<bool>   running_{false};
    std::thread         thread_;
};

}  // namespace nas

#endif  // __linux__ && NAS_HAS_IO_URING
