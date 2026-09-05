#pragma once

#include <vector>

namespace hyprcapture {

struct RgbaFrame {
    std::vector<unsigned char> pixels;
    int                        width = 0;
    int                        height = 0;
};

struct RgbaCrop {
    int x = 0, y = 0, width = 0, height = 0;
};

// Map a logical, top-down crop into the physical framebuffer before readback.
RgbaCrop logicalRgbaCropToFramebuffer(RgbaCrop crop, int framebufferWidth, int framebufferHeight, int transform);

bool      rgbaFrameHasExpectedSize(const RgbaFrame& frame);
RgbaFrame normalizeRgbaFrameToLogicalOrientation(RgbaFrame frame, int transform);
// Converts premultiplied RGB channels in-place using the exact integer
// rounding/clamping contract used by compositor artifact capture.
void      unpremultiplyRgbaPixels(std::vector<unsigned char>& pixels);

} // namespace hyprcapture
