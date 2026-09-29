#include "http/HttpServerFactory.h"

#include "common/Logger.h"
#include "http/HttpStats.h"

#include <httplib.h>

#include <cctype>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>

namespace nas {

namespace {

std::string toLower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// 把 httplib::DataSink 包成平台无关的 BodySink。
class HttplibSink final : public BodySink {
public:
    explicit HttplibSink(httplib::DataSink& sink) : sink_(sink) {}
    bool write(const char* data, std::size_t len) override {
        return sink_.write(data, len);
    }

private:
    httplib::DataSink& sink_;
};

// HttpServer 的 cpp-httplib 适配层。所有 HTTP 库细节都封在这里，
// 业务代码只见到 http/HttpTypes.h 与 HttpServer 抽象。
class HttplibHttpServer final : public HttpServer {
public:
    void setHandler(HttpHandler handler) override {
        handler_ = std::move(handler);

        auto wrap = [this](const httplib::Request& req, httplib::Response& res) {
            // Range 由业务层自行处理，故清空 httplib 解析出的 ranges，避免它再套一层
            // 范围逻辑（只设 Content-Range 不给 206，且越界会触发 assert）。
            // 底层 Request 对象非 const（见 Server::dispatch_request）。
            const_cast<httplib::Request&>(req).ranges.clear();

            HttpRequest hr;
            hr.method = req.method;
            hr.path   = req.path;
            hr.body   = req.body;
            for (const auto& kv : req.headers) {
                hr.headers[toLower(kv.first)] = kv.second;
            }
            for (const auto& kv : req.params) {
                hr.params[kv.first] = kv.second;
            }
            const std::size_t q = req.target.find('?');
            if (q != std::string::npos) hr.query = req.target.substr(q + 1);

            const auto t0 = std::chrono::steady_clock::now();
            HttpResponse out;
            if (handler_) out = handler_(hr);
            const auto t1 = std::chrono::steady_clock::now();
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                                t1 - t0)
                                .count();
            LOG_INFO("%s %s -> %d (%lldus)", hr.method.c_str(), hr.path.c_str(),
                     out.status, static_cast<long long>(us));

            HttpStats& stats = httpStats();
            stats.requestsTotal.fetch_add(1, std::memory_order_relaxed);
            if (out.status >= 500) {
                stats.responses5xx.fetch_add(1, std::memory_order_relaxed);
            } else if (out.status >= 400) {
                stats.responses4xx.fetch_add(1, std::memory_order_relaxed);
            }

            res.status                = out.status;
            std::string contentType   = "application/json; charset=utf-8";
            for (const auto& header : out.headers) {
                if (toLower(header.first) == "content-type") {
                    contentType = header.second;
                } else {
                    res.set_header(header.first, header.second);
                }
            }

            if (out.provider) {
                res.set_content_provider(
                    static_cast<std::size_t>(out.contentLength), contentType,
                    [provider = out.provider](std::size_t offset,
                                              std::size_t length,
                                              httplib::DataSink& sink) {
                        HttplibSink bodySink(sink);
                        return provider(offset, length, bodySink);
                    });
            } else {
                res.set_content(out.body, contentType);
            }
        };

        server_.Get(".*", wrap);
        server_.Post(".*", wrap);
        server_.Put(".*", wrap);
        server_.Patch(".*", wrap);
        server_.Delete(".*", wrap);
        server_.Options(".*", wrap);
    }

    bool listen(const std::string& host, int port) override {
        return server_.listen(host, port);
    }

    void stop() override { server_.stop(); }

private:
    httplib::Server server_;
    HttpHandler     handler_;
};

}  // namespace

std::unique_ptr<HttpServer> makeHttpServer() {
    return std::unique_ptr<HttpServer>(new HttplibHttpServer());
}

}  // namespace nas
