#pragma once
#include <cmath>
#include <cstdint>
#include <vector>

namespace hyprcapture::audio::aec {
// Procedurally generated, redistributable validation input. No personal audio.
// Three scenarios per ten seconds: far only, near only, and double talk.
inline void fixture(std::vector<float>& mic, std::vector<float>& ref) {
    constexpr int rate=16000, count=rate*10;
    mic.resize(count); ref.resize(count);
    uint32_t seed=314159; double filtered=0;
    for (int i=0; i<count; ++i) {
        const double t=double(i)/rate;
        seed=1664525u*seed+1013904223u;
        filtered=.7*filtered+.3*(double(seed>>8)/16777216.-.5);
        ref[i]=(t<3 || t>=6) ? .3*filtered + .08*std::sin(2*3.141592653589793*237*t) : 0;
        double echo=(i>=640 ? .5*ref[i-640] : 0)+(i>=1120 ? .15*ref[i-1120] : 0);
        const double voice=std::sin(2*3.141592653589793*143*t)+.4*std::sin(2*3.141592653589793*286*t)+.2*std::sin(2*3.141592653589793*429*t);
        const double envelope=.5+.5*std::sin(2*3.141592653589793*3*t);
        mic[i]=echo+(t>=3 ? .065*voice*envelope : 0);
    }
}
}
