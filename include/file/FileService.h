#pragma once

// =====================================================================
//  file/FileService.h - 文件系统业务层
//
//  提供目录浏览 / 搜索 / 信息 / 上传(写文件) / 删除 / 重命名 / 建目录，并对
//  所有请求做「白名单根目录 + 规范化路径」校验，防目录穿越与符号链接越界。
//
//  依赖 C++17 <filesystem>（Linux GCC / Buildroot GCC 均原生支持）。
// =====================================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nas {

enum class FileError {
    None = 0,
    Invalid,    // 参数非法 -> 400
    Forbidden,  // 越界/不允许 -> 403
    NotFound,   // 不存在 -> 404
    Io,         // 其它 IO/系统错误 -> 500
};

struct FileEntry {
    std::string   name;
    std::string   path;       // 规范化绝对路径
    bool          isDir     = false;
    bool          isSymlink = false;
    std::uint64_t size      = 0;
};

class FileService {
public:
    explicit FileService(std::vector<std::string> roots);

    // 解析请求路径并校验是否落在允许根内；成功返回规范化绝对路径。
    FileError resolve(const std::string& requested, std::string& canonical,
                      std::string& message) const;

    FileError list(const std::string& dir, std::vector<FileEntry>& out,
                   std::string& message) const;
    // 递归搜索：在 root 下按名称子串（大小写不敏感）匹配，最多 maxResults 条。
    FileError search(const std::string& root, const std::string& query,
                     std::size_t maxResults, std::vector<FileEntry>& out,
                     std::string& message) const;
    FileError stat(const std::string& path, FileEntry& out,
                   std::string& message) const;
    FileError createDir(const std::string& path, std::string& message) const;
    FileError remove(const std::string& path, bool recursive,
                     std::string& message) const;
    FileError rename(const std::string& from, const std::string& to,
                     std::string& message) const;
    FileError writeFile(const std::string& path, const char* data,
                        std::size_t len, std::string& message) const;

    const std::vector<std::string>& roots() const { return roots_; }

private:
    bool isRoot(const std::string& canonical) const;

    std::vector<std::string> roots_;  // 规范化后的允许根
};

}  // namespace nas
