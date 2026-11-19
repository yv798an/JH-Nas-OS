// HTTP 下载压测客户端（RISC-V / Linux）。
//
// 用法：
//   http_bench [--url URL] [--threads N] [--rounds R] [--warmup N]
//              [--label NAME] [--csv PATH] [--expect-hash HEX]
//              [--timeout SEC]
//
// 每个线程先跑 warmup 次（不计入结果），再跑 rounds 次。每次请求校验
// HTTP 状态码，并对响应体计算 FNV-1a 64 哈希；所有请求哈希必须一致。
// --expect-hash 可把哈希固定到 io_bench --verify 的 sync 哈希，用于确认
// sync / io_uring 下载内容一致。
//
// 退出码：0 成功；1 有请求失败；2 哈希不一致。

#include <httplib.h>

#include <sys/resource.h>
#include <sys/time.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr double kMiB = 1024.0 * 1024.0;
constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime  = 1099511628211ULL;

struct Url {
    std::string host;
    int         port = 80;
    std::string path = "/";
};

struct ThreadResult {
    std::uint64_t       bytes    = 0;
    std::uint64_t       requests = 0;
    std::uint64_t       errors   = 0;
    std::uint64_t       hash     = kFnvOffset;
    bool                hashSet  = false;
    bool                hashMismatch = false;
    std::vector<double> latMs;
};

struct LatencyStats {
    double p50Ms = 0;
    double p95Ms = 0;
    double p99Ms = 0;
};

struct CpuDelta {
    double user   = 0;
    double sys    = 0;
    long   maxrss = 0;
    long   minflt = 0;
    long   majflt = 0;
    long   nvcsw  = 0;
    long   nivcsw = 0;
};

Url parseUrl(const std::string& url) {
    Url         u;
    std::string s = url;
    const std::string http  = "http://";
    const std::string https = "https://";
    if (s.rfind(http, 0) == 0) {
        s = s.substr(http.size());
    } else if (s.rfind(https, 0) == 0) {
        s = s.substr(https.size());
    }
    const std::size_t slash    = s.find('/');
    const std::string hostport = slash == std::string::npos ? s : s.substr(0, slash);
    u.path = slash == std::string::npos ? "/" : s.substr(slash);

    const std::size_t colon = hostport.find(':');
    if (colon == std::string::npos) {
        u.host = hostport;
    } else {
        u.host = hostport.substr(0, colon);
        u.port = std::atoi(hostport.substr(colon + 1).c_str());
    }
    return u;
}

std::uint64_t fnv1a(std::uint64_t h, const char* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        h ^= static_cast<unsigned char>(p[i]);
        h *= kFnvPrime;
    }
    return h;
}

std::string hexHash(std::uint64_t h) {
    std::ostringstream os;
    os << std::hex << std::setw(16) << std::setfill('0') << h;
    return os.str();
}

double tvSec(const timeval& tv) {
    return static_cast<double>(tv.tv_sec) +
           static_cast<double>(tv.tv_usec) / 1000000.0;
}

CpuDelta cpuDelta(const rusage& a, const rusage& b) {
    CpuDelta d;
    d.user   = tvSec(b.ru_utime) - tvSec(a.ru_utime);
    d.sys    = tvSec(b.ru_stime) - tvSec(a.ru_stime);
    d.maxrss = b.ru_maxrss;
    d.minflt = b.ru_minflt - a.ru_minflt;
    d.majflt = b.ru_majflt - a.ru_majflt;
    d.nvcsw  = b.ru_nvcsw - a.ru_nvcsw;
    d.nivcsw = b.ru_nivcsw - a.ru_nivcsw;
    return d;
}

double percentile(const std::vector<double>& v, double p) {
    if (v.empty()) return 0.0;
    if (p <= 0.0) return v.front();
    if (p >= 1.0) return v.back();
    const double pos = p * static_cast<double>(v.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(std::floor(pos));
    const std::size_t hi = static_cast<std::size_t>(std::ceil(pos));
    const double frac = pos - static_cast<double>(lo);
    return v[lo] + (v[hi] - v[lo]) * frac;
}

LatencyStats latencyStats(std::vector<double> v) {
    LatencyStats s;
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    s.p50Ms = percentile(v, 0.50);
    s.p95Ms = percentile(v, 0.95);
    s.p99Ms = percentile(v, 0.99);
    return s;
}

bool fetch(httplib::Client& cli, const std::string& path,
           std::uint64_t& outBytes, std::uint64_t& outHash) {
    std::uint64_t bytes = 0;
    std::uint64_t hash  = kFnvOffset;
    auto res = cli.Get(path, [&](const char* d, std::size_t len) {
        hash = fnv1a(hash, d, len);
        bytes += len;
        return true;
    });
    if (!res) return false;
    if (res->status != 200 && res->status != 206) return false;
    outBytes = bytes;
    outHash  = hash;
    return true;
}

void printUsage(const char* argv0) {
    std::printf(
        "用法: %s [选项]\n"
        "  --url URL           下载 URL\n"
        "  --threads N         并发线程数，默认 8\n"
        "  --rounds R          每线程计时请求数，默认 1\n"
        "  --warmup N          每线程预热请求数，默认 0\n"
        "  --label NAME        CSV 标签，如 sync / uring\n"
        "  --csv PATH          输出一行 CSV\n"
        "  --expect-hash HEX   期望的 FNV-1a 64 哈希（可带 0x 前缀）\n"
        "  --timeout SEC       连接/读写超时，默认 120\n"
        "  -h, --help          显示帮助\n",
        argv0);
}

bool parseU64Hex(const std::string& s, std::uint64_t& out) {
    const char* p = s.c_str();
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(p, &end, 16);
    if (!end || end == p || *end != '\0') return false;
    out = static_cast<std::uint64_t>(v);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string url =
        "http://127.0.0.1:8080/api/download?path=/tmp/nas_io_bench.bin";
    std::string label = "http";
    std::string csvPath;
    int         threads = 8;
    int         rounds  = 1;
    int         warmup  = 0;
    int         timeout = 120;
    bool        expectSet = false;
    std::uint64_t expectHash = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::printf("缺少参数: %s\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--url") {
            const char* v = need("--url");
            if (!v) return 1;
            url = v;
        } else if (a == "--threads") {
            const char* v = need("--threads");
            if (!v) return 1;
            threads = std::atoi(v);
        } else if (a == "--rounds") {
            const char* v = need("--rounds");
            if (!v) return 1;
            rounds = std::atoi(v);
        } else if (a == "--warmup") {
            const char* v = need("--warmup");
            if (!v) return 1;
            warmup = std::atoi(v);
        } else if (a == "--label") {
            const char* v = need("--label");
            if (!v) return 1;
            label = v;
        } else if (a == "--csv") {
            const char* v = need("--csv");
            if (!v) return 1;
            csvPath = v;
        } else if (a == "--expect-hash") {
            const char* v = need("--expect-hash");
            if (!v) return 1;
            if (!parseU64Hex(v, expectHash)) {
                std::printf("非法 --expect-hash: %s\n", v);
                return 1;
            }
            expectSet = true;
        } else if (a == "--timeout") {
            const char* v = need("--timeout");
            if (!v) return 1;
            timeout = std::atoi(v);
        } else if (a == "-h" || a == "--help") {
            printUsage(argv[0]);
            return 0;
        } else {
            std::printf("未知参数: %s\n", a.c_str());
            printUsage(argv[0]);
            return 1;
        }
    }
    if (threads < 1) threads = 1;
    if (rounds < 1) rounds = 1;
    if (warmup < 0) warmup = 0;
    if (timeout < 1) timeout = 120;

    const Url u = parseUrl(url);
    std::vector<ThreadResult> results(static_cast<std::size_t>(threads));

    rusage before {};
    rusage after {};
    ::getrusage(RUSAGE_SELF, &before);

    const auto t0 = Clock::now();
    std::vector<std::thread> ts;
    ts.reserve(static_cast<std::size_t>(threads));
    for (int i = 0; i < threads; ++i) {
        ts.emplace_back([&, i] {
            httplib::Client cli(u.host, u.port);
            cli.set_connection_timeout(timeout, 0);
            cli.set_read_timeout(timeout, 0);
            cli.set_write_timeout(timeout, 0);
            cli.set_keep_alive(true);

            ThreadResult& tr = results[static_cast<std::size_t>(i)];
            for (int w = 0; w < warmup; ++w) {
                std::uint64_t bytes = 0;
                std::uint64_t hash  = 0;
                fetch(cli, u.path, bytes, hash);
            }
            for (int r = 0; r < rounds; ++r) {
                std::uint64_t bytes = 0;
                std::uint64_t hash  = 0;
                const auto    a     = Clock::now();
                const bool    ok    = fetch(cli, u.path, bytes, hash);
                const auto    b     = Clock::now();
                if (!ok) {
                    tr.errors++;
                    continue;
                }
                tr.requests++;
                tr.bytes += bytes;
                tr.latMs.push_back(
                    std::chrono::duration<double, std::milli>(b - a).count());
                if (!tr.hashSet) {
                    tr.hash    = hash;
                    tr.hashSet = true;
                } else if (tr.hash != hash) {
                    tr.hashMismatch = true;
                }
            }
        });
    }
    for (auto& t : ts) t.join();
    const auto t1 = Clock::now();
    ::getrusage(RUSAGE_SELF, &after);

    const double wall = std::chrono::duration<double>(t1 - t0).count();
    const CpuDelta cpu = cpuDelta(before, after);

    std::uint64_t totalBytes = 0;
    std::uint64_t totalReq   = 0;
    std::uint64_t totalErr   = 0;
    std::uint64_t firstHash  = 0;
    bool          hashSet    = false;
    bool          hashMismatch = false;
    std::vector<double> latAll;

    for (const ThreadResult& tr : results) {
        totalBytes += tr.bytes;
        totalReq += tr.requests;
        totalErr += tr.errors;
        latAll.insert(latAll.end(), tr.latMs.begin(), tr.latMs.end());
        if (tr.hashSet) {
            if (!hashSet) {
                firstHash = tr.hash;
                hashSet   = true;
            } else if (firstHash != tr.hash) {
                hashMismatch = true;
            }
        }
        if (tr.hashMismatch) hashMismatch = true;
    }

    const LatencyStats lat = latencyStats(latAll);
    const double cpuSec = cpu.user + cpu.sys;
    const double tput = wall > 0.0 ? static_cast<double>(totalBytes) / kMiB / wall : 0.0;
    const double cpuPct = wall > 0.0 ? cpuSec / wall * 100.0 : 0.0;
    const double cpuPerGiB =
        totalBytes > 0 ? cpuSec / (static_cast<double>(totalBytes) / kGiB) : 0.0;

    bool expectMismatch = false;
    if (expectSet) {
        if (!hashSet || firstHash != expectHash) expectMismatch = true;
    }

    std::printf("url=%s\n", url.c_str());
    std::printf("label=%s threads=%d rounds=%d warmup=%d\n", label.c_str(),
                threads, rounds, warmup);
    std::printf("requests=%llu bytes=%llu time=%.3f s throughput=%.1f MiB/s "
                "errors=%llu\n",
                static_cast<unsigned long long>(totalReq),
                static_cast<unsigned long long>(totalBytes), wall, tput,
                static_cast<unsigned long long>(totalErr));
    std::printf("cpu=%.3f s (user=%.3f sys=%.3f) cpu_pct=%.1f%% "
                "cpu_s_per_gib=%.3f p50=%.3f ms p95=%.3f ms p99=%.3f ms\n",
                cpuSec, cpu.user, cpu.sys, cpuPct, cpuPerGiB, lat.p50Ms,
                lat.p95Ms, lat.p99Ms);
    std::printf("hash=%s hash_mismatch=%d errors=%llu%s\n",
                hashSet ? hexHash(firstHash).c_str() : "-",
                hashMismatch ? 1 : 0,
                static_cast<unsigned long long>(totalErr),
                expectMismatch ? " (expected hash mismatch)" : "");

    if (!csvPath.empty()) {
        std::ofstream csv(csvPath, std::ios::out | std::ios::trunc);
        if (!csv) {
            std::printf("无法写 CSV: %s\n", csvPath.c_str());
            return 1;
        }
        csv << "bench,label,threads,rounds,warmup,requests,bytes,wall_s,"
               "throughput_mib_s,user_s,sys_s,cpu_s,cpu_pct,cpu_s_per_gib,"
               "maxrss_kib,minflt,majflt,nvcsw,nivcsw,p50_ms,p95_ms,p99_ms,"
               "hash,hash_mismatch,errors\n";
        csv << "http," << label << ',' << threads << ',' << rounds << ','
            << warmup << ',' << totalReq << ',' << totalBytes << ','
            << std::fixed << std::setprecision(6) << wall << ',' << tput
            << ',' << cpu.user << ',' << cpu.sys << ',' << cpuSec << ','
            << cpuPct << ',' << cpuPerGiB << ',' << cpu.maxrss << ','
            << cpu.minflt << ',' << cpu.majflt << ',' << cpu.nvcsw << ','
            << cpu.nivcsw << ',' << lat.p50Ms << ',' << lat.p95Ms << ','
            << lat.p99Ms << ',' << (hashSet ? hexHash(firstHash) : "")
            << ',' << (hashMismatch ? 1 : 0) << ',' << totalErr << '\n';
    }

    if (totalErr > 0) return 1;
    if (hashMismatch || expectMismatch) return 2;
    return 0;
}
