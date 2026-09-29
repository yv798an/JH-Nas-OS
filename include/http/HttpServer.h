#pragma once

// =====================================================================
//  http/HttpServer.h - HTTP 服务器抽象
//
//  业务只依赖本接口；具体实现（当前 cpp-httplib）通过工厂注入。
//  后期可换 epoll / io_uring 实现而不动业务代码。
// =====================================================================

#include "http/HttpTypes.h"

#include <string>

namespace nas {

class HttpServer {
public:
    virtual ~HttpServer() = default;

    // 注入统一分发函数（通常转发给 Router::dispatch）。
    virtual void setHandler(HttpHandler handler) = 0;

    // 绑定 host:port 并进入 accept 循环；阻塞，直到 stop() 被调用。
    // 绑定失败返回 false。
    virtual bool listen(const std::string& host, int port) = 0;

    // 从其他线程调用以停止 listen()。
    virtual void stop() = 0;
};

}  // namespace nas
