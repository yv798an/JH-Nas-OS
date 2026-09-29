#include "http/Router.h"

#include <utility>

namespace nas {

namespace {

HttpResponse jsonMessage(int status, const char* message) {
    HttpResponse res;
    res.status                      = status;
    res.headers["content-type"]     = "application/json; charset=utf-8";
    res.body                        = std::string("{\"error\":\"") + message + "\"}";
    return res;
}

}  // namespace

void Router::add(const std::string& method, const std::string& path,
                 HttpHandler handler) {
    routes_.push_back(Route{method, path, std::move(handler)});
}

HttpResponse Router::dispatch(const HttpRequest& req) const {
    bool pathMatched = false;
    for (const Route& route : routes_) {
        if (route.path != req.path) continue;
        pathMatched = true;
        if (route.method == req.method) {
            if (route.handler) return route.handler(req);
            return jsonMessage(500, "handler missing");
        }
    }
    if (pathMatched) return jsonMessage(405, "method not allowed");
    return jsonMessage(404, "not found");
}

}  // namespace nas
