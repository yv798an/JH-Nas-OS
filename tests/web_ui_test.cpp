// WebUi 路由测试：GET / 返回 HTML，GET /api/roots 返回白名单根。

#include "http/Router.h"
#include "http/WebUi.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace {

std::atomic<int> g_failed{0};

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failed.fetch_add(1, std::memory_order_relaxed);              \
        }                                                                  \
    } while (0)

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

nas::HttpRequest req(const char* method, const char* path) {
    nas::HttpRequest r;
    r.method = method;
    r.path   = path;
    return r;
}

}  // namespace

int main() {
    std::printf("== WebUi 测试 ==\n");

    nas::Router router;
    nas::registerWebRoutes(router, {"/media", "/srv/nas"});

    {
        std::printf("[1] GET /\n");
        const auto res = router.dispatch(req("GET", "/"));
        CHECK(res.status == 200);
        const auto it = res.headers.find("content-type");
        CHECK(it != res.headers.end());
        CHECK(it != res.headers.end() &&
              contains(it->second, "text/html"));
        CHECK(contains(res.body, "<!DOCTYPE html>"));
        CHECK(contains(res.body, "/api/files"));
        CHECK(contains(res.body, "/api/download"));
    }

    {
        std::printf("[2] GET /index.html\n");
        const auto res = router.dispatch(req("GET", "/index.html"));
        CHECK(res.status == 200);
    }

    {
        std::printf("[3] GET /api/roots\n");
        const auto res = router.dispatch(req("GET", "/api/roots"));
        CHECK(res.status == 200);
        CHECK(contains(res.body, "\"/media\""));
        CHECK(contains(res.body, "\"/srv/nas\""));
    }

    {
        std::printf("[4] 外部 web.dir 覆盖内嵌 UI\n");
        const auto n = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::filesystem::path dir =
            std::filesystem::temp_directory_path() /
            ("nas_web_test_" + std::to_string(n));
        std::filesystem::create_directories(dir);
        {
            std::ofstream f(dir / "index.html", std::ios::binary);
            f << "<html>EXTERNAL-UI-MARKER</html>";
        }

        nas::Router web;
        nas::registerWebRoutes(web, {"/media"}, dir.string());
        const auto res = web.dispatch(req("GET", "/"));
        CHECK(res.status == 200);
        CHECK(contains(res.body, "EXTERNAL-UI-MARKER"));

        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    const int failed = g_failed.load();
    if (failed == 0) {
        std::printf("== 全部通过 ==\n");
        return 0;
    }
    std::printf("== 失败: %d 项 ==\n", failed);
    return 1;
}
