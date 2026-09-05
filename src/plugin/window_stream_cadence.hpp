#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace hyprcapture {

// Pure absolute-deadline scheduler for the compositor stream timer. It never
// asks a late tick to catch up: elapsed slots are skipped and the next wakeup
// stays bounded to one millisecond or later.
struct WindowStreamCadence {
    std::int64_t nextDueUs = 0;
    std::int64_t lastCompletedUs = 0;
    bool         initialized = false;
};

inline std::chrono::microseconds scheduleNextWindowStreamTick(WindowStreamCadence& cadence,
                                                                std::int64_t tickStartedUs,
                                                                std::int64_t tickCompletedUs,
                                                                int fps) {
    const auto safeFps = std::clamp(fps, 1, 1000);
    const auto intervalUs = std::max<std::int64_t>(1'000, 1'000'000 / safeFps);
    const auto completedUs = std::max({tickStartedUs, tickCompletedUs, cadence.lastCompletedUs});

    if (!cadence.initialized) {
        cadence.nextDueUs = std::max(tickStartedUs, cadence.lastCompletedUs) + intervalUs;
        cadence.initialized = true;
    }
    while (cadence.nextDueUs <= completedUs)
        cadence.nextDueUs += intervalUs;

    cadence.lastCompletedUs = completedUs;
    return std::chrono::microseconds{std::max<std::int64_t>(1'000, cadence.nextDueUs - completedUs)};
}

} // namespace hyprcapture
