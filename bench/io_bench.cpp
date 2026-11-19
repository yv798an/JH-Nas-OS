// io_uring vs 同步读取 基准（RISC-V / Linux）。
//
// 用法：
//   io_bench [--file PATH] [--size MiB]
//            [--block KiB] [--blocks 4,8,16,...]
//            [--depth N] [--depths 1,4,8,16,...]
//            [--repeat N] [--warmup N]
//            [--cache cold|hot|none] [--drop-cache]
//            [--mode sync|uring|both]
//            [--csv PATH] [--verify]
//
// --cache cold   每个计时 run 前 drop page cache（需 root）
// --cache hot    开始时预热文件，之后不再 drop
// --verify       不走性能计时，逐个方案读完整文件并输出 FNV-1a 哈希，
//                用于确认 sync / io_uring 读到的内容一致。
//
// 输出 CSV 每个 run 一行；stdout 打每个方案的 median/min/max、CPU%、
// CPU-s/GiB 和 p50/p95/p99 消费端延迟（单位 us）。

#include <cstdio>

#if defined(__linux__) && defined(NAS_HAS_IO_URING)

#include "io/AsyncFileReader.h"
#include "io/IoUring.h"

#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr double kMiB = 1024.0 * 1024.0;
constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime  = 1099511628211ULL;

struct Options {
    std::string           file;
    std::uint64_t         sizeMiB = 128;
    std::vector<unsigned> blocksKiB{64};
    std::vector<unsigned> depths{1, 4, 16, 64};
    int                   repeat = 1;
    int                   warmup = 0;
    std::string           cache  = "none";  // cold / hot / none
    std::string           mode   = "both";  // sync / uring / both
    std::string           handoff = "yield"; // yield / condvar
    std::string           csv;
    bool                  verify = false;
};

struct RunData {
    std::uint64_t       bytes = 0;
    double              wall  = 0;
    std::vector<double> latUs;
    std::uint64_t       hash = kFnvOffset;
    std::uint64_t       submitNs = 0;
    std::uint64_t       handoffNs = 0;
    std::uint64_t       queueNs = 0;
    std::uint64_t       readerCpuNs = 0;
    std::uint64_t       timingBlocks = 0;
};

struct LatencyStats {
    double minUs = 0;
    double p50Us = 0;
    double p95Us = 0;
    double p99Us = 0;
    double maxUs = 0;
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

struct Result {
    std::string       scheme;
    std::string       cache;
    unsigned          blockKiB = 0;
    unsigned          depth    = 0;
    int               run      = 0;
    std::uint64_t     bytes    = 0;
    double            wall     = 0;
    double            user     = 0;
    double            sys      = 0;
    double            cpu      = 0;
    double            tput     = 0;
    double            cpuPct   = 0;
    double            cpuPerGiB = 0;
    long              maxrss   = 0;
    long              minflt   = 0;
    long              majflt   = 0;
    long              nvcsw    = 0;
    long              nivcsw   = 0;
    LatencyStats      lat;
    std::vector<double> latUs;
    std::uint64_t     submitNs = 0;
    std::uint64_t     handoffNs = 0;
    std::uint64_t     queueNs = 0;
    std::uint64_t     readerCpuNs = 0;
    std::uint64_t     timingBlocks = 0;
};

struct Scheme {
    std::string name;
    unsigned    depth = 0;
    bool        uring = false;
};

std::uint64_t fnv1a(std::uint64_t h, const char* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        h ^= static_cast<unsigned char>(p[i]);
        h *= kFnvPrime;
    }
    return h;
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
    s.minUs = v.front();
    s.p50Us = percentile(v, 0.50);
    s.p95Us = percentile(v, 0.95);
    s.p99Us = percentile(v, 0.99);
    s.maxUs = v.back();
    return s;
}

double median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

std::string hexHash(std::uint64_t h) {
    std::ostringstream os;
    os << std::hex << std::setw(16) << std::setfill('0') << h;
    return os.str();
}

bool dropCaches() {
    ::sync();
    const int fd = ::open("/proc/sys/vm/drop_caches", O_WRONLY);
    if (fd < 0) return false;
    const ssize_t n = ::write(fd, "3\n", 2);
    ::close(fd);
    return n == 2;
}

std::uint64_t fileSize(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) return 0;
    return static_cast<std::uint64_t>(st.st_size);
}

bool makeFile(const std::string& path, std::uint64_t bytes) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    std::vector<char> buf(1 << 20);
    for (std::size_t i = 0; i < buf.size(); ++i) {
        buf[i] = static_cast<char>(i * 131 + 7);
    }
    std::uint64_t written = 0;
    bool          ok      = true;
    while (written < bytes) {
        const std::size_t want = static_cast<std::size_t>(
            std::min<std::uint64_t>(buf.size(), bytes - written));
        const ssize_t n = ::write(fd, buf.data(), want);
        if (n <= 0) {
            ok = false;
            break;
        }
        written += static_cast<std::uint64_t>(n);
    }
    ::close(fd);
    return ok && fileSize(path) == bytes;
}

bool prewarm(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    std::vector<char> buf(1 << 20);
    while (::read(fd, buf.data(), buf.size()) > 0) {
    }
    ::close(fd);
    return true;
}

RunData syncRead(const std::string& path, std::size_t block, bool doHash) {
    RunData r;
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return r;

    std::vector<char> buf(block);
    const auto t0 = Clock::now();
    for (;;) {
        const auto a = Clock::now();
        const ssize_t n = ::read(fd, buf.data(), block);
        const auto b = Clock::now();
        if (n <= 0) break;
        r.latUs.push_back(
            std::chrono::duration<double, std::micro>(b - a).count());
        r.bytes += static_cast<std::uint64_t>(n);
        if (doHash) {
            r.hash = fnv1a(r.hash, buf.data(), static_cast<std::size_t>(n));
        }
    }
    const auto t1 = Clock::now();
    r.wall = std::chrono::duration<double>(t1 - t0).count();
    ::close(fd);
    return r;
}

RunData uringRead(const std::string& path, std::size_t block, unsigned depth,
                  bool doHash, bool condvar) {
    RunData r;
    nas::AsyncFileReader reader(
        block, depth,
        condvar ? nas::Handoff::CondVar : nas::Handoff::Yield);
    if (!reader.valid() || !reader.start(path)) return r;

    const auto t0 = Clock::now();
    for (;;) {
        const auto a = Clock::now();
        nas::BufferPool::Buffer b = reader.next();
        const auto btime = Clock::now();
        if (!b) break;
        if (b.length() == 0) continue;
        r.latUs.push_back(
            std::chrono::duration<double, std::micro>(btime - a).count());
        r.bytes += b.length();
        if (doHash) r.hash = fnv1a(r.hash, b.data(), b.length());
    }
    const auto t1 = Clock::now();
    r.wall = std::chrono::duration<double>(t1 - t0).count();

    reader.waitFinished();
    const auto& timing = reader.timing();
    r.submitNs = timing.submitNs;
    r.handoffNs = timing.handoffNs;
    r.queueNs = timing.queueNs;
    r.readerCpuNs = timing.readerCpuNs;
    r.timingBlocks = timing.blocks;
    return r;
}

std::vector<unsigned> parseList(const std::string& s, unsigned lo,
                                unsigned hi, bool& ok) {
    std::vector<unsigned> out;
    std::size_t           i = 0;
    while (i < s.size()) {
        while (i < s.size() &&
               (s[i] == ',' || std::isspace(static_cast<unsigned char>(s[i])))) {
            ++i;
        }
        if (i >= s.size()) break;
        std::size_t j = i;
        while (j < s.size() && s[j] != ',' &&
               !std::isspace(static_cast<unsigned char>(s[j]))) {
            ++j;
        }
        const std::string tok = s.substr(i, j - i);
        char*             end = nullptr;
        const unsigned long v = std::strtoul(tok.c_str(), &end, 10);
        if (!end || *end != '\0' || v < lo || v > hi) {
            ok = false;
            return {};
        }
        out.push_back(static_cast<unsigned>(v));
        i = j;
    }
    if (out.empty()) ok = false;
    return out;
}

void printUsage(const char* argv0) {
    std::printf(
        "用法: %s [选项]\n"
        "  --file PATH         测试文件；默认 /tmp/nas_io_bench.bin\n"
        "  --size MiB          文件不存在时创建的大小，默认 128\n"
        "  --block KiB         单个块大小，默认 64\n"
        "  --blocks LIST       块大小列表，如 4,8,16,64,256,1024\n"
        "  --depth N           单个 io_uring QD\n"
        "  --depths LIST        QD 列表，默认 1,4,16,64\n"
        "  --repeat N          每个方案重复次数，默认 1\n"
        "  --warmup N          计时前预热次数，默认 0\n"
        "  --cache MODE        cold|hot|none，默认 none\n"
        "  --drop-cache        等价于 --cache cold\n"
        "  --mode MODE         sync|uring|both，默认 both\n"
        "  --handoff MODE      yield|condvar（io_uring 交接方式），默认 yield\n"
        "  --csv PATH          每个 run 输出一行 CSV\n"
        "  --verify            只做内容哈希校验（FNV-1a 64）\n"
        "  -h, --help          显示帮助\n",
        argv0);
}

bool parseOptions(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::printf("缺少参数: %s\n", name);
                return nullptr;
            }
            return argv[++i];
        };

        if (a == "--file") {
            const char* v = need("--file");
            if (!v) return false;
            o.file = v;
        } else if (a == "--size") {
            const char* v = need("--size");
            if (!v) return false;
            char* end = nullptr;
            const unsigned long long n = std::strtoull(v, &end, 10);
            if (!end || *end != '\0' || n == 0) {
                std::printf("非法 --size: %s\n", v);
                return false;
            }
            o.sizeMiB = n;
        } else if (a == "--block" || a == "--blocks") {
            const char* v = need(a.c_str());
            if (!v) return false;
            if (a == "--block") {
                char* end = nullptr;
                const unsigned long n = std::strtoul(v, &end, 10);
                if (!end || *end != '\0' || n < 1 || n > 65536) {
                    std::printf("非法 --block: %s\n", v);
                    return false;
                }
                o.blocksKiB = {static_cast<unsigned>(n)};
            } else {
                bool ok = true;
                auto list = parseList(v, 1, 65536, ok);
                if (!ok) {
                    std::printf("非法 --blocks: %s\n", v);
                    return false;
                }
                o.blocksKiB = list;
            }
        } else if (a == "--depth" || a == "--depths") {
            const char* v = need(a.c_str());
            if (!v) return false;
            if (a == "--depth") {
                char* end = nullptr;
                const unsigned long n = std::strtoul(v, &end, 10);
                if (!end || *end != '\0' || n < 1 || n > 1024) {
                    std::printf("非法 --depth: %s\n", v);
                    return false;
                }
                o.depths = {static_cast<unsigned>(n)};
            } else {
                bool ok = true;
                auto list = parseList(v, 1, 1024, ok);
                if (!ok) {
                    std::printf("非法 --depths: %s\n", v);
                    return false;
                }
                o.depths = list;
            }
        } else if (a == "--repeat") {
            const char* v = need("--repeat");
            if (!v) return false;
            char* end = nullptr;
            const long n = std::strtol(v, &end, 10);
            if (!end || *end != '\0' || n < 1 || n > 100000) {
                std::printf("非法 --repeat: %s\n", v);
                return false;
            }
            o.repeat = static_cast<int>(n);
        } else if (a == "--warmup") {
            const char* v = need("--warmup");
            if (!v) return false;
            char* end = nullptr;
            const long n = std::strtol(v, &end, 10);
            if (!end || *end != '\0' || n < 0 || n > 10000) {
                std::printf("非法 --warmup: %s\n", v);
                return false;
            }
            o.warmup = static_cast<int>(n);
        } else if (a == "--cache") {
            const char* v = need("--cache");
            if (!v) return false;
            o.cache = v;
            if (o.cache != "cold" && o.cache != "hot" && o.cache != "none") {
                std::printf("非法 --cache: %s（cold|hot|none）\n", v);
                return false;
            }
        } else if (a == "--drop-cache") {
            o.cache = "cold";
        } else if (a == "--mode") {
            const char* v = need("--mode");
            if (!v) return false;
            o.mode = v;
            if (o.mode != "sync" && o.mode != "uring" && o.mode != "both") {
                std::printf("非法 --mode: %s（sync|uring|both）\n", v);
                return false;
            }
        } else if (a == "--handoff") {
            const char* v = need("--handoff");
            if (!v) return false;
            o.handoff = v;
            if (o.handoff != "yield" && o.handoff != "condvar") {
                std::printf("非法 --handoff: %s（yield|condvar）\n", v);
                return false;
            }
        } else if (a == "--csv") {
            const char* v = need("--csv");
            if (!v) return false;
            o.csv = v;
        } else if (a == "--verify") {
            o.verify = true;
        } else if (a == "-h" || a == "--help") {
            printUsage(argv[0]);
            std::exit(0);
        } else {
            std::printf("未知参数: %s\n", a.c_str());
            return false;
        }
    }
    return true;
}

void writeCsvHeader(std::ofstream& csv) {
    csv << "scheme,cache,block_kib,depth,run,bytes,wall_s,throughput_mib_s,"
           "user_s,sys_s,cpu_s,cpu_pct,cpu_s_per_gib,maxrss_kib,minflt,majflt,"
           "nvcsw,nivcsw,p50_us,p95_us,p99_us,"
           "submit_ms,handoff_ms,queue_ms,reader_cpu_s,consumer_cpu_s\n";
}

void writeCsvRow(std::ofstream& csv, const Result& r) {
    csv << r.scheme << ',' << r.cache << ',' << r.blockKiB << ',' << r.depth
        << ',' << r.run << ',' << r.bytes << ',' << std::fixed
        << std::setprecision(6) << r.wall << ',' << r.tput << ',' << r.user
        << ',' << r.sys << ',' << r.cpu << ',' << r.cpuPct << ','
        << r.cpuPerGiB << ',' << r.maxrss << ',' << r.minflt << ','
        << r.majflt << ',' << r.nvcsw << ',' << r.nivcsw << ','
        << r.lat.p50Us << ',' << r.lat.p95Us << ',' << r.lat.p99Us << ','
        << r.submitNs / 1e6 << ',' << r.handoffNs / 1e6 << ','
        << r.queueNs / 1e6 << ',' << r.readerCpuNs / 1e9 << ','
        << std::max(0.0, r.cpu - static_cast<double>(r.readerCpuNs) / 1e9)
        << '\n';
}

bool runScheme(const Options& o, const Scheme& s, std::size_t block,
               unsigned blockKiB, std::ofstream* csv) {
    std::vector<Result> runs;

    for (int w = 0; w < o.warmup; ++w) {
        if (o.cache == "cold" && !dropCaches()) {
            std::printf("drop_caches 失败（需要 root）\n");
            return false;
        }
        const RunData d =
            s.uring ? uringRead(o.file, block, s.depth, false,
                                o.handoff == "condvar")
                    : syncRead(o.file, block, false);
        if (d.bytes == 0) {
            std::printf("预热读取失败: %s\n", s.name.c_str());
            return false;
        }
    }

    for (int n = 1; n <= o.repeat; ++n) {
        if (o.cache == "cold" && !dropCaches()) {
            std::printf("drop_caches 失败（需要 root）\n");
            return false;
        }

        rusage before {};
        rusage after {};
        ::getrusage(RUSAGE_SELF, &before);
        RunData d =
            s.uring ? uringRead(o.file, block, s.depth, false,
                                o.handoff == "condvar")
                    : syncRead(o.file, block, false);
        ::getrusage(RUSAGE_SELF, &after);

        if (d.bytes == 0 || d.wall <= 0.0) {
            std::printf("读取失败: %s\n", s.name.c_str());
            continue;
        }

        const CpuDelta cpu = cpuDelta(before, after);
        Result r;
        r.scheme     = s.name;
        r.cache      = o.cache;
        r.blockKiB   = blockKiB;
        r.depth      = s.uring ? s.depth : 0;
        r.run        = n;
        r.bytes      = d.bytes;
        r.wall       = d.wall;
        r.user       = cpu.user;
        r.sys        = cpu.sys;
        r.cpu        = cpu.user + cpu.sys;
        r.tput       = static_cast<double>(d.bytes) / kMiB / d.wall;
        r.cpuPct     = r.cpu / d.wall * 100.0;
        r.cpuPerGiB  = r.cpu / (static_cast<double>(d.bytes) / kGiB);
        r.maxrss     = cpu.maxrss;
        r.minflt     = cpu.minflt;
        r.majflt     = cpu.majflt;
        r.nvcsw      = cpu.nvcsw;
        r.nivcsw     = cpu.nivcsw;
        r.latUs      = std::move(d.latUs);
        r.lat        = latencyStats(r.latUs);
        r.submitNs   = d.submitNs;
        r.handoffNs  = d.handoffNs;
        r.queueNs    = d.queueNs;
        r.readerCpuNs = d.readerCpuNs;
        r.timingBlocks = d.timingBlocks;
        runs.push_back(r);
        if (csv) writeCsvRow(*csv, r);
    }

    if (runs.empty()) return false;

    std::vector<double> tputs;
    std::vector<double> cpus;
    std::vector<double> cpugib;
    std::vector<double> lat;
    for (const Result& r : runs) {
        tputs.push_back(r.tput);
        cpus.push_back(r.cpuPct);
        cpugib.push_back(r.cpuPerGiB);
        lat.insert(lat.end(), r.latUs.begin(), r.latUs.end());
    }
    const LatencyStats ls = latencyStats(lat);

    std::printf(
        "  %-14s median %8.1f  min %8.1f  max %8.1f MiB/s  "
        "CPU %6.1f%%  %7.2f cpu-s/GiB  "
        "p50 %7.1f us  p95 %7.1f  p99 %7.1f\n",
        s.name.c_str(), median(tputs), *std::min_element(tputs.begin(), tputs.end()),
        *std::max_element(tputs.begin(), tputs.end()), median(cpus),
        median(cpugib), ls.p50Us, ls.p95Us, ls.p99Us);
    return true;
}

bool verifySchemes(const Options& o, const std::vector<Scheme>& schemes) {
    bool ok = true;
    std::uint64_t reference = 0;
    bool          haveRef   = false;

    for (unsigned bki : o.blocksKiB) {
        const std::size_t block = static_cast<std::size_t>(bki) * 1024;
        for (const Scheme& s : schemes) {
            const RunData d =
                s.uring ? uringRead(o.file, block, s.depth, true,
                                    o.handoff == "condvar")
                        : syncRead(o.file, block, true);
            if (d.bytes == 0) {
                std::printf("VERIFY %s block=%u FAILED bytes=0\n",
                            s.name.c_str(), bki);
                ok = false;
                continue;
            }
            std::printf("VERIFY %s block=%u hash=%s bytes=%llu\n",
                        s.name.c_str(), bki, hexHash(d.hash).c_str(),
                        static_cast<unsigned long long>(d.bytes));
            if (!haveRef) {
                reference = d.hash;
                haveRef   = true;
            } else if (d.hash != reference) {
                std::printf("VERIFY MISMATCH %s block=%u\n", s.name.c_str(), bki);
                ok = false;
            }
        }
    }
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    ::prctl(PR_SET_NAME, "io-consumer", 0, 0, 0);
    Options o;
    if (!parseOptions(argc, argv, o)) {
        printUsage(argv[0]);
        return 1;
    }

    if (o.file.empty()) o.file = "/tmp/nas_io_bench.bin";
    if (fileSize(o.file) == 0) {
        if (!makeFile(o.file, o.sizeMiB * 1024ULL * 1024ULL)) {
            std::printf("无法创建测试文件: %s\n", o.file.c_str());
            return 1;
        }
    }
    const std::uint64_t fsize = fileSize(o.file);
    if (fsize == 0) {
        std::printf("文件不存在或为空: %s\n", o.file.c_str());
        return 1;
    }

    bool haveUring = false;
    if (o.mode != "sync") {
        nas::IoUring probe;
        haveUring = probe.valid();
        if (!haveUring) {
            std::printf("io_uring 不可用: errno=%d (%s)\n", probe.initErrno(),
                        std::strerror(probe.initErrno()));
            if (o.mode == "uring") return 1;
        }
    }

    std::vector<Scheme> schemes;
    if (o.mode != "uring") schemes.push_back({"sync", 0, false});
    if (o.mode != "sync" && haveUring) {
        for (unsigned d : o.depths) {
            schemes.push_back({"uring_qd" + std::to_string(d), d, true});
        }
    }
    if (schemes.empty()) {
        std::printf("没有可用方案\n");
        return 1;
    }

    std::printf("== I/O 基准 ==\n");
    std::printf("file=%s size=%.1f MiB cache=%s repeat=%d warmup=%d\n\n",
                o.file.c_str(), static_cast<double>(fsize) / kMiB,
                o.cache.c_str(), o.repeat, o.warmup);

    if (o.verify) return verifySchemes(o, schemes) ? 0 : 1;

    if (o.cache == "cold") {
        if (!dropCaches()) {
            std::printf("drop_caches 失败（需要 root）\n");
            return 1;
        }
    } else if (o.cache == "hot") {
        std::printf("预热 page cache ...\n");
        if (!prewarm(o.file)) {
            std::printf("预热失败: %s\n", o.file.c_str());
            return 1;
        }
    }

    std::ofstream csvFile;
    std::ofstream* csv = nullptr;
    if (!o.csv.empty()) {
        csvFile.open(o.csv, std::ios::out | std::ios::trunc);
        if (!csvFile) {
            std::printf("无法写 CSV: %s\n", o.csv.c_str());
            return 1;
        }
        writeCsvHeader(csvFile);
        csv = &csvFile;
    }

    bool ok = true;
    for (unsigned bki : o.blocksKiB) {
        const std::size_t block = static_cast<std::size_t>(bki) * 1024;
        if (block == 0) continue;
        std::printf("-- block=%u KiB --\n", bki);
        for (const Scheme& s : schemes) {
            if (!runScheme(o, s, block, bki, csv)) ok = false;
        }
        std::printf("\n");
    }
    return ok ? 0 : 1;
}

#else

int main() {
    std::printf("io_bench 未启用：需要 Linux + liburing。\n");
    return 1;
}

#endif
