#include "fakes/FakeSystemState.h"

#include <utility>

namespace nas {

FakeSystemState::FakeSystemState() {
    snapshot_ = std::make_shared<const SystemSnapshot>(build_);
}

void FakeSystemState::start() {}
void FakeSystemState::stop() {}

std::shared_ptr<const SystemSnapshot> FakeSystemState::snapshot() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return snapshot_;
}

DiskUsage FakeSystemState::diskUsage(const std::string& path) const {
    std::lock_guard<std::mutex> lk(mutex_);
    const auto it = diskUsage_.find(path);
    if (it == diskUsage_.end()) return DiskUsage{};
    return it->second;
}

std::uint64_t FakeSystemState::onMountsChanged(MountCallback cb) {
    std::lock_guard<std::mutex> lk(mutex_);
    const std::uint64_t token = nextToken_++;
    callbacks_[token]         = std::move(cb);
    return token;
}

void FakeSystemState::removeMountCallback(std::uint64_t token) {
    std::lock_guard<std::mutex> lk(mutex_);
    callbacks_.erase(token);
}

void FakeSystemState::setLoad(const LoadInfo& load) {
    std::lock_guard<std::mutex> lk(mutex_);
    build_.load = load;
}
void FakeSystemState::setMem(const MemInfo& mem) {
    std::lock_guard<std::mutex> lk(mutex_);
    build_.mem = mem;
}
void FakeSystemState::setUptime(double seconds) {
    std::lock_guard<std::mutex> lk(mutex_);
    build_.uptimeSec = seconds;
}
void FakeSystemState::setCpuPercent(double percent) {
    std::lock_guard<std::mutex> lk(mutex_);
    build_.cpuPercent = percent;
}
void FakeSystemState::setNet(const NetStats& net) {
    std::lock_guard<std::mutex> lk(mutex_);
    build_.net = net;
}
void FakeSystemState::setDiskUsage(const std::string& path,
                                   const DiskUsage& usage) {
    std::lock_guard<std::mutex> lk(mutex_);
    diskUsage_[path] = usage;
}
void FakeSystemState::setMounts(std::vector<MountPoint> mounts) {
    std::lock_guard<std::mutex> lk(mutex_);
    build_.mounts = std::move(mounts);
}

bool FakeSystemState::publish() {
    std::map<std::uint64_t, MountCallback> callbacks;
    std::shared_ptr<const SystemSnapshot>  published;
    bool                                   changed = false;

    {
        std::lock_guard<std::mutex> lk(mutex_);
        changed = !(snapshot_->mounts == build_.mounts);
        published = std::make_shared<const SystemSnapshot>(build_);
        snapshot_ = published;
        if (changed) callbacks = callbacks_;
    }

    // 锁外调用，避免回调内再调用 snapshot() 造成自锁。
    if (changed) {
        for (auto& kv : callbacks) {
            if (kv.second) kv.second(published);
        }
    }
    return changed;
}

}  // namespace nas
