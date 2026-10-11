#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace hyprcapture::color {
// Transport: linear sRGB/scRGB primaries, 1.0 = 80 cd/m2; straight alpha.
inline constexpr float scRgbNits = 80.0F;
inline constexpr float hdrWhiteNits = 203.0F;
inline constexpr float sdrShoulderKnee = 0.98F;

inline float halfToFloat(std::uint16_t h) {
    const auto sign = std::uint32_t(h & 0x8000U) << 16;
    auto exponent = (h >> 10) & 31U;
    auto mantissa = std::uint32_t(h & 1023U);
    if (!exponent) {
        if (!mantissa) return std::bit_cast<float>(sign);
        int e = -14;
        while (!(mantissa & 1024U)) { mantissa <<= 1; --e; }
        return std::bit_cast<float>(sign | (std::uint32_t(e + 127) << 23) | ((mantissa & 1023U) << 13));
    }
    return std::bit_cast<float>(sign | ((exponent == 31 ? 255U : exponent + 112U) << 23) | (mantissa << 13));
}

inline std::uint16_t floatToHalf(float value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const auto sign = (bits >> 16) & 0x8000U;
    const int e = int((bits >> 23) & 255U) - 127 + 15;
    auto m = bits & 0x7fffffU;
    if (e >= 31) return std::uint16_t(sign | 0x7c00U | (((bits >> 23) & 255U) == 255U && m ? 0x200U : 0));
    if (e <= 0) {
        if (e < -10) return std::uint16_t(sign);
        m |= 0x800000U;
        const int shift = 14 - e;
        const auto rounded = (m + ((1U << (shift - 1)) - 1U) + ((m >> shift) & 1U)) >> shift;
        return std::uint16_t(sign | rounded);
    }
    const auto rounded = m + 0xfffU + ((m >> 13) & 1U);
    return std::uint16_t(sign | ((std::uint32_t(e) << 10) + (rounded >> 13)));
}

inline float linearToSrgb(float v) {
    v = std::max(v, 0.0F);
    return v <= 0.0031308F ? 12.92F * v : 1.055F * std::pow(v, 1.0F / 2.4F) - 0.055F;
}
inline float srgbToLinear(float v) {
    return v <= 0.04045F ? v / 12.92F : std::pow((v + 0.055F) / 1.055F, 2.4F);
}
inline unsigned char byte(float v) {
    return std::isfinite(v) ? static_cast<unsigned char>(std::lround(std::clamp(v, 0.0F, 1.0F) * 255.0F)) : 0;
}

// SDR preview: preserve diffuse tones below the knee, compress highlight
// intensity with a smooth shoulder, then fit out-of-gamut colors towards gray.
// The peak-channel shoulder avoids turning saturated highlights into white.
inline std::array<float, 3> sdrLinear(std::array<float, 3> rgb, float whiteNits = hdrWhiteNits) {
    for (auto& v : rgb) v = std::isfinite(v) ? v * scRgbNits / whiteNits : 0.0F;
    const float peak = std::max({rgb[0], rgb[1], rgb[2], 0.0F});
    constexpr float knee = sdrShoulderKnee;
    if (whiteNits > scRgbNits && peak > knee) {
        const float mapped = knee + (1.0F - knee) * (peak - knee) / (peak - knee + (1.0F - knee));
        for (auto& v : rgb) v *= mapped / peak;
    }
    const float y = std::clamp(0.2126F * rgb[0] + 0.7152F * rgb[1] + 0.0722F * rgb[2], 0.0F, 1.0F);
    float saturation = 1.0F;
    for (const auto v : rgb) if (v < 0.0F) saturation = std::min(saturation, y / (y - v));
    for (auto& v : rgb) v = std::clamp(y + saturation * (v - y), 0.0F, 1.0F);
    return rgb;
}

inline float nitsToPq(float nits) {
    constexpr double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 32.0;
    constexpr double c1 = 3424.0 / 4096.0, c2 = 2413.0 / 128.0, c3 = 2392.0 / 128.0;
    const double p = std::pow(std::clamp(double(nits), 0.0, 10000.0) / 10000.0, m1);
    return float(std::pow((c1 + c2 * p) / (1.0 + c3 * p), m2));
}
inline std::array<float, 3> scRgbToBt2020(std::array<float, 3> rgb) {
    return {0.627404F * rgb[0] + 0.329283F * rgb[1] + 0.043313F * rgb[2],
            0.069097F * rgb[0] + 0.919540F * rgb[1] + 0.011362F * rgb[2],
            0.016391F * rgb[0] + 0.088013F * rgb[1] + 0.895595F * rgb[2]};
}
} // namespace hyprcapture::color
