#pragma once

// =====================================================================
//  io/IoUring.h - liburing 的 RAII 封装（仅在找到 liburing 时编译）
//
//  只暴露最小能力：入队读/写、提交、收割完成事件。user_data 用于把
//  完成事件与提交方关联。
// =====================================================================

#if defined(__linux__) && defined(NAS_HAS_IO_URING)

#include <cstdint>

namespace nas {

class IoUring {
public:
    explicit IoUring(unsigned entries = 256);
    ~IoUring();

    IoUring(const IoUring&) = delete;
    IoUring& operator=(const IoUring&) = delete;

    bool valid() const noexcept { return impl_ != nullptr; }

    // 初始化失败时的 errno（0 表示成功）。ENOSYS 通常意味着内核未开 CONFIG_IO_URING。
    int initErrno() const noexcept { return initErrno_; }

    // 入队（尚未提交），需再调用 submit()/submitAndWait()。
    bool queueRead(int fd, void* buf, unsigned len, std::uint64_t offset,
                   std::uint64_t userData);
    bool queueWrite(int fd, const void* buf, unsigned len, std::uint64_t offset,
                    std::uint64_t userData);

    // 提交所有已入队 SQE；返回提交数（<0 出错）。
    int submit();

    // 提交并等待至少 waitNr 个完成；返回已提交数（<0 出错）。
    int submitAndWait(unsigned waitNr);

    struct Completion {
        std::uint64_t userData = 0;
        int           result   = 0;  // 读/写字节数，或负 errno
    };

    // 收割最多 maxEvents 个已完成事件（非阻塞）；返回收割数量（<0 出错）。
    int peek(Completion* out, unsigned maxEvents);

    unsigned entries() const noexcept { return entries_; }

private:
    unsigned entries_;
    int      initErrno_ = 0;
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace nas

#endif  // __linux__ && NAS_HAS_IO_URING
