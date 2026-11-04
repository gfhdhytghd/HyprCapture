#pragma once

#include <cmath>
#include <initializer_list>
#include <optional>

namespace hyprcapture {

struct RecordingCursorBox {
    double x, y, width, height;

    bool contains(double px, double py) const {
        return px >= x && py >= y && px < x + width && py < y + height;
    }
};

// All inputs are global logical coordinates. The target is the upright
// capture geometry, so output rotation/reflection is applied by the render
// projection (and monitor readback normalization), never a second time here.
inline std::optional<RecordingCursorBox> planRecordingCursor(
    double pointerX, double pointerY, double hotspotX, double hotspotY,
    double cursorWidth, double cursorHeight, double targetX, double targetY,
    double targetWidth, double targetHeight, double scale, bool requirePointerInside) {
    for (double value : {pointerX, pointerY, hotspotX, hotspotY, cursorWidth, cursorHeight,
                         targetX, targetY, targetWidth, targetHeight, scale})
        if (!std::isfinite(value)) return std::nullopt;
    if (cursorWidth <= 0 || cursorHeight <= 0 || targetWidth <= 0 || targetHeight <= 0 || scale <= 0)
        return std::nullopt;
    const double localX = pointerX - targetX, localY = pointerY - targetY;
    if (requirePointerInside && (localX < 0 || localY < 0 || localX >= targetWidth || localY >= targetHeight))
        return std::nullopt;
    const double left = localX - hotspotX, top = localY - hotspotY;
    if (left >= targetWidth || top >= targetHeight || left + cursorWidth <= 0 || top + cursorHeight <= 0)
        return std::nullopt;
    // Match PointerManager's pixel snapping, including fractional output scale.
    RecordingCursorBox box{std::round(left * scale), std::round(top * scale),
                           std::round(cursorWidth * scale), std::round(cursorHeight * scale)};
    if (!std::isfinite(box.x) || !std::isfinite(box.y) || !std::isfinite(box.width) || !std::isfinite(box.height))
        return std::nullopt;
    return box;
}

} // namespace hyprcapture
