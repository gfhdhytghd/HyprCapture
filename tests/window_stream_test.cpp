#include "plugin/window_stream.hpp"

#include <cassert>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <unistd.h>

using namespace hyprcapture;

namespace {

WindowStreamFrameMetadata metadata(std::uint64_t sequence) {
    return {
        .sequence = sequence,
        .captureMonotonicNs = 123456789 + sequence,
        .geometryEpoch = 17,
        .logicalX = 10.5,
        .logicalY = -2.25,
        .logicalWidth = 212.0,
        .logicalHeight = 137.0,
        .pixelWidth = 424,
        .pixelHeight = 274,
        .stride = 1696,
        .format = WINDOW_STREAM_FORMAT_STRAIGHT_RGBA_TOP_DOWN,
        .payloadBytes = 464704,
    };
}

void frameHeaderRoundTripsExactLayout() {
    const auto input = metadata(9);
    const auto encoded = encodeWindowStreamFrameHeader(input);
    assert(std::memcmp(encoded.data(), "HCSF", 4) == 0);
    assert(encoded[4] == 0 && encoded[5] == 1 && encoded[6] == 0 && encoded[7] == 96);
    const auto decoded = decodeWindowStreamFrameHeader(encoded.data(), encoded.size());
    assert(decoded);
    assert(decoded->sequence == input.sequence);
    assert(decoded->captureMonotonicNs == input.captureMonotonicNs);
    assert(decoded->geometryEpoch == input.geometryEpoch);
    assert(decoded->logicalX == input.logicalX && decoded->logicalY == input.logicalY);
    assert(decoded->logicalWidth == input.logicalWidth && decoded->logicalHeight == input.logicalHeight);
    assert(decoded->pixelWidth == input.pixelWidth && decoded->pixelHeight == input.pixelHeight && decoded->stride == input.stride);
    assert(decoded->payloadBytes == input.payloadBytes);
}

void rejectsInvalidOrUnsealedMetadataHeader() {
    auto encoded = encodeWindowStreamFrameHeader(metadata(1));
    encoded[95] = 1;
    assert(!decodeWindowStreamFrameHeader(encoded.data(), encoded.size()));

    auto bad = metadata(1);
    bad.payloadBytes--;
    assert(!validWindowStreamFrameMetadata(bad));
    assert(encodeWindowStreamFrameHeader(bad) == std::array<unsigned char, WINDOW_STREAM_FRAME_HEADER_BYTES>{});
}

void latestSlotDropsReplacedFrameAndPreservesItsOwnTimestamp() {
    int firstPipe[2]{};
    int secondPipe[2]{};
    assert(pipe(firstPipe) == 0 && pipe(secondPipe) == 0);
    close(firstPipe[1]);
    close(secondPipe[1]);
    LatestWindowStreamSlot slot;
    slot.publish(PendingWindowStreamFrame{metadata(1), firstPipe[0]});
    slot.publish(PendingWindowStreamFrame{metadata(2), secondPipe[0]});
    assert(slot.droppedFrames() == 1);
    const auto latest = slot.takeLatest();
    assert(latest && latest->metadata.sequence == 2);
    assert(latest->metadata.captureMonotonicNs == 123456791);
    assert(latest->metadata.geometryEpoch == 17);
    assert(slot.droppedFrames() == 1);
    assert(!slot.takeLatest());
}

void singlePboSlotRejectsASecondOutstandingFrame() {
    WindowStreamPboMetadataSlots slots;
    assert(slots.issue("0xabc", metadata(1)) == std::optional<std::size_t>{0});
    assert(!slots.issue("0xabc", metadata(2)));
    // Mapping the first submitted PBO on a later drain must preserve the
    // original render-start timestamp, not the time of that later drain.
    const auto mapped = slots.mapOldest();
    assert(mapped && mapped->sequence == 1);
}

void drainPollArmsOnlyForPendingPboAndRejectsStaleCallbacks() {
    WindowStreamDrainPoll poll;
    assert(!poll.armed());
    assert(!poll.acceptsCallback(true));
    assert(!poll.armForPending(false));

    const auto earlyDrain = poll.armForPending(true);
    assert(earlyDrain && *earlyDrain == std::chrono::milliseconds{1});
    assert(poll.armed());
    assert(poll.acceptsCallback(true));
    assert(!poll.acceptsCallback(false));

    // A successful ready drain disarms the timer; a stop also makes an event
    // already queued by the compositor event loop harmless.
    assert(!poll.armForPending(false));
    assert(!poll.armed() && !poll.acceptsCallback(true));
    (void)poll.armForPending(true);
    poll.stop();
    assert(!poll.armed() && !poll.acceptsCallback(true));
}

void multiplePboSlotsReturnTheIssuedFrameMetadataAndResetOnSourceChange() {
    WindowStreamPboMetadataSlotsFor<2> slots;
    auto first = metadata(1);
    first.geometryEpoch = 20;
    auto second = metadata(2);
    second.geometryEpoch = 21;
    second.logicalX = 480.0;
    second.logicalY = 120.0;
    assert(slots.issue("0xabc", first) == std::optional<std::size_t>{0});
    assert(slots.issue("0xabc", second) == std::optional<std::size_t>{1});
    const auto mapped = slots.mapOldest();
    assert(mapped && mapped->sequence == 1 && mapped->captureMonotonicNs == first.captureMonotonicNs && mapped->geometryEpoch == 20);
    assert(slots.pendingCount() == 1);

    // Pure translation and a new geometry epoch retain the old PBO slot; the
    // mapped payload must keep the geometry/identity issued with that slot.
    const auto moved = slots.mapOldest();
    assert(moved && moved->sequence == 2 && moved->geometryEpoch == 21 && moved->logicalX == 480.0 && moved->logicalY == 120.0);
    assert(slots.pendingCount() == 0);

    // If the GPU has completed a backlog, stream delivery keeps only the
    // newest completion.  Older slots must be released without changing the
    // newest slot's original timestamp or geometry epoch.
    assert(slots.issue("0xabc", metadata(4)) == std::optional<std::size_t>{0});
    assert(slots.issue("0xabc", metadata(5)) == std::optional<std::size_t>{1});
    // The multi-slot test ring holds two outstanding metadata records; a
    // third issue must be rejected before it can overwrite either frame's
    // original timestamp/geometry lineage.
    assert(!slots.issue("0xabc", metadata(6)));
    assert(slots.discardOldest());
    const auto newestOnly = slots.mapOldest();
    assert(newestOnly && newestOnly->sequence == 5 && newestOnly->captureMonotonicNs == metadata(5).captureMonotonicNs);
    assert(slots.pendingCount() == 0);

    auto resized = metadata(3);
    resized.pixelWidth = 212;
    resized.stride = 848;
    resized.payloadBytes = 232352;
    assert(slots.issue("0xabc", first));
    assert(slots.issue("0xabc", resized) == std::optional<std::size_t>{0});
    assert(slots.pendingCount() == 1);
    const auto resizedMapped = slots.mapOldest();
    assert(resizedMapped && resizedMapped->sequence == 3 && resizedMapped->pixelWidth == 212);

    assert(slots.issue("0xabc", first));
    assert(slots.issue("0xdef", second));
    assert(slots.pendingCount() == 1);
    const auto switched = slots.mapOldest();
    assert(switched && switched->sequence == 2);
}

void printGoldenHeaderHex() {
    const auto header = encodeWindowStreamFrameHeader(metadata(9));
    for (const auto byte : header)
        std::cout << std::hex << std::setfill('0') << std::setw(2) << static_cast<unsigned>(byte);
    std::cout << '\n';
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--golden-header-hex") == 0) {
        printGoldenHeaderHex();
        return 0;
    }
    frameHeaderRoundTripsExactLayout();
    rejectsInvalidOrUnsealedMetadataHeader();
    latestSlotDropsReplacedFrameAndPreservesItsOwnTimestamp();
    singlePboSlotRejectsASecondOutstandingFrame();
    drainPollArmsOnlyForPendingPboAndRejectsStaleCallbacks();
    multiplePboSlotsReturnTheIssuedFrameMetadataAndResetOnSourceChange();
}
