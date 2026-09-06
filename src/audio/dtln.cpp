#include "audio/dtln.hpp"
#include <fftw3.h>
#include <dlfcn.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace hyprcapture::audio::dtln {
namespace {
// TensorFlow Lite's stable C ABI. Loading only here keeps this optional runtime
// out of the compositor and UI, and needs no TensorFlow headers at user build time.
struct TfLiteModel; struct TfLiteInterpreter; struct TfLiteInterpreterOptions; struct TfLiteTensor;
class Cpu final : public Network {
    void* library = nullptr;
    using Model = const TfLiteModel*;
    Model models[2]{};
    TfLiteInterpreter* interpreters[2]{};
    template<class T> T symbol(const char* name) {
        auto result = reinterpret_cast<T>(dlsym(library, name));
        if (!result) throw std::runtime_error(std::string("TensorFlow Lite symbol missing: ") + name);
        return result;
    }
    const char* (*versionFn)() = nullptr;
    Model (*modelCreate)(const char*) = nullptr;
    void (*modelDelete)(Model) = nullptr;
    TfLiteInterpreterOptions* (*optionsCreate)() = nullptr;
    void (*optionsDelete)(TfLiteInterpreterOptions*) = nullptr;
    void (*threads)(TfLiteInterpreterOptions*, int) = nullptr;
    TfLiteInterpreter* (*create)(Model, const TfLiteInterpreterOptions*) = nullptr;
    void (*destroy)(TfLiteInterpreter*) = nullptr;
    int (*allocate)(TfLiteInterpreter*) = nullptr;
    int (*run)(TfLiteInterpreter*) = nullptr;
    TfLiteTensor* (*input)(TfLiteInterpreter*, int) = nullptr;
    const TfLiteTensor* (*output)(const TfLiteInterpreter*, int) = nullptr;
    size_t (*bytes)(const TfLiteTensor*) = nullptr;
    int (*put)(TfLiteTensor*, const void*, size_t) = nullptr;
    int (*get)(const TfLiteTensor*, void*, size_t) = nullptr;
    int units;
    void clear() {
        for (auto* i : interpreters) if (i && destroy) destroy(i);
        for (auto* m : models) if (m && modelDelete) modelDelete(m);
        if (library) dlclose(library);
    }
public:
    Cpu(const std::string& directory, const std::string& path, int size) : units(size) {
        try {
            library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (!library) throw std::runtime_error(std::string("CPU runtime unavailable: ") + dlerror());
#define LOAD(member, name) member = symbol<decltype(member)>(name)
            LOAD(versionFn, "TfLiteVersion"); LOAD(modelCreate, "TfLiteModelCreateFromFile"); LOAD(modelDelete, "TfLiteModelDelete");
            LOAD(optionsCreate, "TfLiteInterpreterOptionsCreate"); LOAD(optionsDelete, "TfLiteInterpreterOptionsDelete");
            LOAD(threads, "TfLiteInterpreterOptionsSetNumThreads"); LOAD(create, "TfLiteInterpreterCreate");
            LOAD(destroy, "TfLiteInterpreterDelete"); LOAD(allocate, "TfLiteInterpreterAllocateTensors");
            LOAD(run, "TfLiteInterpreterInvoke"); LOAD(input, "TfLiteInterpreterGetInputTensor"); LOAD(output, "TfLiteInterpreterGetOutputTensor");
            LOAD(bytes, "TfLiteTensorByteSize"); LOAD(put, "TfLiteTensorCopyFromBuffer"); LOAD(get, "TfLiteTensorCopyToBuffer");
#undef LOAD
            if (std::string(versionFn()) != "2.14.0") throw std::runtime_error("CPU runtime must be TensorFlow Lite 2.14.0");
            for (int p = 0; p < 2; ++p) {
                const auto file = directory + "/dtln_aec_" + std::to_string(size) + "_" + std::to_string(p+1) + ".tflite";
                models[p] = modelCreate(file.c_str());
                if (!models[p]) throw std::runtime_error("Cannot load DTLN model");
                auto* options = optionsCreate(); threads(options, 1);
                interpreters[p] = create(models[p], options); optionsDelete(options);
                if (!interpreters[p] || allocate(interpreters[p]) != 0) throw std::runtime_error("Cannot allocate DTLN tensors");
                const size_t audioBytes = (p == 0 ? 257 : 512) * sizeof(float);
                const size_t stateBytes = units * 4 * sizeof(float);
                if (bytes(input(interpreters[p], 0)) != audioBytes || bytes(input(interpreters[p], 2)) != audioBytes ||
                    bytes(input(interpreters[p], 1)) != stateBytes || bytes(output(interpreters[p], 0)) != audioBytes ||
                    bytes(output(interpreters[p], 1)) != stateBytes) throw std::runtime_error("Unexpected DTLN tensor layout");
            }
        } catch (...) { clear(); throw; }
    }
    ~Cpu() override { clear(); }
    std::string version() const override { return "tflite-" + std::string(versionFn()) + "-xnnpack-1"; }
    void invoke(int p, const float* audio, const float* ref, const float* state, float* result, float* nextState) override {
        auto* i = interpreters[p]; const size_t n = (p ? 512 : 257) * sizeof(float), s = units * 4 * sizeof(float);
        if (put(input(i, 0), audio, n) || put(input(i, 1), state, s) || put(input(i, 2), ref, n) || run(i) ||
            get(output(i, 0), result, n) || get(output(i, 1), nextState, s)) throw std::runtime_error("DTLN inference failed");
    }
};
}
std::unique_ptr<Network> cpuNetwork(const std::string& dir, const std::string& lib, int size) {
    if (size != 256 && size != 512) throw std::runtime_error("Only DTLN 256 and 512 are supported");
    return std::make_unique<Cpu>(dir, lib, size);
}
std::unique_ptr<Network> npuNetwork(const std::string& dir, const std::string& module, int size) {
    // Keep the module loaded until process exit; the returned C++ object owns
    // OpenVINO objects and must never outlive its code.
    static void* handle = dlopen(module.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) throw std::runtime_error("Experimental OpenVINO backend unavailable");
    using Factory = Network* (*)(const char*, int, char*, size_t);
    auto create = reinterpret_cast<Factory>(dlsym(handle, "hyprcapture_create_npu"));
    if (!create) throw std::runtime_error("Incompatible NPU backend");
    char error[1024]{}; auto* result = create(dir.c_str(), size, error, sizeof(error));
    if (!result) throw std::runtime_error(error);
    return std::unique_ptr<Network>(result);
}
struct Engine::Fft {
    double in[window]{};
    fftw_complex mic[window / 2 + 1]{}, ref[window / 2 + 1]{};
    fftw_plan micPlan = fftw_plan_dft_r2c_1d(window, in, mic, FFTW_ESTIMATE);
    fftw_plan refPlan = fftw_plan_dft_r2c_1d(window, in, ref, FFTW_ESTIMATE);
    fftw_plan inverse = fftw_plan_dft_c2r_1d(window, mic, in, FFTW_ESTIMATE);
    ~Fft() { fftw_destroy_plan(micPlan); fftw_destroy_plan(refPlan); fftw_destroy_plan(inverse); }
};
Engine::Engine(std::unique_ptr<Network> network, int size) : fft(std::make_unique<Fft>()), net(std::move(network)) {
    for (int p=0; p<2; ++p) { states[p].resize(4 * size); next[p].resize(4 * size); }
}
Engine::~Engine() = default;
void Engine::reset() {
    for (auto& v : states) std::fill(v.begin(), v.end(), 0);
    input.fill(0); reference.fill(0); overlap.fill(0);
}
void Engine::process(const float* mic, const float* ref, float* result) {
    std::move(input.begin()+hop, input.end(), input.begin()); std::copy_n(mic, hop, input.end()-hop);
    std::move(reference.begin()+hop, reference.end(), reference.begin()); std::copy_n(ref, hop, reference.end()-hop);
    std::copy(input.begin(), input.end(), fft->in); fftw_execute(fft->micPlan);
    std::copy(reference.begin(), reference.end(), fft->in); fftw_execute(fft->refPlan);
    for (int i=0; i<=window/2; ++i) {
        // Match the reference's complex64 FFT and float32 magnitude tensors.
        const float re = fft->mic[i][0], im = fft->mic[i][1];
        fft->mic[i][0] = re; fft->mic[i][1] = im;
        micMagnitude[i] = std::hypot(re, im);
        refMagnitude[i] = std::hypot(float(fft->ref[i][0]), float(fft->ref[i][1]));
    }
    net->invoke(0, micMagnitude.data(), refMagnitude.data(), states[0].data(), mask.data(), next[0].data());
    states[0].swap(next[0]);
    for (int i=0; i<=window/2; ++i) { fft->mic[i][0] = float(float(fft->mic[i][0]) * mask[i]); fft->mic[i][1] = float(float(fft->mic[i][1]) * mask[i]); }
    fftw_execute(fft->inverse);
    for (int i=0; i<window; ++i) estimated[i] = fft->in[i] / window;
    net->invoke(1, estimated.data(), reference.data(), states[1].data(), block.data(), next[1].data());
    states[1].swap(next[1]);
    std::move(overlap.begin()+hop, overlap.end(), overlap.begin()); std::fill(overlap.end()-hop, overlap.end(), 0);
    for (int i=0; i<window; ++i) {
        if (!std::isfinite(block[i])) throw std::runtime_error("Non-finite DTLN output");
        overlap[i] += block[i];
    }
    std::copy_n(overlap.begin(), hop, result);
}
}
