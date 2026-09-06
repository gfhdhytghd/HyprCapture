#include "plugin/artifact_capture.hpp"

#include "plugin/session_launcher.hpp"
#include "plugin/timing.hpp"
#include "plugin/notification.hpp"
#include "plugin/window_stream_sender.hpp"
#include "plugin/window_stream_control.hpp"
#include "plugin/window_stream_extent.hpp"
#include "plugin/window_capture_geometry.hpp"
#include "plugin/window_stream_cadence.hpp"
#include "plugin/window_gpu_export.hpp"
#include "plugin/window_gpu_sender.hpp"
#include "shared/rgba_transform.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>

#define private public
#define protected public
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/errorOverlay/Overlay.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/supplementary/WorkspaceAlgoMatcher.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/pointer/PointerManager.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/notification/NotificationOverlay.hpp>
#include <hyprland/src/render/gl/GLFramebuffer.hpp>
#include <hyprland/src/render/gl/GLTexture.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/render/Renderer.hpp>
#undef protected
#undef private

#include <GLES3/gl3.h>
#include <drm_fourcc.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <functional>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sstream>
#include <system_error>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unordered_map>
#include <utility>
#include <unistd.h>
#include <vector>

extern HANDLE g_pluginHandle;

namespace hyprcapture {
namespace {

using CFramebuffer = Render::IFramebuffer;
using CTexture = Render::ITexture;
using CHyprOpenGLImpl = Render::GL::CHyprOpenGLImpl;
using Render::GL::g_pHyprOpenGL;
using Render::RENDER_MODE_FULL_FAKE;
using Render::RENDER_PASS_ALL;
using Render::RPT_EXPORT;
using Render::eRenderPassMode;
using Json = nlohmann::ordered_json;

struct RgbaReadback {
    std::vector<unsigned char> pixels;
    int                        cropX = 0;
    int                        cropTopY = 0;
    int                        width = 0;
    int                        height = 0;
};

struct PixelBounds {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct RealBackgroundCaptureState {
    PHLWINDOW    window;
    std::size_t  targetIndex = 0;
    CFramebuffer* framebuffer = nullptr;
    int          cropX = 0;
    int          cropY = 0;
    int          width = 0;
    int          height = 0;
    bool         queued = false;
    bool         captured = false;
    bool         awaitingBlurBackground = false;
    bool         blurCaptured = false;
    RgbaReadback readback;
};

struct PendingRealBackgroundCapture {
    PHLWINDOW             window;
    PHLMONITOR            monitor;
    CBox                  artifactBox;
    std::filesystem::path path;
    std::size_t           windowIndex = 0;
};

struct ExportPipeMonitorPayload {
    MonitorInfo                info;
    std::vector<unsigned char> rgba;
};

struct ExportPipePayload {
    std::string                           responseFifo;
    std::string                           headerJson;
    std::vector<ExportPipeMonitorPayload> monitors;
};

struct ExportPipeWriter {
    std::jthread                  thread;
    std::shared_ptr<std::atomic_bool> done;
};

struct RealBackgroundCaptureTarget {
    PHLWINDOW window;
    CBox      artifactBox;
};

constexpr int WINDOW_ARTIFACT_CROP_PADDING = 128;
constexpr std::size_t MAX_SESSION_MONITORS = 64;
constexpr std::size_t MAX_SESSION_WINDOWS = 512;
constexpr std::size_t MAX_WINDOW_METADATA_BYTES = 4096;
constexpr std::size_t MAX_WINDOW_CAPTURE_REQUEST_BYTES = 64 * 1024;
constexpr std::size_t MAX_EXPORT_PIPE_REQUEST_BYTES = 64 * 1024;
constexpr std::size_t MAX_SESSION_JSON_BYTES = 8 * 1024 * 1024;
constexpr int         MAX_RGBA_READBACK_DIMENSION = 32768;
constexpr double      MAX_OVERVIEW_LOGICAL_COORDINATE = 1'000'000.0;
constexpr std::size_t MAX_RGBA_READBACK_BYTES = 512ULL * 1024ULL * 1024ULL;
constexpr std::size_t MAX_SESSION_ARTIFACT_BYTES = 768ULL * 1024ULL * 1024ULL;
constexpr std::size_t RGBA_BYTES_PER_PIXEL = 4;
constexpr auto        STALE_ARTIFACT_MAX_AGE = std::chrono::minutes(10);
constexpr auto        EXPORT_PIPE_REQUEST_TIMEOUT = std::chrono::seconds(2);
constexpr auto        EXPORT_PIPE_OPEN_TIMEOUT = std::chrono::seconds(5);
constexpr auto        EXPORT_PIPE_WRITE_TIMEOUT = std::chrono::seconds(30);
constexpr std::string_view EXPORT_PIPE_MAGIC = "HYPRCAP_PIPE_V1\n";
constexpr int         WINDOW_BACKGROUND_MIN_ALPHA = 32;
constexpr int         WINDOW_SHADOW_MAX_RGB = 32;
constexpr int         WINDOW_SHADOW_MAX_ALPHA = 223;
// Linux clamps and commonly doubles SO_SNDBUF, so this bounds packet backlog
// only approximately. It is deliberately small enough that the existing
// nonblocking EAGAIN path sheds stale SCM_RIGHTS packets instead of allowing
// an unbounded seqpacket queue of 96-byte headers plus large memfds.
constexpr int         WINDOW_STREAM_SOCKET_SEND_BUFFER_BYTES = 4096;
constexpr int         RECORDING_SHADOW_MAX_RGB = 64;
constexpr int         RECORDING_SHADOW_MAX_ALPHA = 249;

struct RgbaReadbackRegion {
    int         outputCropX = 0;
    int         outputCropTopY = 0;
    int         outputWidth = 0;
    int         outputHeight = 0;
    int         srcX = 0;
    int         srcTopY = 0;
    int         srcWidth = 0;
    int         srcHeight = 0;
    int         dstX = 0;
    int         dstY = 0;
    std::size_t outputBytes = 0;
    std::size_t sourceBytes = 0;
};

struct WindowRenderOptions {
    CHyprColor clearColor{0.0, 0.0, 0.0, 0.0};
    SP<CTexture> backgroundTexture;
    // All window consumers use the same tight local projection. Streaming
    // supplies its own allocation and performs readback/export after rendering.
    SP<CFramebuffer>* framebufferOverride = nullptr;
    bool       skipReadback = false;
    bool       asyncReadback = false;
    bool       clipBackgroundToWindow = false;
    bool       postprocessAlpha = true;
};

struct ShadowColorBytes {
    int r = 0;
    int g = 0;
    int b = 0;
    int a = 0;
};

struct ShadowRenderGeometry {
    CBox   shadowBox;
    CBox   windowCutoutBox;
    double range = 0.0;
    double rounding = 0.0;
    double windowRounding = 0.0;
    double roundingPower = 2.0;
    int    shadowPower = 3;
    bool   sharp = false;
};

ShadowColorBytes shadowColorBytes(const PHLWINDOW& window);
void             repairTransparentShadow(RgbaReadback& readback, const CBox& artifactBox, const CBox& visibleBox, const PHLWINDOW& window);
void             expandReadbackToShadowBounds(RgbaReadback& readback, CBox& artifactBox, const CBox& visibleBox, const PHLWINDOW& window);
RgbaReadback     normalizeMonitorReadbackToLogicalOrientation(RgbaReadback readback, int transform);

struct ArtifactBudget {
    std::size_t remaining = MAX_SESSION_ARTIFACT_BYTES;

    bool canFit(std::size_t bytes) const {
        return bytes > 0 && bytes <= remaining;
    }

    bool consume(std::size_t bytes) {
        if (!canFit(bytes))
            return false;
        remaining -= bytes;
        return true;
    }
};

struct RealBackgroundRecordingCache {
    SP<CFramebuffer> framebuffer;
    SP<CTexture>    texture;
    int             width = 0;
    int             height = 0;
};

struct AsyncPboReadbackState {
    static constexpr int BUFFER_COUNT = 3;

    GLuint      buffers[BUFFER_COUNT] = {0, 0, 0};
    std::size_t bytes = 0;
    int         width = 0;
    int         height = 0;
    int         pending = 0;
    int         next = 0;
};

AsyncPboReadbackState g_windowRecordingPboReadback;

struct WindowStreamPboReadbackState {
    static constexpr std::size_t BUFFER_COUNT = WindowStreamPboMetadataSlots::BUFFER_COUNT;

    SP<CFramebuffer> framebuffer;
    GLuint           buffers[BUFFER_COUNT] = {};
    GLsync           fences[BUFFER_COUNT] = {};
    std::size_t      bytes = 0;
    int              width = 0;
    int              height = 0;
    std::size_t      pending = 0;
    std::size_t      next = 0;
    std::string      windowAddress;
    Rect             relativeVisibleGeometry;
    struct SlotGeometry {
        CBox          fullBox;
        CBox          visibleBox;
        std::uint64_t geometryEpoch = 0;
        std::uint64_t captureMonotonicNs = 0;
    } slotGeometry[BUFFER_COUNT];
    WindowStreamPboMetadataSlots metadataSlots;
};

WindowStreamPboReadbackState g_windowStreamPboReadback;

// Stream setup is intentionally quiet in the normal case.  These bounded
// diagnostics make a failed live capture attributable without turning a
// 60-fps timer into a log flood: each reason is emitted at most six times.
std::unordered_map<std::string, std::uint8_t> g_windowStreamDiagnosticCounts;

void noteWindowStreamDiagnostic(std::string_view reason) {
    auto& count = g_windowStreamDiagnosticCounts[std::string(reason)];
    if (count >= 6)
        return;
    ++count;
    Log::logger->log(Log::WARN, "[hyprcapture] window stream diagnostic {} (#{})", reason, count);
}

struct WindowStreamSession {
    std::string                      id;
    std::string                      windowAddress;
    CaptureDefaults                  defaults;
    int                              fps = 60;
    std::uint64_t                    sequence = 0;
    std::uint64_t                    geometryEpoch = 1;
    Rect                             lastWindowGeometry;
    Rect                             lastMonitorGeometry;
    double                           lastMonitorScale = 0.0;
    WindowStreamCadence              cadence;
    SP<CEventLoopTimer>              timer;
    // This timer is armed only while the single stream PBO is pending.  It
    // gives a completed readback a chance to reach the sender between 60 Hz
    // render ticks, without adding a second render cadence.
    SP<CEventLoopTimer>              drainTimer;
    WindowStreamDrainPoll            drainPoll;
    std::unique_ptr<WindowStreamSender> sender;
    // GPU frames own their exported FBO until the peer's exact HCGR release.
    // It is intentionally a separate allocation from the CPU PBO path.
    WindowStreamTransport            transport = WindowStreamTransport::Cpu;
    std::unique_ptr<WindowGpuSender> gpuSender;
    SP<CFramebuffer>                 gpuFramebuffer;
    int                              gpuFramebufferWidth = 0;
    int                              gpuFramebufferHeight = 0;
    WindowGpuExportCache             gpuExport;
    std::int64_t                     gpuNextDueUs = 0;
    struct NotificationSource {
        wl_event_source* value = nullptr;
        void reset() { if (value) wl_event_source_remove(std::exchange(value, nullptr)); }
        ~NotificationSource() { reset(); }
    } gpuNotification;
    CHyprSignalListener              gpuTargetUnmapNotification;
};

std::unique_ptr<WindowStreamSession> g_windowStreamSession;
std::mutex            g_exportPipeWritersMutex;
std::vector<ExportPipeWriter> g_exportPipeWriters;

Rect toRect(const CBox& box) {
    return {.x = box.x, .y = box.y, .width = box.w, .height = box.h};
}

Rect monitorRect(const PHLMONITOR& monitor) {
    return {.x = monitor->m_position.x, .y = monitor->m_position.y, .width = monitor->m_size.x, .height = monitor->m_size.y};
}

bool intersects(const Rect& a, const Rect& b) {
    return a.x < b.x + b.width && a.x + a.width > b.x && a.y < b.y + b.height && a.y + a.height > b.y;
}

bool contains(const Rect& rect, const Point& point) {
    return point.x >= rect.x && point.x < rect.x + rect.width && point.y >= rect.y && point.y < rect.y + rect.height;
}

Rect intersection(const Rect& a, const Rect& b) {
    const double x1 = std::max(a.x, b.x);
    const double y1 = std::max(a.y, b.y);
    const double x2 = std::min(a.x + a.width, b.x + b.width);
    const double y2 = std::min(a.y + a.height, b.y + b.height);
    if (x2 <= x1 || y2 <= y1)
        return {};
    return {.x = x1, .y = y1, .width = x2 - x1, .height = y2 - y1};
}

bool ensurePrivateDirectory(const std::filesystem::path& path) {
    const auto native = path.string();
    if (native.empty())
        return false;

    if (mkdir(native.c_str(), 0700) != 0 && errno != EEXIST)
        return false;

    struct stat st {};
    if (lstat(native.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid())
        return false;

    if ((st.st_mode & 0777) != 0700 && chmod(native.c_str(), 0700) != 0)
        return false;

    return true;
}

bool hasWritableGroupOrOther(mode_t mode) {
    return (mode & 0022) != 0;
}

std::filesystem::path privateRuntimeRoot() {
    const auto rootName = "hyprcapture-" + std::to_string(static_cast<unsigned long long>(geteuid()));
    for (const auto& base : {std::filesystem::path{"/dev/shm"}, std::filesystem::path{"/tmp"}, std::filesystem::temp_directory_path()}) {
        const auto root = base / rootName;
        if (!ensurePrivateDirectory(root))
            continue;

        std::error_code ec;
        const auto      canonical = std::filesystem::weakly_canonical(root, ec);
        if (!ec)
            return canonical;
    }
    return {};
}

bool pathIsInPrivateRuntimeRoot(const std::filesystem::path& path) {
    std::error_code ec;
    const auto      root = privateRuntimeRoot();
    if (root.empty())
        return false;

    const auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (ec)
        return false;

    const auto rootNative = root.native();
    const auto pathNative = canonical.native();
    return pathNative == rootNative || pathNative.starts_with(rootNative + "/");
}

std::optional<std::string> readPrivateRequestFile(const std::string& rawPath) {
    const std::filesystem::path path(rawPath);
    if (rawPath.empty() || !path.is_absolute() || !pathIsInPrivateRuntimeRoot(path))
        return std::nullopt;

    const auto native = path.string();
    struct stat st {};
    if (stat(native.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || hasWritableGroupOrOther(st.st_mode) || st.st_size < 0 ||
        static_cast<std::uintmax_t>(st.st_size) > MAX_WINDOW_CAPTURE_REQUEST_BYTES)
        return std::nullopt;

    std::ifstream file(path, std::ios::binary);
    if (!file)
        return std::nullopt;

    std::ostringstream out;
    out << file.rdbuf();
    std::error_code ec;
    std::filesystem::remove(path, ec);
    const auto value = out.str();
    if (value.empty() || value.size() > MAX_WINDOW_CAPTURE_REQUEST_BYTES)
        return std::nullopt;
    return value;
}

bool trustedPrivateFifoPath(const std::string& rawPath) {
    const std::filesystem::path path(rawPath);
    if (rawPath.empty() || !path.is_absolute() || !pathIsInPrivateRuntimeRoot(path))
        return false;

    const auto native = path.string();
    struct stat st {};
    if (lstat(native.c_str(), &st) != 0 || !S_ISFIFO(st.st_mode) || st.st_uid != geteuid() || hasWritableGroupOrOther(st.st_mode))
        return false;

    return true;
}

bool trustedPrivateStreamSocketPath(const std::string& rawPath) {
    const std::filesystem::path path(rawPath);
    if (rawPath.empty() || !path.is_absolute() || rawPath.find('\0') != std::string::npos || rawPath.size() >= sizeof(sockaddr_un::sun_path) ||
        !pathIsInPrivateRuntimeRoot(path.parent_path()))
        return false;
    struct stat st {};
    const auto native = path.string();
    return lstat(native.c_str(), &st) == 0 && S_ISSOCK(st.st_mode) && st.st_uid == geteuid() && !hasWritableGroupOrOther(st.st_mode);
}

int timeoutUntil(const std::chrono::steady_clock::time_point& deadline) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    return remaining.count() <= 0 ? 0 : static_cast<int>(std::min<std::int64_t>(remaining.count(), 250));
}

std::optional<std::string> readPrivateFifoLine(const std::string& rawPath) {
    if (!trustedPrivateFifoPath(rawPath))
        return std::nullopt;

    const auto native = std::filesystem::path(rawPath).string();
    const int  fd = open(native.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return std::nullopt;

    const auto deadline = std::chrono::steady_clock::now() + EXPORT_PIPE_REQUEST_TIMEOUT;
    std::string value;
    std::array<char, 4096> buffer {};

    while (std::chrono::steady_clock::now() < deadline) {
        const ssize_t chunk = read(fd, buffer.data(), buffer.size());
        if (chunk > 0) {
            value.append(buffer.data(), static_cast<std::size_t>(chunk));
            if (value.size() > MAX_EXPORT_PIPE_REQUEST_BYTES) {
                close(fd);
                return std::nullopt;
            }
            if (const auto newline = value.find('\n'); newline != std::string::npos) {
                value.resize(newline);
                close(fd);
                return value.empty() ? std::nullopt : std::optional<std::string>{value};
            }
            continue;
        }

        if (chunk < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            close(fd);
            return std::nullopt;
        }

        pollfd pfd{.fd = fd, .events = POLLIN | POLLHUP | POLLERR, .revents = 0};
        const int ready = poll(&pfd, 1, timeoutUntil(deadline));
        if (ready < 0 && errno != EINTR) {
            close(fd);
            return std::nullopt;
        }
    }

    close(fd);
    return std::nullopt;
}

std::string jsonStringValue(const Json& obj, const char* key) {
    const auto it = obj.find(key);
    return it != obj.end() && it->is_string() ? it->get<std::string>() : std::string {};
}

std::string validateExportPipeRequest(const std::string& requestJson, std::string& responseFifo) {
    const auto root = Json::parse(requestJson, nullptr, false);
    if (root.is_discarded() || !root.is_object())
        return "invalid export pipe request json";

    const auto versionIt = root.find("version");
    if (versionIt != root.end() && (!versionIt->is_number_integer() || versionIt->get<int>() != 1))
        return "unsupported export pipe protocol version";

    responseFifo = jsonStringValue(root, "responseFifo");
    if (!trustedPrivateFifoPath(responseFifo)) {
        responseFifo.clear();
        return "invalid export pipe response fifo";
    }

    const auto mode = jsonStringValue(root, "mode");
    if (!mode.empty() && parseCaptureMode(mode, CaptureMode::Fullscreen) != CaptureMode::Fullscreen)
        return "export pipe only supports fullscreen mode";

    const auto scope = jsonStringValue(root, "fullscreenScope");
    if (!scope.empty() && parseFullscreenScope(scope, FullscreenScope::All) != FullscreenScope::All)
        return "export pipe only supports fullscreenScope=all";

    return {};
}

std::string exportPipeErrorHeader(const std::string& error) {
    const auto safeError = error.size() <= MAX_WINDOW_METADATA_BYTES ? error : error.substr(0, MAX_WINDOW_METADATA_BYTES);
    return Json{{"ok", false}, {"version", 1}, {"error", safeError}}.dump(-1, ' ', false, Json::error_handler_t::replace);
}

bool writePrivateResponseFile(const std::string& rawPath, std::string_view bytes) {
    const std::filesystem::path path(rawPath);
    if (rawPath.empty() || bytes.empty() || bytes.size() > MAX_SESSION_JSON_BYTES || !path.is_absolute())
        return false;
    if (!pathIsInPrivateRuntimeRoot(path.parent_path()))
        return false;

    const auto native = path.string();
    const int  fd = open(native.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return false;

    const char* data = bytes.data();
    std::size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t chunk = write(fd, data + written, bytes.size() - written);
        if (chunk < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            unlink(native.c_str());
            return false;
        }
        if (chunk == 0) {
            close(fd);
            unlink(native.c_str());
            return false;
        }
        written += static_cast<std::size_t>(chunk);
    }

    if (close(fd) != 0) {
        unlink(native.c_str());
        return false;
    }
    return true;
}

bool isGeneratedSessionRootName(const std::string& name) {
    const auto dash = name.find('-');
    if (dash == std::string::npos || dash == 0 || dash + 1 >= name.size() || name.find('-', dash + 1) != std::string::npos)
        return false;

    return std::all_of(name.begin(), name.end(), [](unsigned char ch) {
        return ch == '-' || std::isxdigit(ch);
    });
}

void cleanupStaleArtifactRoots(const std::filesystem::path& userRoot) {
    std::error_code ec;
    const auto      now = std::filesystem::file_time_type::clock::now();
    for (std::filesystem::directory_iterator it(userRoot, ec), end; !ec && it != end; it.increment(ec)) {
        const auto path = it->path();
        if (!isGeneratedSessionRootName(path.filename().string()))
            continue;

        std::error_code entryEc;
        const auto      status = it->symlink_status(entryEc);
        if (entryEc || !std::filesystem::is_directory(status))
            continue;

        const auto modified = std::filesystem::last_write_time(path, entryEc);
        if (entryEc || now - modified < STALE_ARTIFACT_MAX_AGE)
            continue;

        std::filesystem::remove_all(path, entryEc);
    }
}

std::filesystem::path artifactRoot(const std::string& sessionId) {
    const auto rootName = "hyprcapture-" + std::to_string(static_cast<unsigned long long>(geteuid()));
    for (const auto& base : {std::filesystem::path{"/dev/shm"}, std::filesystem::path{"/tmp"}, std::filesystem::temp_directory_path()}) {
        const auto userRoot = base / rootName;
        if (!ensurePrivateDirectory(userRoot))
            continue;
        cleanupStaleArtifactRoots(userRoot);

        const auto root = userRoot / sessionId;
        if (ensurePrivateDirectory(root))
            return root;
    }

    return {};
}

std::vector<std::filesystem::path> artifactRootCandidates(const std::string& sessionId) {
    const auto rootName = "hyprcapture-" + std::to_string(static_cast<unsigned long long>(geteuid()));
    std::vector<std::filesystem::path> roots;
    for (const auto& base : {std::filesystem::path{"/dev/shm"}, std::filesystem::path{"/tmp"}, std::filesystem::temp_directory_path()}) {
        const auto root = base / rootName / sessionId;
        if (std::find(roots.begin(), roots.end(), root) == roots.end())
            roots.push_back(root);
    }
    return roots;
}

std::string boundedString(const std::string& value, std::size_t maxBytes) {
    if (value.size() <= maxBytes)
        return value;
    return value.substr(0, maxBytes);
}

Json exportPipeRectJson(const Rect& rect) {
    return Json{
        {"x", rect.x},
        {"y", rect.y},
        {"width", rect.width},
        {"height", rect.height},
    };
}

std::string exportPipeOkHeader(const std::vector<ExportPipeMonitorPayload>& monitors) {
    Json root{
        {"ok", true},
        {"version", 1},
        {"format", "rgba8888"},
        {"topDown", true},
    };

    root["monitors"] = Json::array();
    for (const auto& monitor : monitors) {
        root["monitors"].push_back(Json{
            {"name", boundedString(monitor.info.name, MAX_WINDOW_METADATA_BYTES)},
            {"geometry", exportPipeRectJson(monitor.info.logicalGeometry)},
            {"scale", monitor.info.scale},
            {"transform", monitor.info.transform},
            {"artifactWidth", monitor.info.artifactWidth},
            {"artifactHeight", monitor.info.artifactHeight},
            {"byteLength", monitor.rgba.size()},
        });
    }

    return root.dump(-1, ' ', false, Json::error_handler_t::replace);
}

void blockSigpipeForCurrentThread() {
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
}

std::optional<int> openExportPipeWriter(const std::string& rawPath, std::stop_token stopToken) {
    if (!trustedPrivateFifoPath(rawPath))
        return std::nullopt;

    const auto native = std::filesystem::path(rawPath).string();
    const auto deadline = std::chrono::steady_clock::now() + EXPORT_PIPE_OPEN_TIMEOUT;
    while (!stopToken.stop_requested() && std::chrono::steady_clock::now() < deadline) {
        const int fd = open(native.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
        if (fd >= 0)
            return fd;

        if (errno != ENXIO && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
            return std::nullopt;

        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    return std::nullopt;
}

bool waitForExportPipeWritable(int fd, const std::chrono::steady_clock::time_point& deadline, std::stop_token stopToken) {
    while (!stopToken.stop_requested() && std::chrono::steady_clock::now() < deadline) {
        pollfd pfd{.fd = fd, .events = POLLOUT | POLLHUP | POLLERR, .revents = 0};
        const int ready = poll(&pfd, 1, timeoutUntil(deadline));
        if (ready > 0)
            return (pfd.revents & POLLOUT) != 0;
        if (ready < 0 && errno != EINTR)
            return false;
    }

    return false;
}

bool writeExportPipeBytes(int fd, const unsigned char* data, std::size_t size, const std::chrono::steady_clock::time_point& deadline, std::stop_token stopToken) {
    std::size_t written = 0;
    while (written < size && !stopToken.stop_requested() && std::chrono::steady_clock::now() < deadline) {
        const auto remaining = size - written;
        const auto chunkSize = std::min<std::size_t>(remaining, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        const ssize_t chunk = write(fd, data + written, chunkSize);
        if (chunk > 0) {
            written += static_cast<std::size_t>(chunk);
            continue;
        }

        if (chunk < 0 && errno == EINTR)
            continue;
        if (chunk < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (waitForExportPipeWritable(fd, deadline, stopToken))
                continue;
        }
        return false;
    }

    return written == size;
}

bool writeExportPipeString(int fd, const std::string& value, const std::chrono::steady_clock::time_point& deadline, std::stop_token stopToken) {
    return writeExportPipeBytes(fd, reinterpret_cast<const unsigned char*>(value.data()), value.size(), deadline, stopToken);
}

void exportPipeWriterMain(std::stop_token stopToken, ExportPipePayload payload, std::shared_ptr<std::atomic_bool> done) {
    blockSigpipeForCurrentThread();
    const auto markDone = [&]() {
        if (done)
            done->store(true, std::memory_order_release);
    };

    const auto fd = openExportPipeWriter(payload.responseFifo, stopToken);
    if (!fd) {
        markDone();
        return;
    }

    const auto deadline = std::chrono::steady_clock::now() + EXPORT_PIPE_WRITE_TIMEOUT;
    const std::string prefix = std::string(EXPORT_PIPE_MAGIC) + std::to_string(payload.headerJson.size()) + "\n" + payload.headerJson;
    bool ok = writeExportPipeString(*fd, prefix, deadline, stopToken);
    for (const auto& monitor : payload.monitors) {
        if (!ok)
            break;
        ok = writeExportPipeBytes(*fd, monitor.rgba.data(), monitor.rgba.size(), deadline, stopToken);
    }

    close(*fd);
    markDone();
}

void reapExportPipeWritersLocked() {
    for (auto it = g_exportPipeWriters.begin(); it != g_exportPipeWriters.end();) {
        if (!it->done || !it->done->load(std::memory_order_acquire)) {
            ++it;
            continue;
        }

        if (it->thread.joinable())
            it->thread.join();
        it = g_exportPipeWriters.erase(it);
    }
}

void startExportPipeWriter(ExportPipePayload payload) {
    auto done = std::make_shared<std::atomic_bool>(false);
    ExportPipeWriter writer{
        .thread = std::jthread(exportPipeWriterMain, std::move(payload), done),
        .done = done,
    };

    std::lock_guard lock(g_exportPipeWritersMutex);
    reapExportPipeWritersLocked();
    g_exportPipeWriters.push_back(std::move(writer));
}

void startExportPipeErrorWriter(const std::string& responseFifo, const std::string& error) {
    if (responseFifo.empty())
        return;
    startExportPipeWriter({
        .responseFifo = responseFifo,
        .headerJson = exportPipeErrorHeader(error),
    });
}

void shutdownExportPipeWriters() {
    std::vector<ExportPipeWriter> writers;
    {
        std::lock_guard lock(g_exportPipeWritersMutex);
        writers = std::move(g_exportPipeWriters);
        g_exportPipeWriters.clear();
    }

    for (auto& writer : writers)
        writer.thread.request_stop();
    for (auto& writer : writers) {
        if (writer.thread.joinable())
            writer.thread.join();
    }
}

bool containsPath(const std::vector<std::filesystem::path>& paths, const std::filesystem::path& path) {
    return std::find(paths.begin(), paths.end(), path) != paths.end();
}

int clampedIntFromDouble(double value) {
    if (!std::isfinite(value))
        return value < 0.0 ? std::numeric_limits<int>::min() : std::numeric_limits<int>::max();
    if (value <= static_cast<double>(std::numeric_limits<int>::min()))
        return std::numeric_limits<int>::min();
    if (value >= static_cast<double>(std::numeric_limits<int>::max()))
        return std::numeric_limits<int>::max();
    return static_cast<int>(value);
}

int positiveIntFromDouble(double value) {
    if (!std::isfinite(value) || value <= 0.0)
        return 1;
    if (value >= static_cast<double>(std::numeric_limits<int>::max()))
        return std::numeric_limits<int>::max();
    return std::max(1, static_cast<int>(value));
}

int positiveRoundedIntFromDouble(double value) {
    if (!std::isfinite(value) || value <= 0.0)
        return 1;
    if (value >= static_cast<double>(std::numeric_limits<int>::max()))
        return std::numeric_limits<int>::max();
    return std::max(1, static_cast<int>(std::lround(value)));
}

bool checkedRgbaByteSize(int width, int height, std::size_t& bytes) {
    bytes = 0;
    if (width <= 0 || height <= 0 || width > MAX_RGBA_READBACK_DIMENSION || height > MAX_RGBA_READBACK_DIMENSION)
        return false;

    const auto w = static_cast<std::size_t>(width);
    const auto h = static_cast<std::size_t>(height);
    if (w > std::numeric_limits<std::size_t>::max() / h)
        return false;

    const auto pixels = w * h;
    if (pixels > std::numeric_limits<std::size_t>::max() / RGBA_BYTES_PER_PIXEL)
        return false;

    bytes = pixels * RGBA_BYTES_PER_PIXEL;
    return bytes <= MAX_RGBA_READBACK_BYTES;
}

int clampToFramebuffer(std::int64_t value, int framebufferExtent) {
    if (value <= 0)
        return 0;
    if (value >= framebufferExtent)
        return framebufferExtent;
    return static_cast<int>(value);
}

bool prepareRgbaReadbackRegion(int framebufferWidth,
                               int framebufferHeight,
                               int cropX,
                               int cropTopY,
                               int cropWidth,
                               int cropHeight,
                               RgbaReadbackRegion& region) {
    region = {};
    if (framebufferWidth <= 0 || framebufferHeight <= 0 || cropWidth <= 0 || cropHeight <= 0)
        return false;

    const auto cropLeft = static_cast<std::int64_t>(cropX);
    const auto cropTop = static_cast<std::int64_t>(cropTopY);
    const auto cropRight = cropLeft + static_cast<std::int64_t>(cropWidth);
    const auto cropBottom = cropTop + static_cast<std::int64_t>(cropHeight);

    region.srcX = clampToFramebuffer(cropLeft, framebufferWidth);
    region.srcTopY = clampToFramebuffer(cropTop, framebufferHeight);
    const int srcRight = clampToFramebuffer(cropRight, framebufferWidth);
    const int srcBottom = clampToFramebuffer(cropBottom, framebufferHeight);
    region.srcWidth = srcRight - region.srcX;
    region.srcHeight = srcBottom - region.srcTopY;

    std::size_t requestedBytes = 0;
    if (checkedRgbaByteSize(cropWidth, cropHeight, requestedBytes)) {
        region.outputCropX = cropX;
        region.outputCropTopY = cropTopY;
        region.outputWidth = cropWidth;
        region.outputHeight = cropHeight;
        region.dstX = region.srcX - cropX;
        region.dstY = region.srcTopY - cropTopY;
        region.outputBytes = requestedBytes;
    } else {
        if (region.srcWidth <= 0 || region.srcHeight <= 0)
            return false;

        if (!checkedRgbaByteSize(region.srcWidth, region.srcHeight, region.outputBytes))
            return false;

        region.outputCropX = region.srcX;
        region.outputCropTopY = region.srcTopY;
        region.outputWidth = region.srcWidth;
        region.outputHeight = region.srcHeight;
        region.dstX = 0;
        region.dstY = 0;
    }

    if (region.srcWidth > 0 && region.srcHeight > 0 && !checkedRgbaByteSize(region.srcWidth, region.srcHeight, region.sourceBytes))
        return false;

    return true;
}

void rememberParent(std::vector<std::filesystem::path>& parents, const std::filesystem::path& path) {
    const auto parent = path.parent_path();
    if (!parent.empty() && !containsPath(parents, parent))
        parents.push_back(parent);
}

bool writeRgbaFile(const std::filesystem::path& path, const std::vector<unsigned char>& pixels, ArtifactBudget* budget = nullptr) {
    if (budget && !budget->consume(pixels.size()))
        return false;

    const auto native = path.string();
    const int  fd = open(native.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return false;

    const auto* data = reinterpret_cast<const char*>(pixels.data());
    std::size_t written = 0;
    while (written < pixels.size()) {
        const ssize_t chunk = write(fd, data + written, pixels.size() - written);
        if (chunk < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            unlink(native.c_str());
            return false;
        }
        if (chunk == 0) {
            close(fd);
            unlink(native.c_str());
            return false;
        }
        written += static_cast<std::size_t>(chunk);
    }

    if (close(fd) != 0) {
        unlink(native.c_str());
        return false;
    }
    return true;
}

unsigned char alphaAt(const RgbaReadback& readback, int x, int y) {
    const auto i = (static_cast<std::size_t>(y) * readback.width + x) * 4U + 3U;
    return readback.pixels[i];
}

bool findAlphaBounds(const RgbaReadback& readback, PixelBounds& bounds) {
    if (readback.width <= 0 || readback.height <= 0 || readback.pixels.empty())
        return false;

    int minX = readback.width;
    int minY = readback.height;
    int maxX = -1;
    int maxY = -1;

    for (int y = 0; y < readback.height; ++y) {
        for (int x = 0; x < readback.width; ++x) {
            if (alphaAt(readback, x, y) == 0)
                continue;

            minX = std::min(minX, x);
            minY = std::min(minY, y);
            maxX = std::max(maxX, x);
            maxY = std::max(maxY, y);
        }
    }

    if (maxX < minX || maxY < minY)
        return false;

    bounds = {.x = minX, .y = minY, .width = maxX - minX + 1, .height = maxY - minY + 1};
    return true;
}

RgbaReadback cropReadback(const RgbaReadback& readback, const PixelBounds& bounds) {
    if (bounds.width <= 0 || bounds.height <= 0 || readback.pixels.empty())
        return {};

    RgbaReadback cropped;
    cropped.cropX = readback.cropX + bounds.x;
    cropped.cropTopY = readback.cropTopY + bounds.y;
    cropped.width = bounds.width;
    cropped.height = bounds.height;
    std::size_t pixelBytes = 0;
    if (!checkedRgbaByteSize(bounds.width, bounds.height, pixelBytes))
        return {};
    cropped.pixels.assign(pixelBytes, 0);

    const std::size_t rowBytes = static_cast<std::size_t>(bounds.width) * RGBA_BYTES_PER_PIXEL;
    for (int y = 0; y < bounds.height; ++y) {
        const auto* src = readback.pixels.data() + (static_cast<std::size_t>(bounds.y + y) * readback.width + bounds.x) * RGBA_BYTES_PER_PIXEL;
        auto*       dst = cropped.pixels.data() + static_cast<std::size_t>(y) * rowBytes;
        std::copy(src, src + rowBytes, dst);
    }

    return cropped;
}

void resetAsyncPboReadback(AsyncPboReadbackState& state) {
    if (std::any_of(std::begin(state.buffers), std::end(state.buffers), [](GLuint buffer) { return buffer != 0; }))
        glDeleteBuffers(AsyncPboReadbackState::BUFFER_COUNT, state.buffers);
    state = {};
}

void resetWindowStreamPboReadback() {
    auto& state = g_windowStreamPboReadback;
    for (auto& fence : state.fences) {
        if (fence)
            glDeleteSync(fence);
    }
    if (std::any_of(std::begin(state.buffers), std::end(state.buffers), [](GLuint buffer) { return buffer != 0; }))
        glDeleteBuffers(static_cast<GLsizei>(WindowStreamPboReadbackState::BUFFER_COUNT), state.buffers);
    state = {};
}

bool sameWindowStreamSource(const WindowStreamPboReadbackState& state,
                            const std::string& windowAddress,
                            const Rect& relativeVisibleGeometry,
                            int width,
                            int height) {
    return state.pending == 0 ||
        (state.windowAddress == windowAddress && state.width == width && state.height == height &&
         state.relativeVisibleGeometry.x == relativeVisibleGeometry.x && state.relativeVisibleGeometry.y == relativeVisibleGeometry.y &&
         state.relativeVisibleGeometry.width == relativeVisibleGeometry.width && state.relativeVisibleGeometry.height == relativeVisibleGeometry.height);
}

bool ensureWindowStreamPboReadback(WindowStreamPboReadbackState& state, int width, int height, std::size_t bytes) {
    if (std::all_of(std::begin(state.buffers), std::end(state.buffers), [](GLuint buffer) { return buffer != 0; }) && state.width == width &&
        state.height == height && state.bytes == bytes)
        return true;

    resetWindowStreamPboReadback();
    glGenBuffers(static_cast<GLsizei>(WindowStreamPboReadbackState::BUFFER_COUNT), state.buffers);
    if (!std::all_of(std::begin(state.buffers), std::end(state.buffers), [](GLuint buffer) { return buffer != 0; })) {
        resetWindowStreamPboReadback();
        return false;
    }
    state.width = width;
    state.height = height;
    state.bytes = bytes;
    return true;
}

bool windowStreamPboReady(const WindowStreamPboReadbackState& state, std::size_t offset) {
    if (offset >= state.pending)
        return false;
    const auto source = (state.next + WindowStreamPboReadbackState::BUFFER_COUNT - state.pending + offset) % WindowStreamPboReadbackState::BUFFER_COUNT;
    const auto fence = state.fences[source];
    if (!fence)
        return false;
    const auto status = glClientWaitSync(fence, 0, 0);
    return status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED;
}

struct ReadyWindowStreamPboFrame {
    WindowStreamCapturedFrame frame;
    CBox                      fullBox;
    CBox                      visibleBox;
};

std::optional<ReadyWindowStreamPboFrame> takeReadyWindowStreamPboFrames(WindowStreamPboReadbackState& state) {
    ScopedTiming takeReadyTiming("window.stream.pbo_take_ready");
    std::size_t completed = 0;
    while (windowStreamPboReady(state, completed))
        ++completed;
    if (completed == 0)
        return std::nullopt;

    // Keep only the freshest completed readback.  Copying every completed 8+MB
    // PBO merely to overwrite it locally turns backlog into compositor-thread
    // latency.  Fences are checked with timeout zero; discarded slots were
    // already complete and no GPU work is waited on here.
    while (completed > 1) {
        const auto source = (state.next + WindowStreamPboReadbackState::BUFFER_COUNT - state.pending) % WindowStreamPboReadbackState::BUFFER_COUNT;
        if (!state.metadataSlots.discardOldest()) {
            noteWindowStreamDiagnostic("pbo metadata discard mismatch");
            resetWindowStreamPboReadback();
            return std::nullopt;
        }
        glDeleteSync(state.fences[source]);
        state.fences[source] = nullptr;
        state.slotGeometry[source] = {};
        --state.pending;
        --completed;
    }

    const auto source = (state.next + WindowStreamPboReadbackState::BUFFER_COUNT - state.pending) % WindowStreamPboReadbackState::BUFFER_COUNT;
    const auto metadata = state.metadataSlots.mapOldest();
    if (!metadata) {
        noteWindowStreamDiagnostic("pbo metadata map mismatch");
        resetWindowStreamPboReadback();
        return std::nullopt;
    }

    glBindBuffer(GL_PIXEL_PACK_BUFFER, state.buffers[source]);
    const auto* mapped = static_cast<const unsigned char*>(glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(state.bytes), GL_MAP_READ_BIT));
    std::optional<ReadyWindowStreamPboFrame> newest;
    if (mapped) {
        ScopedTiming mapCopyTiming("window.stream.pbo_map_copy");
        newest = ReadyWindowStreamPboFrame{
            .frame = {.metadata = *metadata, .rgba = std::vector<unsigned char>(mapped, mapped + state.bytes)},
            .fullBox = state.slotGeometry[source].fullBox,
            .visibleBox = state.slotGeometry[source].visibleBox,
        };
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    } else {
        noteWindowStreamDiagnostic("pbo map failed");
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glDeleteSync(state.fences[source]);
    state.fences[source] = nullptr;
    state.slotGeometry[source] = {};
    --state.pending;
    return newest;
}

bool issueWindowStreamPboReadback(WindowStreamPboReadbackState& state,
                                  const WindowStreamFrameMetadata& metadata,
                                  const std::string& windowAddress,
                                  CFramebuffer& framebuffer,
                                  int cropX,
                                  int cropTopY,
                                  const CBox& fullBox,
                                  const CBox& visibleBox) {
    ScopedTiming issueTiming("window.stream.pbo_issue");
    if (state.pending >= WindowStreamPboReadbackState::BUFFER_COUNT)
        return false;
    if (cropX < 0 || cropTopY < 0 || cropX > std::numeric_limits<int>::max() - state.width ||
        cropTopY > std::numeric_limits<int>::max() - state.height)
        return false;
    const auto framebufferWidth = positiveRoundedIntFromDouble(framebuffer.m_size.x);
    const auto framebufferHeight = positiveRoundedIntFromDouble(framebuffer.m_size.y);
    const auto readY = framebufferHeight - cropTopY - state.height;
    if (cropX + state.width > framebufferWidth || cropTopY + state.height > framebufferHeight || readY < 0)
        return false;
    const auto slot = state.metadataSlots.issue(windowAddress, metadata);
    if (!slot || *slot != state.next)
        return false;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, state.buffers[state.next]);
    glBufferData(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(state.bytes), nullptr, GL_STREAM_READ);
    glReadPixels(cropX, readY, state.width, state.height, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    state.fences[state.next] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    // Submit without waiting: a timeout-zero client wait must not depend on a
    // later compositor redraw to make this fence observable.
    glFlush();
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    if (!state.fences[state.next]) {
        resetWindowStreamPboReadback();
        return false;
    }
    state.slotGeometry[state.next] = {.fullBox = fullBox, .visibleBox = visibleBox, .geometryEpoch = metadata.geometryEpoch,
                                      .captureMonotonicNs = metadata.captureMonotonicNs};
    ++state.pending;
    state.next = (state.next + 1) % WindowStreamPboReadbackState::BUFFER_COUNT;
    return true;
}

bool ensureAsyncPboReadback(AsyncPboReadbackState& state, int width, int height, std::size_t bytes) {
    if (std::all_of(std::begin(state.buffers), std::end(state.buffers), [](GLuint buffer) { return buffer != 0; }) && state.width == width &&
        state.height == height && state.bytes == bytes)
        return true;

    resetAsyncPboReadback(state);
    glGenBuffers(AsyncPboReadbackState::BUFFER_COUNT, state.buffers);
    if (!std::all_of(std::begin(state.buffers), std::end(state.buffers), [](GLuint buffer) { return buffer != 0; })) {
        resetAsyncPboReadback(state);
        return false;
    }

    state.width = width;
    state.height = height;
    state.bytes = bytes;
    state.next = 0;
    return true;
}

bool issueAsyncPboReadback(AsyncPboReadbackState& state, const RgbaReadbackRegion& region, int framebufferHeight, bool directGlY) {
    if (state.pending >= AsyncPboReadbackState::BUFFER_COUNT)
        return false;

    const int target = state.next;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, state.buffers[target]);
    glBufferData(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(state.bytes), nullptr, GL_STREAM_READ);
    const int readY = directGlY ? region.srcTopY : framebufferHeight - region.srcTopY - region.srcHeight;
    glReadPixels(region.srcX, readY, region.srcWidth, region.srcHeight, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    ++state.pending;
    state.next = (target + 1) % AsyncPboReadbackState::BUFFER_COUNT;
    return true;
}

bool mapPendingPboReadback(AsyncPboReadbackState& state, RgbaReadback& readback, bool force = false) {
    if (state.pending <= 0 || (!force && state.pending < AsyncPboReadbackState::BUFFER_COUNT - 1))
        return false;

    const int source = (state.next + AsyncPboReadbackState::BUFFER_COUNT - state.pending) % AsyncPboReadbackState::BUFFER_COUNT;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, state.buffers[source]);
    const auto* ptr = static_cast<const unsigned char*>(glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(state.bytes), GL_MAP_READ_BIT));
    if (!ptr) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        return false;
    }

    std::copy(ptr, ptr + state.bytes, readback.pixels.data());
    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    --state.pending;
    return true;
}

GLuint framebufferId(CFramebuffer& framebuffer) {
    auto* glFramebuffer = dynamic_cast<Render::GL::CGLFramebuffer*>(&framebuffer);
    return glFramebuffer ? glFramebuffer->getFBID() : 0;
}

RgbaReadback readRgbaFramebufferRegion(CFramebuffer& framebuffer, int cropX, int cropTopY, int cropWidth, int cropHeight, bool directGlY = false, bool asyncPbo = false) {
    const int framebufferWidth = positiveRoundedIntFromDouble(framebuffer.m_size.x);
    const int framebufferHeight = positiveRoundedIntFromDouble(framebuffer.m_size.y);
    RgbaReadbackRegion region;
    if (!prepareRgbaReadbackRegion(framebufferWidth, framebufferHeight, cropX, cropTopY, cropWidth, cropHeight, region))
        return {};

    RgbaReadback readback;
    readback.cropX = region.outputCropX;
    readback.cropTopY = region.outputCropTopY;
    readback.width = region.outputWidth;
    readback.height = region.outputHeight;
    readback.pixels.assign(region.outputBytes, 0);

    if (region.srcWidth > 0 && region.srcHeight > 0) {
        const bool canReadDirectly = region.srcWidth == region.outputWidth && region.srcHeight == region.outputHeight && region.dstX == 0 && region.dstY == 0;
        const bool useAsyncPbo = asyncPbo && canReadDirectly && region.sourceBytes == region.outputBytes && region.outputBytes > 0;
        std::vector<unsigned char> rows;
        if (!canReadDirectly)
            rows.assign(region.sourceBytes, 0);
        unsigned char* target = canReadDirectly ? readback.pixels.data() : rows.data();
        GLint previousReadFramebuffer = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
        const GLuint readFramebuffer = framebufferId(framebuffer);
        if (readFramebuffer == 0)
            return {};
        glBindFramebuffer(GL_READ_FRAMEBUFFER, readFramebuffer);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);

        if (useAsyncPbo && ensureAsyncPboReadback(g_windowRecordingPboReadback, region.outputWidth, region.outputHeight, region.outputBytes)) {
            bool hasMappedFrame = mapPendingPboReadback(g_windowRecordingPboReadback, readback);
            if (!issueAsyncPboReadback(g_windowRecordingPboReadback, region, framebufferHeight, directGlY)) {
                hasMappedFrame = mapPendingPboReadback(g_windowRecordingPboReadback, readback, true);
                if (!hasMappedFrame)
                    resetAsyncPboReadback(g_windowRecordingPboReadback);
                else
                    issueAsyncPboReadback(g_windowRecordingPboReadback, region, framebufferHeight, directGlY);
            }
            if (!hasMappedFrame) {
                const int readY = directGlY ? region.srcTopY : framebufferHeight - region.srcTopY - region.srcHeight;
                glReadPixels(region.srcX, readY, region.srcWidth, region.srcHeight, GL_RGBA, GL_UNSIGNED_BYTE, target);
            }
        } else {
            if (asyncPbo)
                resetAsyncPboReadback(g_windowRecordingPboReadback);
            const int readY = directGlY ? region.srcTopY : framebufferHeight - region.srcTopY - region.srcHeight;
            glReadPixels(region.srcX, readY, region.srcWidth, region.srcHeight, GL_RGBA, GL_UNSIGNED_BYTE, target);
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, previousReadFramebuffer);

        if (!canReadDirectly) {
            for (int y = 0; y < region.srcHeight; ++y) {
                const auto* src = rows.data() + static_cast<std::size_t>(y) * region.srcWidth * RGBA_BYTES_PER_PIXEL;
                auto*       dst = readback.pixels.data() +
                    (static_cast<std::size_t>(region.dstY + y) * region.outputWidth + region.dstX) * RGBA_BYTES_PER_PIXEL;
                std::copy(src, src + static_cast<std::size_t>(region.srcWidth) * RGBA_BYTES_PER_PIXEL, dst);
            }
        }
    }

    return readback;
}

RgbaReadback readRenderPassFramebufferRegion(CFramebuffer& framebuffer, int cropX, int cropTopY, int cropWidth, int cropHeight) {
    return readRgbaFramebufferRegion(framebuffer, cropX, cropTopY, cropWidth, cropHeight, true);
}

bool blitRenderPassFramebufferRegion(CFramebuffer& source, CFramebuffer& target, int cropX, int cropTopY, int cropWidth, int cropHeight) {
    ScopedTiming timing("realbg.blit_framebuffer");

    const int sourceWidth = positiveRoundedIntFromDouble(source.m_size.x);
    const int sourceHeight = positiveRoundedIntFromDouble(source.m_size.y);
    RgbaReadbackRegion region;
    if (!prepareRgbaReadbackRegion(sourceWidth, sourceHeight, cropX, cropTopY, cropWidth, cropHeight, region))
        return false;
    if (region.outputWidth != positiveRoundedIntFromDouble(target.m_size.x) || region.outputHeight != positiveRoundedIntFromDouble(target.m_size.y))
        return false;
    const GLuint sourceFramebuffer = framebufferId(source);
    const GLuint targetFramebuffer = framebufferId(target);
    if (sourceFramebuffer == 0 || targetFramebuffer == 0)
        return false;

    GLint      previousReadFramebuffer = 0;
    GLint      previousDrawFramebuffer = 0;
    GLfloat    previousClearColor[4] = {};
    const bool scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);

    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, previousClearColor);

    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, targetFramebuffer);
    glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
    glClear(GL_COLOR_BUFFER_BIT);

    if (region.srcWidth > 0 && region.srcHeight > 0) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, sourceFramebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, targetFramebuffer);
        glBlitFramebuffer(region.srcX,
                          region.srcTopY,
                          region.srcX + region.srcWidth,
                          region.srcTopY + region.srcHeight,
                          region.dstX,
                          region.dstY,
                          region.dstX + region.srcWidth,
                          region.dstY + region.srcHeight,
                          GL_COLOR_BUFFER_BIT,
                          GL_NEAREST);
    }

    glClearColor(previousClearColor[0], previousClearColor[1], previousClearColor[2], previousClearColor[3]);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, previousReadFramebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previousDrawFramebuffer);
    if (scissorEnabled)
        glEnable(GL_SCISSOR_TEST);
    else
        glDisable(GL_SCISSOR_TEST);

    return glGetError() == GL_NO_ERROR;
}

class RealBackgroundCapturePass final : public IPassElement {
  public:
    explicit RealBackgroundCapturePass(RealBackgroundCaptureState* state) : m_state(state) {}

    std::vector<UP<IPassElement>> draw() override {
        if (!m_state || m_state->captured || !g_pHyprOpenGL || !g_pHyprRenderer->m_renderData.currentFB)
            return {};

        ScopedTiming timing("realbg.capture_pass");
        if (m_state->framebuffer) {
            m_state->captured =
                blitRenderPassFramebufferRegion(*g_pHyprRenderer->m_renderData.currentFB, *m_state->framebuffer, m_state->cropX, m_state->cropY, m_state->width, m_state->height);
        } else {
            m_state->readback =
                readRenderPassFramebufferRegion(*g_pHyprRenderer->m_renderData.currentFB, m_state->cropX, m_state->cropY, m_state->width, m_state->height);
            m_state->captured = !m_state->readback.pixels.empty();
        }
        return {};
    }

    bool needsLiveBlur() override {
        return false;
    }

    bool needsPrecomputeBlur() override {
        return false;
    }

    const char* passName() override {
        return "HyprshotRealBackgroundCapturePass";
    }

    ePassElementType type() override {
        return EK_CUSTOM;
    }

    bool undiscardable() override {
        return true;
    }

    bool disableSimplification() override {
        return true;
    }

  private:
    RealBackgroundCaptureState* m_state = nullptr;
};

class FullSurfaceVisibleRegionOverride {
  public:
    explicit FullSurfaceVisibleRegionOverride(const PHLWINDOW& window) {
        if (!window || !window->wlSurface() || !window->wlSurface()->resource())
            return;

        window->wlSurface()->resource()->breadthfirst(
            [this](SP<CWLSurfaceResource> resource, const Vector2D&, void*) {
                auto surface = Desktop::View::CWLSurface::fromResource(resource);
                if (!surface)
                    return;

                m_records.push_back({.surface = surface, .visibleRegion = surface->m_visibleRegion});

                const int width = std::max(1, static_cast<int>(std::lround(resource->m_current.bufferSize.x > 0 ? resource->m_current.bufferSize.x :
                                                                                                                 resource->m_current.size.x)));
                const int height = std::max(1, static_cast<int>(std::lround(resource->m_current.bufferSize.y > 0 ? resource->m_current.bufferSize.y :
                                                                                                                   resource->m_current.size.y)));
                surface->m_visibleRegion = CRegion{0, 0, width, height};
            },
            nullptr);
    }

    ~FullSurfaceVisibleRegionOverride() {
        for (auto& record : m_records) {
            if (record.surface)
                record.surface->m_visibleRegion = record.visibleRegion;
        }
    }

    FullSurfaceVisibleRegionOverride(const FullSurfaceVisibleRegionOverride&) = delete;
    FullSurfaceVisibleRegionOverride& operator=(const FullSurfaceVisibleRegionOverride&) = delete;

  private:
    struct Record {
        SP<Desktop::View::CWLSurface> surface;
        CRegion                      visibleRegion;
    };

    std::vector<Record> m_records;
};

struct RealBackgroundRenderHookContext {
    PHLMONITOR                              monitor;
    std::vector<RealBackgroundCaptureState>* states = nullptr;
    RealBackgroundCaptureState*             activeBlurState = nullptr;
};

using RenderWindowFn = void (*)(void*, PHLWINDOW, PHLMONITOR, const Time::steady_tp&, bool, eRenderPassMode, bool, bool);
using RenderTextureInternalFn = void (*)(void*, SP<CTexture>, const CBox&, const CHyprOpenGLImpl::STextureRenderData&);
using RenderTextureWithBlurInternalFn = void (*)(void*, SP<CTexture>, const CBox&, const CHyprOpenGLImpl::STextureRenderData&);

RealBackgroundRenderHookContext* g_realBackgroundRenderHookContext = nullptr;
RenderWindowFn                   g_renderWindowOriginal = nullptr;
RenderTextureInternalFn          g_renderTextureInternalOriginal = nullptr;
RenderTextureWithBlurInternalFn  g_renderTextureWithBlurInternalOriginal = nullptr;
CFunctionHook*                   g_realBackgroundRenderWindowHook = nullptr;
CFunctionHook*                   g_realBackgroundRenderTextureInternalHook = nullptr;
CFunctionHook*                   g_realBackgroundRenderTextureWithBlurInternalHook = nullptr;
std::optional<RealBackgroundRecordingCache> g_realBackgroundRecordingCache;

PHLWINDOW currentRenderWindow() {
    if (!g_pHyprOpenGL)
        return {};
    return g_pHyprRenderer->m_renderData.currentWindow.lock();
}

RealBackgroundCaptureState* findRealBackgroundStateForWindow(PHLWINDOW window) {
    if (!g_realBackgroundRenderHookContext || !g_realBackgroundRenderHookContext->states || !window)
        return nullptr;

    for (auto& state : *g_realBackgroundRenderHookContext->states) {
        if (state.window && state.window.get() == window.get())
            return &state;
    }

    return nullptr;
}

bool isActiveBlurBackgroundPass(RealBackgroundCaptureState* state, const CHyprOpenGLImpl::STextureRenderData& data) {
    if (!state || !state->awaitingBlurBackground || state->blurCaptured || data.blur || data.discardActive || !data.allowCustomUV || !g_pHyprOpenGL ||
        !g_pHyprRenderer->m_renderData.currentFB)
        return false;

    const auto window = currentRenderWindow();
    return window && state->window && window.get() == state->window.get();
}

void hkRenderWindow(void* rendererThisptr,
                    PHLWINDOW window,
                    PHLMONITOR monitor,
                    const Time::steady_tp& time,
                    bool decorate,
                    eRenderPassMode mode,
                    bool ignorePosition,
                    bool standalone) {
    if (g_realBackgroundRenderHookContext && g_realBackgroundRenderHookContext->states && monitor && g_realBackgroundRenderHookContext->monitor &&
        monitor.get() == g_realBackgroundRenderHookContext->monitor.get() && g_pHyprRenderer) {
        for (auto& state : *g_realBackgroundRenderHookContext->states) {
            if (state.queued || state.captured || !state.window || !window || state.window.get() != window.get())
                continue;

            state.queued = true;
            g_pHyprRenderer->m_renderPass.add(makeUnique<RealBackgroundCapturePass>(&state));
            break;
        }
    }

    if (g_renderWindowOriginal)
        g_renderWindowOriginal(rendererThisptr, window, monitor, time, decorate, mode, ignorePosition, standalone);
}

void hkRenderTextureInternal(void* openGLThisptr, SP<CTexture> texture, const CBox& box, const CHyprOpenGLImpl::STextureRenderData& data) {
    auto* state = g_realBackgroundRenderHookContext ? g_realBackgroundRenderHookContext->activeBlurState : nullptr;
    const bool captureAfterDraw = isActiveBlurBackgroundPass(state, data);

    if (g_renderTextureInternalOriginal)
        g_renderTextureInternalOriginal(openGLThisptr, texture, box, data);

    if (!captureAfterDraw || !state || !g_pHyprOpenGL || !g_pHyprRenderer->m_renderData.currentFB)
        return;

    state->awaitingBlurBackground = false;
    ScopedTiming timing("realbg.blur_capture");
    if (state->framebuffer) {
        state->captured =
            blitRenderPassFramebufferRegion(*g_pHyprRenderer->m_renderData.currentFB, *state->framebuffer, state->cropX, state->cropY, state->width, state->height);
        state->blurCaptured = state->captured;
    } else {
        auto readback = readRenderPassFramebufferRegion(*g_pHyprRenderer->m_renderData.currentFB, state->cropX, state->cropY, state->width, state->height);
        if (readback.pixels.empty())
            return;

        state->readback = std::move(readback);
        state->captured = true;
        state->blurCaptured = true;
    }
}

void hkRenderTextureWithBlurInternal(void* openGLThisptr, SP<CTexture> texture, const CBox& box, const CHyprOpenGLImpl::STextureRenderData& data) {
    auto* context = g_realBackgroundRenderHookContext;
    auto* state = findRealBackgroundStateForWindow(currentRenderWindow());

    if (!context || !state || state->blurCaptured || !g_renderTextureWithBlurInternalOriginal) {
        if (g_renderTextureWithBlurInternalOriginal)
            g_renderTextureWithBlurInternalOriginal(openGLThisptr, texture, box, data);
        return;
    }

    auto* previousActiveBlurState = context->activeBlurState;
    const bool previousAwaitingBlurBackground = state->awaitingBlurBackground;
    context->activeBlurState = state;
    state->awaitingBlurBackground = true;

    g_renderTextureWithBlurInternalOriginal(openGLThisptr, texture, box, data);

    state->awaitingBlurBackground = previousAwaitingBlurBackground;
    context->activeBlurState = previousActiveBlurState;
}

void repairTopTransparentSeam(RgbaReadback& readback) {
    if (readback.width <= 0 || readback.height <= 2 || readback.pixels.empty())
        return;

    constexpr unsigned char MAX_SEAM_ALPHA = 4;
    constexpr unsigned char MIN_WINDOW_ALPHA = 128;

    const int minRepairColumns = std::max(16, readback.width / 3);
    const int minRepairSpan = std::max(16, readback.width / 2);
    const int maxScanY = std::min(readback.height - 1, 96);

    for (int y = 1; y < maxScanY; ++y) {
        int first = -1;
        int last = -1;
        int repairColumns = 0;

        for (int x = 0; x < readback.width; ++x) {
            if (alphaAt(readback, x, y) > MAX_SEAM_ALPHA || alphaAt(readback, x, y - 1) < MIN_WINDOW_ALPHA || alphaAt(readback, x, y + 1) < MIN_WINDOW_ALPHA)
                continue;

            if (first < 0)
                first = x;
            last = x;
            ++repairColumns;
        }

        if (repairColumns < minRepairColumns || first < 0 || last - first + 1 < minRepairSpan)
            continue;

        for (int x = first; x <= last; ++x) {
            if (alphaAt(readback, x, y) > MAX_SEAM_ALPHA || alphaAt(readback, x, y - 1) < MIN_WINDOW_ALPHA || alphaAt(readback, x, y + 1) < MIN_WINDOW_ALPHA)
                continue;

            const auto src = (static_cast<std::size_t>(y + 1) * readback.width + x) * 4U;
            const auto dst = (static_cast<std::size_t>(y) * readback.width + x) * 4U;
            std::copy(readback.pixels.data() + src, readback.pixels.data() + src + 4U, readback.pixels.data() + dst);
        }

        return;
    }
}

void unpremultiplyAlpha(RgbaReadback& readback) {
    unpremultiplyRgbaPixels(readback.pixels);
}

SP<CFramebuffer> createFramebuffer(const std::string& name, int width, int height, DRMFormat preferredFormat = DRM_FORMAT_ABGR8888) {
    if (!g_pHyprRenderer || width <= 0 || height <= 0)
        return {};

    auto framebuffer = g_pHyprRenderer->createFB(name);
    if (!framebuffer)
        return {};

    if (!framebuffer->alloc(width, height, DRM_FORMAT_ABGR8888) && !framebuffer->alloc(width, height, preferredFormat))
        return {};

    return framebuffer;
}

bool ensureFramebuffer(SP<CFramebuffer>& framebuffer, const std::string& name, int width, int height, DRMFormat preferredFormat = DRM_FORMAT_ABGR8888) {
    if (framebuffer && framebuffer->isAllocated() && positiveRoundedIntFromDouble(framebuffer->m_size.x) == width && positiveRoundedIntFromDouble(framebuffer->m_size.y) == height)
        return true;

    if (framebuffer)
        framebuffer->release();
    framebuffer = createFramebuffer(name, width, height, preferredFormat);
    return framebuffer && framebuffer->isAllocated();
}

SP<CFramebuffer>& reusableWindowRecordingFramebuffer() {
    static SP<CFramebuffer> framebuffer;
    return framebuffer;
}

SP<CFramebuffer>& reusableWindowRecordingMaskFramebuffer() {
    static SP<CFramebuffer> framebuffer;
    return framebuffer;
}

void renderTextureWithAlphaMatte(SP<CTexture> texture, const CBox& box, SP<CFramebuffer> matte) {
    if (!matte)
        return;

    auto matteTexture = matte->getTexture();
    if (!matteTexture) {
        g_pHyprOpenGL->renderTextureMatte(texture, box, matte);
        return;
    }

    glActiveTexture(GL_TEXTURE0 + 1);
    matteTexture->bind();
    if (auto* glTexture = dynamic_cast<Render::GL::CGLTexture*>(matteTexture.get()))
        glTexture->swizzle(std::array<GLint, 4>{GL_ALPHA, GL_GREEN, GL_BLUE, GL_ALPHA});

    g_pHyprOpenGL->renderTextureMatte(texture, box, matte);

    glActiveTexture(GL_TEXTURE0 + 1);
    matteTexture->bind();
    if (auto* glTexture = dynamic_cast<Render::GL::CGLTexture*>(matteTexture.get()))
        glTexture->swizzle(std::array<GLint, 4>{GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA});
}

CBox renderedWindowBox(const PHLWINDOW& window, CBox box) {
    if (window->m_workspace && !window->m_pinned)
        box.translate(window->m_workspace->m_renderOffset->value());
    box.translate(window->m_floatingOffset);
    return box;
}

CBox renderedWindowGoalMainSurfaceBox(const PHLWINDOW& window) {
    if (!window)
        return {};

    CBox box{window->m_realPosition->goal().x, window->m_realPosition->goal().y, window->m_realSize->goal().x, window->m_realSize->goal().y};
    return renderedWindowBox(window, box);
}

bool isScrollingTiledWindow(const PHLWINDOW& window) {
    if (!window || window->m_isFloating || !window->m_workspace || !window->m_workspace->m_space)
        return false;

    const auto algorithm = window->m_workspace->m_space->algorithm();
    if (!algorithm || !algorithm->tiledAlgo())
        return false;

    return Layout::Supplementary::algoMatcher()->getNameForTiledAlgo(algorithm->tiledAlgo().get()) == "scrolling";
}

std::string pointerId(const void* ptr) {
    std::ostringstream out;
    out << std::hex << reinterpret_cast<std::uintptr_t>(ptr);
    return out.str();
}

bool overviewJsonDoubleValue(const Json& obj, const char* key, double& out, double minimum, double maximum) {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_number())
        return false;

    const double value = it->get<double>();
    if (!std::isfinite(value) || value < minimum || value > maximum)
        return false;

    out = value;
    return true;
}

bool overviewJsonRectValue(const Json& obj, Rect& out) {
    if (!obj.is_object())
        return false;

    Rect rect;
    if (!overviewJsonDoubleValue(obj, "x", rect.x, -MAX_OVERVIEW_LOGICAL_COORDINATE, MAX_OVERVIEW_LOGICAL_COORDINATE) ||
        !overviewJsonDoubleValue(obj, "y", rect.y, -MAX_OVERVIEW_LOGICAL_COORDINATE, MAX_OVERVIEW_LOGICAL_COORDINATE) ||
        !overviewJsonDoubleValue(obj, "width", rect.width, 1.0, MAX_OVERVIEW_LOGICAL_COORDINATE) ||
        !overviewJsonDoubleValue(obj, "height", rect.height, 1.0, MAX_OVERVIEW_LOGICAL_COORDINATE))
        return false;

    out = rect;
    return true;
}

std::unordered_map<std::string, Rect> fetchHymissionOverviewSelectionGeometry() {
    std::unordered_map<std::string, Rect> result;
    const std::string response = HyprlandAPI::invokeHyprctlCommand("hymission-overview-state", "", "json");
    if (response.empty() || response.front() != '{')
        return result;

    const auto root = Json::parse(response, nullptr, false);
    if (root.is_discarded() || !root.is_object())
        return result;

    const auto active = root.find("active");
    if (active == root.end() || !active->is_boolean() || !active->get<bool>())
        return result;

    const auto windows = root.find("windows");
    if (windows == root.end() || !windows->is_array() || windows->size() > MAX_SESSION_WINDOWS)
        return result;

    for (const auto& item : *windows) {
        if (!item.is_object())
            continue;

        const auto addressIt = item.find("address");
        const auto selectionIt = item.find("selectionGeometry");
        if (addressIt == item.end() || selectionIt == item.end() || !addressIt->is_string())
            continue;

        const std::string address = addressIt->get<std::string>();
        if (address.empty() || address.size() > MAX_WINDOW_METADATA_BYTES)
            continue;

        Rect selection;
        if (!overviewJsonRectValue(*selectionIt, selection))
            continue;

        result[address] = selection;
    }

    return result;
}

bool hyprctlOk(const std::string& response) {
    auto it = std::find_if_not(response.begin(), response.end(), [](unsigned char ch) { return std::isspace(ch) != 0; });
    return it != response.end() && std::string_view(&*it, static_cast<std::size_t>(std::distance(it, response.end()))).starts_with("ok");
}

class HymissionRawWindowRenderScope {
  public:
    HymissionRawWindowRenderScope() {
        m_token = "hyprcapture-" + makeSessionId();
        m_active = hyprctlOk(HyprlandAPI::invokeHyprctlCommand("hymission-raw-window-render", "begin " + m_token));
    }

    ~HymissionRawWindowRenderScope() {
        if (m_active)
            (void)HyprlandAPI::invokeHyprctlCommand("hymission-raw-window-render", "end " + m_token);
    }

    HymissionRawWindowRenderScope(const HymissionRawWindowRenderScope&) = delete;
    HymissionRawWindowRenderScope& operator=(const HymissionRawWindowRenderScope&) = delete;

  private:
    std::string m_token;
    bool        m_active = false;
};

class WindowAnimationGoalOverride {
  public:
    explicit WindowAnimationGoalOverride(const PHLWINDOW& window) : m_window(window) {
        if (!m_window || !m_window->m_realPosition || !m_window->m_realSize)
            return;

        m_position = m_window->m_realPosition->value();
        m_size = m_window->m_realSize->value();
        m_active = true;
        setPositionOffset({});
    }

    void setPositionOffset(const Vector2D& offset) {
        if (!m_active || !m_window || !m_window->m_realPosition || !m_window->m_realSize)
            return;

        m_window->m_realPosition->value() = m_window->m_realPosition->goal() + offset;
        m_window->m_realSize->value() = m_window->m_realSize->goal();
        m_window->updateWindowDecos();
    }

    ~WindowAnimationGoalOverride() {
        if (!m_active || !m_window || !m_window->m_realPosition || !m_window->m_realSize)
            return;

        m_window->m_realPosition->value() = m_position;
        m_window->m_realSize->value() = m_size;
        m_window->updateWindowDecos();
    }

    WindowAnimationGoalOverride(const WindowAnimationGoalOverride&) = delete;
    WindowAnimationGoalOverride& operator=(const WindowAnimationGoalOverride&) = delete;

  private:
    PHLWINDOW m_window;
    Vector2D  m_position;
    Vector2D  m_size;
    bool      m_active = false;
};

RgbaReadback renderMonitorReadback(const PHLMONITOR& monitor,
                                   const Time::steady_tp& frozenTime,
                                   int cropX,
                                   int cropTopY,
                                   int cropWidth,
                                   int cropHeight,
                                   ArtifactBudget* budget = nullptr) {
    if (!monitor || !monitor->m_activeWorkspace || !g_pHyprRenderer || !g_pHyprOpenGL)
        return {};

    const int width = positiveRoundedIntFromDouble(monitor->m_pixelSize.x);
    const int height = positiveRoundedIntFromDouble(monitor->m_pixelSize.y);
    std::size_t framebufferBytes = 0;
    if (!checkedRgbaByteSize(width, height, framebufferBytes))
        return {};
    if (budget && !budget->canFit(framebufferBytes))
        return {};

    auto framebuffer = createFramebuffer("hyprcapture-monitor", width, height, monitor->m_output->state->state().drmFormat);
    if (!framebuffer)
        return {};

    const bool previousBlockFeedback = g_pHyprRenderer->m_bBlockSurfaceFeedback;
    const bool previousBlockShader = g_pHyprRenderer->m_renderData.blockScreenShader;
    const bool previousTransformDamage = g_pHyprRenderer->m_renderData.transformDamage;

    auto restoreRendererState = [&]() {
        g_pHyprRenderer->m_renderData.transformDamage = previousTransformDamage;
        g_pHyprRenderer->m_renderData.blockScreenShader = previousBlockShader;
        g_pHyprRenderer->m_bBlockSurfaceFeedback = previousBlockFeedback;
    };

    const int transformedWidth = positiveRoundedIntFromDouble(monitor->m_transformedSize.x);
    const int transformedHeight = positiveRoundedIntFromDouble(monitor->m_transformedSize.y);
    CRegion fakeDamage{0, 0, transformedWidth, transformedHeight};

    g_pHyprOpenGL->makeEGLCurrent();
    g_pHyprRenderer->m_bBlockSurfaceFeedback = true;
    if (!g_pHyprRenderer->beginRender(monitor, fakeDamage, RENDER_MODE_FULL_FAKE, nullptr, framebuffer)) {
        restoreRendererState();
        return {};
    }

    g_pHyprRenderer->draw(CClearPassElement::SClearData{CHyprColor{0.0, 0.0, 0.0, 1.0}});
    g_pHyprRenderer->renderWorkspace(monitor, monitor->m_activeWorkspace, frozenTime, CBox{0, 0, static_cast<double>(width), static_cast<double>(height)});
    if (monitor == Desktop::focusState()->monitor())
        Notification::overlay()->draw(monitor);
    if (monitor == Desktop::focusState()->monitor())
        ErrorOverlay::overlay()->draw();
    g_pHyprRenderer->m_renderData.blockScreenShader = true;
    g_pHyprRenderer->endRender();
    restoreRendererState();

    auto readback = readRgbaFramebufferRegion(*framebuffer, cropX, cropTopY, cropWidth, cropHeight);
    if (budget && !readback.pixels.empty() && !budget->consume(readback.pixels.size()))
        return {};
    return readback;
}

std::optional<ExportPipePayload> captureExportPipePayload(const std::string& responseFifo, std::string& error) {
    if (!g_pCompositor || !g_pHyprRenderer || !g_pHyprOpenGL) {
        error = "Hyprland renderer unavailable";
        return std::nullopt;
    }

    ExportPipePayload payload;
    payload.responseFifo = responseFifo;

    const auto frozenTime = Time::steadyNow();
    ArtifactBudget artifactBudget;
    int monitorIndex = 0;
    for (const auto& monitor : State::monitorState()->monitors()) {
        if (!monitor)
            continue;

        const int width = positiveRoundedIntFromDouble(monitor->m_pixelSize.x);
        const int height = positiveRoundedIntFromDouble(monitor->m_pixelSize.y);
        auto readback = renderMonitorReadback(monitor, frozenTime, 0, 0, width, height, &artifactBudget);
        if (readback.pixels.empty()) {
            error = "monitor export readback failed";
            return std::nullopt;
        }

        MonitorInfo info;
        info.name = monitor->m_name.empty() ? "monitor-" + std::to_string(monitorIndex) : monitor->m_name;
        info.logicalGeometry = monitorRect(monitor);
        info.scale = monitor->m_scale;
        info.transform = static_cast<int>(monitor->m_transform);
        info.focused = monitor == Desktop::focusState()->monitor();
        info.artifactWidth = readback.width;
        info.artifactHeight = readback.height;
        info.artifactTopDown = true;
        payload.monitors.emplace_back(ExportPipeMonitorPayload{std::move(info), std::move(readback.pixels)});
        ++monitorIndex;
    }

    if (payload.monitors.empty()) {
        error = "no active monitors to export";
        return std::nullopt;
    }

    payload.headerJson = exportPipeOkHeader(payload.monitors);
    return payload;
}

bool renderMonitorArtifact(const PHLMONITOR& monitor, const Time::steady_tp& frozenTime, const std::filesystem::path& path, int& width, int& height, ArtifactBudget& budget) {
    width = positiveRoundedIntFromDouble(monitor ? monitor->m_pixelSize.x : 0.0);
    height = positiveRoundedIntFromDouble(monitor ? monitor->m_pixelSize.y : 0.0);
    auto readback = renderMonitorReadback(monitor, frozenTime, 0, 0, width, height, &budget);
    return !readback.pixels.empty() && writeRgbaFile(path, readback.pixels);
}

bool renderCursorArtifact(const PHLMONITOR& monitor,
                          const Time::steady_tp& frozenTime,
                          const Point& capturedCursorPosition,
                          const std::filesystem::path& path,
                          int& width,
                          int& height,
                          ArtifactBudget& budget) {
    if (!monitor || !g_pHyprRenderer || !g_pHyprOpenGL || !Pointer::mgr())
        return false;

    width = positiveRoundedIntFromDouble(monitor->m_pixelSize.x);
    height = positiveRoundedIntFromDouble(monitor->m_pixelSize.y);
    std::size_t framebufferBytes = 0;
    if (!checkedRgbaByteSize(width, height, framebufferBytes) || !budget.canFit(framebufferBytes))
        return false;

    auto framebuffer = createFramebuffer("hyprcapture-cursor", width, height, monitor->m_output->state->state().drmFormat);
    if (!framebuffer)
        return false;

    const bool previousBlockFeedback = g_pHyprRenderer->m_bBlockSurfaceFeedback;
    const bool previousBlockShader = g_pHyprRenderer->m_renderData.blockScreenShader;
    const bool previousTransformDamage = g_pHyprRenderer->m_renderData.transformDamage;
    const auto restoreRendererState = [&]() {
        g_pHyprRenderer->m_renderData.transformDamage = previousTransformDamage;
        g_pHyprRenderer->m_renderData.blockScreenShader = previousBlockShader;
        g_pHyprRenderer->m_bBlockSurfaceFeedback = previousBlockFeedback;
    };

    const int transformedWidth = positiveRoundedIntFromDouble(monitor->m_transformedSize.x);
    const int transformedHeight = positiveRoundedIntFromDouble(monitor->m_transformedSize.y);
    CRegion fakeDamage{0, 0, transformedWidth, transformedHeight};

    g_pHyprOpenGL->makeEGLCurrent();
    g_pHyprRenderer->m_bBlockSurfaceFeedback = true;
    if (!g_pHyprRenderer->beginRender(monitor, fakeDamage, RENDER_MODE_FULL_FAKE, nullptr, framebuffer)) {
        restoreRendererState();
        return false;
    }

    g_pHyprRenderer->draw(CClearPassElement::SClearData{CHyprColor{0.0, 0.0, 0.0, 0.0}});
    const Vector2D cursorPosition =
        Vector2D{capturedCursorPosition.x, capturedCursorPosition.y} - monitor->m_position;
    const bool forceSoftwareRender = Pointer::mgr()->hasVisibleHWCursor(monitor);
    Pointer::mgr()->renderSoftwareCursorsFor(monitor, frozenTime, fakeDamage, cursorPosition, forceSoftwareRender);
    g_pHyprRenderer->m_renderData.blockScreenShader = true;
    g_pHyprRenderer->endRender();
    restoreRendererState();

    auto readback = readRgbaFramebufferRegion(*framebuffer, 0, 0, width, height);
    PixelBounds alphaBounds;
    if (readback.pixels.empty() || !findAlphaBounds(readback, alphaBounds) || !budget.consume(readback.pixels.size()))
        return false;
    return writeRgbaFile(path, readback.pixels);
}

WindowCaptureGeometry windowCaptureGeometry(const CBox& bounds, const PHLMONITOR& monitor) {
    return planWindowCaptureGeometry(bounds.x, bounds.y, bounds.w, bounds.h,
                                     monitor->m_position.x, monitor->m_position.y,
                                     monitor->m_scale <= 0 ? 1.0 : monitor->m_scale, static_cast<int>(monitor->m_transform));
}

// Hyprland 0.56's rounded-corner/border shaders consult monitor->m_transform
// directly even in RPT_EXPORT. Normalize it only for this synchronous local
// pass, then restore before returning to the compositor. No output state is
// committed, no monitor dimensions or scale are changed, and no event is sent.
class WindowLocalProjectionScope {
  public:
    explicit WindowLocalProjectionScope(const PHLMONITOR& monitor) : m_monitor(monitor), m_transform(monitor->m_transform),
        m_fbSize(g_pHyprRenderer->m_renderData.fbSize), m_projection(g_pHyprRenderer->m_renderData.targetProjection),
        m_type(g_pHyprRenderer->m_renderData.projectionType), m_transformDamage(g_pHyprRenderer->m_renderData.transformDamage),
        m_noSimplify(g_pHyprRenderer->m_renderData.noSimplify) {
        glGetIntegerv(GL_VIEWPORT, m_viewport.data());
        m_monitor->m_transform = WL_OUTPUT_TRANSFORM_NORMAL;
    }
    void apply(int width, int height) {
        g_pHyprRenderer->m_renderData.fbSize = Vector2D{width, height};
        g_pHyprRenderer->setProjectionType(RPT_EXPORT);
        g_pHyprRenderer->m_renderData.transformDamage = false;
        // Pass simplification clips against the physical monitor's bounds.
        // Export targets can be larger or have a different axis orientation.
        g_pHyprRenderer->m_renderData.noSimplify = true;
        g_pHyprOpenGL->setViewport(0, 0, width, height);
    }
    ~WindowLocalProjectionScope() {
        m_monitor->m_transform = m_transform;
        auto& data = g_pHyprRenderer->m_renderData;
        data.fbSize = m_fbSize;
        data.targetProjection = m_projection;
        data.projectionType = m_type;
        data.transformDamage = m_transformDamage;
        data.noSimplify = m_noSimplify;
        g_pHyprOpenGL->setViewport(m_viewport[0], m_viewport[1], m_viewport[2], m_viewport[3]);
    }
    WindowLocalProjectionScope(const WindowLocalProjectionScope&) = delete;
    WindowLocalProjectionScope& operator=(const WindowLocalProjectionScope&) = delete;
  private:
    PHLMONITOR m_monitor;
    wl_output_transform m_transform;
    Vector2D m_fbSize;
    Hyprutils::Math::Mat3x3 m_projection;
    Render::eRenderProjectionType m_type;
    bool m_transformDamage;
    bool m_noSimplify;
    std::array<GLint, 4> m_viewport{};
};

RgbaReadback renderWindowArtifactReadback(const PHLWINDOW& window,
                                          const PHLMONITOR& monitor,
                                          const Time::steady_tp& frozenTime,
                                          bool decorate,
                                          int& width,
                                          int& height,
                                          CBox& artifactBox,
                                          ArtifactBudget* budget = nullptr,
                                          bool trimToAlphaBounds = true,
                                          const WindowRenderOptions& options = {}) {
    if (!window || !monitor || !g_pHyprRenderer || !g_pHyprOpenGL)
        return {};

    HymissionRawWindowRenderScope rawWindowRenderScope;
    WindowAnimationGoalOverride windowGoal(window);
    const CBox fullBox = renderedWindowBox(window, window->getFullWindowBoundingBox());
    const auto geometry = windowCaptureGeometry(fullBox, monitor);
    if (!geometry.supported)
        return {};
    width = geometry.pixelWidth;
    height = geometry.pixelHeight;
    const int framebufferWidth = width;
    const int framebufferHeight = height;
    std::size_t framebufferBytes = 0;
    if (!checkedRgbaByteSize(width, height, framebufferBytes) || (budget && !budget->consume(framebufferBytes)))
        return {};

    const double scale = monitor->m_scale <= 0.0 ? 1.0 : monitor->m_scale;
    const Vector2D renderOffset = monitor->m_position - Vector2D{geometry.x, geometry.y};
    windowGoal.setPositionOffset(renderOffset);
    const CBox renderCropBox{0, 0, width, height};

    const auto drmFormat = monitor->m_output && monitor->m_output->state ? monitor->m_output->state->state().drmFormat : DRM_FORMAT_ABGR8888;
    SP<CFramebuffer> localFramebuffer;
    SP<CFramebuffer>& framebuffer = options.framebufferOverride ? *options.framebufferOverride :
        ((!budget && !trimToAlphaBounds) ? reusableWindowRecordingFramebuffer() : localFramebuffer);
    if (!ensureFramebuffer(framebuffer, "hyprcapture-window", framebufferWidth, framebufferHeight, drmFormat))
        return {};
    if (timingEnabled())
        traceTiming(std::format("window.target.{}x{}", framebufferWidth, framebufferHeight));

    const bool previousBlockFeedback = g_pHyprRenderer->m_bBlockSurfaceFeedback;
    const bool previousBlockShader = g_pHyprRenderer->m_renderData.blockScreenShader;
    const bool previousRenderingSnapshot = g_pHyprRenderer->m_bRenderingSnapshot;

    const auto renderIntoFramebuffer = [&](SP<CFramebuffer> targetFramebuffer, SP<CFramebuffer> backgroundMatte = {}) {
        if (!targetFramebuffer)
            return false;
        ScopedTiming timing("window.render");
        CRegion fakeDamage{0, 0, framebufferWidth, framebufferHeight};
        g_pHyprOpenGL->makeEGLCurrent();
        WindowLocalProjectionScope projection(monitor);
        g_pHyprRenderer->m_bBlockSurfaceFeedback = true;
        targetFramebuffer->setImageDescription(monitor->workBufferImageDescription());
        if (!g_pHyprRenderer->beginFullFakeRender(monitor, fakeDamage, targetFramebuffer)) {
            g_pHyprRenderer->m_bBlockSurfaceFeedback = previousBlockFeedback;
            return false;
        }
        projection.apply(framebufferWidth, framebufferHeight);

        g_pHyprRenderer->m_bRenderingSnapshot = true;
        FullSurfaceVisibleRegionOverride fullVisibleRegion(window);
        g_pHyprRenderer->draw(CClearPassElement::SClearData{options.clearColor});
        g_pHyprRenderer->startRenderPass();
        if (options.backgroundTexture) {
            if (backgroundMatte)
                renderTextureWithAlphaMatte(options.backgroundTexture, renderCropBox, backgroundMatte);
            else
                g_pHyprOpenGL->renderTexture(options.backgroundTexture, renderCropBox, {.a = 1.0F});
        }
        g_pHyprRenderer->renderWindow(window, monitor, frozenTime, decorate, RENDER_PASS_ALL, false, false);
        g_pHyprRenderer->m_bRenderingSnapshot = previousRenderingSnapshot;

        g_pHyprRenderer->m_renderData.blockScreenShader = true;
        g_pHyprRenderer->endRender();
        g_pHyprRenderer->m_renderData.blockScreenShader = previousBlockShader;
        g_pHyprRenderer->m_bBlockSurfaceFeedback = previousBlockFeedback;
        return true;
    };

    const int cropX = clampedIntFromDouble(renderCropBox.x);
    const int cropY = clampedIntFromDouble(renderCropBox.y);

    RgbaReadback readback;
    if (options.backgroundTexture && options.clipBackgroundToWindow) {
        auto& matteFramebuffer = reusableWindowRecordingMaskFramebuffer();
        if (!ensureFramebuffer(matteFramebuffer, "hyprcapture-window-mask", width, height, drmFormat))
            return {};

        const auto renderMask = [&]() {
            ScopedTiming timing("window.mask_render");
            CRegion fakeDamage{0, 0, framebufferWidth, framebufferHeight};
            g_pHyprOpenGL->makeEGLCurrent();
            WindowLocalProjectionScope projection(monitor);
            g_pHyprRenderer->m_bBlockSurfaceFeedback = true;
            matteFramebuffer->setImageDescription(monitor->workBufferImageDescription());
            if (!g_pHyprRenderer->beginFullFakeRender(monitor, fakeDamage, matteFramebuffer)) {
                g_pHyprRenderer->m_bBlockSurfaceFeedback = previousBlockFeedback;
                return false;
            }

            projection.apply(framebufferWidth, framebufferHeight);
            g_pHyprRenderer->m_bRenderingSnapshot = true;
            FullSurfaceVisibleRegionOverride fullVisibleRegion(window);
            g_pHyprRenderer->draw(CClearPassElement::SClearData{CHyprColor{0.0, 0.0, 0.0, 0.0}});
            g_pHyprRenderer->startRenderPass();
            g_pHyprRenderer->renderWindow(window, monitor, frozenTime, decorate, RENDER_PASS_ALL, false, false);
            g_pHyprRenderer->m_bRenderingSnapshot = previousRenderingSnapshot;

            g_pHyprRenderer->m_renderData.blockScreenShader = true;
            g_pHyprRenderer->endRender();
            g_pHyprRenderer->m_renderData.blockScreenShader = previousBlockShader;
            g_pHyprRenderer->m_bBlockSurfaceFeedback = previousBlockFeedback;
            return true;
        };

        if (!renderMask() || !renderIntoFramebuffer(framebuffer, matteFramebuffer))
            return {};
    } else if (!renderIntoFramebuffer(framebuffer)) {
        return {};
    }

    // A streaming caller renders through this exact path but owns the later
    // fenced PBO readback. This keeps recording behavior and its PBO state
    // independent from window-streaming state.
    if (options.skipReadback) {
        artifactBox = CBox{geometry.x, geometry.y, geometry.width, geometry.height};
        return {.pixels = {}, .cropX = cropX, .cropTopY = cropY, .width = width, .height = height};
    }

    ScopedTiming timing("window.readback");
    readback = readRgbaFramebufferRegion(*framebuffer, 0, 0, width, height, false, options.asyncReadback);
    if (readback.pixels.empty())
        return {};

    if (trimToAlphaBounds) {
        if (readback.pixels.empty())
            return {};

        PixelBounds bounds;
        if (findAlphaBounds(readback, bounds))
            readback = cropReadback(readback, bounds);
        else
            readback = readRgbaFramebufferRegion(*framebuffer, cropX, cropY, width, height);
    }
    if (readback.pixels.empty())
        return {};

    width = readback.width;
    height = readback.height;
    artifactBox = CBox{geometry.x + readback.cropX / scale, geometry.y + readback.cropTopY / scale, width / scale, height / scale};

    if (options.postprocessAlpha) {
        ScopedTiming timing("window.alpha_postprocess");
        repairTopTransparentSeam(readback);
        unpremultiplyAlpha(readback);
    }

    return readback;
}

bool renderWindowArtifact(const PHLWINDOW& window,
                          const PHLMONITOR& monitor,
                          const Time::steady_tp& frozenTime,
                          bool decorate,
                          const std::filesystem::path& path,
                          int& width,
                          int& height,
                          CBox& artifactBox,
                          ArtifactBudget& budget) {
    auto readback = renderWindowArtifactReadback(window, monitor, frozenTime, decorate, width, height, artifactBox, &budget, true);
    if (decorate && !readback.pixels.empty()) {
        expandReadbackToShadowBounds(readback, artifactBox, renderedWindowGoalMainSurfaceBox(window), window);
        repairTransparentShadow(readback, artifactBox, renderedWindowGoalMainSurfaceBox(window), window);
        width = readback.width;
        height = readback.height;
    }
    return !readback.pixels.empty() && writeRgbaFile(path, readback.pixels);
}

void* findFunctionByDemangledName(const std::string& lookupName, const std::string& demangledNeedle) {
    const auto matches = HyprlandAPI::findFunctionsByName(g_pluginHandle, lookupName);
    const auto it = std::find_if(matches.begin(), matches.end(), [&](const SFunctionMatch& match) {
        return match.demangled.find(demangledNeedle) != std::string::npos;
    });

    if (it != matches.end())
        return it->address;
    return nullptr;
}

void* findRenderWindowFunction() {
    return findFunctionByDemangledName("renderWindow", "CHyprRenderer::renderWindow(");
}

void* findRenderTextureInternalFunction() {
    return findFunctionByDemangledName("renderTextureInternal", "CHyprOpenGLImpl::renderTextureInternal(");
}

void* findRenderTextureWithBlurInternalFunction() {
    return findFunctionByDemangledName("renderTextureWithBlurInternal", "CHyprOpenGLImpl::renderTextureWithBlurInternal(");
}

void removeRealBackgroundHook(CFunctionHook*& hook) {
    if (!hook)
        return;
    hook->unhook();
    HyprlandAPI::removeFunctionHook(g_pluginHandle, hook);
    hook = nullptr;
}

void shutdownRealBackgroundHooks() {
    g_realBackgroundRenderHookContext = nullptr;
    g_renderWindowOriginal = nullptr;
    g_renderTextureInternalOriginal = nullptr;
    g_renderTextureWithBlurInternalOriginal = nullptr;
    removeRealBackgroundHook(g_realBackgroundRenderTextureWithBlurInternalHook);
    removeRealBackgroundHook(g_realBackgroundRenderTextureInternalHook);
    removeRealBackgroundHook(g_realBackgroundRenderWindowHook);
}

bool ensureRealBackgroundHooks() {
    if (g_realBackgroundRenderWindowHook && g_renderWindowOriginal)
        return true;

    shutdownRealBackgroundHooks();

    void* renderWindowSource = findRenderWindowFunction();
    if (!renderWindowSource)
        return false;

    g_realBackgroundRenderWindowHook = HyprlandAPI::createFunctionHook(g_pluginHandle, renderWindowSource, reinterpret_cast<void*>(&hkRenderWindow));
    if (!g_realBackgroundRenderWindowHook || !g_realBackgroundRenderWindowHook->hook()) {
        shutdownRealBackgroundHooks();
        return false;
    }

    g_renderWindowOriginal = reinterpret_cast<RenderWindowFn>(g_realBackgroundRenderWindowHook->m_original);
    if (!g_renderWindowOriginal) {
        shutdownRealBackgroundHooks();
        return false;
    }

    void* renderTextureInternalSource = findRenderTextureInternalFunction();
    void* renderTextureWithBlurInternalSource = findRenderTextureWithBlurInternalFunction();
    if (!renderTextureInternalSource || !renderTextureWithBlurInternalSource)
        return true;

    g_realBackgroundRenderTextureInternalHook =
        HyprlandAPI::createFunctionHook(g_pluginHandle, renderTextureInternalSource, reinterpret_cast<void*>(&hkRenderTextureInternal));
    g_realBackgroundRenderTextureWithBlurInternalHook =
        HyprlandAPI::createFunctionHook(g_pluginHandle, renderTextureWithBlurInternalSource, reinterpret_cast<void*>(&hkRenderTextureWithBlurInternal));

    const bool textureHooksReady = g_realBackgroundRenderTextureInternalHook && g_realBackgroundRenderTextureWithBlurInternalHook &&
        g_realBackgroundRenderTextureInternalHook->hook() && g_realBackgroundRenderTextureWithBlurInternalHook->hook();

    if (!textureHooksReady) {
        removeRealBackgroundHook(g_realBackgroundRenderTextureWithBlurInternalHook);
        removeRealBackgroundHook(g_realBackgroundRenderTextureInternalHook);
        return true;
    }

    g_renderTextureInternalOriginal = reinterpret_cast<RenderTextureInternalFn>(g_realBackgroundRenderTextureInternalHook->m_original);
    g_renderTextureWithBlurInternalOriginal =
        reinterpret_cast<RenderTextureWithBlurInternalFn>(g_realBackgroundRenderTextureWithBlurInternalHook->m_original);

    if (!g_renderTextureInternalOriginal || !g_renderTextureWithBlurInternalOriginal) {
        g_renderTextureInternalOriginal = nullptr;
        g_renderTextureWithBlurInternalOriginal = nullptr;
        removeRealBackgroundHook(g_realBackgroundRenderTextureWithBlurInternalHook);
        removeRealBackgroundHook(g_realBackgroundRenderTextureInternalHook);
    }

    return true;
}

std::vector<RgbaReadback> renderRealBackgroundReadbacksForMonitor(const PHLMONITOR& monitor,
                                                                  const Time::steady_tp& frozenTime,
                                                                  const std::vector<RealBackgroundCaptureTarget>& targets) {
    std::vector<RgbaReadback> readbacks(targets.size());
    if (!monitor || !monitor->m_activeWorkspace || targets.empty() || !g_pHyprRenderer || !g_pHyprOpenGL)
        return readbacks;

    HymissionRawWindowRenderScope rawWindowRenderScope;
    const int framebufferWidth = positiveRoundedIntFromDouble(monitor->m_pixelSize.x);
    const int framebufferHeight = positiveRoundedIntFromDouble(monitor->m_pixelSize.y);
    std::size_t framebufferBytes = 0;
    if (!checkedRgbaByteSize(framebufferWidth, framebufferHeight, framebufferBytes))
        return readbacks;
    const double scale = monitor->m_scale <= 0.0 ? 1.0 : monitor->m_scale;

    std::vector<RealBackgroundCaptureState> states;
    states.reserve(targets.size());
    for (std::size_t targetIndex = 0; targetIndex < targets.size(); ++targetIndex) {
        const auto& target = targets[targetIndex];
        if (!target.window)
            continue;

        CBox cropBox = target.artifactBox.copy().translate(-monitor->m_position).scale(scale).round();
        RealBackgroundCaptureState state;
        state.window = target.window;
        state.targetIndex = targetIndex;
        state.cropX = clampedIntFromDouble(cropBox.x);
        state.cropY = clampedIntFromDouble(cropBox.y);
        state.width = positiveIntFromDouble(cropBox.w);
        state.height = positiveIntFromDouble(cropBox.h);
        states.push_back(std::move(state));
    }
    if (states.empty())
        return readbacks;

    const auto drmFormat = monitor->m_output && monitor->m_output->state ? monitor->m_output->state->state().drmFormat : DRM_FORMAT_ABGR8888;
    auto framebuffer = createFramebuffer("hyprcapture-real-background", framebufferWidth, framebufferHeight, drmFormat);
    if (!framebuffer)
        return readbacks;

    if (!ensureRealBackgroundHooks())
        return readbacks;

    RealBackgroundRenderHookContext hookContext{.monitor = monitor, .states = &states};
    g_realBackgroundRenderHookContext = &hookContext;
    const auto clearHookContext = [&]() {
        g_realBackgroundRenderHookContext = nullptr;
    };

    const bool previousBlockFeedback = g_pHyprRenderer->m_bBlockSurfaceFeedback;
    const bool previousBlockShader = g_pHyprRenderer->m_renderData.blockScreenShader;
    CRegion fakeDamage{0, 0, framebufferWidth, framebufferHeight};

    g_pHyprOpenGL->makeEGLCurrent();
    g_pHyprRenderer->m_bBlockSurfaceFeedback = true;
    if (!g_pHyprRenderer->beginRender(monitor, fakeDamage, RENDER_MODE_FULL_FAKE, nullptr, framebuffer)) {
        g_pHyprRenderer->m_bBlockSurfaceFeedback = previousBlockFeedback;
        clearHookContext();
        return readbacks;
    }

    g_pHyprRenderer->draw(CClearPassElement::SClearData{CHyprColor{0.0, 0.0, 0.0, 1.0}});
    g_pHyprRenderer->renderWorkspace(monitor, monitor->m_activeWorkspace, frozenTime, CBox{0, 0, static_cast<double>(framebufferWidth), static_cast<double>(framebufferHeight)});
    g_pHyprRenderer->m_renderData.blockScreenShader = true;
    g_pHyprRenderer->endRender();
    clearHookContext();
    g_pHyprRenderer->m_renderPass.clear();
    g_pHyprRenderer->m_renderData.blockScreenShader = previousBlockShader;
    g_pHyprRenderer->m_bBlockSurfaceFeedback = previousBlockFeedback;

    for (std::size_t i = 0; i < states.size(); ++i) {
        auto& state = states[i];
        if (!state.captured || state.readback.pixels.empty() || state.targetIndex >= readbacks.size())
            continue;

        readbacks[state.targetIndex] = std::move(state.readback);
    }

    return readbacks;
}

bool renderRealBackgroundFramebufferForMonitor(const PHLMONITOR& monitor,
                                               const Time::steady_tp& frozenTime,
                                               const RealBackgroundCaptureTarget& target,
                                               CFramebuffer& targetFramebuffer) {
    ScopedTiming timing("realbg.framebuffer_total");

    if (!monitor || !monitor->m_activeWorkspace || !target.window || !targetFramebuffer.isAllocated() || !g_pHyprRenderer || !g_pHyprOpenGL)
        return false;

    HymissionRawWindowRenderScope rawWindowRenderScope;
    const int framebufferWidth = positiveRoundedIntFromDouble(monitor->m_pixelSize.x);
    const int framebufferHeight = positiveRoundedIntFromDouble(monitor->m_pixelSize.y);
    std::size_t framebufferBytes = 0;
    if (!checkedRgbaByteSize(framebufferWidth, framebufferHeight, framebufferBytes))
        return false;

    const double scale = monitor->m_scale <= 0.0 ? 1.0 : monitor->m_scale;
    CBox         cropBox = target.artifactBox.copy().translate(-monitor->m_position).scale(scale).round();
    RealBackgroundCaptureState state;
    state.window = target.window;
    state.framebuffer = &targetFramebuffer;
    state.cropX = clampedIntFromDouble(cropBox.x);
    state.cropY = clampedIntFromDouble(cropBox.y);
    state.width = positiveIntFromDouble(cropBox.w);
    state.height = positiveIntFromDouble(cropBox.h);
    if (state.width <= 0 || state.height <= 0)
        return false;

    std::vector<RealBackgroundCaptureState> states;
    states.push_back(std::move(state));

    const auto drmFormat = monitor->m_output && monitor->m_output->state ? monitor->m_output->state->state().drmFormat : DRM_FORMAT_ABGR8888;
    auto framebuffer = createFramebuffer("hyprcapture-real-background-framebuffer", framebufferWidth, framebufferHeight, drmFormat);
    if (!framebuffer)
        return false;

    if (!ensureRealBackgroundHooks())
        return false;

    RealBackgroundRenderHookContext hookContext{.monitor = monitor, .states = &states};
    g_realBackgroundRenderHookContext = &hookContext;
    const auto clearHookContext = [&]() {
        g_realBackgroundRenderHookContext = nullptr;
    };

    const bool previousBlockFeedback = g_pHyprRenderer->m_bBlockSurfaceFeedback;
    const bool previousBlockShader = g_pHyprRenderer->m_renderData.blockScreenShader;
    CRegion    fakeDamage{0, 0, framebufferWidth, framebufferHeight};

    g_pHyprOpenGL->makeEGLCurrent();
    g_pHyprRenderer->m_bBlockSurfaceFeedback = true;
    if (!g_pHyprRenderer->beginRender(monitor, fakeDamage, RENDER_MODE_FULL_FAKE, nullptr, framebuffer)) {
        g_pHyprRenderer->m_bBlockSurfaceFeedback = previousBlockFeedback;
        clearHookContext();
        return false;
    }

    g_pHyprRenderer->draw(CClearPassElement::SClearData{CHyprColor{0.0, 0.0, 0.0, 1.0}});
    {
        ScopedTiming timing("realbg.render_workspace");
        g_pHyprRenderer->renderWorkspace(monitor, monitor->m_activeWorkspace, frozenTime, CBox{0, 0, static_cast<double>(framebufferWidth), static_cast<double>(framebufferHeight)});
    }
    g_pHyprRenderer->m_renderData.blockScreenShader = true;
    g_pHyprRenderer->endRender();
    clearHookContext();
    g_pHyprRenderer->m_renderPass.clear();
    g_pHyprRenderer->m_renderData.blockScreenShader = previousBlockShader;
    g_pHyprRenderer->m_bBlockSurfaceFeedback = previousBlockFeedback;

    return !states.empty() && states.front().captured;
}

void renderRealBackgroundArtifactsForMonitor(const PHLMONITOR& monitor,
                                             const Time::steady_tp& frozenTime,
                                             const std::vector<PendingRealBackgroundCapture*>& requests,
                                             CaptureSession& session,
                                             ArtifactBudget& budget) {
    if (requests.empty())
        return;

    std::vector<RealBackgroundCaptureTarget> targets;
    targets.reserve(requests.size());
    for (const auto* request : requests)
        targets.push_back(request ? RealBackgroundCaptureTarget{.window = request->window, .artifactBox = request->artifactBox} : RealBackgroundCaptureTarget{});

    auto readbacks = renderRealBackgroundReadbacksForMonitor(monitor, frozenTime, targets);
    for (std::size_t i = 0; i < readbacks.size() && i < requests.size(); ++i) {
        auto* request = requests[i];
        if (!request || request->windowIndex >= session.windows.size() || readbacks[i].pixels.empty())
            continue;

        if (!writeRgbaFile(request->path, readbacks[i].pixels, &budget))
            continue;

        auto& info = session.windows[request->windowIndex];
        info.realBackgroundPath = request->path.string();
        info.realBackgroundWidth = readbacks[i].width;
        info.realBackgroundHeight = readbacks[i].height;
        info.realBackgroundTopDown = true;
    }
}

bool shouldCaptureWindow(const PHLWINDOW& window) {
    if (!window || !window->m_isMapped || window->isHidden() || !window->m_workspace)
        return false;

    if (g_pHyprRenderer)
        return g_pHyprRenderer->shouldRenderWindow(window);

    return window->m_pinned || window->m_workspace->isVisible();
}

bool isLiveWindowCaptureTarget(const PHLWINDOW& window) {
    return window && window->m_isMapped && !window->isHidden() && window->m_workspace;
}

std::vector<PHLWINDOW> windowsInRenderOrder() {
    std::vector<PHLWINDOW> ordered;
    if (!g_pCompositor)
        return ordered;

    ordered.reserve(Desktop::windowState()->windows().size());
    const auto appendPass = [&](bool special, bool floating) {
        for (const auto& window : Desktop::windowState()->windows()) {
            if (!shouldCaptureWindow(window) || (window->m_pinned && window->m_isFloating) || window->onSpecialWorkspace() != special ||
                window->m_isFloating != floating)
                continue;

            ordered.push_back(window);
        }
    };

    appendPass(false, false);
    appendPass(false, true);
    appendPass(true, false);
    appendPass(true, true);

    for (const auto& window : Desktop::windowState()->windows()) {
        if (!shouldCaptureWindow(window) || !window->m_pinned || !window->m_isFloating)
            continue;

        ordered.push_back(window);
    }

    return ordered;
}

void populateWorkspaceWindowMetadata(const PHLMONITOR& monitor, MonitorInfo& info) {
    if (!monitor)
        return;

    std::vector<PHLWINDOW> workspaceWindows;
    workspaceWindows.reserve(Desktop::windowState()->windows().size());
    for (const auto& window : Desktop::windowState()->windows()) {
        if (!window || !window->m_isMapped || window->isHidden() || !window->m_workspace)
            continue;

        const auto windowMonitor = window->m_monitor.lock();
        const bool onActiveWorkspace = window->m_workspace == monitor->m_activeWorkspace;
        const bool onActiveSpecialWorkspace =
            monitor->m_activeSpecialWorkspace && window->m_workspace == monitor->m_activeSpecialWorkspace;
        const bool pinnedOnMonitor = window->m_pinned && windowMonitor && windowMonitor == monitor;
        if (onActiveWorkspace || onActiveSpecialWorkspace || pinnedOnMonitor)
            workspaceWindows.push_back(window);
    }

    info.workspaceWindowCount = static_cast<int>(std::min(workspaceWindows.size(), MAX_SESSION_WINDOWS));
    if (workspaceWindows.size() == 1) {
        info.singleWorkspaceWindowClass = boundedString(workspaceWindows.front()->m_class, MAX_WINDOW_METADATA_BYTES);
        info.singleWorkspaceWindowTitle = boundedString(workspaceWindows.front()->m_title, MAX_WINDOW_METADATA_BYTES);
    }
}

void blitScaledRgba(const RgbaReadback& source,
                    std::vector<unsigned char>& target,
                    int targetWidth,
                    int targetHeight,
                    int targetX,
                    int targetY,
                    int targetPartWidth,
                    int targetPartHeight) {
    if (source.pixels.empty() || source.width <= 0 || source.height <= 0 || target.empty() || targetWidth <= 0 || targetHeight <= 0 || targetPartWidth <= 0 ||
        targetPartHeight <= 0)
        return;

    for (int y = 0; y < targetPartHeight; ++y) {
        const int dstY = targetY + y;
        if (dstY < 0 || dstY >= targetHeight)
            continue;

        const int srcY = std::clamp(static_cast<int>((static_cast<long long>(y) * source.height) / targetPartHeight), 0, source.height - 1);
        for (int x = 0; x < targetPartWidth; ++x) {
            const int dstX = targetX + x;
            if (dstX < 0 || dstX >= targetWidth)
                continue;

            const int srcX = std::clamp(static_cast<int>((static_cast<long long>(x) * source.width) / targetPartWidth), 0, source.width - 1);
            const auto src = (static_cast<std::size_t>(srcY) * source.width + srcX) * RGBA_BYTES_PER_PIXEL;
            const auto dst = (static_cast<std::size_t>(dstY) * targetWidth + dstX) * RGBA_BYTES_PER_PIXEL;
            std::copy(source.pixels.data() + src, source.pixels.data() + src + RGBA_BYTES_PER_PIXEL, target.data() + dst);
        }
    }
}

RgbaReadback cropReadbackToBounds(const RgbaReadback& source, const PixelBounds& bounds) {
    const PixelBounds clipped{.x = std::clamp(bounds.x, 0, std::max(0, source.width - 1)),
                              .y = std::clamp(bounds.y, 0, std::max(0, source.height - 1)),
                              .width = std::max(0, std::min(bounds.width, source.width - std::clamp(bounds.x, 0, std::max(0, source.width - 1)))),
                              .height = std::max(0, std::min(bounds.height, source.height - std::clamp(bounds.y, 0, std::max(0, source.height - 1))))};
    return cropReadback(source, clipped);
}

bool readbackHasSize(const RgbaReadback& frame, int width, int height) {
    std::size_t bytes = 0;
    return frame.width == width && frame.height == height && checkedRgbaByteSize(width, height, bytes) && frame.pixels.size() == bytes;
}

RgbaReadback normalizeMonitorReadbackToLogicalOrientation(RgbaReadback readback, int transform) {
    auto frame = normalizeRgbaFrameToLogicalOrientation(
        {.pixels = std::move(readback.pixels), .width = readback.width, .height = readback.height},
        transform);
    return {
        .pixels = std::move(frame.pixels),
        .width = frame.width,
        .height = frame.height,
    };
}

RgbaReadback solidBackgroundReadback(int width, int height, unsigned char r, unsigned char g, unsigned char b) {
    std::size_t bytes = 0;
    if (!checkedRgbaByteSize(width, height, bytes))
        return {};

    RgbaReadback background;
    background.width = width;
    background.height = height;
    background.pixels.assign(bytes, 0);
    for (std::size_t i = 0; i + 3 < background.pixels.size(); i += RGBA_BYTES_PER_PIXEL) {
        background.pixels[i + 0] = r;
        background.pixels[i + 1] = g;
        background.pixels[i + 2] = b;
        background.pixels[i + 3] = 255;
    }
    return background;
}

bool isWindowContentPixel(const unsigned char* px) {
    const int alpha = px[3];
    if (alpha < WINDOW_BACKGROUND_MIN_ALPHA)
        return false;

    const int maxRgb = std::max({px[0], px[1], px[2]});
    return maxRgb > WINDOW_SHADOW_MAX_RGB || alpha > WINDOW_SHADOW_MAX_ALPHA;
}

bool isRecordingShadowPixel(const unsigned char* px) {
    const int alpha = px[3];
    if (alpha <= 0 || alpha > RECORDING_SHADOW_MAX_ALPHA)
        return false;

    const int maxRgb = std::max({px[0], px[1], px[2]});
    return maxRgb <= RECORDING_SHADOW_MAX_RGB;
}

bool isShadowColoredPixel(const unsigned char* px) {
    if (px[3] <= 0)
        return false;

    const int maxRgb = std::max({px[0], px[1], px[2]});
    return maxRgb <= RECORDING_SHADOW_MAX_RGB;
}

int colorByte(float value) {
    return std::clamp(static_cast<int>(std::lround(value * 255.0F)), 0, 255);
}

ShadowColorBytes shadowColorBytes(const PHLWINDOW& window) {
    const CHyprColor color = window && !window->m_realShadowColor.m_colors.empty() ? window->m_realShadowColor.m_colors.front() : CHyprColor(0xee1a1a1a);
    return {
        .r = colorByte(color.r),
        .g = colorByte(color.g),
        .b = colorByte(color.b),
        .a = colorByte(color.a),
    };
}

int reconstructedShadowAlpha(const unsigned char* px, const ShadowColorBytes& color) {
    int alpha = 0;
    const auto channelAlpha = [&](int pixelChannel, int colorChannel) {
        if (colorChannel <= 0)
            return 0;
        return static_cast<int>(std::lround(static_cast<double>(pixelChannel) * px[3] / colorChannel));
    };

    alpha = std::max(alpha, channelAlpha(px[0], color.r));
    alpha = std::max(alpha, channelAlpha(px[1], color.g));
    alpha = std::max(alpha, channelAlpha(px[2], color.b));
    return std::clamp(alpha, 0, color.a);
}

double modifiedShadowLength(double x, double y, double roundingPower) {
    roundingPower = std::clamp(roundingPower, 1.0, 10.0);
    return std::pow(std::pow(std::abs(x), roundingPower) + std::pow(std::abs(y), roundingPower), 1.0 / roundingPower);
}

double hyprlandRoundedShadowMultiplier(double x,
                                       double y,
                                       double fullWidth,
                                       double fullHeight,
                                       double range,
                                       double rounding,
                                       double roundingPower,
                                       int    shadowPower) {
    if (range <= 0.0 || fullWidth <= 0.0 || fullHeight <= 0.0)
        return 0.0;

    const double radius = range + std::max(0.0, rounding);
    const double left = range + std::max(0.0, rounding);
    const double top = range + std::max(0.0, rounding);
    const double right = fullWidth - left;
    const double bottom = fullHeight - top;

    const auto roundedDistanceMultiplier = [&](double distanceToCorner) {
        if (distanceToCorner > radius)
            return 0.0;
        if (distanceToCorner > radius - range)
            return std::pow((radius - distanceToCorner) / range, shadowPower);
        return 1.0;
    };

    bool   corner = false;
    double multiplier = 1.0;
    if (x < left) {
        if (y < top) {
            multiplier = roundedDistanceMultiplier(modifiedShadowLength(x - left, y - top, roundingPower));
            corner = true;
        } else if (y > bottom) {
            multiplier = roundedDistanceMultiplier(modifiedShadowLength(x - left, y - bottom, roundingPower));
            corner = true;
        }
    } else if (x > right) {
        if (y < top) {
            multiplier = roundedDistanceMultiplier(modifiedShadowLength(x - right, y - top, roundingPower));
            corner = true;
        } else if (y > bottom) {
            multiplier = roundedDistanceMultiplier(modifiedShadowLength(x - right, y - bottom, roundingPower));
            corner = true;
        }
    }

    if (!corner) {
        const double smallest = std::min({y, fullHeight - y, x, fullWidth - x});
        if (smallest < range)
            multiplier = std::pow(std::clamp(smallest / range, 0.0, 1.0), shadowPower);
    }

    return std::clamp(multiplier, 0.0, 1.0);
}

bool hyprlandPointInRoundedRect(double x, double y, double left, double top, double right, double bottom, double radius, double roundingPower) {
    if (x < left || x > right || y < top || y > bottom)
        return false;

    if (radius <= 0.0)
        return true;

    radius = std::min(radius, std::min((right - left) * 0.5, (bottom - top) * 0.5));
    const double innerLeft = left + radius;
    const double innerTop = top + radius;
    const double innerRight = right - radius;
    const double innerBottom = bottom - radius;

    if (x >= innerLeft && x <= innerRight)
        return true;
    if (y >= innerTop && y <= innerBottom)
        return true;

    const double dx = x < innerLeft ? innerLeft - x : x - innerRight;
    const double dy = y < innerTop ? innerTop - y : y - innerBottom;
    return modifiedShadowLength(dx, dy, roundingPower) <= radius;
}

double shadowRoundingPx(const PHLWINDOW& window, double scale) {
    if (!window)
        return 0.0;

    const double borderSize = window->m_X11DoesntWantBorders ? 0.0 : std::max(0, window->getRealBorderSize());
    const double roundingBase = std::max(0.0F, window->rounding());
    const double roundingPower = std::clamp(static_cast<double>(window->roundingPower()), 1.0, 10.0);
    const double correctionOffset = borderSize * (std::sqrt(2.0) - 1.0) * std::max(2.0 - roundingPower, 0.0);
    const double rounding = roundingBase > 0.0 ? (roundingBase + borderSize) - correctionOffset : 0.0;
    return std::max(0.0, rounding * scale);
}

double shadowBorderPx(const PHLWINDOW& window, double scale) {
    if (!window || window->m_X11DoesntWantBorders)
        return 0.0;
    return std::max(0.0, static_cast<double>(std::max(0, window->getRealBorderSize())) * scale);
}

double windowRoundingPx(const PHLWINDOW& window, double scale) {
    if (!window)
        return 0.0;
    return std::max(0.0, static_cast<double>(window->rounding()) * scale);
}

int configuredShadowRangePx(double scale) {
    static auto PSHADOWRANGE = CConfigValue<Config::INTEGER>("decoration:shadow:range");
    return std::max(0, static_cast<int>(std::ceil(static_cast<double>(std::max(0, sc<int>(*PSHADOWRANGE))) * scale)));
}

bool configuredShadowEnabled() {
    static auto PSHADOWS = CConfigValue<Config::INTEGER>("decoration:shadow:enabled");
    return *PSHADOWS == 1;
}

double configuredShadowScale() {
    static auto PSHADOWSCALE = CConfigValue<Config::FLOAT>("decoration:shadow:scale");
    return std::clamp(static_cast<double>(*PSHADOWSCALE), 0.0, 1.0);
}

Vector2D configuredShadowOffsetPx(double scale) {
    static auto PSHADOWOFFSET = CConfigValue<Config::VEC2>("decoration:shadow:offset");
    return {(*PSHADOWOFFSET).x * scale, (*PSHADOWOFFSET).y * scale};
}

bool configuredShadowSharp() {
    static auto PSHADOWSHARP = CConfigValue<Config::INTEGER>("decoration:shadow:sharp");
    return *PSHADOWSHARP == 1;
}

void scaleBoxFromCenter(CBox& box, double scale) {
    const double centerX = box.x + box.w * 0.5;
    const double centerY = box.y + box.h * 0.5;
    box.w *= scale;
    box.h *= scale;
    box.x = centerX - box.w * 0.5;
    box.y = centerY - box.h * 0.5;
}

std::optional<ShadowRenderGeometry> shadowRenderGeometry(const RgbaReadback& readback, const CBox& artifactBox, const CBox& visibleBox, const PHLWINDOW& window) {
    // The geometry is also serialized for the GPU stream, where the pixels
    // remain in the exported FBO.  It depends on the captured dimensions, not
    // on CPU accessibility of the pixels.
    if (!configuredShadowEnabled() || readback.width <= 0 || readback.height <= 0 || artifactBox.w <= 0.0 || artifactBox.h <= 0.0 ||
        visibleBox.w <= 0.0 || visibleBox.h <= 0.0)
        return std::nullopt;

    const double scaleX = static_cast<double>(readback.width) / artifactBox.w;
    const double scaleY = static_cast<double>(readback.height) / artifactBox.h;
    if (!std::isfinite(scaleX) || !std::isfinite(scaleY) || scaleX <= 0.0 || scaleY <= 0.0)
        return std::nullopt;

    const double shadowScale = std::max(scaleX, scaleY);
    const double range = static_cast<double>(configuredShadowRangePx(shadowScale));
    if (range <= 0.0)
        return std::nullopt;

    const double visibleLeft = (visibleBox.x - artifactBox.x) * scaleX;
    const double visibleTop = (visibleBox.y - artifactBox.y) * scaleY;
    const double visibleRight = (visibleBox.x + visibleBox.w - artifactBox.x) * scaleX;
    const double visibleBottom = (visibleBox.y + visibleBox.h - artifactBox.y) * scaleY;
    if (!std::isfinite(visibleLeft) || !std::isfinite(visibleTop) || !std::isfinite(visibleRight) || !std::isfinite(visibleBottom))
        return std::nullopt;

    const double border = shadowBorderPx(window, shadowScale);
    CBox baseBox{visibleLeft - border, visibleTop - border, std::max(1.0, visibleRight - visibleLeft + 2.0 * border),
                 std::max(1.0, visibleBottom - visibleTop + 2.0 * border)};
    CBox shadowBox = baseBox;
    shadowBox.x -= range;
    shadowBox.y -= range;
    shadowBox.w += 2.0 * range;
    shadowBox.h += 2.0 * range;
    scaleBoxFromCenter(shadowBox, configuredShadowScale());
    shadowBox.translate(configuredShadowOffsetPx(shadowScale));

    if (shadowBox.w < 1.0 || shadowBox.h < 1.0)
        return std::nullopt;

    CBox windowCutoutBox{visibleLeft - shadowBox.x, visibleTop - shadowBox.y, std::max(1.0, visibleRight - visibleLeft), std::max(1.0, visibleBottom - visibleTop)};

    static auto PSHADOWPOWER = CConfigValue<Config::INTEGER>("decoration:shadow:render_power");
    return ShadowRenderGeometry{
        .shadowBox = shadowBox,
        .windowCutoutBox = windowCutoutBox,
        .range = range,
        .rounding = shadowRoundingPx(window, shadowScale),
        .windowRounding = windowRoundingPx(window, shadowScale),
        .roundingPower = window ? std::clamp(static_cast<double>(window->roundingPower()), 1.0, 10.0) : 2.0,
        .shadowPower = std::clamp(sc<int>(*PSHADOWPOWER), 1, 4),
        .sharp = configuredShadowSharp(),
    };
}

void expandReadbackToShadowBounds(RgbaReadback& readback, CBox& artifactBox, const CBox& visibleBox, const PHLWINDOW& window) {
    ScopedTiming timing("window.shadow_expand");
    const auto color = shadowColorBytes(window);
    if (readback.width <= 0 || readback.height <= 0 || readback.pixels.empty() || artifactBox.w <= 0.0 || artifactBox.h <= 0.0 || visibleBox.w <= 0.0 ||
        visibleBox.h <= 0.0 || color.a <= 0)
        return;

    const double scaleX = static_cast<double>(readback.width) / artifactBox.w;
    const double scaleY = static_cast<double>(readback.height) / artifactBox.h;
    if (!std::isfinite(scaleX) || !std::isfinite(scaleY) || scaleX <= 0.0 || scaleY <= 0.0)
        return;

    const auto geometry = shadowRenderGeometry(readback, artifactBox, visibleBox, window);
    if (!geometry)
        return;

    const int targetLeft = static_cast<int>(std::floor(geometry->shadowBox.x));
    const int targetTop = static_cast<int>(std::floor(geometry->shadowBox.y));
    const int targetRight = static_cast<int>(std::ceil(geometry->shadowBox.x + geometry->shadowBox.w));
    const int targetBottom = static_cast<int>(std::ceil(geometry->shadowBox.y + geometry->shadowBox.h));
    const int padLeft = std::max(0, -targetLeft);
    const int padTop = std::max(0, -targetTop);
    const int padRight = std::max(0, targetRight - readback.width);
    const int padBottom = std::max(0, targetBottom - readback.height);
    if (padLeft == 0 && padTop == 0 && padRight == 0 && padBottom == 0)
        return;

    const int newWidth = readback.width + padLeft + padRight;
    const int newHeight = readback.height + padTop + padBottom;
    std::size_t bytes = 0;
    if (!checkedRgbaByteSize(newWidth, newHeight, bytes))
        return;

    std::vector<unsigned char> expanded(bytes, 0);
    const std::size_t oldRowBytes = static_cast<std::size_t>(readback.width) * RGBA_BYTES_PER_PIXEL;
    const std::size_t newRowBytes = static_cast<std::size_t>(newWidth) * RGBA_BYTES_PER_PIXEL;
    for (int y = 0; y < readback.height; ++y) {
        const auto* src = readback.pixels.data() + static_cast<std::size_t>(y) * oldRowBytes;
        auto*       dst = expanded.data() + static_cast<std::size_t>(y + padTop) * newRowBytes + static_cast<std::size_t>(padLeft) * RGBA_BYTES_PER_PIXEL;
        std::copy(src, src + oldRowBytes, dst);
    }

    readback.cropX -= padLeft;
    readback.cropTopY -= padTop;
    readback.width = newWidth;
    readback.height = newHeight;
    readback.pixels = std::move(expanded);
    artifactBox.x -= static_cast<double>(padLeft) / scaleX;
    artifactBox.y -= static_cast<double>(padTop) / scaleY;
    artifactBox.w = static_cast<double>(newWidth) / scaleX;
    artifactBox.h = static_cast<double>(newHeight) / scaleY;
}

void repairTransparentShadow(RgbaReadback& readback, const CBox& artifactBox, const CBox& visibleBox, const PHLWINDOW& window) {
    ScopedTiming timing("window.shadow_repair");
    const auto color = shadowColorBytes(window);
    if (readback.width <= 0 || readback.height <= 0 || readback.pixels.empty() || artifactBox.w <= 0.0 || artifactBox.h <= 0.0 || visibleBox.w <= 0.0 ||
        visibleBox.h <= 0.0 || color.a <= 0)
        return;

    const auto geometry = shadowRenderGeometry(readback, artifactBox, visibleBox, window);
    if (!geometry)
        return;

    const double shadowLeft = geometry->shadowBox.x;
    const double shadowTop = geometry->shadowBox.y;
    const double shadowWidth = geometry->shadowBox.w;
    const double shadowHeight = geometry->shadowBox.h;
    const double windowLeft = geometry->windowCutoutBox.x;
    const double windowTop = geometry->windowCutoutBox.y;
    const double windowRight = geometry->windowCutoutBox.x + geometry->windowCutoutBox.w;
    const double windowBottom = geometry->windowCutoutBox.y + geometry->windowCutoutBox.h;

    const int repairLeft = std::clamp(static_cast<int>(std::floor(shadowLeft)), 0, readback.width);
    const int repairTop = std::clamp(static_cast<int>(std::floor(shadowTop)), 0, readback.height);
    const int repairRight = std::clamp(static_cast<int>(std::ceil(shadowLeft + shadowWidth)), repairLeft, readback.width);
    const int repairBottom = std::clamp(static_cast<int>(std::ceil(shadowTop + shadowHeight)), repairTop, readback.height);

    const double cutoutAbsLeft = shadowLeft + windowLeft;
    const double cutoutAbsTop = shadowTop + windowTop;
    const double cutoutAbsRight = shadowLeft + windowRight;
    const double cutoutAbsBottom = shadowTop + windowBottom;
    const int    cutoutLeft = std::clamp(static_cast<int>(std::floor(cutoutAbsLeft)), repairLeft, repairRight);
    const int    cutoutTop = std::clamp(static_cast<int>(std::floor(cutoutAbsTop)), repairTop, repairBottom);
    const int    cutoutRight = std::clamp(static_cast<int>(std::ceil(cutoutAbsRight)), cutoutLeft, repairRight);
    const int    cutoutBottom = std::clamp(static_cast<int>(std::ceil(cutoutAbsBottom)), cutoutTop, repairBottom);

    const auto repairPixel = [&](int x, int y, bool testCutout) {
        const double centerX = x - shadowLeft + 0.5;
        const double centerY = y - shadowTop + 0.5;
        if (testCutout &&
            hyprlandPointInRoundedRect(centerX, centerY, windowLeft, windowTop, windowRight, windowBottom, geometry->windowRounding, geometry->roundingPower))
            return;

        const auto i = (static_cast<std::size_t>(y) * readback.width + x) * RGBA_BYTES_PER_PIXEL;
        auto*      px = readback.pixels.data() + i;
        const bool existingShadowPixel = isRecordingShadowPixel(px);
        const bool shadowColoredPixel = isShadowColoredPixel(px);
        const bool transparentShadowPadding = px[3] == 0;
        if (!shadowColoredPixel && !transparentShadowPadding)
            return;

        const double multiplier = geometry->sharp ? 1.0 :
                                                    hyprlandRoundedShadowMultiplier(centerX,
                                                                                   centerY,
                                                                                   shadowWidth,
                                                                                   shadowHeight,
                                                                                   geometry->range,
                                                                                   geometry->rounding,
                                                                                   geometry->roundingPower,
                                                                                   geometry->shadowPower);
        int          repairedAlpha = std::clamp(static_cast<int>(std::lround(color.a * multiplier)), 0, color.a);
        if (repairedAlpha <= 0 && existingShadowPixel)
            repairedAlpha = reconstructedShadowAlpha(px, color);

        px[3] = static_cast<unsigned char>(repairedAlpha);
        if (px[3] == 0) {
            px[0] = 0;
            px[1] = 0;
            px[2] = 0;
            return;
        }
        px[0] = static_cast<unsigned char>(color.r);
        px[1] = static_cast<unsigned char>(color.g);
        px[2] = static_cast<unsigned char>(color.b);
    };

    const auto repairRegion = [&](int left, int top, int right, int bottom, bool testCutout) {
        if (left >= right || top >= bottom)
            return;
        for (int y = top; y < bottom; ++y) {
            for (int x = left; x < right; ++x)
                repairPixel(x, y, testCutout);
        }
    };

    repairRegion(repairLeft, repairTop, repairRight, cutoutTop, false);
    repairRegion(repairLeft, cutoutBottom, repairRight, repairBottom, false);
    repairRegion(repairLeft, cutoutTop, cutoutLeft, cutoutBottom, false);
    repairRegion(cutoutRight, cutoutTop, repairRight, cutoutBottom, false);

    const int cornerRadius = std::clamp(static_cast<int>(std::ceil(geometry->windowRounding)), 0, std::max(cutoutRight - cutoutLeft, cutoutBottom - cutoutTop));
    if (cornerRadius <= 0)
        return;

    repairRegion(cutoutLeft, cutoutTop, std::min(cutoutLeft + cornerRadius, cutoutRight), std::min(cutoutTop + cornerRadius, cutoutBottom), true);
    repairRegion(std::max(cutoutRight - cornerRadius, cutoutLeft), cutoutTop, cutoutRight, std::min(cutoutTop + cornerRadius, cutoutBottom), true);
    repairRegion(cutoutLeft, std::max(cutoutBottom - cornerRadius, cutoutTop), std::min(cutoutLeft + cornerRadius, cutoutRight), cutoutBottom, true);
    repairRegion(std::max(cutoutRight - cornerRadius, cutoutLeft),
                 std::max(cutoutBottom - cornerRadius, cutoutTop),
                 cutoutRight,
                 cutoutBottom,
                 true);
}

void compositeWindowOverBackground(RgbaReadback& frame, const RgbaReadback& background) {
    if (!readbackHasSize(frame, background.width, background.height))
        return;

    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            const auto i = (static_cast<std::size_t>(y) * frame.width + x) * RGBA_BYTES_PER_PIXEL;
            if (!isWindowContentPixel(frame.pixels.data() + i))
                continue;

            const int  alpha = frame.pixels[i + 3];
            if (alpha >= 255) {
                frame.pixels[i + 3] = 255;
                continue;
            }

            const int inverseAlpha = 255 - alpha;
            for (int channel = 0; channel < 3; ++channel)
                frame.pixels[i + channel] =
                    static_cast<unsigned char>((frame.pixels[i + channel] * alpha + background.pixels[i + channel] * inverseAlpha + 127) / 255);
            frame.pixels[i + 3] = 255;
        }
    }
}

void compositePixelOverSolidBackground(unsigned char* px, unsigned char r, unsigned char g, unsigned char b) {
    const int alpha = px[3];
    if (alpha >= 255) {
        px[3] = 255;
        return;
    }
    if (alpha <= 0) {
        px[0] = r;
        px[1] = g;
        px[2] = b;
        px[3] = 255;
        return;
    }

    const int inverseAlpha = 255 - alpha;
    px[0] = static_cast<unsigned char>((px[0] * alpha + r * inverseAlpha + 127) / 255);
    px[1] = static_cast<unsigned char>((px[1] * alpha + g * inverseAlpha + 127) / 255);
    px[2] = static_cast<unsigned char>((px[2] * alpha + b * inverseAlpha + 127) / 255);
    px[3] = 255;
}

void compositeContentOverSolidBackground(RgbaReadback& frame, unsigned char r, unsigned char g, unsigned char b) {
    compositeWindowOverBackground(frame, solidBackgroundReadback(frame.width, frame.height, r, g, b));
}

void compositeOutsideWindowOverSolidBackground(RgbaReadback& frame,
                                               const CBox& artifactBox,
                                               const CBox& visibleBox,
                                               const PHLWINDOW& window,
                                               unsigned char r,
                                               unsigned char g,
                                               unsigned char b) {
    if (frame.width <= 0 || frame.height <= 0 || frame.pixels.empty() || artifactBox.w <= 0.0 || artifactBox.h <= 0.0 || visibleBox.w <= 0.0 || visibleBox.h <= 0.0)
        return;

    const double scaleX = static_cast<double>(frame.width) / artifactBox.w;
    const double scaleY = static_cast<double>(frame.height) / artifactBox.h;
    if (!std::isfinite(scaleX) || !std::isfinite(scaleY) || scaleX <= 0.0 || scaleY <= 0.0)
        return;

    const double visibleLeft = (visibleBox.x - artifactBox.x) * scaleX;
    const double visibleTop = (visibleBox.y - artifactBox.y) * scaleY;
    const double visibleRight = (visibleBox.x + visibleBox.w - artifactBox.x) * scaleX;
    const double visibleBottom = (visibleBox.y + visibleBox.h - artifactBox.y) * scaleY;
    if (!std::isfinite(visibleLeft) || !std::isfinite(visibleTop) || !std::isfinite(visibleRight) || !std::isfinite(visibleBottom))
        return;

    const double rounding = windowRoundingPx(window, std::max(scaleX, scaleY));
    const double roundingPower = window ? std::clamp(static_cast<double>(window->roundingPower()), 1.0, 10.0) : 2.0;
    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            if (hyprlandPointInRoundedRect(x + 0.5, y + 0.5, visibleLeft, visibleTop, visibleRight, visibleBottom, rounding, roundingPower))
                continue;

            auto* px = frame.pixels.data() + (static_cast<std::size_t>(y) * frame.width + x) * RGBA_BYTES_PER_PIXEL;
            compositePixelOverSolidBackground(px, r, g, b);
        }
    }
}

std::optional<std::array<unsigned char, 3>> recordingSolidBackgroundRgb(WindowBackground background) {
    switch (background) {
        case WindowBackground::White: return std::array<unsigned char, 3>{255, 255, 255};
        case WindowBackground::Black: return std::array<unsigned char, 3>{0, 0, 0};
        case WindowBackground::FollowSystem: return std::array<unsigned char, 3>{245, 245, 245};
        case WindowBackground::Real:
        case WindowBackground::Transparent: return std::nullopt;
    }
    return std::nullopt;
}

std::optional<CHyprColor> recordingSolidBackgroundColor(WindowBackground background) {
    switch (background) {
        case WindowBackground::White: return CHyprColor{1.0, 1.0, 1.0, 1.0};
        case WindowBackground::Black: return CHyprColor{0.0, 0.0, 0.0, 1.0};
        case WindowBackground::FollowSystem: return CHyprColor{245.0 / 255.0, 245.0 / 255.0, 245.0 / 255.0, 1.0};
        case WindowBackground::Real:
        case WindowBackground::Transparent: return std::nullopt;
    }
    return std::nullopt;
}

PHLWINDOW findWindowByAddress(const std::string& address) {
    if (address.empty() || !g_pCompositor)
        return {};

    for (const auto& window : Desktop::windowState()->windows()) {
        if (window && "0x" + pointerId(window.get()) == address)
            return window;
    }
    return {};
}

std::optional<RecordingFrame> captureDesktopRegionRecordingFrame(const Rect& targetGeometry) {
    if (!g_pCompositor || !g_pHyprRenderer || targetGeometry.width <= 0.0 || targetGeometry.height <= 0.0)
        return std::nullopt;

    std::vector<PHLMONITOR> intersecting;
    double                  outputScale = 0.0;
    for (const auto& monitor : State::monitorState()->monitors()) {
        if (!monitor)
            continue;
        const Rect monRect = monitorRect(monitor);
        if (!intersects(targetGeometry, monRect))
            continue;
        intersecting.push_back(monitor);
        outputScale = std::max(outputScale, monitor->m_scale <= 0.0 ? 1.0 : monitor->m_scale);
    }

    if (intersecting.empty() || outputScale <= 0.0)
        return std::nullopt;

    const int outputWidth = positiveRoundedIntFromDouble(targetGeometry.width * outputScale);
    const int outputHeight = positiveRoundedIntFromDouble(targetGeometry.height * outputScale);
    std::size_t outputBytes = 0;
    if (!checkedRgbaByteSize(outputWidth, outputHeight, outputBytes))
        return std::nullopt;

    RecordingFrame frame;
    frame.width = outputWidth;
    frame.height = outputHeight;
    frame.rgba.assign(outputBytes, 0);

    const auto now = Time::steadyNow();
    for (const auto& monitor : intersecting) {
        const Rect monRect = monitorRect(monitor);
        const Rect logicalPart = intersection(targetGeometry, monRect);
        if (logicalPart.width <= 0.0 || logicalPart.height <= 0.0)
            continue;

        const double monitorScale = monitor->m_scale <= 0.0 ? 1.0 : monitor->m_scale;
        const int cropX = clampedIntFromDouble((logicalPart.x - monRect.x) * monitorScale);
        const int cropY = clampedIntFromDouble((logicalPart.y - monRect.y) * monitorScale);
        const int cropWidth = positiveRoundedIntFromDouble(logicalPart.width * monitorScale);
        const int cropHeight = positiveRoundedIntFromDouble(logicalPart.height * monitorScale);
        RgbaReadback readback;
        const int    transform = std::clamp(static_cast<int>(monitor->m_transform), 0, 7);
        if (transform == 0) {
            readback = renderMonitorReadback(monitor, now, cropX, cropY, cropWidth, cropHeight);
        } else {
            const int monitorPixelWidth = positiveRoundedIntFromDouble(monitor->m_pixelSize.x);
            const int monitorPixelHeight = positiveRoundedIntFromDouble(monitor->m_pixelSize.y);
            readback = renderMonitorReadback(monitor, now, 0, 0, monitorPixelWidth, monitorPixelHeight);
            readback = normalizeMonitorReadbackToLogicalOrientation(std::move(readback), transform);
            readback = cropReadbackToBounds(readback, PixelBounds{.x = cropX, .y = cropY, .width = cropWidth, .height = cropHeight});
        }
        if (readback.pixels.empty())
            continue;

        const int dstX = clampedIntFromDouble((logicalPart.x - targetGeometry.x) * outputScale);
        const int dstY = clampedIntFromDouble((logicalPart.y - targetGeometry.y) * outputScale);
        const int dstWidth = positiveRoundedIntFromDouble(logicalPart.width * outputScale);
        const int dstHeight = positiveRoundedIntFromDouble(logicalPart.height * outputScale);
        blitScaledRgba(readback, frame.rgba, frame.width, frame.height, dstX, dstY, dstWidth, dstHeight);
    }

    if (frame.rgba.empty())
        return std::nullopt;
    return frame;
}

const RealBackgroundRecordingCache* captureWindowRealBackgroundRecordingFrame(const PHLWINDOW& window,
                                                                              const PHLMONITOR& monitor,
                                                                              const Time::steady_tp& frozenTime,
                                                                              const Rect& targetGeometry) {
    if (!window || !monitor || targetGeometry.width <= 0.0 || targetGeometry.height <= 0.0)
        return nullptr;

    const CBox artifactBox{targetGeometry.x, targetGeometry.y, targetGeometry.width, targetGeometry.height};
    const double scale = monitor->m_scale <= 0.0 ? 1.0 : monitor->m_scale;
    CBox         cropBox = artifactBox.copy().translate(-monitor->m_position).scale(scale).round();
    const int    width = positiveIntFromDouble(cropBox.w);
    const int    height = positiveIntFromDouble(cropBox.h);
    std::size_t  bytes = 0;
    if (!checkedRgbaByteSize(width, height, bytes))
        return nullptr;

    SP<CFramebuffer> framebuffer;
    if (g_realBackgroundRecordingCache && g_realBackgroundRecordingCache->framebuffer)
        framebuffer = g_realBackgroundRecordingCache->framebuffer;

    const auto drmFormat = monitor->m_output && monitor->m_output->state ? monitor->m_output->state->state().drmFormat : DRM_FORMAT_ABGR8888;
    if (!ensureFramebuffer(framebuffer, "hyprcapture-recording-real-background", width, height, drmFormat))
        return nullptr;

    if (!renderRealBackgroundFramebufferForMonitor(monitor, frozenTime, {.window = window, .artifactBox = artifactBox}, *framebuffer))
        return nullptr;

    auto texture = framebuffer->getTexture();
    if (!texture)
        return nullptr;

    g_realBackgroundRecordingCache = RealBackgroundRecordingCache{
        .framebuffer = std::move(framebuffer),
        .texture = std::move(texture),
        .width = width,
        .height = height,
    };
    return &*g_realBackgroundRecordingCache;
}

std::optional<RecordingFrame> captureWindowRecordingFrame(const RecordingFrameRequest& request) {
    ScopedTiming timing("record.capture_window_total");

    const auto window = findWindowByAddress(request.windowAddress);
    if (!window || !shouldCaptureWindow(window))
        return std::nullopt;

    const auto monitor = window->m_monitor.lock();
    if (!monitor)
        return std::nullopt;

    const bool renderDecorations =
        request.defaults.fushionMode || request.defaults.windowBorder == DecorationPolicy::Keep || request.defaults.windowShadow == DecorationPolicy::Keep;

    int  width = 0;
    int  height = 0;
    CBox artifactBox;
    WindowRenderOptions renderOptions;
    renderOptions.asyncReadback = true;
    const bool solidAlpha = request.defaults.recordSolidAlpha &&
        (request.defaults.windowBackground == WindowBackground::White || request.defaults.windowBackground == WindowBackground::Black ||
         request.defaults.windowBackground == WindowBackground::FollowSystem);
    const auto solidBackground = recordingSolidBackgroundColor(request.defaults.windowBackground);
    const auto solidBackgroundRgb = recordingSolidBackgroundRgb(request.defaults.windowBackground);
    const auto frozenTime = Time::steadyNow();
    if (solidBackground && !solidAlpha) {
        renderOptions.clearColor = *solidBackground;
        renderOptions.postprocessAlpha = false;
    } else if (request.defaults.windowBackground == WindowBackground::Real) {
        const CBox fullBox = renderedWindowBox(window, window->getFullWindowBoundingBox());
        const auto captureBackground = [&]() {
            ScopedTiming timing("record.realbg_capture");
            const auto geometry = windowCaptureGeometry(fullBox, monitor);
            return geometry.supported ? captureWindowRealBackgroundRecordingFrame(window, monitor, frozenTime,
                {.x = geometry.x, .y = geometry.y, .width = geometry.width, .height = geometry.height}) : nullptr;
        };
        if (auto background = captureBackground()) {
            renderOptions.backgroundTexture = background->texture;
            renderOptions.clipBackgroundToWindow = true;
            renderOptions.postprocessAlpha = false;
        }
    }
    RgbaReadback readback;
    {
        ScopedTiming timing("record.window_readback");
        readback = renderWindowArtifactReadback(window, monitor, frozenTime, renderDecorations, width, height, artifactBox, nullptr, false, renderOptions);
    }
    if (readback.pixels.empty())
        return std::nullopt;

    const bool cropShadow = request.defaults.windowShadow == DecorationPolicy::Remove;
    if (cropShadow) {
        const CBox visibleBox = renderedWindowGoalMainSurfaceBox(window);
        if (visibleBox.w > 0.0 && visibleBox.h > 0.0 && artifactBox.w > 0.0 && artifactBox.h > 0.0) {
            const double scaleX = static_cast<double>(readback.width) / artifactBox.w;
            const double scaleY = static_cast<double>(readback.height) / artifactBox.h;
            PixelBounds bounds;
            bounds.x = std::max(0, static_cast<int>(std::floor((visibleBox.x - artifactBox.x) * scaleX)));
            bounds.y = std::max(0, static_cast<int>(std::floor((visibleBox.y - artifactBox.y) * scaleY)));
            bounds.width = std::max(1, static_cast<int>(std::ceil(visibleBox.w * scaleX)));
            bounds.height = std::max(1, static_cast<int>(std::ceil(visibleBox.h * scaleY)));
            readback = cropReadbackToBounds(readback, bounds);
            artifactBox = visibleBox;
        }
    } else if ((request.defaults.windowBackground == WindowBackground::Transparent || solidAlpha) && request.defaults.windowShadow == DecorationPolicy::Keep) {
        expandReadbackToShadowBounds(readback, artifactBox, renderedWindowGoalMainSurfaceBox(window), window);
        repairTransparentShadow(readback, artifactBox, renderedWindowGoalMainSurfaceBox(window), window);
    }

    if (readback.pixels.empty())
        return std::nullopt;

    if (request.defaults.windowBackground == WindowBackground::Real) {
        if (!renderOptions.backgroundTexture)
            compositeContentOverSolidBackground(readback, 30, 34, 38);
        if (const auto shadowBackground = recordingSolidBackgroundRgb(WindowBackground::FollowSystem))
            compositeOutsideWindowOverSolidBackground(readback,
                                                      artifactBox,
                                                      renderedWindowGoalMainSurfaceBox(window),
                                                      window,
                                                      (*shadowBackground)[0],
                                                      (*shadowBackground)[1],
                                                      (*shadowBackground)[2]);
    } else if (solidAlpha && solidBackgroundRgb) {
        compositeContentOverSolidBackground(readback, (*solidBackgroundRgb)[0], (*solidBackgroundRgb)[1], (*solidBackgroundRgb)[2]);
    }

    return RecordingFrame{.rgba = std::move(readback.pixels), .width = readback.width, .height = readback.height};
}

} // namespace

CaptureSession captureCompositorArtifacts(const CaptureDefaults& defaults, bool quick) {
    CaptureSession session;
    session.id = makeSessionId();
    session.defaults = defaults;
    if (g_pInputManager) {
        const auto cursor = g_pInputManager->getMouseCoordsInternal();
        if (std::isfinite(cursor.x) && std::isfinite(cursor.y))
            session.cursorPosition = Point{.x = cursor.x, .y = cursor.y};
    }
    const auto root = artifactRoot(session.id);
    if (root.empty())
        return session;
    const auto frozenTime = Time::steadyNow();
    const bool renderWindowImages = defaults.mode == CaptureMode::Window;
    const bool renderDecorations = renderWindowImages && (defaults.windowBorder == DecorationPolicy::Keep || defaults.windowShadow == DecorationPolicy::Keep);
    const bool captureMonitorArtifacts = true;
    // The helper can switch into window mode from the toolbar after launch.
    // Keep hit-test metadata available even when the initial mode is region or
    // fullscreen; only the heavier window image render stays window-mode gated.
    const bool captureWindowMetadata = true;
    const bool captureRealBackgroundArtifacts = renderWindowImages && defaults.windowBackground == WindowBackground::Real;
    ArtifactBudget artifactBudget;

    int monitorIndex = 0;
    for (const auto& monitor : State::monitorState()->monitors()) {
        if (session.monitors.size() >= MAX_SESSION_MONITORS)
            break;
        if (!monitor)
            continue;
        MonitorInfo info;
        info.name = monitor->m_name;
        info.logicalGeometry = monitorRect(monitor);
        info.scale = monitor->m_scale;
        info.transform = static_cast<int>(monitor->m_transform);
        info.focused = monitor == Desktop::focusState()->monitor();
        populateWorkspaceWindowMetadata(monitor, info);
        const int artifactIndex = monitorIndex++;
        const auto path = root / ("monitor-" + std::to_string(artifactIndex) + ".rgba");
        if (captureMonitorArtifacts && renderMonitorArtifact(monitor, frozenTime, path, info.artifactWidth, info.artifactHeight, artifactBudget))
            info.artifactPath = path.string();
        if (defaults.includeCursor && session.cursorPosition && contains(info.logicalGeometry, *session.cursorPosition)) {
            const auto cursorPath = root / ("cursor-" + std::to_string(artifactIndex) + ".rgba");
            if (renderCursorArtifact(monitor,
                                     frozenTime,
                                     *session.cursorPosition,
                                     cursorPath,
                                     info.cursorArtifactWidth,
                                     info.cursorArtifactHeight,
                                     artifactBudget))
                info.cursorArtifactPath = cursorPath.string();
        }
        session.monitors.push_back(std::move(info));
    }

    if (!captureWindowMetadata)
        return session;

    const auto overviewSelectionGeometry = fetchHymissionOverviewSelectionGeometry();
    int z = 0;
    std::vector<PendingRealBackgroundCapture> pendingRealBackgrounds;
    for (const auto& window : windowsInRenderOrder()) {
        if (session.windows.size() >= MAX_SESSION_WINDOWS)
            break;
        const CBox fullBox = renderedWindowBox(window, window->getFullWindowBoundingBox());
        const Rect full = toRect(fullBox);
        auto monitor = window->m_monitor.lock();
        if (!monitor)
            continue;

        const std::string address = "0x" + pointerId(window.get());
        const auto overviewSelectionIt = overviewSelectionGeometry.find(address);

        bool visible = overviewSelectionIt != overviewSelectionGeometry.end();
        for (const auto& mon : session.monitors)
            visible = visible || intersects(full, mon.logicalGeometry);
        if (!visible)
            continue;

        WindowInfo info;
        info.address = address;
        if (overviewSelectionIt != overviewSelectionGeometry.end())
            info.selectionGeometry = overviewSelectionIt->second;
        if (isScrollingTiledWindow(window))
            info.selectionClipGeometry = monitorRect(monitor);
        info.title = boundedString(window->m_title, MAX_WINDOW_METADATA_BYTES);
        info.appClass = boundedString(window->m_class, MAX_WINDOW_METADATA_BYTES);
        info.focused = window == Desktop::focusState()->window();
        info.fullscreen = Fullscreen::controller()->getFullscreenModes(window).internal == Fullscreen::FSMODE_FULLSCREEN;
        info.zIndex = z++;
        const bool dontRound = info.fullscreen;
        info.rounding = dontRound ? 0.0 : std::max(0.0F, window->rounding());
        info.roundingPower = dontRound ? 2.0 : std::clamp(static_cast<double>(window->roundingPower()), 1.0, 10.0);
        info.borderSize = dontRound || window->m_X11DoesntWantBorders ? 0.0 : std::max(0, window->getRealBorderSize());
        const auto path = root / ("window-" + pointerId(window.get()) + ".rgba");
        CBox artifactBox;
        const std::size_t windowIndex = session.windows.size();
        if (renderWindowImages && renderWindowArtifact(window, monitor, frozenTime, renderDecorations, path, info.artifactWidth, info.artifactHeight, artifactBox,
                                                       artifactBudget)) {
            info.artifactPath = path.string();
            info.fullGeometry = toRect(artifactBox);
            if (captureRealBackgroundArtifacts) {
                pendingRealBackgrounds.push_back({
                    .window = window,
                    .monitor = monitor,
                    .artifactBox = artifactBox,
                    .path = root / ("window-real-" + pointerId(window.get()) + ".rgba"),
                    .windowIndex = windowIndex,
                });
            }
        } else
            info.fullGeometry = full;
        info.visibleGeometry = toRect(renderedWindowGoalMainSurfaceBox(window));
        session.windows.push_back(std::move(info));
    }

    for (const auto& monitor : State::monitorState()->monitors()) {
        if (!monitor)
            continue;

        std::vector<PendingRealBackgroundCapture*> monitorRequests;
        for (auto& request : pendingRealBackgrounds) {
            if (request.monitor && request.monitor.get() == monitor.get())
                monitorRequests.push_back(&request);
        }
        renderRealBackgroundArtifactsForMonitor(monitor, frozenTime, monitorRequests, session, artifactBudget);
    }

    return session;
}

LaunchResult captureWindowArtifactFromRequestFile(const std::string& path) {
    const auto requestJson = readPrivateRequestFile(path);
    if (!requestJson)
        return {.success = false, .error = "invalid window capture request file"};

    const auto request = decodeRecordingRequestJson(*requestJson);
    if (!request || request->mode != CaptureMode::Window || request->windowAddress.empty())
        return {.success = false, .error = "invalid window capture metadata"};

    const auto window = findWindowByAddress(request->windowAddress);
    // The overview may close after the UI records this address. A live window
    // can then return to an inactive workspace, but direct framebuffer capture
    // can still render it independently of normal workspace visibility.
    if (!isLiveWindowCaptureTarget(window))
        return {.success = false, .error = "window capture target unavailable"};

    const auto monitor = window->m_monitor.lock();
    if (!monitor)
        return {.success = false, .error = "window capture monitor unavailable"};

    CaptureSession session;
    session.id = makeSessionId();
    session.defaults = request->defaults;
    session.defaults.mode = CaptureMode::Window;

    const auto root = artifactRoot(session.id);
    if (root.empty())
        return {.success = false, .error = "window capture runtime path failed"};

    WindowInfo info;
    info.address = "0x" + pointerId(window.get());
    info.title = boundedString(window->m_title, MAX_WINDOW_METADATA_BYTES);
    info.appClass = boundedString(window->m_class, MAX_WINDOW_METADATA_BYTES);
    info.focused = window == Desktop::focusState()->window();
    info.fullscreen = Fullscreen::controller()->getFullscreenModes(window).internal == Fullscreen::FSMODE_FULLSCREEN;
    info.zIndex = 0;
    const bool dontRound = info.fullscreen;
    info.rounding = dontRound ? 0.0 : std::max(0.0F, window->rounding());
    info.roundingPower = dontRound ? 2.0 : std::clamp(static_cast<double>(window->roundingPower()), 1.0, 10.0);
    info.borderSize = dontRound || window->m_X11DoesntWantBorders ? 0.0 : std::max(0, window->getRealBorderSize());
    info.visibleGeometry = toRect(renderedWindowGoalMainSurfaceBox(window));

    const bool renderDecorations = request->defaults.windowBorder == DecorationPolicy::Keep || request->defaults.windowShadow == DecorationPolicy::Keep;
    const auto artifactPath = root / ("window-" + pointerId(window.get()) + ".rgba");
    CBox       artifactBox;
    ArtifactBudget artifactBudget;
    if (!renderWindowArtifact(window, monitor, Time::steadyNow(), renderDecorations, artifactPath, info.artifactWidth, info.artifactHeight, artifactBox,
                              artifactBudget)) {
        cleanupCompositorArtifacts(session);
        return {.success = false, .error = "window capture render failed"};
    }

    info.artifactPath = artifactPath.string();
    info.fullGeometry = toRect(artifactBox);
    session.windows.push_back(std::move(info));

    const auto responseJson = encodeSessionJson(session);
    if (!writePrivateResponseFile(path, responseJson)) {
        cleanupCompositorArtifacts(session);
        return {.success = false, .error = "window capture response write failed"};
    }

    return {.success = true};
}

LaunchResult captureExportPipeFromRequestFile(const std::string& path) {
    const auto requestJson = readPrivateFifoLine(path);
    if (!requestJson)
        return {.success = false, .error = "invalid export pipe request fifo"};

    std::string responseFifo;
    if (const auto validationError = validateExportPipeRequest(*requestJson, responseFifo); !validationError.empty()) {
        startExportPipeErrorWriter(responseFifo, validationError);
        return responseFifo.empty() ? LaunchResult{.success = false, .error = validationError} : LaunchResult{.success = true};
    }

    std::string error;
    auto payload = captureExportPipePayload(responseFifo, error);
    if (!payload) {
        startExportPipeErrorWriter(responseFifo, error.empty() ? "export pipe capture failed" : error);
        return {.success = true};
    }

    startExportPipeWriter(std::move(*payload));
    return {.success = true};
}

std::optional<RecordingFrame> captureRecordingFrame(const RecordingFrameRequest& request) {
    if (request.mode == CaptureMode::Window)
        return captureWindowRecordingFrame(request);

    if (request.targetGeometry.width <= 0.0 || request.targetGeometry.height <= 0.0)
        return std::nullopt;

    return captureDesktopRegionRecordingFrame(request.targetGeometry);
}

std::optional<WindowStreamCapturedFrame> finalizeReadyWindowStreamFrame(ReadyWindowStreamPboFrame ready,
                                                                          const CaptureDefaults& defaults,
                                                                          const PHLWINDOW& window) {
    ScopedTiming finalizeTiming("window.stream.finalize");
    // This is a transparent window render, not a crop of a monitor screenshot.
    // Keep the render-time border/shadow. Shadow repair is applied only after
    // source/visible-geometry continuity kept this PBO ring valid; it is never
    // applied across a geometry change.
    RgbaReadback repaired{.pixels = std::move(ready.frame.rgba), .width = static_cast<int>(ready.frame.metadata.pixelWidth),
                          .height = static_cast<int>(ready.frame.metadata.pixelHeight)};
    {
        ScopedTiming seamTiming("window.stream.seam_repair");
        repairTopTransparentSeam(repaired);
    }
    {
        ScopedTiming unpremultiplyTiming("window.stream.unpremultiply");
        unpremultiplyAlpha(repaired);
    }
    if (defaults.windowShadow == DecorationPolicy::Keep)
        repairTransparentShadow(repaired, ready.fullBox, ready.visibleBox, window);
    ready.frame.rgba = std::move(repaired.pixels);
    return std::move(ready.frame);
}

// This is deliberately separate from the render tick: the fence probe has a
// zero timeout and the sender handoff is latest-only/non-I/O, so this cannot
// wait for the GPU or submit another fake render on the compositor thread.
// It can therefore deliver a PBO as soon as the event loop gets control back
// from a long render rather than waiting for the next 60 Hz deadline.
void drainReadyWindowStreamPbo(WindowStreamSession& session) {
    auto& stream = g_windowStreamPboReadback;
    if (stream.pending == 0)
        return;

    const auto window = findWindowByAddress(session.windowAddress);
    if (!isLiveWindowCaptureTarget(window)) {
        // Do not expose a completed PBO once its address no longer resolves
        // to a live capture target.  The render tick owns session shutdown.
        resetWindowStreamCapture();
        return;
    }

    if (!g_pHyprOpenGL) {
        resetWindowStreamCapture();
        return;
    }
    g_pHyprOpenGL->makeEGLCurrent();

    GLint previousPackBuffer = 0;
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previousPackBuffer);
    if (auto ready = takeReadyWindowStreamPboFrames(stream)) {
        if (auto frame = finalizeReadyWindowStreamFrame(std::move(*ready), session.defaults, window); frame) {
            if (session.sender)
                (void)session.sender->submit(frame->metadata, std::move(frame->rgba));
        } else
            noteWindowStreamDiagnostic("ready frame postprocess failed");
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(previousPackBuffer));
}

// The callback is intentionally invoked before the next render.  At 60 FPS,
// waiting to hand a signaled PBO to the sender until after another window
// render adds a whole capture pass to the displayed age without buying newer
// pixels.  submit() is a bounded non-I/O handoff.
bool captureWindowGpuStreamFrame(WindowStreamSession& session, const WindowStreamCaptureRequest& request) {
    // There is exactly one exported source allocation.  Do not render into it
    // until its matching HCGR has transitioned the sender back to Ready.
    const auto senderState = session.gpuSender ? session.gpuSender->state() : WindowGpuSenderState::Retired;
    if (senderState == WindowGpuSenderState::Connecting || senderState == WindowGpuSenderState::Busy)
        return true;
    if (senderState != WindowGpuSenderState::Ready)
        return false;

    const auto window = findWindowByAddress(request.windowAddress);
    if (!isLiveWindowCaptureTarget(window))
        return false;
    const auto monitor = window->m_monitor.lock();
    if (!monitor || !g_pHyprOpenGL)
        return false;
    g_pHyprOpenGL->makeEGLCurrent();

    // Freeze every observable attribute before the renderer is entered.  The
    // DMA-BUF header and its shadow snapshot remain tied to this render even
    // if the window moves before the receiver consumes the fence.
    const CBox sampledBox = renderedWindowBox(window, window->getFullWindowBoundingBox());
    const CBox sampledVisibleBox = renderedWindowGoalMainSurfaceBox(window);
    const auto sampledGeometry = windowCaptureGeometry(sampledBox, monitor);
    const int sampledWidth = sampledGeometry.pixelWidth;
    const int sampledHeight = sampledGeometry.pixelHeight;
    const auto extentPlan = planWindowStreamFramebuffer(positiveRoundedIntFromDouble(monitor->m_pixelSize.x), positiveRoundedIntFromDouble(monitor->m_pixelSize.y),
                                                        sampledWidth, sampledHeight, static_cast<int>(monitor->m_transform));
    if (sampledWidth <= 0 || sampledHeight <= 0 || !extentPlan.supported)
        return false;
    const Rect sampledOutputGeometry{.x = sampledGeometry.x, .y = sampledGeometry.y, .width = sampledGeometry.width, .height = sampledGeometry.height};

    // Config and decoration values participate in the frame identity just as
    // much as its geometry.  Snapshot them before renderWindow(), which may
    // re-enter compositor code that changes the live decoration/config state.
    bool             frozenShadowEnabled = false;
    gpuwire::Shadow  frozenShadow;
    if (request.defaults.windowShadow == DecorationPolicy::Keep) {
        RgbaReadback styleSnapshot{.width = sampledWidth, .height = sampledHeight};
        const CBox sampledArtifactBox{sampledOutputGeometry.x, sampledOutputGeometry.y, sampledOutputGeometry.width, sampledOutputGeometry.height};
        if (const auto shadow = shadowRenderGeometry(styleSnapshot, sampledArtifactBox, sampledVisibleBox, window)) {
            const auto color = shadowColorBytes(window);
            frozenShadowEnabled = true;
            frozenShadow = {
                .left = shadow->shadowBox.x,
                .top = shadow->shadowBox.y,
                .width = shadow->shadowBox.w,
                .height = shadow->shadowBox.h,
                .cutoutLeft = shadow->windowCutoutBox.x,
                .cutoutTop = shadow->windowCutoutBox.y,
                .cutoutWidth = shadow->windowCutoutBox.w,
                .cutoutHeight = shadow->windowCutoutBox.h,
                .range = shadow->range,
                .rounding = shadow->rounding,
                .windowRounding = shadow->windowRounding,
                .roundingPower = shadow->roundingPower,
                .power = static_cast<std::uint32_t>(shadow->shadowPower),
                .rgba = {static_cast<std::uint8_t>(color.r), static_cast<std::uint8_t>(color.g), static_cast<std::uint8_t>(color.b), static_cast<std::uint8_t>(color.a)},
                .sharp = shadow->sharp,
            };
        }
    }

    const int framebufferWidth = extentPlan.framebufferWidth;
    const int framebufferHeight = extentPlan.framebufferHeight;
    if (session.gpuFramebuffer && (session.gpuFramebufferWidth != framebufferWidth || session.gpuFramebufferHeight != framebufferHeight)) {
        // Ready is the only state in which replacing the backing allocation is
        // safe.  Busy was handled above; Retired stops the whole session.
        session.gpuExport.reset();
        session.gpuFramebuffer.reset();
        session.gpuFramebufferWidth = 0;
        session.gpuFramebufferHeight = 0;
    }

    WindowRenderOptions renderOptions;
    renderOptions.framebufferOverride = &session.gpuFramebuffer;
    renderOptions.skipReadback = true;
    const bool renderDecorations = request.defaults.fushionMode || request.defaults.windowBorder == DecorationPolicy::Keep ||
        request.defaults.windowShadow == DecorationPolicy::Keep;
    timespec sampledTime{};
    if (clock_gettime(CLOCK_MONOTONIC, &sampledTime) != 0)
        return false;
    const auto captureMonotonicNs = static_cast<std::uint64_t>(sampledTime.tv_sec) * 1'000'000'000ULL + static_cast<std::uint64_t>(sampledTime.tv_nsec);
    int width = 0;
    int height = 0;
    CBox artifactBox;
    const auto renderInfo = renderWindowArtifactReadback(window, monitor, Time::steadyNow(), renderDecorations, width, height, artifactBox, nullptr, false, renderOptions);
    if (width != sampledWidth || height != sampledHeight || artifactBox.x != sampledOutputGeometry.x || artifactBox.y != sampledOutputGeometry.y ||
        artifactBox.w != sampledOutputGeometry.width || artifactBox.h != sampledOutputGeometry.height || !session.gpuFramebuffer ||
        renderInfo.cropX != extentPlan.cropX || renderInfo.cropTopY != extentPlan.cropTopY)
        return false;
    session.gpuFramebufferWidth = framebufferWidth;
    session.gpuFramebufferHeight = framebufferHeight;

    const auto framebuffer = framebufferId(*session.gpuFramebuffer);
    const int imageWidth = positiveRoundedIntFromDouble(session.gpuFramebuffer->m_size.x);
    const int imageHeight = positiveRoundedIntFromDouble(session.gpuFramebuffer->m_size.y);
    if (framebuffer == 0 || imageWidth != framebufferWidth || imageHeight != framebufferHeight || renderInfo.cropTopY > imageHeight - height)
        return false;

    gpuwire::Frame metadata{
        .sequence = request.sequence,
        .captureMonotonicNs = captureMonotonicNs,
        .geometryEpoch = request.geometryEpoch,
        .logicalX = sampledOutputGeometry.x,
        .logicalY = sampledOutputGeometry.y,
        .logicalWidth = sampledOutputGeometry.width,
        .logicalHeight = sampledOutputGeometry.height,
        .imageWidth = static_cast<std::uint32_t>(imageWidth),
        .imageHeight = static_cast<std::uint32_t>(imageHeight),
        .cropX = static_cast<std::uint32_t>(renderInfo.cropX),
        // Keep the same source row interval as readRgbaFramebufferRegion.
        // Hyprland's rendered rows already follow the screenshot path's
        // output order on a normal output; GL coordinates alone do not imply
        // that the pixel content needs another vertical flip.
        .cropY = static_cast<std::uint32_t>(imageHeight - renderInfo.cropTopY - height),
        .cropWidth = static_cast<std::uint32_t>(width),
        .cropHeight = static_cast<std::uint32_t>(height),
        .flipY = false,
    };
    metadata.shadowEnabled = frozenShadowEnabled;
    metadata.shadow = frozenShadow;

    const auto imageBuilds = session.gpuExport.imageBuilds();
    auto packet = [&] {
        ScopedTiming exportTiming("window.gpu.export");
        return session.gpuExport.exportFrame(framebuffer, metadata);
    }();
    if (session.gpuExport.imageBuilds() != imageBuilds)
        traceTiming("window.gpu.image_build");
    if (!packet)
        return false;
    // submit owns the descriptors only on success. A false/Ready result is a
    // permitted try-lock handoff drop: no descriptor reached the peer, so the
    // source allocation has not become externally owned and may be sampled on
    // a later tick. Any other false result is fatal and retires the session.
    const auto releasedUs = session.gpuSender->lastReleaseUs();
    const bool submitted = session.gpuSender->submit(std::move(*packet));
    if (submitted && releasedUs > 0 && timingEnabled()) {
        const auto nowUs = std::chrono::duration_cast<std::chrono::microseconds>(Time::steadyNow().time_since_epoch()).count();
        traceTiming("window.gpu.release_to_submit", nowUs - releasedUs);
    }
    return submitted || session.gpuSender->state() == WindowGpuSenderState::Ready;
}

std::optional<WindowStreamCapturedFrame> captureWindowStreamFrameImpl(const WindowStreamCaptureRequest& request,
                                                                        const std::function<void(WindowStreamCapturedFrame&&)>& consumeReady) {
    if (request.windowAddress.empty() || request.sequence == 0 || request.geometryEpoch == 0) {
        noteWindowStreamDiagnostic("invalid capture request");
        resetWindowStreamCapture();
        return std::nullopt;
    }

    const auto window = findWindowByAddress(request.windowAddress);
    // A stream is an explicit single-window request, not an overview/window
    // enumeration. Its mapped, unhidden target may be on an inactive
    // workspace, where shouldCaptureWindow correctly rejects it for ordinary
    // screenshots but would make this live session produce zero frames.
    if (!isLiveWindowCaptureTarget(window)) {
        noteWindowStreamDiagnostic("capture target unavailable");
        // Do not leak a former client's pixels if an address is re-used.
        resetWindowStreamCapture();
        return std::nullopt;
    }
    const auto monitor = window->m_monitor.lock();
    if (!monitor || !g_pHyprOpenGL) {
        noteWindowStreamDiagnostic("monitor or OpenGL unavailable");
        resetWindowStreamCapture();
        return std::nullopt;
    }
    g_pHyprOpenGL->makeEGLCurrent();

    // Snapshot both geometry and time before invoking Hyprland's render path.
    // A later PBO mapping must only ever expose this immutable snapshot.
    const CBox sampledBox = renderedWindowBox(window, window->getFullWindowBoundingBox());
    const CBox sampledVisibleBox = renderedWindowGoalMainSurfaceBox(window);
    const auto sampledGeometry = windowCaptureGeometry(sampledBox, monitor);
    const int sampledWidth = sampledGeometry.pixelWidth;
    const int sampledHeight = sampledGeometry.pixelHeight;
    const auto extentPlan = planWindowStreamFramebuffer(positiveRoundedIntFromDouble(monitor->m_pixelSize.x), positiveRoundedIntFromDouble(monitor->m_pixelSize.y),
                                                        sampledWidth, sampledHeight, static_cast<int>(monitor->m_transform));
    // The renderer rounds physical crop bounds. Publish the corresponding
    // logical extent, not an unrounded box that could describe different
    // pixels on a fractional-scale output.
    const Rect sampledOutputGeometry{.x = sampledGeometry.x, .y = sampledGeometry.y, .width = sampledGeometry.width, .height = sampledGeometry.height};
    const Rect sampledVisibleGeometry = toRect(sampledVisibleBox);
    const Rect relativeVisibleGeometry{.x = sampledVisibleGeometry.x - sampledOutputGeometry.x,
                                       .y = sampledVisibleGeometry.y - sampledOutputGeometry.y,
                                       .width = sampledVisibleGeometry.width,
                                       .height = sampledVisibleGeometry.height};
    std::size_t bytes = 0;
    if (sampledWidth <= 0 || sampledHeight <= 0 || !checkedRgbaByteSize(sampledWidth, sampledHeight, bytes) ||
        bytes > MAX_RGBA_READBACK_BYTES / WindowStreamPboReadbackState::BUFFER_COUNT) {
        noteWindowStreamDiagnostic("invalid sampled extent");
        return std::nullopt;
    }
    if (!extentPlan.supported) {
        noteWindowStreamDiagnostic("unsupported stream extent");
        resetWindowStreamCapture();
        return std::nullopt;
    }

    auto& stream = g_windowStreamPboReadback;
    if (!sameWindowStreamSource(stream, request.windowAddress, relativeVisibleGeometry, sampledWidth, sampledHeight)) {
        noteWindowStreamDiagnostic("pbo source changed; reset");
        resetWindowStreamPboReadback();
    }
    // Allocate/reset the PBO ring before rendering. A reset releases its
    // framebuffer too, so doing this after render would discard that frame.
    if (!ensureWindowStreamPboReadback(stream, sampledWidth, sampledHeight, bytes)) {
        noteWindowStreamDiagnostic("pbo allocation failed");
        return std::nullopt;
    }

    // Drain before starting another render so a completed frame is delivered
    // without paying the current frame's render time. This uses only a
    // timeout-zero fence test and preserves the caller's pack-buffer binding.
    GLint previousEarlyPackBuffer = 0;
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previousEarlyPackBuffer);
    if (auto ready = takeReadyWindowStreamPboFrames(stream)) {
        if (auto frame = finalizeReadyWindowStreamFrame(std::move(*ready), request.defaults, window); frame)
            consumeReady(std::move(*frame));
        else
            noteWindowStreamDiagnostic("ready frame postprocess failed");
    } else if (stream.pending != 0) {
        noteWindowStreamDiagnostic("pbo fence pending");
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(previousEarlyPackBuffer));

    // Never enqueue a new fake render when every PBO slot is still owned by
    // the GPU.  Rendering anyway cannot produce a frame (issue below would
    // reject it), but it does put more large offscreen work ahead of the
    // fences we are waiting to map.  On a 2674x2514 window that feedback loop
    // made capture timestamps age by seconds.  Returning here preserves the
    // zero-timeout fence policy and lets the first ready slot drain to the
    // sender's latest-only handoff before more GPU work is submitted.
    if (stream.pending >= WindowStreamPboReadbackState::BUFFER_COUNT) {
        noteWindowStreamDiagnostic("pbo ring full; render skipped");
        return std::nullopt;
    }

    int width = 0;
    int height = 0;
    CBox artifactBox;
    WindowRenderOptions renderOptions;
    renderOptions.framebufferOverride = &stream.framebuffer;
    renderOptions.skipReadback = true;
    const bool renderDecorations = request.defaults.fushionMode || request.defaults.windowBorder == DecorationPolicy::Keep ||
        request.defaults.windowShadow == DecorationPolicy::Keep;
    // Sample this frame only after a previous PBO has been drained/finalized
    // and after the full-ring skip decision. The stored metadata remains
    // immutable when this PBO is delivered on a later tick.
    timespec sampledTime {};
    if (clock_gettime(CLOCK_MONOTONIC, &sampledTime) != 0) {
        noteWindowStreamDiagnostic("monotonic clock failed");
        return std::nullopt;
    }
    const auto captureMonotonicNs = static_cast<std::uint64_t>(sampledTime.tv_sec) * 1'000'000'000ULL + static_cast<std::uint64_t>(sampledTime.tv_nsec);
    const auto frozenTime = Time::steadyNow();
    const auto renderInfo = renderWindowArtifactReadback(window, monitor, frozenTime, renderDecorations, width, height, artifactBox, nullptr, false, renderOptions);
    if (width != sampledWidth || height != sampledHeight || artifactBox.x != sampledOutputGeometry.x || artifactBox.y != sampledOutputGeometry.y ||
        artifactBox.w != sampledOutputGeometry.width || artifactBox.h != sampledOutputGeometry.height || !stream.framebuffer ||
        renderInfo.cropX != extentPlan.cropX || renderInfo.cropTopY != extentPlan.cropTopY) {
        noteWindowStreamDiagnostic("render metadata invariant failed");
        resetWindowStreamPboReadback();
        return std::nullopt;
    }

    stream.windowAddress = request.windowAddress;
    stream.relativeVisibleGeometry = relativeVisibleGeometry;

    GLint previousPackBuffer = 0;
    GLint previousPackAlignment = 0;
    GLint previousReadFramebuffer = 0;
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previousPackBuffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &previousPackAlignment);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
    const auto restoreGlReadbackState = [&]() {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(previousPackBuffer));
        glPixelStorei(GL_PACK_ALIGNMENT, previousPackAlignment);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousReadFramebuffer));
    };

    const auto framebuffer = framebufferId(*stream.framebuffer);
    if (framebuffer == 0) {
        noteWindowStreamDiagnostic("stream framebuffer unavailable");
        restoreGlReadbackState();
        return std::nullopt;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    WindowStreamFrameMetadata metadata{
        .sequence = request.sequence,
        .captureMonotonicNs = captureMonotonicNs,
        .geometryEpoch = request.geometryEpoch,
        .logicalX = sampledOutputGeometry.x,
        .logicalY = sampledOutputGeometry.y,
        .logicalWidth = sampledOutputGeometry.width,
        .logicalHeight = sampledOutputGeometry.height,
        .pixelWidth = static_cast<std::uint32_t>(width),
        .pixelHeight = static_cast<std::uint32_t>(height),
        .stride = static_cast<std::uint32_t>(width * static_cast<int>(RGBA_BYTES_PER_PIXEL)),
        .payloadBytes = bytes,
    };
    if (!validWindowStreamFrameMetadata(metadata)) {
        noteWindowStreamDiagnostic("invalid frame metadata");
    } else if (!issueWindowStreamPboReadback(stream, metadata, request.windowAddress, *stream.framebuffer, renderInfo.cropX, renderInfo.cropTopY,
                                               CBox{sampledOutputGeometry.x, sampledOutputGeometry.y, sampledOutputGeometry.width, sampledOutputGeometry.height}, sampledVisibleBox)) {
        noteWindowStreamDiagnostic("pbo issue failed");
    }
    restoreGlReadbackState();
    return std::nullopt;
}

std::optional<WindowStreamCapturedFrame> captureWindowStreamFrame(const WindowStreamCaptureRequest& request) {
    std::optional<WindowStreamCapturedFrame> completed;
    (void)captureWindowStreamFrameImpl(request, [&completed](WindowStreamCapturedFrame&& frame) { completed = std::move(frame); });
    return completed;
}

void resetWindowStreamCapture() {
    if (g_pHyprOpenGL)
        g_pHyprOpenGL->makeEGLCurrent();
    resetWindowStreamPboReadback();
}

namespace {

bool rectEqual(const Rect& left, const Rect& right) {
    return left.x == right.x && left.y == right.y && left.width == right.width && left.height == right.height;
}

void stopWindowStreamSession() {
    auto session = std::move(g_windowStreamSession);
    if (session)
        session->drainPoll.stop();
    if (session && session->timer && g_pEventLoopManager)
        g_pEventLoopManager->removeTimer(session->timer);
    if (session && session->drainTimer && g_pEventLoopManager)
        g_pEventLoopManager->removeTimer(session->drainTimer);
    if (session)
        session->timer.reset();
    if (session)
        session->drainTimer.reset();
    if (session && session->sender)
        session->sender->stop();
    if (session)
        session->gpuNotification.reset();
    if (session && session->gpuSender)
        session->gpuSender.reset();
    if (g_pHyprOpenGL)
        g_pHyprOpenGL->makeEGLCurrent();
    if (session)
        session->gpuExport.reset();
    // A Retired sender can have receiver-owned DMA-BUF imports. Dropping this
    // compositor-side framebuffer is intentional; it is never reused after
    // an uncertain release or disconnect.
    if (session)
        session->gpuFramebuffer.reset();
    resetWindowStreamCapture();
}

void captureWindowStreamDrainTick(SP<CEventLoopTimer> self) {
    ScopedTiming drainTiming("window.stream.drain_tick");
    auto* session = g_windowStreamSession.get();
    if (!session || !session->drainTimer || !session->drainPoll.acceptsCallback(session->drainTimer.get() == self.get()))
        return;

    drainReadyWindowStreamPbo(*session);
    // Poll only while the one PBO is outstanding.  `drainReady...` uses a
    // timeout-zero fence check; this rearm therefore cannot form a GPU wait
    // loop or add work when no readback exists.
    self->updateTimeout(session->drainPoll.armForPending(g_windowStreamPboReadback.pending != 0));
}

void captureWindowStreamTick(SP<CEventLoopTimer> self) {
    ScopedTiming tickTiming("window.stream.tick");
    const auto tickStartedAt = Time::steadyNow();
    auto* session = g_windowStreamSession.get();
    if (!session || !session->timer || session->timer.get() != self.get()) {
        noteWindowStreamDiagnostic("timer stale or session missing");
        return;
    }
    if (session->transport == WindowStreamTransport::Gpu) {
        const auto gpuState = session->gpuSender ? session->gpuSender->state() : WindowGpuSenderState::Retired;
        if (gpuState == WindowGpuSenderState::Retired) {
            noteWindowStreamDiagnostic("GPU sender retired before capture tick");
            notifyUser("GPU window stream stopped: peer release or transport failed", NotificationLevel::Error, 5000);
            stopWindowStreamSession();
            return;
        }
    } else {
        const auto senderState = session->sender ? session->sender->state() : WindowStreamSenderState::Stopped;
        if (senderState == WindowStreamSenderState::Disconnected || senderState == WindowStreamSenderState::Stopped) {
            noteWindowStreamDiagnostic("sender stopped before capture tick");
            stopWindowStreamSession();
            return;
        }
    }
    const auto window = findWindowByAddress(session->windowAddress);
    const auto monitor = window ? window->m_monitor.lock() : PHLMONITOR{};
    if (!window || !monitor) {
        noteWindowStreamDiagnostic("target or monitor missing before capture tick");
        stopWindowStreamSession();
        return;
    }
    if (session->transport == WindowStreamTransport::Gpu) {
        const auto state = session->gpuSender->state();
        if (state == WindowGpuSenderState::Busy || state == WindowGpuSenderState::Connecting) {
            self->updateTimeout(std::nullopt);
            return; // a release/retirement event wakes the main loop
        }
        const auto nowUs = std::chrono::duration_cast<std::chrono::microseconds>(tickStartedAt.time_since_epoch()).count();
        if (nowUs < session->gpuNextDueUs) {
            self->updateTimeout(windowGpuReadyDelay(session->gpuNextDueUs, nowUs));
            return;
        }
    }
    const Rect windowGeometry = toRect(renderedWindowBox(window, window->getFullWindowBoundingBox()));
    const Rect outputGeometry = monitorRect(monitor);
    if (session->sequence != 0 && (!rectEqual(windowGeometry, session->lastWindowGeometry) || !rectEqual(outputGeometry, session->lastMonitorGeometry) ||
                                   monitor->m_scale != session->lastMonitorScale))
        ++session->geometryEpoch;
    session->lastWindowGeometry = windowGeometry;
    session->lastMonitorGeometry = outputGeometry;
    session->lastMonitorScale = monitor->m_scale;

    WindowStreamCaptureRequest request{.defaults = session->defaults, .windowAddress = session->windowAddress, .sequence = ++session->sequence,
                                       .geometryEpoch = session->geometryEpoch};
    if (session->transport == WindowStreamTransport::Gpu) {
        if (!captureWindowGpuStreamFrame(*session, request)) {
            noteWindowStreamDiagnostic("GPU frame export failed");
            notifyUser("GPU window stream stopped: DMA-BUF export is unavailable", NotificationLevel::Error, 5000);
            stopWindowStreamSession();
            return;
        }
    } else {
        // `captureWindowStreamFrameImpl` hands a signaled PBO to this callback
        // before it renders the next sample. The sender's submit path is a
        // try-lock/latest replacement and performs no socket I/O on this thread.
        (void)captureWindowStreamFrameImpl(request, [session](WindowStreamCapturedFrame&& frame) {
            if (session->sender)
                (void)session->sender->submit(frame.metadata, std::move(frame.rgba));
        });
    }

    // Arm the independent early-drain path only after this tick has actually
    // issued a PBO.  Render cadence remains governed solely by `cadence`.
    if (session->transport == WindowStreamTransport::Cpu && g_windowStreamSession.get() == session && session->drainTimer)
        session->drainTimer->updateTimeout(session->drainPoll.armForPending(g_windowStreamPboReadback.pending != 0));

    const auto toMicroseconds = [](const Time::steady_tp& time) {
        return std::chrono::duration_cast<std::chrono::microseconds>(time.time_since_epoch()).count();
    };
    if (session->transport == WindowStreamTransport::Gpu) {
        session->gpuNextDueUs = toMicroseconds(tickStartedAt) + 1'000'000 / std::clamp(session->fps, 1, 1000);
        self->updateTimeout(windowGpuReadyDelay(session->gpuNextDueUs, toMicroseconds(Time::steadyNow())));
    } else {
        self->updateTimeout(scheduleNextWindowStreamTick(session->cadence, toMicroseconds(tickStartedAt), toMicroseconds(Time::steadyNow()), session->fps));
    }
}

int onWindowGpuNotification(int, uint32_t, void* data) {
    auto* session = static_cast<WindowStreamSession*>(data);
    if (g_windowStreamSession.get() != session || !session->gpuSender || !session->timer)
        return 0;
    session->gpuSender->drainNotifications();
    const auto nowUs = std::chrono::duration_cast<std::chrono::microseconds>(Time::steadyNow().time_since_epoch()).count();
    const auto dueUs = session->gpuSender->state() == WindowGpuSenderState::Retired ? 0 : session->gpuNextDueUs;
    session->timer->updateTimeout(windowGpuReadyDelay(dueUs, nowUs));
    return 0;
}

} // namespace

bool isValidWindowStreamStartRequest(const std::string& json) {
    return decodeWindowStreamStartControl(json).has_value();
}

LaunchResult startWindowStreamFromRequestFile(const std::string& path) {
    const auto raw = readPrivateRequestFile(path);
    if (!raw)
        return {.success = false, .error = "invalid window stream request file"};
    const auto control = decodeWindowStreamStartControl(*raw);
    if (!control || !trustedPrivateStreamSocketPath(control->socketPath))
        return {.success = false, .error = "invalid window stream request"};
    if (g_windowStreamSession)
        return {.success = false, .error = "window stream already active"};
    if (!g_pEventLoopManager)
        return {.success = false, .error = "window stream event loop unavailable"};
    if (!isLiveWindowCaptureTarget(findWindowByAddress(control->windowAddress)))
        return {.success = false, .error = "window stream target unavailable"};

    g_windowStreamDiagnosticCounts.clear();
    auto session = std::make_unique<WindowStreamSession>();
    session->id = control->id;
    session->windowAddress = control->windowAddress;
    session->defaults.mode = CaptureMode::Window;
    session->defaults.windowBackground = WindowBackground::Transparent;
    session->defaults.windowBorder = DecorationPolicy::Keep;
    session->defaults.windowShadow = DecorationPolicy::Keep;
    session->fps = control->fps;
    session->transport = control->transport;
    if (control->transport == WindowStreamTransport::Gpu) {
        session->gpuSender = std::make_unique<WindowGpuSender>(control->socketPath);
        if (session->gpuSender->notificationFd() < 0 || !g_pCompositor || !g_pCompositor->m_wlEventLoop)
            return {.success = false, .error = "GPU window stream notification unavailable"};
        session->gpuNotification.value = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, session->gpuSender->notificationFd(),
                                                             WL_EVENT_READABLE, onWindowGpuNotification, session.get());
        if (!session->gpuNotification.value)
            return {.success = false, .error = "GPU window stream notification registration failed"};
        // A Busy GPU stream has no polling timer. Unmap must still schedule
        // target validation without waiting for a peer's release timeout.
        const auto notificationWindow = findWindowByAddress(control->windowAddress);
        if (!notificationWindow)
            return {.success = false, .error = "GPU window stream target disappeared"};
        session->gpuTargetUnmapNotification = notificationWindow->m_events.unmap.listen([target = session.get()] {
            if (target->timer)
                target->timer->updateTimeout(std::chrono::microseconds(1));
        });
    } else {
        session->sender = std::make_unique<WindowStreamSender>(WindowStreamSenderConfig{
            .socketPath = control->socketPath,
            .socketSendBufferBytes = WINDOW_STREAM_SOCKET_SEND_BUFFER_BYTES,
        });
    }
    session->timer = makeShared<CEventLoopTimer>(std::chrono::milliseconds(1), [](SP<CEventLoopTimer> self, void*) { captureWindowStreamTick(self); }, nullptr);
    session->drainTimer = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer> self, void*) { captureWindowStreamDrainTick(self); }, nullptr);
    const auto response = Json{{"ok", true}, {"version", 1}, {"streamId", control->id}, {"socketPath", control->socketPath},
                               {"mode", control->transport == WindowStreamTransport::Gpu ? "window-gpu" : "window"}}.dump();
    if (!writePrivateResponseFile(path, response))
        return {.success = false, .error = "window stream response write failed"};
    g_windowStreamSession = std::move(session);
    g_pEventLoopManager->addTimer(g_windowStreamSession->timer);
    g_pEventLoopManager->addTimer(g_windowStreamSession->drainTimer);
    traceTiming("window.stream.timer_added");
    return {.success = true};
}

LaunchResult stopWindowStreamFromRequestFile(const std::string& path) {
    const auto raw = readPrivateRequestFile(path);
    if (!raw)
        return {.success = false, .error = "invalid window stream stop request file"};
    const auto id = decodeWindowStreamStopControl(*raw);
    if (!id)
        return {.success = false, .error = "invalid window stream stop request"};
    if (!g_windowStreamSession || *id != g_windowStreamSession->id)
        return {.success = false, .error = "window stream not active"};
    stopWindowStreamSession();
    const auto response = Json{{"ok", true}, {"version", 1}, {"streamId", *id}, {"stopped", true}}.dump();
    if (!writePrivateResponseFile(path, response))
        return {.success = false, .error = "window stream stop response write failed"};
    return {.success = true};
}

void resetRecordingCaptureState() {
    if (g_pHyprOpenGL)
        g_pHyprOpenGL->makeEGLCurrent();
    resetAsyncPboReadback(g_windowRecordingPboReadback);
    g_realBackgroundRecordingCache.reset();
}

std::string writeCompositorSessionJsonFile(const CaptureSession& session, std::string_view json) {
    if (json.empty() || json.size() > MAX_SESSION_JSON_BYTES)
        return {};

    const auto root = artifactRoot(session.id);
    if (root.empty())
        return {};

    const auto path = root / "session.json";
    std::vector<unsigned char> bytes(json.begin(), json.end());
    if (!writeRgbaFile(path, bytes))
        return {};

    return path.string();
}

void cleanupCompositorArtifacts(const CaptureSession& session) {
    std::vector<std::filesystem::path> parents;
    const auto removeArtifact = [&](const std::string& rawPath) {
        if (rawPath.empty())
            return;

        const std::filesystem::path path(rawPath);
        std::error_code             ec;
        std::filesystem::remove(path, ec);
        rememberParent(parents, path);
    };

    for (const auto& monitor : session.monitors) {
        removeArtifact(monitor.artifactPath);
        removeArtifact(monitor.cursorArtifactPath);
    }

    for (const auto& window : session.windows) {
        removeArtifact(window.artifactPath);
        removeArtifact(window.realBackgroundPath);
    }

    for (const auto& root : artifactRootCandidates(session.id)) {
        removeArtifact((root / "session.json").string());
        rememberParent(parents, root);
    }

    std::sort(parents.begin(), parents.end(), [](const auto& left, const auto& right) {
        return left.native().size() > right.native().size();
    });

    for (const auto& parent : parents) {
        std::error_code ec;
        std::filesystem::remove(parent, ec);
    }
}

void shutdownArtifactCapture() {
    shutdownExportPipeWriters();
    stopWindowStreamSession();
    if (g_pHyprOpenGL)
        g_pHyprOpenGL->makeEGLCurrent();
    resetAsyncPboReadback(g_windowRecordingPboReadback);
    resetWindowStreamPboReadback();
    g_realBackgroundRecordingCache.reset();
    shutdownRealBackgroundHooks();
}

} // namespace hyprcapture
