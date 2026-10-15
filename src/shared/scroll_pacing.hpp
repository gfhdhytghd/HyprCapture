#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>

namespace hyprcapture {
// Wheel bursts may run ahead of stitching within a conservative overlap
// budget. Stable acknowledgements retire only input visible to that frame.
// Released touchpad events bypass this budget on the native input path.
class ScrollPacing {
public:
  double allowance(double height, double scale, bool finger, double direction = 1) const {
    double outstanding = 0;
    for (const auto &input : m_input)
      outstanding += input.amount * factor(scale, input.finger);
    return std::max(0.0, (height * .4 - std::copysign(1.0,direction) * outstanding) / factor(scale, finger));
  }
  void delivered(uint64_t sequence, double delta, bool finger) {
    const double amount = delta;
    if (!amount) return;
    m_input.push_back({sequence, amount, finger});
  }
  void aligned(uint64_t sequence, double /*offset*/) {
    while (!m_input.empty() && m_input.front().sequence <= sequence) {
      m_input.pop_front();
    }
  }
private:
  double factor(double scale, bool finger) const {
    return (finger ? 2.0 : 15.0) * scale;
  }
  struct Input { uint64_t sequence; double amount; bool finger; };
  std::deque<Input> m_input;
};
}
