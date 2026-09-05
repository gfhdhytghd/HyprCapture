#include "plugin/window_stream_extent.hpp"

#include <cassert>

int main() {
    const auto normal = hyprcapture::planWindowStreamFramebuffer(1920, 1080, 1280, 720, 0);
    assert(normal.supported && !normal.dedicatedFramebuffer && normal.cropX == 320 && normal.cropTopY == 180);
    const auto crossEdge = hyprcapture::planWindowStreamFramebuffer(1920, 1080, 2400, 720, 0);
    assert(crossEdge.supported && crossEdge.dedicatedFramebuffer && crossEdge.framebufferWidth == 2400 && crossEdge.framebufferHeight == 720 && crossEdge.cropX == 0);
    const auto fractionalScale = hyprcapture::planWindowStreamFramebuffer(2560, 1440, 2561, 901, 0);
    assert(fractionalScale.supported && fractionalScale.dedicatedFramebuffer && fractionalScale.framebufferWidth == 2561 && fractionalScale.framebufferHeight == 901);
    const auto rotatedFits = hyprcapture::planWindowStreamFramebuffer(1080, 1920, 1000, 800, 1);
    assert(rotatedFits.supported && !rotatedFits.dedicatedFramebuffer);
    const auto rotatedOversized = hyprcapture::planWindowStreamFramebuffer(1080, 1920, 1200, 800, 1);
    assert(!rotatedOversized.supported);
}
