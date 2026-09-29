#pragma once

// =====================================================================
//  http/Router.h - 极简路由器（与 HTTP 库无关，可单测）
//
//  MVP：method + path 精确匹配。未收录路径返回 404，方法不符返回 405。
//  路径参数/通配后续按需扩展。
// =====================================================================

#include "http/HttpTypes.h"

#include <string>
#include <vector>

namespace nas {

class Router {
public:
    void add(const std::string& method, const std::string& path,
             HttpHandler handler);

    HttpResponse dispatch(const HttpRequest& req) const;

private:
    struct Route {
        std::string method;
        std::string path;
        HttpHandler handler;
    };
    std::vector<Route> routes_;
};

}  // namespace nas
