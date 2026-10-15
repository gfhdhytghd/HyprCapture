#include "plugin/timing.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <memory>
#include <utility>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace hyprcapture {
namespace {

std::mutex g_timingMutex;
struct TimingConfig {
    bool enabled = false;
    std::string file;
};

// Readers never reach into the compositor configuration. Publish both values
// together so an encoder worker sees an immutable, consistent snapshot.
std::atomic<std::shared_ptr<const TimingConfig>> g_timingConfig{std::make_shared<const TimingConfig>()};

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

} // namespace

void refreshTimingConfig(bool enabled, std::string file) {
    if (file.empty()) {
        if (const char* env = std::getenv("HYPRCAPTURE_TIMING_FILE"); env && *env)
            file = env;
    }
    enabled = enabled || std::getenv("HYPRCAPTURE_TIMING") || std::getenv("HYPRCAPTURE_TIMING_FILE");
    g_timingConfig.store(std::make_shared<const TimingConfig>(TimingConfig{enabled, std::move(file)}), std::memory_order_release);
}

bool timingEnabled() {
    return g_timingConfig.load(std::memory_order_acquire)->enabled;
}

void traceTiming(std::string_view event, long long elapsedUs) {
    const auto config = g_timingConfig.load(std::memory_order_acquire);
    if (!config->enabled)
        return;

    const auto now = std::chrono::system_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    std::string line = "time_ms=" + std::to_string(ms) + " event=" + std::string(event);
    line += " monotonic_us=" + std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    if (elapsedUs >= 0)
        line += " elapsed_us=" + std::to_string(elapsedUs);
    line += "\n";

    std::lock_guard lock(g_timingMutex);
    const auto&     path = config->file;
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
