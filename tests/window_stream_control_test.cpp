#include "plugin/window_stream_control.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <string>

namespace {

std::string start(std::string extra = "\"fps\":60") {
    return "{\"id\":\"test-1\",\"mode\":\"window\",\"windowAddress\":\"0x123\",\"socketPath\":\"/tmp/hyprcapture-1/stream.sock\"" +
        (extra.empty() ? "" : "," + extra) + ",\"defaults\":{\"mode\":\"window\",\"windowBackground\":\"transparent\",\"windowBorder\":\"keep\",\"windowShadow\":\"keep\"}}";
}

void validAndDefaultFps() {
    const auto explicitRate = hyprcapture::decodeWindowStreamStartControl(start());
    assert(explicitRate && explicitRate->fps == 60);
    const auto defaultRate = hyprcapture::decodeWindowStreamStartControl(start(""));
    assert(defaultRate && defaultRate->fps == 60);
}

void selectsExplicitGpuTransportWithoutChangingCpuRequests() {
    const auto cpu = hyprcapture::decodeWindowStreamStartControl(start());
    assert(cpu && cpu->transport == hyprcapture::WindowStreamTransport::Cpu);

    auto gpuJson = nlohmann::ordered_json::parse(start());
    gpuJson["mode"] = "window-gpu";
    const auto gpu = hyprcapture::decodeWindowStreamStartControl(gpuJson.dump());
    assert(gpu && gpu->transport == hyprcapture::WindowStreamTransport::Gpu);

    gpuJson["mode"] = "auto";
    assert(!hyprcapture::decodeWindowStreamStartControl(gpuJson.dump()));
}

void rejectsMalformedTypesAndRanges() {
    for (const char* key : {"id", "mode", "windowAddress", "socketPath", "defaults"}) {
        auto json = nlohmann::ordered_json::parse(start());
        json[key] = 1;
        assert(!hyprcapture::decodeWindowStreamStartControl(json.dump()));
    }
    for (const char* key : {"mode", "windowBackground", "windowBorder", "windowShadow"}) {
        auto json = nlohmann::ordered_json::parse(start());
        json["defaults"][key] = 1;
        assert(!hyprcapture::decodeWindowStreamStartControl(json.dump()));
    }
    for (const auto& fps : {"-1", "0", "1001", "18446744073709551615", "1.5", "\"60\""}) {
        const auto decoded = hyprcapture::decodeWindowStreamStartControl(start("\"fps\":" + std::string(fps)));
        assert(!decoded);
    }
}

void rejectsDuplicatesUnknownAndInvalidIdentity() {
    assert(!hyprcapture::decodeWindowStreamStartControl(
        "{\"id\":\"test-1\",\"id\":\"other\",\"mode\":\"window\",\"windowAddress\":\"0x123\",\"socketPath\":\"/tmp/x\",\"defaults\":{\"mode\":\"window\",\"windowBackground\":\"transparent\",\"windowBorder\":\"keep\",\"windowShadow\":\"keep\"}}"));
    assert(!hyprcapture::decodeWindowStreamStartControl(
        "{\"id\":\"test-1\",\"mode\":\"window\",\"windowAddress\":\"0x123\",\"socketPath\":\"/tmp/x\",\"defaults\":{\"mode\":\"window\",\"mode\":\"window\",\"windowBackground\":\"transparent\",\"windowBorder\":\"keep\",\"windowShadow\":\"keep\"}}"));
    assert(!hyprcapture::decodeWindowStreamStartControl(start("\"extra\":1")));
    auto unknownDefault = nlohmann::ordered_json::parse(start());
    unknownDefault["defaults"]["extra"] = 1;
    assert(!hyprcapture::decodeWindowStreamStartControl(unknownDefault.dump()));
    auto badAddress = start();
    badAddress.replace(badAddress.find("0x123"), 5, "0xnope");
    assert(!hyprcapture::decodeWindowStreamStartControl(badAddress));
    auto badId = start();
    badId.replace(badId.find("test-1"), 6, "bad_id");
    assert(!hyprcapture::decodeWindowStreamStartControl(badId));
}

void validatesStopShape() {
    assert(hyprcapture::decodeWindowStreamStopControl("{\"streamId\":\"test-1\"}") == "test-1");
    assert(!hyprcapture::decodeWindowStreamStopControl("{\"streamId\":1}"));
    assert(!hyprcapture::decodeWindowStreamStopControl("{\"streamId\":\"test-1\",\"extra\":true}"));
    assert(!hyprcapture::decodeWindowStreamStopControl("{\"streamId\":\"test-1\",\"streamId\":\"other\"}"));
}

} // namespace

int main() {
    validAndDefaultFps();
    selectsExplicitGpuTransportWithoutChangingCpuRequests();
    rejectsMalformedTypesAndRanges();
    rejectsDuplicatesUnknownAndInvalidIdentity();
    validatesStopShape();
}
