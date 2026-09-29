#pragma once

// =====================================================================
//  http/HttpServerFactory.h - 服务器实现的工厂
//
//  业务/main 只依赖 HttpServer 抽象与工厂，不直接包含任何 HTTP 库。
// =====================================================================

#include "http/HttpServer.h"

#include <memory>

namespace nas {

std::unique_ptr<HttpServer> makeHttpServer();

}  // namespace nas
