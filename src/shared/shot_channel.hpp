#pragma once

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace hyprcapture::shot {
// Reuse the export_pipe namespace, with a private subdirectory per invocation.
inline std::string runtimeRoot() {
    for (const auto* base : {"/dev/shm", "/tmp"}) {
        auto path = std::string(base) + "/hyprcapture-" + std::to_string(geteuid());
        if (mkdir(path.c_str(), 0700) < 0 && errno != EEXIST) continue;
        struct stat st{};
        if (lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == geteuid() && (st.st_mode & 0777) == 0700)
            return path;
    }
    return {};
}

inline bool privateSocket(const std::string& path) {
    const auto root = runtimeRoot();
    if (root.empty() || path.empty() || path[0] != '/' || path.size() >= sizeof(sockaddr_un::sun_path) || path.find('\0') != std::string::npos)
        return false;
    std::filesystem::path p(path);
    if (p.lexically_normal() != p || !path.starts_with(root + "/")) return false;
    // Reject symlinks and writable parents, including the invocation directory.
    for (auto parent = p.parent_path(); parent != root; parent = parent.parent_path()) {
        struct stat st{};
        if (parent.empty() || parent == parent.root_path() || lstat(parent.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0022))
            return false;
    }
    struct stat st{};
    return lstat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode) && st.st_uid == geteuid() && !(st.st_mode & 0022);
}

inline int connectResult(const std::string& path) {
    if (!privateSocket(path)) { errno = EACCES; return -1; }
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) { close(fd); return -1; }
    ucred peer{};
    socklen_t size = sizeof(peer);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) || peer.uid != geteuid()) { close(fd); errno = EACCES; return -1; }
    return fd;
}

// Header: status (0 PNG, 1 cancel, 2 error), then unsigned big-endian length.
inline std::string header(unsigned char status, uint64_t size) {
    std::string data(9, '\0'); data[0] = status;
    for (int i = 8; i > 0; --i) { data[i] = size & 255; size >>= 8; }
    return data;
}
inline uint64_t payloadSize(std::string_view header) {
    uint64_t size = 0;
    for (int i = 1; i < 9; ++i) size = (size << 8) | static_cast<unsigned char>(header[i]);
    return size;
}
} // namespace hyprcapture::shot
