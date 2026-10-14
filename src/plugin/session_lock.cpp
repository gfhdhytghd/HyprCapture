#include "plugin/session_lock.hpp"
#include "plugin/artifact_capture.hpp"
#include "plugin/recording.hpp"
#include "plugin/session_launcher.hpp"
#include "shared/session_lock.hpp"

#include <hyprland/src/Compositor.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/un.h>
#include <vector>

namespace hyprcapture {
namespace {
CHyprSignalListener lockListener;
wl_event_source* acceptSource = nullptr;
int listenFd = -1;
std::string socketPath;
std::vector<int> clients;
std::uint64_t generation = 0;

bool removeStaleSocket(const sockaddr_un& address) {
    struct stat original{}, current{};
    if (lstat(socketPath.c_str(), &original) != 0) return errno == ENOENT;
    if (!S_ISSOCK(original.st_mode) || original.st_uid != geteuid()) return false;
    const int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return false;
    const int connected = connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    const int error = errno;
    close(fd);
    if (connected == 0 || error != ECONNREFUSED) return false;
    return lstat(socketPath.c_str(), &current) == 0 && current.st_dev == original.st_dev && current.st_ino == original.st_ino &&
        unlink(socketPath.c_str()) == 0;
}

int acceptHelpers(int, uint32_t, void*) {
    // This callback only sends lock state, never input or focus.
    std::erase_if(clients, [](int fd) {
        char byte;
        if (recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT) < 0 && errno == EAGAIN)
            return false;
        close(fd);
        return true;
    });
    for (;;) {
        const int fd = accept4(listenFd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        ucred credentials{};
        socklen_t size = sizeof(credentials);
        const bool locked = sessionLocked();
        const std::string state = locked ? "locked" : "unlocked:" + sessionLockGeneration();
        const size_t bytes = state.size();
        if (clients.size() >= 256 || getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) != 0 || credentials.uid != geteuid() ||
            send(fd, state.data(), bytes, MSG_DONTWAIT | MSG_NOSIGNAL) != static_cast<ssize_t>(bytes) || locked) {
            close(fd);
            continue;
        }
        clients.push_back(fd);
    }
    return 0;
}

void onSessionLock() {
    ++generation;
    // Accept pre-existing connections while locked too, so a fast unlock cannot
    // revive a helper still initializing. EOF is also a terminal cancellation.
    acceptHelpers(0, 0, nullptr);
    for (const int fd : clients) {
        (void)send(fd, "locked", 6, MSG_DONTWAIT | MSG_NOSIGNAL);
        close(fd);
    }
    clients.clear();
    cancelCaptureHelpers();
    abortArtifactCapture();
    stopRecordingForSessionLock();
}
} // namespace

void initializeSessionLock() {
    if (!g_pSessionLockManager || !g_pCompositor)
        throw std::runtime_error("HyprCapture requires session lock management");
    generation = std::chrono::steady_clock::now().time_since_epoch().count();
    socketPath = sessionLockSocketPath();
    if (socketPath.empty() || socketPath.size() >= sizeof(sockaddr_un{}.sun_path))
        throw std::runtime_error("HyprCapture lock socket directory unavailable");
    listenFd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socketPath.c_str(), socketPath.size() + 1);
    // Only remove an owned, unchanged endpoint that refuses connections.
    if (listenFd < 0 || !removeStaleSocket(address) || bind(listenFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        if (listenFd >= 0) close(listenFd);
        listenFd = -1;
        socketPath.clear();
        throw std::runtime_error("HyprCapture lock socket bind failed");
    }
    if (chmod(socketPath.c_str(), 0600) == 0 && listen(listenFd, 16) == 0)
        acceptSource = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, listenFd, WL_EVENT_READABLE, acceptHelpers, nullptr);
    if (!acceptSource) {
        shutdownSessionLock();
        throw std::runtime_error("HyprCapture lock socket watch failed");
    }
    lockListener = g_pSessionLockManager->m_events.lock.listen(onSessionLock);
}

std::string sessionLockGeneration() {
    return std::to_string(generation);
}

void shutdownSessionLock() {
    lockListener.reset();
    if (acceptSource) wl_event_source_remove(acceptSource);
    acceptSource = nullptr;
    for (const int fd : clients) close(fd);
    clients.clear();
    if (listenFd >= 0) close(listenFd);
    listenFd = -1;
    if (!socketPath.empty()) unlink(socketPath.c_str());
    socketPath.clear();
}
} // namespace hyprcapture
