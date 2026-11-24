#include "timestamped_rgba.hpp"
#include <algorithm>
#include <cerrno>
#include <limits>
#include <poll.h>
#include <unistd.h>
extern "C" {
#include <libavcodec/codec_id.h>
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
}

namespace hyprcapture {
int writeRecordingBytes(int fd, std::span<const unsigned char> bytes, std::stop_token stop, std::chrono::milliseconds stallTimeout) {
    using Clock = std::chrono::steady_clock;
    auto deadline = Clock::now() + stallTimeout;
    while (!bytes.empty()) {
        if (stop.stop_requested()) return AVERROR(ECANCELED);
        if (Clock::now() >= deadline) return AVERROR(ETIMEDOUT);
        const auto n = ::write(fd, bytes.data(), bytes.size());
        if (n > 0) {
            bytes = bytes.subspan(n);
            deadline = Clock::now() + stallTimeout;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd descriptor{fd, POLLOUT, 0};
            const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
            const int wait = static_cast<int>(std::clamp(remaining, std::chrono::milliseconds::zero(),
                                                         std::chrono::milliseconds(50)).count());
            if (poll(&descriptor, 1, wait) < 0 && errno != EINTR) return AVERROR(errno);
        } else if (n == 0 || errno != EINTR) {
            return AVERROR(n < 0 ? errno : EIO);
        }
    }
    return 0;
}

namespace {
struct RecordingPipe {
    int fd = -1;
    std::stop_token stop;
};
int writePacket(void* opaque, const unsigned char* bytes, int count) {
    const auto& pipe = *static_cast<RecordingPipe*>(opaque);
    const int error = writeRecordingBytes(pipe.fd, {bytes, static_cast<std::size_t>(count)}, pipe.stop);
    return error < 0 ? error : count;
}
// FFmpeg before 7 used a non-const write callback.
[[maybe_unused]] int writePacket(void* opaque, unsigned char* bytes, int count) {
    return writePacket(opaque, static_cast<const unsigned char*>(bytes), count);
}
constexpr AVRational MICROSECONDS{1, 1'000'000};
}
struct TimestampedRgbaWriter::Impl {
    RecordingPipe pipe;
    int fps = 60;
    std::size_t frameBytes = 0;
    std::int64_t lastPts = -1;
    AVFormatContext* mux = nullptr;
    AVIOContext* io = nullptr;
    AVStream* stream = nullptr;
    std::string failure;
    bool ended = false;
    ~Impl() {
        if (mux) avformat_free_context(mux);
        if (io) { av_freep(&io->buffer); avio_context_free(&io); }
    }
    bool check(int status) {
        if (status >= 0) return true;
        char text[AV_ERROR_MAX_STRING_SIZE]{};
        av_strerror(status, text, sizeof(text)); failure = text;
        return false;
    }
};
TimestampedRgbaWriter::TimestampedRgbaWriter() : m_impl(std::make_unique<Impl>()) {}
TimestampedRgbaWriter::~TimestampedRgbaWriter() = default;
bool TimestampedRgbaWriter::open(int fd, int width, int height, int fps, std::stop_token stop) {
    auto& s = *m_impl;
    const auto bytes = static_cast<std::uint64_t>(std::max(0, width)) * std::max(0, height) * 4;
    if (s.mux || fd < 0 || !bytes || bytes > std::numeric_limits<int>::max() || fps <= 0) return s.check(AVERROR(EINVAL));
    s.pipe = {fd, stop}; s.fps = fps; s.frameBytes = bytes;
    if (!s.check(avformat_alloc_output_context2(&s.mux, nullptr, "nut", nullptr))) return false;
    auto* buffer = static_cast<unsigned char*>(av_malloc(65536));
    if (!buffer) return s.check(AVERROR(ENOMEM));
    s.io = avio_alloc_context(buffer, 65536, 1, &s.pipe, nullptr, writePacket, nullptr);
    if (!s.io) { av_free(buffer); return s.check(AVERROR(ENOMEM)); }
    s.mux->pb = s.io; s.mux->flags |= AVFMT_FLAG_CUSTOM_IO;
    s.stream = avformat_new_stream(s.mux, nullptr);
    if (!s.stream) return s.check(AVERROR(ENOMEM));
    s.stream->time_base = MICROSECONDS;
    s.stream->avg_frame_rate = s.stream->r_frame_rate = AVRational{fps, 1};
    auto* par = s.stream->codecpar;
    par->codec_type = AVMEDIA_TYPE_VIDEO; par->codec_id = AV_CODEC_ID_RAWVIDEO;
    par->color_range = AVCOL_RANGE_JPEG;
    par->color_primaries = AVCOL_PRI_BT709;
    par->color_trc = AVCOL_TRC_IEC61966_2_1;
    par->color_space = AVCOL_SPC_RGB;
    par->format = AV_PIX_FMT_RGBA; par->codec_tag = MKTAG('R', 'G', 'B', 'A');
    par->width = width; par->height = height; par->sample_aspect_ratio = {1, 1};
    if (!s.check(avformat_write_header(s.mux, nullptr))) return false;
    avio_flush(s.io);
    return s.check(s.io->error);
}
bool TimestampedRgbaWriter::write(std::span<const unsigned char> pixels, std::int64_t ptsUs) {
    auto& s = *m_impl;
    if (!s.stream || s.ended || pixels.size() != s.frameBytes || ptsUs < 0 || ptsUs <= s.lastPts) return s.check(AVERROR(EINVAL));
    AVPacket packet{};
    packet.data = const_cast<unsigned char*>(pixels.data()); packet.size = static_cast<int>(pixels.size());
    packet.pts = packet.dts = av_rescale_q(ptsUs, MICROSECONDS, s.stream->time_base);
    packet.duration = std::max<std::int64_t>(1, av_rescale_q(1, AVRational{1, s.fps}, s.stream->time_base));
    packet.stream_index = s.stream->index; packet.flags = AV_PKT_FLAG_KEY;
    if (!s.check(av_write_frame(s.mux, &packet))) return false;
    avio_flush(s.io);
    if (!s.check(s.io->error)) return false;
    s.lastPts = ptsUs;
    return true;
}
bool TimestampedRgbaWriter::finish() {
    auto& s = *m_impl;
    if (!s.stream || s.ended) return s.check(AVERROR(EINVAL));
    s.ended = true;
    if (!s.check(av_write_trailer(s.mux))) return false;
    avio_flush(s.io);
    return s.check(s.io->error);
}
std::string TimestampedRgbaWriter::error() const { return m_impl->failure; }
std::vector<std::string> timestampedRgbaInputArgs() {
    return {"-f", "nut", "-probesize", "32", "-analyzeduration", "0", "-i", "pipe:0", "-an"};
}
std::vector<std::string> timestampedRgbaOutputArgs() {
    // Reordering sparse PTS through B-frames can shorten MP4/MOV duration.
    return {"-fps_mode", "passthrough", "-enc_time_base", "1:1000000", "-bf", "0"};
}
std::int64_t recordingTailPts(std::int64_t lastPtsUs, std::int64_t endUs, int fps) {
    return std::max(lastPtsUs, endUs - std::max<std::int64_t>(1, 1'000'000 / std::max(1, fps)));
}
} // namespace hyprcapture
