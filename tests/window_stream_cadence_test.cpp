#include "plugin/window_stream_cadence.hpp"

#include <cassert>

using namespace hyprcapture;
using namespace std::chrono_literals;

int main() {
    // At 60Hz, work shorter than the period waits only for the remaining time.
    WindowStreamCadence shortWork;
    assert(scheduleNextWindowStreamTick(shortWork, 0, 5'000, 60) == 11'666us);
    assert(shortWork.nextDueUs == 16'666);

    // A 50ms overrun skips three due slots rather than requesting a burst.
    WindowStreamCadence overrun;
    assert(scheduleNextWindowStreamTick(overrun, 0, 50'000, 60) == 16'664us);
    assert(overrun.nextDueUs == 66'664);

    // A non-monotonic fake clock cannot move the deadline or completion state
    // backwards.
    WindowStreamCadence monotonic;
    assert(scheduleNextWindowStreamTick(monotonic, 10'000, 11'000, 60) == 15'666us);
    assert(scheduleNextWindowStreamTick(monotonic, 5'000, 9'000, 60) == 15'666us);
    assert(monotonic.lastCompletedUs == 11'000 && monotonic.nextDueUs == 26'666);

    // A full 60Hz interval is exactly the existing integer 1e6/fps cadence.
    WindowStreamCadence sixtyHz;
    assert(scheduleNextWindowStreamTick(sixtyHz, 100'000, 100'000, 60) == 16'666us);
}
