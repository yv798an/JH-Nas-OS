// Config 测试：默认值、键值解析、注释/空白、优先级、非法值报错。

#include "config/Config.h"

#include <atomic>
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

void testDefaults() {
    std::printf("[1] 默认值\n");
    nas::Config c;
    CHECK(c.httpHost == "0.0.0.0");
    CHECK(c.httpPort == 8080);
    CHECK(c.shareRoots.size() == 1 && c.shareRoots[0] == "/media");
    CHECK(c.logLevel == "INFO");
    CHECK(c.pollMs == 1000);
}

void testParse() {
    std::printf("[2] 解析与注释/空白\n");
    nas::Config c;
    std::string err;
    const std::string text =
        "# 注释行\n"
        "\n"
        "http.host = 127.0.0.1   # 行尾注释\n"
        "  http.port=9000 \n"
        "share.roots = /media, /mnt/usb ,/srv\n"
        "log.level = debug\n"
        "web.dir = /usr/share/nas/web\n"
        "io.uring = true\n"
        "io.depth = 16\n"
        "monitor.pollMs = 250\n";
    CHECK(c.loadFromString(text, err));
    CHECK(c.httpHost == "127.0.0.1");
    CHECK(c.httpPort == 9000);
    CHECK(c.shareRoots.size() == 3);
    CHECK(c.shareRoots[0] == "/media");
    CHECK(c.shareRoots[1] == "/mnt/usb");
    CHECK(c.shareRoots[2] == "/srv");
    CHECK(c.logLevel == "DEBUG");  // 归一化为大写
    CHECK(c.webDir == "/usr/share/nas/web");
    CHECK(c.ioUseUring == true);
    CHECK(c.ioDepth == 16);
    CHECK(c.pollMs == 250);
}

void testPrecedence() {
    std::printf("[3] 优先级：文件后可被命令行覆盖\n");
    nas::Config c;
    std::string err;
    CHECK(c.loadFromString("http.port = 9000\n", err));
    CHECK(c.httpPort == 9000);
    // 模拟命令行覆盖
    CHECK(c.set("http.port", "9100", err));
    CHECK(c.httpPort == 9100);
}

void testInvalid() {
    std::printf("[4] 非法值\n");
    nas::Config c;
    std::string err;

    CHECK(!c.set("http.port", "0", err));
    CHECK(!c.set("http.port", "70000", err));
    CHECK(!c.set("http.port", "abc", err));
    CHECK(!c.set("log.level", "LOUD", err));
    CHECK(!c.set("monitor.pollMs", "-5", err));
    CHECK(c.set("share.roots", "", err));  // 空列表合法
    CHECK(c.shareRoots.empty());
    CHECK(!c.set("no.such.key", "x", err));

    // 行号应出现在错误里
    std::string e2;
    CHECK(!c.loadFromString("http.port = 8080\nhttp.port = bad\n", e2));
    CHECK(e2.find("第 2 行") != std::string::npos);
}

void testMissingEquals() {
    std::printf("[5] 缺少 '='\n");
    nas::Config c;
    std::string err;
    CHECK(!c.loadFromString("http.port 8080\n", err));
    CHECK(err.find("第 1 行") != std::string::npos);
}

}  // namespace

int main() {
    std::printf("== Config 测试 ==\n");
    testDefaults();
    testParse();
    testPrecedence();
    testInvalid();
    testMissingEquals();

    const int failed = g_failed.load();
    if (failed == 0) {
        std::printf("== 全部通过 ==\n");
        return 0;
    }
    std::printf("== 失败: %d 项 ==\n", failed);
    return 1;
}
