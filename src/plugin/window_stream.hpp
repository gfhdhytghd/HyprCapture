#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace hyprcapture {

// This metadata is sent over SOCK_SEQPACKET together with exactly one
// SCM_RIGHTS memfd. Pixels never occupy the socket packet itself.
constexpr std::size_t WINDOW_STREAM_FRAME_HEADER_BYTES = 96;
constexpr std::uint16_t WINDOW_STREAM_FRAME_VERSION = 1;
constexpr std::uint32_t WINDOW_STREAM_FORMAT_STRAIGHT_RGBA_TOP_DOWN = 1;

// Pure lifecycle policy for the event-loop PBO-drain timer.  The callback
// owns the actual GL fence test; this only makes its arm/disarm and stale
// callback decisions independently testable.
class WindowStreamDrainPoll {
  public:
    [[nodiscard]] std::optional<std::chrono::milliseconds> armForPending(bool pending) {
        m_armed = pending;
        return pending ? std::optional{std::chrono::milliseconds{1}} : std::nullopt;
    }

    [[nodiscard]] bool acceptsCallback(bool matchingTimer) const {
        return m_armed && matchingTimer;
    }

    void stop() { m_armed = false; }
    [[nodiscard]] bool armed() const { return m_armed; }

  private:
    bool m_armed = false;
};

struct WindowStreamFrameMetadata {
    std::uint64_t sequence = 0;
    // Timestamp sampled when the matching render begins, never rewritten when
    // a later PBO is drained or while a worker sends the frame.
    std::uint64_t captureMonotonicNs = 0;
    std::uint64_t geometryEpoch = 0;
    double        logicalX = 0.0;
    double        logicalY = 0.0;
    double        logicalWidth = 0.0;
    double        logicalHeight = 0.0;
    std::uint32_t pixelWidth = 0;
    std::uint32_t pixelHeight = 0;
    std::uint32_t stride = 0;
    std::uint32_t format = WINDOW_STREAM_FORMAT_STRAIGHT_RGBA_TOP_DOWN;
    std::uint64_t payloadBytes = 0;
};

bool validWindowStreamFrameMetadata(const WindowStreamFrameMetadata& metadata);
std::array<unsigned char, WINDOW_STREAM_FRAME_HEADER_BYTES> encodeWindowStreamFrameHeader(const WindowStreamFrameMetadata& metadata);
std::optional<WindowStreamFrameMetadata> decodeWindowStreamFrameHeader(const unsigned char* bytes, std::size_t length);

// The frame owns a payload descriptor. The future worker must create and
// validate a sealed, read-only memfd before this reaches SCM_RIGHTS; this
// ownership boundary deliberately does not claim that an arbitrary fd is sealed.
struct PendingWindowStreamFrame {
    WindowStreamFrameMetadata metadata;
    int                       payloadFd = -1;

    PendingWindowStreamFrame() = default;
    PendingWindowStreamFrame(WindowStreamFrameMetadata metadata, int payloadFd);
    PendingWindowStreamFrame(const PendingWindowStreamFrame&) = delete;
    PendingWindowStreamFrame& operator=(const PendingWindowStreamFrame&) = delete;
    PendingWindowStreamFrame(PendingWindowStreamFrame&& other) noexcept;
    PendingWindowStreamFrame& operator=(PendingWindowStreamFrame&& other) noexcept;
    ~PendingWindowStreamFrame();
};

// Independent from the recording PBO state. OpenGL code must call `issue`
// immediately before glReadPixels for a slot and `mapOldest` only after that
// exact slot maps. Thus an old PBO can never inherit current-window metadata.
template <std::size_t SlotCount>
class WindowStreamPboMetadataSlotsFor {
  public:
    static_assert(SlotCount > 0);
    static constexpr std::size_t BUFFER_COUNT = SlotCount;

    void reset();
    void resetIfSourceChanged(const std::string& windowAddress, std::uint32_t pixelWidth, std::uint32_t pixelHeight, std::uint32_t stride);
    [[nodiscard]] bool canIssue() const;
    [[nodiscard]] std::optional<std::size_t> issue(const std::string& windowAddress, const WindowStreamFrameMetadata& metadata);
    [[nodiscard]] std::optional<WindowStreamFrameMetadata> mapOldest();
    // Drop an already-completed older PBO without copying it into compositor
    // memory.  The caller must use this only after its GL fence is signaled.
    [[nodiscard]] bool discardOldest();
    [[nodiscard]] std::size_t pendingCount() const;

  private:
    struct Slot {
        std::string                     windowAddress;
        std::optional<WindowStreamFrameMetadata> metadata;
    };

    std::array<Slot, BUFFER_COUNT> m_slots;
    std::string                    m_windowAddress;
    std::uint32_t                  m_pixelWidth = 0;
    std::uint32_t                  m_pixelHeight = 0;
    std::uint32_t                  m_stride = 0;
    std::size_t                    m_pending = 0;
    std::size_t                    m_next = 0;
};

// One outstanding stream readback is the production latency budget. Tests
// also instantiate the template with two slots to retain coverage for stale
// completion dropping and ring-wrap metadata lineage.
using WindowStreamPboMetadataSlots = WindowStreamPboMetadataSlotsFor<1>;

// A one-entry latest-frame boundary for the future memfd-sealing worker. The
// compositor never performs socket I/O; publish replaces an unsent frame.
class LatestWindowStreamSlot {
  public:
    void publish(PendingWindowStreamFrame frame);
    std::optional<PendingWindowStreamFrame> takeLatest();
    [[nodiscard]] std::uint64_t droppedFrames() const;

  private:
    mutable std::mutex                       m_mutex;
    std::optional<PendingWindowStreamFrame> m_latest;
    std::uint64_t                            m_droppedFrames = 0;
};

} // namespace hyprcapture
