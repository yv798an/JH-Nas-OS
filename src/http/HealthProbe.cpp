#include "http/HealthProbe.h"

#include <httplib.h>

#include <chrono>
#include <string>

namespace nas {

namespace {

// 把监听地址映射成可连接的回环地址：通配地址不能直接作为目的地址。
std::string connectHost(const std::string& host) {
    if (host.empty() || host == "0.0.0.0") return "127.0.0.1";
    if (host == "::" || host == "[::]" || host == "0:0:0:0:0:0:0:0") {
        return "::1";
    }
    return host;
}

}  // namespace

bool probeHealth(const std::string& host, int port, int timeoutMs) {
    if (port <= 0 || port > 65535) return false;

    httplib::Client client(connectHost(host), port);
    const auto timeout = std::chrono::milliseconds(timeoutMs);
    client.set_connection_timeout(timeout);
    client.set_read_timeout(timeout);
    client.set_write_timeout(timeout);

    const auto res = client.Get("/api/health");
    return res && res->status == 200;
}

}  // namespace nas
