#include "plugin/window_stream.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <unistd.h>
#include <utility>

namespace hyprcapture {
namespace {

constexpr std::array<unsigned char, 4> MAGIC{'H', 'C', 'S', 'F'};

void writeU16(std::array<unsigned char, WINDOW_STREAM_FRAME_HEADER_BYTES>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<unsigned char>(value >> 8U);
    bytes[offset + 1] = static_cast<unsigned char>(value);
}

void writeU32(std::array<unsigned char, WINDOW_STREAM_FRAME_HEADER_BYTES>& bytes, std::size_t offset, std::uint32_t value) {
    for (int index = 3; index >= 0; --index)
        bytes[offset + static_cast<std::size_t>(3 - index)] = static_cast<unsigned char>(value >> (index * 8));
}

void writeU64(std::array<unsigned char, WINDOW_STREAM_FRAME_HEADER_BYTES>& bytes, std::size_t offset, std::uint64_t value) {
    for (int index = 7; index >= 0; --index)
        bytes[offset + static_cast<std::size_t>(7 - index)] = static_cast<unsigned char>(value >> (index * 8));
}

std::uint16_t readU16(const unsigned char* bytes, std::size_t offset) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[offset]) << 8U) | bytes[offset + 1]);
}

std::uint32_t readU32(const unsigned char* bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index)
        value = (value << 8U) | bytes[offset + index];
    return value;
}

std::uint64_t readU64(const unsigned char* bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index)
        value = (value << 8U) | bytes[offset + index];
    return value;
}

void writeDouble(std::array<unsigned char, WINDOW_STREAM_FRAME_HEADER_BYTES>& bytes, std::size_t offset, double value) {
    writeU64(bytes, offset, std::bit_cast<std::uint64_t>(value));
}

double readDouble(const unsigned char* bytes, std::size_t offset) {
    return std::bit_cast<double>(readU64(bytes, offset));
}

} // namespace

bool validWindowStreamFrameMetadata(const WindowStreamFrameMetadata& metadata) {
    constexpr std::uint64_t RGBA_BYTES_PER_PIXEL = 4;
    constexpr std::uint64_t MAX_WINDOW_STREAM_PAYLOAD_BYTES = 512ULL * 1024ULL * 1024ULL;
    if (metadata.sequence == 0 || metadata.geometryEpoch == 0 || !std::isfinite(metadata.logicalX) || !std::isfinite(metadata.logicalY) || !std::isfinite(metadata.logicalWidth) ||
        !std::isfinite(metadata.logicalHeight) || metadata.logicalWidth <= 0.0 || metadata.logicalHeight <= 0.0 ||
        !std::isfinite(metadata.logicalX + metadata.logicalWidth) || !std::isfinite(metadata.logicalY + metadata.logicalHeight) ||
        metadata.pixelWidth == 0 || metadata.pixelHeight == 0 || metadata.format != WINDOW_STREAM_FORMAT_STRAIGHT_RGBA_TOP_DOWN)
        return false;

    const auto minimumStride = static_cast<std::uint64_t>(metadata.pixelWidth) * RGBA_BYTES_PER_PIXEL;
    if (metadata.stride != minimumStride)
        return false;
    const auto expectedBytes = static_cast<std::uint64_t>(metadata.stride) * metadata.pixelHeight;
    return expectedBytes <= MAX_WINDOW_STREAM_PAYLOAD_BYTES && metadata.payloadBytes == expectedBytes;
}

std::array<unsigned char, WINDOW_STREAM_FRAME_HEADER_BYTES> encodeWindowStreamFrameHeader(const WindowStreamFrameMetadata& metadata) {
    std::array<unsigned char, WINDOW_STREAM_FRAME_HEADER_BYTES> bytes{};
    if (!validWindowStreamFrameMetadata(metadata))
        return bytes;
    std::copy(MAGIC.begin(), MAGIC.end(), bytes.begin());
    writeU16(bytes, 4, WINDOW_STREAM_FRAME_VERSION);
    writeU16(bytes, 6, WINDOW_STREAM_FRAME_HEADER_BYTES);
    writeU64(bytes, 8, metadata.sequence);
    writeU64(bytes, 16, metadata.captureMonotonicNs);
    writeU64(bytes, 24, metadata.geometryEpoch);
    writeDouble(bytes, 32, metadata.logicalX);
    writeDouble(bytes, 40, metadata.logicalY);
    writeDouble(bytes, 48, metadata.logicalWidth);
    writeDouble(bytes, 56, metadata.logicalHeight);
    writeU32(bytes, 64, metadata.pixelWidth);
    writeU32(bytes, 68, metadata.pixelHeight);
    writeU32(bytes, 72, metadata.stride);
    writeU32(bytes, 76, metadata.format);
    writeU64(bytes, 80, metadata.payloadBytes);
    // bytes 88..95 are specified zero reserved bytes.
    return bytes;
}

std::optional<WindowStreamFrameMetadata> decodeWindowStreamFrameHeader(const unsigned char* bytes, std::size_t length) {
    if (!bytes || length != WINDOW_STREAM_FRAME_HEADER_BYTES || !std::equal(MAGIC.begin(), MAGIC.end(), bytes) ||
        readU16(bytes, 4) != WINDOW_STREAM_FRAME_VERSION || readU16(bytes, 6) != WINDOW_STREAM_FRAME_HEADER_BYTES || readU64(bytes, 88) != 0)
        return std::nullopt;
    WindowStreamFrameMetadata metadata{
        .sequence = readU64(bytes, 8),
        .captureMonotonicNs = readU64(bytes, 16),
        .geometryEpoch = readU64(bytes, 24),
        .logicalX = readDouble(bytes, 32),
        .logicalY = readDouble(bytes, 40),
        .logicalWidth = readDouble(bytes, 48),
        .logicalHeight = readDouble(bytes, 56),
        .pixelWidth = readU32(bytes, 64),
        .pixelHeight = readU32(bytes, 68),
        .stride = readU32(bytes, 72),
        .format = readU32(bytes, 76),
        .payloadBytes = readU64(bytes, 80),
    };
    return validWindowStreamFrameMetadata(metadata) ? std::optional{metadata} : std::nullopt;
}

PendingWindowStreamFrame::PendingWindowStreamFrame(WindowStreamFrameMetadata nextMetadata, int nextPayloadFd) : metadata(nextMetadata), payloadFd(nextPayloadFd) {}

PendingWindowStreamFrame::PendingWindowStreamFrame(PendingWindowStreamFrame&& other) noexcept : metadata(other.metadata), payloadFd(std::exchange(other.payloadFd, -1)) {}

PendingWindowStreamFrame& PendingWindowStreamFrame::operator=(PendingWindowStreamFrame&& other) noexcept {
    if (this != &other) {
        if (payloadFd >= 0)
            close(payloadFd);
        metadata = other.metadata;
        payloadFd = std::exchange(other.payloadFd, -1);
    }
    return *this;
}

PendingWindowStreamFrame::~PendingWindowStreamFrame() {
    if (payloadFd >= 0)
        close(payloadFd);
}

template <std::size_t SlotCount>
void WindowStreamPboMetadataSlotsFor<SlotCount>::reset() {
    for (auto& slot : m_slots)
        slot = {};
    m_windowAddress.clear();
    m_pixelWidth = 0;
    m_pixelHeight = 0;
    m_stride = 0;
    m_pending = 0;
    m_next = 0;
}

template <std::size_t SlotCount>
void WindowStreamPboMetadataSlotsFor<SlotCount>::resetIfSourceChanged(
    const std::string& windowAddress,
    std::uint32_t pixelWidth,
    std::uint32_t pixelHeight,
    std::uint32_t stride) {
    if (m_pending != 0 && (m_windowAddress != windowAddress || m_pixelWidth != pixelWidth || m_pixelHeight != pixelHeight || m_stride != stride))
        reset();
}

template <std::size_t SlotCount>
bool WindowStreamPboMetadataSlotsFor<SlotCount>::canIssue() const {
    return m_pending < BUFFER_COUNT;
}

template <std::size_t SlotCount>
std::optional<std::size_t> WindowStreamPboMetadataSlotsFor<SlotCount>::issue(const std::string& windowAddress, const WindowStreamFrameMetadata& metadata) {
    if (windowAddress.empty() || !validWindowStreamFrameMetadata(metadata))
        return std::nullopt;
    resetIfSourceChanged(windowAddress, metadata.pixelWidth, metadata.pixelHeight, metadata.stride);
    if (!canIssue())
        return std::nullopt;
    auto& slot = m_slots[m_next];
    if (slot.metadata)
        return std::nullopt;
    slot.windowAddress = windowAddress;
    slot.metadata = metadata;
    const auto issued = m_next;
    m_next = (m_next + 1) % BUFFER_COUNT;
    ++m_pending;
    m_windowAddress = windowAddress;
    m_pixelWidth = metadata.pixelWidth;
    m_pixelHeight = metadata.pixelHeight;
    m_stride = metadata.stride;
    return issued;
}

template <std::size_t SlotCount>
std::optional<WindowStreamFrameMetadata> WindowStreamPboMetadataSlotsFor<SlotCount>::mapOldest() {
    if (m_pending == 0)
        return std::nullopt;
    const auto oldest = (m_next + BUFFER_COUNT - m_pending) % BUFFER_COUNT;
    auto& slot = m_slots[oldest];
    if (!slot.metadata) {
        reset();
        return std::nullopt;
    }
    auto metadata = std::move(slot.metadata);
    slot = {};
    --m_pending;
    return metadata;
}

template <std::size_t SlotCount>
bool WindowStreamPboMetadataSlotsFor<SlotCount>::discardOldest() {
    if (m_pending == 0)
        return false;
    const auto oldest = (m_next + BUFFER_COUNT - m_pending) % BUFFER_COUNT;
    auto& slot = m_slots[oldest];
    if (!slot.metadata) {
        reset();
        return false;
    }
    slot = {};
    --m_pending;
    return true;
}

template <std::size_t SlotCount>
std::size_t WindowStreamPboMetadataSlotsFor<SlotCount>::pendingCount() const {
    return m_pending;
}

template class WindowStreamPboMetadataSlotsFor<1>;
template class WindowStreamPboMetadataSlotsFor<2>;

void LatestWindowStreamSlot::publish(PendingWindowStreamFrame frame) {
    std::scoped_lock lock(m_mutex);
    if (m_latest)
        ++m_droppedFrames;
    m_latest = std::move(frame);
}

std::optional<PendingWindowStreamFrame> LatestWindowStreamSlot::takeLatest() {
    std::scoped_lock lock(m_mutex);
    return std::exchange(m_latest, std::nullopt);
}

std::uint64_t LatestWindowStreamSlot::droppedFrames() const {
    std::scoped_lock lock(m_mutex);
    return m_droppedFrames;
}

} // namespace hyprcapture
