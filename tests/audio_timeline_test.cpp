#include "shared/audio_timeline.hpp"
#include <cassert>
#include <limits>
int main() {
    using namespace hyprcapture::audio;
    assert(sampleAt(-1) == 0);
    assert(sampleAt(1000000) == 48000);
    assert(sampleAt(600000000) == 28800000);
    assert(alignedSample(48010, 48000) == 48000);
    assert(alignedSample(96000, 48000) == 96000);
    assert(alignedSample(96000, -1) == 96000);
    assert(monotonicUs() > 0);
}
