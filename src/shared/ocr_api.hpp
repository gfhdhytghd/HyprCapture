#pragma once

#include "shared/config.hpp"
#include "shared/ocr_result.hpp"
#include <chrono>
#include <filesystem>
#include <functional>
#include <stop_token>

namespace hyprcapture::detail {
struct OcrProcessOutput { std::string out, error; int exitCode = 0; };
using OcrApiRunner = std::function<OcrProcessOutput(std::chrono::milliseconds, int)>;
// Uses the engine's private directory and supervised curl runner.
OcrResponse runOcrApi(const std::filesystem::path& image, const OcrConfig& config,
                      const std::filesystem::path& temporary, std::stop_token stop,
                      const OcrApiRunner& run);
bool ocrApiGuessPaddlex(std::string_view url);
}
