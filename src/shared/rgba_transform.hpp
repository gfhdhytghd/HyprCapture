#pragma once

#include <vector>

namespace hyprcapture {

struct RgbaFrame {
    std::vector<unsigned char> pixels;
    int                        width = 0;
    int                        height = 0;
};

bool      rgbaFrameHasExpectedSize(const RgbaFrame& frame);
RgbaFrame normalizeRgbaFrameToLogicalOrientation(RgbaFrame frame, int transform);

} // namespace hyprcapture
