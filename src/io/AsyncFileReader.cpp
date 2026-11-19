#include "io/AsyncFileReader.h"

#if defined(__linux__) && defined(NAS_HAS_IO_URING)

#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>

namespace {

std::uint64_t nowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::uint64_t timevalNs(const struct timeval& tv) {
    return static_cast<std::uint64_t>(tv.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(tv.tv_usec) * 1000ULL;
}

}  // namespace

namespace nas {

AsyncFileReader::AsyncFileReader(std::size_t blockSize, unsigned depth,
                                 Handoff handoff)
    : blockSize_(blockSize),
      depth_(depth),
      handoff_(handoff),
      pool_(blockSize, depth + 4),
      ring_(depth * 2 + 2) {}

AsyncFileReader::~AsyncFileReader() {
    running_.store(false, std::memory_order_release);
    cvData_.notify_all();
    cvSpace_.notify_all();
    waitFinished();
    if (fd_ >= 0) ::close(fd_);
}

void AsyncFileReader::waitFinished() {
    if (thread_.joinable()) thread_.join();
}

bool AsyncFileReader::start(const std::string& path, std::uint64_t offset,
                            std::uint64_t length) {
    if (!ring_.valid()) return false;

    fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) return false;

    struct stat st {};
    if (::fstat(fd_, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    const std::uint64_t fileSize = static_cast<std::uint64_t>(st.st_size);
    begin_ = std::min(offset, fileSize);
    const std::uint64_t avail = fileSize - begin_;
    end_                      = begin_ + std::min(length, avail);
    size_                     = end_ - begin_;

    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&AsyncFileReader::loop, this);
    return true;
}

BufferPool::Buffer AsyncFileReader::next() {
    const std::uint64_t t0 = nowNs();
    DoneItem item;
    bool got = false;

    if (handoff_ == Handoff::CondVar) {
        std::unique_lock<std::mutex> lk(handoffMutex_);
        cvData_.wait(lk, [this] {
            return done_.size() > 0 ||
                   !running_.load(std::memory_order_acquire);
        });
        got = done_.pop(item);
        if (got) {
            lk.unlock();
            cvSpace_.notify_all();  // 队列可能腾出了空位，唤醒生产者
        }
    } else {
        for (;;) {
            if (done_.pop(item)) {
                got = true;
                break;
            }
            if (!running_.load(std::memory_order_acquire) &&
                done_.size() == 0) {
                break;
            }
            std::this_thread::yield();
        }
    }

    const std::uint64_t t1 = nowNs();
    timing_.handoffNs += t1 - t0;
    if (!got) return BufferPool::Buffer{};
    if (item.stampNs != 0) timing_.queueNs += t1 - item.stampNs;
    if (item.buf) timing_.blocks++;
    return std::move(item.buf);
}

bool AsyncFileReader::pushDone(DoneItem&& item) {
    if (handoff_ == Handoff::CondVar) {
        std::unique_lock<std::mutex> lk(handoffMutex_);
        cvSpace_.wait(lk, [this] {
            return done_.size() < kReaderDoneCapacity ||
                   !running_.load(std::memory_order_acquire);
        });
        if (!done_.push(std::move(item))) return false;
        lk.unlock();
        cvData_.notify_one();
        return true;
    }

    while (!done_.push(std::move(item)) &&
           running_.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    return true;
}

void AsyncFileReader::loop() {
    struct rusage ru0 {};
    struct rusage ru1 {};
    ::getrusage(RUSAGE_THREAD, &ru0);
    ::prctl(PR_SET_NAME, "nas-reader", 0, 0, 0);

    struct Inflight {
        BufferPool::Buffer buf;
        std::size_t        requested = 0;
    };

    const std::uint64_t total         = end_;
    std::uint64_t       nextOffset    = begin_;
    std::uint64_t       nextPushOffset = begin_;
    std::unordered_map<std::uint64_t, Inflight> inflight;
    // 完成事件可能乱序；先按 offset 收进 completed，再严格按 offset
    // 推给消费者，保证下载内容顺序。顺序读 + QD>1 时这一点必须成立。
    std::map<std::uint64_t, Inflight> completed;

    auto submitMore = [&]() {
        while (inflight.size() < depth_ && nextOffset < total) {
            std::optional<BufferPool::Buffer> buf = pool_.acquire();
            if (!buf) break;  // 缓冲耗尽：等消费者归还
            const std::size_t len = static_cast<std::size_t>(
                std::min<std::uint64_t>(blockSize_, total - nextOffset));
            if (!ring_.queueRead(fd_, buf->data(),
                                 static_cast<unsigned>(len), nextOffset,
                                 nextOffset)) {
                break;  // SQ 满
            }
            inflight.emplace(nextOffset, Inflight{std::move(*buf), len});
            nextOffset += len;
        }
    };

    while (running_.load(std::memory_order_acquire)) {
        submitMore();

        if (!inflight.empty()) {
            const std::uint64_t submitStart = nowNs();
            const int submitRc = ring_.submitAndWait(1);
            timing_.submitNs += nowNs() - submitStart;
            if (submitRc < 0) break;
            IoUring::Completion c;
            int n = 0;
            while ((n = ring_.peek(&c, 1)) > 0) {
                auto it = inflight.find(c.userData);
                if (it == inflight.end()) continue;
                Inflight item = std::move(it->second);
                inflight.erase(it);
                item.buf.setLength(c.result > 0
                                       ? static_cast<std::size_t>(c.result)
                                       : 0);
                if (c.userData < nextPushOffset) continue;  // 重复/过期
                completed.emplace(c.userData, std::move(item));

                for (;;) {
                    auto next = completed.find(nextPushOffset);
                    if (next == completed.end()) break;
                    Inflight& slot = next->second;
                    // 按“提交时请求的长度”推进，保证与 nextOffset 的
                    // 偏移序列一致；短读/错误时也不会卡在缺失的 offset。
                    const std::size_t advance = slot.requested;
                    pushDone(DoneItem{std::move(slot.buf), nowNs()});
                    nextPushOffset += advance;
                    completed.erase(next);
                }
            }
        } else if (nextOffset >= total) {
            break;
        } else {
            // 缓冲池耗尽：等消费者归还 buffer。
            if (handoff_ == Handoff::CondVar) {
                std::unique_lock<std::mutex> lk(handoffMutex_);
                cvSpace_.wait_for(lk, std::chrono::milliseconds(1));
            } else {
                std::this_thread::yield();
            }
        }
    }

    // EOF 哨兵：一个无效 Buffer，让消费者确定读到结尾。
    if (running_.load(std::memory_order_acquire)) {
        pushDone(DoneItem{});
    }
    running_.store(false, std::memory_order_release);
    cvData_.notify_all();
    cvSpace_.notify_all();

    ::getrusage(RUSAGE_THREAD, &ru1);
    timing_.readerCpuNs =
        (timevalNs(ru1.ru_utime) - timevalNs(ru0.ru_utime)) +
        (timevalNs(ru1.ru_stime) - timevalNs(ru0.ru_stime));
}

}  // namespace nas

#endif  // __linux__ && NAS_HAS_IO_URING
