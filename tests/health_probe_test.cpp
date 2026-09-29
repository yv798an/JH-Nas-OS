// HealthProbe 测试：起一个真实 httplib 服务，验证
//   * 服务在线时探活成功；
//   * 通配监听地址（0.0.0.0）被正确映射到回环；
//   * 服务停止 / 端口无监听时探活失败。

#include "http/HealthProbe.h"

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

namespace {

std::atomic<int> g_failed{0};

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failed.fetch_add(1, std::memory_order_relaxed);              \
        }                                                                  \
    } while (0)

}  // namespace

int main() {
    std::printf("== HealthProbe 测试 ==\n");

    // [1] 无监听端口：应判定不健康。
    std::printf("[1] 关闭端口 -> FAIL\n");
    CHECK(!nas::probeHealth("127.0.0.1", 1, 500));

    // [2] 起真实服务并探活。
    std::printf("[2] 服务在线 -> OK\n");
    httplib::Server server;
    server.Get("/api/health",
               [](const httplib::Request&, httplib::Response& res) {
                   res.status = 200;
                   res.set_content("{\"status\":\"ok\"}",
                                   "application/json; charset=utf-8");
               });
    const int port = server.bind_to_any_port("127.0.0.1");
    CHECK(port > 0);

    std::thread runner([&server] { server.listen_after_bind(); });

    bool ok = false;
    for (int i = 0; i < 50 && !ok; ++i) {
        ok = nas::probeHealth("127.0.0.1", port, 1000);
        if (!ok) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(ok);

    // [3] 通配监听地址 0.0.0.0 应被映射到 127.0.0.1。
    std::printf("[3] 通配地址映射 -> OK\n");
    CHECK(nas::probeHealth("0.0.0.0", port, 1000));

    server.stop();
    if (runner.joinable()) runner.join();

    // [4] 停服后：应判定不健康。
    std::printf("[4] 停服 -> FAIL\n");
    CHECK(!nas::probeHealth("127.0.0.1", port, 1000));

    const int failed = g_failed.load();
    if (failed == 0) {
        std::printf("== 全部通过 ==\n");
        return 0;
    }
    std::printf("== 失败: %d 项 ==\n", failed);
    return 1;
}
