#pragma once

#include "shared/config.hpp"
#include "shared/ocr_result.hpp"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>

namespace hyprcapture::detail {
struct OcrProcessOutput { std::string out, error; int exitCode = 0; };
using OcrApiClock = std::function<std::chrono::system_clock::time_point()>;
struct OcrApiCacheEntry {
    std::string hash, format;
    std::int64_t rejectedAt = 0;
};
struct OcrApiCacheView {
    std::vector<OcrApiCacheEntry> entries;
    std::string error;
    explicit operator bool() const { return error.empty(); }
};
std::string ocrApiUrlHash(std::string_view url);
OcrApiCacheView ocrApiCacheShow();
OcrApiCacheView ocrApiCacheClear(std::optional<std::string_view> url = {});
OcrProcessOutput ocrReadHyprlandOption(std::string_view name);
using OcrApiRunner = std::function<OcrProcessOutput(std::chrono::milliseconds, int)>;
// Uses the engine's private directory and supervised curl runner.
OcrResponse runOcrApi(const std::filesystem::path& image, const OcrConfig& config,
                      const std::filesystem::path& temporary, std::stop_token stop,
                      const OcrApiRunner& run, const OcrApiClock& now = std::chrono::system_clock::now,
                      bool ignoreCache = false);
bool ocrApiGuessPaddlex(std::string_view url);
}
