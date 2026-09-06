#include "audio/dtln.hpp"
#include <spa/interfaces/audio/aec.h>
#include <spa/support/plugin.h>
#include <spa/utils/names.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

namespace {
using namespace hyprcapture::audio;
constexpr size_t slots = 256, delayBlocks = 5; // 40 ms scheduling + 24 ms model history.
std::atomic<bool> unhealthy{false};
struct Block { std::array<float, dtln::Stream48::packet> mic{}, ref{}, out{}; std::atomic<uint64_t> completed{UINT64_MAX}; };
struct Impl {
    spa_handle handle{};
    spa_audio_aec aec{};
    std::unique_ptr<dtln::Engine> engine;
    std::unique_ptr<dtln::Stream48> stream;
    std::array<Block, slots> blocks;
    std::atomic<uint64_t> written{0}, consumed{0};
    std::atomic<bool> stopping{false};
    std::chrono::steady_clock::time_point lateSince{};
    std::thread thread;
    ~Impl() { stopping = true; if (thread.joinable()) thread.join(); }
};
int init(void* data, const spa_dict* args, spa_audio_info_raw* rec, spa_audio_info_raw* out, spa_audio_info_raw* play) {
    auto& impl = *static_cast<Impl*>(data);
    if (rec->rate != 48000 || out->rate != 48000 || play->rate != 48000 || rec->channels != 1 || out->channels != 1 || play->channels != 1) return -EINVAL;
    auto get = [&](const char* key) { const auto* s = spa_dict_lookup(args, key); return std::string(s ? s : ""); };
    try {
        const int size = std::stoi(get("dtln.model"));
        auto net = get("dtln.backend") == "npu" ? dtln::npuNetwork(get("dtln.directory"), get("dtln.npu-library"), size)
                                                    : dtln::cpuNetwork(get("dtln.directory"), get("dtln.cpu-library"), size);
        impl.engine = std::make_unique<dtln::Engine>(std::move(net), size);
        std::array<float, dtln::Stream48::packet> zero{}, output{};
        for (int i=0; i<250; ++i) impl.engine->process(zero.data(), zero.data(), output.data());
        impl.engine->reset();
        // Match the upstream's initial zero padding, including recurrent state.
        for (int i=0; i<3; ++i) impl.engine->process(zero.data(), zero.data(), output.data());
        unhealthy = false;
        impl.stream = std::make_unique<dtln::Stream48>(*impl.engine);
        impl.thread = std::thread([&impl] {
            auto lateSince = std::chrono::steady_clock::time_point{};
            while (!impl.stopping) {
                const auto read = impl.consumed.load(std::memory_order_relaxed);
                const auto count = impl.written.load(std::memory_order_acquire) - read;
                if (!count) { std::this_thread::sleep_for(std::chrono::microseconds(250)); continue; }
                const auto now = std::chrono::steady_clock::now();
                if (count * 8 > 100) {
                    if (lateSince == std::chrono::steady_clock::time_point{}) lateSince = now;
                    if (now - lateSince > std::chrono::seconds(1)) { unhealthy = true; break; }
                } else lateSince = {};
                auto& block = impl.blocks[read % slots];
                try { impl.stream->process(block.mic.data(), block.ref.data(), block.out.data()); }
                catch (...) { unhealthy = true; break; }
                block.completed.store(read, std::memory_order_release);
                impl.consumed.store(read+1, std::memory_order_release);
            }
        });
    } catch (...) { return -EIO; }
    return 0;
}
int run(void* data, const float* rec[], const float* play[], float* out[], uint32_t samples) {
    auto& impl = *static_cast<Impl*>(data);
    if (samples % dtln::Stream48::packet) { unhealthy = true; return -EINVAL; }
    for (uint32_t offset=0; offset<samples; offset+=dtln::Stream48::packet) {
        auto seq = impl.written.load(std::memory_order_relaxed);
        const auto backlog = seq - impl.consumed.load(std::memory_order_acquire);
        // Observe from the audio callback too: inference may never return.
        if (backlog * 8 > 100) {
            const auto now = std::chrono::steady_clock::now();
            if (impl.lateSince == std::chrono::steady_clock::time_point{}) impl.lateSince = now;
            if (now - impl.lateSince >= std::chrono::seconds(1)) unhealthy = true;
        } else impl.lateSince = {};
        if (seq - impl.consumed.load(std::memory_order_acquire) >= slots-1) { unhealthy = true; std::fill_n(out[0]+offset, dtln::Stream48::packet, 0); continue; }
        auto& input = impl.blocks[seq % slots];
        // Nonblocking bounded SPSC input. No inference or allocation on PW RT.
        std::copy_n(rec[0]+offset, dtln::Stream48::packet, input.mic.data());
        std::copy_n(play[0]+offset, dtln::Stream48::packet, input.ref.data());
        impl.written.store(seq+1, std::memory_order_release);
        if (seq >= delayBlocks) {
            auto& result = impl.blocks[(seq-delayBlocks) % slots];
            if (result.completed.load(std::memory_order_acquire) == seq-delayBlocks) {
                std::copy(result.out.begin(), result.out.end(), out[0]+offset); continue;
            }
        }
        std::fill_n(out[0]+offset, dtln::Stream48::packet, 0);
    }
    return 0;
}
const spa_audio_aec_methods methods{.version=SPA_VERSION_AUDIO_AEC_METHODS, .run=run, .init2=init};
size_t getSize(const spa_handle_factory*, const spa_dict*) { return sizeof(Impl); }
int initialize(const spa_handle_factory*, spa_handle* handle, const spa_dict*, const spa_support*, uint32_t) {
    auto* impl = new (handle) Impl;
    impl->handle.get_interface = [](spa_handle* h, const char* type, void** out) {
        if (std::strcmp(type, SPA_TYPE_INTERFACE_AUDIO_AEC)) return -ENOENT;
        *out = &reinterpret_cast<Impl*>(h)->aec; return 0;
    };
    impl->handle.clear = [](spa_handle* h) { reinterpret_cast<Impl*>(h)->~Impl(); return 0; };
    impl->aec.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_AUDIO_AEC, SPA_VERSION_AUDIO_AEC, &methods, impl);
    impl->aec.name = "HyprCapture DTLN-AEC"; impl->aec.latency = "384/48000";
    return 0;
}
const spa_handle_factory factory{SPA_VERSION_HANDLE_FACTORY, SPA_NAME_AEC, nullptr, getSize, initialize, nullptr};
}
extern "C" SPA_EXPORT int spa_handle_factory_enum(const spa_handle_factory** result, uint32_t* index) {
    if ((*index)++ != 0) return 0; *result = &factory; return 1;
}
extern "C" SPA_EXPORT bool hyprcapture_aec_unhealthy() { return unhealthy.load(); }
