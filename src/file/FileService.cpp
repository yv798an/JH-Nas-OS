#include "file/FileService.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace nas {

namespace fs = std::filesystem;

namespace {

std::string toLower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

FileEntry makeEntry(const fs::path& path) {
    FileEntry entry;
    entry.name = path.filename().string();
    entry.path = path.string();

    std::error_code ec;
    const fs::file_status linkStatus = fs::symlink_status(path, ec);
    if (ec) return entry;

    entry.isSymlink = fs::is_symlink(linkStatus);
    entry.isDir     = fs::is_directory(linkStatus);
    if (!entry.isDir && !entry.isSymlink) {
        const std::uintmax_t size = fs::file_size(path, ec);
        if (!ec) entry.size = static_cast<std::uint64_t>(size);
    }
    return entry;
}

}  // namespace

FileService::FileService(std::vector<std::string> roots) {
    roots_.reserve(roots.size());
    for (const std::string& root : roots) {
        if (root.empty()) continue;
        std::error_code ec;
        const fs::path canonical = fs::weakly_canonical(fs::path(root), ec);
        roots_.push_back(ec ? root : canonical.string());
    }
}

bool FileService::isRoot(const std::string& canonical) const {
    for (const std::string& root : roots_) {
        if (canonical == root) return true;
    }
    return false;
}

FileError FileService::resolve(const std::string& requested,
                               std::string& canonical,
                               std::string& message) const {
    if (requested.empty()) {
        message = "path 不能为空";
        return FileError::Invalid;
    }
    const fs::path req(requested);
    if (!req.is_absolute()) {
        message = "path 必须是绝对路径";
        return FileError::Invalid;
    }

    std::error_code ec;
    // weakly_canonical 解析已存在部分的符号链接，其余词法规范化。
    const std::string resolved =
        fs::weakly_canonical(req, ec).string();
    if (ec) {
        message = "路径解析失败: " + ec.message();
        return FileError::Invalid;
    }

    for (const std::string& root : roots_) {
        if (resolved == root) {
            canonical = resolved;
            return FileError::None;
        }
        if (resolved.size() > root.size() &&
            resolved.compare(0, root.size(), root) == 0 &&
            resolved[root.size()] == '/') {
            canonical = resolved;
            return FileError::None;
        }
    }
    message = "路径越界（不在允许的共享根内）";
    return FileError::Forbidden;
}

FileError FileService::list(const std::string& dir,
                            std::vector<FileEntry>& out,
                            std::string& message) const {
    std::string canonical;
    const FileError err = resolve(dir, canonical, message);
    if (err != FileError::None) return err;

    std::error_code ec;
    if (!fs::is_directory(canonical, ec)) {
        message = "不是目录";
        return FileError::NotFound;
    }

    out.clear();
    fs::directory_iterator it(canonical,
                              fs::directory_options::skip_permission_denied,
                              ec);
    if (ec) {
        message = "无法读取目录: " + ec.message();
        return FileError::Io;
    }
    const fs::directory_iterator end;
    while (it != end) {
        out.push_back(makeEntry(it->path()));
        it.increment(ec);
        if (ec) {
            message = "读取目录失败: " + ec.message();
            return FileError::Io;
        }
    }
    std::sort(out.begin(), out.end(),
              [](const FileEntry& a, const FileEntry& b) {
                  if (a.isDir != b.isDir) return a.isDir > b.isDir;
                  return a.name < b.name;
              });
    return FileError::None;
}

FileError FileService::search(const std::string& root, const std::string& query,
                              std::size_t maxResults, std::vector<FileEntry>& out,
                              std::string& message) const {
    out.clear();
    if (query.empty()) {
        message = "q 不能为空";
        return FileError::Invalid;
    }

    std::string canonical;
    const FileError err = resolve(root, canonical, message);
    if (err != FileError::None) return err;

    std::error_code ec;
    if (!fs::is_directory(canonical, ec)) {
        message = "不是目录";
        return FileError::NotFound;
    }

    const std::string needle = toLower(query);
    std::vector<std::string> stack;
    stack.push_back(canonical);

    while (!stack.empty() && out.size() < maxResults) {
        std::string dir = std::move(stack.back());
        stack.pop_back();

        fs::directory_iterator it(
            dir, fs::directory_options::skip_permission_denied, ec);
        if (ec) {
            ec.clear();
            continue;
        }
        const fs::directory_iterator end;
        while (it != end && out.size() < maxResults) {
            std::error_code linkEc;
            const fs::file_status st = it->symlink_status(linkEc);
            if (!linkEc && !fs::is_symlink(st)) {
                const std::string name = it->path().filename().string();
                if (toLower(name).find(needle) != std::string::npos) {
                    out.push_back(makeEntry(it->path()));
                }
                if (fs::is_directory(st)) {
                    stack.push_back(it->path().string());
                }
            }
            it.increment(ec);
            if (ec) {
                ec.clear();
                break;
            }
        }
    }
    return FileError::None;
}

FileError FileService::stat(const std::string& path, FileEntry& out,
                            std::string& message) const {
    std::string canonical;
    const FileError err = resolve(path, canonical, message);
    if (err != FileError::None) return err;

    std::error_code ec;
    if (!fs::exists(canonical, ec)) {
        message = "文件不存在";
        return FileError::NotFound;
    }
    out = makeEntry(fs::path(canonical));
    return FileError::None;
}

FileError FileService::createDir(const std::string& path,
                                 std::string& message) const {
    std::string canonical;
    const FileError err = resolve(path, canonical, message);
    if (err != FileError::None) return err;

    std::error_code ec;
    if (!fs::create_directories(canonical, ec) && ec) {
        message = "创建目录失败: " + ec.message();
        return FileError::Io;
    }
    return FileError::None;
}

FileError FileService::remove(const std::string& path, bool recursive,
                              std::string& message) const {
    std::string canonical;
    const FileError err = resolve(path, canonical, message);
    if (err != FileError::None) return err;
    if (isRoot(canonical)) {
        message = "不允许操作共享根目录";
        return FileError::Forbidden;
    }

    std::error_code ec;
    if (!fs::exists(canonical, ec)) {
        message = "文件不存在";
        return FileError::NotFound;
    }
    if (recursive) {
        fs::remove_all(canonical, ec);
    } else {
        fs::remove(canonical, ec);
    }
    if (ec) {
        message = "删除失败: " + ec.message();
        return FileError::Io;
    }
    return FileError::None;
}

FileError FileService::rename(const std::string& from, const std::string& to,
                              std::string& message) const {
    std::string canonicalFrom;
    FileError err = resolve(from, canonicalFrom, message);
    if (err != FileError::None) return err;
    if (isRoot(canonicalFrom)) {
        message = "不允许操作共享根目录";
        return FileError::Forbidden;
    }

    std::string canonicalTo;
    err = resolve(to, canonicalTo, message);
    if (err != FileError::None) return err;
    if (isRoot(canonicalTo)) {
        message = "不允许操作共享根目录";
        return FileError::Forbidden;
    }

    std::error_code ec;
    if (!fs::exists(canonicalFrom, ec)) {
        message = "源文件不存在";
        return FileError::NotFound;
    }
    fs::rename(canonicalFrom, canonicalTo, ec);
    if (ec) {
        message = "重命名失败: " + ec.message();
        return FileError::Io;
    }
    return FileError::None;
}

FileError FileService::writeFile(const std::string& path, const char* data,
                                 std::size_t len, std::string& message) const {
    std::string canonical;
    const FileError err = resolve(path, canonical, message);
    if (err != FileError::None) return err;
    if (isRoot(canonical)) {
        message = "不能以共享根作为文件名";
        return FileError::Forbidden;
    }

    std::error_code ec;
    const fs::path parent = fs::path(canonical).parent_path();
    if (!fs::is_directory(parent, ec)) {
        message = "目标目录不存在";
        return FileError::NotFound;
    }

    const int fd = ::open(canonical.c_str(),
                          O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        message = std::string("无法写入文件: ") + std::strerror(errno);
        return FileError::Io;
    }

    const char* p         = data;
    std::size_t remaining = len;
    while (remaining > 0) {
        const ssize_t n = ::write(fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            message = std::string("写入失败: ") + std::strerror(errno);
            ::close(fd);
            return FileError::Io;
        }
        p += n;
        remaining -= static_cast<std::size_t>(n);
    }

    // 落盘：fsync 文件数据，再 fsync 父目录项。避免掉电/直接拔盘时文件丢失。
    if (::fsync(fd) != 0) {
        message = std::string("fsync 失败: ") + std::strerror(errno);
        ::close(fd);
        return FileError::Io;
    }
    ::close(fd);

    const int dirFd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirFd >= 0) {
        ::fsync(dirFd);
        ::close(dirFd);
    }
    return FileError::None;
}

}  // namespace nas
