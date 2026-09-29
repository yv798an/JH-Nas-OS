#include "io/AsyncFileReader.h"

#if defined(__linux__) && defined(NAS_HAS_IO_URING)

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <utility>

namespace nas {

AsyncFileReader::AsyncFileReader(std::size_t blockSize, unsigned depth)
    : blockSize_(blockSize),
      depth_(depth),
      pool_(blockSize, depth + 4),
      ring_(depth * 2 + 2) {}

AsyncFileReader::~AsyncFileReader() {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) ::close(fd_);
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
    BufferPool::Buffer buf;
    for (;;) {
        if (done_.pop(buf)) return buf;
        if (!running_.load(std::memory_order_acquire) && done_.size() == 0) {
            return BufferPool::Buffer{};
        }
        std::this_thread::yield();
    }
}

void AsyncFileReader::loop() {
    const std::uint64_t total = end_;
    std::uint64_t       nextOffset = begin_;
    std::unordered_map<std::uint64_t, BufferPool::Buffer> inflight;

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
            inflight.emplace(nextOffset, std::move(*buf));
            nextOffset += len;
        }
    };

    while (running_.load(std::memory_order_acquire)) {
        submitMore();

        if (!inflight.empty()) {
            if (ring_.submitAndWait(1) < 0) break;
            IoUring::Completion c;
            int n = 0;
            while ((n = ring_.peek(&c, 1)) > 0) {
                auto it = inflight.find(c.userData);
                if (it == inflight.end()) continue;
                BufferPool::Buffer b = std::move(it->second);
                inflight.erase(it);
                b.setLength(c.result > 0 ? static_cast<std::size_t>(c.result)
                                         : 0);
                while (!done_.push(std::move(b)) &&
                       running_.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
            }
        } else if (nextOffset >= total) {
            break;
        } else {
            std::this_thread::yield();  // 等消费者归还 buffer
        }
    }

    // EOF 哨兵：一个无效 Buffer，让消费者确定读到结尾。
    while (!done_.push(BufferPool::Buffer{}) &&
           running_.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

}  // namespace nas

#endif  // __linux__ && NAS_HAS_IO_URING
