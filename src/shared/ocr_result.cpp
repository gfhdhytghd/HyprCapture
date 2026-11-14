#include "shared/ocr_result.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
#include <unicode/ubrk.h>
#include <unicode/utext.h>
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
void pairedWords(const Json& texts, const Json& boxes, OcrResult& result,
                 OcrResponse& response, std::size_t& count) {
    if (!texts.is_array() || !boxes.is_array() || texts.size() != boxes.size())
        throw std::runtime_error("word arrays must have equal lengths");
    if (texts.size() > OcrMaxWords - count) {
        count = OcrMaxWords;
        throw std::runtime_error("too many OCR words");
    }
    Json entries = Json::array();
    for (std::size_t j = 0; j < texts.size(); ++j)
        entries.push_back({{"text", texts[j]}, {"box", boxes[j]}});
    words(entries, result, response, count);
}
void rowWords(const Json& obj, OcrResult& result, OcrResponse& response, std::size_t& count) {
    if (obj.contains("words")) {
        words(obj["words"], result, response, count);
        return;
    }
    if (!obj.contains("text_word") && !obj.contains("text_word_boxes")) return;
    try {
        pairedWords(obj.at("text_word"), obj.at("text_word_boxes"), result, response, count);
    } catch (...) { result.words.clear(); wordWarning(response); }
}
void paddleWords(const Json& obj, std::size_t i, std::size_t lines, OcrResult& result,
                 OcrResponse& response, std::size_t& count) {
    if (!obj.contains("text_word") && !obj.contains("text_word_boxes")) return;
    try {
        const auto& texts = obj.at("text_word");
        const auto& boxes = obj.at("text_word_boxes");
        if (!texts.is_array() || !boxes.is_array() || texts.size() != lines || boxes.size() != lines)
            throw std::runtime_error("word arrays must match recognition lines");
        pairedWords(texts[i], boxes[i], result, response, count);
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
                rowWords(entry, response.results.back(), response, wordCount);
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

namespace {
struct Frame {
    OcrPoint u{1,0}, v{0,1};
    bool valid = false;
};
Frame frame(const OcrResult& result) {
    if (result.poly.size() < 3) return {};
    const auto a = result.poly[0], b = result.poly[1];
    const double length = std::hypot(b.x-a.x,b.y-a.y);
    if (length < 1e-6) return {};
    return {{(b.x-a.x)/length,(b.y-a.y)/length},{-(b.y-a.y)/length,(b.x-a.x)/length},true};
}
double project(OcrPoint p, OcrPoint axis) { return p.x*axis.x+p.y*axis.y; }
double angle(Frame a, Frame b) {
    return std::acos(std::clamp(project(a.u,b.u),-1.,1.))*180/std::acos(-1.);
}
struct Extent { double left=1e100,right=-1e100,top=1e100,bottom=-1e100; };
void extend(Extent& extent, const std::vector<OcrPoint>& poly, Frame axes) {
    for (const auto p : poly) {
        const double x=project(p,axes.u),y=project(p,axes.v);
        extent.left=std::min(extent.left,x); extent.right=std::max(extent.right,x);
        extent.top=std::min(extent.top,y); extent.bottom=std::max(extent.bottom,y);
    }
}
}
std::vector<std::vector<std::size_t>> ocrReadingRows(const std::vector<OcrResult>& results) {
    Frame axes;
    for (const auto& r : results) if ((axes=frame(r)).valid) break;
    struct Item { std::size_t index; double x,y,top,bottom; Frame axes; };
    std::vector<Item> items;
    for (std::size_t index=0;index<results.size();++index) {
        const auto& r=results[index];
        if (r.text.empty()) continue;
        if (r.textOnly()) { items.push_back({index,0,0,0,0,{}}); continue; }
        double x=0,y=0,area=0;
        for (std::size_t i=0;i<r.poly.size();++i) {
            const auto a=r.poly[i],b=r.poly[(i+1)%r.poly.size()];
            const double cross=a.x*b.y-b.x*a.y;
            area+=cross; x+=(a.x+b.x)*cross; y+=(a.y+b.y)*cross;
        }
        const OcrPoint center=area==0?r.poly[0]:OcrPoint{x/(3*area),y/(3*area)};
        Extent extent; extend(extent,r.poly,axes);
        items.push_back({index,project(center,axes.u),project(center,axes.v),extent.top,extent.bottom,frame(r)});
    }
    std::stable_sort(items.begin(),items.end(),[](const auto& a,const auto& b) { return a.y==b.y?a.x<b.x:a.y<b.y; });
    struct Row { std::vector<Item> items; double top,bottom; };
    std::vector<Row> rows;
    for (const auto& item : items) {
        Row* target=nullptr;
        if (!rows.empty()) {
            auto& row=rows.back();
            const auto first=row.items.front().axes;
            const bool compatible=(!first.valid&&!item.axes.valid)||(first.valid&&item.axes.valid&&angle(first,item.axes)<=10+1e-7);
            const double overlap=std::min(row.bottom,item.bottom)-std::max(row.top,item.top);
            if (compatible&&overlap>0&&overlap>=.5*std::min(row.bottom-row.top,item.bottom-item.top)) target=&row;
        }
        if (!target) rows.push_back({{item},item.top,item.bottom});
        else { target->items.push_back(item); target->top=std::max(target->top,item.top); target->bottom=std::min(target->bottom,item.bottom); }
    }
    std::vector<std::vector<std::size_t>> order;
    for (auto& row : rows) {
        std::stable_sort(row.items.begin(),row.items.end(),[](const auto& a,const auto& b) { return a.x<b.x; });
        order.emplace_back();
        for (const auto& item : row.items) order.back().push_back(item.index);
    }
    return order;
}

std::string ocrTextSeparator(std::string_view previous, std::string_view next) {
    if (!previous.empty() && !cjkBoundary(previous, next) &&
        previous.find_last_of(" \t\r\n") != previous.size() - 1 && next.find_first_of(" \t\r\n") != 0)
        return " ";
    return {};
}

namespace {
// ICU supplies UAX #29 grapheme counts without introducing a Qt dependency.
struct Graphemes {
    UBreakIterator* iterator=nullptr;
    UText utf8=UTEXT_INITIALIZER;
    Graphemes() { UErrorCode error=U_ZERO_ERROR; iterator=ubrk_open(UBRK_CHARACTER,"root",nullptr,0,&error); }
    ~Graphemes() { if (iterator) ubrk_close(iterator); utext_close(&utf8); }
    std::size_t count(std::string_view text) {
        UErrorCode error=U_ZERO_ERROR;
        utext_openUTF8(&utf8,text.data(),text.size(),&error);
        if (iterator&&U_SUCCESS(error)) {
            ubrk_setUText(iterator,&utf8,&error);
            if (U_SUCCESS(error)) {
                std::size_t count=0; ubrk_first(iterator);
                while (ubrk_next(iterator)!=UBRK_DONE) ++count;
                return count;
            }
        }
        return 0; // Missing character boundaries cannot establish monospace evidence.
    }
};
bool copyCjk(char32_t c) {
    return (c>=0x4e00&&c<=0x9fff)||(c>=0x3400&&c<=0x4dbf)||(c>=0x20000&&c<=0x2a6df)||
        (c>=0x2a700&&c<=0x2b73f)||(c>=0x2b740&&c<=0x2b81f)||(c>=0x2b820&&c<=0x2ceaf)||(c>=0xf900&&c<=0xfaff);
}
bool copyPunctuation(char32_t c) {
    if ((c>=33&&c<=47)||(c>=58&&c<=64)||(c>=91&&c<=96)||(c>=123&&c<=126)) return true;
    constexpr std::u32string_view punctuation=U"，。、；：！？（）【】《》〈〉「」『』“”‘’—…·～﹔﹕﹖﹗．｡､";
    return punctuation.find(c)!=punctuation.npos;
}
std::string wrapSeparator(std::string_view previous,std::string_view next) {
    if (previous.empty()||next.empty()) return {};
    auto last=previous.size()-1;
    while (last&&(static_cast<unsigned char>(previous[last])&0xc0)==0x80) --last;
    const auto a=codepoint(previous,last),b=codepoint(next,0);
    return !whitespace(a)&&!whitespace(b)&&!copyPunctuation(a)&&!copyPunctuation(b)&&!copyCjk(a)&&!copyCjk(b)?" ":"";
}
bool listMarker(std::string_view text) {
    // Only the prefix is relevant; do not copy or scan an entire long row.
    std::u32string prefix;
    for (std::size_t at=0;at<text.size()&&prefix.size()<64;) {
        const auto point=codepoint(text,at);
        ++at; while (at<text.size()&&(static_cast<unsigned char>(text[at])&0xc0)==0x80) ++at;
        if (prefix.empty()&&whitespace(point)) continue;
        prefix.push_back(point);
    }
    if (prefix.empty()) return false;
    const auto digit=[](char32_t c) { return (c>=U'0'&&c<=U'9')||(c>=U'０'&&c<=U'９'); };
    const auto letter=[](char32_t c) { return (c>=U'a'&&c<=U'z')||(c>=U'A'&&c<=U'Z'); };
    const auto hanNumber=[](char32_t c) {
        constexpr std::u32string_view numbers=U"一二三四五六七八九十百千万零〇两壹贰叁肆伍陆柒捌玖拾佰仟";
        return numbers.find(c)!=numbers.npos;
    };
    const auto first=prefix[0];
    if (std::u32string_view(U"-*•·+").find(first)!=std::u32string_view::npos)
        return prefix.size()==1||whitespace(prefix[1])||cjk(prefix[1]);
    if (first>=U'①'&&first<=U'⑳') return true;
    if (prefix.size()>=3&&first==U'['&&prefix[2]==U']'&&
        (prefix[1]==U' '||prefix[1]==U'x'||prefix[1]==U'X')) return true;
    if (prefix.size()>=2&&letter(first)&&
        (prefix[1]==U'.'||prefix[1]==U'．'||prefix[1]==U')'||prefix[1]==U'）')) {
        // Common dotted abbreviations (e.g., i.e.) are prose, not enumeration.
        if (prefix.size()>=4&&(prefix[1]==U'.'||prefix[1]==U'．')&&letter(prefix[2])&&prefix[3]==U'.') return false;
        return true; // OCR may omit the space, including before Latin item text.
    }
    std::size_t end=0;
    if (digit(first)) {
        while (end<prefix.size()&&digit(prefix[end])) ++end;
        if (end==prefix.size()) return false;
        if (prefix[end]==U')'||prefix[end]==U'）') return true;
        if (prefix[end]==U'.'||prefix[end]==U'．')
            return end+1==prefix.size()||!digit(prefix[end+1]); // Preserve decimal prose.
    }
    if (hanNumber(first)) {
        while (end<prefix.size()&&hanNumber(prefix[end])) ++end;
        if (end<prefix.size()&&prefix[end]==U'、') return true;
    }
    if (first==U'('||first==U'（') {
        end=1;
        if (end==prefix.size()) return false;
        const bool numeric=digit(prefix[end]),han=hanNumber(prefix[end]);
        if (!numeric&&!han) return false;
        while (end<prefix.size()&&(numeric?digit(prefix[end]):hanNumber(prefix[end]))) ++end;
        return end<prefix.size()&&prefix[end]==(first==U'('?U')':U'）');
    }
    return false;
}
bool leadInColon(std::string_view text) {
    for (std::size_t end=text.size();end;) {
        auto at=end-1;
        while (at&&(static_cast<unsigned char>(text[at])&0xc0)==0x80) --at;
        const auto point=codepoint(text,at);
        if (!whitespace(point)) return point==U':'||point==U'：';
        end=at;
    }
    return false;
}
double median(std::vector<double> values,double fallback) {
    if (values.empty()) return fallback;
    const auto middle=values.begin()+values.size()/2;
    std::nth_element(values.begin(),middle,values.end()); return *middle;
}
double variation(const std::vector<double>& values) {
    if (values.empty()) return 1e100;
    const double mean=std::accumulate(values.begin(),values.end(),0.)/values.size();
    if (mean<=1e-6) return 1e100;
    double variance=0;
    for (const auto value : values) variance+=(value-mean)*(value-mean);
    return std::sqrt(variance/values.size())/mean;
}
bool codeShape(std::string_view text) {
    const auto first=text.find_first_not_of(" \t");
    if (first==text.npos) return false;
    text.remove_prefix(first);
    if (text.find('=')!=text.npos||text.find("--")!=text.npos||text.starts_with("#")||text.starts_with("//")) return true;
    for (auto open=text.find('(');open!=text.npos;open=text.find('(',open+1))
        if (open&&text.find(')',open)!=text.npos&&
            (std::isalnum(static_cast<unsigned char>(text[open-1]))||text[open-1]=='_')) return true;
    if (text.starts_with("- ")) return true;
    for (const auto prefix : {"return ","local ","def ","class ","import ","from ","if ","for ","while ","with ","end","pass","else","then","do"})
        if (text==prefix||(text.starts_with(prefix)&&(std::string_view(prefix).ends_with(' ')||
            (text.size()>std::string_view(prefix).size()&&(whitespace(text[std::string_view(prefix).size()])||copyPunctuation(text[std::string_view(prefix).size()])))))) return true;
    if (const auto colon=text.find(':'); colon!=text.npos&&colon>0&&text.substr(0,colon).find_first_of(" \t")==text.npos) {
        auto value=text.substr(colon+1);
        const auto begin=value.find_first_not_of(" \t");
        if (begin!=value.npos) value.remove_prefix(begin);
        // An unquoted sentence after a prose label is not a key/value code shape.
        std::size_t spaces=0;
        for (std::size_t i=1;i<value.size();++i) spaces+=value[i]==' '&&value[i-1]!=' ';
        if (value.empty()||value.front()=='\"'||value.front()=='\''||spaces<=1) return true;
    }
    for (std::size_t i=1;i+1<text.size();++i)
        if (text[i]=='.'&&std::isalnum(static_cast<unsigned char>(text[i-1]))&&(std::isalpha(static_cast<unsigned char>(text[i+1]))||text[i+1]=='_')) return true;
    return false;
}
struct CopyInfo {
    Frame axes; Extent extent;
    double width=0,height=0,wordAdvance=0;
    std::size_t characters=0,nonspace=0,ascii=0,symbols=0;
    bool candidate=false;
    std::vector<double> advances;
    std::vector<double> gaps;
};
// Returns an advance only when text AND independent geometric evidence agree.
double codeAdvance(const std::vector<OcrCopyRow>& rows,const std::vector<CopyInfo>& info,
                   std::size_t begin,std::size_t end) {
    if (end-begin<2) return 0;
    std::size_t nonspace=0,ascii=0,symbols=0,wordRows=0;
    std::size_t shapes=0;
    std::vector<double> words,lines,withoutIndent;
    bool suppliedIndent=false;
    std::set<std::string_view> distinct;
    std::size_t shortRows=0,veryShortRows=0,minChars=SIZE_MAX,maxChars=0;
    const auto axes=info[begin].axes;
    if (!axes.valid) return 0;
    for (auto i=begin;i<end;++i) {
        if (!info[i].axes.valid||angle(axes,info[i].axes)>10+1e-7) return 0;
        nonspace+=info[i].nonspace; ascii+=info[i].ascii; symbols+=info[i].symbols;
        shapes+=codeShape(rows[i].text);
        words.insert(words.end(),info[i].advances.begin(),info[i].advances.end());
        wordRows+=!info[i].advances.empty();
        if (info[i].characters) lines.push_back(info[i].width/info[i].characters);
        const auto at=rows[i].text.find_first_not_of(' ');
        const auto leading=at==rows[i].text.npos?0:at;
        const auto content=info[i].characters>=leading?info[i].characters-leading:0;
        suppliedIndent|=leading>0;
        if (content) withoutIndent.push_back(info[i].width/content);
        minChars=std::min(minChars,content); maxChars=std::max(maxChars,content);
        shortRows+=content<=40; veryShortRows+=content<=12;
        distinct.insert(rows[i].text);
    }
    if (!nonspace||double(ascii)/nonspace<.85||(double(symbols)/nonspace<.08&&shapes<1)) return 0;
    double advance=0;
    if (words.size()>=3&&wordRows>=2) {
        if (variation(words)>.08+1e-9) return 0;
        advance=std::accumulate(words.begin(),words.end(),0.)/words.size();
        std::size_t gaps=0,onGrid=0;
        for (auto i=begin;i<end;++i) for (const double gap : info[i].gaps) {
            if (gap < -advance*.2) return 0;
            if (gap<advance*.2) continue;
            ++gaps; const double cells=gap/advance;
            onGrid+=std::abs(cells-std::round(cells))<=.2;
        }
        if (gaps&&double(onGrid)/gaps<.9) return 0;
    } else {
        if (lines.size()!=end-begin) return 0;
        const bool trimIndent=variation(lines)>.02+1e-9&&suppliedIndent&&withoutIndent.size()==lines.size();
        if (trimIndent) lines=withoutIndent;
        if (variation(lines)>.02+1e-9) return 0;
        advance=median(lines,0);
        double left=1e100;
        for (auto i=begin;i<end;++i) left=std::min(left,info[i].extent.left);
        std::size_t indentRows=0,onGrid=0;
        for (auto i=begin;i<end;++i) {
            const double cells=(info[i].extent.left-left)/advance;
            if (cells<.5) continue;
            ++indentRows; onGrid+=std::abs(cells-std::round(cells))<=.12;
        }
        const bool varied=distinct.size()>=3&&maxChars>=minChars*1.4;
        const bool shortEvidence=lines.size()>=3&&varied&&shortRows>=2&&veryShortRows>=1;
        const bool gridEvidence=indentRows&&onGrid==indentRows&&distinct.size()>=2;
        if ((!shortEvidence&&!gridEvidence)||(trimIndent&&!gridEvidence)) return 0;
    }
    double left=1e100;
    for (auto i=begin;i<end;++i) left=std::min(left,info[i].extent.left);
    for (auto i=begin;i<end;++i)
        if ((info[i].extent.left-left)/advance>4096) return 0;
    return advance;
}
}

OcrCopyResult ocrCopyText(const std::vector<OcrResult>& results) {
    OcrCopyResult copy;
    for (auto row : ocrReadingRows(results)) {
        OcrCopyRow rendered; rendered.results=std::move(row);
        std::string_view previous;
        for (const auto index : rendered.results) {
            const auto& text=results[index].text;
            rendered.text+=ocrTextSeparator(previous,text); rendered.text+=text; previous=text;
        }
        copy.rows.push_back(std::move(rendered));
    }
    if (copy.rows.empty()) return copy;
    Graphemes graphemes;
    std::vector<CopyInfo> info(copy.rows.size());
    std::vector<double> heights,gaps;
    constexpr std::string_view symbols=R"(=()[]{}<>;:,. _/\"'`|&*+-#$@%~!)";
    for (std::size_t i=0;i<copy.rows.size();++i) {
        const auto& row=copy.rows[i]; auto& item=info[i];
        for (const auto index : row.results) if ((item.axes=frame(results[index])).valid) break;
        if (item.axes.valid) {
            for (const auto index : row.results) extend(item.extent,results[index].poly,item.axes);
            item.width=std::max(1.,item.extent.right-item.extent.left);
            item.height=std::max(1.,item.extent.bottom-item.extent.top); heights.push_back(item.height);
        }
        item.characters=graphemes.count(row.text);
        for (std::size_t at=0;at<row.text.size();) {
            const auto c=codepoint(row.text,at);
            if (!whitespace(c)) { ++item.nonspace; item.ascii+=c<128; item.symbols+=c<128&&symbols.find(char(c))!=symbols.npos; }
            ++at; while (at<row.text.size()&&(static_cast<unsigned char>(row.text[at])&0xc0)==0x80) ++at;
        }
        item.candidate=item.nonspace&&double(item.ascii)/item.nonspace>=.85&&
            (double(item.symbols)/item.nonspace>=.08||codeShape(row.text));
        bool hasPrevious=false; double previousRight=0;
        std::vector<double> boundaryAdvances;
        for (const auto index : row.results) {
            const auto& source=results[index];
            if (source.level==OcrLevel::Word&&source.words.empty()) {
                Extent extent; extend(extent,source.poly,item.axes);
                const auto count=graphemes.count(source.text);
                if (count&&extent.right>extent.left) boundaryAdvances.push_back((extent.right-extent.left)/count);
            }
            for (const auto& word : source.words) {
                Extent extent; extend(extent,word.poly,item.axes);
                const auto count=graphemes.count(word.text);
                if (count&&extent.right>extent.left) boundaryAdvances.push_back((extent.right-extent.left)/count);
                if (count>=2&&extent.right>extent.left) item.advances.push_back((extent.right-extent.left)/count);
                if (hasPrevious) item.gaps.push_back(extent.left-previousRight);
                previousRight=extent.right; hasPrevious=true;
            }
        }
        item.wordAdvance=median(std::move(boundaryAdvances),0);
        if (item.advances.size()>=3&&variation(item.advances)>.08) item.candidate=false;
        if (!item.advances.empty()) {
            const double advance=std::accumulate(item.advances.begin(),item.advances.end(),0.)/item.advances.size();
            std::size_t measured=0,onGrid=0;
            for (const auto gap : item.gaps) {
                if (gap < -advance*.2) item.candidate=false;
                if (gap<advance*.2) continue;
                ++measured; const double cells=gap/advance;
                onGrid+=std::abs(cells-std::round(cells))<=.2;
            }
            if (measured&&double(onGrid)/measured<.9) item.candidate=false;
        }
        if (i&&item.axes.valid&&info[i-1].axes.valid&&angle(info[i-1].axes,item.axes)<=10+1e-7) {
            Extent extent; for (const auto index : row.results) extend(extent,results[index].poly,info[i-1].axes);
            const double gap=extent.top-info[i-1].extent.bottom;
            if (gap>0) gaps.push_back(gap);
        }
    }
    const double height=median(heights,1),gap=median(gaps,.4*height);
    const double paragraphGap=std::round(std::max(1.5*height,2*gap)*.5);
    auto markCode=[&](std::size_t begin,std::size_t end,double advance) {
        double left=1e100;
        for (auto i=begin;i<end;++i) left=std::min(left,info[i].extent.left);
        for (auto i=begin;i<end;++i) {
            auto& row=copy.rows[i]; row.code=true; row.prefix.clear();
            const auto indent=std::size_t(std::max(0.,std::round((info[i].extent.left-left)/advance)));
            const auto existing=row.text.find_first_not_of(' ');
            const auto spaces=existing==row.text.npos?row.text.size():existing;
            if (indent>spaces) row.prefix.assign(indent-spaces,' ');
        }
    };
    auto structuredBoundary=[&](std::size_t before) {
        const auto after=before+1;
        if (listMarker(copy.rows[after].text)||leadInColon(copy.rows[before].text)) return true;
        if (!info[before].axes.valid||!info[after].axes.valid) return false;
        const double advance=info[after].wordAdvance>0?info[after].wordAdvance:
            info[before].wordAdvance>0?info[before].wordAdvance:
            info[after].characters?info[after].width/info[after].characters:0;
        if (advance<=1e-6) return false;
        // Compare both left edges in the preceding run's u frame, not screen x.
        Extent current;
        for (const auto index : copy.rows[after].results) extend(current,results[index].poly,info[before].axes);
        return std::abs(current.left-info[before].extent.left)+1e-6>=advance;
    };
    auto paragraph=[&](std::size_t begin,std::size_t end) {
        const double advance=codeAdvance(copy.rows,info,begin,end);
        if (advance) markCode(begin,end,advance);
        else for (auto i=begin;i<end;) {
            if (!info[i].candidate) { ++i; continue; }
            auto next=i+1;
            while (next<end&&info[next].candidate) ++next;
            if (const double tokenAdvance=codeAdvance(copy.rows,info,i,next)) markCode(i,next,tokenAdvance);
            i=next;
        }
        double width=1;
        for (auto i=begin;i<end;++i) width=std::max(width,info[i].width);
        for (auto i=begin;i+1<end;++i) {
            auto& row=copy.rows[i];
            row.separator=row.code||copy.rows[i+1].code||structuredBoundary(i)||info[i].width+1e-6<.92*width?"\n":wrapSeparator(row.text,copy.rows[i+1].text);
        }
    };
    std::size_t begin=0;
    for (std::size_t i=1;i<copy.rows.size();++i) {
        const bool compatible=info[i-1].axes.valid&&info[i].axes.valid&&angle(info[i-1].axes,info[i].axes)<=10+1e-7;
        Extent current;
        if (compatible) for (const auto index : copy.rows[i].results) extend(current,results[index].poly,info[i-1].axes);
        const bool newParagraph=compatible&&current.top-info[i-1].extent.bottom+1e-6>=paragraphGap;
        if (!compatible||newParagraph) {
            paragraph(begin,i); copy.rows[i-1].separator=newParagraph?"\n\n":"\n"; begin=i;
        }
    }
    paragraph(begin,copy.rows.size());
    // Blank lines split paragraphs, but do not reset an adjacent code block's
    // indentation origin (including a one-row function header before a gap).
    for (std::size_t first=0;first<copy.rows.size();) {
        if (!copy.rows[first].code&&!info[first].candidate) { ++first; continue; }
        auto end=first+1; bool acrossParagraph=false;
        while (end<copy.rows.size()&&(copy.rows[end].code||info[end].candidate)) {
            acrossParagraph|=copy.rows[end-1].separator=="\n\n"; ++end;
        }
        if (acrossParagraph) if (const double advance=codeAdvance(copy.rows,info,first,end)) markCode(first,end,advance);
        first=end;
    }
    for (std::size_t i=0;i+1<copy.rows.size();++i)
        if ((copy.rows[i].code||copy.rows[i+1].code)&&copy.rows[i].separator!="\n\n") copy.rows[i].separator="\n";
    for (const auto& row : copy.rows) { copy.text+=row.prefix; copy.text+=row.text; copy.text+=row.separator; }
    return copy;
}

std::string joinOcrText(const std::vector<OcrResult>& results) { return ocrCopyText(results).text; }

} // namespace hyprcapture
