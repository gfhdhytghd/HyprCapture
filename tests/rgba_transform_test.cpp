#include "shared/rgba_transform.hpp"

#include <algorithm>
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

unsigned char legacyUnpremultiply(unsigned char channel, unsigned char alpha) {
    if (alpha == 0)
        return 0;
    if (alpha == 255)
        return channel;
    const int straight = (static_cast<int>(channel) * 255 + alpha / 2) / alpha;
    return static_cast<unsigned char>(std::min(255, straight));
}

void unpremultiplyLutMatchesLegacyFormulaForEveryBytePair() {
    for (int alpha = 0; alpha < 256; ++alpha) {
        for (int channel = 0; channel < 256; ++channel) {
            std::vector<unsigned char> pixels{
                static_cast<unsigned char>(channel),
                static_cast<unsigned char>(channel),
                static_cast<unsigned char>(channel),
                static_cast<unsigned char>(alpha),
            };
            hyprcapture::unpremultiplyRgbaPixels(pixels);
            const auto expected = legacyUnpremultiply(static_cast<unsigned char>(channel), static_cast<unsigned char>(alpha));
            require(pixels[0] == expected && pixels[1] == expected && pixels[2] == expected && pixels[3] == alpha,
                    "unpremultiply LUT must match the legacy formula for every channel/alpha pair");
        }
    }
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

    // Reading just the transformed physical crop must match normalizing the
    // whole monitor first. Use an off-center, non-square region for all eight
    // transforms so a swapped origin or extent cannot pass accidentally.
    hyprcapture::RgbaFrame monitor;
    monitor.width = 7;
    monitor.height = 5;
    monitor.pixels.resize(7 * 5 * 4);
    for (int i = 0; i < 35; ++i)
        monitor.pixels[i * 4] = i + 1;
    for (int transform = 0; transform < 8; ++transform) {
        const auto full = hyprcapture::normalizeRgbaFrameToLogicalOrientation(monitor, transform);
        const auto crop = hyprcapture::logicalRgbaCropToFramebuffer({1, 1, 2, 3}, 7, 5, transform);
        hyprcapture::RgbaFrame physical;
        physical.width = crop.width;
        physical.height = crop.height;
        for (int y = 0; y < crop.height; ++y)
            for (int x = 0; x < crop.width; ++x)
                for (int channel = 0; channel < 4; ++channel)
                    physical.pixels.push_back(monitor.pixels[((crop.y + y) * 7 + crop.x + x) * 4 + channel]);
        const auto logical = hyprcapture::normalizeRgbaFrameToLogicalOrientation(std::move(physical), transform);
        require(logical.width == 2 && logical.height == 3, "normalized crop extent");
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 2; ++x)
                require(logical.pixels[(y * 2 + x) * 4] == full.pixels[((y + 1) * full.width + x + 1) * 4], "normalized crop pixels");
    }

    auto malformed = numberedFrame();
    malformed.pixels.pop_back();
    require(hyprcapture::normalizeRgbaFrameToLogicalOrientation(std::move(malformed), 1).pixels.empty(), "reject malformed frame");

    unpremultiplyLutMatchesLegacyFormulaForEveryBytePair();

    std::cout << "rgba transform tests passed\n";
    return 0;
}
