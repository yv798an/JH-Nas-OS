// LinuxSystemState 集成测试（仅 Linux）。直接在 x86 Linux 上跑即可，
// 因为 /proc、/sys 是通用的，不需要开发板。
//
// 覆盖：首个快照非空、/proc 采集合理、diskUsage、回调生命周期、
// stop() 幂等与停止后仍可读快照。

#include <cstdint>
#include <cstdio>

#if defined(__linux__)

#include "platform/linux/LinuxSystemState.h"

#include <atomic>
#include <chrono>
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

void testSnapshot() {
    std::printf("[1] 首个快照与 /proc 采集\n");
    nas::LinuxSystemState s(std::chrono::milliseconds(50));
    s.start();

    auto snap = s.snapshot();
    CHECK(snap != nullptr);
    if (snap) {
        CHECK(snap->mem.totalKb > 0);
        CHECK(snap->uptimeSec > 0.0);
        CHECK(!snap->mounts.empty());
        std::printf("  total=%llu kB uptime=%.1fs mounts=%zu cpu=%.1f%%\n",
                    static_cast<unsigned long long>(snap->mem.totalKb),
                    snap->uptimeSec, snap->mounts.size(), snap->cpuPercent);
    }
    s.stop();
}

void testDiskUsage() {
    std::printf("[2] diskUsage\n");
    nas::LinuxSystemState s;
    const nas::DiskUsage usage = s.diskUsage("/");
    CHECK(usage.ok);
    CHECK(usage.totalBytes > 0);
    CHECK(usage.freeBytes <= usage.totalBytes);

    const nas::DiskUsage miss = s.diskUsage("/definitely/not/here");
    CHECK(miss.ok == false);
}

void testCallbackLifecycle() {
    std::printf("[3] 回调生命周期与 stop 幂等\n");
    nas::LinuxSystemState s(std::chrono::milliseconds(50));
    s.start();

    std::atomic<int> calls{0};
    const std::uint64_t token =
        s.onMountsChanged([&](const std::shared_ptr<const nas::SystemSnapshot>&) {
            calls.fetch_add(1, std::memory_order_relaxed);
        });
    (void)token;
    std::this_thread::sleep_for(std::chrono::milliseconds(120));

    s.removeMountCallback(token);
    s.stop();
    s.stop();  // 幂等
}

void testSnapshotAfterStop() {
    std::printf("[4] stop 后仍可读快照\n");
    nas::LinuxSystemState s(std::chrono::milliseconds(50));
    s.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    s.stop();

    auto snap = s.snapshot();
    CHECK(snap != nullptr);
}

}  // namespace

int main() {
    std::printf("== LinuxSystemState 测试 ==\n");
    testSnapshot();
    testDiskUsage();
    testCallbackLifecycle();
    testSnapshotAfterStop();

    const int failed = g_failed.load();
    if (failed == 0) {
        std::printf("== 全部通过 ==\n");
        return 0;
    }
    std::printf("== 失败: %d 项 ==\n", failed);
    return 1;
}

#else

int main() {
    std::printf("LinuxSystemState 测试：非 Linux 平台，跳过。\n");
    return 0;
}

#endif  // __linux__
