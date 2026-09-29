#pragma once

// =====================================================================
//  platform/linux/LinuxSystemState.h - ISystemState 的 Linux 实现
//
//  机制：启动时主动扫描一次 /proc，随后由一个监控线程以固定周期
//  （默认 1s）轮询重建快照；仅在挂载表发生变化时触发回调。
//  只读消费挂载表，绝不 mount/umount（挂载由 automount.sh 负责）。
//
//  本头文件本身平台无关；实现体在 LinuxSystemState.cpp 内用
//  #if defined(__linux__) 包裹，非 Linux 平台编译为空。
// =====================================================================

#include "platform/ISystemState.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace nas {

class LinuxSystemState final : public ISystemState {
public:
    explicit LinuxSystemState(
        std::chrono::milliseconds pollInterval = std::chrono::milliseconds(1000));
    ~LinuxSystemState() override;

    LinuxSystemState(const LinuxSystemState&)            = delete;
    LinuxSystemState& operator=(const LinuxSystemState&) = delete;

    void start() override;
    void stop() override;

    std::shared_ptr<const SystemSnapshot> snapshot() const override;
    DiskUsage diskUsage(const std::string& path) const override;

    std::uint64_t onMountsChanged(MountCallback cb) override;
    void removeMountCallback(std::uint64_t token) override;

private:
    void loop();
    SystemSnapshot      collect();
    std::vector<MountPoint> readMounts() const;
    void                publishIfChanged(SystemSnapshot&& next);

    std::chrono::milliseconds pollInterval_;

    // C++17：用 std::atomic_load/atomic_store 自由函数安全发布快照，
    // 而不是 C++20 的 std::atomic<std::shared_ptr>。
    mutable std::shared_ptr<const SystemSnapshot> snapshot_;

    std::mutex                            cbMutex_;
    std::map<std::uint64_t, MountCallback> callbacks_;
    std::uint64_t                         nextToken_ = 1;

    std::mutex              sleepMutex_;
    std::condition_variable sleepCv_;
    std::thread             thread_;
    std::atomic<bool>       running_{false};

    // CPU 采样：仅监控线程访问（start() 的首次采样发生在建线程之前）。
    std::uint64_t prevTotal_ = 0;
    std::uint64_t prevIdle_  = 0;
};

}  // namespace nas
