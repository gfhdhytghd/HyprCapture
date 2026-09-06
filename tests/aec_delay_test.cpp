#include "audio/dtln.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
using namespace hyprcapture::audio::dtln;
struct Identity:Network {void invoke(int p,const float* a,const float*,const float*,float* o,float* s)override{if(!p)std::fill_n(o,257,1);else for(int i=0;i<512;++i)o[i]=a[i]*.25;std::fill_n(s,1024,0);}std::string version()const override{return "test";}};
int main(){Engine e(std::make_unique<Identity>(),256);Stream48 stream(e);std::array<float,384> mic{},ref{},out{};std::vector<float> result;for(int b=0;b<100;++b){mic.fill(0);if(b==10)mic[0]=1;stream.process(mic.data(),ref.data(),out.data());result.insert(result.end(),out.begin(),out.end());}auto peak=std::max_element(result.begin(),result.end());std::cout<<"peak index "<<peak-result.begin()<<" delay samples "<<(peak-result.begin()-3840)<<" value "<<*peak<<'\n';return peak-result.begin()-3840 == Stream48::delayUs*48/1000 ? 0 : 1;}
