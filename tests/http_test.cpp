// HTTP 抽象层测试：Router 匹配 + API 路由（不启动真实 socket）。
//
// 用 FakeSystemState 注入数据，验证 /api/health、/api/system、/api/metrics、
// 404、405 以及 JSON 内容。

#include "fakes/FakeSystemState.h"
#include "http/Api.h"
#include "http/Router.h"
#include "system/SystemMonitor.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

std::atomic<int> g_failed{0};

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failed.fetch_add(1, std::memory_order_relaxed);              \
        }                                                                  \
    } while (0)

nas::HttpRequest req(const char* method, const char* path) {
    nas::HttpRequest r;
    r.method = method;
    r.path   = path;
    return r;
}

nas::MountPoint mount(const char* dev, const char* path) {
    nas::MountPoint mp;
    mp.device    = dev;
    mp.mountPath = path;
    mp.fstype    = "vfat";
    mp.writable  = true;
    return mp;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

bool hasJsonContentType(const nas::HttpResponse& res) {
    const auto it = res.headers.find("content-type");
    return it != res.headers.end() && contains(it->second, "application/json");
}

void testHealthAndSystem() {
    std::printf("[1] /api/health 与 /api/system\n");
    nas::FakeSystemState state;
    state.setUptime(42.0);
    state.setMounts({mount("/dev/sda1", "/media/sda1")});
    state.setDiskUsage("/media/sda1", nas::DiskUsage{1000, 400, true});
    state.publish();

    nas::SystemMonitor monitor(state);
    nas::Router        router;
    nas::registerApiRoutes(router, monitor);

    const nas::HttpResponse health = router.dispatch(req("GET", "/api/health"));
    CHECK(health.status == 200);
    CHECK(hasJsonContentType(health));
    CHECK(contains(health.body, "\"status\":\"ok\""));
    CHECK(contains(health.body, "\"uptimeSec\":42.0"));

    const nas::HttpResponse system = router.dispatch(req("GET", "/api/system"));
    CHECK(system.status == 200);
    CHECK(hasJsonContentType(system));
    CHECK(contains(system.body, "\"name\":\"sda1\""));
    CHECK(contains(system.body, "\"uptimeSec\":42.0"));
    std::printf("  /api/system: %s\n", system.body.c_str());

    const nas::HttpResponse metrics =
        router.dispatch(req("GET", "/api/metrics"));
    CHECK(metrics.status == 200);
    CHECK(contains(metrics.body, "nas_cpu_percent"));
    CHECK(contains(metrics.body, "nas_mem_total_bytes"));
    CHECK(contains(metrics.body, "nas_share_used_bytes"));
}

void testErrors() {
    std::printf("[2] 404 / 405\n");
    nas::FakeSystemState state;
    state.publish();
    nas::SystemMonitor monitor(state);
    nas::Router        router;
    nas::registerApiRoutes(router, monitor);

    const nas::HttpResponse notFound = router.dispatch(req("GET", "/nope"));
    CHECK(notFound.status == 404);
    CHECK(contains(notFound.body, "\"error\":\"not found\""));

    const nas::HttpResponse wrongMethod =
        router.dispatch(req("POST", "/api/system"));
    CHECK(wrongMethod.status == 405);
    CHECK(contains(wrongMethod.body, "\"error\":\"method not allowed\""));
}

}  // namespace

int main() {
    std::printf("== HTTP 测试 ==\n");
    testHealthAndSystem();
    testErrors();

    const int failed = g_failed.load();
    if (failed == 0) {
        std::printf("== 全部通过 ==\n");
        return 0;
    }
    std::printf("== 失败: %d 项 ==\n", failed);
    return 1;
}
