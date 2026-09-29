// FileApi 测试：路由匹配、浏览/下载(Range)/上传/建目录/重命名/删除，
// 以及路径越界的 403。使用系统临时目录。

#include "file/FileService.h"
#include "http/FileApi.h"
#include "http/Router.h"

#include <atomic>
#include <chrono>
#include <cstdint>
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

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

nas::HttpRequest req(const char* method, const char* path) {
    nas::HttpRequest r;
    r.method = method;
    r.path   = path;
    return r;
}

class FakeSink final : public nas::BodySink {
public:
    std::string data;
    bool write(const char* d, std::size_t len) override {
        data.append(d, len);
        return true;
    }
};

fs::path makeTempRoot() {
    const auto n = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() / ("nas_api_test_" + std::to_string(n));
}

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
    std::printf("== FileApi 测试 ==\n");

    const fs::path root = makeTempRoot();
    fs::create_directories(root);
    {
        std::ofstream f(root / "hello.txt", std::ios::binary);
        f << "hello world";
    }
    const std::string rootStr = fs::weakly_canonical(root).string();
    const std::string fileStr = (fs::weakly_canonical(root) / "hello.txt").string();

    nas::FileService files({rootStr});
    nas::Router      router;
    nas::registerFileRoutes(router, files);

    // [1] 浏览
    {
        std::printf("[1] 浏览\n");
        auto r = req("GET", "/api/files");
        r.params["path"] = rootStr;
        const auto res = router.dispatch(r);
        CHECK(res.status == 200);
        CHECK(contains(res.body, "\"name\":\"hello.txt\""));
        CHECK(contains(res.body, "\"dir\":false"));
    }

    // [2] 越界 -> 403
    {
        std::printf("[2] 越界\n");
        auto r = req("GET", "/api/files");
        r.params["path"] = "/etc";
        const auto res = router.dispatch(r);
        CHECK(res.status == 403);
    }

    // [3] 下载（整文件）
    {
        std::printf("[3] 下载\n");
        auto r = req("GET", "/api/download");
        r.params["path"] = fileStr;
        const auto res = router.dispatch(r);
        CHECK(res.status == 200);
        CHECK(res.contentLength == 11);
        CHECK(static_cast<bool>(res.provider));
        FakeSink sink;
        CHECK(res.provider(0, static_cast<std::size_t>(res.contentLength), sink));
        CHECK(sink.data == "hello world");
    }

    // [4] 下载（Range: bytes=6-10 -> "world"）
    {
        std::printf("[4] Range\n");
        auto r = req("GET", "/api/download");
        r.params["path"]   = fileStr;
        r.headers["range"] = "bytes=6-10";
        const auto res = router.dispatch(r);
        CHECK(res.status == 206);
        CHECK(res.contentLength == 5);
        const auto it = res.headers.find("content-range");
        CHECK(it != res.headers.end());
        CHECK(it != res.headers.end() && it->second == "bytes 6-10/11");
        FakeSink sink;
        CHECK(res.provider(0, 5, sink));
        CHECK(sink.data == "world");
    }

    // [5] 上传（PUT 原始 body）
    {
        std::printf("[5] 上传\n");
        auto r = req("PUT", "/api/file");
        r.params["path"] = rootStr + "/new.txt";
        r.body           = "abc123";
        const auto res = router.dispatch(r);
        CHECK(res.status == 201);
        CHECK(readFile(fs::path(root) / "new.txt") == "abc123");
    }

    // [6] mkdir / rename / delete
    {
        std::printf("[6] mkdir/rename/delete\n");
        auto mk = req("POST", "/api/mkdir");
        mk.params["path"] = rootStr + "/d1";
        CHECK(router.dispatch(mk).status == 201);

        auto rn = req("POST", "/api/rename");
        rn.params["from"] = rootStr + "/new.txt";
        rn.params["to"]   = rootStr + "/d1/moved.txt";
        CHECK(router.dispatch(rn).status == 200);
        CHECK(fs::exists(fs::path(root) / "d1" / "moved.txt"));

        auto del = req("DELETE", "/api/file");
        del.params["path"]      = rootStr + "/d1";
        del.params["recursive"] = "1";
        CHECK(router.dispatch(del).status == 200);
        CHECK(!fs::exists(fs::path(root) / "d1"));
    }

    // [7] 二进制往返 + Content-Disposition（中文名）
    {
        std::printf("[7] 二进制与下载头\n");
        std::string bin;
        bin.push_back('\0');
        bin.push_back('\1');
        bin.push_back(static_cast<char>(0xFF));
        bin.push_back(static_cast<char>(0xFE));
        bin += "binary";
        const std::string name = rootStr + "/\xE4\xB8\xAD\xE6\x96\x87.bin";  // 中文.bin

        auto up = req("PUT", "/api/file");
        up.params["path"] = name;
        up.body           = bin;
        CHECK(router.dispatch(up).status == 201);

        auto dl = req("GET", "/api/download");
        dl.params["path"] = name;
        const auto res = router.dispatch(dl);
        CHECK(res.status == 200);
        CHECK(res.contentLength == bin.size());
        const auto cd = res.headers.find("content-disposition");
        CHECK(cd != res.headers.end());
        CHECK(cd != res.headers.end() && contains(cd->second, "inline"));
        FakeSink sink;
        CHECK(res.provider(0, static_cast<std::size_t>(res.contentLength), sink));
        CHECK(sink.data == bin);  // 逐字节（含 NUL / 0xFF）一致

        auto dl2 = req("GET", "/api/download");
        dl2.params["path"]     = name;
        dl2.params["download"] = "1";
        const auto res2 = router.dispatch(dl2);
        const auto cd2  = res2.headers.find("content-disposition");
        CHECK(cd2 != res2.headers.end());
        CHECK(cd2 != res2.headers.end() && contains(cd2->second, "attachment"));
        CHECK(cd2 != res2.headers.end() && contains(cd2->second, "filename*=UTF-8''"));
    }

    // [8] 递归搜索
    {
        std::printf("[8] 搜索\n");
        fs::create_directories(root / "a" / "b");
        { std::ofstream f(root / "a" / "note1.txt"); f << "x"; }
        { std::ofstream f(root / "a" / "b" / "note2.txt"); f << "y"; }
        { std::ofstream f(root / "other.bin"); f << "z"; }

        auto r = req("GET", "/api/search");
        r.params["path"] = rootStr;
        r.params["q"]    = "note";
        const auto res = router.dispatch(r);
        CHECK(res.status == 200);
        CHECK(contains(res.body, "note1.txt"));
        CHECK(contains(res.body, "note2.txt"));
        CHECK(!contains(res.body, "other.bin"));

        // 越界搜索 -> 403
        auto bad = req("GET", "/api/search");
        bad.params["path"] = "/etc";
        bad.params["q"]    = "passwd";
        CHECK(router.dispatch(bad).status == 403);
    }

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
