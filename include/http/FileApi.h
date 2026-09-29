#pragma once

// =====================================================================
//  http/FileApi.h - 文件 REST 路由
//
//  路由：
//    GET    /api/files?path=DIR            目录浏览
//    GET    /api/search?path=DIR&q=SUBSTR  递归搜索（名称子串，可带 limit）
//    GET    /api/download?path=FILE        下载（支持 Range）
//    PUT    /api/file?path=FILE            上传/覆盖（原始 body）
//    DELETE /api/file?path=FILE[&recursive=1]  删除
//    POST   /api/mkdir?path=DIR            建目录
//    POST   /api/rename?from=A&to=B        重命名/移动
//
//  所有路径都经 FileService 做白名单 + 规范化校验。
// =====================================================================

#include "file/FileService.h"
#include "http/Router.h"

#include <cstddef>
#include <cstdint>

namespace nas {

// 下载 I/O 选项。useUring 只有在编译期检测到 liburing 时才生效，否则自动回退同步。
struct DownloadIoOptions {
    bool          useUring       = false;
    std::size_t   blockSize      = 128 * 1024;
    unsigned      depth          = 8;
    std::uint64_t thresholdBytes = 4 * 1024 * 1024;  // 文件 >= 阈值才用 io_uring
};

void registerFileRoutes(Router& router, const FileService& files,
                        const DownloadIoOptions& io = {});

}  // namespace nas
