#include "audio/dtln.hpp"
extern "C" {
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
}
#include <algorithm>
#include <stdexcept>

namespace hyprcapture::audio::dtln {
struct Stream48::Impl {
    Engine& engine;
    SwrContext *mic=nullptr,*ref=nullptr,*output=nullptr;
    std::array<float,512> micBuffer{},refBuffer{};
    std::array<float,2048> outputBuffer{};
    size_t inputCount=0,outputCount=768;
    static SwrContext* context(int in,int out){
        SwrContext* s=nullptr;AVChannelLayout mono=AV_CHANNEL_LAYOUT_MONO;
        if(swr_alloc_set_opts2(&s,&mono,AV_SAMPLE_FMT_FLT,out,&mono,AV_SAMPLE_FMT_FLT,in,0,nullptr)<0)throw std::runtime_error("Resampler allocation failed");
        av_opt_set_int(s,"filter_size",32,0);av_opt_set_int(s,"phase_shift",10,0);
        if(swr_init(s)<0){swr_free(&s);throw std::runtime_error("Resampler initialization failed");}return s;
    }
    explicit Impl(Engine& e):engine(e){
        try{mic=context(48000,16000);ref=context(48000,16000);output=context(16000,48000);}catch(...){swr_free(&mic);swr_free(&ref);swr_free(&output);throw;}
    }
    ~Impl(){swr_free(&mic);swr_free(&ref);swr_free(&output);}
    static int convert(SwrContext* s,const float* in,int count,float* out,int capacity){
        const uint8_t* src[]{reinterpret_cast<const uint8_t*>(in)};uint8_t* dst[]{reinterpret_cast<uint8_t*>(out)};
        int n=swr_convert(s,dst,capacity,src,count);if(n<0)throw std::runtime_error("Resampling failed");return n;
    }
};
Stream48::Stream48(Engine& e):m(std::make_unique<Impl>(e)){}
Stream48::~Stream48()=default;
void Stream48::process(const float* mic,const float* ref,float* output){
    const int n=Impl::convert(m->mic,mic,384,m->micBuffer.data()+m->inputCount,512-m->inputCount);
    if(Impl::convert(m->ref,ref,384,m->refBuffer.data()+m->inputCount,512-m->inputCount)!=n)throw std::runtime_error("AEC input clocks diverged");
    m->inputCount+=n;
    while(m->inputCount>=128){
        std::array<float,128> result{};m->engine.process(m->micBuffer.data(),m->refBuffer.data(),result.data());
        m->inputCount-=128;std::move(m->micBuffer.begin()+128,m->micBuffer.begin()+128+m->inputCount,m->micBuffer.begin());
        std::move(m->refBuffer.begin()+128,m->refBuffer.begin()+128+m->inputCount,m->refBuffer.begin());
        m->outputCount+=Impl::convert(m->output,result.data(),128,m->outputBuffer.data()+m->outputCount,2048-m->outputCount);
    }
    if(m->outputCount<384)throw std::runtime_error("AEC output reservoir underflow");
    std::copy_n(m->outputBuffer.begin(),384,output);m->outputCount-=384;
    std::move(m->outputBuffer.begin()+384,m->outputBuffer.begin()+384+m->outputCount,m->outputBuffer.begin());
}
}
