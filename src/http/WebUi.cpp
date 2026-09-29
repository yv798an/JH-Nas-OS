#include "http/WebUi.h"

#include "web_index_generated.h"  // 由 CMake 从 web/index.html 生成

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

namespace nas {

namespace {

std::string escapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (const char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;
        }
    }
    return out;
}

HttpResponse htmlResponse(std::string html) {
    HttpResponse res;
    res.status                  = 200;
    res.headers["content-type"] = "text/html; charset=utf-8";
    res.body                    = std::move(html);
    return res;
}

// 优先读取 webDir/index.html（可运行时替换，免重编译）；读不到则用内嵌版本。
std::string loadIndexHtml(const std::string& webDir) {
    if (!webDir.empty()) {
        std::ifstream f(std::filesystem::path(webDir) / "index.html",
                        std::ios::binary);
        if (f) {
            std::ostringstream ss;
            ss << f.rdbuf();
            std::string html = ss.str();
            if (!html.empty()) return html;
        }
    }
    return std::string(reinterpret_cast<const char*>(kIndexHtml));
}

}  // namespace

void registerWebRoutes(Router& router, const std::vector<std::string>& roots,
                       const std::string& webDir) {
    router.add("GET", "/", [webDir](const HttpRequest&) {
        return htmlResponse(loadIndexHtml(webDir));
    });
    router.add("GET", "/index.html", [webDir](const HttpRequest&) {
        return htmlResponse(loadIndexHtml(webDir));
    });

    router.add("GET", "/api/roots", [roots](const HttpRequest&) {
        std::ostringstream os;
        os << "{\"roots\":[";
        for (std::size_t i = 0; i < roots.size(); ++i) {
            if (i) os << ",";
            os << "\"" << escapeJson(roots[i]) << "\"";
        }
        os << "]}";

        HttpResponse res;
        res.status                  = 200;
        res.headers["content-type"] = "application/json; charset=utf-8";
        res.body                    = os.str();
        return res;
    });
}

}  // namespace nas
