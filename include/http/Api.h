#pragma once

// =====================================================================
//  http/Api.h - REST API 路由注册
//
//  把 SystemMonitor 的状态暴露为 JSON 接口。HTTP 库无关，可单测。
// =====================================================================

#include "http/Router.h"
#include "system/SystemMonitor.h"

namespace nas {

// 注册：
//   GET /api/health  -> 存活/就绪探针
//   GET /api/system  -> 完整系统状态（SystemStatus JSON）
// monitor 必须在 router 及所有请求处理期间保持存活。
void registerApiRoutes(Router& router, const SystemMonitor& monitor);

}  // namespace nas
