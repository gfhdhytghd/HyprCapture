#pragma once

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace hyprcapture {

template <typename Manager>
inline bool sessionLocked(Manager* manager) {
    return manager && manager->isSessionLocked();
}

inline std::optional<std::string> unlockedSessionGeneration(std::string_view state, std::string_view expected = {}) {
    if (state.size() > 64 || !state.starts_with("unlocked:")) return std::nullopt;
    state.remove_prefix(9);
    if (state.empty() || state.find('\0') != std::string_view::npos || (!expected.empty() && state != expected)) return std::nullopt;
    return std::string(state);
}

// Separate compositor instances must not share helper lock state.
inline std::string sessionLockSocketPath() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    const char* instance = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!runtime || runtime[0] != '/' || !instance || !*instance || std::string(instance).find('/') != std::string::npos)
        return {};
    const std::string directory = std::string(runtime) + "/hypr/" + instance;
    struct stat st{};
    if (lstat(directory.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0022))
        return {};
    return directory + "/hyprcapture-lock.sock";
}

} // namespace hyprcapture
