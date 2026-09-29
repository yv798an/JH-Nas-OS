#pragma once

#include <atomic>
#include <cstddef>
#include <utility>

namespace nas {

// 单生产者 / 单消费者 无锁环形队列（SPSC）。
//
// 线程模型：
//   * 只能有一个线程调用 push（生产者），且只能有另一个线程调用 pop（消费者）；
//   * push/pop 各自线程安全，但不得由多个生产者或多个消费者并发调用。
//
// 容量约束：N 必须是 2 的幂（这样索引回绕可用位与代替取模）。
//
// 语义：
//   * push：队列满时返回 false 且不改变状态；成功写入返回 true。
//   * pop ：队列空时返回 false 且不改变状态；成功则取出一个元素到 out 并返回 true。
//   * FIFO：pop 的次序必须与 push 完全一致，不丢、不重、不乱序。
//   * 并发安全：消费者绝不能看到“写了一半”的元素，也不能漏掉已成功 push 的元素。
//
// 内存序（关键）：
//   * head_ 只由消费者写、tail_ 只由生产者写，读对方使用 acquire；
//     写自己使用 release。这样生产者对 buffer_ 的写入 happens-before
//     消费者读到 tail_ 更新，消费者对槽位的读取也 happens-before
//     生产者复写该槽位（通过 release head_ / acquire head_ 建立顺序）。
//   * 读自己的索引用 relaxed（只有本线程会改它）。
//
// head_ / tail_ 采用单调递增的计数（不主动回绕），用 `tail - head`
// 判断满、`head == tail` 判断空，从而把 N 个槽位全部用上。
template <typename T, std::size_t N>
class SpscRingBuffer {
    static_assert(N >= 2, "容量至少为 2");
    static_assert((N & (N - 1)) == 0, "容量 N 必须是 2 的幂");

public:
    bool push(const T& value) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        if (tail - head >= N) return false; // 满
        buffer_[tail & kMask] = value;
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    bool push(T&& value) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        if (tail - head >= N) return false; // 满
        buffer_[tail & kMask] = std::move(value);
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    bool pop(T& out) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        if (head == tail) return false; // 空
        out = std::move(buffer_[head & kMask]);
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // 仅用于单线程诊断；并发调用时返回值仅供参考。
    std::size_t size() const {
        const std::size_t head = head_.load(std::memory_order_acquire);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        return tail - head;
    }

private:
    static constexpr std::size_t kMask = N - 1;

    // 生产者独占 tail_，消费者独占 head_；分处不同缓存行以避免伪共享。
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
    alignas(64) T                        buffer_[N];
};

}  // namespace nas
