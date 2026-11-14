#include "shared/ocr_api.hpp"
#include "shared/ocr_engine.hpp"
#include "ocr/probe_png.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

using namespace hyprcapture;
namespace {
struct Failure : std::runtime_error {
    int code;
    Failure(int code, const std::string& text) : std::runtime_error(text), code(code) {}
};
struct Options { std::string command, url, keyFile; bool help = false; };
Options options(int argc, char** argv) {
    Options out;
    int i = 1;
    if (i < argc && std::string_view(argv[i]) == "cache") {
        if (++i >= argc) throw Failure(64, "cache requires show or clear");
        const std::string action = argv[i++];
        if (action == "--help") out.help = true;
        else if (action == "show" || action == "clear") out.command = action;
        else throw Failure(64, "cache requires show or clear");
    } else if (i < argc && std::string_view(argv[i]) == "probe") { out.command = "probe"; ++i; }
    for (; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help") out.help = true;
        else if (arg == "--url" && out.url.empty() && i + 1 < argc) out.url = argv[++i];
        else if (arg == "--key-file" && out.command == "probe" && out.keyFile.empty() && i + 1 < argc) out.keyFile = argv[++i];
        else throw Failure(64, "invalid arguments; see --help");
        if ((arg == "--url" && out.url.empty()) || (arg == "--key-file" && out.keyFile.empty())) throw Failure(64, "option requires a nonempty value");
    }
    if (out.command.empty() && !out.help) throw Failure(64, "expected cache show, cache clear, or probe; see --help");
    return out;
}
std::string keyFile(const std::string& path) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    struct stat st{};
    if (fd < 0) throw Failure(1, "could not open key file");
    const bool safe = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
        (st.st_mode & 0777) == 0600 && st.st_size >= 0 && st.st_size <= 65536;
    std::string value;
    if (safe) {
        char buffer[4096];
        for (;;) {
            const auto n = read(fd, buffer, sizeof(buffer));
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) { close(fd); throw Failure(1, "could not read key file"); }
            if (!n) break;
            if (value.size() + n > 65536) { close(fd); throw Failure(1, "key file exceeds limit"); }
            value.append(buffer, n);
        }
    }
    close(fd);
    if (!safe) throw Failure(1, "key file must be a regular owned 0600 file, at most 65536 bytes");
    if (value.ends_with('\n')) { value.pop_back(); if (value.ends_with('\r')) value.pop_back(); }
    return value;
}
nlohmann::json option(std::string_view name) {
    const auto result = detail::ocrReadHyprlandOption(name);
    if (!result.error.empty()) throw Failure(1, "could not read OCR configuration from Hyprland; use --url");
    try { return nlohmann::json::parse(result.out); }
    catch (...) { throw Failure(1, "Hyprland returned invalid OCR configuration"); }
}
struct Sample {
    std::string path;
    ~Sample() { if (!path.empty()) unlink(path.c_str()); }
    void create() {
        const char* runtime = std::getenv("XDG_RUNTIME_DIR");
        std::error_code ec;
        const auto base = runtime && std::filesystem::path(runtime).is_absolute() ? std::filesystem::path(runtime) :
            std::filesystem::temp_directory_path(ec);
        if (ec || base.empty()) throw Failure(1, "could not find a probe temporary directory");
        auto pattern = (base / "hyprcapture-ocr-probe-XXXXXX").string();
        const int fd = mkstemp(pattern.data());
        if (fd < 0) throw Failure(1, "could not create probe image");
        path = pattern;
        std::size_t at = 0;
        while (at < OcrProbePng.size()) {
            const auto n = write(fd, OcrProbePng.data() + at, OcrProbePng.size() - at);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { close(fd); throw Failure(1, "could not write probe image"); }
            at += n;
        }
        close(fd);
    }
};
int execute(const Options& opts) {
    if (opts.help) {
        std::cout << "Usage:\n  hyprcapture-ocr cache show [--url URL]\n  hyprcapture-ocr cache clear [--url URL]\n"
                     "  hyprcapture-ocr probe [--url URL] [--key-file FILE]\n"
                     "Without --url, probe reads the OCR options from hyprctl. Key files must be owned 0600 files.\n"
                     "Probe ignores the cache; rejected word-box parameters expire after 24 hours.\n";
        return 0;
    }
    if (opts.command == "show" || opts.command == "clear") {
        const auto filter = opts.url.empty() ? std::optional<std::string_view>{} : opts.url;
        const auto view = opts.command == "show" ? detail::ocrApiCacheShow() : detail::ocrApiCacheClear(filter);
        if (!view) throw Failure(1, view.error);
        if (view.entries.empty()) std::cout << "No cached entries.\n";
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        for (const auto& entry : view.entries) {
            if (opts.command == "clear") std::cout << "Removed ";
            else std::cout << (!opts.url.empty() && detail::ocrApiUrlHash(opts.url) == entry.hash ? "* " : "  ");
            std::cout << entry.hash.substr(0,12) << " " << entry.format;
            if (entry.rejectedAt > 0) {
                const auto age = now - entry.rejectedAt;
                std::cout << " word-box-parameter=rejected age=" << age << "s expires-in=" << (age >= 0 && age < 86400 ? 86400-age : 0) << "s";
            } else std::cout << " word-box-parameter=probe-on-next-call";
            std::cout << '\n';
        }
        return 0;
    }
    OcrConfig config;
    config.mode = "capture"; config.engine = "paddleocr"; config.apiUrl = opts.url;
    if (opts.url.empty()) {
        try {
            config.apiUrl = option("plugin:hyprcapture:ocr_api_url").at("str").get<std::string>();
            config.apiKey = option("plugin:hyprcapture:ocr_api_key").at("str").get<std::string>();
            config.timeout = option("plugin:hyprcapture:ocr_timeout").at("int").get<int>();
        } catch (const Failure&) { throw; }
        catch (...) { throw Failure(1, "Hyprland returned invalid OCR configuration"); }
    }
    if (!opts.keyFile.empty()) config.apiKey = keyFile(opts.keyFile);
    if (config.apiUrl.empty()) throw Failure(1, "OCR API URL is empty; use --url");
    Sample sample; sample.create();
    const auto start = std::chrono::steady_clock::now();
    const auto result = runOcrEngine(sample.path, config, {}, true);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
    if (!result) throw Failure(1, result.error);
    std::size_t words = 0;
    for (const auto& line : result.results) words += line.words.size();
    std::cout << "format: " << result.apiFormat << "\nword boxes: " << (words ? "yes" : "not returned")
              << "\nlines: " << result.results.size() << "\nwords: " << words << "\nlatency: " << elapsed << " ms\n";
    return 0;
}
}
int main(int argc, char** argv) {
    try { return execute(options(argc, argv)); }
    catch (const Failure& e) { std::cerr << "hyprcapture-ocr: " << e.what() << '\n'; return e.code; }
    catch (...) { std::cerr << "hyprcapture-ocr: operation failed\n"; return 1; }
}
