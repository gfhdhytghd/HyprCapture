#include "shared/ocr_result.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace hyprcapture {
namespace {
using Json = nlohmann::json;

// The DOM callback parser scans an entire parent array at every object end.
// A bounded SAX pass followed by the ordinary DOM parser stays linear even at
// the 65536-result limit, while rejecting deep input before allocating a DOM.
struct DepthCheck final : nlohmann::json_sax<Json> {
    int depth = 0;
    bool null() override { return true; }
    bool boolean(bool) override { return true; }
    bool number_integer(Json::number_integer_t) override { return true; }
    bool number_unsigned(Json::number_unsigned_t) override { return true; }
    bool number_float(Json::number_float_t, const Json::string_t&) override { return true; }
    bool string(Json::string_t&) override { return true; }
    bool binary(Json::binary_t&) override { return true; }
    bool key(Json::string_t&) override { return true; }
    bool start_object(std::size_t) override { return ++depth <= 32; }
    bool start_array(std::size_t) override { return ++depth <= 32; }
    bool end_object() override { --depth; return true; }
    bool end_array() override { --depth; return true; }
    bool parse_error(std::size_t, const std::string&, const Json::exception&) override { return false; }
};
Json boundedJson(std::string_view input) {
    DepthCheck check;
    if (!Json::sax_parse(input, &check)) throw std::runtime_error("invalid JSON or nesting exceeds 32");
    return Json::parse(input);
}

template<class Result> void polygon(const Json& value, Result& result) {
    if (!value.is_array() || value.size() < 3 || value.size() > OcrMaxPoints)
        throw std::runtime_error("poly must have 3..256 points");
    for (const auto& p : value) {
        if (!p.is_array() || p.size() != 2 || !p[0].is_number() || !p[1].is_number())
            throw std::runtime_error("poly points must be [x,y] numbers");
        OcrPoint point{p[0].get<double>(), p[1].get<double>()};
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || std::abs(point.x) > 1e7 || std::abs(point.y) > 1e7)
            throw std::runtime_error("poly coordinates must be finite and within +/-10000000");
        result.poly.push_back(point);
    }
    double area = 0;
    for (std::size_t i = 0; i < result.poly.size(); ++i) {
        const auto a = result.poly[i], b = result.poly[(i + 1) % result.poly.size()];
        area += a.x * b.y - b.x * a.y;
    }
    if (area == 0) throw std::runtime_error("poly must have non-zero signed area");
    // Positive shoelace area is clockwise when image y increases downwards.
    // Preserve the engine's first vertex (text-direction start).
    if (area < 0) std::reverse(result.poly.begin() + 1, result.poly.end());
}

template<class Result> void confidence(const Json& value, Result& result) {
    if (!value.is_number()) throw std::runtime_error("conf must be a number in 0..1");
    const auto conf = value.get<double>();
    if (!std::isfinite(conf) || conf < 0 || conf > 1) throw std::runtime_error("conf must be in 0..1");
    result.conf = conf;
}

std::vector<std::string_view> split(std::string_view value, char separator) {
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    for (;;) {
        auto end = value.find(separator, start);
        fields.push_back(value.substr(start, end == value.npos ? end : end - start));
        if (end == value.npos) break;
        start = end + 1;
    }
    return fields;
}

double number(std::string_view value) {
    double out;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), out);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !std::isfinite(out))
        throw std::runtime_error("invalid TSV number");
    return out;
}

bool blank(std::string_view text) { return text.find_first_not_of(" \t\r\n") == text.npos; }

bool cjk(char32_t c) {
    return (c >= 0x2e80 && c <= 0x9fff) || (c >= 0xac00 && c <= 0xd7af) ||
           (c >= 0xf900 && c <= 0xfaff) || (c >= 0x20000 && c <= 0x323af);
}
char32_t codepoint(std::string_view s, std::size_t i) {
    const auto first = static_cast<unsigned char>(s[i]);
    if (first < 0x80) return first;
    const int length = first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
    char32_t out = first & ((1 << (7 - length)) - 1);
    for (int n = 1; n < length && i + n < s.size(); ++n)
        out = (out << 6) | (static_cast<unsigned char>(s[i + n]) & 63);
    return out;
}
bool whitespace(char32_t c) {
    return (c >= 9 && c <= 13) || c == 32 || c == 0x85 || c == 0xa0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000;
}
std::string compact(std::string_view text, std::vector<std::size_t>* offsets = nullptr) {
    std::string out;
    for (std::size_t i = 0; i < text.size();) {
        auto end = i + 1;
        while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) ++end;
        if (!whitespace(codepoint(text, i))) for (auto j = i; j < end; ++j) {
            out += text[j];
            if (offsets) offsets->push_back(j);
        }
        i = end;
    }
    return out;
}
std::string trimmed(std::string_view text) {
    std::vector<std::size_t> offsets;
    compact(text, &offsets);
    if (offsets.empty()) return {};
    return std::string(text.substr(offsets.front(), offsets.back() + 1 - offsets.front()));
}
template<class Result> void geometry(const Json& box, Result& result) {
    if (box.is_array() && box.size() == 4 && box[0].is_number()) {
        if (!std::ranges::all_of(box, [](const Json& n) { return n.is_number(); }))
            throw std::runtime_error("box must contain four numbers");
        const double x0 = box[0], y0 = box[1], x1 = box[2], y1 = box[3];
        if (x1 <= x0 || y1 <= y0) throw std::runtime_error("box must have positive dimensions");
        polygon(Json::array({{x0,y0},{x1,y0},{x1,y1},{x0,y1}}), result);
    } else polygon(box, result);
}
void wordWarning(OcrResponse& response) {
    response.warning = "OCR word boxes ignored: malformed words, unmatched text or word limit exceeded";
}
// Word metadata is optional: all failures discard only this line's enhancement.
void words(const Json& entries, OcrResult& result, OcrResponse& response, std::size_t& count) {
    try {
        if (!entries.is_array()) throw std::runtime_error("words must be an array");
        const auto available = OcrMaxWords - count;
        count += std::min(entries.size(), available);
        if (entries.size() > available) throw std::runtime_error("too many OCR words");
        for (const auto& entry : entries) {
            if (!entry.is_object() || !entry.contains("text") || !entry["text"].is_string())
                throw std::runtime_error("word requires text");
            OcrWord word;
            word.text = trimmed(entry["text"].get<std::string>());
            if (word.text.empty()) continue;
            if (entry.contains("poly")) polygon(entry["poly"], word);
            else geometry(entry.at("box"), word);
            if (entry.contains("conf")) confidence(entry["conf"], word);
            result.words.push_back(std::move(word));
        }
        if (!ocrWordRanges(result.text, result.words)) throw std::runtime_error("word text mismatch");
    } catch (...) { result.words.clear(); wordWarning(response); }
}
void paddleWords(const Json& obj, std::size_t i, std::size_t lines, OcrResult& result,
                 OcrResponse& response, std::size_t& count) {
    if (!obj.contains("text_word") && !obj.contains("text_word_boxes")) return;
    try {
        const auto& texts = obj.at("text_word");
        const auto& boxes = obj.at("text_word_boxes");
        if (!texts.is_array() || !boxes.is_array() || texts.size() != lines || boxes.size() != lines ||
            !texts[i].is_array() || !boxes[i].is_array() || texts[i].size() != boxes[i].size())
            throw std::runtime_error("word arrays must match recognition lines");
        if (texts[i].size() > OcrMaxWords - count) {
            count = OcrMaxWords;
            throw std::runtime_error("too many OCR words");
        }
        Json entries = Json::array();
        for (std::size_t j = 0; j < texts[i].size(); ++j)
            entries.push_back({{"text", texts[i][j]}, {"box", boxes[i][j]}});
        words(entries, result, response, count);
    } catch (...) { result.words.clear(); wordWarning(response); }
}
bool cjkBoundary(std::string_view a, std::string_view b) {
    if (a.empty() || b.empty()) return false;
    auto i = a.size() - 1;
    while (i && (static_cast<unsigned char>(a[i]) & 0xc0) == 0x80) --i;
    return cjk(codepoint(a, i)) && cjk(codepoint(b, 0));
}
} // namespace

std::optional<std::vector<std::pair<std::size_t, std::size_t>>> ocrWordRanges(
    std::string_view text, const std::vector<OcrWord>& words) {
    std::vector<std::size_t> offsets;
    const auto line = compact(text, &offsets);
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    std::size_t cursor = 0;
    for (const auto& word : words) {
        const auto token = compact(word.text);
        if (token.empty()) return {};
        const auto at = line.find(token, cursor);
        if (at == line.npos) return {};
        cursor = at + token.size();
        ranges.emplace_back(offsets[at], offsets[cursor - 1] + 1);
    }
    return ranges;
}

OcrResponse parseOcrJsonLines(std::string_view input) {
    if (input.size() > OcrMaxBytes) return {{}, "OCR JSON Lines exceeds 8 MiB (line 1)"};
    OcrResponse response;
    std::size_t start = 0, line = 0, wordCount = 0;
    while (start < input.size()) {
        ++line;
        const auto end = input.find('\n', start);
        const auto text = input.substr(start, end == input.npos ? end : end - start);
        try {
            if (response.results.size() >= OcrMaxResults) throw std::runtime_error("too many OCR results");
            auto obj = boundedJson(text);
            if (!obj.is_object() || !obj.contains("text") || !obj["text"].is_string() || !obj.contains("poly"))
                throw std::runtime_error("required text string and poly array");
            OcrResult result;
            result.text = obj["text"].get<std::string>();
            polygon(obj["poly"], result);
            if (obj.contains("conf")) confidence(obj["conf"], result);
            if (obj.contains("level")) {
                if (obj["level"] == "word") result.level = OcrLevel::Word;
                else if (obj["level"] != "line") throw std::runtime_error("level must be line or word");
            }
            if (result.level == OcrLevel::Line && obj.contains("words"))
                words(obj["words"], result, response, wordCount);
            response.results.push_back(std::move(result));
        } catch (const std::exception& e) {
            return {{}, "OCR JSON Lines line " + std::to_string(line) + ": " + std::string(e.what()).substr(0, 512)};
        }
        if (end == input.npos) break;
        start = end + 1;
    }
    return response;
}

OcrResponse parseTesseractTsv(std::string_view input) {
    if (input.size() > OcrMaxBytes) return {{}, "tesseract TSV exceeds 8 MiB"};
    OcrResponse response;
    const auto lines = split(input, '\n');
    constexpr std::string_view header = "level\tpage_num\tblock_num\tpar_num\tline_num\tword_num\tleft\ttop\twidth\theight\tconf\ttext";
    if (lines.empty() || (lines[0] != header && lines[0] != std::string(header) + "\r"))
        return {{}, "tesseract TSV line 1: invalid header"};
    std::map<std::array<double, 4>, std::vector<std::pair<double, OcrResult>>> groups;
    std::size_t wordCount = 0;
    for (std::size_t i = 1; i < lines.size(); ++i) {
        auto line = lines[i];
        if (i + 1 == lines.size() && line.empty()) break;
        if (line.ends_with('\r')) line.remove_suffix(1);
        try {
            const auto fields = split(line, '\t');
            if (fields.size() != 12) throw std::runtime_error("expected 12 TSV columns");
            if (number(fields[0]) != 5) continue;
            const double conf = number(fields[10]);
            if (conf == -1 || blank(fields[11])) continue;
            if (++wordCount > OcrMaxWords) throw std::runtime_error("too many OCR words");
            const double x = number(fields[6]), y = number(fields[7]), w = number(fields[8]), h = number(fields[9]);
            if (w <= 0 || h <= 0) throw std::runtime_error("word dimensions must be positive");
            OcrResult result;
            result.text = fields[11];
            result.level = OcrLevel::Word;
            confidence(conf / 100, result);
            polygon(Json::array({{x,y}, {x+w,y}, {x+w,y+h}, {x,y+h}}), result);
            std::array<double, 4> key;
            for (std::size_t j = 0; j < key.size(); ++j) {
                key[j] = number(fields[j + 1]);
                if (key[j] < 0 || std::floor(key[j]) != key[j]) throw std::runtime_error("invalid TSV line id");
            }
            const double order = number(fields[5]);
            if (order < 0 || std::floor(order) != order) throw std::runtime_error("invalid TSV word id");
            groups[key].emplace_back(order, std::move(result));
            if (groups.size() > OcrMaxResults) throw std::runtime_error("too many OCR results");
        } catch (const std::exception& e) {
            return {{}, "tesseract TSV line " + std::to_string(i + 1) + ": " + std::string(e.what()).substr(0, 512)};
        }
    }
    for (auto& [key, entries] : groups) {
        std::stable_sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        OcrResult line;
        double left = 1e7, top = 1e7, right = -1e7, bottom = -1e7, conf = 0;
        for (auto& [order, word] : entries) {
            const std::string_view previous = line.words.empty() ? std::string_view{} : line.words.back().text;
            line.text += ocrTextSeparator(previous, word.text);
            line.text += word.text;
            for (const auto p : word.poly) {
                left = std::min(left, p.x); right = std::max(right, p.x);
                top = std::min(top, p.y); bottom = std::max(bottom, p.y);
            }
            conf += *word.conf;
            line.words.push_back({std::move(word.text), std::move(word.poly), word.conf});
        }
        line.conf = conf / entries.size();
        line.poly = {{left,top},{right,top},{right,bottom},{left,bottom}};
        response.results.push_back(std::move(line));
    }
    return response;
}

OcrResponse parsePaddleOcrJson(std::string_view input) {
    if (input.size() > OcrMaxBytes) return {{}, "paddleocr JSON exceeds 8 MiB"};
    try {
        auto obj = boundedJson(input);
        for (const auto* key : {"rec_texts", "rec_polys", "rec_scores"})
            if (!obj.is_object() || !obj.contains(key) || !obj[key].is_array())
                throw std::runtime_error(std::string("required array: ") + key);
        const auto size = obj["rec_texts"].size();
        if (size > OcrMaxResults || obj["rec_polys"].size() != size || obj["rec_scores"].size() != size)
            throw std::runtime_error("recognition arrays must have equal lengths <= 65536");
        OcrResponse response;
        std::size_t wordCount = 0;
        for (std::size_t i = 0; i < size; ++i) {
            if (!obj["rec_texts"][i].is_string()) throw std::runtime_error("rec_texts entries must be strings");
            OcrResult result;
            result.text = obj["rec_texts"][i].get<std::string>();
            polygon(obj["rec_polys"][i], result);
            confidence(obj["rec_scores"][i], result);
            paddleWords(obj, i, size, result, response, wordCount);
            if (response.results.size() >= OcrMaxResults) throw std::runtime_error("too many OCR results");
            response.results.push_back(std::move(result));
        }
        return response;
    } catch (const std::exception& e) {
        return {{}, std::string("paddleocr JSON: ") + std::string(e.what()).substr(0, 512)};
    }
}

OcrResponse parseOcrApiJson(std::string_view input, bool* mismatch, bool* serviceError) {
    if (mismatch) *mismatch = false;
    if (serviceError) *serviceError = false;
    if (input.size() > OcrMaxBytes) return {{}, "OCR API response exceeds 8 MiB"};
    Json obj;
    try { obj = boundedJson(input); }
    catch (...) { return {{}, "OCR API response is not valid JSON"}; }
    try {
        if (obj.is_object() && obj.contains("errorCode")) {
            if (!obj["errorCode"].is_number_integer()) throw std::runtime_error("invalid errorCode");
            if (obj["errorCode"] != 0) {
                if (serviceError) *serviceError = true;
                auto message = obj.value("errorMsg", std::string("recognition failed"));
                message = message.substr(0, message.find_first_of("\r\n"));
                // The transport redacts credentials before applying its display
                // limit; truncating here could expose a credential prefix.
                return {{}, "OCR API: " + message};
            }
        }
        OcrResponse response;
        std::size_t wordCount = 0;
        auto append = [&](const Json& text, const Json& poly, const Json* score) {
            if (response.results.size() >= OcrMaxResults) throw std::runtime_error("too many OCR results");
            if (!text.is_string()) throw std::runtime_error("text must be a string");
            OcrResult result;
            result.text = text.get<std::string>();
            polygon(poly, result);
            if (score) confidence(*score, result);
            response.results.push_back(std::move(result));
        };
        if (obj.is_object() && obj.contains("results") && obj["results"].is_array()) {
            if (obj["results"].size() > OcrMaxResults) throw std::runtime_error("too many OCR results");
            for (const auto& entry : obj["results"]) {
                if (!entry.is_object()) throw std::runtime_error("result must be an object");
                const auto* box = entry.contains("box") ? "box" : entry.contains("poly") ? "poly" : "bbox";
                const Json* score = entry.contains("confidence") ? &entry["confidence"] : entry.contains("score") ? &entry["score"] : nullptr;
                append(entry.at("text"), entry.at(box), score);
                if (entry.contains("words")) words(entry["words"], response.results.back(), response, wordCount);
            }
        } else if (obj.is_object() && obj.contains("result") && obj["result"].is_object() &&
                   obj["result"].contains("ocrResults") && obj["result"]["ocrResults"].is_array()) {
            for (const auto& page : obj["result"]["ocrResults"]) {
                const auto& result = page.at("prunedResult");
                const auto& texts = result.at("rec_texts");
                const auto& scores = result.at("rec_scores");
                const bool rectangles = !result.contains("rec_polys");
                const auto& polys = result.at(rectangles ? "rec_boxes" : "rec_polys");
                if (!texts.is_array() || !scores.is_array() || !polys.is_array() ||
                    texts.size() != scores.size() || texts.size() != polys.size())
                    throw std::runtime_error("recognition arrays must have equal lengths");
                for (std::size_t i = 0; i < texts.size(); ++i) {
                    if (!rectangles) append(texts[i], polys[i], &scores[i]);
                    else {
                        const auto& box = polys[i];
                        if (!box.is_array() || box.size() != 4 ||
                            !std::ranges::all_of(box, [](const Json& n) { return n.is_number(); }))
                            throw std::runtime_error("rec_boxes must contain [x1,y1,x2,y2]");
                        const double x1 = box[0], y1 = box[1], x2 = box[2], y2 = box[3];
                        if (x2 <= x1 || y2 <= y1) throw std::runtime_error("box must have positive dimensions");
                        append(texts[i], Json::array({{x1,y1},{x2,y1},{x2,y2},{x1,y2}}), &scores[i]);
                    }
                    paddleWords(result, i, texts.size(), response.results.back(), response, wordCount);
                }
            }
        } else {
            if (mismatch) *mismatch = true;
            return {{}, "OCR API response has an unsupported format"};
        }
        return response;
    } catch (...) {
        // Do not echo JSON fragments: servers can reflect credentials/URLs.
        return {{}, "OCR API response contains invalid recognition data"};
    }
}

bool ocrSupportsTextLayer(const std::vector<OcrResult>& results) {
    return std::ranges::all_of(results, [](const auto& r) { return r.poly.size() >= 3; });
}

std::vector<std::vector<std::size_t>> ocrReadingRows(const std::vector<OcrResult>& results) {
    struct Item { const OcrResult* result; double x, y, top, bottom; };
    std::vector<Item> items;
    for (const auto& r : results) {
        if (r.text.empty()) continue;
        if (r.textOnly()) { items.push_back({&r, 0, 0, 0, 0}); continue; }
        double x = 0, y = 0, area = 0, top = r.poly[0].y, bottom = top;
        for (std::size_t i = 0; i < r.poly.size(); ++i) {
            const auto a = r.poly[i], b = r.poly[(i + 1) % r.poly.size()];
            const auto cross = a.x * b.y - b.x * a.y;
            area += cross; x += (a.x + b.x) * cross; y += (a.y + b.y) * cross;
            top = std::min(top, a.y); bottom = std::max(bottom, a.y);
        }
        if (area == 0) { x = r.poly[0].x; y = r.poly[0].y; }
        else { x /= 3 * area; y /= 3 * area; }
        items.push_back({&r, x, y, top, bottom});
    }
    std::stable_sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
        return a.y == b.y ? a.x < b.x : a.y < b.y;
    });
    struct Row { std::vector<Item> items; double top, bottom; };
    std::vector<Row> rows;
    for (const auto& item : items) {
        Row* target = nullptr;
        // Consecutive centroid-sorted items form rows. Only compare the last
        // row, keeping the join bounded even for 65536 separate lines.
        if (!rows.empty()) {
            auto& row = rows.back();
            const double overlap = std::min(row.bottom, item.bottom) - std::max(row.top, item.top);
            if (overlap > 0 && overlap >= .5 * std::min(row.bottom - row.top, item.bottom - item.top)) {
                target = &row;
            }
        }
        if (!target) rows.push_back({{item}, item.top, item.bottom});
        else {
            target->items.push_back(item);
            target->top = std::max(target->top, item.top);
            target->bottom = std::min(target->bottom, item.bottom);
        }
    }
    std::vector<std::vector<std::size_t>> order;
    for (auto& row : rows) {
        std::stable_sort(row.items.begin(), row.items.end(), [](const auto& a, const auto& b) { return a.x < b.x; });
        order.emplace_back();
        for (const auto& item : row.items) order.back().push_back(item.result - results.data());
    }
    return order;
}

std::string ocrTextSeparator(std::string_view previous, std::string_view next) {
    if (!previous.empty() && !cjkBoundary(previous, next) &&
        previous.find_last_of(" \t\r\n") != previous.size() - 1 && next.find_first_of(" \t\r\n") != 0)
        return " ";
    return {};
}

std::string joinOcrText(const std::vector<OcrResult>& results) {
    std::string text;
    for (const auto& row : ocrReadingRows(results)) {
        if (!text.empty()) text += '\n';
        std::string_view previous;
        for (const auto index : row) {
            const auto& next = results[index].text;
            text += ocrTextSeparator(previous, next);
            text += next;
            previous = next;
        }
    }
    return text;
}

} // namespace hyprcapture
