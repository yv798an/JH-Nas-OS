#pragma once

// =====================================================================
//  system/ShareRegistry.h - 共享目录白名单策略
//
//  适配层（ISystemState）只吐「原始挂载表」；本类按配置允许的根目录
//  （如 /media）过滤出可对外共享的挂载点。白名单是业务策略，不放进
//  平台抽象。
// =====================================================================

#include "platform/SystemTypes.h"

#include <string>
#include <vector>

namespace nas {

struct Share {
    std::string name;    // 由挂载点末段派生，如 sda1
    std::string path;    // /media/sda1
    std::string device;  // /dev/sda1
    std::string fstype;  // vfat / ntfs / ext4
    bool        writable = false;
};

class ShareRegistry {
public:
    explicit ShareRegistry(std::vector<std::string> allowedRoots = {"/media"});

    // 过滤并整理挂载表：仅保留 allowedRoots 的严格子路径，按 path 升序，
    // 保证输出确定。
    std::vector<Share> resolve(const std::vector<MountPoint>& mounts) const;

    // path 是否位于任一允许根目录之下（不含根目录自身）。
    bool allows(const std::string& path) const;

    const std::vector<std::string>& roots() const { return allowedRoots_; }

private:
    std::vector<std::string> allowedRoots_;
};

}  // namespace nas
