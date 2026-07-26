#include "shared/rgba_transform.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

hyprcapture::RgbaFrame numberedFrame() {
    hyprcapture::RgbaFrame frame;
    frame.width = 3;
    frame.height = 2;
    frame.pixels.resize(3 * 2 * 4);
    for (int i = 0; i < 6; ++i) {
        frame.pixels[static_cast<std::size_t>(i) * 4] = static_cast<unsigned char>(i + 1);
        frame.pixels[static_cast<std::size_t>(i) * 4 + 3] = 255;
    }
    return frame;
}

std::vector<int> pixelNumbers(const hyprcapture::RgbaFrame& frame) {
    std::vector<int> values;
    for (std::size_t i = 0; i < frame.pixels.size(); i += 4)
        values.push_back(frame.pixels[i]);
    return values;
}

void requireTransform(int transform, int width, int height, const std::vector<int>& expected) {
    const auto result = hyprcapture::normalizeRgbaFrameToLogicalOrientation(numberedFrame(), transform);
    require(result.width == width, "transform " + std::to_string(transform) + " width");
    require(result.height == height, "transform " + std::to_string(transform) + " height");
    require(pixelNumbers(result) == expected, "transform " + std::to_string(transform) + " pixels");
}

} // namespace

int main() {
    requireTransform(0, 3, 2, {1, 2, 3, 4, 5, 6});
    requireTransform(1, 2, 3, {4, 1, 5, 2, 6, 3});
    requireTransform(2, 3, 2, {6, 5, 4, 3, 2, 1});
    requireTransform(3, 2, 3, {3, 6, 2, 5, 1, 4});
    requireTransform(4, 3, 2, {3, 2, 1, 6, 5, 4});
    requireTransform(5, 2, 3, {1, 4, 2, 5, 3, 6});
    requireTransform(6, 3, 2, {4, 5, 6, 1, 2, 3});
    requireTransform(7, 2, 3, {6, 3, 5, 2, 4, 1});

    auto malformed = numberedFrame();
    malformed.pixels.pop_back();
    require(hyprcapture::normalizeRgbaFrameToLogicalOrientation(std::move(malformed), 1).pixels.empty(), "reject malformed frame");

    std::cout << "rgba transform tests passed\n";
    return 0;
}
