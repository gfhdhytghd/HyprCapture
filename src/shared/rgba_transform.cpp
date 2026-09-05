#include "shared/rgba_transform.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <utility>

namespace hyprcapture {
namespace {

constexpr std::size_t RGBA_BYTES_PER_PIXEL = 4;

constexpr unsigned char unpremultiplyChannel(unsigned char channel, unsigned char alpha) {
    if (alpha == 0)
        return 0;
    if (alpha == 255)
        return channel;
    const int straight = (static_cast<int>(channel) * 255 + alpha / 2) / alpha;
    return static_cast<unsigned char>(std::min(255, straight));
}

constexpr std::array<unsigned char, 256 * 256> makeUnpremultiplyLut() {
    std::array<unsigned char, 256 * 256> lut{};
    for (std::size_t alpha = 0; alpha < 256; ++alpha)
        for (std::size_t channel = 0; channel < 256; ++channel)
            lut[(alpha << 8U) | channel] = unpremultiplyChannel(static_cast<unsigned char>(channel), static_cast<unsigned char>(alpha));
    return lut;
}

constexpr auto UNPREMULTIPLY_LUT = makeUnpremultiplyLut();

bool checkedRgbaByteSize(int width, int height, std::size_t& bytes) {
    if (width <= 0 || height <= 0)
        return false;

    const auto w = static_cast<std::size_t>(width);
    const auto h = static_cast<std::size_t>(height);
    if (w > std::numeric_limits<std::size_t>::max() / h / RGBA_BYTES_PER_PIXEL)
        return false;

    bytes = w * h * RGBA_BYTES_PER_PIXEL;
    return true;
}

template <typename SourcePixelFn>
RgbaFrame remapRgbaPixels(const RgbaFrame& source, int targetWidth, int targetHeight, SourcePixelFn sourcePixel) {
    if (!rgbaFrameHasExpectedSize(source))
        return {};

    std::size_t bytes = 0;
    if (!checkedRgbaByteSize(targetWidth, targetHeight, bytes))
        return {};

    RgbaFrame target;
    target.width = targetWidth;
    target.height = targetHeight;
    target.pixels.assign(bytes, 0);

    for (int y = 0; y < targetHeight; ++y) {
        for (int x = 0; x < targetWidth; ++x) {
            const auto [srcX, srcY] = sourcePixel(x, y);
            if (srcX < 0 || srcX >= source.width || srcY < 0 || srcY >= source.height)
                return {};

            const auto src = (static_cast<std::size_t>(srcY) * source.width + srcX) * RGBA_BYTES_PER_PIXEL;
            const auto dst = (static_cast<std::size_t>(y) * targetWidth + x) * RGBA_BYTES_PER_PIXEL;
            std::copy_n(source.pixels.data() + src, RGBA_BYTES_PER_PIXEL, target.pixels.data() + dst);
        }
    }

    return target;
}

RgbaFrame rotate90Clockwise(const RgbaFrame& source) {
    return remapRgbaPixels(source, source.height, source.width, [&](int x, int y) {
        return std::pair<int, int>{y, source.height - 1 - x};
    });
}

RgbaFrame rotate180(const RgbaFrame& source) {
    return remapRgbaPixels(source, source.width, source.height, [&](int x, int y) {
        return std::pair<int, int>{source.width - 1 - x, source.height - 1 - y};
    });
}

RgbaFrame rotate90CounterClockwise(const RgbaFrame& source) {
    return remapRgbaPixels(source, source.height, source.width, [&](int x, int y) {
        return std::pair<int, int>{source.width - 1 - y, x};
    });
}

RgbaFrame flipHorizontal(const RgbaFrame& source) {
    return remapRgbaPixels(source, source.width, source.height, [&](int x, int y) {
        return std::pair<int, int>{source.width - 1 - x, y};
    });
}

} // namespace

bool rgbaFrameHasExpectedSize(const RgbaFrame& frame) {
    std::size_t bytes = 0;
    return checkedRgbaByteSize(frame.width, frame.height, bytes) && frame.pixels.size() == bytes;
}

RgbaCrop logicalRgbaCropToFramebuffer(RgbaCrop crop, int framebufferWidth, int framebufferHeight, int transform) {
    const auto [x, y, w, h] = crop;
    switch (std::clamp(transform, 0, 7)) {
        case 1: return {y, framebufferHeight - x - w, h, w};
        case 2: return {framebufferWidth - x - w, framebufferHeight - y - h, w, h};
        case 3: return {framebufferWidth - y - h, x, h, w};
        case 4: return {framebufferWidth - x - w, y, w, h};
        case 5: return {y, x, h, w};
        case 6: return {x, framebufferHeight - y - h, w, h};
        case 7: return {framebufferWidth - y - h, framebufferHeight - x - w, h, w};
        default: return crop;
    }
}

RgbaFrame normalizeRgbaFrameToLogicalOrientation(RgbaFrame frame, int transform) {
    if (!rgbaFrameHasExpectedSize(frame))
        return {};

    switch (std::clamp(transform, 0, 7)) {
        case 1: return rotate90Clockwise(frame);
        case 2: return rotate180(frame);
        case 3: return rotate90CounterClockwise(frame);
        case 4: return flipHorizontal(frame);
        case 5: return flipHorizontal(rotate90Clockwise(frame));
        case 6: return flipHorizontal(rotate180(frame));
        case 7: return flipHorizontal(rotate90CounterClockwise(frame));
        default: return frame;
    }
}

void unpremultiplyRgbaPixels(std::vector<unsigned char>& pixels) {
    for (std::size_t i = 0; i + 3 < pixels.size(); i += RGBA_BYTES_PER_PIXEL) {
        const auto alpha = pixels[i + 3];
        if (alpha == 0) {
            pixels[i] = 0;
            pixels[i + 1] = 0;
            pixels[i + 2] = 0;
            continue;
        }
        if (alpha == 255)
            continue;
        const auto base = static_cast<std::size_t>(alpha) << 8U;
        pixels[i] = UNPREMULTIPLY_LUT[base | pixels[i]];
        pixels[i + 1] = UNPREMULTIPLY_LUT[base | pixels[i + 1]];
        pixels[i + 2] = UNPREMULTIPLY_LUT[base | pixels[i + 2]];
    }
}

} // namespace hyprcapture
