#pragma once

#include "shared/config.hpp"
#include <algorithm>
#include <vector>
#include <utility>

namespace hyprcapture {
inline constexpr std::size_t kThumbnailColorMaxBytes = 64;

// Validate before argv/JSON construction without allocating an unbounded string.
// Keep supported spellings intact; all accepted results are at most 14 bytes.
inline std::string normalizedThumbnailColor(std::string_view value) {
    if (value.size() > kThumbnailColorMaxBytes)
        return {};
    const auto first = value.find_first_not_of(" \t\r\n\f\v");
    if (first == std::string_view::npos)
        return {};
    value = value.substr(first, value.find_last_not_of(" \t\r\n\f\v") - first + 1);
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
    std::string_view digits;
    if ((value.size() == 7 || value.size() == 9) && value.front() == '#')
        digits = value.substr(1);
    else if (value.size() == 14 && lower(value[0]) == 'r' && lower(value[1]) == 'g' && lower(value[2]) == 'b' && lower(value[3]) == 'a' && value[4] == '(' &&
             value.back() == ')')
        digits = value.substr(5, 8);
    else if (value.size() == 10 && value[0] == '0' && lower(value[1]) == 'x')
        digits = value.substr(2);
    else
        return {};
    for (const char c : digits)
        if (!((c >= '0' && c <= '9') || (lower(c) >= 'a' && lower(c) <= 'f')))
            return {};
    return std::string(value);
}

inline bool validThumbnailPosition(std::string_view position) {
    return position == "top_left" || position == "top_right" || position == "bottom_left" || position == "bottom_right";
}

// Apply again at the UI boundary before narrowing helper-controlled integers.
inline CaptureDefaults normalizedThumbnailDefaults(CaptureDefaults defaults) {
    defaults.thumbnailMaxWidth = std::clamp<std::int64_t>(defaults.thumbnailMaxWidth, 48, 1024);
    defaults.thumbnailMaxHeight = std::clamp<std::int64_t>(defaults.thumbnailMaxHeight, 48, 1024);
    defaults.thumbnailMargin = std::clamp<std::int64_t>(defaults.thumbnailMargin, 0, 512);
    defaults.thumbnailRounding = std::clamp<std::int64_t>(defaults.thumbnailRounding, 0, 128);
    defaults.thumbnailOpacity = std::clamp<std::int64_t>(defaults.thumbnailOpacity, 0, 100);
    defaults.thumbnailBorderSize = std::clamp<std::int64_t>(defaults.thumbnailBorderSize, 0, 16);
    if (!validThumbnailPosition(defaults.thumbnailPosition))
        defaults.thumbnailPosition = "bottom_right";
    defaults.thumbnailBorderColor = normalizedThumbnailColor(defaults.thumbnailBorderColor);
    return defaults;
}

// All helper launch paths share the same explicit --thumbnail-* arguments.
inline std::vector<std::string> thumbnailStyleArguments(const CaptureDefaults &raw) {
    const auto defaults = normalizedThumbnailDefaults(raw);
    return {
        "--thumbnail-max-width",    std::to_string(defaults.thumbnailMaxWidth),
        "--thumbnail-max-height",   std::to_string(defaults.thumbnailMaxHeight),
        "--thumbnail-margin",       std::to_string(defaults.thumbnailMargin),
        "--thumbnail-rounding",     std::to_string(defaults.thumbnailRounding),
        "--thumbnail-opacity",      std::to_string(defaults.thumbnailOpacity),
        "--thumbnail-border-size",  std::to_string(defaults.thumbnailBorderSize),
        "--thumbnail-position",     defaults.thumbnailPosition,
        "--thumbnail-border-color", defaults.thumbnailBorderColor,
        "--thumbnail-animation",    defaults.thumbnailAnimation ? "1" : "0",
    };
}
} // namespace hyprcapture
