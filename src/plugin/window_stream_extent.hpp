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

// Plan physical stream bounds without any Hyprland/GL dependency. A rotated
// output is retained on the established monitor-FBO path only while the full
// window fits; an oversized rotated target is explicitly unsupported because
// the current fake-render transform cannot prove a complete local viewport.
inline WindowStreamFramebufferPlan planWindowStreamFramebuffer(int monitorWidth, int monitorHeight, int windowWidth, int windowHeight, int monitorTransform) {
    if (monitorWidth <= 0 || monitorHeight <= 0 || windowWidth <= 0 || windowHeight <= 0)
        return {};
    const bool oversized = windowWidth > monitorWidth || windowHeight > monitorHeight;
    if (oversized && monitorTransform != 0)
        return {};
    if (oversized)
        return {.framebufferWidth = windowWidth, .framebufferHeight = windowHeight, .cropX = 0, .cropTopY = 0, .dedicatedFramebuffer = true, .supported = true};
    return {.framebufferWidth = monitorWidth,
            .framebufferHeight = monitorHeight,
            .cropX = (monitorWidth - windowWidth) / 2,
            .cropTopY = (monitorHeight - windowHeight) / 2,
            .dedicatedFramebuffer = false,
            .supported = true};
}

} // namespace hyprcapture
