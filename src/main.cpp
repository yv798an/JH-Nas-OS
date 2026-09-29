// nas-server 入口。
//   nas-server [--config PATH] [--host H] [--port P]      阶段 0：日志 + /proc
//   nas-server --dump-system [--config PATH]              采集一次系统状态打印 JSON（仅 Linux）
//   nas-server --serve [--host H] [--port P]              启动 HTTP 服务（仅 Linux）
#include "common/Logger.h"
#include "config/Config.h"
#include "file/FileService.h"
#include "http/Api.h"
#include "http/FileApi.h"
#include "http/HealthProbe.h"
#include "http/HttpServerFactory.h"
#include "http/Router.h"
#include "http/WebUi.h"
#include "platform/linux/LinuxSystemState.h"
#include "system/ShareRegistry.h"
#include "system/SystemMonitor.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr const char* kDefaultConfigPath = "/etc/nas/nas.conf";

// 信号处理：收到 SIGINT/SIGTERM 时停止 accept 循环，让 listen() 返回，
// 进而走完 state.stop() / Logger::shutdown() 的优雅收尾。
// httplib 的 stop() 只做原子标志 + shutdown(socket)，可安全地在信号处理器中调用。
#if defined(__linux__)
nas::HttpServer* g_server = nullptr;

extern "C" void handleTerminate(int) {
    if (g_server) g_server->stop();
}
#endif

nas::LogLevel toLogLevel(const std::string& level) {
    if (level == "TRACE") return nas::LogLevel::TRACE;
    if (level == "DEBUG") return nas::LogLevel::DEBUG;
    if (level == "INFO")  return nas::LogLevel::INFO;
    if (level == "WARN")  return nas::LogLevel::WARN;
    if (level == "ERROR") return nas::LogLevel::ERROR;
    if (level == "FATAL") return nas::LogLevel::FATAL;
    return nas::LogLevel::OFF;
}

void printUsage(const char* argv0) {
    std::printf(
        "用法: %s [选项]\n"
        "  --serve              启动 HTTP 服务（仅 Linux）\n"
        "  --dump-system        采集一次系统状态并打印 JSON（仅 Linux）\n"
        "  --health-check       对本地服务做一次 /api/health 探活，健康退出 0\n"
        "  --config PATH        配置文件（默认 %s）\n"
        "  --host H             覆盖 http.host\n"
        "  --port P             覆盖 http.port\n"
        "  --help               显示帮助\n"
        "无选项时进入阶段 0：写日志 + 读 /proc/loadavg。\n",
        argv0, kDefaultConfigPath);
}

int runDumpSystem(const nas::Config& cfg) {
#if defined(__linux__)
    nas::LinuxSystemState state{std::chrono::milliseconds(cfg.pollMs)};
    state.start();
    // CPU% 需要两次 /proc/stat 采样求差，等一个轮询周期再取报告。
    std::this_thread::sleep_for(std::chrono::milliseconds(cfg.pollMs + 200));

    nas::ShareRegistry registry(cfg.shareRoots);
    nas::SystemMonitor monitor(state, std::move(registry));

    const std::string json = nas::toJson(monitor.report());
    std::printf("%s\n", json.c_str());

    state.stop();
    return 0;
#else
    (void)cfg;
    std::printf("--dump-system 仅支持 Linux。\n");
    return 1;
#endif
}

int runServe(const nas::Config& cfg) {
#if defined(__linux__)
    nas::Logger::init(cfg.logFile, toLogLevel(cfg.logLevel));

    nas::LinuxSystemState state{std::chrono::milliseconds(cfg.pollMs)};
    state.start();

    nas::ShareRegistry registry(cfg.shareRoots);
    nas::SystemMonitor monitor(state, std::move(registry));

    nas::FileService files(cfg.shareRoots);

    nas::DownloadIoOptions io;
    io.useUring = cfg.ioUseUring;
    io.blockSize = static_cast<std::size_t>(cfg.ioBlockKb) * 1024;
    io.depth = static_cast<unsigned>(cfg.ioDepth);
    io.thresholdBytes = static_cast<std::uint64_t>(cfg.ioThresholdKb) * 1024;

    nas::Router router;
    nas::registerApiRoutes(router, monitor);
    nas::registerFileRoutes(router, files, io);
    nas::registerWebRoutes(router, cfg.shareRoots, cfg.webDir);

    auto server = nas::makeHttpServer();
    server->setHandler([&router](const nas::HttpRequest& req) {
        return router.dispatch(req);
    });

    g_server = server.get();
    std::signal(SIGINT, handleTerminate);
    std::signal(SIGTERM, handleTerminate);

    LOG_INFO("serve start on %s:%d (roots=%u)",
             cfg.httpHost.c_str(), cfg.httpPort,
             static_cast<unsigned>(cfg.shareRoots.size()));
#if defined(NAS_HAS_IO_URING)
    LOG_INFO("download io_uring: compiled=yes requested=%d depth=%d",
             cfg.ioUseUring ? 1 : 0, cfg.ioDepth);
#else
    LOG_INFO("download io_uring: compiled=no requested=%d",
             cfg.ioUseUring ? 1 : 0);
#endif
    std::printf("nas-server listening on http://%s:%d\n",
                cfg.httpHost.c_str(), cfg.httpPort);
    std::fflush(stdout);

    const bool ok = server->listen(cfg.httpHost, cfg.httpPort);
    g_server = nullptr;
    std::printf("nas-server stopped.\n");
    state.stop();
    LOG_INFO("serve stopped");
    nas::Logger::shutdown();
    return ok ? 0 : 1;
#else
    (void)cfg;
    std::printf("--serve 仅支持 Linux。\n");
    return 1;
#endif
}

// 探活：对配置里的 http.host:http.port 发一次 GET /api/health。
// 健康退出 0、否则退出 1，供镜像 watchdog 的 test-binary 周期调用
// （见 docs/image-integration.md 看门狗探活契约）。
int runHealthCheck(const nas::Config& cfg) {
    const bool ok = nas::probeHealth(cfg.httpHost, cfg.httpPort, 3000);
    std::printf("health-check %s:%d: %s\n", cfg.httpHost.c_str(),
                cfg.httpPort, ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

int runStage0(const nas::Config& cfg) {
    nas::Logger::init(cfg.logFile, toLogLevel(cfg.logLevel));
    LOG_INFO("nas-server starting (http=%s:%d, roots=%u)",
             cfg.httpHost.c_str(), cfg.httpPort,
             static_cast<unsigned>(cfg.shareRoots.size()));

    std::ifstream f("/proc/loadavg");
    if (f) {
        std::string line;
        std::getline(f, line);
        LOG_INFO("/proc/loadavg: %s", line.c_str());
    } else {
        LOG_WARN("cannot read /proc/loadavg");
    }

    nas::Logger::shutdown();
    std::printf("nas-server: log written to %s, dropped=%llu\n",
                cfg.logFile.c_str(),
                static_cast<unsigned long long>(nas::Logger::droppedCount()));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string configPath = kDefaultConfigPath;
    bool        configExplicit = false;
    std::string mode = "stage0";
    std::vector<std::pair<std::string, std::string>> overrides;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--serve") {
            mode = "serve";
        } else if (a == "--dump-system") {
            mode = "dump-system";
        } else if (a == "--health-check") {
            mode = "health-check";
        } else if (a == "--help" || a == "-h") {
            printUsage(argv[0]);
            return 0;
        } else if (a == "--config" && i + 1 < argc) {
            configPath     = argv[++i];
            configExplicit = true;
        } else if (a == "--host" && i + 1 < argc) {
            overrides.push_back({"http.host", argv[++i]});
        } else if (a == "--port" && i + 1 < argc) {
            overrides.push_back({"http.port", argv[++i]});
        } else {
            std::fprintf(stderr, "未知参数: %s（--help 查看用法）\n", a.c_str());
            return 2;
        }
    }

    nas::Config  config;
    std::string  error;

    std::ifstream probe(configPath);
    if (probe.good()) {
        if (!config.loadFile(configPath, error)) {
            std::fprintf(stderr, "配置错误: %s\n", error.c_str());
            return 1;
        }
    } else if (configExplicit) {
        std::fprintf(stderr, "找不到配置文件: %s\n", configPath.c_str());
        return 1;
    }
    // 默认路径不存在时使用内置默认（开发友好）。

    for (const auto& kv : overrides) {
        if (!config.set(kv.first, kv.second, error)) {
            std::fprintf(stderr, "参数错误: %s\n", error.c_str());
            return 1;
        }
    }

    if (mode == "serve") return runServe(config);
    if (mode == "dump-system") return runDumpSystem(config);
    if (mode == "health-check") return runHealthCheck(config);
    return runStage0(config);
}
