#pragma once

// =====================================================================
//  http/HealthProbe.h - 服务健康自检
//
//  对运行中的 HTTP 服务做一次 GET /api/health，用于镜像 watchdog 的
//  test-binary 探活（见 docs/image-integration.md 看门狗探活契约）。
//  实现依赖 cpp-httplib，细节封在 .cpp，调用方只见本接口。
// =====================================================================

#include <string>

namespace nas {

// 返回 true 表示服务健康（HTTP 200）。
//   host : 监听地址；为 "0.0.0.0" / "::" 等通配地址时自动改探本机回环。
//   port : 监听端口。
//   timeoutMs : 连接 / 读 / 写超时（毫秒）。
bool probeHealth(const std::string& host, int port, int timeoutMs);

}  // namespace nas
