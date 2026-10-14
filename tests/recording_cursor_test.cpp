#include "plugin/recording_cursor.hpp"
#include "plugin/window_capture_geometry.hpp"

#include <cassert>
#include <limits>

int main() {
    using namespace hyprcapture;
    // Window exports are upright for all eight output transforms, including
    // fractional scale and bounds that extend beyond a negative output.
    for (int transform = 0; transform < 8; ++transform) {
        for (double scale : {1.0, 1.25, 1.5, 2.0, 3.0}) {
            const auto geometry = planWindowCaptureGeometry(-2060.3, -11.7, 2511.2, 833.6, -1920, -100, scale, transform);
            assert(geometry.supported);
            const double px = geometry.x + 2000, py = geometry.y + 100;
            const auto cursor = planRecordingCursor(px, py, 3.5, 5, 24, 32,
                geometry.x, geometry.y, geometry.width, geometry.height, scale, true);
            assert(cursor);
            assert(cursor->x == std::round(1996.5 * scale));
            assert(cursor->y == std::round(95 * scale));
            assert(cursor->width == std::round(24 * scale));
            assert(cursor->height == std::round(32 * scale));
        }
    }
    // Window gating follows the pointer hotspot, with right/bottom exclusive.
    for (const auto point : {RecordingCursorBox{99, 220, 0, 0}, {200, 220, 0, 0},
                             {120, 199, 0, 0}, {120, 280, 0, 0}}) {
        assert(!planRecordingCursor(point.x, point.y, 5, 5, 24, 32, 100, 200, 100, 80, 2, true));
    }
    auto edge = planRecordingCursor(100, 200, 5, 5, 24, 32, 100, 200, 100, 80, 2, true);
    assert(edge && edge->x == -10 && edge->y == -10);
    assert(edge->contains(-10, -10) && edge->contains(37, 53));
    assert(!edge->contains(38, 10) && !edge->contains(0, 54));
    // Monitor/region crops retain the visible part even if the hotspot is
    // outside. GPU crop and existing mixed-scale assembly clip the rest.
    auto region = planRecordingCursor(-101, -50, 2, 3, 24, 32, -100, -60, 120, 80, 1.25, false);
    assert(region && region->x == -4 && region->y == 9);
    assert(!planRecordingCursor(-125, -50, 0, 0, 24, 32, -100, -60, 120, 80, 1.25, false));
    // A moved window changes the local pointer position each frame.
    auto moved = planRecordingCursor(150, 250, 5, 5, 24, 32, 125, 225, 100, 80, 2, true);
    assert(moved && moved->x == 40 && moved->y == 40);
    for (double bad : {0.0, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
        assert(!planRecordingCursor(150, 250, 5, 5, 24, 32, 100, 200, 100, 80, bad, true));
    assert(!planRecordingCursor(std::numeric_limits<double>::quiet_NaN(), 250, 5, 5, 24, 32, 100, 200, 100, 80, 2, true));
    assert(!planRecordingCursor(150, 250, 5, 5, 0, 32, 100, 200, 100, 80, 2, true));
}
