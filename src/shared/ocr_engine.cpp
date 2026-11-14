#include "shared/ocr_engine.hpp"
#include "shared/ocr_api.hpp"
#include "shared/process_environment.hpp"
#include "shared/supervised_process.hpp"
#include "shared/trusted_path.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

extern char** environ;

namespace hyprcapture {
namespace {
struct Fd {
    int value = -1;
    ~Fd() { reset(); }
    void reset() { if (value >= 0) close(value); value = -1; }
};

bool pipe(Fd& reader, Fd& writer) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) return false;
    // The fixed supervisor reserves fd 3. Keep every action source above it.
    reader.value = fcntl(fds[0], F_DUPFD_CLOEXEC, 4);
    writer.value = fcntl(fds[1], F_DUPFD_CLOEXEC, 4);
    close(fds[0]); close(fds[1]);
    return reader.value >= 0 && writer.value >= 0;
}

std::vector<std::string> searchDirectories() {
    std::vector<std::string> dirs;
    const auto add = [&](std::string_view directory) {
        if (directory.empty() || directory.front() != '/') return;
        std::error_code ec;
        const auto canonical = std::filesystem::canonical(directory, ec);
        if (!ec && security::parentChainTrusted(canonical / "program")) dirs.push_back(canonical.string());
    };
    if (const char* path = std::getenv("PATH")) {
        std::string_view remaining(path);
        while (!remaining.empty()) {
            const auto end = remaining.find(':');
            const auto part = remaining.substr(0, end);
            // No cwd search, and no group/other-writable path components.
            add(part);
            if (end == remaining.npos) break;
            remaining.remove_prefix(end + 1);
        }
    }
#ifdef HYPRCAPTURE_TRUSTED_BIN_DIRS
    std::string_view extra(HYPRCAPTURE_TRUSTED_BIN_DIRS);
    while (!extra.empty()) {
        const auto end = extra.find(':');
        add(extra.substr(0, end));
        if (end == extra.npos) break;
        extra.remove_prefix(end + 1);
    }
#endif
    for (const auto* dir : {"/run/current-system/sw/bin", "/nix/var/nix/profiles/default/bin", "/usr/local/bin", "/usr/bin", "/bin"})
        add(dir);
    return dirs;
}

std::string executable(const std::string& program) {
    if (program.find('/') != program.npos) {
        const auto trusted = security::trustedExecutablePath(program);
        return trusted.value_or("");
    }
    for (const auto& dir : searchDirectories())
        if (auto trusted = security::trustedExecutablePath((std::filesystem::path(dir) / program).string())) return *trusted;
    return {};
}

std::vector<std::string> environment() {
    std::vector<std::string> env;
    for (char** entry = environ; *entry; ++entry) {
        const std::string_view value(*entry);
        const auto name = value.substr(0, value.find('='));
        if (desktopEnvironmentNameAllowed(name)) env.emplace_back(value);
    }
    std::string path = "PATH=";
    for (const auto& dir : searchDirectories()) { if (path.size() > 5) path += ':'; path += dir; }
    env.push_back(std::move(path));
    return env;
}

detail::OcrProcessOutput run(std::vector<std::string> args, std::chrono::milliseconds timeout, std::stop_token stop, int inputFd = -1, std::size_t outputLimit = OcrMaxBytes) {
    if (stop.stop_requested()) return {{}, "OCR cancelled"};
    args[0] = executable(args[0]);
    const auto shell = executable("sh");
    if (args[0].empty() || shell.empty()) return {{}, "OCR executable not found or not trusted"};
    Fd output, outputWriter, errors, errorWriter;
    if (!pipe(output, outputWriter) || !pipe(errors, errorWriter)) return {{}, "OCR pipe creation failed"};
    if (fcntl(output.value, F_SETFL, O_NONBLOCK) < 0 || fcntl(errors.value, F_SETFL, O_NONBLOCK) < 0)
        return {{}, "OCR nonblocking pipe setup failed"};
    auto env = environment();
    std::vector<char*> envp;
    for (auto& entry : env) envp.push_back(entry.data());
    envp.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (error) return {{}, "OCR spawn actions failed: " + std::string(std::strerror(error))};
    error = posix_spawn_file_actions_adddup2(&actions, outputWriter.value, STDOUT_FILENO);
    if (!error) error = posix_spawn_file_actions_adddup2(&actions, errorWriter.value, STDERR_FILENO);
    if (!error) error = inputFd >= 0 ? posix_spawn_file_actions_adddup2(&actions, inputFd, STDIN_FILENO) :
                                     posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    SupervisedProcess process;
    if (!error) process = spawnSupervisedProcess(shell, args, envp.data(), actions, true);
    posix_spawn_file_actions_destroy(&actions);
    if (error || process.spawnError) return {{}, "OCR spawn failed: " + std::string(std::strerror(error ? error : process.spawnError))};
    outputWriter.reset(); errorWriter.reset();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string stdoutText, stderrText, failure;
    bool statusClosed = false;
    while (output.value >= 0 || errors.value >= 0 || !statusClosed) {
        if (stop.stop_requested()) { failure = "OCR cancelled"; break; }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) { failure = "OCR timed out (ocr_timeout=" + (timeout.count() % 1000 == 0 ? std::to_string(timeout.count() / 1000) : std::to_string(timeout.count() / 1000.0)) + "s)"; break; }
        pollfd fds[] = {{output.value, POLLIN, 0}, {errors.value, POLLIN, 0}, {statusClosed ? -1 : process.statusFd, 0, 0}};
        const int ready = poll(fds, 3, static_cast<int>(std::min<std::int64_t>(remaining, 100)));
        if (ready < 0) {
            if (errno == EINTR) continue;
            failure = "OCR poll failed"; break;
        }
        if (fds[2].revents & POLLHUP) statusClosed = true;
        for (int i = 0; i < 2; ++i) {
            auto& fd = i == 0 ? output : errors;
            auto& text = i == 0 ? stdoutText : stderrText;
            if (fd.value < 0 || !fds[i].revents) continue;
            std::array<char, 8192> buffer;
            const auto count = read(fd.value, buffer.data(), buffer.size());
            if (count > 0) {
                const auto limit = i == 0 ? outputLimit : std::size_t(64 * 1024);
                const auto available = limit - text.size();
                text.append(buffer.data(), std::min<std::size_t>(count, available));
                if (static_cast<std::size_t>(count) > available) failure = i == 0 ? "OCR stdout exceeds 8 MiB" : "OCR stderr exceeds 64 KiB";
            } else if (count == 0) fd.reset();
            else if (errno != EAGAIN && errno != EINTR) { failure = "OCR pipe read failed"; fd.reset(); }
        }
        if (!failure.empty()) break;
    }
    // Also remove descendants that closed stdio before the engine returned.
    kill(-process.pid, SIGKILL);
    const auto status = waitSupervisedProcess(process);
    if (failure.empty() && (!status || *status != 0))
        failure = status ? "OCR exited with status " + std::to_string(*status) : "OCR supervisor returned no exit status";
    if (!failure.empty()) {
        const auto end = stderrText.find_first_of("\r\n");
        const auto first = stderrText.substr(0, std::min<std::size_t>(end, 1024));
        if (!first.empty()) failure += ": " + first;
        return {{}, std::move(failure), status.value_or(-1)};
    }
    return {std::move(stdoutText), {}};
}

struct PrivateDirectory {
    std::filesystem::path path;
    ~PrivateDirectory() { if (!path.empty()) { std::error_code ec; std::filesystem::remove_all(path, ec); } }
    bool create() {
        // Prefer the caller's private runtime directory. /tmp is safe with
        // mkdtemp's atomic 0700 creation, independent of the process umask.
        std::string base = "/tmp";
        if (const char* runtime = std::getenv("XDG_RUNTIME_DIR")) {
            struct stat st{};
            if (*runtime == '/' && lstat(runtime, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == geteuid() && !(st.st_mode & 0077))
                base = runtime;
        }
        auto pattern = base + "/hyprcapture-ocr-XXXXXX";
        if (!mkdtemp(pattern.data())) return false;
        path = pattern;
        return true;
    }
};

OcrResponse readPaddle(const std::filesystem::path& file) {
    Fd fd{open(file.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    struct stat st{};
    if (fd.value < 0 || fstat(fd.value, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid())
        return {{}, "paddleocr result is missing or not a regular owned file: " + file.filename().string()};
    if (st.st_size < 0 || st.st_size > static_cast<off_t>(OcrMaxBytes)) return {{}, "paddleocr JSON exceeds 8 MiB"};
    std::string input;
    std::array<char, 8192> buffer;
    for (;;) {
        const auto count = read(fd.value, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return {{}, "paddleocr result read failed"};
        if (!count) break;
        if (input.size() + count > OcrMaxBytes) return {{}, "paddleocr JSON exceeds 8 MiB"};
        input.append(buffer.data(), count);
    }
    return parsePaddleOcrJson(input);
}
} // namespace

detail::OcrProcessOutput detail::ocrReadHyprlandOption(std::string_view name) {
    return run({"hyprctl", "getoption", "-j", std::string(name)}, std::chrono::seconds(5), {});
}

OcrCommand splitOcrCommand(std::string_view command, std::string_view image) {
    if (command.size() > 65536 || command.find('\0') != command.npos || image.find('\0') != image.npos)
        return {{}, "ocr_command is too long or contains NUL"};
    OcrCommand result;
    std::string word;
    char quote = 0;
    bool escaped = false, started = false;
    auto finish = [&] {
        if (!started) return;
        if (word.starts_with("~/")) {
            if (const char* home = std::getenv("HOME")) word.replace(0, 1, home);
        }
        std::size_t at = 0;
        while ((at = word.find("{image}", at)) != word.npos) {
            word.replace(at, 7, image); at += image.size();
        }
        result.argv.push_back(std::move(word)); word.clear(); started = false;
    };
    for (char c : command) {
        if (escaped) { word += c; escaped = false; continue; }
        if (c == '\\' && quote != '\'') { escaped = true; started = true; continue; }
        if (quote) {
            if (c == quote) quote = 0;
            else word += c;
            continue;
        }
        if (c == '\'' || c == '"') { quote = c; started = true; }
        else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') finish();
        else { word += c; started = true; }
    }
    if (quote || escaped) return {{}, "ocr_command has an unmatched quote or trailing backslash"};
    finish();
    if (result.argv.empty() || result.argv[0].empty()) return {{}, "ocr_command must name an executable"};
    return result;
}

OcrResponse runOcrEngine(const std::filesystem::path& png, const OcrConfig& config, std::stop_token stop, bool ignoreApiCache) {
    if (stop.stop_requested()) return {{}, "OCR cancelled"};
    if (const auto error = validateOcrConfig(config); !error.empty()) return {{}, error};
    if (config.mode == "off") return {{}, "OCR is disabled: set ocr_mode"};
    std::error_code ec;
    const auto image = std::filesystem::absolute(png, ec);
    struct stat st{};
    if (ec || image.string().find('\0') != std::string::npos || stat(image.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || access(image.c_str(), R_OK) != 0)
        return {{}, "OCR input PNG is not a readable regular file"};
    std::vector<std::string> args;
    PrivateDirectory temporary;
    if (config.engine == "paddleocr" && !config.apiUrl.empty()) {
        if (executable("curl").empty()) return {{}, "OCR API needs curl"};
        if (!temporary.create()) return {{}, "OCR API private directory creation failed"};
        return detail::runOcrApi(image, config, temporary.path, stop, [&](auto remaining, int input) {
            return run({"curl", "-q", "-K", "-"}, remaining, stop, input, OcrMaxBytes + 4);
        }, std::chrono::system_clock::now, ignoreApiCache);
    }
    if (config.engine == "tesseract") args = {"tesseract", image.string(), "stdout", "-l", config.languages, "tsv"};
    else if (config.engine == "paddleocr") {
        if (!temporary.create()) return {{}, "paddleocr private directory creation failed"};
        args = {"paddleocr", "ocr", "-i", image.string(), "--save_path", temporary.path.string(),
                "--use_doc_orientation_classify", "False", "--use_doc_unwarping", "False",
                "--use_textline_orientation", "False", "--enable_mkldnn", "False", "--return_word_box", "True"};
        // Paddle 3.x's recognition pipeline does not accept Tesseract language
        // expressions. Its CLI/model selection is described in docs/ocr.md.
    } else {
        auto parsed = splitOcrCommand(config.command, image.string());
        if (!parsed) return {{}, parsed.error};
        args = std::move(parsed.argv);
    }
    auto output = run(std::move(args), std::chrono::seconds(config.timeout), stop);
    if (!output.error.empty()) return {{}, output.error};
    if (config.engine == "tesseract") return parseTesseractTsv(output.out);
    if (config.engine == "paddleocr") return readPaddle(temporary.path / (image.stem().string() + "_res.json"));
    if (config.output == "json") return parseOcrJsonLines(output.out);
    OcrResponse response;
    response.results.push_back({std::move(output.out), {}, {}, OcrLevel::Line});
    return response;
}

} // namespace hyprcapture
