#include "http/FileApi.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if defined(NAS_HAS_IO_URING)
#include "io/AsyncFileReader.h"
#include "io/BufferPool.h"
#endif

namespace nas {

namespace {

std::string param(const HttpRequest& req, const char* key) {
    const auto it = req.params.find(key);
    return it == req.params.end() ? std::string() : it->second;
}

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
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

HttpResponse jsonResponse(int status, std::string body) {
    HttpResponse res;
    res.status                  = status;
    res.headers["content-type"] = "application/json; charset=utf-8";
    res.body                    = std::move(body);
    return res;
}

HttpResponse errorResponse(FileError error, const std::string& message) {
    int status = 400;
    switch (error) {
        case FileError::Forbidden: status = 403; break;
        case FileError::NotFound:  status = 404; break;
        case FileError::Io:        status = 500; break;
        default:                   status = 400; break;
    }
    return jsonResponse(status, "{\"error\":\"" + escapeJson(message) + "\"}");
}

std::string mimeType(const std::string& path) {
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return "application/octet-stream";
    std::string ext = path.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (ext == "txt" || ext == "log" || ext == "md" || ext == "csv" ||
        ext == "ini" || ext == "conf" || ext == "sh" || ext == "c" ||
        ext == "cpp" || ext == "h" || ext == "hpp" || ext == "py")
        return "text/plain; charset=utf-8";
    if (ext == "json") return "application/json; charset=utf-8";
    if (ext == "xml") return "application/xml; charset=utf-8";
    if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
    if (ext == "css") return "text/css; charset=utf-8";
    if (ext == "js") return "application/javascript; charset=utf-8";
    if (ext == "pdf") return "application/pdf";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "png") return "image/png";
    if (ext == "gif") return "image/gif";
    if (ext == "webp") return "image/webp";
    if (ext == "bmp") return "image/bmp";
    if (ext == "svg") return "image/svg+xml";
    if (ext == "ico") return "image/x-icon";
    if (ext == "tif" || ext == "tiff") return "image/tiff";
    if (ext == "heic") return "image/heic";
    if (ext == "mp4") return "video/mp4";
    if (ext == "webm") return "video/webm";
    if (ext == "mkv") return "video/x-matroska";
    if (ext == "mov") return "video/quicktime";
    if (ext == "mp3") return "audio/mpeg";
    if (ext == "wav") return "audio/wav";
    if (ext == "flac") return "audio/flac";
    if (ext == "ogg") return "audio/ogg";
    if (ext == "m4a") return "audio/mp4";
    if (ext == "zip") return "application/zip";
    if (ext == "gz") return "application/gzip";
    if (ext == "tar") return "application/x-tar";
    return "application/octet-stream";
}

// RFC 3986 百分号编码，用于 Content-Disposition 的 filename*（支持中文名）。
std::string percentEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (const unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

// 生成 Content-Disposition：ASCII 回退 + RFC 5987 的 filename*（UTF-8）。
std::string contentDisposition(const std::string& filename, bool attachment) {
    std::string ascii;
    ascii.reserve(filename.size());
    for (const unsigned char c : filename) {
        if (c < 0x20 || c > 0x7E || c == '"' || c == '\\') {
            ascii += '_';
        } else {
            ascii += static_cast<char>(c);
        }
    }
    std::string out = attachment ? "attachment" : "inline";
    out += "; filename=\"" + ascii + "\"";
    out += "; filename*=UTF-8''" + percentEncode(filename);
    return out;
}

// 解析单段 Range：bytes=start-end / bytes=start- / bytes=-suffix。
bool parseRange(const std::string& value, std::uint64_t size,
                std::uint64_t& start, std::uint64_t& end) {
    if (value.rfind("bytes=", 0) != 0 || size == 0) return false;
    const std::string spec = value.substr(6);
    const std::size_t dash = spec.find('-');
    if (dash == std::string::npos) return false;
    const std::string a = spec.substr(0, dash);
    const std::string b = spec.substr(dash + 1);

    if (a.empty()) {  // bytes=-N ：末尾 N 字节
        if (b.empty()) return false;
        std::uint64_t n = std::strtoull(b.c_str(), nullptr, 10);
        if (n == 0) return false;
        if (n > size) n = size;
        start = size - n;
        end   = size - 1;
        return true;
    }
    start = std::strtoull(a.c_str(), nullptr, 10);
    end   = b.empty() ? size - 1 : std::strtoull(b.c_str(), nullptr, 10);
    if (start > end || start >= size) return false;
    if (end >= size) end = size - 1;
    return true;
}

std::string entryToJson(const FileEntry& e) {
    std::ostringstream os;
    os << "{\"name\":\"" << escapeJson(e.name) << "\",\"path\":\""
       << escapeJson(e.path) << "\",\"dir\":" << (e.isDir ? "true" : "false")
       << ",\"symlink\":" << (e.isSymlink ? "true" : "false")
       << ",\"size\":" << e.size << "}";
    return os.str();
}

void handleDownload(const FileService& files, const HttpRequest& req,
                    HttpResponse& res, const DownloadIoOptions& io) {
    (void)io;  // 未启用 io_uring 时该参数不参与编译
    const std::string path = param(req, "path");
    if (path.empty()) {
        res = errorResponse(FileError::Invalid, "缺少 path");
        return;
    }
    FileEntry entry;
    std::string message;
    const FileError err = files.stat(path, entry, message);
    if (err != FileError::None) {
        res = errorResponse(err, message);
        return;
    }
    if (entry.isDir) {
        res = errorResponse(FileError::Invalid, "不能下载目录");
        return;
    }

    const std::uint64_t size = entry.size;
    res.headers["accept-ranges"] = "bytes";
    res.headers["content-type"]  = mimeType(entry.path);

    // 默认 inline（浏览器可预览图片/PDF/视频）；?download=1 强制下载。
    const bool asAttachment = param(req, "download") == "1";
    res.headers["content-disposition"] =
        contentDisposition(entry.name, asAttachment);

    // Range 由本层处理（适配层已清空 httplib 的 ranges，不会重复处理）。
    std::uint64_t start = 0;
    std::uint64_t end   = size > 0 ? size - 1 : 0;
    const auto rangeIt = req.headers.find("range");
    if (rangeIt != req.headers.end() &&
        parseRange(rangeIt->second, size, start, end)) {
        res.status = 206;
        std::ostringstream cr;
        cr << "bytes " << start << "-" << end << "/" << size;
        res.headers["content-range"] = cr.str();
    }

    const std::uint64_t sliceLen = size > 0 ? (end - start + 1) : 0;
    res.contentLength = sliceLen;

#if defined(NAS_HAS_IO_URING)
    // io_uring 流水线：启用且文件够大时使用；否则回退同步读。
    if (io.useUring && sliceLen >= io.thresholdBytes) {
        res.provider = [path = entry.path, start, sliceLen,
                        blockSize = io.blockSize, depth = io.depth](
                           std::size_t offset, std::size_t /*length*/,
                           BodySink& sink) -> bool {
            if (offset != 0) return false;  // 一次性写完整段
            AsyncFileReader reader(blockSize, depth);
            if (!reader.valid() || !reader.start(path, start, sliceLen)) {
                return false;
            }
            for (;;) {
                BufferPool::Buffer b = reader.next();
                if (!b) break;  // EOF 哨兵
                if (b.length() == 0) continue;
                if (!sink.write(b.data(), b.length())) return false;
            }
            return true;
        };
        return;
    }
#endif

    res.provider = [canonical = entry.path, start](std::size_t offset,
                                                   std::size_t length,
                                                   BodySink& sink) -> bool {
        std::ifstream f(canonical, std::ios::binary);
        if (!f) return false;
        f.seekg(static_cast<std::streamoff>(start + offset));

        constexpr std::size_t kChunk = 64 * 1024;
        std::vector<char> buf(std::min(length, kChunk));
        std::size_t remaining = length;
        while (remaining > 0) {
            const std::size_t want = std::min(remaining, buf.size());
            f.read(buf.data(), static_cast<std::streamsize>(want));
            const std::size_t n = static_cast<std::size_t>(f.gcount());
            if (n != want) return false;  // 必须写满，避免错位损坏
            if (!sink.write(buf.data(), n)) return false;
            remaining -= n;
        }
        return true;
    };
}

}  // namespace

void registerFileRoutes(Router& router, const FileService& files,
                        const DownloadIoOptions& io) {
    router.add("GET", "/api/files", [&files](const HttpRequest& req) {
        const std::string path = param(req, "path");
        if (path.empty()) {
            return errorResponse(FileError::Invalid, "缺少 path");
        }
        std::vector<FileEntry> entries;
        std::string            message;
        const FileError err = files.list(path, entries, message);
        if (err != FileError::None) return errorResponse(err, message);

        std::ostringstream os;
        os << "{\"path\":\"" << escapeJson(path) << "\",\"entries\":[";
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (i) os << ",";
            os << entryToJson(entries[i]);
        }
        os << "]}";
        return jsonResponse(200, os.str());
    });

    router.add("GET", "/api/search", [&files](const HttpRequest& req) {
        const std::string path  = param(req, "path");
        const std::string query = param(req, "q");
        if (path.empty() || query.empty()) {
            return errorResponse(FileError::Invalid, "缺少 path/q");
        }
        std::size_t       limit      = 200;
        const std::string limitParam = param(req, "limit");
        if (!limitParam.empty()) {
            const long v = std::strtol(limitParam.c_str(), nullptr, 10);
            if (v > 0 && v <= 2000) limit = static_cast<std::size_t>(v);
        }

        std::vector<FileEntry> results;
        std::string            message;
        const FileError err =
            files.search(path, query, limit, results, message);
        if (err != FileError::None) return errorResponse(err, message);

        std::ostringstream os;
        os << "{\"root\":\"" << escapeJson(path) << "\",\"query\":\""
           << escapeJson(query) << "\",\"results\":[";
        for (std::size_t i = 0; i < results.size(); ++i) {
            if (i) os << ",";
            os << entryToJson(results[i]);
        }
        os << "]}";
        return jsonResponse(200, os.str());
    });

    router.add("GET", "/api/download", [&files, io](const HttpRequest& req) {
        HttpResponse res;
        handleDownload(files, req, res, io);
        return res;
    });

    router.add("PUT", "/api/file", [&files](const HttpRequest& req) {
        const std::string path = param(req, "path");
        if (path.empty()) return errorResponse(FileError::Invalid, "缺少 path");
        std::string message;
        const FileError err =
            files.writeFile(path, req.body.data(), req.body.size(), message);
        if (err != FileError::None) return errorResponse(err, message);
        std::ostringstream os;
        os << "{\"path\":\"" << escapeJson(path) << "\",\"size\":"
           << req.body.size() << "}";
        return jsonResponse(201, os.str());
    });

    router.add("DELETE", "/api/file", [&files](const HttpRequest& req) {
        const std::string path = param(req, "path");
        if (path.empty()) return errorResponse(FileError::Invalid, "缺少 path");
        const std::string recursiveParam = param(req, "recursive");
        const bool recursive =
            (recursiveParam == "1" || recursiveParam == "true");
        std::string message;
        const FileError err = files.remove(path, recursive, message);
        if (err != FileError::None) return errorResponse(err, message);
        return jsonResponse(200,
                            "{\"deleted\":\"" + escapeJson(path) + "\"}");
    });

    router.add("POST", "/api/mkdir", [&files](const HttpRequest& req) {
        const std::string path = param(req, "path");
        if (path.empty()) return errorResponse(FileError::Invalid, "缺少 path");
        std::string message;
        const FileError err = files.createDir(path, message);
        if (err != FileError::None) return errorResponse(err, message);
        return jsonResponse(201,
                            "{\"created\":\"" + escapeJson(path) + "\"}");
    });

    router.add("POST", "/api/rename", [&files](const HttpRequest& req) {
        const std::string from = param(req, "from");
        const std::string to   = param(req, "to");
        if (from.empty() || to.empty()) {
            return errorResponse(FileError::Invalid, "缺少 from/to");
        }
        std::string message;
        const FileError err = files.rename(from, to, message);
        if (err != FileError::None) return errorResponse(err, message);
        return jsonResponse(200, "{\"from\":\"" + escapeJson(from) +
                                     "\",\"to\":\"" + escapeJson(to) + "\"}");
    });
}

}  // namespace nas
