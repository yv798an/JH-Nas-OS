#pragma once

// =====================================================================
//  platform/SystemTypes.h - 平台无关的系统状态数据结构
//
//  这些结构是平台适配层与业务层之间的数据契约，只含纯数据，不含
//  任何 OS 依赖，便于在 x86 上单元测试。业务层只依赖这些类型与
//  ISystemState 接口，不直接接触 /proc、/sys。
// =====================================================================

#include <cstdint>
#include <string>
#include <vector>

namespace nas {

struct LoadInfo {
    double load1  = 0.0;
    double load5  = 0.0;
    double load15 = 0.0;
};

struct MemInfo {
    std::uint64_t totalKb     = 0;
    std::uint64_t freeKb      = 0;
    std::uint64_t availableKb = 0;
};

struct NetStats {
    std::uint64_t rxBytes = 0;
    std::uint64_t txBytes = 0;
};

struct DiskUsage {
    std::uint64_t totalBytes = 0;
    std::uint64_t freeBytes  = 0;
    bool          ok         = false;
};

// 原始挂载表的一项。适配层只吐原始数据，白名单/共享策略由业务层
// （ShareRegistry）决定。
struct MountPoint {
    std::string device;     // /dev/sda1
    std::string mountPath;  // /media/sda1
    std::string fstype;     // vfat / ntfs / ext4
    bool        writable = false;
};

inline bool operator==(const MountPoint& a, const MountPoint& b) {
    return a.device == b.device && a.mountPath == b.mountPath &&
           a.fstype == b.fstype && a.writable == b.writable;
}
inline bool operator!=(const MountPoint& a, const MountPoint& b) {
    return !(a == b);
}

// 一次性、发布后不可变的系统状态快照。读者取到 shared_ptr 后即可无锁
// 使用；旧快照由引用计数自动回收（C++17：配 ISystemState 内部
// atomic_load/atomic_store 使用）。
struct SystemSnapshot {
    LoadInfo   load;
    MemInfo    mem;
    double     uptimeSec  = 0.0;
    double     cpuPercent = 0.0;
    NetStats   net;
    std::vector<MountPoint> mounts;  // 原始挂载表（未过滤）
};

}  // namespace nas
