#include "io/BufferPool.h"

#include <utility>

namespace nas {

BufferPool::BufferPool(std::size_t bufferSize, std::size_t count)
    : bufferSize_(bufferSize), count_(count), storage_(bufferSize * count) {
    free_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        free_.push_back(storage_.data() + i * bufferSize);
    }
}

std::optional<BufferPool::Buffer> BufferPool::acquire() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (free_.empty()) return std::nullopt;
    char* p = free_.back();
    free_.pop_back();
    return Buffer(this, p, bufferSize_);
}

void BufferPool::release(char* p) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    free_.push_back(p);
}

std::size_t BufferPool::available() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return free_.size();
}

BufferPool::Buffer::Buffer(BufferPool* pool, char* data, std::size_t size) noexcept
    : pool_(pool), data_(data), size_(size) {}

BufferPool::Buffer::~Buffer() { reset(); }

BufferPool::Buffer::Buffer(Buffer&& other) noexcept
    : pool_(other.pool_), data_(other.data_), size_(other.size_),
      length_(other.length_) {
    other.pool_   = nullptr;
    other.data_   = nullptr;
    other.size_   = 0;
    other.length_ = 0;
}

BufferPool::Buffer& BufferPool::Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        reset();
        pool_   = other.pool_;
        data_   = other.data_;
        size_   = other.size_;
        length_ = other.length_;
        other.pool_   = nullptr;
        other.data_   = nullptr;
        other.size_   = 0;
        other.length_ = 0;
    }
    return *this;
}

void BufferPool::Buffer::reset() noexcept {
    if (pool_ && data_) pool_->release(data_);
    pool_   = nullptr;
    data_   = nullptr;
    size_   = 0;
    length_ = 0;
}

}  // namespace nas
