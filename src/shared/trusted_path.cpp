#include "shared/trusted_path.hpp"

#include <fstream>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace hyprcapture::security {
namespace {

bool isUnmappedOwner(uid_t uid) {
    // Do not trust the literal uid 65534: it can be a real account outside a
    // user namespace, and the kernel's overflow uid is configurable.
    unsigned long long overflow;
    std::ifstream overflowFile("/proc/sys/kernel/overflowuid");
    if (!(overflowFile >> overflow) || uid != overflow)
        return false;
    std::ifstream mappings("/proc/self/uid_map");
    unsigned long long inside, outside, count;
    bool sawMapping = false;
    while (mappings >> inside >> outside >> count) {
        sawMapping = true;
        if (uid >= inside && uid - inside < count)
            return false;
    }
    return sawMapping && mappings.eof();
}

bool isTrustedOwner(const struct stat& st, const std::filesystem::path& path, const std::filesystem::path& immutableBoundary) {
    const auto uid = st.st_uid;
    if (uid == 0 || uid == geteuid())
        return true;

    // A Nix build user namespace leaves the store owner's uid unmapped.
    // Only accept that ownership below the immutable store boundary, with
    // read-only permissions or a read-only mount. Nix inputs can be 0555 on
    // writable bind mounts. Unknown owners elsewhere are still untrusted.
    const auto relative = path.lexically_relative(immutableBoundary);
    if (!immutableBoundary.is_absolute() || relative.empty() || relative == "." || *relative.begin() == "..")
        return false;
    if (!isUnmappedOwner(uid))
        return false;
    if (!(st.st_mode & 0222))
        return true;
    struct statvfs fs {};
    return statvfs(path.c_str(), &fs) == 0 && (fs.f_flag & ST_RDONLY);
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
        if (stat(native.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || !isTrustedOwner(st, current, immutableBoundary) || hasWritableGroupOrOther(st.st_mode))
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
    immutableBoundary = immutableBoundary.lexically_normal();
    if (ec || !path.is_absolute() || !parentChainTrusted(path, immutableBoundary))
        return std::nullopt;

    const auto native = path.string();
    struct stat st {};
    if (stat(native.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || !isTrustedOwner(st, path, immutableBoundary) || hasWritableGroupOrOther(st.st_mode))
        return std::nullopt;
    if (access(native.c_str(), X_OK) != 0)
        return std::nullopt;
    return native;
}

} // namespace hyprcapture::security
