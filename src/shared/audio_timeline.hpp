#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ctime>

namespace hyprcapture::audio {
constexpr std::int64_t sampleRate = 48000;
constexpr std::int64_t frameBytes = 2 * sizeof(float);
inline std::int64_t monotonicUs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}
inline std::int64_t sampleAt(std::int64_t us) {
    return std::max<std::int64_t>(0, us) * sampleRate / 1000000;
}
// Keep ordinary callback jitter from punching tiny holes into continuous sound.
// Larger discontinuities (suspension, dropped samples) retain their real position.
inline std::int64_t alignedSample(std::int64_t measured, std::int64_t next) {
    return next >= 0 && std::abs(measured - next) <= 240 ? next : measured;
}
}
