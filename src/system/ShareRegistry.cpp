#include "system/ShareRegistry.h"

#include <algorithm>
#include <utility>

namespace nas {

namespace {

std::string baseName(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return path;
    if (slash + 1 >= path.size()) return path;  // 以 '/' 结尾，退回整串
    return path.substr(slash + 1);
}

}  // namespace

ShareRegistry::ShareRegistry(std::vector<std::string> allowedRoots)
    : allowedRoots_(std::move(allowedRoots)) {}

bool ShareRegistry::allows(const std::string& path) const {
    for (const std::string& root : allowedRoots_) {
        if (root.empty() || path.size() <= root.size()) continue;
        if (path.compare(0, root.size(), root) != 0) continue;
        // 必须是根目录的严格子路径：root + "/..."
        if (path[root.size()] == '/') return true;
    }
    return false;
}

std::vector<Share> ShareRegistry::resolve(
    const std::vector<MountPoint>& mounts) const {
    std::vector<Share> shares;
    shares.reserve(mounts.size());
    for (const MountPoint& mp : mounts) {
        if (!allows(mp.mountPath)) continue;
        Share s;
        s.name     = baseName(mp.mountPath);
        s.path     = mp.mountPath;
        s.device   = mp.device;
        s.fstype   = mp.fstype;
        s.writable = mp.writable;
        shares.push_back(std::move(s));
    }
    std::sort(shares.begin(), shares.end(),
              [](const Share& a, const Share& b) { return a.path < b.path; });
    return shares;
}

}  // namespace nas
