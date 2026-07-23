#include "plugin/recording_state.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace hyprcapture {
namespace {

using Json = nlohmann::ordered_json;

constexpr const char* SOCKET_FILENAME = "recording.sock";

void closeFd(int& fd) {
    if (fd >= 0)
        close(fd);
    fd = -1;
}

bool setCloseOnExec(int fd) {
    const int flags = fcntl(fd, F_GETFD);
    return flags >= 0 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

bool setNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool directoryOwnedByUser(const std::filesystem::path& path) {
    struct stat st {};
    const auto  native = path.string();
    return lstat(native.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == geteuid() && (st.st_mode & 0022) == 0;
}

bool ensurePrivateDirectory(const std::filesystem::path& path) {
    const auto native = path.string();
    if (native.empty() || (mkdir(native.c_str(), 0700) != 0 && errno != EEXIST))
        return false;

    struct stat st {};
    if (lstat(native.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid())
        return false;
    return (st.st_mode & 0777) == 0700 || chmod(native.c_str(), 0700) == 0;
}

std::filesystem::path runtimeDirectory() {
    if (const char* xdgRuntime = std::getenv("XDG_RUNTIME_DIR"); xdgRuntime && *xdgRuntime) {
        const std::filesystem::path base(xdgRuntime);
        if (base.is_absolute() && directoryOwnedByUser(base)) {
            const auto directory = base / "hyprcapture";
            if (ensurePrivateDirectory(directory))
                return directory;
        }
    }

    const auto directoryName = "hyprcapture-" + std::to_string(static_cast<unsigned long long>(geteuid()));
    for (const auto& base : {std::filesystem::path{"/dev/shm"}, std::filesystem::path{"/tmp"}}) {
        const auto directory = base / directoryName;
        if (ensurePrivateDirectory(directory))
            return directory;
    }
    return {};
}

std::string phaseName(RecordingState::Phase phase) {
    switch (phase) {
        case RecordingState::Phase::Recording: return "recording";
        case RecordingState::Phase::Finalizing: return "finalizing";
        case RecordingState::Phase::Inactive: return "inactive";
    }
    return "inactive";
}

std::int64_t elapsedMilliseconds(const RecordingState& state) {
    if (state.phase == RecordingState::Phase::Inactive)
        return 0;
    if (state.phase == RecordingState::Phase::Finalizing)
        return std::max<std::int64_t>(0, state.elapsedAtStop.count());
    return std::max<std::int64_t>(
        0,
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - state.startedAt).count());
}

std::string iso8601Utc(std::chrono::system_clock::time_point value) {
    if (value.time_since_epoch().count() == 0)
        return {};

    const std::time_t raw = std::chrono::system_clock::to_time_t(value);
    std::tm           utc {};
    if (!gmtime_r(&raw, &utc))
        return {};

    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

bool socketIsLive(const std::filesystem::path& path) {
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return false;

    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    const auto native = path.string();
    if (native.size() >= sizeof(address.sun_path)) {
        close(fd);
        return false;
    }
    std::memcpy(address.sun_path, native.c_str(), native.size() + 1);
    const bool connected = connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    close(fd);
    return connected;
}

bool removeStaleSocket(const std::filesystem::path& path, std::string* error) {
    const auto native = path.string();
    struct stat original {};
    if (lstat(native.c_str(), &original) != 0)
        return errno == ENOENT;
    if (!S_ISSOCK(original.st_mode) || original.st_uid != geteuid()) {
        if (error)
            *error = "recording state socket path is not a user-owned socket";
        return false;
    }
    if (socketIsLive(path)) {
        if (error)
            *error = "recording state socket is already in use";
        return false;
    }

    struct stat current {};
    if (lstat(native.c_str(), &current) != 0 || current.st_dev != original.st_dev || current.st_ino != original.st_ino) {
        if (error)
            *error = "recording state socket changed while checking stale state";
        return false;
    }
    if (unlink(native.c_str()) != 0) {
        if (error)
            *error = std::string("failed to remove stale recording state socket: ") + std::strerror(errno);
        return false;
    }
    return true;
}

} // namespace

std::string recordingStateJson(const RecordingState& state) {
    const auto phase = phaseName(state.phase);
    const auto elapsedMs = elapsedMilliseconds(state);
    return Json{
        {"version", 1},
        {"active", state.phase == RecordingState::Phase::Recording},
        {"phase", phase},
        {"backend", state.backend},
        {"output", state.output},
        {"elapsed", static_cast<double>(elapsedMs) / 1000.0},
        {"elapsedMs", elapsedMs},
        {"startedAt", iso8601Utc(state.startedAtWallClock)},
        {"mode", state.mode},
        {"format", state.format},
    }.dump();
}

std::filesystem::path defaultRecordingStateSocketPath() {
    const auto directory = runtimeDirectory();
    return directory.empty() ? std::filesystem::path{} : directory / SOCKET_FILENAME;
}

RecordingStateServer::~RecordingStateServer() {
    stop();
}

bool RecordingStateServer::start(const std::filesystem::path& requestedPath, std::string* error) {
    stop();

    const auto path = requestedPath.empty() ? defaultRecordingStateSocketPath() : requestedPath;
    if (path.empty() || !path.is_absolute() || !directoryOwnedByUser(path.parent_path())) {
        if (error)
            *error = "recording state socket needs a private user-owned runtime directory";
        return false;
    }

    const auto native = path.string();
    if (native.size() >= sizeof(sockaddr_un::sun_path)) {
        if (error)
            *error = "recording state socket path is too long";
        return false;
    }
    if (!removeStaleSocket(path, error))
        return false;

    int wakeFds[2] = {-1, -1};
    if (pipe(wakeFds) != 0 || !setCloseOnExec(wakeFds[0]) || !setCloseOnExec(wakeFds[1]) || !setNonBlocking(wakeFds[0]) || !setNonBlocking(wakeFds[1])) {
        closeFd(wakeFds[0]);
        closeFd(wakeFds[1]);
        if (error)
            *error = "failed to create recording state wake pipe";
        return false;
    }

    const int listenFd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (listenFd < 0) {
        closeFd(wakeFds[0]);
        closeFd(wakeFds[1]);
        if (error)
            *error = std::string("failed to create recording state socket: ") + std::strerror(errno);
        return false;
    }

    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, native.c_str(), native.size() + 1);
    if (bind(listenFd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        const int savedErrno = errno;
        close(listenFd);
        closeFd(wakeFds[0]);
        closeFd(wakeFds[1]);
        if (error)
            *error = std::string("failed to bind recording state socket: ") + std::strerror(savedErrno);
        return false;
    }
    if (chmod(native.c_str(), 0600) != 0 || listen(listenFd, 16) != 0) {
        const int savedErrno = errno;
        close(listenFd);
        unlink(native.c_str());
        closeFd(wakeFds[0]);
        closeFd(wakeFds[1]);
        if (error)
            *error = std::string("failed to initialize recording state socket: ") + std::strerror(savedErrno);
        return false;
    }

    struct stat st {};
    if (lstat(native.c_str(), &st) != 0 || !S_ISSOCK(st.st_mode) || st.st_uid != geteuid()) {
        close(listenFd);
        unlink(native.c_str());
        closeFd(wakeFds[0]);
        closeFd(wakeFds[1]);
        if (error)
            *error = "recording state socket ownership verification failed";
        return false;
    }

    {
        std::lock_guard lock(m_mutex);
        m_socketPath = path;
        m_listenFd = listenFd;
        m_wakeReadFd = wakeFds[0];
        m_wakeWriteFd = wakeFds[1];
        m_socketDevice = static_cast<unsigned long long>(st.st_dev);
        m_socketInode = static_cast<unsigned long long>(st.st_ino);
        m_state = {};
    }
    m_thread = std::thread([this] {
        run();
    });
    return true;
}

void RecordingStateServer::stop() {
    int wakeFd = -1;
    {
        std::lock_guard lock(m_mutex);
        wakeFd = m_wakeWriteFd;
    }
    if (wakeFd >= 0) {
        const char wake = 1;
        (void)write(wakeFd, &wake, 1);
    }
    if (m_thread.joinable())
        m_thread.join();

    std::filesystem::path path;
    unsigned long long    device = 0;
    unsigned long long    inode = 0;
    {
        std::lock_guard lock(m_mutex);
        closeFd(m_listenFd);
        closeFd(m_wakeReadFd);
        closeFd(m_wakeWriteFd);
        path = std::move(m_socketPath);
        device = m_socketDevice;
        inode = m_socketInode;
        m_socketDevice = 0;
        m_socketInode = 0;
        m_state = {};
    }

    if (!path.empty()) {
        struct stat st {};
        const auto native = path.string();
        if (lstat(native.c_str(), &st) == 0 && S_ISSOCK(st.st_mode) && st.st_uid == geteuid() &&
            static_cast<unsigned long long>(st.st_dev) == device && static_cast<unsigned long long>(st.st_ino) == inode)
            unlink(native.c_str());
    }
}

void RecordingStateServer::begin(std::string backend, std::filesystem::path output, std::string mode, std::string format) {
    std::lock_guard lock(m_mutex);
    m_state.phase = RecordingState::Phase::Recording;
    m_state.backend = std::move(backend);
    m_state.output = output.string();
    m_state.mode = std::move(mode);
    m_state.format = std::move(format);
    m_state.startedAt = std::chrono::steady_clock::now();
    m_state.startedAtWallClock = std::chrono::system_clock::now();
    m_state.elapsedAtStop = std::chrono::milliseconds(0);
}

void RecordingStateServer::beginFinalizing() {
    std::lock_guard lock(m_mutex);
    if (m_state.phase != RecordingState::Phase::Recording)
        return;
    m_state.elapsedAtStop =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_state.startedAt);
    m_state.phase = RecordingState::Phase::Finalizing;
}

void RecordingStateServer::clear() {
    std::lock_guard lock(m_mutex);
    m_state = {};
}

bool RecordingStateServer::running() const {
    std::lock_guard lock(m_mutex);
    return m_listenFd >= 0;
}

std::filesystem::path RecordingStateServer::socketPath() const {
    std::lock_guard lock(m_mutex);
    return m_socketPath;
}

std::string RecordingStateServer::snapshotJson() const {
    std::lock_guard lock(m_mutex);
    return recordingStateJson(m_state);
}

void RecordingStateServer::run() {
    int listenFd = -1;
    int wakeFd = -1;
    {
        std::lock_guard lock(m_mutex);
        listenFd = m_listenFd;
        wakeFd = m_wakeReadFd;
    }

    std::array<pollfd, 2> fds{{
        {.fd = listenFd, .events = POLLIN, .revents = 0},
        {.fd = wakeFd, .events = POLLIN, .revents = 0},
    }};
    while (true) {
        const int ready = poll(fds.data(), fds.size(), -1);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (fds[1].revents & (POLLIN | POLLHUP | POLLERR))
            break;
        if (!(fds[0].revents & POLLIN))
            continue;

        while (true) {
            const int client = accept4(listenFd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (client < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            const auto response = snapshotJson() + "\n";
            (void)send(client, response.data(), response.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
            close(client);
        }
        fds[0].revents = 0;
        fds[1].revents = 0;
    }
}

} // namespace hyprcapture
