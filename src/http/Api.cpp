#include "http/Api.h"

#include "http/HttpStats.h"

#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

#if defined(__linux__)
#include <unistd.h>

#include <filesystem>
#endif

namespace nas {

namespace {

HttpResponse textResponse(int status, const std::string& contentType,
                          std::string body) {
    HttpResponse res;
    res.status                  = status;
    res.headers["content-type"] = contentType;
    res.body                    = std::move(body);
    return res;
}

HttpResponse jsonResponse(int status, std::string body) {
    return textResponse(status, "application/json; charset=utf-8",
                        std::move(body));
}

std::string escapeLabel(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

// 进程级指标（仅 Linux）：RSS、线程数、打开 fd 数。
struct ProcMetrics {
    std::uint64_t rssBytes = 0;
    std::uint64_t threads  = 0;
    std::uint64_t openFds  = 0;
    bool          ok       = false;
};

#if defined(__linux__)
ProcMetrics readProcMetrics() {
    ProcMetrics m;

    if (std::ifstream f("/proc/self/statm"); f) {
        std::uint64_t total = 0, resident = 0;
        if (f >> total >> resident) {
            const long pageSize = ::sysconf(_SC_PAGESIZE);
            if (pageSize > 0) {
                m.rssBytes = resident * static_cast<std::uint64_t>(pageSize);
            }
        }
    }
    if (std::ifstream f("/proc/self/status"); f) {
        std::string line;
        while (std::getline(f, line)) {
            if (line.compare(0, 8, "Threads:") == 0) {
                std::istringstream is(line.substr(8));
                is >> m.threads;
                break;
            }
        }
    }
    std::error_code ec;
    std::uint64_t   count = 0;
    for (std::filesystem::directory_iterator it("/proc/self/fd", ec), end;
         !ec && it != end; it.increment(ec)) {
        ++count;
    }
    m.openFds = count;
    m.ok      = true;
    return m;
}
#endif

// 组装 Prometheus 文本格式指标。
std::string buildMetrics(const SystemMonitor& monitor) {
    const SystemStatus s = monitor.report();

    std::ostringstream os;
    os << std::fixed;

    auto gauge = [&](const char* name, const char* help, double value,
                     int precision) {
        os << "# HELP " << name << ' ' << help << '\n'
           << "# TYPE " << name << " gauge\n"
           << name << ' ' << std::setprecision(precision) << value << '\n';
    };

    gauge("nas_uptime_seconds", "System uptime in seconds", s.uptimeSec, 1);
    gauge("nas_cpu_percent", "CPU usage percent", s.cpuPercent, 2);
    gauge("nas_load1", "1-minute load average", s.load.load1, 3);
    gauge("nas_load5", "5-minute load average", s.load.load5, 3);
    gauge("nas_load15", "15-minute load average", s.load.load15, 3);
    gauge("nas_mem_total_bytes", "Total memory bytes",
          static_cast<double>(s.mem.totalBytes), 0);
    gauge("nas_mem_used_bytes", "Used memory bytes",
          static_cast<double>(s.mem.usedBytes), 0);
    gauge("nas_mem_available_bytes", "Available memory bytes",
          static_cast<double>(s.mem.availableBytes), 0);

    os << "# HELP nas_net_rx_bytes_total Received bytes (counter)\n"
       << "# TYPE nas_net_rx_bytes_total counter\n"
       << "nas_net_rx_bytes_total " << s.net.rxBytes << '\n';
    os << "# HELP nas_net_tx_bytes_total Transmitted bytes (counter)\n"
       << "# TYPE nas_net_tx_bytes_total counter\n"
       << "nas_net_tx_bytes_total " << s.net.txBytes << '\n';

    os << "# HELP nas_share_used_bytes Used bytes per share\n"
       << "# TYPE nas_share_used_bytes gauge\n";
    for (const ShareStatus& sh : s.shares) {
        os << "nas_share_used_bytes{name=\"" << escapeLabel(sh.name)
           << "\",path=\"" << escapeLabel(sh.path) << "\"} " << sh.usedBytes
           << '\n';
    }
    os << "# HELP nas_share_total_bytes Total bytes per share\n"
       << "# TYPE nas_share_total_bytes gauge\n";
    for (const ShareStatus& sh : s.shares) {
        os << "nas_share_total_bytes{name=\"" << escapeLabel(sh.name)
           << "\",path=\"" << escapeLabel(sh.path) << "\"} " << sh.totalBytes
           << '\n';
    }

#if defined(__linux__)
    const ProcMetrics p = readProcMetrics();
    if (p.ok) {
        gauge("nas_process_rss_bytes", "Process resident set size bytes",
              static_cast<double>(p.rssBytes), 0);
        gauge("nas_process_threads", "Process thread count",
              static_cast<double>(p.threads), 0);
        gauge("nas_process_open_fds", "Process open file descriptors",
              static_cast<double>(p.openFds), 0);
    }
#endif

    const HttpStats& hs = httpStats();
    os << "# HELP nas_http_requests_total Total HTTP requests\n"
       << "# TYPE nas_http_requests_total counter\n"
       << "nas_http_requests_total " << hs.requestsTotal.load() << '\n';
    os << "# HELP nas_http_responses_4xx_total HTTP 4xx responses\n"
       << "# TYPE nas_http_responses_4xx_total counter\n"
       << "nas_http_responses_4xx_total " << hs.responses4xx.load() << '\n';
    os << "# HELP nas_http_responses_5xx_total HTTP 5xx responses\n"
       << "# TYPE nas_http_responses_5xx_total counter\n"
       << "nas_http_responses_5xx_total " << hs.responses5xx.load() << '\n';

    return os.str();
}

}  // namespace

void registerApiRoutes(Router& router, const SystemMonitor& monitor) {
    router.add("GET", "/api/health", [&monitor](const HttpRequest&) {
        const SystemStatus status = monitor.report();
        std::ostringstream os;
        os << std::fixed << std::setprecision(1);
        os << "{\"status\":\"ok\",\"uptimeSec\":" << status.uptimeSec
           << ",\"shares\":" << status.shares.size() << "}";
        return jsonResponse(200, os.str());
    });

    router.add("GET", "/api/system", [&monitor](const HttpRequest&) {
        return jsonResponse(200, toJson(monitor.report()));
    });

    router.add("GET", "/api/metrics", [&monitor](const HttpRequest&) {
        return textResponse(200, "text/plain; version=0.0.4; charset=utf-8",
                            buildMetrics(monitor));
    });
}

}  // namespace nas
