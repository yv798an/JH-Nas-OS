// SystemMonitor / ShareRegistry 测试（平台无关，用 FakeSystemState 注入）。
//
// 覆盖：白名单过滤与排序、内存换算、共享容量聚合、JSON 输出。

#include "fakes/FakeSystemState.h"
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

nas::MountPoint mount(const char* dev, const char* path, const char* fs,
                      bool rw) {
    nas::MountPoint mp;
    mp.device    = dev;
    mp.mountPath = path;
    mp.fstype    = fs;
    mp.writable  = rw;
    return mp;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// 1) ShareRegistry 白名单过滤 + 排序
void testShareRegistry() {
    std::printf("[1] ShareRegistry 过滤/排序\n");
    nas::ShareRegistry reg({"/media"});
    CHECK(reg.allows("/media/sda1"));
    CHECK(!reg.allows("/media"));
    CHECK(!reg.allows("/"));
    CHECK(!reg.allows("/proc"));
    CHECK(!reg.allows("/media2/sda1"));
    CHECK(!reg.allows("/etc/passwd"));

    std::vector<nas::MountPoint> mounts = {
        mount("/dev/sda2", "/media/sda2", "ntfs", false),
        mount("proc", "/proc", "proc", false),
        mount("/dev/sda1", "/media/sda1", "vfat", true),
        mount("/", "/", "ext4", true),
    };
    auto shares = reg.resolve(mounts);
    CHECK(shares.size() == 2);
    CHECK(shares[0].path == "/media/sda1");
    CHECK(shares[0].name == "sda1");
    CHECK(shares[0].writable);
    CHECK(shares[1].path == "/media/sda2");
    CHECK(!shares[1].writable);
}

// 2) 内存换算与百分比
void testMemory() {
    std::printf("[2] 内存换算\n");
    nas::FakeSystemState state;
    state.setMem(nas::MemInfo{2048, 512, 1024});  // kB
    state.publish();

    nas::SystemMonitor monitor(state);
    nas::SystemStatus st = monitor.report();
    CHECK(st.mem.totalBytes == 2048ull * 1024);
    CHECK(st.mem.availableBytes == 1024ull * 1024);
    CHECK(st.mem.usedBytes == 1024ull * 1024);
    CHECK(st.mem.usedPercent == 50.0);
}

// 3) 共享容量聚合（diskUsage + used/percent）
void testShareUsage() {
    std::printf("[3] 共享容量聚合\n");
    nas::FakeSystemState state;
    state.setMounts({
        mount("/dev/sda1", "/media/sda1", "vfat", true),
        mount("/dev/sda2", "/media/sda2", "ntfs", false),
    });
    state.setDiskUsage("/media/sda1", nas::DiskUsage{1000, 400, true});
    state.setDiskUsage("/media/sda2", nas::DiskUsage{500, 100, true});
    state.publish();

    nas::SystemMonitor monitor(state);
    nas::SystemStatus st = monitor.report();
    CHECK(st.shares.size() == 2);

    CHECK(st.shares[0].name == "sda1");
    CHECK(st.shares[0].usageOk);
    CHECK(st.shares[0].totalBytes == 1000);
    CHECK(st.shares[0].freeBytes == 400);
    CHECK(st.shares[0].usedBytes == 600);
    CHECK(st.shares[0].usedPercent == 60.0);

    CHECK(st.shares[1].usedPercent == 80.0);
}

// 4) diskUsage 失败时 usageOk=false，不崩
void testUsageMiss() {
    std::printf("[4] diskUsage 未命中\n");
    nas::FakeSystemState state;
    state.setMounts({mount("/dev/sda1", "/media/sda1", "vfat", true)});
    state.publish();

    nas::SystemMonitor monitor(state);
    auto st = monitor.report();
    CHECK(st.shares.size() == 1);
    CHECK(st.shares[0].usageOk == false);
    CHECK(st.shares[0].totalBytes == 0);
}

// 5) 无盘：共享为空但其余字段正常
void testNoShares() {
    std::printf("[5] 无盘\n");
    nas::FakeSystemState state;
    state.setMem(nas::MemInfo{1000, 0, 500});
    state.publish();

    nas::SystemMonitor monitor(state);
    auto st = monitor.report();
    CHECK(st.shares.empty());
    CHECK(st.mem.usedPercent == 50.0);
}

// 6) JSON 输出结构
void testJson() {
    std::printf("[6] JSON 输出\n");
    nas::FakeSystemState state;
    state.setMem(nas::MemInfo{2048, 0, 1024});
    state.setUptime(99.5);
    state.setMounts({mount("/dev/sda1", "/media/sda1", "vfat", true)});
    state.setDiskUsage("/media/sda1", nas::DiskUsage{1000, 400, true});
    state.publish();

    nas::SystemMonitor monitor(state);
    const std::string json = nas::toJson(monitor.report());

    CHECK(!json.empty());
    CHECK(json.front() == '{');
    CHECK(json.back() == '}');
    CHECK(contains(json, "\"uptimeSec\":99.5"));
    CHECK(contains(json, "\"name\":\"sda1\""));
    CHECK(contains(json, "\"totalBytes\":1000"));
    CHECK(contains(json, "\"usedPercent\":60.0"));
    CHECK(contains(json, "\"shares\":["));
    std::printf("  JSON: %s\n", json.c_str());
}

}  // namespace

int main() {
    std::printf("== SystemMonitor 测试 ==\n");
    testShareRegistry();
    testMemory();
    testShareUsage();
    testUsageMiss();
    testNoShares();
    testJson();

    const int failed = g_failed.load();
    if (failed == 0) {
        std::printf("== 全部通过 ==\n");
        return 0;
    }
    std::printf("== 失败: %d 项 ==\n", failed);
    return 1;
}
