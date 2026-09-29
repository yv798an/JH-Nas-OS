# 镜像集成文档

> 目的：定义 `nas-server` 如何与自建 Buildroot 镜像集成。本程序不是独立 App，
> 而是镜像里的一个**服务单元**（与 `smbd` 等系统服务并存）。贴合 = 在每条边界上
> 建立显式契约。
>
> 本文只记录**已在代码 / 脚本中落地**的集成；尚未实现的部分不在此展开。
> 镜像侧配置与构建见《镜像开发流程》；项目总览见仓库根 `README.md`。

---

## 1. 设计原则

1. **单一事实来源**：每个系统级事实（挂载点、配置、日志）都有唯一权威产生者，
   本程序只消费、不自造。
2. **职责不重叠**：镜像里已有的职责，本程序不重复实现。
3. **可观测、可停止、可配置**：进程能被 init 干净地启停，改配置无需重编译。
4. **契约优先**：先冻结路径、配置 schema、启动顺序，再写代码。

---

## 2. 运行时事实与消费方式

| 事实 | 权威（唯一来源） | 本程序的对待方式 | 落地位置 |
|------|------------------|------------------|----------|
| U 盘挂载点 | 镜像挂载脚本 → `/media/sdaX` | 只读消费，绝不自己 `mount` | `LinuxSystemState::readMounts` |
| 设备名变化 | mdev 事件（sda → sdb） | 动态发现，禁止写死 `sda1` | 同上，1s 重读 `/proc/mounts` |
| 服务配置 | `/etc/nas/nas.conf` | 只读加载，overlay 注入 | `Config` |
| 日志 | `/var/log/` | 写入 `log.file` 配置路径 | `Logger` |

---

## 3. 启动顺序与进程模型

镜像使用 BusyBox init 的 `S##` 约定。本程序为 `S92nasweb`，排在 Samba（`S91smb`）
之后，保证共享已就绪：

```text
S91smb      -> Samba
S92nasweb   -> 本程序
```

约定：

- init 脚本只实现 `start | stop | restart`，不发明新动词。
- 程序必须能响应 **SIGTERM 优雅退出**：flush 日志、关闭连接、删除 pidfile。
- 启动失败须返回**非零退出码**。

**进程模型（已定案）**：程序**前台运行**，由 init 脚本
`deploy/S92nasweb` 用 BusyBox `start-stop-daemon --background --make-pidfile`
后台化并管理 pidfile。

- pidfile：`/run/nas/nasweb.pid`，**由 init 脚本拥有**，程序自身不写。
- 已落地：信号处理 `src/main.cpp`（SIGINT/SIGTERM → 停 server/state → flush 日志 →
  `nas-server stopped.`）；脚本 `deploy/S92nasweb`。

---

## 4. 已落地契约

### 4.1 部署契约

- 安装位置：二进制 → `/usr/bin/nas-server`；配置 → `/etc/nas/`；升级时 `/etc` 需保留。
- 当前部署：`scripts/build.sh <arch>` 用 Buildroot 工具链交叉编译出静态二进制，
  `scp` 到板子 `/usr/bin/`。

### 4.2 配置契约

- 位置 `/etc/nas/nas.conf`，`key = value` 纯文本（BusyBox 环境解析成本低）。
- 优先级：**命令行参数 > 配置文件 > 内置默认**。默认路径不存在时回退内置默认（开发友好）；
  显式 `--config` 指定的文件不存在则启动失败。
- 未知键/非法值**启动即失败**（防拼写错误），错误信息带行号。
- 已落地的键（`include/config/Config.h` + `src/config/Config.cpp`）：

  | 键 | 默认 | 说明 |
  |----|------|------|
  | `http.host` | `0.0.0.0` | 监听地址 |
  | `http.port` | `8080` | 监听端口 |
  | `share.roots` | `/media` | 共享白名单根，逗号分隔，交 `ShareRegistry` 过滤 |
  | `log.file` | `nas-server.log` | 镜像内应设为 `/var/log/nas-server.log` |
  | `log.level` | `INFO` | TRACE/DEBUG/INFO/WARN/ERROR/FATAL/OFF |
  | `web.dir` | 空 | 若含 `index.html` 则优先使用（改 UI 免重编译），否则用内嵌版本 |
  | `io.uring` | `false` | 大文件下载走 io_uring（需编译期检测到 liburing） |
  | `io.blockKb` | `128` | io_uring 读块大小 |
  | `io.depth` | `8` | io_uring in-flight 读队列深度 |
  | `io.thresholdKb` | `4096` | 文件 ≥ 该大小才用 io_uring |
  | `monitor.pollMs` | `1000` | 重读 `/proc/mounts` 的周期 |

- 示例见 `deploy/nas.conf.example`；由 overlay 注入，改配置无需重编译镜像。

### 4.3 文件系统 / 挂载契约

- **只读消费**挂载状态：适配层解析 `/proc/mounts`，绝不 mount/umount。
- **机制：纯轮询，1s 一次**。原因：`S91smb` 预建了 `/media/sdaX`，挂载变化不一定
  产生 inotify 事件；轮询 1s 开销可忽略，并天然规避「mkdir 先于 mount」的竞态。
- 适配层**只吐原始挂载表**；「当前可用共享目录」由业务层 `ShareRegistry` 按配置白名单
  过滤得到，业务代码只向它查询。
- 挂载表变化时重建快照并触发回调（快照式发布，读者无锁读取）。
- **写入落盘**：Web 上传写文件后 `fsync` 文件与父目录，降低掉电/拔盘丢数据风险。
- **io_uring（可选）**：Web 下载可走 io_uring 异步流水线（`io.uring=true`，且文件 ≥
  `io.thresholdKb`）。板上启用需镜像勾选 `BR2_PACKAGE_LIBURING=y` 且内核
  `CONFIG_IO_URING=y`；交叉构建会在工具链 sysroot 内查找 liburing（优先 `liburing.a`），
  找不到则自动关闭，不影响其余功能（见 `CMakeLists.txt`）。

### 4.4 观测 / 健康契约

- 日志路径来自配置 `log.file`（镜像内应设为 `/var/log/nas-server.log`）；
  `Logger::init` 自带 10 MiB × 5 轮转，无需外部 logrotate。
- 健康：`GET /api/health`。指标：`GET /api/metrics`（Prometheus 文本格式，含
  CPU/内存/负载/网络/各共享容量/进程 RSS·线程·fd/HTTP 请求计数）。
- 访问日志：HTTP 适配层记录 `方法 路径 -> 状态 (耗时us)` 到 `log.file`。
- 监控数据只从 `/proc`、`statvfs` 采集，**不依赖** `free`/`top` 等极简镜像
  可能缺失的命令行工具。
- 网页 UI 内置「监控」页：实时 CPU/内存曲线、负载/运行时长卡片、各共享占用条。

### 4.5 看门狗探活契约（方案 A：镜像 watchdog 探活）

`/dev/watchdog` 只能被一个进程打开。本程序**不打开** `/dev/watchdog`，由镜像的
`watchdog` 守护作为唯一所有者，通过 `test-binary` 周期探测本程序是否健康卡死。

- 程序侧提供自检测子命令：
  `nas-server --health-check --config /etc/nas/nas.conf`，
  对配置里的 `http.host:http.port` 发一次 `GET /api/health`；健康退出 `0`，否则退出 `1`。
  监听地址为 `0.0.0.0`（或 `::`）时自动改探本机回环 `127.0.0.1`（`::1`）。
- watchdog 的 `test-binary` 不支持传参，故用薄包装脚本 `deploy/nas-web-check.sh`
  （安装到 `/usr/bin/nas-web-check.sh`）转发子命令；脚本失败后重试 3 次才判定不健康，
  避免单次抖动触发复位。
- **启动顺序**：watchdog 的 init 脚本必须排在 `S92nasweb` **之后**（例如命名为
  `S95watchdog`），否则开机时 watchdog 先于本程序启动，首次探活必然失败，
  可能触发复位循环。
- 镜像 `/etc/watchdog.conf` 追加：

  ```ini
  test-binary = /usr/bin/nas-web-check.sh
  test-timeout = 15
  ```

  探测失败 → watchdog 停止喂狗 → 硬件复位，从而恢复卡死的服务。

---

## 5. 组件接口关系

```text
   镜像挂载脚本 ──挂载状态(只读)──┐
                                  ▼
                         ┌────────────────────────┐
   Samba (smbd) ────────►│        nas-server       │
      共享同一批文件      │  FileService / Monitor  │
                         └────────────┬───────────┘
                                      │ 读同一批文件
                                      ▼
                                 /media/sdaX
```

---

## 6. 环境适配层（ISystemState）

### 6.1 问题

业务代码若直接 `ifstream("/proc/...")` 或直接解析 `/proc/mounts`，会带来三个后果：

1. 路径与内核接口**硬编码**，与 §2 的「单一事实来源」原则冲突；
2. **无法本机（x86）测试**，任何测试都被迫跑在板子上；
3. 数据源无法替换（未来若改用 netlink / udev / `statvfs`，业务代码得跟着改）。

### 6.2 目标：依赖反转

```text
业务层(FileService/Monitor) ──依赖──► ISystemState (接口)
                                          ▲
                      ┌───────────────────┴───────────────────┐
          LinuxSystemState (镜像适配)              FakeSystemState (本机测试)
          读 /proc、statvfs                        固定 / 脚本化数据
```

业务只依赖接口，运行期选择实现，便于同一二进制切换。

### 6.3 接口（`include/platform/ISystemState.h`）

| 方法 | 说明 |
|------|------|
| `start()` | 启动监控；返回前已发布首个快照 |
| `stop()` | 停止监控并等待内部线程退出；幂等 |
| `snapshot()` | 取当前不可变快照；线程安全，`start()` 后永不为 nullptr |
| `diskUsage(path)` | 按路径查询磁盘用量（`statvfs`），按需调用 |
| `onMountsChanged(cb)` | 注册挂载表变化回调，返回 token |
| `removeMountCallback(token)` | 注销回调 |

### 6.4 与 RCU 的结合

`snapshot()` 返回 `shared_ptr<const SystemSnapshot>`：更新线程重建快照并原子发布，
读者 `load` 后持有一份，旧快照由引用计数自动回收。**C++17 下用
`std::atomic_load/atomic_store` 自由函数**（而非 C++20 的 `std::atomic<std::shared_ptr>`），
即 RCU-like snapshot 方案。

### 6.5 实现与线程模型

| 实现 | 位置 | 数据源 | 用途 |
|------|------|--------|------|
| `LinuxSystemState` | `src/platform/linux/` | `/proc`、`statvfs` | 镜像运行时 |
| `FakeSystemState` | `tests/fakes/` | 固定值 / 测试脚本 | x86 单测、无板调试 |

- 一个线程周期性（默认 1s）重建快照；对比挂载表变化后才触发回调，避免空转。
- **只读**挂载表：挂载由镜像挂载脚本完成，适配层仅解析 `/proc/mounts`。
- **适配层不做过滤**：白名单是业务策略，由 `ShareRegistry` 消费原始挂载表后得出。

### 6.6 落地状态

| 文件 | 说明 |
|------|------|
| `include/platform/SystemTypes.h` | 纯数据结构（LoadInfo / MemInfo / MountPoint / SystemSnapshot…） |
| `include/platform/ISystemState.h` | 接口（start/stop、snapshot、diskUsage、挂载回调） |
| `src/platform/linux/LinuxSystemState.{h,cpp}` | Linux 实现，1s 轮询 `/proc`，非 Linux 编译为空 |
| `tests/fakes/FakeSystemState.{h,cpp}` | 确定性测试替身，注入 + `publish()` |
| `tests/fake_systemstate_test.cpp` | 替身单测（含并发/TSan） |
| `tests/linux_systemstate_test.cpp` | 真实 `/proc` 集成测试（仅 Linux） |

---

## 7. 路径与命名约定

| 用途 | 路径 |
|------|------|
| 二进制 | `/usr/bin/nas-server` |
| 配置文件 | `/etc/nas/nas.conf` |
| init 脚本 | `/etc/init.d/S92nasweb` |
| 日志 | `/var/log/nas-server.log` |
| pidfile（init 脚本持有） | `/run/nas/nasweb.pid` |
| 探活脚本（watchdog 调用） | `/usr/bin/nas-web-check.sh` |
| 共享挂载点（Samba 共用） | `/media/sda1`、`/media/sdb1` … |

---

## 8. 验收标准（证明「贴合」）

- 拔插 U 盘：Web 列表自动更新（1s 轮询），与 Samba 可见内容一致。
- 停服务：`/etc/init.d/S92nasweb stop` 后进程干净退出、pidfile 消失、端口释放。
- 改配置：只改 `/etc/nas/nas.conf` 重启即生效，二进制不变。
- 探活：服务运行时 `nas-server --health-check` 退出 `0`，停服后退出非 `0`。
- 断网 / 无 U 盘：Web 不崩，返回明确状态。

---

## 9. 与 README 的关系

- 仓库根 `README.md`：项目定位、构建 / 运行、功能与 API、基准结论。
- 本文：与自建 Buildroot 镜像的已落地集成契约。
