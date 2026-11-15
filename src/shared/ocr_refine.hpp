#pragma once

#include "shared/ocr_result.hpp"
#include <array>
#include <cstdint>

namespace hyprcapture {

enum class OcrPixelFormat { Gray8, Rgba8 };
// Non-owning, top-down pixels of the exact engine input, in physical pixels.
struct OcrImageView {
    const std::uint8_t* data = nullptr;
    int width = 0, height = 0;
    std::size_t stride = 0;
    OcrPixelFormat format = OcrPixelFormat::Gray8;
};

// A clockwise quad in reading orientation. Tall detector crops read downwards,
// just as a 90-degree counterclockwise crop rotation does for recognition.
std::optional<std::array<OcrPoint, 4>> ocrReadingQuad(const std::vector<OcrPoint>& polygon);
bool ocrWideCharacter(char32_t c);
// Grapheme estimates in the line frame, using the Live Text 1:2 width model.
std::vector<OcrWord> estimatedOcrWords(const OcrResult& line);

// Changes only word polygons; line-only results gain estimated words only when
// pixel refinement succeeds. Invalid/ambiguous lines retain their input intact.
void refineOcrWordBoxes(OcrImageView image, OcrResponse& response);

} // namespace hyprcapture
