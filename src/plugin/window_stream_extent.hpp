#pragma once

#include <algorithm>

namespace hyprcapture {

struct WindowStreamFramebufferPlan {
    int  framebufferWidth = 0;
    int  framebufferHeight = 0;
    int  cropX = 0;
    int  cropTopY = 0;
    bool dedicatedFramebuffer = false;
    bool supported = false;
};

// Compatibility adapter for stream consumers. The common window renderer
// uses an upright, window-sized export projection on every output transform.
inline WindowStreamFramebufferPlan planWindowStreamFramebuffer(int monitorWidth, int monitorHeight, int windowWidth, int windowHeight, int monitorTransform) {
    if (monitorWidth <= 0 || monitorHeight <= 0 || windowWidth <= 0 || windowHeight <= 0 || monitorTransform < 0 || monitorTransform > 7)
        return {};
    return {.framebufferWidth = windowWidth, .framebufferHeight = windowHeight,
            .cropX = 0, .cropTopY = 0, .dedicatedFramebuffer = true, .supported = true};
}

} // namespace hyprcapture
