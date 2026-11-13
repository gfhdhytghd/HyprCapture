#include "shared/ocr_engine.hpp"
#include "shared/ocr_api.hpp"
#include <chrono>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>

// Test-only line protocol keeps URL/key out of argv and supports repeated calls
// in one helper process. Each request runs the production engine unchanged.
int main() {
    using Json = nlohmann::json;
    std::string line;
    while (std::getline(std::cin, line)) {
        try {
            const auto request = Json::parse(line);
            if (request.contains("guess")) {
                std::cout << Json{{"paddlex", hyprcapture::detail::ocrApiGuessPaddlex(request.at("guess").get<std::string>())}}.dump() << std::endl;
                continue;
            }
            hyprcapture::OcrConfig config;
            config.mode = "capture"; config.engine = "paddleocr";
            config.apiUrl = request.at("url"); config.apiKey = request.value("key", "");
            config.timeout = request.value("timeout", 5);
            std::stop_source stop;
            const auto cancel = request.value("cancelMs", 0);
            std::jthread timer([&](std::stop_token done) {
                if (!cancel) return;
                const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(cancel);
                while (!done.stop_requested() && std::chrono::steady_clock::now() < end)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                if (!done.stop_requested()) stop.request_stop();
            });
            const auto start = std::chrono::steady_clock::now();
            const auto result = hyprcapture::runOcrEngine(request.at("image").get<std::string>(), config, stop.get_token());
            timer.request_stop();
            std::cout << Json{{"error", result.error}, {"text", hyprcapture::joinOcrText(result.results)},
                {"count", result.results.size()}, {"ms", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count()}}.dump() << std::endl;
        } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    }
}
