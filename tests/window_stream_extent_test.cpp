#include "plugin/window_stream_extent.hpp"
#include "plugin/window_capture_geometry.hpp"

#include <cassert>
#include <cmath>
#include <limits>

int main() {
    using namespace hyprcapture;
    for (int transform = 0; transform < 8; ++transform) {
        for (auto size : {128, 1280, 2400}) {
            const auto plan = planWindowStreamFramebuffer(1920, 1080, size, 720, transform);
            assert(plan.supported && plan.dedicatedFramebuffer);
            assert(plan.framebufferWidth == size && plan.framebufferHeight == 720);
            assert(plan.cropX == 0 && plan.cropTopY == 0);
        }
        // Body plus asymmetric border/shadow padding, partly off a negative
        // output. Capture bounds must contain every edge on each pixel grid.
        for (double scale : {1.0, 1.25, 1.5, 2.0, 2.4}) {
            constexpr double x = -2060.3, y = -11.7, width = 1511.2, height = 833.6;
            const auto geometry = planWindowCaptureGeometry(x, y, width, height, -1920, -100, scale, transform);
            assert(geometry.supported);
            assert(geometry.x <= x + 1e-9 && geometry.y <= y + 1e-9);
            assert(geometry.x + geometry.width >= x + width - 1e-9);
            assert(geometry.y + geometry.height >= y + height - 1e-9);
            assert(x - geometry.x < 1 / scale + 1e-9 && y - geometry.y < 1 / scale + 1e-9);
            assert(geometry.width - width < 2 / scale + 1e-9);
            assert(std::abs(geometry.width * scale - geometry.pixelWidth) < 1e-9);
            assert(std::abs(geometry.height * scale - geometry.pixelHeight) < 1e-9);
            // Metadata and local viewport map the same content point back to
            // the original global position; no transform is applied twice.
            const double localX = (x + 25 - geometry.x) * scale;
            const double localY = (y + 13 - geometry.y) * scale;
            assert(std::abs(geometry.x + localX / scale - (x + 25)) < 1e-9);
            assert(std::abs(geometry.y + localY / scale - (y + 13)) < 1e-9);
            const auto upright = planWindowCaptureGeometry(x, y, width, height, -1920, -100, scale, 0);
            assert(upright.pixelWidth == geometry.pixelWidth && upright.pixelHeight == geometry.pixelHeight);
        }
    }
    auto aligned = planWindowCaptureGeometry(100, 200, 1280, 720, 0, 0, 1.25, 0);
    assert(aligned.supported && aligned.x == 100 && aligned.y == 200 && aligned.pixelWidth == 1600 && aligned.pixelHeight == 900);
    // A subpixel extent remains visible instead of rounding to zero.
    auto tiny = planWindowCaptureGeometry(-0.1, -0.1, 0.2, 0.2, 0, 0, 1, 0);
    assert(tiny.supported && tiny.pixelWidth == 2 && tiny.pixelHeight == 2);
    for (double bad : {0.0, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
        assert(!planWindowCaptureGeometry(0, 0, 10, 10, 0, 0, bad, 0).supported);
    assert(!planWindowCaptureGeometry(0, 0, 1e30, 10, 0, 0, 1, 0).supported);
    assert(!planWindowCaptureGeometry(1e30, 0, 10, 10, 0, 0, 1, 0).supported);
    assert(!planWindowCaptureGeometry(0, 0, 10, 10, 0, 0, 1, 8).supported);
    assert(!planWindowStreamFramebuffer(1920, 1080, 0, 720, 0).supported);
}
