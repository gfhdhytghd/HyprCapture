#include "shared/trusted_path.hpp"

#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace hyprcapture::security {
namespace {

bool isTrustedOwner(uid_t uid) {
    return uid == 0 || uid == geteuid();
}

bool hasWritableGroupOrOther(mode_t mode) {
    return (mode & 0022) != 0;
}

} // namespace

bool parentChainTrusted(const std::filesystem::path& path, std::filesystem::path immutableBoundary) {
    immutableBoundary = immutableBoundary.lexically_normal();

    for (auto current = path.parent_path(); !current.empty(); current = current.parent_path()) {
        current = current.lexically_normal();
        // Multi-user Nix stores are commonly group-writable at /nix/store,
        // while their realized children remain immutable and root-owned.
        if (!immutableBoundary.empty() && current == immutableBoundary)
            return true;

        struct stat st {};
        const auto  native = current.string();
        if (stat(native.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || !isTrustedOwner(st.st_uid) || hasWritableGroupOrOther(st.st_mode))
            return false;
        if (current == current.root_path())
            break;
    }
    return true;
}

std::optional<std::string> trustedExecutablePath(const std::string& candidate, std::filesystem::path immutableBoundary) {
    if (candidate.empty())
        return std::nullopt;

    std::error_code ec;
    auto            path = std::filesystem::path(candidate);
    if (!path.is_absolute())
        return std::nullopt;

    path = std::filesystem::weakly_canonical(path, ec);
    if (ec || !path.is_absolute() || !parentChainTrusted(path, std::move(immutableBoundary)))
        return std::nullopt;

    const auto native = path.string();
    struct stat st {};
    if (stat(native.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || !isTrustedOwner(st.st_uid) || hasWritableGroupOrOther(st.st_mode))
        return std::nullopt;
    if (access(native.c_str(), X_OK) != 0)
        return std::nullopt;
    return native;
}

} // namespace hyprcapture::security
