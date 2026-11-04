#pragma once

#include <deque>
#include <utility>

namespace hyprcapture {
enum class ScrollInputDisposition { Deliver, Queued, Dropped, Overflow };

// Keep the pre-baseline gesture ordered until its backlog has drained. In
// particular, an incoming axis-stop must not overtake queued finger movement.
template <class Input>
ScrollInputDisposition queueScrollInput(std::deque<Input>& pending, Input input,
                                       bool released, bool draining, bool paced,
                                       bool finger) {
  if (released && paced && !draining && !pending.empty() &&
      (input.event.delta * pending.back().event.delta < 0 ||
       (input.event.delta && finger &&
        pending.back().event.source != input.event.source)))
    pending.clear();
  if (released && finger && !draining && pending.empty())
    return ScrollInputDisposition::Deliver;
  if (!paced && released && !draining)
    return ScrollInputDisposition::Deliver;
  if (pending.size() >= 256)
    return released && paced && !draining ? ScrollInputDisposition::Dropped
                              : ScrollInputDisposition::Overflow;
  // Only coalesce wheel input. Finger events retain both axes and their stop
  // boundaries while the baseline queue is draining.
  if (released && paced && !draining && !finger && !pending.empty()) {
    auto& back = pending.back();
    if (back.native == input.native && back.pointer == input.pointer &&
        back.event.axis == input.event.axis &&
        back.event.source == input.event.source &&
        back.event.relativeDirection == input.event.relativeDirection &&
        back.event.delta * input.event.delta > 0) {
      back.event.delta += input.event.delta;
      back.event.deltaDiscrete += input.event.deltaDiscrete;
      back.event.timeMs = input.event.timeMs;
      return ScrollInputDisposition::Queued;
    }
  }
  pending.push_back(std::move(input));
  return ScrollInputDisposition::Queued;
}
} // namespace hyprcapture
