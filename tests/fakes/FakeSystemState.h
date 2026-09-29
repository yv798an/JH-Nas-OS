#pragma once

// =====================================================================
//  fakes/FakeSystemState.h - ISystemState 的确定性测试替身
//
//  不启动线程、不访问系统。测试通过 set* 注入数据，再显式 publish()
//  发布快照；仅当挂载表发生变化时，publish() 才同步触发回调。
//  这样测试完全确定，可覆盖「无盘 / 单盘 / 双盘 / 拔盘」等边界。
//
//  本文件仅用于测试，不参与 nas-server 构建。
// =====================================================================

#include "platform/ISystemState.h"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace nas {

class FakeSystemState final : public ISystemState {
public:
    FakeSystemState();

    void start() override;
    void stop() override;

    std::shared_ptr<const SystemSnapshot> snapshot() const override;
    DiskUsage diskUsage(const std::string& path) const override;

    std::uint64_t onMountsChanged(MountCallback cb) override;
    void removeMountCallback(std::uint64_t token) override;

    // ---- 测试注入接口（不自动发布，需随后调用 publish()）----
    void setLoad(const LoadInfo& load);
    void setMem(const MemInfo& mem);
    void setUptime(double seconds);
    void setCpuPercent(double percent);
    void setNet(const NetStats& net);
    void setDiskUsage(const std::string& path, const DiskUsage& usage);
    void setMounts(std::vector<MountPoint> mounts);

    // 用当前注入值重建快照并发布。
    // 返回挂载表是否发生变化（true 时已顺序触发所有回调）。
    bool publish();

private:
    mutable std::mutex                    mutex_;
    std::shared_ptr<const SystemSnapshot> snapshot_;
    SystemSnapshot                        build_;   // 待发布的累积值
    std::map<std::string, DiskUsage>      diskUsage_;
    std::map<std::uint64_t, MountCallback> callbacks_;
    std::uint64_t                         nextToken_ = 1;
};

}  // namespace nas
