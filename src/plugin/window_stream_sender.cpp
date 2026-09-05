#include "plugin/window_stream_sender.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace hyprcapture {
namespace {

struct ScopedFd {
    int fd = -1;
    ScopedFd() = default;
    explicit ScopedFd(int next) : fd(next) {}
    ~ScopedFd() { if (fd >= 0) close(fd); }
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    ScopedFd(ScopedFd&& other) noexcept : fd(std::exchange(other.fd, -1)) {}
    ScopedFd& operator=(ScopedFd&& other) noexcept {
        if (this != &other) {
            if (fd >= 0) close(fd);
            fd = std::exchange(other.fd, -1);
        }
        return *this;
    }
};

bool unixAddress(const std::string& path, sockaddr_un& address, socklen_t& length) {
    if (path.empty() || path.size() >= sizeof(address.sun_path) || path.find('\0') != std::string::npos)
        return false;
    address = {};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.data(), path.size());
    length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
    return true;
}

bool waitForConnect(int fd, std::chrono::milliseconds timeout) {
    pollfd pollFd{.fd = fd, .events = POLLOUT, .revents = 0};
    const auto result = poll(&pollFd, 1, static_cast<int>(timeout.count()));
    if (result <= 0)
        return false;
    int socketError = 0;
    socklen_t length = sizeof(socketError);
    return getsockopt(fd, SOL_SOCKET, SO_ERROR, &socketError, &length) == 0 && socketError == 0;
}

std::chrono::milliseconds boundedWait(std::chrono::milliseconds configured) {
    return std::clamp(configured, std::chrono::milliseconds{1}, std::chrono::milliseconds{10});
}

bool writeAll(int fd, const std::vector<unsigned char>& bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto written = write(fd, bytes.data() + offset, bytes.size() - offset);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

std::optional<ScopedFd> sealedMemfd(const std::vector<unsigned char>& pixels) {
    const int raw = memfd_create("hyprcapture-window-stream", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (raw < 0)
        return std::nullopt;
    ScopedFd fd(raw);
    if (!writeAll(fd.fd, pixels))
        return std::nullopt;
    constexpr int seals = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
    if (fcntl(fd.fd, F_ADD_SEALS, seals) != 0)
        return std::nullopt;
    return fd;
}

} // namespace

WindowStreamSender::WindowStreamSender(WindowStreamSenderConfig config) : m_config(std::move(config)), m_worker([this](std::stop_token token) { run(token); }) {}

WindowStreamSender::~WindowStreamSender() {
    stop();
}

bool WindowStreamSender::submit(WindowStreamFrameMetadata metadata, std::vector<unsigned char> rgba) noexcept {
    if (!validWindowStreamFrameMetadata(metadata) || rgba.size() != metadata.payloadBytes) {
        m_failure.store(WindowStreamSenderFailure::InvalidFrame, std::memory_order_release);
        return false;
    }
    std::unique_lock lock(m_mutex, std::try_to_lock);
    const auto currentState = m_state.load(std::memory_order_acquire);
    if (!lock.owns_lock() || currentState == WindowStreamSenderState::Stopped || currentState == WindowStreamSenderState::Disconnected) {
        m_droppedFrames.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (m_latest)
        m_droppedFrames.fetch_add(1, std::memory_order_relaxed);
    m_latest = Frame{.metadata = metadata, .rgba = std::move(rgba)};
    lock.unlock();
    m_ready.notify_one();
    return true;
}

void WindowStreamSender::stop() {
    if (m_worker.joinable()) {
        m_worker.request_stop();
        m_ready.notify_all();
        m_worker.join();
    }
}

WindowStreamSenderState WindowStreamSender::state() const noexcept { return m_state.load(std::memory_order_acquire); }
WindowStreamSenderFailure WindowStreamSender::failure() const noexcept { return m_failure.load(std::memory_order_acquire); }
std::uint64_t WindowStreamSender::droppedFrames() const noexcept { return m_droppedFrames.load(std::memory_order_relaxed); }

std::optional<WindowStreamSender::Frame> WindowStreamSender::takeLatest() {
    std::scoped_lock lock(m_mutex);
    return std::exchange(m_latest, std::nullopt);
}

bool WindowStreamSender::connectUntil(std::stop_token stopToken, std::chrono::steady_clock::time_point deadline, int& connectedFd) {
    sockaddr_un address{};
    socklen_t length = 0;
    if (!unixAddress(m_config.socketPath, address, length)) {
        m_failure.store(WindowStreamSenderFailure::Socket, std::memory_order_release);
        return false;
    }
    while (!stopToken.stop_requested() && std::chrono::steady_clock::now() < deadline) {
        ScopedFd candidate(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
        if (candidate.fd < 0) {
            m_failure.store(WindowStreamSenderFailure::Socket, std::memory_order_release);
            return false;
        }
        const int result = connect(candidate.fd, reinterpret_cast<const sockaddr*>(&address), length);
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::milliseconds::zero())
            break;
        if (result == 0 || (result < 0 && errno == EINPROGRESS && waitForConnect(candidate.fd, std::min(boundedWait(m_config.retryInterval), remaining)))) {
            connectedFd = std::exchange(candidate.fd, -1);
            m_state.store(WindowStreamSenderState::Connected, std::memory_order_release);
            return true;
        }
        std::unique_lock lock(m_mutex);
        m_ready.wait_for(lock, stopToken, boundedWait(m_config.retryInterval), [] { return false; });
    }
    if (!stopToken.stop_requested())
        m_failure.store(WindowStreamSenderFailure::ConnectDeadline, std::memory_order_release);
    return false;
}

WindowStreamSender::SendResult WindowStreamSender::sendFrame(const Frame& frame, int socketFd) {
    auto fd = sealedMemfd(frame.rgba);
    if (!fd) {
        m_failure.store(WindowStreamSenderFailure::Socket, std::memory_order_release);
        return SendResult::Disconnected;
    }
    const auto header = encodeWindowStreamFrameHeader(frame.metadata);
    iovec iov{.iov_base = const_cast<unsigned char*>(header.data()), .iov_len = header.size()};
    alignas(cmsghdr) std::array<unsigned char, CMSG_SPACE(sizeof(int))> control{};
    msghdr message{};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    cmsghdr* cmsg = CMSG_FIRSTHDR(&message);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(cmsg), &fd->fd, sizeof(fd->fd));
    const auto sent = sendmsg(socketFd, &message, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (sent == static_cast<ssize_t>(header.size()))
        return SendResult::Sent;
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        m_failure.store(WindowStreamSenderFailure::SendWouldBlock, std::memory_order_release);
        return SendResult::WouldBlock;
    }
    m_failure.store(WindowStreamSenderFailure::Send, std::memory_order_release);
    return SendResult::Disconnected;
}

void WindowStreamSender::run(std::stop_token stopToken) {
    ScopedFd socket;
    // The receiver has bound its owner-private socket before it asks the
    // compositor to start this session.  Establish that connection at session
    // start, rather than making it conditional on the first completed GPU
    // readback.  A PBO that is temporarily not ready must not make the
    // receiver mistake an otherwise live producer for a failed session.
    m_state.store(WindowStreamSenderState::Connecting, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + m_config.connectDeadline;
    int connectedFd = -1;
    if (!connectUntil(stopToken, deadline, connectedFd)) {
        if (stopToken.stop_requested())
            m_state.store(WindowStreamSenderState::Stopped, std::memory_order_release);
        else
            m_state.store(WindowStreamSenderState::Disconnected, std::memory_order_release);
        return;
    }
    socket.fd = connectedFd;
    if (m_config.socketSendBufferBytes > 0 &&
        setsockopt(socket.fd, SOL_SOCKET, SO_SNDBUF, &m_config.socketSendBufferBytes, sizeof(m_config.socketSendBufferBytes)) != 0) {
        m_failure.store(WindowStreamSenderFailure::Socket, std::memory_order_release);
        m_state.store(WindowStreamSenderState::Disconnected, std::memory_order_release);
        return;
    }

    while (!stopToken.stop_requested()) {
        std::optional<Frame> frame;
        {
            std::unique_lock lock(m_mutex);
            m_ready.wait(lock, stopToken, [this] { return m_latest.has_value(); });
            if (stopToken.stop_requested())
                break;
            frame = std::exchange(m_latest, std::nullopt);
        }
        if (!frame)
            continue;
        // A frame arriving while connect retries were underway replaces this
        // stale candidate before any memfd or packet work begins.
        if (auto newer = takeLatest()) {
            frame = std::move(newer);
            m_droppedFrames.fetch_add(1, std::memory_order_relaxed);
        }
        const auto sent = stopToken.stop_requested() ? SendResult::WouldBlock : sendFrame(*frame, socket.fd);
        if (sent == SendResult::Disconnected) {
            m_state.store(WindowStreamSenderState::Disconnected, std::memory_order_release);
            break;
        }
    }
    if (stopToken.stop_requested())
        m_state.store(WindowStreamSenderState::Stopped, std::memory_order_release);
}

} // namespace hyprcapture
