#include "shared/scroll_motion.hpp"
#include <cstdlib>
#include <iostream>
using namespace hyprcapture;
void require(bool ok, const char *message) {
  if (!ok) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
int main() {
  ScrollMotionPrior model;
  ScrollMotionSnapshot input;
  uint64_t now = 1000000000;
  model.begin(&input, now);
  model.confirm(0, true, true);
  for (int burst = 0; burst < 3; ++burst) {
    now += 300000000;
    input.amount[0] += 2;
    input.travel[0] += 2;
    input.lastInputNs = now;
    model.begin(&input, now);
    model.confirm(0, true, true); // browser has not rendered yet
    now += 80000000;
    model.begin(&input, now);
    model.confirm(120, true, true);
    now += 200000000;
    model.begin(&input, now);
    model.confirm(0, true, true);
  }
  now += 300000000;
  input.amount[0] += 5;
  input.travel[0] += 5;
  input.lastInputNs = now;
  auto p = model.begin(&input, now);
  require(p.valid && std::abs(p.offset - 300) < .001,
          "settled unique motion learns the wheel ratio despite rendering lag");
  require(p.input[0] == 5 && p.gain[0] == 60,
          "cumulative samples retain input from dropped frames");
  model.confirm(300, true, true);
  now += 10000000;
  input.amount[0] -= 1;
  input.travel[0] += 1;
  input.lastInputNs = now;
  p = model.begin(&input, now);
  require(p.valid && p.offset < 0,
          "a delivered reversal predicts the opposite direction");
  model.confirm(0, false, false);
  now += 10000000;
  input.amount[0] += 1;
  input.travel[0] += 1;
  input.lastInputNs = now;
  p = model.begin(&input, now);
  require(!p.valid,
          "net-zero rebound travel cannot appear as a confident zero prior");
  model.confirm(0, true, true);
  now += 300000000;
  p = model.begin(&input, now);
  model.confirm(0, true, true);
  now += 300000000;
  input.amount[1] += 10;
  input.travel[1] += 10;
  input.lastInputNs = now;
  p = model.begin(&input, now);
  require(!p.valid && p.gain[1] == 0,
          "wheel calibration cannot silently calibrate a touchpad");
  model.confirm(100, true,
                false); // a prior-selected match must not train itself
  now += 300000000;
  model.begin(&input, now);
  model.confirm(0, true, true);
  require(model.begin(&input, now + 1).gain[1] == 0,
          "ambiguous/prior-selected pixels never train the model");
  ScrollMotionPrior edge;
  ScrollMotionSnapshot stopped;
  edge.begin(&stopped, 1000000000);
  edge.confirm(0, true, true);
  stopped.amount[0] = 10;
  stopped.travel[0] = 10;
  stopped.lastInputNs = 1100000000;
  edge.begin(&stopped, 1100000000);
  edge.confirm(0, true, true);
  edge.begin(&stopped, 1500000000);
  edge.confirm(0, true, true);
  require(edge.begin(&stopped, 1500000001).gain[0] == 0,
          "overscroll at a page edge does not train a zero gain");
  ScrollMotionPrior delayedWheel;
  ScrollMotionSnapshot wheel;
  uint64_t tick=1000000000;
  delayedWheel.begin(&wheel,tick);delayedWheel.confirm(0,true,true);
  for(int burst=0;burst<3;++burst) {
    tick+=1000000000;wheel.amount[0]+=2;wheel.travel[0]+=2;wheel.lastInputNs=tick;
    delayedWheel.begin(&wheel,tick);delayedWheel.confirm(120,true,true);
    tick+=400000000;delayedWheel.begin(&wheel,tick);delayedWheel.confirm(80,true,true);
    tick+=200000000;delayedWheel.begin(&wheel,tick);delayedWheel.confirm(0,true,true);
  }
  tick+=1000000000;wheel.amount[0]+=2;wheel.travel[0]+=2;wheel.lastInputNs=tick;
  const auto wheelPrior=delayedWheel.begin(&wheel,tick);
  require(wheelPrior.valid && std::abs(wheelPrior.offset-200)<.001 && wheelPrior.gain[0]==100,
          "long smooth wheel animations train their complete settled distance");
  ScrollMotionPrior kinetic;
  ScrollMotionSnapshot fling;
  kinetic.begin(&fling, 1000000000);
  kinetic.confirm(0, true, true);
  fling.amount[1] = 10;
  fling.travel[1] = 10;
  fling.lastInputNs = 1100000000;
  kinetic.begin(&fling, 1100000000);
  kinetic.confirm(100, true, true);
  kinetic.begin(&fling, 1600000000);
  kinetic.confirm(300, true, true);
  kinetic.begin(&fling, 1900000000);
  kinetic.confirm(0, true, true);
  require(kinetic.begin(&fling, 1900000001).gain[1] == 0,
          "uncommanded kinetic tails cannot inflate the finger gain");
}
