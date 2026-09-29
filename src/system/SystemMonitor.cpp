#include "system/SystemMonitor.h"

#include <cstdio>
#include <iomanip>
#include <sstream>
#include <utility>

namespace nas {

namespace {

constexpr std::uint64_t kKb = 1024;

std::uint64_t kbToBytes(std::uint64_t kb) { return kb * kKb; }

double percent(std::uint64_t used, std::uint64_t total) {
    if (total == 0) return 0.0;
    return 100.0 * static_cast<double>(used) / static_cast<double>(total);
}

std::string escapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (const char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

void writeShare(std::ostringstream& os, const ShareStatus& s) {
    os << "{\"name\":\"" << escapeJson(s.name) << "\",\"path\":\""
       << escapeJson(s.path) << "\",\"device\":\"" << escapeJson(s.device)
       << "\",\"fstype\":\"" << escapeJson(s.fstype)
       << "\",\"writable\":" << (s.writable ? "true" : "false")
       << ",\"usageOk\":" << (s.usageOk ? "true" : "false")
       << ",\"totalBytes\":" << s.totalBytes
       << ",\"usedBytes\":" << s.usedBytes
       << ",\"freeBytes\":" << s.freeBytes << ",\"usedPercent\":"
       << std::fixed << std::setprecision(1) << s.usedPercent << "}";
}

}  // namespace

SystemMonitor::SystemMonitor(const ISystemState& state, ShareRegistry registry)
    : state_(state), registry_(std::move(registry)) {}

SystemStatus SystemMonitor::report() const {
    SystemStatus out;

    const std::shared_ptr<const SystemSnapshot> snap = state_.snapshot();
    if (!snap) return out;

    out.uptimeSec  = snap->uptimeSec;
    out.load       = snap->load;
    out.cpuPercent = snap->cpuPercent;
    out.net        = snap->net;

    out.mem.totalBytes     = kbToBytes(snap->mem.totalKb);
    out.mem.availableBytes = kbToBytes(snap->mem.availableKb);
    if (out.mem.totalBytes >= out.mem.availableBytes) {
        out.mem.usedBytes = out.mem.totalBytes - out.mem.availableBytes;
    }
    out.mem.usedPercent = percent(out.mem.usedBytes, out.mem.totalBytes);

    for (const Share& share : registry_.resolve(snap->mounts)) {
        ShareStatus st;
        st.name     = share.name;
        st.path     = share.path;
        st.device   = share.device;
        st.fstype   = share.fstype;
        st.writable = share.writable;

        const DiskUsage usage = state_.diskUsage(share.path);
        st.usageOk    = usage.ok;
        st.totalBytes = usage.totalBytes;
        st.freeBytes  = usage.freeBytes;
        if (usage.ok && usage.totalBytes >= usage.freeBytes) {
            st.usedBytes = usage.totalBytes - usage.freeBytes;
        }
        st.usedPercent = percent(st.usedBytes, st.totalBytes);
        out.shares.push_back(std::move(st));
    }

    return out;
}

std::string toJson(const SystemStatus& status) {
    std::ostringstream os;
    os << std::fixed;
    os << "{\"uptimeSec\":" << std::setprecision(1) << status.uptimeSec
       << ",\"load\":{\"load1\":" << std::setprecision(2) << status.load.load1
       << ",\"load5\":" << status.load.load5
       << ",\"load15\":" << status.load.load15 << "}"
       << ",\"cpu\":{\"percent\":" << std::setprecision(1) << status.cpuPercent
       << "}"
       << ",\"mem\":{\"totalBytes\":" << status.mem.totalBytes
       << ",\"usedBytes\":" << status.mem.usedBytes
       << ",\"availableBytes\":" << status.mem.availableBytes
       << ",\"usedPercent\":" << std::setprecision(1) << status.mem.usedPercent
       << "}"
       << ",\"net\":{\"rxBytes\":" << status.net.rxBytes
       << ",\"txBytes\":" << status.net.txBytes << "}"
       << ",\"shares\":[";
    for (std::size_t i = 0; i < status.shares.size(); ++i) {
        if (i) os << ",";
        writeShare(os, status.shares[i]);
    }
    os << "]}";
    return os.str();
}

}  // namespace nas
