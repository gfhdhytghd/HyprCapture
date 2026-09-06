#pragma once
#include "shared/config.hpp"
#include <string>

namespace hyprcapture::audio {
// Resolve Auto only at the point where the actual recording target is known.
// An unselected window stays a window source; never fall back to desktop sound.
inline std::string resolveOutput(const std::string& source, CaptureMode mode, const std::string& windowAddress) {
    if (source != "auto") return source;
    return mode == CaptureMode::Window ? "window:" + windowAddress : "default";
}
}
