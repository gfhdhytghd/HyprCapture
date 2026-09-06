#include "plugin/session_launcher.hpp"

#include "plugin/artifact_capture.hpp"
#include "shared/process_environment.hpp"
#include "shared/protocol.hpp"
#include "shared/trusted_path.hpp"

#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/state/MonitorState.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <poll.h>
#include <spawn.h>
#include <sstream>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace hyprcapture {
namespace {

constexpr std::size_t MAX_INLINE_SESSION_JSON_BYTES = 64 * 1024;
constexpr int EXEC_FAILURE_PIPE_TIMEOUT_MS = 2000;

std::string boolArg(bool value) {
    return value ? "1" : "0";
}

std::string trimLower(std::string value) {
    const auto notSpace = [](unsigned char character) {
        return !std::isspace(character);
    };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string resolvedThumbnailMonitor(const std::string& selector) {
    const std::string normalized = trimLower(selector);
    if (normalized == "primary" || normalized == "all")
        return selector;

    if (normalized != "active" && !normalized.empty()) {
        for (const auto& monitor : State::monitorState()->monitors()) {
            if (monitor && trimLower(monitor->m_name) == normalized)
                return monitor->m_name;
        }
    }

    if (!g_pInputManager)
        return selector;
    const auto cursor = g_pInputManager->getMouseCoordsInternal();
    if (!std::isfinite(cursor.x) || !std::isfinite(cursor.y))
        return selector;
    const auto monitor = State::monitorState()->query().vec(cursor).run();
    return monitor && !monitor->m_name.empty() ? monitor->m_name : selector;
}

std::string defaultInstalledHelperPath() {
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::string(home) + "/.local/bin/hyprcapture-ui";
    return {};
}

std::vector<std::string> helperCandidates(const std::string& configured) {
    std::vector<std::string> candidates;
    if (!configured.empty())
        candidates.push_back(configured);

    if (const char* helperEnv = std::getenv("HYPRCAPTURE_HELPER"); helperEnv && *helperEnv)
        candidates.push_back(helperEnv);

#ifdef HYPRCAPTURE_DEFAULT_HELPER_PATH
    candidates.emplace_back(HYPRCAPTURE_DEFAULT_HELPER_PATH);
#endif

    if (auto installed = defaultInstalledHelperPath(); !installed.empty())
        candidates.push_back(std::move(installed));

    candidates.push_back("/usr/local/bin/hyprcapture-ui");
    candidates.push_back("/usr/bin/hyprcapture-ui");

    std::vector<std::string> unique;
    for (auto& candidate : candidates) {
        if (std::find(unique.begin(), unique.end(), candidate) == unique.end())
            unique.push_back(candidate);
    }
    return unique;
}

std::optional<std::string> firstRunnableHelper(const std::string& configured) {
    for (const auto& candidate : helperCandidates(configured)) {
        if (const auto trusted = security::trustedExecutablePath(candidate))
            return trusted;
    }
    return std::nullopt;
}

std::vector<std::string> trustedBinDirectories() {
    std::vector<std::string> directories;
#ifdef HYPRCAPTURE_TRUSTED_BIN_DIRS
    std::string_view configured(HYPRCAPTURE_TRUSTED_BIN_DIRS);
    while (!configured.empty()) {
        const auto separator = configured.find(':');
        auto       directory = configured.substr(0, separator);
        if (!directory.empty())
            directories.emplace_back(directory);
        if (separator == std::string_view::npos)
            break;
        configured.remove_prefix(separator + 1);
    }
#endif
    if (const char* home = std::getenv("HOME"); home && *home)
        directories.emplace_back(std::filesystem::path(home) / ".nix-profile/bin");
    if (const char* user = std::getenv("USER"); user && *user)
        directories.emplace_back(std::filesystem::path("/etc/profiles/per-user") / user / "bin");
    directories.emplace_back("/run/current-system/sw/bin");
    directories.emplace_back("/nix/var/nix/profiles/default/bin");
    directories.emplace_back("/usr/local/bin");
    directories.emplace_back("/usr/bin");
    directories.emplace_back("/bin");
    return directories;
}

std::string trustedProgramPath(std::string_view name) {
    for (const auto& directory : trustedBinDirectories()) {
        if (const auto trusted = security::trustedExecutablePath((std::filesystem::path(directory) / name).string()))
            return *trusted;
    }
    return {};
}

std::vector<std::string> childEnvironment() {
    std::vector<std::string> env;
    std::string path = "PATH=";
    for (const auto& directory : trustedBinDirectories()) {
        if (path.size() > 5)
            path.push_back(':');
        path += directory;
    }
    env.push_back(std::move(path));
    for (char** item = environ; item && *item; ++item) {
        const std::string entry(*item);
        const auto        separator = entry.find('=');
        if (separator == std::string::npos)
            continue;
        if (desktopEnvironmentNameAllowed(std::string_view(entry).substr(0, separator)))
            env.push_back(entry);
    }
    return env;
}

bool hasCompositorArtifactPaths(const CaptureSession& session) {
    for (const auto& monitor : session.monitors) {
        if (!monitor.artifactPath.empty())
            return true;
    }
    for (const auto& window : session.windows) {
        if (!window.artifactPath.empty() || !window.realBackgroundPath.empty())
            return true;
    }
    return false;
}

bool setCloseOnExec(int fd) {
    const int flags = fcntl(fd, F_GETFD);
    return flags >= 0 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

bool setNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

struct PipeFds {
    int read = -1;
    int write = -1;
};

void closeFd(int& fd) {
    if (fd >= 0)
        close(fd);
    fd = -1;
}

std::optional<PipeFds> makeExecErrorPipe() {
    int fds[2] = {-1, -1};
    if (pipe(fds) != 0)
        return std::nullopt;

    PipeFds pipe{.read = fds[0], .write = fds[1]};
    if (!setCloseOnExec(pipe.read) || !setCloseOnExec(pipe.write) || !setNonBlocking(pipe.read)) {
        closeFd(pipe.read);
        closeFd(pipe.write);
        return std::nullopt;
    }
    return pipe;
}

std::optional<int> readExecFailure(int fd) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(EXEC_FAILURE_PIPE_TIMEOUT_MS);
    auto       remainingTimeout = [&]() -> int {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        return remaining.count() <= 0 ? 0 : static_cast<int>(remaining.count());
    };

    pollfd pfd{.fd = fd, .events = POLLIN | POLLHUP | POLLERR, .revents = 0};
    while (true) {
        const int timeoutMs = remainingTimeout();
        const int ready = poll(&pfd, 1, timeoutMs);
        if (ready > 0)
            break;
        if (ready == 0)
            return std::nullopt;
        if (errno != EINTR)
            return errno;
        if (remainingTimeout() == 0)
            return std::nullopt;
    }

    int         error = 0;
    auto*       data = reinterpret_cast<char*>(&error);
    std::size_t bytes = 0;
    while (bytes < sizeof(error)) {
        const ssize_t chunk = read(fd, data + bytes, sizeof(error) - bytes);
        if (chunk < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return std::nullopt;
            return errno;
        }
        if (chunk == 0)
            return bytes == 0 ? std::nullopt : std::optional<int>{EIO};
        bytes += static_cast<std::size_t>(chunk);
    }
    return error;
}

struct SpawnFileActions {
    posix_spawn_file_actions_t value {};
    bool                       initialized = false;

    ~SpawnFileActions() {
        if (initialized)
            posix_spawn_file_actions_destroy(&value);
    }
};

struct SpawnAttributes {
    posix_spawnattr_t value {};
    bool              initialized = false;

    ~SpawnAttributes() {
        if (initialized)
            posix_spawnattr_destroy(&value);
    }
};

std::optional<int> initSpawnFileActions(SpawnFileActions& actions) {
    const int error = posix_spawn_file_actions_init(&actions.value);
    if (error != 0)
        return error;
    actions.initialized = true;
    return std::nullopt;
}

std::optional<int> initSpawnAttributes(SpawnAttributes& attrs) {
    const int error = posix_spawnattr_init(&attrs.value);
    if (error != 0)
        return error;
    attrs.initialized = true;
    return std::nullopt;
}

std::optional<int> configureSpawnFileDescriptorPolicy(SpawnFileActions& actions, SpawnAttributes& attrs) {
#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
    const int error = posix_spawnattr_setflags(&attrs.value, POSIX_SPAWN_CLOEXEC_DEFAULT);
    if (error != 0)
        return error;
#elif defined(__GLIBC__) && defined(__GLIBC_PREREQ) && __GLIBC_PREREQ(2, 34)
    const int error = posix_spawn_file_actions_addclosefrom_np(&actions.value, 3);
    if (error != 0)
        return error;
#else
    long maxFd = sysconf(_SC_OPEN_MAX);
    if (maxFd < 0)
        maxFd = 1024;

    for (int fd = 3; fd < maxFd; ++fd) {
        const int error = posix_spawn_file_actions_addclose(&actions.value, fd);
        if (error != 0)
            return error;
    }
#endif
    return std::nullopt;
}

} // namespace

std::optional<std::string> recordingHelperPath(const CaptureDefaults& defaults) {
    return firstRunnableHelper(defaults.helper);
}

LaunchResult launchHelper(const LaunchRequest& request) {
    const auto helper = firstRunnableHelper(request.defaults.helper);
    if (!helper)
        return {.success = false, .error = "no trusted hyprcapture-ui helper found"};

    CaptureDefaults captureDefaults = request.defaults;
    captureDefaults.mode = request.requestedMode;
    captureDefaults.thumbnailMonitor = resolvedThumbnailMonitor(captureDefaults.thumbnailMonitor);
    CaptureSession session = captureCompositorArtifacts(captureDefaults, request.quick || request.record || request.recordActive);
    session.defaults.mode = request.requestedMode;

    const auto sessionJson = encodeSessionJson(session);
    const bool sessionHasArtifacts = hasCompositorArtifactPaths(session);
    const auto sessionJsonFile = writeCompositorSessionJsonFile(session, sessionJson);
    const bool useSessionJsonFile = !sessionJsonFile.empty();
    if (!useSessionJsonFile && (sessionHasArtifacts || sessionJson.size() > MAX_INLINE_SESSION_JSON_BYTES)) {
        cleanupCompositorArtifacts(session);
        return {.success = false, .error = "failed to write bounded session metadata"};
    }

    std::vector<std::string> args;
    args.push_back(*helper);
    args.push_back("--mode");
    args.push_back(toString(request.requestedMode));
    args.push_back("--fullscreen-scope");
    args.push_back(toString(request.defaults.fullscreenScope));
    args.push_back("--overlay-scope");
    args.push_back(toString(request.defaults.overlayScope));
    args.push_back("--window-background");
    args.push_back(toString(request.defaults.windowBackground));
    args.push_back("--window-border");
    args.push_back(toString(request.defaults.windowBorder));
    args.push_back("--window-shadow");
    args.push_back(toString(request.defaults.windowShadow));
    args.push_back("--notification-backend");
    args.push_back(toString(request.defaults.notificationBackend));
    args.push_back("--save");
    args.push_back(boolArg(request.defaults.save));
    args.push_back("--clipboard");
    args.push_back(boolArg(request.defaults.clipboard));
    args.push_back("--thumbnail");
    args.push_back(boolArg(request.defaults.showThumbnail));
    args.push_back("--screenshot-notification");
    args.push_back(boolArg(request.defaults.screenshotNotification));
    args.push_back("--include-cursor");
    args.push_back(boolArg(request.defaults.includeCursor));
    args.push_back("--confirm-before-capture");
    args.push_back(boolArg(request.defaults.confirmBeforeCapture));
    args.push_back("--fushion-mode");
    args.push_back(boolArg(request.defaults.fushionMode));
    args.push_back("--capture-fullscreen-clients-as-monitor");
    args.push_back(boolArg(request.defaults.captureFullscreenClientsAsMonitor));
    args.push_back("--dynamic-window-metadata");
    args.push_back(boolArg(request.defaults.dynamicWindowMetadata));
    args.push_back("--window-wheel-scroll");
    args.push_back(boolArg(request.defaults.windowWheelScroll));
    args.push_back("--window-wheel-scope");
    args.push_back(toString(request.defaults.windowWheelScope));
    args.push_back("--fullscreen-preview-rounding");
    args.push_back(request.defaults.fullscreenPreviewRounding);
    args.push_back("--save-dir");
    args.push_back(request.defaults.saveDir);
    args.push_back("--filename-template");
    args.push_back(request.defaults.filenameTemplate);
    args.push_back("--notification-title-template");
    args.push_back(request.defaults.notificationTitleTemplate);
    args.push_back("--notification-body-template");
    args.push_back(request.defaults.notificationBodyTemplate);
    args.push_back("--record-save-dir");
    args.push_back(request.defaults.recordSaveDir);
    args.push_back("--record-filename-template");
    args.push_back(request.defaults.recordFilenameTemplate);
    args.push_back("--record-format");
    args.push_back(request.defaults.recordFormat);
    args.push_back("--record-transparent-format");
    args.push_back(request.defaults.recordTransparentFormat);
    args.insert(args.end(), {"--record-audio", toString(request.defaults.recordAudio),
                             "--record-audio-output", request.defaults.recordAudioOutput,
                             "--record-audio-input", request.defaults.recordAudioInput,
                             "--record-audio-mix", request.defaults.recordAudioMix,
                             "--record-audio-echo-cancellation", request.defaults.recordAudioEchoCancellation ? "1" : "0",
                             "--record-audio-system-gain", std::to_string(request.defaults.recordAudioSystemGain),
                             "--record-audio-mic-gain", std::to_string(request.defaults.recordAudioMicGain)});
    args.push_back("--record-codec");
    args.push_back(request.defaults.recordCodec);
    args.push_back("--record-transparent-codec");
    args.push_back(request.defaults.recordTransparentCodec);
    args.push_back("--record-solid-alpha");
    args.push_back(boolArg(request.defaults.recordSolidAlpha));
    args.push_back("--record-preset");
    args.push_back(request.defaults.recordPreset);
    args.push_back("--record-gsr-flags");
    args.push_back(request.defaults.recordGsrFlags);
    args.push_back("--record-window-backend");
    args.push_back(toString(request.defaults.recordWindowBackend));
    args.push_back("--record-fps");
    args.push_back(std::to_string(request.defaults.recordFps));
    args.push_back("--record-fps-options");
    args.push_back(request.defaults.recordFpsOptions);
    args.push_back("--record-window-fps-limit");
    args.push_back(std::to_string(request.defaults.recordWindowFpsLimit));
    args.push_back("--record-window-real-bg-fps-limit");
    args.push_back(std::to_string(request.defaults.recordWindowRealBgFpsLimit));
    args.push_back("--record-max-seconds");
    args.push_back(std::to_string(request.defaults.recordMaxSeconds));
    args.push_back("--record-countdown-seconds");
    args.push_back(std::to_string(request.defaults.recordCountdownSeconds));
    args.push_back("--thumbnail-timeout-ms");
    args.push_back(std::to_string(request.defaults.thumbnailTimeoutMs));
    args.push_back("--thumbnail-monitor");
    args.push_back(captureDefaults.thumbnailMonitor);
    args.push_back("--watermark");
    args.push_back(request.defaults.watermark);
    args.push_back("--watermark-position");
    args.push_back(toString(request.defaults.watermarkPosition));
    args.push_back("--watermark-width");
    args.push_back(request.defaults.watermarkWidth);
    args.push_back("--watermark-offset");
    args.push_back(request.defaults.watermarkOffset);
    if (request.quick)
        args.push_back("--quick");
    if (request.record)
        args.push_back("--record");
    if (request.recordActive)
        args.push_back("--record-active");
    if (useSessionJsonFile) {
        args.push_back("--session-json-file");
        args.push_back(sessionJsonFile);
    } else {
        args.push_back("--session-json");
        args.push_back(sessionJson);
    }

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& arg : args)
        argv.push_back(arg.data());
    argv.push_back(nullptr);

    auto childEnv = childEnvironment();
    std::vector<char*> envp;
    envp.reserve(childEnv.size() + 1);
    for (auto& env : childEnv)
        envp.push_back(env.data());
    envp.push_back(nullptr);

    auto execErrorPipe = makeExecErrorPipe();
    if (!execErrorPipe) {
        cleanupCompositorArtifacts(session);
        return {.success = false, .error = std::string("pipe failed: ") + std::strerror(errno)};
    }

    SpawnFileActions fileActions;
    if (const auto error = initSpawnFileActions(fileActions)) {
        cleanupCompositorArtifacts(session);
        closeFd(execErrorPipe->read);
        closeFd(execErrorPipe->write);
        return {.success = false, .error = std::string("spawn setup failed: ") + std::strerror(*error)};
    }

    SpawnAttributes attrs;
    if (const auto error = initSpawnAttributes(attrs)) {
        cleanupCompositorArtifacts(session);
        closeFd(execErrorPipe->read);
        closeFd(execErrorPipe->write);
        return {.success = false, .error = std::string("spawn setup failed: ") + std::strerror(*error)};
    }

    if (const auto error = configureSpawnFileDescriptorPolicy(fileActions, attrs)) {
        cleanupCompositorArtifacts(session);
        closeFd(execErrorPipe->read);
        closeFd(execErrorPipe->write);
        return {.success = false, .error = std::string("spawn setup failed: ") + std::strerror(*error)};
    }

    pid_t     pid = -1;
    const int spawnError = posix_spawn(&pid, argv[0], &fileActions.value, &attrs.value, argv.data(), envp.data());
    closeFd(execErrorPipe->write);
    if (spawnError != 0) {
        cleanupCompositorArtifacts(session);
        closeFd(execErrorPipe->read);
        return {.success = false, .error = std::string("exec failed: ") + std::strerror(spawnError)};
    }

    const auto execFailure = readExecFailure(execErrorPipe->read);
    closeFd(execErrorPipe->read);
    if (execFailure) {
        cleanupCompositorArtifacts(session);
        return {.success = false, .error = std::string("exec failed: ") + std::strerror(*execFailure)};
    }

    return {.success = true};
}

LaunchResult launchRecordingResultHelper(const CaptureDefaults& defaults, const std::string& outputPath) {
    if (outputPath.empty())
        return {.success = false, .error = "recording output path missing"};

    const auto helper = firstRunnableHelper(defaults.helper);
    if (!helper)
        return {.success = false, .error = "no trusted hyprcapture-ui helper found"};

    std::vector<std::string> args;
    args.push_back(*helper);
    args.push_back("--recording-result");
    args.push_back(outputPath);
    args.push_back("--clipboard");
    args.push_back(boolArg(defaults.clipboard));
    args.push_back("--thumbnail");
    args.push_back(boolArg(defaults.showThumbnail));
    args.push_back("--record-save-dir");
    args.push_back(defaults.recordSaveDir);
    args.push_back("--thumbnail-timeout-ms");
    args.push_back(std::to_string(defaults.thumbnailTimeoutMs));
    args.push_back("--thumbnail-monitor");
    args.push_back(resolvedThumbnailMonitor(defaults.thumbnailMonitor));

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& arg : args)
        argv.push_back(arg.data());
    argv.push_back(nullptr);

    auto childEnv = childEnvironment();
    std::vector<char*> envp;
    envp.reserve(childEnv.size() + 1);
    for (auto& env : childEnv)
        envp.push_back(env.data());
    envp.push_back(nullptr);

    auto execErrorPipe = makeExecErrorPipe();
    if (!execErrorPipe)
        return {.success = false, .error = std::string("pipe failed: ") + std::strerror(errno)};

    SpawnFileActions fileActions;
    if (const auto error = initSpawnFileActions(fileActions)) {
        closeFd(execErrorPipe->read);
        closeFd(execErrorPipe->write);
        return {.success = false, .error = std::string("spawn setup failed: ") + std::strerror(*error)};
    }

    SpawnAttributes attrs;
    if (const auto error = initSpawnAttributes(attrs)) {
        closeFd(execErrorPipe->read);
        closeFd(execErrorPipe->write);
        return {.success = false, .error = std::string("spawn setup failed: ") + std::strerror(*error)};
    }

    if (const auto error = configureSpawnFileDescriptorPolicy(fileActions, attrs)) {
        closeFd(execErrorPipe->read);
        closeFd(execErrorPipe->write);
        return {.success = false, .error = std::string("spawn setup failed: ") + std::strerror(*error)};
    }

    pid_t     pid = -1;
    const int spawnError = posix_spawn(&pid, argv[0], &fileActions.value, &attrs.value, argv.data(), envp.data());
    closeFd(execErrorPipe->write);
    if (spawnError != 0) {
        closeFd(execErrorPipe->read);
        return {.success = false, .error = std::string("exec failed: ") + std::strerror(spawnError)};
    }

    const auto execFailure = readExecFailure(execErrorPipe->read);
    closeFd(execErrorPipe->read);
    if (execFailure)
        return {.success = false, .error = std::string("exec failed: ") + std::strerror(*execFailure)};

    return {.success = true};
}

LaunchResult launchRecordingTranscodeHelper(const CaptureDefaults& defaults,
                                            const std::string&     inputPath,
                                            const std::string&     outputPath,
                                            bool                  preserveAlpha,
                                            int                   durationMs) {
    if (inputPath.empty() || outputPath.empty())
        return {.success = false, .error = "recording transcode path missing"};

    const auto helper = firstRunnableHelper(defaults.helper);
    if (!helper)
        return {.success = false, .error = "no trusted hyprcapture-ui helper found"};

    std::vector<std::string> args;
    args.push_back(*helper);
    args.push_back("--recording-transcode-input");
    args.push_back(inputPath);
    args.push_back("--recording-transcode-output");
    args.push_back(outputPath);
    args.push_back("--recording-transcode-alpha");
    args.push_back(boolArg(preserveAlpha));
    args.push_back("--recording-transcode-duration-ms");
    args.push_back(std::to_string(std::max(1, durationMs)));
    args.push_back("--clipboard");
    args.push_back(boolArg(defaults.clipboard));
    args.push_back("--thumbnail");
    args.push_back(boolArg(defaults.showThumbnail));
    args.push_back("--record-save-dir");
    args.push_back(defaults.recordSaveDir);
    args.push_back("--thumbnail-timeout-ms");
    args.push_back(std::to_string(defaults.thumbnailTimeoutMs));
    args.push_back("--thumbnail-monitor");
    args.push_back(resolvedThumbnailMonitor(defaults.thumbnailMonitor));

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& arg : args)
        argv.push_back(arg.data());
    argv.push_back(nullptr);

    auto childEnv = childEnvironment();
    std::vector<char*> envp;
    envp.reserve(childEnv.size() + 1);
    for (auto& env : childEnv)
        envp.push_back(env.data());
    envp.push_back(nullptr);

    auto execErrorPipe = makeExecErrorPipe();
    if (!execErrorPipe)
        return {.success = false, .error = std::string("pipe failed: ") + std::strerror(errno)};

    SpawnFileActions fileActions;
    if (const auto error = initSpawnFileActions(fileActions)) {
        closeFd(execErrorPipe->read);
        closeFd(execErrorPipe->write);
        return {.success = false, .error = std::string("spawn setup failed: ") + std::strerror(*error)};
    }

    SpawnAttributes attrs;
    if (const auto error = initSpawnAttributes(attrs)) {
        closeFd(execErrorPipe->read);
        closeFd(execErrorPipe->write);
        return {.success = false, .error = std::string("spawn setup failed: ") + std::strerror(*error)};
    }

    if (const auto error = configureSpawnFileDescriptorPolicy(fileActions, attrs)) {
        closeFd(execErrorPipe->read);
        closeFd(execErrorPipe->write);
        return {.success = false, .error = std::string("spawn setup failed: ") + std::strerror(*error)};
    }

    pid_t     pid = -1;
    const int spawnError = posix_spawn(&pid, argv[0], &fileActions.value, &attrs.value, argv.data(), envp.data());
    closeFd(execErrorPipe->write);
    if (spawnError != 0) {
        closeFd(execErrorPipe->read);
        return {.success = false, .error = std::string("exec failed: ") + std::strerror(spawnError)};
    }

    const auto execFailure = readExecFailure(execErrorPipe->read);
    closeFd(execErrorPipe->read);
    if (execFailure)
        return {.success = false, .error = std::string("exec failed: ") + std::strerror(*execFailure)};

    return {.success = true};
}

LaunchResult launchSystemNotification(const std::string& message, int timeoutMs, bool warning) {
    const auto executable = trustedProgramPath("notify-send");
    if (executable.empty())
        return {.success = false, .error = "notify-send is not installed in a trusted system path"};

    std::vector<std::string> args = {
        executable,
        "--app-name=HyprCapture",
        "--icon=camera-photo",
        "--urgency=" + std::string(warning ? "normal" : "low"),
        "--expire-time=" + std::to_string(std::clamp(timeoutMs, 0, 60 * 60 * 1000)),
        "HyprCapture",
        message,
    };
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& arg : args)
        argv.push_back(arg.data());
    argv.push_back(nullptr);

    auto childEnv = childEnvironment();
    std::vector<char*> envp;
    envp.reserve(childEnv.size() + 1);
    for (auto& env : childEnv)
        envp.push_back(env.data());
    envp.push_back(nullptr);

    SpawnFileActions fileActions;
    if (const auto error = initSpawnFileActions(fileActions))
        return {.success = false, .error = std::string("spawn setup failed: ") + std::strerror(*error)};

    SpawnAttributes attrs;
    if (const auto error = initSpawnAttributes(attrs))
        return {.success = false, .error = std::string("spawn setup failed: ") + std::strerror(*error)};

    if (const auto error = configureSpawnFileDescriptorPolicy(fileActions, attrs))
        return {.success = false, .error = std::string("spawn setup failed: ") + std::strerror(*error)};

    pid_t     pid = -1;
    const int spawnError = posix_spawn(&pid, argv[0], &fileActions.value, &attrs.value, argv.data(), envp.data());
    if (spawnError != 0)
        return {.success = false, .error = std::string("exec failed: ") + std::strerror(spawnError)};
    return {.success = true};
}

} // namespace hyprcapture
