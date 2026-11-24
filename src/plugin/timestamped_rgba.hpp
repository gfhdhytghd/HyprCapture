#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace hyprcapture {
// The fd must be nonblocking. Backpressure waits until progress or cancellation.
int writeRecordingBytes(int fd, std::span<const unsigned char> bytes, std::stop_token stop);
// Stream raw RGBA with capture timestamps to FFmpeg without synthesizing CFR
// frames. The descriptor is borrowed; all calls belong to the encoder worker.
class TimestampedRgbaWriter {
  public:
    TimestampedRgbaWriter();
    ~TimestampedRgbaWriter();
    bool open(int fd, int width, int height, int fps, std::stop_token stop = {});
    bool write(std::span<const unsigned char> pixels, std::int64_t ptsUs);
    bool finish();
    std::string error() const;
  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Preserve gaps at the encoder as well as at its timestamped input.
std::vector<std::string> timestampedRgbaInputArgs();
std::vector<std::string> timestampedRgbaOutputArgs();
std::int64_t recordingTailPts(std::int64_t lastPtsUs, std::int64_t endUs, int fps);
} // namespace hyprcapture
