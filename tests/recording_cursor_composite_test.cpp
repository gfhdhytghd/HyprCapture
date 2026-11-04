#include "plugin/recording_cursor_composite.hpp"

#include <array>
#include <cassert>
#include <limits>

using namespace hyprcapture;
using Pixel = std::array<unsigned char, 4>;

static RecordingCursorImage image(RecordingCursorBox box, std::vector<unsigned char> pixels) {
    return {box, std::make_shared<const std::vector<unsigned char>>(std::move(pixels))};
}

static Pixel referenceOver(Pixel source, Pixel destination) {
    const double sa = source[3] / 255.0, da = destination[3] / 255.0;
    const double alpha = sa + da * (1.0 - sa);
    if (sa == 0.0) return destination;
    Pixel result;
    for (int c = 0; c < 3; ++c)
        result[c] = std::lround((source[c] * sa + destination[c] * da * (1.0 - sa)) / alpha);
    result[3] = std::lround(alpha * 255.0);
    return result;
}

int main() {
    // Regression: shadow reconstruction has filled every pixel beneath the
    // cursor rectangle. Transparent texels must retain that repaired shadow;
    // translucent black/colored edges blend with it, rather than skipping the
    // rectangle or overwriting the cursor with reconstructed shadow color.
    std::vector<unsigned char> shadow{10,20,30,128, 10,20,30,128, 10,20,30,128};
    auto cursor = image({0,0,3,1}, {255,0,255,0, 0,0,0,128, 200,100,50,255});
    compositeRecordingCursor(shadow, 3, 1, cursor);
    assert((Pixel{shadow[0],shadow[1],shadow[2],shadow[3]} == Pixel{10,20,30,128}));
    assert((Pixel{shadow[4],shadow[5],shadow[6],shadow[7]} == referenceOver({0,0,0,128}, {10,20,30,128})));
    assert((Pixel{shadow[8],shadow[9],shadow[10],shadow[11]} == Pixel{200,100,50,255}));

    // Independent normalized-alpha oracle covers opaque, transparent and faint
    // cursor/window pixels, including anti-aliased edges over empty padding.
    for (unsigned char sa : {0,1,4,64,127,128,192,254,255}) {
        for (unsigned char da : {0,1,4,64,127,128,192,254,255}) {
            for (auto rgb : {Pixel{0,0,0,sa}, Pixel{255,255,255,sa}, Pixel{17,125,240,sa}}) {
                Pixel dst{195,70,31,da};
                const auto expected = referenceOver(rgb, dst);
                compositeRecordingCursor(dst, 1, 1, image({0,0,1,1}, {rgb.begin(),rgb.end()}));
                for (int c = 0; c < 4; ++c)
                    assert(std::abs(int(dst[c]) - int(expected[c])) <= 1);
            }
        }
    }

    // A crop can put the origin outside the readback; clip both axes without
    // scaling or advancing the wrong source row.
    const auto clipped = image({-1,-1,2,2}, {1,0,0,255, 2,0,0,255, 3,0,0,255, 4,0,0,255});
    std::vector<unsigned char> frame(2*2*4, 0);
    compositeRecordingCursor(frame, 2, 2, clipped);
    assert(frame[0] == 4 && frame[3] == 255);
    for (int i = 4; i < 16; ++i) assert(frame[i] == 0);

    // Padding moves just the queued layer's coordinates. Later captures or
    // another readback copy cannot mutate the older layer's placement/pixels.
    auto padded = clipped;
    padded.box.x += 2;
    padded.box.y += 2;
    std::vector<unsigned char> expanded(4*4*4, 0);
    compositeRecordingCursor(expanded, 4, 4, padded);
    assert(expanded[(1*4+1)*4] == 1);
    assert(expanded[(2*4+2)*4] == 4);
    assert(clipped.box.x == -1 && clipped.box.y == -1);

    // Malformed dimensions/buffers and far-offscreen origins do not write.
    const auto untouched = frame;
    for (double x : {1e100, std::numeric_limits<double>::quiet_NaN(), 0.25, 200.0})
        compositeRecordingCursor(frame, 2, 2, image({x,0,1,1}, {9,9,9,255}));
    compositeRecordingCursor(frame, 2, 2, image({0,0,2,2}, {9,9,9,255}));
    compositeRecordingCursor(frame, 2, 2, {{0,0,1,1},{}});
    assert(frame == untouched);
}
