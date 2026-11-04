#include "shared/scroll_pacing.hpp"
#include <cstdlib>
#include <iostream>
using hyprcapture::ScrollPacing;
void require(bool value, const char *message) {
  if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
int main() {
  ScrollPacing pacing;
  // A 200ms matcher must not turn 100Hz, subpixel touchpad input into 5Hz
  // scrolling. All 20 events fit without waiting for one acknowledgement.
  for (int i=0; i<20; ++i) {
    require(pacing.allowance(1700,2,true) >= .5,"small events flow concurrently");
    pacing.delivered(i+2,.5,true);
  }
  const double before=pacing.allowance(1700,2,true);
  pacing.aligned(1,0);
  require(pacing.allowance(1700,2,true)==before,"pre-input frame cannot release debt");
  pacing.aligned(11,10);
  require(pacing.allowance(1700,2,true)>before,"ack retires only covered events");
  pacing.delivered(30,pacing.allowance(1700,2,true),true);
  require(pacing.allowance(1700,2,true)<.001,"large bursts exhaust overlap budget");
  require(pacing.allowance(1700,2,false)<.001,"sources share one displacement budget");
  require(pacing.allowance(1700,2,true,-1)>100,"reversal can recover without waiting for a failed frame");
  pacing.aligned(29,0);
  require(pacing.allowance(1700,2,true)>0,"older ack releases only earlier debt");
  pacing.aligned(30,300);
  require(pacing.allowance(1700,2,true)>0,"accepted frames permit continued motion");
  ScrollPacing wheel;
  wheel.delivered(2,15,false);
  wheel.aligned(2,600);
  require(std::abs(wheel.allowance(1700,2,false)-680./30)<.0001,"delayed rendering cannot tighten budget indefinitely");
  require(wheel.allowance(1700,2,true)==170,"wheel calibration does not throttle touchpad");
}
