#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hyprcapture {

inline constexpr std::size_t OcrMaxBytes = 8 * 1024 * 1024;
inline constexpr std::size_t OcrMaxPoints = 256;
inline constexpr std::size_t OcrMaxResults = 65536;
inline constexpr std::size_t OcrMaxWords = 262144;

struct OcrPoint { double x = 0, y = 0; };
enum class OcrLevel { Line, Word };
struct OcrWord {
    std::string text;
    std::vector<OcrPoint> poly;
    std::optional<double> conf;
};
struct OcrResult {
    std::string text;
    std::vector<OcrPoint> poly;
    std::optional<double> conf;
    OcrLevel level = OcrLevel::Line;
    std::vector<OcrWord> words;
    bool textOnly() const { return poly.empty(); }
};
struct OcrResponse {
    std::vector<OcrResult> results;
    std::string error;
    std::string warning;
    std::string apiFormat; // Set only by the HTTP transport after successful detection.
    explicit operator bool() const { return error.empty(); }
};

// Byte ranges in line text, ignoring Unicode whitespace during matching.
std::optional<std::vector<std::pair<std::size_t, std::size_t>>> ocrWordRanges(
    std::string_view text, const std::vector<OcrWord>& words);

// Atomic parsers: errors contain no partial results. JSONL errors name the line.
OcrResponse parseOcrJsonLines(std::string_view input);
OcrResponse parseTesseractTsv(std::string_view input);
OcrResponse parsePaddleOcrJson(std::string_view input);
// mismatch is true only for valid JSON with neither supported response shape.
// serviceError identifies nonzero errorCode, including on mismatch HTTP statuses.
OcrResponse parseOcrApiJson(std::string_view input, bool* mismatch = nullptr, bool* serviceError = nullptr);
bool ocrSupportsTextLayer(const std::vector<OcrResult>& results);
std::vector<std::vector<std::size_t>> ocrReadingRows(const std::vector<OcrResult>& results);
std::string ocrTextSeparator(std::string_view previous, std::string_view next);
std::string joinOcrText(const std::vector<OcrResult>& results);

} // namespace hyprcapture
