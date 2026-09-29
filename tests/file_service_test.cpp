// FileService 测试：目录浏览、路径安全（越界/符号链接）、增删改、写文件。
// 使用系统临时目录，验证后清理。

#include "file/FileService.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

std::atomic<int> g_failed{0};

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failed.fetch_add(1, std::memory_order_relaxed);              \
        }                                                                  \
    } while (0)

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

fs::path makeTempRoot() {
    const auto n = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() /
           ("nas_file_test_" + std::to_string(n));
}

void testList(const std::string& root) {
    std::printf("[1] 目录浏览\n");
    nas::FileService svc({root});

    std::vector<nas::FileEntry> entries;
    std::string message;
    CHECK(svc.list(root, entries, message) == nas::FileError::None);
    // root 下有 a.txt、dir1、dir2
    CHECK(entries.size() == 3);
    CHECK(entries[0].isDir);              // 目录排前
    CHECK(!entries.back().isDir);         // 文件在后
    CHECK(entries.back().name == "a.txt");
    CHECK(entries.back().size == 5);
}

void testPathSafety(const std::string& root) {
    std::printf("[2] 路径安全\n");
    nas::FileService svc({root});
    std::string canon, message;

    CHECK(svc.resolve(root + "/a.txt", canon, message) == nas::FileError::None);
    CHECK(svc.resolve(root + "/../etc", canon, message) == nas::FileError::Forbidden);
    CHECK(svc.resolve("/etc/passwd", canon, message) == nas::FileError::Forbidden);
    CHECK(svc.resolve("/", canon, message) == nas::FileError::Forbidden);
    CHECK(svc.resolve("relative/path", canon, message) == nas::FileError::Invalid);
    CHECK(svc.resolve("", canon, message) == nas::FileError::Invalid);

#if defined(__linux__) || defined(__APPLE__)
    // 符号链接越界：root/evil -> /etc，解析后应被拒
    std::error_code ec;
    fs::create_symlink("/etc", fs::path(root) / "evil", ec);
    if (!ec) {
        CHECK(svc.resolve(root + "/evil/passwd", canon, message) ==
              nas::FileError::Forbidden);
    }
#endif
}

void testMutations(const std::string& root) {
    std::printf("[3] 增删改\n");
    nas::FileService svc({root});
    std::string message;

    // mkdir
    CHECK(svc.createDir(root + "/sub", message) == nas::FileError::None);
    nas::FileEntry e;
    CHECK(svc.stat(root + "/sub", e, message) == nas::FileError::None);
    CHECK(e.isDir);

    // writeFile + 读回
    const std::string content = "hello nas";
    CHECK(svc.writeFile(root + "/sub/x.bin", content.data(), content.size(),
                        message) == nas::FileError::None);
    CHECK(readFile(fs::path(root) / "sub" / "x.bin") == content);

    // rename
    CHECK(svc.rename(root + "/sub/x.bin", root + "/sub/y.bin", message) ==
          nas::FileError::None);
    CHECK(!fs::exists(fs::path(root) / "sub" / "x.bin"));
    CHECK(fs::exists(fs::path(root) / "sub" / "y.bin"));

    // remove file
    CHECK(svc.remove(root + "/sub/y.bin", false, message) == nas::FileError::None);
    CHECK(!fs::exists(fs::path(root) / "sub" / "y.bin"));

    // remove dir (recursive)
    CHECK(svc.remove(root + "/sub", true, message) == nas::FileError::None);
    CHECK(!fs::exists(fs::path(root) / "sub"));

    // 不允许操作共享根
    CHECK(svc.remove(root, true, message) == nas::FileError::Forbidden);
}

}  // namespace

int main() {
    std::printf("== FileService 测试 ==\n");

    const fs::path root = makeTempRoot();
    fs::create_directories(fs::path(root) / "dir1");
    fs::create_directories(fs::path(root) / "dir2");
    {
        std::ofstream f(fs::path(root) / "a.txt", std::ios::binary);
        f << "hello";
    }
    const std::string rootStr = fs::weakly_canonical(root).string();

    testList(rootStr);
    testPathSafety(rootStr);
    testMutations(rootStr);

    std::error_code ec;
    fs::remove_all(root, ec);

    const int failed = g_failed.load();
    if (failed == 0) {
        std::printf("== 全部通过 ==\n");
        return 0;
    }
    std::printf("== 失败: %d 项 ==\n", failed);
    return 1;
}
