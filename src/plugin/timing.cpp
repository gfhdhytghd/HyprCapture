#include "plugin/timing.hpp"

#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/shared/Types.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <typeindex>
#include <unistd.h>

extern HANDLE g_pluginHandle;

namespace hyprcapture {
namespace {

std::mutex g_timingMutex;
std::atomic<bool> g_cachedTimingEnabled = false;
std::atomic<long long> g_nextTimingRefreshNs = 0;

std::string configName(const std::string& suffix) {
    return "plugin:hyprcapture:" + suffix;
}

const Config::SConfigOptionReply configOption(const std::string& name) {
    if (!g_pluginHandle)
        return {};
    return Config::mgr()->getConfigValue(name);
}

bool configBool(const std::string& suffix, bool fallback) {
    const auto value = configOption(configName(suffix));
    if (!value.dataptr || !value.type)
        return fallback;

    const auto type = std::type_index(*value.type);
    if (type == typeid(Config::BOOL))
        return **reinterpret_cast<Config::BOOL* const*>(value.dataptr);

    if (type == typeid(Config::INTEGER))
        return **reinterpret_cast<Config::INTEGER* const*>(value.dataptr) != 0;

    return fallback;
}

std::string configString(const std::string& suffix) {
    const auto value = configOption(configName(suffix));
    if (!value.dataptr || !value.type)
        return {};

    const auto type = std::type_index(*value.type);
    if (type == typeid(Config::STRING))
        return **reinterpret_cast<Config::STRING* const*>(value.dataptr);

    if (type == typeid(Hyprlang::STRING)) {
        const auto raw = *reinterpret_cast<Hyprlang::STRING const*>(value.dataptr);
        return raw ? std::string(raw) : std::string{};
    }

    return {};
}

bool runtimeRootMatches(const std::filesystem::path& root, const std::filesystem::path& path) {
    std::error_code ec;
    const auto      canonicalRoot = std::filesystem::weakly_canonical(root, ec);
    if (ec)
        return false;
    const auto canonicalPath = std::filesystem::weakly_canonical(path, ec);
    if (ec)
        return false;

    struct stat st {};
    const auto  rootNative = canonicalRoot.string();
    if (stat(rootNative.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0777) != 0700)
        return false;

    const auto rootString = canonicalRoot.native();
    const auto pathString = canonicalPath.native();
    return pathString == rootString || pathString.starts_with(rootString + "/");
}

bool privateTimingPath(const std::filesystem::path& path) {
    if (path.empty() || !path.is_absolute())
        return false;

    const auto rootName = "hyprcapture-" + std::to_string(static_cast<unsigned long long>(geteuid()));
    return runtimeRootMatches(std::filesystem::path{"/dev/shm"} / rootName, path) ||
        runtimeRootMatches(std::filesystem::temp_directory_path() / rootName, path);
}

std::string timingFilePath() {
    if (const auto configured = configString("timing_file"); !configured.empty())
        return configured;
    if (const char* env = std::getenv("HYPRCAPTURE_TIMING_FILE"); env && *env)
        return env;
    return {};
}

} // namespace

bool timingEnabled() {
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now < g_nextTimingRefreshNs.load(std::memory_order_relaxed))
        return g_cachedTimingEnabled.load(std::memory_order_relaxed);

    std::lock_guard lock(g_timingMutex);
    if (now < g_nextTimingRefreshNs.load(std::memory_order_relaxed))
        return g_cachedTimingEnabled.load(std::memory_order_relaxed);

    const bool enabled = configBool("timing", false) || std::getenv("HYPRCAPTURE_TIMING") || std::getenv("HYPRCAPTURE_TIMING_FILE");
    g_cachedTimingEnabled.store(enabled, std::memory_order_relaxed);
    g_nextTimingRefreshNs.store(now + std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::milliseconds(250)).count(), std::memory_order_relaxed);
    return enabled;
}

void traceTiming(std::string_view event, long long elapsedUs) {
    if (!timingEnabled())
        return;

    const auto now = std::chrono::system_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    std::string line = "time_ms=" + std::to_string(ms) + " event=" + std::string(event);
    line += " monotonic_us=" + std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    if (elapsedUs >= 0)
        line += " elapsed_us=" + std::to_string(elapsedUs);
    line += "\n";

    std::lock_guard lock(g_timingMutex);
    const auto      path = timingFilePath();
    if (!path.empty() && privateTimingPath(path)) {
        std::ofstream out(path, std::ios::app);
        if (out) {
            out << line;
            return;
        }
    }

    std::cerr << "[hyprcapture-timing] " << line;
}

ScopedTiming::ScopedTiming(std::string_view event) : m_event(event), m_started(std::chrono::steady_clock::now()), m_enabled(timingEnabled()) {}

ScopedTiming::~ScopedTiming() {
    if (!m_enabled)
        return;

    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - m_started).count();
    traceTiming(m_event, elapsed);
}

} // namespace hyprcapture
