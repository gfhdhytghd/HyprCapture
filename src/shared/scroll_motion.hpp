#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>

namespace hyprcapture {
// Capture-time totals of delivered (not merely queued) input. Separate wheel
// v120 ticks, finger distance, continuous-axis distance and legacy wheel units.
// Absolute travel detects reversals which a signed sum would hide. Totals keep
// their meaning when a PBO, socket sender or receiver drops intermediate
// frames.
struct ScrollMotionSnapshot {
  std::array<double, 4> amount{}, travel{};
  uint64_t lastInputNs = 0;
};
struct ScrollPrediction {
  bool valid = false, resolvesTies = false;
  double offset = 0, tolerance = 0;
  std::array<double, 4> input{}, gain{};
};

class ScrollMotionPrior {
public:
  ScrollPrediction begin(const ScrollMotionSnapshot *snapshot,
                         uint64_t timeNs) {
    m_prediction = {};
    if (!snapshot || !timeNs) {
      m_current.reset();
      return m_prediction;
    }
    if (m_last && timeNs <= m_lastTime)
      reset();
    m_current = *snapshot;
    m_now = timeNs;
    if (!m_last) {
      m_last = *snapshot;
      m_anchor = *snapshot;
      m_anchorTime = timeNs;
    }
    for (int i = 0; i < 4; ++i) {
      m_prediction.input[i] = snapshot->amount[i] - m_last->amount[i];
      const double travel = snapshot->travel[i] - m_last->travel[i];
      if (travel < 0 || travel + .000001 < std::abs(m_prediction.input[i]) ||
          !std::isfinite(travel)) {
        reset();
        return {};
      }
      m_epochInput[i] += m_prediction.input[i];
      m_epochTravel[i] += travel;
      m_prediction.gain[i] = gain(i);
    }
    double estimate = 0, uncertainty = 2;
    bool calibrated = true, pending = false;
    for (int i = 0; i < 4; ++i) {
      const double input = snapshot->amount[i] - m_anchor.amount[i];
      const double travel = snapshot->travel[i] - m_anchor.travel[i];
      if (travel > std::abs(input) + .001)
        calibrated = false; // rebound/mixed direction
      if (std::abs(input) < 1e-6)
        continue;
      pending = true;
      if (m_ratios[i].size() < 2) {
        calibrated = false;
        continue;
      }
      const double ratio = gain(i);
      estimate += input * ratio;
      double spread = 0;
      for (double r : m_ratios[i])
        spread = std::max(spread, std::abs(r - ratio));
      uncertainty += std::abs(input) * (spread + std::abs(ratio) * .02);
    }
    const bool inputPending = pending;
    // Easing can continue after the input-bearing frame. Velocity only offers
    // a broad prior; it is never a reason to create pixels or train a gain.
    if (!pending && m_velocitySamples >= 3 && timeNs >= snapshot->lastInputNs &&
        timeNs - snapshot->lastInputNs < 200000000 && m_anchorTime < timeNs) {
      estimate = m_velocity * double(timeNs - m_anchorTime) * 1e-9;
      uncertainty = std::max(12., std::abs(estimate) * .5);
      pending = true;
    }
    m_prediction.valid = pending && calibrated;
    m_prediction.resolvesTies = m_prediction.valid && inputPending;
    m_prediction.offset = estimate;
    m_prediction.tolerance = uncertainty;
    m_last = *snapshot;
    m_lastTime = timeNs;
    return m_prediction;
  }
  void confirm(int displacement, bool accepted, bool independent) {
    if (!m_current)
      return;
    if (!accepted || !independent)
      m_epochTrusted = false;
    // Finger flings can add nonlinear uncommanded travel. A discrete wheel
    // burst may simply animate for longer: learn its complete settled distance.
    if (m_epochTravel[1] > .0001 && displacement && std::abs(displacement) > 2 &&
        m_now > m_current->lastInputNs + 300000000)
      m_epochTrusted = false;
    if (accepted) {
      m_epochPixels += displacement;
      if (displacement && m_now > m_anchorTime) {
        const double velocity =
            displacement / (double(m_now - m_anchorTime) * 1e-9);
        if (m_velocity * velocity > 0 &&
            std::abs(velocity - m_velocity) < std::abs(m_velocity) * .5)
          ++m_velocitySamples;
        else
          m_velocitySamples = 1;
        m_velocity = velocity;
      }
      // An unchanged frame just after dispatch may precede browser rendering.
      // Do not consume that input before the matching pixels have arrived.
      if (displacement || m_now >= m_current->lastInputNs + 200000000) {
        m_anchor = *m_current;
        m_anchorTime = m_now;
      }
    }
    const bool settled = accepted && displacement == 0 &&
                         m_now >= m_current->lastInputNs + 150000000;
    if (!settled)
      return;
    int source = -1;
    bool clean = m_epochTrusted;
    for (int i = 0; i < 4; ++i)
      if (m_epochTravel[i] > .0001) {
        if (source != -1 || m_epochTravel[i] > std::abs(m_epochInput[i]) + .001)
          clean = false;
        source = i;
      }
    if (clean && source >= 0 && std::abs(m_epochPixels) >= 8 &&
        std::abs(m_epochInput[source]) > .001) {
      const double ratio = m_epochPixels / m_epochInput[source];
      if (std::isfinite(ratio) && std::abs(ratio) >= .05 &&
          std::abs(ratio) <= 4096) {
        auto &samples = m_ratios[source];
        samples.push_back(ratio);
        if (samples.size() > 5)
          samples.pop_front();
      }
    }
    m_epochInput = {};
    m_epochTravel = {};
    m_epochPixels = 0;
    m_epochTrusted = true;
  }
  void reset() { *this = ScrollMotionPrior{}; }

private:
  double gain(int source) const {
    if (m_ratios[source].empty())
      return 0;
    auto sorted = m_ratios[source];
    std::sort(sorted.begin(), sorted.end());
    return sorted[sorted.size() / 2];
  }
  std::array<std::deque<double>, 4> m_ratios;
  std::optional<ScrollMotionSnapshot> m_last, m_current;
  ScrollMotionSnapshot m_anchor;
  ScrollPrediction m_prediction;
  uint64_t m_lastTime = 0, m_anchorTime = 0, m_now = 0;
  std::array<double, 4> m_epochInput{}, m_epochTravel{};
  double m_epochPixels = 0, m_velocity = 0;
  int m_velocitySamples = 0;
  bool m_epochTrusted = true;
};
} // namespace hyprcapture
