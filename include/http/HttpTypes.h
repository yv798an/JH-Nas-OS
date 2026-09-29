#pragma once

// =====================================================================
//  http/HttpTypes.h - 与具体 HTTP 库无关的请求/响应类型
//
//  业务层只依赖这些类型，不依赖 cpp-httplib。头部键统一小写存放。
// =====================================================================

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

namespace nas {

// 流式响应体的写入端，由 HTTP 适配层实现（包装 httplib::DataSink 等）。
class BodySink {
public:
    virtual ~BodySink() = default;
    virtual bool write(const char* data, std::size_t len) = 0;
};

// offset 相对响应体起点（与 contentLength 对应）；length 为建议单次写入长度。
// HTTP Range 由业务层解析，并把 contentLength/offset 限定在所选区间内。
using BodyProvider =
    std::function<bool(std::size_t offset, std::size_t length, BodySink& sink)>;

struct HttpRequest {
    std::string method;   // GET / POST / PUT / DELETE / ...
    std::string path;     // /api/system
    std::string query;    // 原始查询串（不含 '?'）
    std::unordered_map<std::string, std::string> headers;  // 键小写
    std::unordered_map<std::string, std::string> params;   // 查询/路径参数
    std::string body;
};

struct HttpResponse {
    int status = 200;
    std::unordered_map<std::string, std::string> headers;
    std::string body;  // 普通响应；provider 非空时忽略

    // 流式响应：provider 非空时优先，contentLength 为响应体总长度。
    std::uint64_t contentLength = 0;
    BodyProvider  provider;
};

using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

}  // namespace nas
