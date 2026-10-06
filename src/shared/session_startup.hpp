#pragma once

#include <array>
#include <cerrno>
#include <chrono>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

namespace hyprcapture {

// One bounded metadata filename, never the pixel payload. The compositor must
// not block on a helper that is still initializing Qt or has already exited.
inline constexpr std::size_t MAX_STARTUP_PATH_BYTES = 4096;

inline bool validStartupPath(std::string_view path) {
    return !path.empty() && path.front() == '/' && path.size() <= MAX_STARTUP_PATH_BYTES && path.find('\0') == std::string_view::npos;
}

class SessionStartupChannel {
  public:
    SessionStartupChannel() { socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, m_fds.data()); }
    SessionStartupChannel(const SessionStartupChannel&) = delete;
    SessionStartupChannel& operator=(const SessionStartupChannel&) = delete;
    ~SessionStartupChannel() {
        for (const int fd : m_fds)
            if (fd >= 0)
                close(fd);
    }

    bool valid() const { return m_fds[0] >= 0 && m_fds[1] >= 0; }
    int childFd() const { return m_fds[1]; }
    void closeChild() {
        if (m_fds[1] >= 0)
            close(m_fds[1]);
        m_fds[1] = -1;
    }
    bool publish(std::string_view path) const {
        if (!validStartupPath(path))
            return false;
        ssize_t sent;
        do {
            sent = send(m_fds[0], path.data(), path.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
        } while (sent < 0 && errno == EINTR);
        return sent == static_cast<ssize_t>(path.size());
    }

  private:
    std::array<int, 2> m_fds{-1, -1};
};

inline std::optional<std::string> receiveStartupPath(int fd, int timeoutMs = 10000) {
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (remaining < 0)
            return std::nullopt;
        pollfd descriptor{fd, POLLIN, 0};
        const int ready = poll(&descriptor, 1, static_cast<int>(remaining));
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0)
            return std::nullopt;
        std::array<char, MAX_STARTUP_PATH_BYTES> buffer{};
        const auto size = recv(fd, buffer.data(), buffer.size(), MSG_TRUNC | MSG_DONTWAIT);
        if (size < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (size <= 0 || size > static_cast<ssize_t>(buffer.size()))
            return std::nullopt;
        std::string path(buffer.data(), static_cast<std::size_t>(size));
        return validStartupPath(path) ? std::optional<std::string>(std::move(path)) : std::nullopt;
    }
}

} // namespace hyprcapture
