#include "shared/ocr_api.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <fcntl.h>
#include <map>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hyprcapture::detail {
namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
struct Fd {
    int value = -1;
    ~Fd() { if (value >= 0) close(value); }
};
std::mutex cacheMutex;
// The location is included so changing XDG_CACHE_HOME cannot reuse another cache.
struct CachedFormat { bool paddlex = false; std::int64_t rejectedAt = 0; };
struct FileStamp {
    dev_t device = 0; ino_t inode = 0; off_t size = 0;
    std::int64_t seconds = 0, nanoseconds = 0;
    bool operator==(const FileStamp&) const = default;
};
FileStamp stamp(const struct stat& st) { return {st.st_dev, st.st_ino, st.st_size, st.st_mtim.tv_sec, st.st_mtim.tv_nsec}; }
std::map<std::string, FileStamp> memoryStamps;
constexpr std::int64_t RejectionTtl = 24 * 60 * 60;
std::int64_t recordedAt(const Json& entries, const std::string& hash) {
    const auto flag = entries.find(hash + ":no_word_box");
    if (flag == entries.end() || !flag->is_object()) return 0; // Legacy flag without a timestamp expires immediately.
    const auto at = flag->find("recorded_at");
    if (at == flag->end() || !at->is_number_integer() || at->get<double>() <= 0 || at->get<double>() > 9e15) return 0;
    return at->get<std::int64_t>();
}
std::map<std::pair<std::string, std::string>, CachedFormat> memoryCache;

std::string hashUrl(std::string_view url) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (!EVP_Digest(url.data(), url.size(), digest, &length, EVP_sha256(), nullptr)) return {};
    std::string out;
    for (unsigned int i = 0; i < length; ++i) {
        out += "0123456789abcdef"[digest[i] >> 4];
        out += "0123456789abcdef"[digest[i] & 15];
    }
    return out;
}

// Cache I/O is advisory. Reject links, non-owned files and writable directories.
// Serialize read/modify/rename across helpers and threads; store hashes only.
class Cache {
  public:
    Cache() {
        std::filesystem::path base;
        if (const char* value = std::getenv("XDG_CACHE_HOME"); value && *value) base = value;
        else if (const char* home = std::getenv("HOME")) base = std::filesystem::path(home) / ".cache";
        if (!base.is_absolute()) return;
        location = (base / "hyprcapture").string();
        std::error_code ec;
        std::filesystem::create_directories(base, ec);
        if (ec) return;
        if (mkdir(location.c_str(), 0700) && errno != EEXIST) return;
        directory.value = open(location.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        struct stat st{};
        if (directory.value < 0 || fstat(directory.value, &st) || st.st_uid != geteuid() || (st.st_mode & 0022)) return;
        lock.value = openat(directory.value, "ocr-api-format.lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
        if (lock.value < 0 || fstat(lock.value, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0077)) return;
        // Do not wait behind another process while an OCR timeout is running.
        if (flock(lock.value, LOCK_EX | LOCK_NB)) return;
        usable = true;
        Fd file{openat(directory.value, "ocr-api-format.json", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
        if (file.value < 0 || fstat(file.value, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
            (st.st_mode & 0077) || st.st_size < 0 || st.st_size > 1024 * 1024) return;
        fingerprint = stamp(st);
        std::string content(static_cast<std::size_t>(st.st_size), '\0');
        std::size_t at = 0;
        while (at < content.size()) {
            const auto n = read(file.value, content.data() + at, content.size() - at);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return;
            at += n;
        }
        try {
            const auto data = Json::parse(content, [](int depth, Json::parse_event_t, Json&) {
                if (depth > 2) throw std::runtime_error("invalid cache nesting");
                return true;
            });
            if (!data.is_object()) return;
            for (auto it = data.begin(); it != data.end(); ++it) {
                if (it.key().size() == 64 && std::ranges::all_of(it.key(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }) &&
                    (it.value() == "simple" || it.value() == "paddlex")) entries[it.key()] = it.value();
                else if (it.key().size() == 76 && it.key().ends_with(":no_word_box") && (it.value() == true || it.value().is_object()) &&
                         std::ranges::all_of(it.key().substr(0, 64), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
                    entries[it.key()] = it.value();
            }
        } catch (...) {} // Corrupt cache is equivalent to no cache.
    }
    bool save() {
        if (!usable) return false;
        // Unique 0600 sibling, then atomic replacement (never follow target links).
        const auto name = "ocr-api-format.tmp-" + std::to_string(getpid());
        Fd file{openat(directory.value, name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600)};
        if (file.value < 0) return false;
        const auto content = entries.dump();
        std::size_t at = 0;
        while (at < content.size()) {
            const auto n = write(file.value, content.data() + at, content.size() - at);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            at += n;
        }
        const bool saved = at == content.size() && fsync(file.value) == 0 &&
            renameat(directory.value, name.c_str(), directory.value, "ocr-api-format.json") == 0;
        unlinkat(directory.value, name.c_str(), 0);
        struct stat st{};
        if (saved && fstat(file.value, &st) == 0) fingerprint = stamp(st);
        return saved;
    }
    bool available() const { return usable; }
    FileStamp fingerprint;
    std::string location;
    Json entries = Json::object();
  private:
    Fd directory, lock;
    bool usable = false;
};

void syncMemory(const Cache& cache) {
    if (const auto it = memoryStamps.find(cache.location); it != memoryStamps.end() && it->second == cache.fingerprint) return;
    std::erase_if(memoryCache, [&](const auto& entry) { return entry.first.first == cache.location; });
    memoryStamps[cache.location] = cache.fingerprint;
}
std::optional<CachedFormat> cachedFormat(const std::string& hash) {
    std::lock_guard guard(cacheMutex);
    Cache cache;
    syncMemory(cache);
    const auto key = std::pair(cache.location, hash);
    if (const auto it = memoryCache.find(key); it != memoryCache.end()) return it->second;
    if (cache.entries.contains(hash)) {
        const CachedFormat value{cache.entries[hash] == "paddlex", recordedAt(cache.entries, hash)};
        memoryCache[key] = value;
        return value;
    }
    return {};
}
void cacheFormat(const std::string& hash, std::optional<bool> value, std::int64_t rejected = 0) {
    if (hash.empty()) return;
    std::lock_guard guard(cacheMutex);
    Cache cache;
    syncMemory(cache);
    const auto key = std::pair(cache.location, hash);
    if (value) {
        memoryCache[key] = {*value, rejected};
        cache.entries[hash] = *value ? "paddlex" : "simple";
        if (rejected) cache.entries[hash + ":no_word_box"] = Json{{"recorded_at", rejected}};
        else cache.entries.erase(hash + ":no_word_box");
    } else {
        memoryCache.erase(key);
        cache.entries.erase(hash);
        cache.entries.erase(hash + ":no_word_box");
    }
    if (cache.save()) memoryStamps[cache.location] = cache.fingerprint;
}

std::string curlQuote(std::string_view value) {
    std::string out = "\"";
    for (const char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c;
        }
    }
    return out + '"';
}
bool writeAll(int fd, std::string_view bytes) {
    while (!bytes.empty()) {
        const auto n = write(fd, bytes.data(), bytes.size());
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        bytes.remove_prefix(n);
    }
    return true;
}
std::string redact(std::string text, const OcrConfig& config) {
    for (const auto* value : {&config.apiUrl, &config.apiKey}) {
        if (value->empty()) continue;
        std::size_t at = 0;
        while ((at = text.find(*value, at)) != text.npos) { text.replace(at, value->size(), "[redacted]"); at += 10; }
    }
    return text.substr(0, 512);
}

bool bodyFile(const std::filesystem::path& image, const std::filesystem::path& file,
              std::stop_token stop, Clock::time_point deadline, bool wordBoxes) {
    Fd input{open(image.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK)};
    Fd output{open(file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)};
    if (input.value < 0 || output.value < 0 || !writeAll(output.value, "{\"file\":\"")) return false;
    std::array<unsigned char, 3 * 4096> bytes;
    std::array<unsigned char, 4 * 4096 + 1> encoded;
    // Accumulate short reads to multiples of three, except the final block.
    std::size_t used = 0;
    for (;;) {
        if (stop.stop_requested() || Clock::now() >= deadline) return false;
        const auto n = read(input.value, bytes.data() + used, bytes.size() - used);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return false;
        used += n;
        if (used == bytes.size() || n == 0) {
            const auto length = EVP_EncodeBlock(encoded.data(), bytes.data(), used);
            if (!writeAll(output.value, {reinterpret_cast<char*>(encoded.data()), static_cast<std::size_t>(length)})) return false;
            used = 0;
        }
        if (n == 0) break;
    }
    return writeAll(output.value, std::string("\",\"fileType\":1,\"useDocOrientationClassify\":false,\"useDocUnwarping\":false,\"useTextlineOrientation\":false,\"visualize\":false") +
                    (wordBoxes ? ",\"returnWordBox\":true}" : "}"));
}
} // namespace

std::string ocrApiUrlHash(std::string_view url) { return hashUrl(url); }
OcrApiCacheView ocrApiCacheShow() {
    std::lock_guard guard(cacheMutex);
    Cache cache;
    syncMemory(cache);
    if (!cache.available()) return {{}, "OCR cache is unavailable or locked"};
    OcrApiCacheView view;
    for (auto it = cache.entries.begin(); it != cache.entries.end(); ++it)
        if (it.key().size() == 64) view.entries.push_back({it.key(), it.value().get<std::string>(), recordedAt(cache.entries, it.key())});
    return view;
}
OcrApiCacheView ocrApiCacheClear(std::optional<std::string_view> url) {
    std::lock_guard guard(cacheMutex);
    Cache cache;
    syncMemory(cache);
    if (!cache.available()) return {{}, "OCR cache is unavailable or locked"};
    const auto hash = url ? hashUrl(*url) : std::string{};
    OcrApiCacheView removed;
    for (auto it = cache.entries.begin(); it != cache.entries.end(); ++it)
        if (it.key().size() == 64 && (!url || it.key() == hash))
            removed.entries.push_back({it.key(), it.value().get<std::string>(), recordedAt(cache.entries, it.key())});
    if (url) { cache.entries.erase(hash); cache.entries.erase(hash + ":no_word_box"); }
    else cache.entries = Json::object();
    if (!cache.save()) return {{}, "OCR cache write failed"};
    syncMemory(cache);
    return removed;
}

bool ocrApiGuessPaddlex(std::string_view url) {
    const auto scheme = url.find("://");
    if (scheme == url.npos) return false;
    auto rest = url.substr(scheme + 3);
    auto host = rest.substr(0, rest.find_first_of("/?#"));
    if (const auto at = host.rfind('@'); at != host.npos) host.remove_prefix(at + 1);
    host = host.substr(0, host.find(':'));
    std::string lower(host);
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return std::tolower(c); });
    const auto domain = [&](std::string_view name) { return lower == name || lower.ends_with("." + std::string(name)); };
    const auto start = rest.find_first_of("/?#");
    auto path = start == rest.npos || rest[start] != '/' ? std::string_view{} : rest.substr(start);
    path = path.substr(0, path.find_first_of("?#"));
    return domain("aistudio-app.com") || domain("aistudio.baidu.com") || path.ends_with("/layout-parsing");
}

OcrResponse runOcrApi(const std::filesystem::path& image, const OcrConfig& config,
                      const std::filesystem::path& temporary, std::stop_token stop, const OcrApiRunner& run, const OcrApiClock& now, bool ignoreCache) {
    const auto deadline = Clock::now() + std::chrono::seconds(config.timeout);
    const auto hash = hashUrl(config.apiUrl);
    const auto cached = ignoreCache ? std::optional<CachedFormat>{} : cachedFormat(hash);
    const auto epoch = [&] { return std::chrono::duration_cast<std::chrono::seconds>(now().time_since_epoch()).count(); };
    const bool guess = ocrApiGuessPaddlex(config.apiUrl);
    bool format = cached ? cached->paddlex : guess;
    std::int64_t rejected = cached && cached->rejectedAt > 0 && epoch() >= cached->rejectedAt &&
        epoch() - cached->rejectedAt < RejectionTtl ? cached->rejectedAt : 0;
    bool noWordBox = rejected != 0;
    int formatProbes = 1;
    std::array<bool, 2> bodyReady{};
    // Each of two formats gets at most one retry without its optional field.
    constexpr int attempts = 4;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (stop.stop_requested()) return {{}, "OCR cancelled"};
        if (Clock::now() >= deadline) return {{}, "OCR API timed out"};
        const auto body = temporary / (noWordBox ? "request-lines.json" : "request.json");
        if (format && !bodyReady[noWordBox]) {
            if (!bodyFile(image, body, stop, deadline, !noWordBox)) {
                if (stop.stop_requested()) return {{}, "OCR cancelled"};
                if (Clock::now() >= deadline) return {{}, "OCR API timed out"};
                return {{}, "OCR API request file creation failed"};
            }
            bodyReady[noWordBox] = true;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if (remaining.count() <= 0) return {{}, "OCR API timed out"};
        std::string options = "url = " + curlQuote(config.apiUrl) + "\nrequest = \"POST\"\nsilent\nshow-error\ngloboff\n"
            "proto = \"=http,https\"\nproto-redir = \"=http,https\"\nmax-filesize = 8388608\n"
            "write-out = \"\\n%{http_code}\"\nmax-time = " + curlQuote(std::to_string(remaining.count() / 1000.0)) + "\n";
        if (!config.apiKey.empty()) options += "header = " + curlQuote(std::string("Authorization: ") + (format ? "token " : "Bearer ") + config.apiKey) + "\n";
        if (format) options += "header = \"Content-Type: application/json\"\ndata-binary = " + curlQuote("@" + body.string()) + "\n";
        else {
            options += "form = " + curlQuote("file=@" + curlQuote(image.string()) + ";type=image/png;filename=image.png") + "\n";
            if (!noWordBox) options += "form = \"return_word_box=true\"\n";
        }
        // An unlinked, owner-only file backs stdin. No secret or body in argv.
        const auto inputPath = temporary / "curl-config";
        Fd input{open(inputPath.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)};
        if (input.value < 0) return {{}, "OCR API config creation failed"};
        unlink(inputPath.c_str());
        if (!writeAll(input.value, options) || lseek(input.value, 0, SEEK_SET) < 0) return {{}, "OCR API config write failed"};
        auto output = run(remaining, input.value);
        if (!output.error.empty()) {
            if (stop.stop_requested()) return {{}, "OCR cancelled"};
            if (output.exitCode == 28 || output.error.starts_with("OCR timed out")) return {{}, "OCR API timed out"};
            if (output.exitCode == 63 || output.error == "OCR stdout exceeds 8 MiB") return {{}, "OCR API response exceeds 8 MiB"};
            return {{}, redact("OCR API unreachable: " + output.error, config)};
        }
        const auto marker = output.out.rfind('\n');
        if (marker == output.out.npos || output.out.size() - marker != 4) return {{}, "OCR API returned an invalid HTTP status"};
        const auto status = output.out.substr(marker + 1);
        output.out.resize(marker);
        bool shapeMismatch = false, serviceError = false;
        auto result = parseOcrApiJson(output.out, &shapeMismatch, &serviceError);
        bool mismatch = shapeMismatch;
        if (!status.starts_with('2')) {
            mismatch = !serviceError && (status == "400" || status == "415" || status == "422");
            if (!serviceError) result = {{}, "OCR API returned HTTP " + status};
        }
        const bool rejectedWordField = serviceError &&
            result.error.find(format ? "returnWordBox" : "return_word_box") != std::string::npos;
        if ((status == "400" || status == "422" || (status.starts_with('2') && rejectedWordField)) &&
            !noWordBox && (!serviceError || rejectedWordField)) {
            // The response need not name the rejected field. Probe once without
            // it before changing formats; cache the exception only on success.
            noWordBox = true;
            rejected = epoch();
            continue;
        }
        if (result) { result.apiFormat = format ? "paddlex" : "simple"; cacheFormat(hash, format, rejected); return result; }
        if (!mismatch) { result.error = redact(result.error, config); return result; }
        if (cached) cacheFormat(hash, {});
        if (formatProbes++ == 2) return result;
        format = !format;
        noWordBox = false;
        rejected = 0;
    }
    return {{}, "OCR API format detection failed"};
}
} // namespace hyprcapture::detail
