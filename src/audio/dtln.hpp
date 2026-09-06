#pragma once
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace hyprcapture::audio::dtln {
constexpr int rate = 16000, hop = 128, window = 512;
// Stable interface shared by the CPU and optional OpenVINO modules. State is
// explicit: no hidden history may escape reset/validation.
class Network {
public:
    virtual ~Network() = default;
    virtual void invoke(int part, const float* audio, const float* reference,
                        const float* state, float* output, float* nextState) = 0;
    virtual std::string version() const = 0;
};
std::unique_ptr<Network> cpuNetwork(const std::string& directory, const std::string& library, int size);
std::unique_ptr<Network> npuNetwork(const std::string& directory, const std::string& module, int size);
class Engine {
public:
    Engine(std::unique_ptr<Network> network, int size);
    ~Engine();
    void reset();
    void process(const float* mic, const float* reference, float* output);
    const std::vector<float>& state(int part) const { return states[part]; }
    std::string version() const { return net->version(); }
private:
    struct Fft;
    std::unique_ptr<Fft> fft;
    std::unique_ptr<Network> net;
    std::array<std::vector<float>, 2> states, next;
    std::array<float, window> input{}, reference{}, overlap{}, estimated{}, block{};
    std::array<float, window / 2 + 1> micMagnitude{}, refMagnitude{}, mask{};
};
// Fixed FFmpeg resampling and 8 ms packets for the PipeWire adapter. The 16 ms
// output reservoir avoids inserting variable padding at resampler startup.
class Stream48 {
public:
    explicit Stream48(Engine& engine);
    ~Stream48();
    void process(const float* mic, const float* reference, float* output); // 384 mono samples
    static constexpr int packet = 384;
    static constexpr int delayUs = 40000; // impulse-verified: 24 ms model + 16 ms reservoir
private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
} // namespace hyprcapture::audio::dtln
