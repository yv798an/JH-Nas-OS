// HTTP 下载压测客户端（用 vendored cpp-httplib 的 Client）。
//
// 用法：
//   http_bench [--url URL] [--threads N] [--rounds R]
// 默认: http://127.0.0.1:8080/api/download?path=/tmp/nas_io_bench.bin
//
// 用于对照服务端 io.mode：同一 URL，分别在 sync / io_uring 下跑，比吞吐。

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Url {
    std::string host;
    int         port = 80;
    std::string path = "/";
};

Url parseUrl(const std::string& url) {
    Url         u;
    std::string s    = url;
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

}  // namespace

int main(int argc, char** argv) {
    std::string url =
        "http://127.0.0.1:8080/api/download?path=/tmp/nas_io_bench.bin";
    int threads = 8;
    int rounds  = 1;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--url" && i + 1 < argc) {
            url = argv[++i];
        } else if (a == "--threads" && i + 1 < argc) {
            threads = std::atoi(argv[++i]);
        } else if (a == "--rounds" && i + 1 < argc) {
            rounds = std::atoi(argv[++i]);
        }
    }
    if (threads < 1) threads = 1;
    if (rounds < 1) rounds = 1;

    const Url u = parseUrl(url);
    std::atomic<std::uint64_t> totalBytes{0};
    std::atomic<int>           errors{0};

    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> ts;
    ts.reserve(static_cast<std::size_t>(threads));
    for (int i = 0; i < threads; ++i) {
        ts.emplace_back([&] {
            httplib::Client cli(u.host, u.port);
            cli.set_read_timeout(120, 0);
            cli.set_keep_alive(true);
            for (int r = 0; r < rounds; ++r) {
                std::uint64_t local = 0;
                auto res = cli.Get(u.path, [&](const char*, std::size_t len) {
                    local += len;
                    return true;
                });
                if (!res) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                totalBytes.fetch_add(local, std::memory_order_relaxed);
            }
        });
    }
    for (auto& t : ts) t.join();
    const auto t1 = std::chrono::steady_clock::now();

    const double        secs  = std::chrono::duration<double>(t1 - t0).count();
    const std::uint64_t bytes = totalBytes.load();

    std::printf("url=%s\n", url.c_str());
    std::printf("threads=%d rounds=%d\n", threads, rounds);
    std::printf("bytes=%llu  time=%.3f s  throughput=%.1f MiB/s  errors=%d\n",
                static_cast<unsigned long long>(bytes), secs,
                static_cast<double>(bytes) / 1048576.0 / (secs > 0 ? secs : 1.0),
                errors.load());
    return 0;
}
