#include "io/IoUring.h"

#if defined(__linux__) && defined(NAS_HAS_IO_URING)

#include <liburing.h>

namespace nas {

struct IoUring::Impl {
    struct io_uring ring;
};

IoUring::IoUring(unsigned entries) : entries_(entries) {
    impl_ = new Impl;
    const int ret = io_uring_queue_init(entries_, &impl_->ring, 0);
    if (ret != 0) {
        initErrno_ = -ret;  // io_uring_queue_init 返回 -errno
        delete impl_;
        impl_ = nullptr;
    }
}

IoUring::~IoUring() {
    if (impl_) {
        io_uring_queue_exit(&impl_->ring);
        delete impl_;
    }
}

bool IoUring::queueRead(int fd, void* buf, unsigned len, std::uint64_t offset,
                        std::uint64_t userData) {
    if (!impl_) return false;
    struct io_uring_sqe* sqe = io_uring_get_sqe(&impl_->ring);
    if (!sqe) return false;
    io_uring_prep_read(sqe, fd, buf, len, offset);
    io_uring_sqe_set_data(
        sqe, reinterpret_cast<void*>(static_cast<std::uintptr_t>(userData)));
    return true;
}

bool IoUring::queueWrite(int fd, const void* buf, unsigned len,
                         std::uint64_t offset, std::uint64_t userData) {
    if (!impl_) return false;
    struct io_uring_sqe* sqe = io_uring_get_sqe(&impl_->ring);
    if (!sqe) return false;
    io_uring_prep_write(sqe, fd, buf, len, offset);
    io_uring_sqe_set_data(
        sqe, reinterpret_cast<void*>(static_cast<std::uintptr_t>(userData)));
    return true;
}

int IoUring::submit() {
    if (!impl_) return -1;
    return io_uring_submit(&impl_->ring);
}

int IoUring::submitAndWait(unsigned waitNr) {
    if (!impl_) return -1;
    return io_uring_submit_and_wait(&impl_->ring, waitNr);
}

int IoUring::peek(Completion* out, unsigned maxEvents) {
    if (!impl_) return -1;
    unsigned n = 0;
    struct io_uring_cqe* cqe = nullptr;
    while (n < maxEvents && io_uring_peek_cqe(&impl_->ring, &cqe) == 0 &&
           cqe != nullptr) {
        out[n].userData = static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(io_uring_cqe_get_data(cqe)));
        out[n].result   = cqe->res;
        io_uring_cqe_seen(&impl_->ring, cqe);
        ++n;
    }
    return static_cast<int>(n);
}

}  // namespace nas

#endif  // __linux__ && NAS_HAS_IO_URING
