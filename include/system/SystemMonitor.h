#pragma once

// =====================================================================
//  system/SystemMonitor.h - 系统状态消费方
//
//  把平台层（ISystemState）的原始快照 + diskUsage 组装成面向 API 的
//  DTO（SystemStatus），并附一个 JSON 序列化。业务/HTTP 层只依赖
//  SystemMonitor，不再直接接触 ISystemState 或 /proc。
//
//  设计：可注入 ISystemState，因此单测用 FakeSystemState 即可确定性
//  验证，无需开发板。
// =====================================================================

#include "platform/ISystemState.h"
#include "platform/SystemTypes.h"
#include "system/ShareRegistry.h"

#include <cstdint>
#include <string>
#include <vector>

namespace nas {

struct MemoryStatus {
    std::uint64_t totalBytes     = 0;
    std::uint64_t usedBytes      = 0;
    std::uint64_t availableBytes = 0;
    double        usedPercent    = 0.0;
};

struct ShareStatus {
    std::string   name;
    std::string   path;
    std::string   device;
    std::string   fstype;
    bool          writable  = false;
    bool          usageOk   = false;
    std::uint64_t totalBytes = 0;
    std::uint64_t usedBytes  = 0;
    std::uint64_t freeBytes  = 0;
    double        usedPercent = 0.0;
};

struct SystemStatus {
    double                  uptimeSec  = 0.0;
    LoadInfo                load;
    MemoryStatus            mem;
    double                  cpuPercent = 0.0;
    NetStats                net;
    std::vector<ShareStatus> shares;
};

class SystemMonitor {
public:
    explicit SystemMonitor(const ISystemState& state,
                           ShareRegistry     registry = ShareRegistry{});

    // 采集一次并组装为展示就绪的状态。线程安全（依赖 ISystemState 的契约）。
    SystemStatus report() const;

private:
    const ISystemState& state_;
    ShareRegistry       registry_;
};

// 将状态序列化为单行 JSON（供 /api/system 与离线诊断复用）。
std::string toJson(const SystemStatus& status);

}  // namespace nas
