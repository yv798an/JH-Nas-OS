#pragma once

// =====================================================================
//  platform/ISystemState.h - 系统状态平台抽象接口
//
//  设计原则（见 docs/image-integration.md §6）：
//    * 纯平台抽象，零 OS 依赖，业务层只依赖本接口；
//    * 适配层（Linux 实现）只吐「原始挂载表」，白名单过滤是业务策略
//      （ShareRegistry），不放这里；
//    * 运行期注入实现：镜像用 LinuxSystemState，测试用 FakeSystemState。
//
//  线程契约：
//    * start()/stop() 由同一控制线程调用，不得并发；
//    * snapshot()/diskUsage() 线程安全，可从任意线程调用；
//    * onMountsChanged()/removeMountCallback() 线程安全；
//    * 挂载变化回调由实现内部线程执行，回调内不得阻塞、不得递归调用
//      removeMountCallback（会自锁）。
// =====================================================================

#include "platform/SystemTypes.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace nas {

class ISystemState {
public:
    virtual ~ISystemState() = default;

    // 启动监控。返回前必须已发布首个快照，因此 start() 之后
    // snapshot() 永不返回 nullptr。
    virtual void start() = 0;

    // 停止监控并等待内部线程退出。可重复调用；stop() 后不再触发回调。
    virtual void stop() = 0;

    // 当前快照；线程安全，start() 之后永不为 nullptr。
    virtual std::shared_ptr<const SystemSnapshot> snapshot() const = 0;

    // 按路径查询磁盘用量（statvfs）。按需调用，不进快照，避免慢设备
    // 拖住监控线程。
    virtual DiskUsage diskUsage(const std::string& path) const = 0;

    // 挂载表变化通知。回调携带最新快照；返回 token 用于注销。
    using MountCallback =
        std::function<void(const std::shared_ptr<const SystemSnapshot>&)>;

    virtual std::uint64_t onMountsChanged(MountCallback cb) = 0;
    virtual void removeMountCallback(std::uint64_t token) = 0;
};

}  // namespace nas
