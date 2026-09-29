#include "platform/linux/LinuxSystemState.h"

#if defined(__linux__)

#include <sys/statvfs.h>
#include <unistd.h>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

namespace nas {

namespace {

// 还原 /proc/mounts 中的八进制转义（空格 \040、制表 \011、换行 \012、
// 反斜杠 \134）。
std::string unescapeMountField(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\\' && i + 3 < in.size() && in[i + 1] >= '0' &&
            in[i + 1] <= '7' && in[i + 2] >= '0' && in[i + 2] <= '7' &&
            in[i + 3] >= '0' && in[i + 3] <= '7') {
            const int code = (in[i + 1] - '0') * 64 + (in[i + 2] - '0') * 8 +
                             (in[i + 3] - '0');
            out.push_back(static_cast<char>(code));
            i += 3;
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

std::string trim(const std::string& s) {
    const std::size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return {};
    const std::size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

// 挂载选项是逗号分隔的，判断其中是否含某个 token。
bool hasOption(const std::string& options, const char* name) {
    std::size_t pos = 0;
    while (pos <= options.size()) {
        const std::size_t comma = options.find(',', pos);
        const std::string tok =
            options.substr(pos, comma == std::string::npos ? std::string::npos
                                                           : comma - pos);
        if (tok == name) return true;
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return false;
}

// /proc/meminfo 一行形如 "MemTotal:  123456 kB"，取冒号后的数字（kB）。
std::uint64_t meminfoKb(const std::string& line) {
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos) return 0;
    std::istringstream is(line.substr(colon + 1));
    std::uint64_t value = 0;
    is >> value;
    return value;
}

}  // namespace

LinuxSystemState::LinuxSystemState(std::chrono::milliseconds pollInterval)
    : pollInterval_(pollInterval) {
    std::atomic_store_explicit(
        &snapshot_,
        std::make_shared<const SystemSnapshot>(SystemSnapshot{}),
        std::memory_order_release);
}

LinuxSystemState::~LinuxSystemState() { stop(); }

void LinuxSystemState::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;

    // 初始化阶段：主动扫描一次，保证 start() 返回后 snapshot() 非空。
    std::atomic_store_explicit(
        &snapshot_, std::make_shared<const SystemSnapshot>(collect()),
        std::memory_order_release);

    thread_ = std::thread(&LinuxSystemState::loop, this);
}

void LinuxSystemState::stop() {
    running_.store(false, std::memory_order_release);
    sleepCv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void LinuxSystemState::loop() {
    while (running_.load(std::memory_order_acquire)) {
        {
            std::unique_lock<std::mutex> lk(sleepMutex_);
            sleepCv_.wait_for(lk, pollInterval_, [this] {
                return !running_.load(std::memory_order_acquire);
            });
        }
        if (!running_.load(std::memory_order_acquire)) break;
        publishIfChanged(collect());
    }
}

std::shared_ptr<const SystemSnapshot> LinuxSystemState::snapshot() const {
    return std::atomic_load_explicit(&snapshot_, std::memory_order_acquire);
}

std::vector<MountPoint> LinuxSystemState::readMounts() const {
    std::vector<MountPoint> mounts;

    std::ifstream f("/proc/mounts");
    if (!f) f.open("/proc/self/mounts");
    if (!f) return mounts;

    std::string line;
    while (std::getline(f, line)) {
        std::istringstream is(line);
        std::string device, mountPoint, fstype, options;
        if (!(is >> device >> mountPoint >> fstype >> options)) continue;

        MountPoint mp;
        mp.device    = unescapeMountField(device);
        mp.mountPath = unescapeMountField(mountPoint);
        mp.fstype    = fstype;
        mp.writable  = !hasOption(options, "ro");
        mounts.push_back(std::move(mp));
    }
    return mounts;
}

SystemSnapshot LinuxSystemState::collect() {
    SystemSnapshot snap;

    if (std::ifstream f("/proc/loadavg"); f) {
        double a = 0, b = 0, c = 0;
        if (f >> a >> b >> c) {
            snap.load.load1  = a;
            snap.load.load5  = b;
            snap.load.load15 = c;
        }
    }

    if (std::ifstream f("/proc/meminfo"); f) {
        std::string line;
        while (std::getline(f, line)) {
            if (line.compare(0, 9, "MemTotal:") == 0) {
                snap.mem.totalKb = meminfoKb(line);
            } else if (line.compare(0, 8, "MemFree:") == 0) {
                snap.mem.freeKb = meminfoKb(line);
            } else if (line.compare(0, 13, "MemAvailable:") == 0) {
                snap.mem.availableKb = meminfoKb(line);
            }
        }
    }

    if (std::ifstream f("/proc/uptime"); f) {
        double seconds = 0.0;
        if (f >> seconds) snap.uptimeSec = seconds;
    }

    if (std::ifstream f("/proc/stat"); f) {
        std::string   cpu;
        std::uint64_t user = 0, nice = 0, system = 0, idle = 0;
        std::uint64_t iowait = 0, irq = 0, softirq = 0, steal = 0;
        if (f >> cpu >> user >> nice >> system >> idle >> iowait >> irq >>
            softirq >> steal) {
            const std::uint64_t idleAll = idle + iowait;
            const std::uint64_t total =
                user + nice + system + idle + iowait + irq + softirq + steal;
            if (prevTotal_ != 0 && total > prevTotal_) {
                const std::uint64_t dTotal = total - prevTotal_;
                const std::uint64_t dIdle  = idleAll - prevIdle_;
                snap.cpuPercent =
                    100.0 * static_cast<double>(dTotal - dIdle) /
                    static_cast<double>(dTotal);
            }
            prevTotal_ = total;
            prevIdle_  = idleAll;
        }
    }

    if (std::ifstream f("/proc/net/dev"); f) {
        std::string line;
        std::getline(f, line);  // 表头第 1 行
        std::getline(f, line);  // 表头第 2 行
        while (std::getline(f, line)) {
            const std::size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            if (trim(line.substr(0, colon)) == "lo") continue;

            std::istringstream is(line.substr(colon + 1));
            std::uint64_t rx = 0, p1 = 0, p2 = 0, p3 = 0, p4 = 0, p5 = 0,
                          p6 = 0, p7 = 0, tx = 0;
            if (is >> rx >> p1 >> p2 >> p3 >> p4 >> p5 >> p6 >> p7 >> tx) {
                snap.net.rxBytes += rx;
                snap.net.txBytes += tx;
            }
        }
    }

    snap.mounts = readMounts();
    return snap;
}

void LinuxSystemState::publishIfChanged(SystemSnapshot&& next) {
    const std::shared_ptr<const SystemSnapshot> current =
        std::atomic_load_explicit(&snapshot_, std::memory_order_acquire);
    const bool mountsChanged =
        !current || !(current->mounts == next.mounts);

    std::shared_ptr<const SystemSnapshot> published =
        std::make_shared<const SystemSnapshot>(std::move(next));
    std::atomic_store_explicit(&snapshot_, published, std::memory_order_release);

    if (!mountsChanged) return;

    std::map<std::uint64_t, MountCallback> cbs;
    {
        std::lock_guard<std::mutex> lk(cbMutex_);
        cbs = callbacks_;
    }
    for (auto& kv : cbs) {
        if (kv.second) kv.second(published);
    }
}

DiskUsage LinuxSystemState::diskUsage(const std::string& path) const {
    DiskUsage usage;
    struct statvfs vfs {};
    if (::statvfs(path.c_str(), &vfs) != 0) return usage;
    usage.totalBytes =
        static_cast<std::uint64_t>(vfs.f_blocks) * vfs.f_frsize;
    usage.freeBytes = static_cast<std::uint64_t>(vfs.f_bavail) * vfs.f_frsize;
    usage.ok        = true;
    return usage;
}

std::uint64_t LinuxSystemState::onMountsChanged(MountCallback cb) {
    std::lock_guard<std::mutex> lk(cbMutex_);
    const std::uint64_t token = nextToken_++;
    callbacks_[token]         = std::move(cb);
    return token;
}

void LinuxSystemState::removeMountCallback(std::uint64_t token) {
    std::lock_guard<std::mutex> lk(cbMutex_);
    callbacks_.erase(token);
}

}  // namespace nas

#endif  // __linux__
