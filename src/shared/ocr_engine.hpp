#pragma once

#include "shared/config.hpp"
#include "shared/ocr_result.hpp"

#include <filesystem>

namespace hyprcapture {

struct OcrCommand {
    std::vector<std::string> argv;
    std::string error;
    explicit operator bool() const { return error.empty(); }
};

// Whitespace separates arguments; single/double quotes group text; backslash
// escapes outside single quotes. No operators or variable expansion. Leading
// ~/ in each parsed argument expands HOME. Substitute {image} after splitting.
OcrCommand splitOcrCommand(std::string_view command, std::string_view image);

// Blocking worker API. Caller owns the SDR PNG and must remove it afterwards.
// mode=off is an error. Never call from the compositor/UI event thread.
OcrResponse runOcrEngine(const std::filesystem::path& png, const OcrConfig& config);

} // namespace hyprcapture
