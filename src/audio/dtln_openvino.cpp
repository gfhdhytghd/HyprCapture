#include "audio/dtln.hpp"
#include <openvino/openvino.hpp>
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace hyprcapture::audio::dtln {
class Npu final : public Network {
    ov::Core core;
    std::array<ov::CompiledModel, 2> models;
    std::array<ov::InferRequest, 2> requests;
    int units;
public:
    Npu(const std::string& directory, int size) : units(size) {
        if (size != 256 && size != 512) throw std::runtime_error("Unsupported NPU model");
        for (int p=0; p<2; ++p) {
            auto m = core.read_model(directory + "/dtln_aec_" + std::to_string(size) + "_" + std::to_string(p+1) + ".tflite");
            // Intel NPU's final interleaving Concat duplicated half of the 512
            // state in our regression. Export h1,h2,c1,c2 and pack on the host.
            // Validate the graph shape instead of depending on node names.
            auto state = m->get_results().at(1)->input_value(0).get_node_shared_ptr();
            if (state->get_type_name() != std::string("Concat") || state->get_input_size() != 2) throw std::runtime_error("Unexpected DTLN state graph");
            ov::OutputVector outputs{m->get_results().at(0)->input_value(0)};
            for (int s=0; s<2; ++s) {
                auto unsqueeze = state->input_value(s).get_node_shared_ptr();
                auto reshape = unsqueeze->input_value(0).get_node_shared_ptr();
                auto concat = reshape->input_value(0).get_node_shared_ptr();
                if (std::string(unsqueeze->get_type_name()) != "Unsqueeze" || std::string(reshape->get_type_name()) != "Reshape" ||
                    std::string(concat->get_type_name()) != "Concat" || concat->get_input_size() != 2) throw std::runtime_error("Unexpected DTLN state packing");
                for (int layer=0; layer<2; ++layer) {
                    auto vector = concat->input_value(layer).get_node()->input_value(0);
                    if (vector.get_shape() != ov::Shape{1, size_t(size)}) throw std::runtime_error("Unexpected DTLN state vector");
                    outputs.push_back(vector);
                }
            }
            m = std::make_shared<ov::Model>(outputs, m->get_parameters());
            models[p] = core.compile_model(m, "NPU"); // Never AUTO/HETERO/GPU.
            for (const auto& device : models[p].get_property(ov::execution_devices))
                if (!device.starts_with("NPU")) throw std::runtime_error("NPU backend selected another device");
            requests[p] = models[p].create_infer_request();
        }
    }
    std::string version() const override { return "openvino-" + std::string(ov::get_openvino_version().buildNumber) + "-state-split-1"; }
    void invoke(int p, const float* audio, const float* ref, const float* state, float* output, float* nextState) override {
        auto& r = requests[p];
        const float* inputs[]{audio, state, ref};
        for (size_t i=0; i<3; ++i) {
            auto t = r.get_input_tensor(i); std::memcpy(t.data(), inputs[i], t.get_byte_size());
        }
        r.infer();
        auto out = r.get_output_tensor(0); std::memcpy(output, out.data(), out.get_byte_size());
        const float* h1 = r.get_output_tensor(1).data<float>(); const float* h2 = r.get_output_tensor(2).data<float>();
        const float* c1 = r.get_output_tensor(3).data<float>(); const float* c2 = r.get_output_tensor(4).data<float>();
        for (int i=0; i<units; ++i) {
            nextState[2*i] = h1[i]; nextState[2*i+1] = c1[i];
            nextState[2*(units+i)] = h2[i]; nextState[2*(units+i)+1] = c2[i];
        }
    }
};
}
extern "C" __attribute__((visibility("default"))) hyprcapture::audio::dtln::Network*
hyprcapture_create_npu(const char* directory, int size, char* error, size_t length) {
    try { return new hyprcapture::audio::dtln::Npu(directory, size); }
    catch (const std::exception& e) { if (length) { std::strncpy(error, e.what(), length-1); error[length-1] = 0; } return nullptr; }
}
