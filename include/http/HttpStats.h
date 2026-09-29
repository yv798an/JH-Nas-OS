#pragma once

// =====================================================================
//  http/HttpStats.h - 进程级 HTTP 计数（供 /api/metrics 暴露）
//
//  适配层在每次响应后累加；Api 的 metrics 处理器读取。函数内静态保证
//  全进程一个实例，无需显式传递。
// =====================================================================

#include <atomic>
#include <cstdint>

namespace nas {

struct HttpStats {
    std::atomic<std::uint64_t> requestsTotal{0};
    std::atomic<std::uint64_t> responses4xx{0};
    std::atomic<std::uint64_t> responses5xx{0};
};

inline HttpStats& httpStats() {
    static HttpStats instance;
    return instance;
}

}  // namespace nas
