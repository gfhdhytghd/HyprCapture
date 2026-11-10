#pragma once

#include "shared/config.hpp"
#include <algorithm>
#include <vector>
#include <utility>

namespace hyprcapture {
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
        "--thumbnail-background",   defaults.thumbnailBackground,
        "--thumbnail-foreground",   defaults.thumbnailForeground,
        "--thumbnail-accent",       defaults.thumbnailAccent,
        "--thumbnail-animation",    defaults.thumbnailAnimation ? "1" : "0",
    };
}
} // namespace hyprcapture
