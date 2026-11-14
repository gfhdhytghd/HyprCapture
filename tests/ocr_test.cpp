#include "shared/ocr_engine.hpp"
#include "shared/ocr_api.hpp"
#include "shared/ocr_warning.hpp"
#include "shared/protocol.hpp"
#include "shared/trusted_path.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using namespace hyprcapture;
using Json = nlohmann::json;
namespace fs = std::filesystem;
namespace {
int checks = 0;
void check(bool value, const std::string& message) {
    ++checks;
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
template<class T> void success(const T& value, const std::string& message) {
    check(static_cast<bool>(value), message + ": " + value.error);
}
struct Temp {
    fs::path path;
    Temp() {
        auto pattern = (fs::current_path() / "ocr-fixture-XXXXXX").string();
        check(mkdtemp(pattern.data()) != nullptr, "create fixture in build dir");
        path = pattern;
    }
    ~Temp() { std::error_code ec; fs::remove_all(path, ec); }
};
struct Env {
    std::string key;
    std::optional<std::string> previous;
    Env(std::string key, const std::string& value) : key(std::move(key)) {
        if (const char* old = std::getenv(this->key.c_str())) previous = old;
        setenv(this->key.c_str(), value.c_str(), 1);
    }
    ~Env() { if (previous) setenv(key.c_str(), previous->c_str(), 1); else unsetenv(key.c_str()); }
};
void write(const fs::path& path, std::string_view value, bool script = false) {
    std::ofstream output(path); output << value; output.close();
    check(output.good(), "write " + path.string());
    check(chmod(path.c_str(), script ? 0700 : 0600) == 0, "fixture permissions");
}
std::string read(const fs::path& path) {
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), {}};
}
Json quad(double x = 0, double y = 0, double w = 20, double h = 10) {
    return Json::array({{x,y}, {x+w,y}, {x+w,y+h}, {x,y+h}});
}
Json item(std::string text = "Hello", double x = 0, double y = 0) {
    return {{"text", text}, {"poly", quad(x, y)}};
}
const std::string tsv =
    "level\tpage_num\tblock_num\tpar_num\tline_num\tword_num\tleft\ttop\twidth\theight\tconf\ttext\n"
    "1\t1\t0\t0\t0\t0\t0\t0\t100\t100\t-1\t\n"
    "5\t1\t1\t1\t1\t2\t30\t10\t20\t10\t92.5\tworld\n"
    "5\t1\t1\t1\t1\t1\t0\t10\t20\t10\t100\tHello\n"
    "5\t1\t1\t1\t2\t1\t0\t40\t20\t10\t88\t中\n"
    "5\t1\t1\t1\t2\t2\t22\t41\t20\t10\t88\t文\n"
    "5\t1\t1\t1\t3\t1\t0\t60\t20\t10\t-1\tskip\n"
    "5\t1\t1\t1\t3\t2\t0\t60\t20\t10\t90\t \n";
Json paddle() {
    return {{"rec_texts", {"中文", "Rotated"}},
            {"rec_polys", Json::array({quad(0, 0), Json::array({{30,30}, {25,40}, {45,50}, {50,40}})})},
            {"rec_scores", {.99, .75}}, {"ignored", true}};
}
OcrConfig config(std::string engine = "command") {
    OcrConfig c; c.mode = "capture"; c.engine = std::move(engine); return c;
}
void parser() {
    auto obj = item("中文"); obj["conf"] = .99; obj["ignored"] = {1,2,3};
    auto r = parseOcrJsonLines(obj.dump() + "\n" + item("next").dump() + "\n");
    success(r, "valid JSONL"); check(r.results.size() == 2, "two records");
    check(r.results[0].text == "中文" && r.results[0].conf == .99 && r.results[0].level == OcrLevel::Line, "fields/defaults");
    obj["poly"] = Json::array({{0,0}, {0,10}, {20,10}, {20,0}});
    obj["level"] = "word";
    r = parseOcrJsonLines(obj.dump()); success(r, "reverse winding");
    check(r.results[0].poly[0].x == 0 && r.results[0].poly[1].x == 20 && r.results[0].level == OcrLevel::Word, "clockwise and first vertex");
    for (const auto& bad : {Json(nullptr), Json::array(), Json{{"poly", quad()}}, Json{{"text", 42}, {"poly", quad()}},
                           Json{{"text", "x"}, {"poly", Json::array()}}, Json{{"text", "x"}, {"poly", {{0,0}, {1,1}, {2,2}}}},
                           Json{{"text", "x"}, {"poly", {{0,0}, {1}, {2,2}}}}}) {
        const auto invalid = parseOcrJsonLines(item().dump() + "\n" + bad.dump());
        check(!invalid && invalid.results.empty() && invalid.error.find("line 2") != std::string::npos, "atomic malformed record with line");
    }
    for (const auto& field : {"conf", "level"}) {
        obj = item(); obj[field] = "invalid";
        check(!parseOcrJsonLines(obj.dump()), "invalid optional field");
        obj[field] = nullptr; check(!parseOcrJsonLines(obj.dump()), "null optional field");
    }
    for (double conf : {-1., 1.01}) { obj = item(); obj["conf"] = conf; check(!parseOcrJsonLines(obj.dump()), "confidence bounds"); }
    obj = item(); obj["poly"][0][0] = 1e8; check(!parseOcrJsonLines(obj.dump()), "coordinate bounds");
    obj = item(); obj["poly"] = Json::array();
    for (std::size_t i = 0; i <= OcrMaxPoints; ++i) obj["poly"].push_back({i, i % 2});
    check(!parseOcrJsonLines(obj.dump()), "point count bound");
    check(!parseOcrJsonLines(std::string(OcrMaxBytes + 1, ' ')), "input cap");
    std::string many;
    for (std::size_t i = 0; i <= OcrMaxResults; ++i) many += item().dump() + '\n';
    check(!parseOcrJsonLines(many), "result count cap");
    check(!parseOcrJsonLines("\n") && !parseOcrJsonLines("{"), "blank/malformed line");
    check(!parseOcrJsonLines("{\"text\":\"x\",\"poly\":[[1e999,0],[1,0],[0,1]]}"), "nonfinite number");
    std::string nested(40, '['); nested += '0'; nested += std::string(40, ']');
    check(!parseOcrJsonLines("{\"text\":\"x\",\"poly\":[],\"unknown\":" + nested + "}"), "nesting cap");
    success(parseOcrJsonLines(""), "empty OCR output");
    obj = item("中文 OCR");
    obj["words"] = {{{"text", "中"}, {"poly", {{0,0},{0,10},{5,10},{5,0}}}, {"conf", .8}},
                    {{"text", "文"}, {"box", {5,0,10,10}}},
                    {{"text", " OCR "}, {"box", {12,0,20,10}}}, {{"text", "　 \t"}}};
    r = parseOcrJsonLines(obj.dump() + "\r\n"); success(r, "words CRLF poly and box");
    check(r.warning.empty() && r.results[0].words.size() == 3, "blank words skipped");
    check(r.results[0].words[0].conf == .8 && !r.results[0].words[1].conf &&
          r.results[0].words[0].poly[1].x == 5 && r.results[0].words[2].text == "OCR", "word winding confidence and trim");
    for (const auto& bad : {Json{{"text", "absent"}, {"box", {0,0,10,10}}},
                           Json{{"text", "中"}}, Json{{"text", "中"}, {"box", {0,0,0,10}}},
                           Json{{"text", "中"}, {"box", {0,0,10,10}}, {"conf", 1.1}},
                           Json{{"text", "中"}, {"poly", {{0,0},{1e8,0},{0,1}}}},
                           Json{{"text", 3}}, Json{{"text", "中"}, {"poly", {{0,0},{1,1},{2,2}}}}}) {
        auto copy = obj; copy["words"].push_back(bad);
        r = parseOcrJsonLines(copy.dump() + "\n" + copy.dump());
        check(r && r.results.size() == 2 && r.results[0].words.empty() && !r.warning.empty(), "malformed words soft drop one response warning");
    }
    auto tooManyPoints = obj;
    tooManyPoints["words"][0]["poly"] = Json::array();
    for (std::size_t i = 0; i <= OcrMaxPoints; ++i) tooManyPoints["words"][0]["poly"].push_back({i,i%2});
    r = parseOcrJsonLines(tooManyPoints.dump());
    check(r && r.results[0].words.empty() && !r.warning.empty(), "word point count limit soft failure");
    auto spaced = item("中　文 OCR");
    spaced["words"] = {{{"text", "中 文"}, {"box", {0,0,10,10}}}, {{"text", "OCR"}, {"box", {10,0,20,10}}}};
    r = parseOcrJsonLines(spaced.dump());
    check(r && r.results[0].words.size() == 2 && r.warning.empty(), "word matching ignores Unicode whitespace");
    auto copyWords = obj; std::swap(copyWords["words"][0], copyWords["words"][1]);
    r = parseOcrJsonLines(copyWords.dump()); check(r && r.results[0].words.empty() && !r.warning.empty(), "word reading order mismatch");
    copyWords = obj; copyWords["level"] = "word"; copyWords["words"] = false;
    r = parseOcrJsonLines(copyWords.dump()); check(r && r.results[0].words.empty() && r.warning.empty(), "level word ignores words");
    copyWords = obj; copyWords["words"] = false;
    r = parseOcrJsonLines(copyWords.dump()); check(r && !r.warning.empty(), "nonarray words soft failure");
    copyWords = item("中"); copyWords["words"] = Json::array();
    for (std::size_t i = 0; i < OcrMaxWords; ++i) copyWords["words"].push_back({{"text", " "}});
    auto tiny = item("中"); tiny["words"] = {{{"text", "中"}, {"box", {0,0,10,10}}}};
    const auto limitInput = copyWords.dump();
    r = parseOcrJsonLines(limitInput); check(r && r.warning.empty(), "word count cap inclusive");
    r = parseOcrJsonLines(limitInput + "\n" + tiny.dump());
    check(r && r.results[1].words.empty() && !r.warning.empty(), "word count cap across lines");
    copyWords["words"].push_back({{"text", " "}});
    r = parseOcrJsonLines(copyWords.dump()); check(r && !r.warning.empty(), "single line word limit");
    // Preserve the pre-grouping copy text for CJK, Latin and punctuation words.
    const std::vector<std::string> mixedTokens{"中", "文", "OCR", "123", ",", "测", "试", "。", "next", "word", "!", "中", "A", "文"};
    std::string mixedTsv = tsv.substr(0, tsv.find('\n') + 1);
    std::vector<OcrResult> legacyWords;
    for (std::size_t i = 0; i < mixedTokens.size(); ++i) {
        const double x = i * 25;
        mixedTsv += "5\t1\t1\t1\t1\t" + std::to_string(i + 1) + "\t" + std::to_string(i * 25) +
            "\t0\t20\t10\t95\t" + mixedTokens[i] + "\n";
        legacyWords.push_back({mixedTokens[i], {{x,0},{x+20,0},{x+20,10},{x,10}}, .95, OcrLevel::Word});
    }
    r = parseTesseractTsv(mixedTsv); success(r, "mixed CJK/Latin/punctuation TSV");
    const std::string mixedExpected = "中文 OCR 123 , 测试。 next word ! 中 A 文";
    check(joinOcrText(r.results) == mixedExpected, "TSV mixed copy text preserves separators: got " + joinOcrText(r.results));
    check(joinOcrText(r.results) == joinOcrText(legacyWords), "grouped TSV copy text matches legacy word records");
    check(r.results.size() == 1 && r.results[0].words.size() == mixedTokens.size(), "mixed TSV retains all words in one line");
    const auto& mixedLine = r.results[0];
    const auto ranges = ocrWordRanges(mixedLine.text, mixedLine.words);
    check(ranges && ranges->size() == mixedTokens.size(), "joined TSV words match line text in order");
    Json nestedWords = Json::array();
    for (std::size_t i = 0; i < mixedTokens.size(); ++i) {
        const auto [begin, end] = (*ranges)[i];
        check(mixedLine.text.substr(begin, end - begin) == mixedTokens[i], "joined TSV word range preserves token text");
        nestedWords.push_back({{"text", mixedTokens[i]}, {"poly", quad(i * 25,0)}});
    }
    auto mixedRecord = item(mixedLine.text);
    mixedRecord["poly"] = quad(0,0,(mixedTokens.size() - 1) * 25 + 20);
    mixedRecord["words"] = std::move(nestedWords);
    const auto validated = parseOcrJsonLines(mixedRecord.dump());
    check(validated && validated.warning.empty() && validated.results[0].words.size() == mixedTokens.size(),
          "joined TSV text passes word-in-line parser validation");
    r = parseTesseractTsv(tsv); success(r, "TSV fixture");
    check(r.results.size() == 2 && r.results[0].level == OcrLevel::Line && r.results[0].words.size() == 2 && r.results[0].words[0].conf == 1 && r.results[0].words[1].conf == .925, "TSV fields and skips");
    check(joinOcrText(r.results) == "Hello world\n中文", "TSV copy text unchanged from legacy words");
    check(r.results[1].poly[2].x == 42 && r.results[1].poly[2].y == 51, "TSV line union");
    check(!parseTesseractTsv("not TSV"), "TSV header");
    check(!parseTesseractTsv(tsv + "5\tbad\n"), "TSV columns");
    auto invalidTsv = tsv; invalidTsv.replace(invalidTsv.find("92.5"), 4, "101");
    check(!parseTesseractTsv(invalidTsv), "TSV confidence");
    r = parsePaddleOcrJson(paddle().dump()); success(r, "Paddle fixture");
    check(r.results[1].level == OcrLevel::Line && r.results[1].poly[1].x == 50, "rotated quad normalized");
    auto p = paddle(); p["rec_scores"] = {1}; check(!parsePaddleOcrJson(p.dump()), "Paddle array length");
    p = paddle(); p["rec_scores"][0] = 2; check(!parsePaddleOcrJson(p.dump()), "Paddle confidence");
    p = paddle(); p["rec_polys"][0] = Json::array(); check(!parsePaddleOcrJson(p.dump()), "Paddle polygon");
    p = paddle(); p.erase("rec_texts"); check(!parsePaddleOcrJson(p.dump()), "Paddle required arrays");
    p = paddle();
    p["text_word"] = {{"中", "文"}, {"Rotated"}};
    p["text_word_boxes"] = {{{0,0,10,10},{10,0,20,10}}, {{{50,50},{60,60},{50,70},{40,60}}}};
    r = parsePaddleOcrJson(p.dump()); success(r,"Paddle word boxes");
    check(r.results.size()==2 && r.results[0].level==OcrLevel::Line && r.results[0].words.size()==2 && r.results[1].words[0].poly.size()==4,"word geometry preserves adapter level");
    p["text_word"][0].push_back("extra"); r = parsePaddleOcrJson(p.dump());
    check(r && r.results[0].words.empty() && !r.warning.empty(),"Paddle word arrays mismatch retains line");

}
void apiParser() {
    auto a = Json{{"results", {{{"text", "hello"}, {"confidence", .99}, {"box", quad(.25, .75)}}}}};
    auto r = parseOcrApiJson(a.dump()); success(r, "simple shape");
    check(r.results.size() == 1 && r.results[0].poly[0].x == .25 && r.results[0].conf == .99 && r.results[0].level == OcrLevel::Line, "simple float fields");
    for (const auto* alias : {"poly", "bbox"}) {
        auto copy = a; auto& entry = copy["results"][0];
        entry[alias] = entry["box"]; entry.erase("box"); entry["score"] = entry["confidence"]; entry.erase("confidence");
        success(parseOcrApiJson(copy.dump()), "simple aliases");
    }
    Json page{{"prunedResult", paddle()}};
    auto b = Json{{"errorCode", 0}, {"errorMsg", "Success"}, {"result", {{"ocrResults", {page, page}}}}};
    r = parseOcrApiJson(b.dump()); success(r, "official shape");
    check(r.results.size() == 4 && r.results[1].poly[1].x == 50 && r.results[0].level == OcrLevel::Line, "pages concatenated and winding normalized");
    b["result"]["ocrResults"][0]["prunedResult"] = {{"rec_texts", {"rectangle"}}, {"rec_boxes", {{.5,1.5,10.5,20.5}}}, {"rec_scores", {.9}}};
    r = parseOcrApiJson(b.dump()); success(r, "official rec_boxes");
    check(r.results.size() == 3 && r.results[0].poly[2].x == 10.5, "rectangle float coordinates");
    auto withWords = a;
    withWords["results"][0]["words"] = {{{"text", "he"}, {"box", {0,0,8,10}}}, {{"text", "llo"}, {"poly", quad(8,0,12)}}};
    r = parseOcrApiJson(withWords.dump()); check(r && r.results[0].words.size() == 2 && r.warning.empty(), "simple API nested words");
    withWords["results"][0]["words"][0]["text"] = "bad";
    r = parseOcrApiJson(withWords.dump()); check(r && r.results[0].words.empty() && !r.warning.empty(), "simple API metadata soft failure");
    auto officialWords = page;
    officialWords["prunedResult"]["text_word"] = {{"中", "　", "文"}, {"Rotated"}};
    officialWords["prunedResult"]["text_word_boxes"] = {{{0,0,10,10}, nullptr, {10,0,20,10}}, {{30,30,50,50}}};
    auto officialResponse = Json{{"result", {{"ocrResults", {officialWords}}}}};
    r = parseOcrApiJson(officialResponse.dump());
    check(r && r.results.size() == 2 && r.results[0].words.size() == 2 && r.warning.empty(), "official API parallel word arrays and whitespace");
    officialResponse["result"]["ocrResults"][0]["prunedResult"]["text_word_boxes"][0] = nullptr;
    r = parseOcrApiJson(officialResponse.dump());
    check(r && r.results[0].words.empty() && r.results[1].words.size() == 1 && !r.warning.empty(), "official bad words preserve other lines");
    auto emptyPage = page;
    emptyPage["prunedResult"] = {{"rec_texts", Json::array()}, {"rec_scores", Json::array()}, {"rec_polys", Json::array()}};
    auto emptyOfficial = b;
    emptyOfficial["result"]["ocrResults"] = Json::array();
    auto emptyLine = b;
    emptyLine["result"]["ocrResults"] = {emptyPage};
    for (const auto& empty : {Json{{"results", Json::array()}}, emptyOfficial, emptyLine}) {
        r = parseOcrApiJson(empty.dump()); check(r && r.results.empty(), "empty recognition succeeds");
    }
    bool mismatch = false;
    r = parseOcrApiJson("{}", &mismatch); check(!r && mismatch, "unknown JSON permits format fallback");
    r = parseOcrApiJson("{", &mismatch); check(!r && !mismatch && r.error == "OCR API response is not valid JSON", "invalid JSON is a real error");
    b["errorCode"] = 7; b["errorMsg"] = "model unavailable";
    r = parseOcrApiJson(b.dump(), &mismatch); check(!r && !mismatch && r.error.find("model unavailable") != r.error.npos, "official error is not mismatch");
    check(!parseOcrApiJson(std::string(OcrMaxBytes + 1, ' ')), "API byte limit");
    for (const auto& poly : {Json::array(), Json::array({{0,0},{1,1},{2,2}}), Json::array({{0,0},{1e8,0},{0,1}}), Json::array({{0,0},{1},{0,1}})}) {
        auto copy = a; copy["results"][0]["box"] = poly;
        r = parseOcrApiJson(copy.dump(), &mismatch); check(!r && !mismatch && r.results.empty(), "bad polygon atomic failure");
    }
    auto many = Json::array();
    for (std::size_t i = 0; i <= OcrMaxResults; ++i) many.push_back(a["results"][0]);
    check(!parseOcrApiJson(Json{{"results", many}}.dump()), "API result count limit");
    auto copy = a; copy["results"][0]["confidence"] = 2; check(!parseOcrApiJson(copy.dump()), "API confidence bounds");
    copy = a; copy["results"][0]["box"] = Json::array();
    for (std::size_t i = 0; i <= OcrMaxPoints; ++i) copy["results"][0]["box"].push_back({i,i%2});
    check(!parseOcrApiJson(copy.dump()), "API polygon point limit");
    check(!parseOcrApiJson("{\"results\":[],\"extra\":" + std::string(40, '[') + "0" + std::string(40, ']') + "}"), "API nesting limit");
}
void apiCache() {
    Temp root;
    Env cacheEnv("XDG_CACHE_HOME", root.path.string());
    const auto cacheFile = root.path / "hyprcapture/ocr-api-format.json";
    const auto image = root.path / "probe.png"; write(image, "sample");
    std::int64_t now = 1800000000;
    for (bool official : {false, true}) {
        auto c = config("paddleocr");
        c.apiUrl = official ? "http://cache.test/layout-parsing" : "http://cache.test/ocr";
        const auto hash = detail::ocrApiUrlHash(c.apiUrl);
        bool reject = true;
        std::vector<bool> flags;
        auto call = [&] {
            Temp attempt;
            return detail::runOcrApi(image, c, attempt.path, {}, [&](auto, int fd) {
                std::string options;
                char buffer[1024];
                for (ssize_t n; (n = ::read(fd, buffer, sizeof(buffer))) > 0;) options.append(buffer, n);
                const bool flag = official ? options.find("request-lines.json") == std::string::npos :
                    options.find("return_word_box=true") != std::string::npos;
                flags.push_back(flag);
                if (reject && flag) return detail::OcrProcessOutput{"{}\n422", {}};
                return detail::OcrProcessOutput{Json{{"results", {{{"text", "hello"}, {"box", quad()}}}}}.dump() + "\n200", {}};
            }, [&] { return std::chrono::system_clock::time_point(std::chrono::seconds(now)); });
        };
        success(call(), "initial rejected parameter fallback");
        check(flags == std::vector<bool>({true,false}), "one retry on rejection");
        auto data = Json::parse(read(cacheFile));
        check(data[hash + ":no_word_box"]["recorded_at"] == now, "rejection timestamp persisted");
        struct stat st{}; check(stat(cacheFile.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "cache remains 0600");
        const auto recorded = now;
        now += 86399; flags.clear(); success(call(), "unexpired rejection reused");
        check(flags == std::vector<bool>({false}), "no retry cost within 24 hours");
        check(Json::parse(read(cacheFile))[hash + ":no_word_box"]["recorded_at"] == recorded, "cache hits do not extend rejection lifetime");
        ++now; reject = false; flags.clear(); success(call(), "reprobe at exact 24-hour expiry");
        check(flags == std::vector<bool>({true}), "expired in-memory flag sends parameter again");
        check(!Json::parse(read(cacheFile)).contains(hash + ":no_word_box"), "parameter success clears persisted flag");
        flags.clear(); success(call(), "success clears in-memory rejection"); check(flags == std::vector<bool>({true}), "parameter stays enabled");
        data = Json::parse(read(cacheFile)); data[hash + ":no_word_box"] = true;
        write(cacheFile, data.dump()); flags.clear(); success(call(), "legacy flag without timestamp loads");
        check(flags == std::vector<bool>({true}), "legacy untimed flag reprobed immediately");
        // An independent atomic writer changes inode, even with equal-size contents.
        data = Json::parse(read(cacheFile)); data[hash + ":no_word_box"] = {{"recorded_at", now}};
        const auto replacement = cacheFile.string()+".external";
        write(replacement, data.dump()); fs::rename(replacement, cacheFile);
        flags.clear(); success(call(), "external cache replacement reloaded");
        check(flags == std::vector<bool>({false}), "external rejection flag replaces in-memory copy");
        data[hash + ":no_word_box"]["recorded_at"] = now-86400;
        write(replacement, data.dump()); fs::rename(replacement, cacheFile);
        flags.clear(); success(call(), "expired persisted flag reloaded");
        check(flags == std::vector<bool>({true}), "expired disk flag re-probed and cleared");
        check(!Json::parse(read(cacheFile)).contains(hash + ":no_word_box"), "disk expiry clears rejection after success");
    }
    const auto view = detail::ocrApiCacheShow(); success(view, "cache view");
    check(view.entries.size() == 2, "format cache retained across timestamp changes");
    const auto removed = detail::ocrApiCacheClear("http://cache.test/ocr"); success(removed, "scoped cache clear");
    check(removed.entries.size() == 1 && detail::ocrApiCacheShow().entries.size() == 1, "scoped cache clear retains other URLs");
    success(detail::ocrApiCacheClear(), "clear all cache entries");
    check(detail::ocrApiCacheShow().entries.empty(), "cache empty after clear");
}
void configTests() {
    auto c = OcrConfig{};
    check(c.mode == "off" && c.engine == "tesseract" && c.languages == "eng+chi_sim" && c.command.empty() && c.output == "text" && c.timeout == 30, "six defaults");
    check(c.apiUrl.empty() && c.apiKey.empty(), "API defaults empty");
    std::string error;
    for (auto [key, value] : {std::pair{"ocr_mode", "capture"}, {"ocr_engine", "command"}, {"ocr_languages", "eng"},
                             {"ocr_command", "test '{image}'"}, {"ocr_output", "json"}, {"ocr_timeout", "10"}})
        check(setOcrConfigValue(c, key, value, error), std::string("parse ") + key);
    for (auto [key, value] : {std::pair{"ocr_mode", "invalid"}, {"ocr_engine", "invalid"}, {"ocr_output", "invalid"},
                             {"ocr_timeout", "0"}, {"ocr_timeout", "86401"}, {"ocr_timeout", "1.5"}, {"ocr_languages", ""}, {"bad", "x"}}) {
        check(!setOcrConfigValue(c, key, value, error) && !error.empty(), "invalid configuration");
        check(c.mode == "capture" && c.timeout == 10 && c.languages == "eng", "failed set leaves config unchanged");
    }
    check(setOcrConfigValue(c, "ocr_api_url", "http://127.0.0.1/ocr", error), "API URL setting");
    check(setOcrConfigValue(c, "ocr_api_key", "secret", error), "API key setting");
    for (const auto* key : {"ocr_api_url", "ocr_api_key"}) {
        check(!setOcrConfigValue(c, key, "bad\r\nheader", error), "API CRLF rejected");
        check(!setOcrConfigValue(c, key, std::string_view("bad\0value",9), error), "API NUL rejected");
        check(!setOcrConfigValue(c, key, std::string(65537,'x'), error), "API length limit");
    }
    OcrApiWarning apiWarning;
    check(apiWarning.update(c), "URL with command warns");
    check(!apiWarning.update(c), "identical API config warns once");
    c.apiKey = "changed"; check(apiWarning.update(c), "API config change warns once");
    c.engine = "paddleocr"; check(!apiWarning.update(c), "URL with paddleocr has no warning");
    c.apiUrl.clear(); c.engine = "tesseract"; check(!apiWarning.update(c), "empty URL no warning");
    c.apiUrl = "http://127.0.0.1/ocr"; c.engine = "command";
    OcrFallbackWarning warning;
    c.mode = "text_layer"; c.output = "text";
    check(warning.update(c), "fallback warns at first config load");
    check(!warning.update(c), "same config reload does not warn");
    c.languages = "chi_sim";
    check(warning.update(c), "changed config warns once");
    check(!warning.update(c), "capture reads do not repeat warning");
    c.output = "json"; check(!warning.update(c), "geometry config resets warning key");
    c.output = "text"; check(warning.update(c), "returning to fallback warns");
    c.languages = "eng";

    check(validateOcrConfig(c).empty(), "text-only text_layer uses copy fallback");
    c.mode = "both"; check(validateOcrConfig(c).empty(), "text-only both uses copy fallback");
    c.output = "json"; check(validateOcrConfig(c).empty(), "JSON text_layer accepted");
    CaptureSession session; session.id = "ocr-test"; session.defaults.ocr = c;
    const auto encoded = encodeSessionJson(session);
    auto decoded = decodeSessionJson(encoded);
    check(decoded && decoded->defaults.ocr.command == c.command && decoded->defaults.ocr.timeout == 10 && decoded->defaults.ocr.mode == "both", "session roundtrip");
    check(decoded && decoded->defaults.ocr.apiUrl == c.apiUrl && decoded->defaults.ocr.apiKey == c.apiKey, "API session roundtrip");
    auto json = Json::parse(encoded);
    auto beforeApi = json;
    beforeApi["defaults"].erase("ocrApiUrl"); beforeApi["defaults"].erase("ocrApiKey");
    auto legacyApi = decodeSessionJson(beforeApi.dump());
    check(legacyApi && legacyApi->defaults.ocr.mode == c.mode && legacyApi->defaults.ocr.engine == c.engine &&
          legacyApi->defaults.ocr.apiUrl.empty() && legacyApi->defaults.ocr.apiKey.empty(), "existing OCR session without API keys retains old settings");
    for (const auto* key : {"ocrMode", "ocrEngine", "ocrLanguages", "ocrCommand", "ocrOutput", "ocrTimeout", "ocrApiUrl", "ocrApiKey"}) json["defaults"].erase(key);
    decoded = decodeSessionJson(json.dump()); check(decoded && decoded->defaults.ocr.mode == "off", "legacy session defaults off");
    check(decoded && decoded->defaults.ocr.apiUrl.empty() && decoded->defaults.ocr.apiKey.empty(), "legacy API defaults");
    for (const auto* key : {"ocrApiUrl", "ocrApiKey"}) {
        auto bad = json; bad["defaults"][key] = 42; check(!decodeSessionJson(bad.dump()), "API session rejects non-string");
    }
    for (const auto& bad : {Json(0), Json(1.5), Json("30"), Json(86401)}) {
        auto copy = json; copy["defaults"]["ocrTimeout"] = bad; check(!decodeSessionJson(copy.dump()), "protocol timeout validation");
    }
    json["defaults"]["ocrEngine"] = "bogus"; check(!decodeSessionJson(json.dump()), "protocol engine validation");
    RecordingRequest recording; recording.id = "ocr-test"; recording.targetGeometry = {0,0,100,100}; recording.defaults.ocr = c;
    auto decodedRecording = decodeRecordingRequestJson(encodeRecordingRequestJson(recording));
    check(decodedRecording && decodedRecording->defaults.ocr.command == c.command, "recording defaults roundtrip");
}
void adapters() {
    Temp t;
    Env path("PATH", t.path.string() + ":" + (std::getenv("PATH") ? std::getenv("PATH") : "/usr/bin:/bin"));
    Env runtime("XDG_RUNTIME_DIR", t.path.string());
    const auto image = t.path / "test image.png"; write(image, "fixture");
    write(t.path / "tesseract", "#!/bin/sh\n[ \"$2\" = stdout ] && [ \"$3\" = -l ] && [ \"$4\" = eng+chi_sim ] && [ \"$5\" = tsv ] && [ \"$#\" = 5 ] || exit 8\ncat <<'TSV'\n" + tsv + "TSV\n", true);
    check(security::trustedExecutablePath((t.path / "tesseract").string()).has_value(), "fake executable path must be trusted; run fixture suites in the CTest sandbox");
    auto r = runOcrEngine(image, config("tesseract")); success(r, "fake Tesseract");
    check(joinOcrText(r.results) == "Hello world\n中文", "Tesseract copy text unchanged from legacy words");
    auto saved = paddle();
    saved["text_word"] = {{"中", "文"}, {"Rotated"}};
    saved["text_word_boxes"] = {{{0,0,10,10},{10,0,20,10}}, {{30,30,50,50}}};
    const std::string paddleScript =
        "#!/bin/sh\n[ \"$1\" = ocr ] && [ \"$2\" = -i ] && [ \"$4\" = --save_path ] || exit 9\n"
        "[ \"$6\" = --use_doc_orientation_classify ] && [ \"$7\" = False ] && [ \"$8\" = --use_doc_unwarping ] && [ \"$9\" = False ] || exit 10\n"
        "[ \"${10}\" = --use_textline_orientation ] && [ \"${11}\" = False ] && [ \"${12}\" = --enable_mkldnn ] && [ \"${13}\" = False ] && [ \"${14}\" = --return_word_box ] && [ \"${15}\" = True ] && [ \"$#\" = 15 ] || exit 11\n"
        "stat -c %a \"$5\" > \"${3}.mode\"\nprintf '%s' \"$5\" > \"${3}.temp\"\n"
        "name=${3##*/}; name=${name%.*}\ncat > \"$5/${name}_res.json\" <<'JSON'\n" + saved.dump() + "\nJSON\n"
        "printf 'numpy.float64(0.99): ignored logs\\n'\n";
    write(t.path / "paddleocr", paddleScript, true);
    r = runOcrEngine(image, config("paddleocr")); success(r, "fake Paddle");
    check(r.results.size() == 2 && r.results[0].text == "中文" && r.results[0].words.size() == 2, "Paddle saved line plus words not stdout");
    check(read(image.string() + ".mode") == "700\n", "Paddle directory private");
    check(!fs::exists(read(image.string() + ".temp")), "Paddle temporary directory removed");
    write(t.path / "paddleocr", "#!/bin/sh\nprintf '%s' \"$5\" > \"${3}.temp\"\necho 'first failure' >&2\nexit 4\n", true);
    r = runOcrEngine(image, config("paddleocr"));
    check(!r && r.error.find("first failure") != std::string::npos && !fs::exists(read(image.string() + ".temp")), "Paddle failure cleanup");
    write(t.path / "paddleocr", "#!/bin/sh\nexit 0\n", true);
    check(!runOcrEngine(image, config("paddleocr")), "Paddle missing JSON");
    write(t.path / "paddleocr", "#!/bin/sh\nname=${3##*/}; name=${name%.*}\nprintf '{' > \"$5/${name}_res.json\"\n", true);
    check(!runOcrEngine(image, config("paddleocr")), "Paddle malformed JSON");
    write(t.path / "paddleocr", "#!/bin/sh\nprintf '%s' \"$5\" > \"${3}.temp\"\nsleep 5\n", true);
    auto slow = config("paddleocr"); slow.timeout = 1;
    r = runOcrEngine(image, slow);
    check(!r && r.error.find("timed out") != r.error.npos && !fs::exists(read(image.string() + ".temp")), "Paddle timeout cleanup");
}
void runner() {
    Temp t;
    const auto image = t.path / "image ' $(literal) {image}.png"; write(image, "fixture");
    Env path("PATH", t.path.string() + ":" + (std::getenv("PATH") ? std::getenv("PATH") : "/usr/bin:/bin"));
    auto parsed = splitOcrCommand("prog 'one two' \"three four\" five\\ six '' x{image}y '{image}' '$HOME' '~/file' ; $(no)", image.string());
    success(parsed, "split command");
    check(parsed.argv.size() == 11 && parsed.argv[1] == "one two" && parsed.argv[2] == "three four" && parsed.argv[3] == "five six" && parsed.argv[4].empty(), "quoting");
    check(parsed.argv[5] == "x" + image.string() + "y" && parsed.argv[6] == image.string(), "whole/embedded image replacement without recursion");
    check(parsed.argv[7] == "$HOME" && parsed.argv[8] == std::string(std::getenv("HOME")) + "/file" && parsed.argv[9] == ";", "only tilde expansion, operators literal");
    for (const auto* bad : {"", "'' arg", "prog 'bad", "prog \"bad", "prog bad\\"}) check(!splitOcrCommand(bad, "png"), "invalid quoting");
    write(t.path / "echo-argv", "#!/bin/sh\nprintf '%s\\n' \"$@\"\n", true);
    auto c = config(); c.command = "echo-argv '{image}' 'pre{image}post' 'a b' '$HOME' '$(touch injected)'";
    auto r = runOcrEngine(image, c); success(r, "literal execution");
    check(r.results.size() == 1 && r.results[0].textOnly() && !ocrSupportsTextLayer(r.results), "explicit text-only result");
    check(r.results[0].text == image.string() + "\npre" + image.string() + "post\na b\n$HOME\n$(touch injected)\n", "argv preserved");
    check(!fs::exists("injected"), "no shell interpretation");
    write(t.path / "environment-engine", "#!/bin/sh\nprintf '%s' \"${HYPRCAPTURE_OCR_TEST_SECRET-unset}\"\n", true);
    Env secret("HYPRCAPTURE_OCR_TEST_SECRET", "must-not-leak");
    c.command = "environment-engine"; r = runOcrEngine(image, c); success(r, "filtered environment");
    check(r.results[0].text == "unset", "arbitrary variables not inherited");
    write(t.path / "json-engine", "#!/bin/sh\ncat <<'JSON'\n" + item().dump() + "\nJSON\n", true);
    c.command = "json-engine"; c.output = "json"; r = runOcrEngine(image, c); success(r, "command JSON");
    check(ocrSupportsTextLayer(r.results), "JSON supports layer");
    struct sigaction old{}, policy{}; policy.sa_flags = SA_NOCLDWAIT; sigemptyset(&policy.sa_mask);
    check(sigaction(SIGCHLD, &policy, &old) == 0, "set compositor child policy");
    r = runOcrEngine(image, c); success(r, "runner under SA_NOCLDWAIT");
    check(sigaction(SIGCHLD, &old, nullptr) == 0, "restore child policy");
    write(t.path / "bad-engine", "#!/bin/sh\necho 'first error' >&2\necho 'second error' >&2\nexit 7\n", true);
    c.command = "bad-engine"; r = runOcrEngine(image, c);
    check(!r && r.results.empty() && r.error.find("status 7: first error") != std::string::npos && r.error.find("second error") == std::string::npos, "nonzero with first stderr line");
    write(t.path / "timeout-engine", "#!/bin/sh\necho 'waiting' >&2\n(sleep 3; echo survived > \"$1\") &\nprintf '%s' \"$!\" > \"$2\"\nwait\n", true);
    c.command = "timeout-engine '" + (t.path / "survived").string() + "' '" + (t.path / "pid").string() + "'"; c.timeout = 1;
    const auto start = std::chrono::steady_clock::now();
    r = runOcrEngine(image, c);
    check(!r && r.error.find("timed out") != std::string::npos && r.error.find("waiting") != std::string::npos, "timeout diagnostic");
    check(std::chrono::steady_clock::now() - start < std::chrono::seconds(2), "bounded wall time");
    const auto pid = std::stoi(read(t.path / "pid"));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto status = read("/proc/" + std::to_string(pid) + "/stat");
    const auto stateAt = status.rfind(") ");
    check(status.empty() || (stateAt != status.npos && status[stateAt + 2] == 'Z'), "descendant killed, not running");
    check(!fs::exists(t.path / "survived"), "timeout descendant did not write");
    write(t.path / "flood", "#!/bin/sh\nhead -c 8388609 /dev/zero\n", true);
    c.command = "flood"; c.timeout = 5; r = runOcrEngine(image, c);
    check(!r && r.error.find("stdout exceeds") != std::string::npos, "stdout cap");
    write(t.path / "flood", "#!/bin/sh\nhead -c 65537 /dev/zero >&2\n", true);
    r = runOcrEngine(image, c); check(!r && r.error.find("stderr exceeds") != std::string::npos, "stderr cap");
    c.command = "missing-ocr-program-9321"; check(!runOcrEngine(image, c), "missing executable");
    c.command = "echo-argv"; check(!runOcrEngine(t.path / "missing.png", c), "missing PNG");
    chmod((t.path / "echo-argv").c_str(), 0777);
    check(!runOcrEngine(image, c), "untrusted executable rejected");
    c.mode = "off"; r = runOcrEngine(image, c); check(!r && r.error.find("ocr_mode") != r.error.npos, "off mode error names key");
}
void join() {
    auto a = item("world", 40, 11); a["level"] = "word";
    auto b = item("Hello", 0, 10); b["level"] = "word";
    auto c = item("文", 22, 40), d = item("中", 0, 41);
    auto r = parseOcrJsonLines(c.dump() + "\n" + a.dump() + "\n" + d.dump() + "\n" + b.dump());
    success(r, "join fixture");
    check(joinOcrText(r.results) == "Hello world\n中文", "vertical overlap and centroid order");
    r = parsePaddleOcrJson(paddle().dump()); success(r, "rotated join");
    check(joinOcrText(r.results) == "中文\nRotated", "rotated centroid order");
    r = parseOcrJsonLines(item("中文", 0, 0).dump() + "\n" + item("OCR", 40, 0).dump());
    check(joinOcrText(r.results) == "中文 OCR", "CJK Latin boundary gets space");
    check(joinOcrText({}).empty(), "empty join");
    check(joinOcrText({{"raw\ntext\n", {}, {}, OcrLevel::Line}}) == "raw\ntext\n", "text-only stdout preserved");
    r = parseOcrJsonLines(item("Hello ", 0, 0).dump() + "\n" + item("world", 40, 0).dump());
    check(joinOcrText(r.results) == "Hello world", "no duplicate separator");
}
int live(const std::string& engine) {
    if (std::string(std::getenv("HYPRCAPTURE_OCR_LIVE") ? std::getenv("HYPRCAPTURE_OCR_LIVE") : "") != "1") return 77;
    Temp t;
    // A generated 320x96 grayscale PNG with bitmap text "OCR 123".
    // Deterministic recognition expectations stay in the offline fixtures.
    constexpr unsigned char png[] = {137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,1,64,0,0,0,96,8,0,0,0,0,73,32,244,244,0,0,1,137,73,68,65,84,120,156,237,215,193,174,194,48,12,5,209,252,255,79,195,178,160,52,238,117,166,18,170,152,217,61,201,56,230,176,122,227,101,168,241,235,3,158,158,128,48,1,97,2,194,4,132,9,8,19,16,38,32,76,64,152,128,48,1,97,2,194,4,132,9,8,19,16,38,32,76,64,152,128,48,1,97,2,194,4,132,9,8,19,16,38,32,108,13,56,190,75,231,206,134,131,145,221,243,175,87,5,239,146,251,4,132,247,9,8,239,19,16,222,39,32,188,239,98,225,217,159,203,185,224,59,180,62,214,90,122,227,187,173,251,4,220,218,252,49,76,30,22,80,192,205,205,31,195,228,97,1,159,12,120,172,106,13,11,56,111,110,13,11,56,111,110,13,11,56,111,110,13,11,56,111,110,13,63,28,240,70,187,99,115,58,87,15,111,222,39,224,226,200,248,130,224,57,1,171,205,193,115,2,86,155,131,231,4,172,54,7,207,221,13,152,222,22,212,122,188,181,52,254,105,130,13,2,86,195,193,6,1,171,225,96,131,128,213,112,176,65,192,106,56,216,112,35,224,241,103,112,91,80,189,106,243,55,19,176,123,31,249,152,128,240,99,2,194,143,9,8,63,198,255,181,25,39,213,23,109,126,175,254,125,103,167,213,247,245,79,19,48,251,10,235,51,200,129,233,235,2,10,184,62,131,28,152,190,254,159,128,22,37,32,76,64,152,128,48,1,97,2,194,4,132,9,8,19,16,38,32,76,64,152,128,48,1,97,2,194,4,132,9,8,19,16,38,32,76,64,152,128,48,1,97,2,194,4,132,9,8,19,16,38,32,76,64,152,128,48,1,97,2,194,4,132,9,8,19,16,38,32,76,64,152,128,176,55,185,16,250,229,204,83,63,119,0,0,0,0,73,69,78,68,174,66,96,130};
    const auto image = t.path / "live.png"; write(image, {reinterpret_cast<const char*>(png), sizeof(png)});
    std::optional<Env> path;
    if (engine == "paddleocr") {
        const auto dir = fs::path(std::getenv("HOME")) / "data/model/.venv/bin";
        if (!fs::exists(dir / "paddleocr")) return 77;
        path.emplace("PATH", dir.string() + ":/usr/bin:/bin");
    }
    auto c = config(engine); c.languages = "eng"; c.timeout = 120;
    auto r = runOcrEngine(image, c);
    if (!r && r.error.find("not found or not trusted") != r.error.npos) return 77;
    success(r, "live " + engine);
    return 0;
}
} // namespace
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string suite = argv[1];
    if (suite == "api-cache") apiCache();
    else if (suite == "api-parser") apiParser();
    else if (suite == "parser") parser();
    else if (suite == "config") configTests();
    else if (suite == "adapters") adapters();
    else if (suite == "runner") runner();
    else if (suite == "join") join();
    else if (suite.starts_with("live-")) return live(suite.substr(5));
    else return 2;
    std::cout << suite << ": " << checks << " checks passed\n";
}
