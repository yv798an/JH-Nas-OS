#pragma once

// =====================================================================
//  config/Config.h - 运行配置
//
//  优先级：内置默认 < 配置文件 < 命令行。
//  文件格式：每行 `key = value`，`#` 起注释，空行忽略。
//  对应镜像里的 /etc/nas/nas.conf（由 overlay 注入），见
//  docs/image-integration.md §4.3。
// =====================================================================

#include <string>
#include <vector>

namespace nas {

struct Config {
    // HTTP
    std::string httpHost = "0.0.0.0";
    int         httpPort = 8080;

    // 共享目录白名单（挂载点根），如 /media
    std::vector<std::string> shareRoots = {"/media"};

    // 日志
    std::string logFile  = "nas-server.log";
    std::string logLevel = "INFO";  // TRACE..OFF

    // 网页 UI 目录（可选）：存在 <webDir>/index.html 时优先使用，否则用内嵌版本。
    std::string webDir;

    // 下载 I/O（io_uring 需编译期检测到 liburing，见 CMake）。
    bool ioUseUring     = false;  // 是否对下载启用 io_uring
    int  ioBlockKb      = 128;    // 每次读的块大小
    int  ioDepth        = 8;      // in-flight 读队列深度
    int  ioThresholdKb  = 4096;   // 文件 >= 该大小才用 io_uring

    // 系统状态轮询周期（毫秒）
    int pollMs = 1000;

    // 应用单条键值。未知键或非法值返回 false 并写 error。
    bool set(const std::string& key, const std::string& value,
             std::string& error);

    // 解析整段文本；出错时 error 含行号。
    bool loadFromString(const std::string& text, std::string& error);

    // 读取文件；文件无法打开返回 false。
    bool loadFile(const std::string& path, std::string& error);
};

}  // namespace nas
