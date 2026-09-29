// FakeSystemState 正确性测试（平台无关，可在 x86 上直接跑）。
//
// 覆盖：初始快照非空、注入值发布、回调仅在挂载变化时触发、注销回调、
// diskUsage 命中/未命中、并发发布与读取（配合 TSan）。

#include "fakes/FakeSystemState.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

std::atomic<int> g_failed{0};

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failed.fetch_add(1, std::memory_order_relaxed);              \
        }                                                                  \
    } while (0)

nas::MountPoint makeMount(const char* device, const char* path) {
    nas::MountPoint mp;
    mp.device    = device;
    mp.mountPath = path;
    mp.fstype    = "vfat";
    mp.writable  = true;
    return mp;
}

// 1) 初始快照非空，且内容为零值
void testInitialSnapshot() {
    std::printf("[1] 初始快照\n");
    nas::FakeSystemState s;
    auto snap = s.snapshot();
    CHECK(snap != nullptr);
    CHECK(snap->mem.totalKb == 0);
    CHECK(snap->mounts.empty());
}

// 2) 注入 + publish 反映到最新快照
void testInjection() {
    std::printf("[2] 注入与发布\n");
    nas::FakeSystemState s;
    s.setMem(nas::MemInfo{1024, 256, 512});
    s.setUptime(123.5);
    s.setCpuPercent(7.25);
    s.setNet(nas::NetStats{1000, 2000});
    const bool changed = s.publish();
    CHECK(changed == false);  // 挂载表没变

    auto snap = s.snapshot();
    CHECK(snap->mem.totalKb == 1024);
    CHECK(snap->mem.availableKb == 512);
    CHECK(snap->uptimeSec == 123.5);
    CHECK(snap->cpuPercent == 7.25);
    CHECK(snap->net.rxBytes == 1000);
    CHECK(snap->net.txBytes == 2000);
}

// 3) 回调仅在挂载表变化时触发，且携带最新快照
void testMountCallback() {
    std::printf("[3] 挂载变化回调\n");
    nas::FakeSystemState s;

    int calls = 0;
    std::size_t lastMounts = 0;
    s.onMountsChanged([&](const std::shared_ptr<const nas::SystemSnapshot>& snap) {
        ++calls;
        lastMounts = snap->mounts.size();
    });

    s.setMounts({makeMount("/dev/sda1", "/media/sda1")});
    CHECK(s.publish() == true);   // 变化 -> 回调
    CHECK(calls == 1);
    CHECK(lastMounts == 1);

    CHECK(s.publish() == false);  // 未变化 -> 不回调
    CHECK(calls == 1);

    s.setMounts({});
    CHECK(s.publish() == true);
    CHECK(calls == 2);
    CHECK(lastMounts == 0);
}

// 4) 注销回调后不再触发
void testRemoveCallback() {
    std::printf("[4] 注销回调\n");
    nas::FakeSystemState s;
    int calls = 0;
    const std::uint64_t token =
        s.onMountsChanged([&](const std::shared_ptr<const nas::SystemSnapshot>&) {
            ++calls;
        });

    s.setMounts({makeMount("/dev/sda1", "/media/sda1")});
    s.publish();
    CHECK(calls == 1);

    s.removeMountCallback(token);
    s.setMounts({});
    CHECK(s.publish() == true);  // 挂载确实变了
    CHECK(calls == 1);           // 但已注销，不回调
}

// 5) diskUsage 命中与未命中
void testDiskUsage() {
    std::printf("[5] diskUsage\n");
    nas::FakeSystemState s;
    s.setDiskUsage("/media/sda1", nas::DiskUsage{100000, 40000, true});

    auto hit = s.diskUsage("/media/sda1");
    CHECK(hit.ok);
    CHECK(hit.totalBytes == 100000);
    CHECK(hit.freeBytes == 40000);

    auto miss = s.diskUsage("/media/nope");
    CHECK(miss.ok == false);
}

// 6) 并发：一个线程反复发布，另一些线程并发读快照
void testConcurrent() {
    std::printf("[6] 并发发布/读取\n");
    nas::FakeSystemState s;
    s.onMountsChanged([](const std::shared_ptr<const nas::SystemSnapshot>&) {});

    std::atomic<bool> stop{false};
    std::thread writer([&] {
        for (int i = 0; i < 20000; ++i) {
            if (i % 2 == 0) {
                s.setMounts({makeMount("/dev/sda1", "/media/sda1")});
            } else {
                s.setMounts({});
            }
            s.publish();
        }
        stop.store(true);
    });

    std::thread reader([&] {
        std::uint64_t seen = 0;
        while (!stop.load()) {
            auto snap = s.snapshot();
            if (snap) seen += snap->mounts.size();
        }
        (void)seen;
    });

    writer.join();
    reader.join();
}

}  // namespace

int main() {
    std::printf("== FakeSystemState 测试 ==\n");
    testInitialSnapshot();
    testInjection();
    testMountCallback();
    testRemoveCallback();
    testDiskUsage();
    testConcurrent();

    const int failed = g_failed.load();
    if (failed == 0) {
        std::printf("== 全部通过 ==\n");
        return 0;
    }
    std::printf("== 失败: %d 项 ==\n", failed);
    return 1;
}
