#include "config/Config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace nas {

namespace {

std::string trim(const std::string& s) {
    const std::size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const std::size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string toUpper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return s;
}

bool parseInt(const std::string& value, int min, int max, int& out) {
    const std::string v = trim(value);
    if (v.empty()) return false;
    char*      end = nullptr;
    const long n   = std::strtol(v.c_str(), &end, 10);
    if (end == v.c_str() || *end != '\0') return false;
    if (n < min || n > max) return false;
    out = static_cast<int>(n);
    return true;
}

bool parseBool(const std::string& value, bool& out) {
    std::string v = toUpper(trim(value));
    if (v == "1" || v == "TRUE" || v == "YES" || v == "ON") {
        out = true;
        return true;
    }
    if (v == "0" || v == "FALSE" || v == "NO" || v == "OFF") {
        out = false;
        return true;
    }
    return false;
}

std::vector<std::string> splitList(const std::string& value) {
    std::vector<std::string> out;
    std::size_t              pos = 0;
    while (pos <= value.size()) {
        const std::size_t comma = value.find(',', pos);
        const std::string item =
            trim(value.substr(pos, comma == std::string::npos ? std::string::npos
                                                              : comma - pos));
        if (!item.empty()) out.push_back(item);
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return out;
}

bool isLogLevel(const std::string& upper) {
    return upper == "TRACE" || upper == "DEBUG" || upper == "INFO" ||
           upper == "WARN" || upper == "ERROR" || upper == "FATAL" ||
           upper == "OFF";
}

}  // namespace

bool Config::set(const std::string& key, const std::string& value,
                 std::string& error) {
    if (key == "http.host") {
        const std::string v = trim(value);
        if (v.empty()) {
            error = "http.host 不能为空";
            return false;
        }
        httpHost = v;
    } else if (key == "http.port") {
        if (!parseInt(value, 1, 65535, httpPort)) {
            error = "http.port 必须是 1..65535 的整数";
            return false;
        }
    } else if (key == "share.roots") {
        shareRoots = splitList(value);
    } else if (key == "log.file") {
        const std::string v = trim(value);
        if (v.empty()) {
            error = "log.file 不能为空";
            return false;
        }
        logFile = v;
    } else if (key == "log.level") {
        const std::string v = toUpper(trim(value));
        if (!isLogLevel(v)) {
            error = "log.level 必须是 TRACE/DEBUG/INFO/WARN/ERROR/FATAL/OFF";
            return false;
        }
        logLevel = v;
    } else if (key == "web.dir") {
        webDir = trim(value);  // 允许为空（表示用内嵌 UI）
    } else if (key == "io.uring") {
        if (!parseBool(value, ioUseUring)) {
            error = "io.uring 必须是 true/false";
            return false;
        }
    } else if (key == "io.blockKb") {
        if (!parseInt(value, 4, 1 << 20, ioBlockKb)) {
            error = "io.blockKb 必须是 4..1048576 的整数";
            return false;
        }
    } else if (key == "io.depth") {
        if (!parseInt(value, 1, 4096, ioDepth)) {
            error = "io.depth 必须是 1..4096 的整数";
            return false;
        }
    } else if (key == "io.thresholdKb") {
        if (!parseInt(value, 0, 1 << 30, ioThresholdKb)) {
            error = "io.thresholdKb 必须是非负整数";
            return false;
        }
    } else if (key == "monitor.pollMs") {
        if (!parseInt(value, 1, 3600000, pollMs)) {
            error = "monitor.pollMs 必须是正整数";
            return false;
        }
    } else {
        error = "未知配置项: " + key;
        return false;
    }
    return true;
}

bool Config::loadFromString(const std::string& text, std::string& error) {
    std::istringstream is(text);
    std::string        line;
    int                lineNo = 0;
    while (std::getline(is, line)) {
        ++lineNo;
        const std::size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        const std::string trimmed = trim(line);
        if (trimmed.empty()) continue;

        const std::size_t eq = trimmed.find('=');
        if (eq == std::string::npos) {
            error = "第 " + std::to_string(lineNo) + " 行: 缺少 '='";
            return false;
        }
        const std::string key   = trim(trimmed.substr(0, eq));
        const std::string value = trim(trimmed.substr(eq + 1));

        std::string setError;
        if (!set(key, value, setError)) {
            error = "第 " + std::to_string(lineNo) + " 行: " + setError;
            return false;
        }
    }
    return true;
}

bool Config::loadFile(const std::string& path, std::string& error) {
    std::ifstream f(path);
    if (!f) {
        error = "无法打开配置文件: " + path;
        return false;
    }
    std::ostringstream buf;
    buf << f.rdbuf();
    return loadFromString(buf.str(), error);
}

}  // namespace nas
