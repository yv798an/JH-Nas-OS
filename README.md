# OrangePi NAS Web Server (C++17)

在 RISC-V（StarFive JH7110 / Orange Pi RV）自建 Buildroot 镜像上运行的 **NAS Web 服务**：
目录浏览 / 上传 / 下载（断点续传 Range）/ 重命名 / 搜索 / 实时监控，单文件静态二进制。

- **语言/构建**：C++17、CMake；第三方仅 `cpp-httplib`（MIT，单头文件，已 vendor）
- **平台**：Linux / RISC-V 64；同时可在 x86 上跑完整单元测试（含 TSan）
- **异步 I/O**：内置 `io_uring` 异步读后端 + 无锁 SPSC 队列 + 缓冲池（附对照基准）
- **可观测**：`/api/metrics`（Prometheus）+ 访问日志 + 网页实时监控
- **可配置**：`/etc/nas/nas.conf`，改配置无需重编译

---

## 特性

- **文件 API**：浏览 / 上传 / 下载（Range 206，视频秒开）/ 删除 / 重命名 / 建目录 / 递归搜索
- **路径安全**：白名单根目录 + `weakly_canonical` 规范化，防 `../` 与符号链接越界
- **写入落盘**：上传后 `fsync` 文件与父目录，降低掉电/拔盘丢数据
- **网页 UI**：文件管理 + 监控页（CPU/内存实时曲线、磁盘占用条），内嵌进二进制，可外部覆盖
- **系统监控**：从 `/proc`、`statvfs` 采集，不依赖 `free`/`top` 等工具
- **优雅退出**：SIGTERM 停服务、flush 日志、释放资源；配 BusyBox init 脚本
- **看门狗探活**：`--health-check` 自检子命令，供镜像 watchdog 的 `test-binary` 周期调用

---

## 架构

严格分层，业务只依赖抽象，平台细节集中在一层：

```
        ┌───────────────────────── 业务/展示 ─────────────────────────┐
        │  FileService        SystemMonitor        WebUi / Api        │
        └───────▲──────────────────▲────────────────────▲───────────┘
                │                  │                    │
        ┌───────┴──────────────────┴────────────────────┴───────────┐
        │                    http/ HTTP 抽象层                        │
        │  HttpTypes · Router · HttpServer(接口) · BodyProvider 流式  │
        └───────────────────────────▲───────────────────────────────┘
                                    │ 仅接口
        ┌───────────────┬───────────┴──────────┬────────────────────┐
        │ HttplibHttpServer  LinuxSystemState   FakeSystemState       │
        │ (cpp-httplib)      (读 /proc /statvfs) (测试替身)            │
        └───────────────┴──────────────────────┴────────────────────┘
                │
        ┌───────┴─────────┐     io/ 并发与异步
        │  io_uring 读后端 │     SpscRingBuffer · BufferPool · AsyncFileReader
        └─────────────────┘
```

- `ISystemState` 把「读 `/proc`、解析挂载表」从业务里抽离 → 本机可用 `FakeSystemState`
  做确定性测试，产物同一套代码按配置切换实现。
- `HttpServer` 抽象让 HTTP 库可替换（后期可换 epoll / io_uring 服务器而不动业务）。

---

## 快速开始

### 依赖

```bash
sudo apt install -y build-essential cmake liburing-dev   # liburing 可选，启用 io_uring
```

### 主机测试（x86，含 TSan）

```bash
sh scripts/test.sh        # 13 个用例全绿
```

> 若 TSan 报 `unexpected memory mapping`：`sudo sysctl -w vm.mmap_rnd_bits=28`

### 本机运行

```bash
cat > /tmp/nas.conf <<EOF
http.host = 0.0.0.0
http.port = 8080
share.roots = $HOME/nasroot
log.file = /tmp/nas-server.log
io.uring = true
EOF
mkdir -p "$HOME/nasroot"
./build-host/nas-server --serve --config /tmp/nas.conf
```
浏览器打开 `http://127.0.0.1:8080/`。

### 交叉编译上板（RISC-V）

```bash
TC=~/buildroot/output/host sh scripts/build.sh riscv64
# 产物 build-riscv64/nas-server（静态链接，无动态依赖）
scp build-riscv64/nas-server root@<board>:/usr/bin/nas-server
```
板子上放置 `deploy/S92nasweb` → `/etc/init.d/`、`deploy/nas.conf.example` → `/etc/nas/nas.conf`，
然后 `chmod +x /etc/init.d/S92nasweb && /etc/init.d/S92nasweb start`。

---

## 测试

### 全部测试

```bash
sh scripts/test.sh
```

### 只跑某一个功能

先用 `-N` 看所有测试名，再用 `-R`（正则）挑着跑：

```bash
ctest --test-dir build-host -N                                   # 列出全部用例
ctest --test-dir build-host -R config --output-on-failure        # 只跑 config
ctest --test-dir build-host -R 'file_.*' --output-on-failure     # 跑 file_service + file_api
./build-host/tests/config_test                                   # 也可以直接运行某个二进制
```

只想重新构建某个目标（不动其它）：

```bash
cmake --build build-host --target file_api_test
```

> 若 TSan 报 `unexpected memory mapping`：`sudo sysctl -w vm.mmap_rnd_bits=28`，
> 或用 `-DNAS_TSAN=OFF` 重新配置。

### 功能 ↔ 用例 ↔ 代码 对照

| 功能 | ctest 名 | 代码位置 |
|------|----------|----------|
| 无锁 SPSC 环形队列 | `ringbuffer` | `include/concurrent/SpscRingBuffer.h` |
| 异步日志（轮转） | `logger` | `src/common/Logger.cpp` |
| 配置解析（前缀/校验/行号） | `config` | `src/config/Config.cpp` |
| 平台状态（真实 `/proc`） | `linux_systemstate` | `src/platform/linux/LinuxSystemState.cpp` |
| 平台状态（测试替身） | `fake_systemstate` | `tests/fakes/FakeSystemState.cpp` |
| 系统状态 / 共享过滤 | `system_monitor` | `src/system/{SystemMonitor,ShareRegistry}.cpp` |
| 路由 + 系统 API | `http` | `src/http/{Router,Api}.cpp` |
| 健康探活 | `health_probe` | `src/http/HealthProbe.cpp` |
| 文件业务（安全/读写） | `file_service` | `src/file/FileService.cpp` |
| 文件 HTTP API | `file_api` | `src/http/FileApi.cpp` |
| 网页内嵌 / 外部覆盖 | `web_ui` | `src/http/WebUi.cpp` |
| 缓冲池 | `buffer_pool` | `src/io/BufferPool.cpp` |
| io_uring 异步读 | `io_uring` | `src/io/{IoUring,AsyncFileReader}.cpp` |

### 端到端手动验证（服务运行时）

先按「本机运行」起服务，再单独打某个接口：

```bash
curl 'http://127.0.0.1:8080/api/files?path=/srv/nas'
curl 'http://127.0.0.1:8080/api/search?path=/srv/nas&q=mp4'
curl -r 100-199 -o /tmp/part 'http://127.0.0.1:8080/api/download?path=/srv/nas/big.bin'   # Range
curl -X PUT --data-binary @local.bin 'http://127.0.0.1:8080/api/file?path=/srv/nas/a.bin' # 上传
curl -X DELETE 'http://127.0.0.1:8080/api/file?path=/srv/nas/a.bin'                        # 删除
curl -s http://127.0.0.1:8080/api/system          # 系统状态 JSON
curl -s http://127.0.0.1:8080/api/metrics         # Prometheus 指标
```

### 基准单独跑

```bash
./build-host/io_bench --size 256 --block 64                       # io_uring vs 同步读
./build-host/http_bench --threads 8 --rounds 3 \
  --url 'http://127.0.0.1:8080/api/download?path=/srv/nas/big.bin' # HTTP 下载压测
```

### 只测某个模块（不起服务）

单元测试直接构造对应类即可，例如平台无关部分用 `FakeSystemState` 注入：

```cpp
FakeSystemState state;
state.setMounts({/* ... */});
state.publish();
SystemMonitor monitor(state);      // 不依赖 /proc、不依赖开发板
auto st = monitor.report();
```

---

## HTTP API

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/` | 网页 UI（文件 + 监控） |
| GET | `/api/roots` | 允许的共享根 |
| GET | `/api/files?path=DIR` | 目录浏览 |
| GET | `/api/search?path=DIR&q=SUBSTR` | 递归搜索 |
| GET | `/api/download?path=FILE[&download=1]` | 下载，支持 `Range`；默认 inline 预览 |
| PUT | `/api/file?path=FILE` | 上传/覆盖（原始 body） |
| DELETE | `/api/file?path=FILE[&recursive=1]` | 删除 |
| POST | `/api/mkdir?path=DIR` | 建目录 |
| POST | `/api/rename?from=A&to=B` | 重命名/移动 |
| GET | `/api/system` | 系统状态 JSON |
| GET | `/api/metrics` | Prometheus 指标 |
| GET | `/api/health` | 健康探针 |

---

## 配置（`/etc/nas/nas.conf`）

`key = value`，优先级 `命令行 > 文件 > 默认`；未知键/非法值启动即失败（带行号）。

| 键 | 默认 | 说明 |
|----|------|------|
| `http.host` / `http.port` | `0.0.0.0` / `8080` | 监听地址/端口 |
| `share.roots` | `/media` | 共享白名单根，逗号分隔 |
| `log.file` / `log.level` | `nas-server.log` / `INFO` | 日志与级别 |
| `web.dir` | 空 | 含 `index.html` 时覆盖内嵌 UI |
| `io.uring` / `io.blockKb` / `io.depth` / `io.thresholdKb` | `false`/`128`/`8`/`4096` | io_uring 下载 |
| `monitor.pollMs` | `1000` | 重读 `/proc/mounts` 周期 |

完整契约见 [`docs/image-integration.md`](docs/image-integration.md)。

---

## 基准（实测，`bench/io_bench`，256 MiB 文件 / 64 KiB 块 / 重复 3 次取中位数）

> 实测机器：OrangePi RV（StarFive JH7110，4×U74 `rv64imafdc`，1.88 GiB RAM，内核 5.15）。
> 关键：**区分冷盘（drop page cache）与热盘（page cache 命中）**——两者结论相反。

**板端 `io_bench`（MiB/s）**

| 方案 | 冷盘 | 热盘 |
|------|-----:|-----:|
| 同步 `read` 单流 | 171 | 1371 |
| `io_uring` QD=1 | 224 | 1250 |
| `io_uring` QD=4 | 223 | 1226 |
| `io_uring` QD=16 | 235 | 1103 |
| `io_uring` QD=64 | 234 | 882 |

**板端 `http_bench` 下载（热缓存，MiB/s）**

| 模式 | threads=1 | threads=8 |
|------|----------:|----------:|
| 同步 | 380 | 649 |
| `io_uring` | 341 | 453 |

**诚实结论**：io_uring 的价值**严格取决于数据是否在 page cache、以及存储是否被单流打满**——
冷盘约 **1.3–1.4×**（靠异步提交掩盖设备延迟，队列深度再加深几乎不再提升），
但热盘反而**纯亏**（QD=64 仅同步的 64%），因为缓存命中时同步 `read()` 就是 memcpy，
io_uring 只多付出建 ring / 起线程 / 缓冲交接 / 自旋的开销；经 HTTP 后同样更慢（8 线程 0.70×）。
同一套基准在 x86 云虚拟机上对照时，冷盘因虚拟盘已被同步读打满而几无收益（~1.0×），
可见收益高度依赖宿主存储特性。因此 io_uring 做成**可配置 + 自动回退**，而非默认全开——
这类"优化要看负载"的判断本身也是工程重点。

`bench/http_bench` 可对运行中的服务做多线程拉取压测。

---

## 目录结构

```
NAS-Web-Cpp/
├── CMakeLists.txt
├── README.md
├── docs/
│   └── image-integration.md     # 与自建镜像的系统集成契约
├── third_party/cpp-httplib/     # MIT 单头文件（vendor）
├── include/
│   ├── common/Logger.h
│   ├── concurrent/SpscRingBuffer.h
│   ├── config/Config.h
│   ├── file/FileService.h
│   ├── platform/                # SystemTypes.h / ISystemState.h
│   ├── system/                  # SystemMonitor.h / ShareRegistry.h
│   ├── http/                    # HttpTypes/HttpServer/Router/Api/FileApi/WebUi
│   └── io/                      # BufferPool.h / IoUring.h / AsyncFileReader.h
├── src/                         # 与 include 对应的实现 + main.cpp
├── bench/                       # io_bench / http_bench
├── deploy/                      # nas.conf.example / S92nasweb
├── web/index.html               # 网页 UI（CMake 生成内嵌资源）
├── scripts/                     # build.sh（交叉编译）/ test.sh（本机测试）
└── tests/                       # 13 个用例（多库 + TSan）
```

---

## 设计要点

- **依赖反转的测试性**：平台访问走接口，`FakeSystemState` 让业务逻辑在无开发板、无真实
  `/proc` 的条件下可确定性验证。
- **无锁与异步**：`SpscRingBuffer`（release/acquire）作为 io_uring 完成队列，
  `BufferPool` 复用缓冲，`AsyncFileReader` 用一条 ring 多 in-flight 读流水线。
- **快照式状态发布**：`snapshot()` 返回不可变快照，配 `std::atomic_load/store` 实现
  读者无锁（C++17）。
- **可观测即契约**：指标、访问日志、健康探针从一开始就是设计的一部分。

---

## 部署 / 镜像集成

约定见 [`docs/image-integration.md`](docs/image-integration.md)：启动顺序（`S92nasweb`）、
路径约定、配置注入（overlay）等。

部署脚本 `deploy/S92nasweb` 用 BusyBox `start-stop-daemon` 后台化并持有 pidfile，
程序自身前台运行、响应 SIGTERM 优雅退出。`deploy/nas-web-check.sh` 供镜像 watchdog
的 `test-binary` 调用 `nas-server --health-check` 探活（`/dev/watchdog` 归镜像 watchdog 独占）。

---

## 已知限制 / 后续

- 无鉴权 / 无 TLS（当前定位为可信内网、单人使用）
- 尚未做 Buildroot 包化（目前 `scp` 部署二进制）
- io_uring 后端每请求建一个 ring，后续可改为共享 ring 池

---

## 第三方

- [`cpp-httplib`](https://github.com/yhirose/cpp-httplib)（MIT），见 `third_party/cpp-httplib/LICENSE`
- 运行时可选依赖 `liburing`（LGPL/MIT）；未安装则自动禁用 io_uring 后端
