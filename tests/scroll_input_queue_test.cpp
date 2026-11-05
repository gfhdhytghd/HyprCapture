#include "shared/scroll_input_queue.hpp"
#include <cstdlib>
#include <iostream>
#include <vector>

using namespace hyprcapture;
namespace {
struct Input {
  struct Event {
    double delta = 0;
    int deltaDiscrete = 0, axis = 0, source = 1, relativeDirection = 0;
    unsigned timeMs = 0;
  } event;
  bool native = true;
  int pointer = 1;
};
void require(bool condition, const char* message) {
  if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
Input finger(unsigned time, double delta, int axis = 0) {
  return {.event = {.delta = delta, .axis = axis, .timeMs = time}};
}
}
int main() {
  std::deque<Input> pending;
  // Before release, even a negotiated pacing flag must not discard a
  // reversal, source switch, or either axis-stop from the initial gesture.
  for (bool paced : {false, true}) {
    auto wheel = finger(1, 15);
    wheel.event.source = 0;
    wheel.event.deltaDiscrete = 120;
    require(queueScrollInput(pending, wheel, false, false, paced, false) ==
                ScrollInputDisposition::Queued, "pre-baseline wheel must wait");
    for (const auto input : {finger(2, 3), finger(3, -3), finger(4, 0), finger(5, 0, 1)})
      require(queueScrollInput(pending, input, false, false, paced, true) ==
                  ScrollInputDisposition::Queued, "pre-baseline finger input must wait");
    require(pending.size() == 5, "pre-baseline changes must retain every event");
    for (unsigned i = 0; i < pending.size(); ++i)
      require(pending[i].event.timeMs == i + 1, "pre-baseline event order must be preserved");
    require(pending[3].event.axis == 0 && pending[4].event.axis == 1,
            "pre-baseline axis-stops must remain separate");
    pending.clear();
  }
  // A region waits for its first image before forwarding the gesture.
  for (unsigned time = 1; time <= 20; ++time)
    require(queueScrollInput(pending, finger(time, 2), false, false, false, true) ==
                ScrollInputDisposition::Queued, "baseline input must wait");
  std::vector<unsigned> delivered;
  for (int i = 0; i < 8; ++i) {
    delivered.push_back(pending.front().event.timeMs);
    pending.pop_front();
  }
  // Baseline was acknowledged, but more than one batch remains. Reversals
  // and both axis-stops must follow all older input, without clearing it.
  for (const auto input : {finger(21, -3), finger(22, 0), finger(23, 0, 1)})
    require(queueScrollInput(pending, input, true, true, true, true) ==
                ScrollInputDisposition::Queued, "new finger input must not overtake backlog");
  require(pending.size() == 15, "reversal and stops must preserve baseline backlog");
  require(pending[pending.size()-2].event.axis == 0 && pending.back().event.axis == 1,
          "both axis-stop boundaries must be retained");
  while (!pending.empty()) {
    delivered.push_back(pending.front().event.timeMs);
    pending.pop_front();
  }
  for (unsigned i = 0; i < delivered.size(); ++i)
    require(delivered[i] == i + 1, "gesture delivery must remain chronological");
  require(queueScrollInput(pending, finger(24, 1), true, false, true, true) ==
              ScrollInputDisposition::Deliver, "finger input resumes native cadence after draining");

  // Paced wheel coalescing remains available outside the startup drain.
  auto wheel = finger(25, 15); wheel.event.source = 0; wheel.event.deltaDiscrete = 120;
  queueScrollInput(pending, wheel, true, false, true, false);
  wheel.event.timeMs = 26;
  queueScrollInput(pending, wheel, true, false, true, false);
  require(pending.size() == 1 && pending.front().event.delta == 30 &&
              pending.front().event.deltaDiscrete == 240, "wheel events still coalesce");
  require(queueScrollInput(pending, finger(27, 0), true, false, true, true) ==
              ScrollInputDisposition::Queued, "a stop must not overtake an outstanding wheel event");
  require(pending.size() == 2, "stop must retain its own queue entry");

  pending.clear();
  for (unsigned time = 0; time < 256; ++time)
    queueScrollInput(pending, finger(time, 1), true, true, true, true);
  require(queueScrollInput(pending, finger(257, 0), true, true, true, true) ==
              ScrollInputDisposition::Overflow, "startup overflow must not silently discard an axis-stop");
}
