#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace hyprcapture {

inline std::vector<std::string> recordingCodecCandidates(std::string_view requested, int width, int height) {
    std::string codec(requested);
    std::ranges::transform(codec, codec.begin(), [](unsigned char ch) {
        return ch == '_' || ch == '.' ? '-' : static_cast<char>(std::tolower(ch));
    });
    if (codec.empty() || codec == "auto") {
        // Prefer HEVC above NVENC H.264's 4096-pixel dimension limit, but
        // still probe every hardware candidate at the actual canvas size.
        // If neither family works in hardware, retain the software fallback.
        if (width > 4096 || height > 4096)
            return {"hevc_nvenc", "hevc_vaapi", "h264_nvenc", "h264_vaapi", "libx264"};
        return {"h264_nvenc", "h264_vaapi", "hevc_nvenc", "hevc_vaapi", "libx264"};
    }
    // An explicitly selected family must remain that family.
    if (codec == "h264") return {"h264_nvenc", "h264_vaapi", "libx264"};
    if (codec == "h265" || codec == "hevc") return {"hevc_nvenc", "hevc_vaapi", "libx265"};
    if (codec == "av1") return {"av1_nvenc", "av1_vaapi", "libsvtav1"};
    if (codec == "vp9") return {"vp9_vaapi", "libvpx-vp9"};
    return {std::string(requested)};
}

template<class Probe>
std::string selectRecordingCodec(std::string_view requested, int width, int height, Probe&& probe) {
    const auto candidates = recordingCodecCandidates(requested, width, height);
    for (const auto& candidate : candidates) {
        if (probe(candidate, width, height)) return candidate;
    }
    return candidates.back();
}

} // namespace hyprcapture
