#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace hyprcapture::security {

std::filesystem::path xdgNixProfileBinDirectory();
bool parentChainTrusted(const std::filesystem::path& path, std::filesystem::path immutableBoundary = "/nix/store");
std::optional<std::string> trustedExecutablePath(const std::string& candidate, std::filesystem::path immutableBoundary = "/nix/store");

} // namespace hyprcapture::security
