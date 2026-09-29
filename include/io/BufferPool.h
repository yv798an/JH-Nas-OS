#pragma once

// =====================================================================
//  io/BufferPool.h - 固定大小缓冲池
//
//  预分配 count 个 bufferSize 的缓冲，acquire() 取出 RAII 句柄，句柄析构
//  自动归还。用于 io_uring 读/写，避免每请求 malloc。
// =====================================================================

#include <cstddef>
#include <mutex>
#include <optional>
#include <vector>

namespace nas {

class BufferPool {
public:
    class Buffer {
    public:
        Buffer() = default;
        ~Buffer();

        Buffer(Buffer&& other) noexcept;
        Buffer& operator=(Buffer&& other) noexcept;
        Buffer(const Buffer&) = delete;
        Buffer& operator=(const Buffer&) = delete;

        char*       data() noexcept { return data_; }
        const char* data() const noexcept { return data_; }
        std::size_t size() const noexcept { return size_; }      // 容量
        std::size_t length() const noexcept { return length_; }  // 有效字节
        void        setLength(std::size_t n) noexcept { length_ = n; }

        explicit operator bool() const noexcept { return data_ != nullptr; }
        void reset() noexcept;

    private:
        friend class BufferPool;
        Buffer(BufferPool* pool, char* data, std::size_t size) noexcept;

        BufferPool* pool_   = nullptr;
        char*       data_   = nullptr;
        std::size_t size_   = 0;
        std::size_t length_ = 0;
    };

    BufferPool(std::size_t bufferSize, std::size_t count);

    // 取一个空闲缓冲；池空返回 nullopt。
    std::optional<Buffer> acquire();

    std::size_t available() const;
    std::size_t bufferSize() const noexcept { return bufferSize_; }
    std::size_t capacity() const noexcept { return count_; }

private:
    void release(char* p) noexcept;

    std::size_t              bufferSize_;
    std::size_t              count_;
    std::vector<char>        storage_;
    std::vector<char*>       free_;
    mutable std::mutex       mutex_;
};

}  // namespace nas
