#pragma once

#include <chrono>
#include <string>
#include <string_view>

namespace hyprcapture {

// Call only on the compositor main thread, with freshly read host settings.
// The enabled flag and output path are atomically published for worker readers.
void refreshTimingConfig(bool enabled, std::string file);
bool timingEnabled();
void traceTiming(std::string_view event, long long elapsedUs = -1);

class ScopedTiming {
  public:
    explicit ScopedTiming(std::string_view event);
    ~ScopedTiming();

    ScopedTiming(const ScopedTiming&) = delete;
    ScopedTiming& operator=(const ScopedTiming&) = delete;

  private:
    std::string_view                                m_event;
    std::chrono::steady_clock::time_point          m_started;
    bool                                           m_enabled = false;
};

} // namespace hyprcapture
