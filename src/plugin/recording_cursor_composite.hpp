#pragma once

#include "plugin/recording_cursor.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <vector>

namespace hyprcapture {

// The small cursor image travels with its window's PBO slot. Coordinates are
// local to that readback and follow the same crop/padding adjustments.
struct RecordingCursorImage {
    RecordingCursorBox box;
    std::shared_ptr<const std::vector<unsigned char>> pixels;
};

// Both inputs are straight-alpha SDR RGBA, as used by recording frame assembly.
// Preserve transparent texels exactly; blend translucent edges with the repaired
// background instead of treating the whole cursor rectangle as opaque.
inline void compositeRecordingCursor(std::span<unsigned char> destination, int width, int height, const RecordingCursorImage& cursor) {
    const auto& box = cursor.box;
    for (double value : {box.x, box.y, box.width, box.height})
        if (!std::isfinite(value) || value != std::round(value) || std::abs(value) > std::numeric_limits<int>::max()) return;
    if (width <= 0 || height <= 0 || box.width <= 0 || box.height <= 0 || !cursor.pixels)
        return;
    const auto cursorWidth = static_cast<std::int64_t>(box.width), cursorHeight = static_cast<std::int64_t>(box.height);
    if (static_cast<std::uint64_t>(width) * height * 4 != destination.size() ||
        static_cast<std::uint64_t>(cursorWidth) * cursorHeight * 4 != cursor.pixels->size())
        return;
    const auto offsetX = static_cast<std::int64_t>(box.x), offsetY = static_cast<std::int64_t>(box.y);
    const auto left = std::max<std::int64_t>(0, offsetX), top = std::max<std::int64_t>(0, offsetY);
    const auto right = std::min<std::int64_t>(width, offsetX + cursorWidth), bottom = std::min<std::int64_t>(height, offsetY + cursorHeight);
    for (auto y = top; y < bottom; ++y) {
        for (auto x = left; x < right; ++x) {
            const auto* src = cursor.pixels->data() + ((y - offsetY) * cursorWidth + x - offsetX) * 4;
            auto* dst = destination.data() + (y * width + x) * 4;
            const unsigned sa = src[3];
            if (sa == 0)
                continue;
            const unsigned da = dst[3], inverse = 255 - sa;
            const unsigned alpha = sa * 255 + da * inverse;
            for (int channel = 0; channel < 3; ++channel)
                dst[channel] = static_cast<unsigned char>((src[channel] * sa * 255 + dst[channel] * da * inverse + alpha / 2) / alpha);
            dst[3] = static_cast<unsigned char>((alpha + 127) / 255);
        }
    }
}

} // namespace hyprcapture
