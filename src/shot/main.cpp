#include "shared/shot_channel.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/wait.h>
#include <vector>

extern char** environ;
namespace {
using Clock = std::chrono::steady_clock;
volatile sig_atomic_t interrupted = 0;
void signalHandler(int signal) { interrupted = signal; }
struct Failure : std::runtime_error {
    int code;
    Failure(int code, const std::string& reason) : std::runtime_error(reason), code(code) {}
};
struct Fd {
    int value = -1;
    Fd() = default;
    explicit Fd(int fd) : value(fd) {}
    ~Fd() { if (value >= 0) close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};
struct Temporary {
    std::string path;
    bool directory = false;
    ~Temporary() { if (!path.empty()) { if (directory) rmdir(path.c_str()); else unlink(path.c_str()); } }
};
struct Options { std::string mode = "region", output = "-"; bool editor = false; double timeout = 0; };
Options parse(int argc, char** argv) {
    Options options;
    bool mode = false, output = false, timeout = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--editor" && !options.editor) options.editor = true;
        else if (arg == "-o" && !output && i + 1 < argc) {
            options.output = argv[++i]; output = true;
            if (options.output.empty()) throw Failure(64, "output path must not be empty");
        } else if (arg == "--timeout" && !timeout && i + 1 < argc) {
            char* end = nullptr; const char* value = argv[++i]; errno = 0;
            options.timeout = std::strtod(value, &end); timeout = true;
            if (errno || end == value || *end || !std::isfinite(options.timeout) || options.timeout <= 0)
                throw Failure(64, "timeout must be a positive finite number of seconds");
        } else if ((arg == "region" || arg == "fullscreen" || arg == "window") && !mode) {
            options.mode = arg; mode = true;
        } else throw Failure(64, "usage: hyprcapture-shot [region|fullscreen|window] [-o PATH|-o -] [--editor] [--timeout SEC]");
    }
    if (options.output == "-" && isatty(STDOUT_FILENO)) throw Failure(64, "refusing to write PNG to a terminal; use -o PATH");
    return options;
}
std::string luaString(const std::string& value) {
    std::string quoted = "\"";
    for (unsigned char c : value) {
        // Decimal escapes keep all bytes unambiguous, including quotes/newlines.
        if (c < 32 || c > 126 || c == '\\' || c == '"') {
            char escape[5]; std::snprintf(escape, sizeof(escape), "\\%03u", c); quoted += escape;
        } else quoted += c;
    }
    return quoted + '"';
}
struct Hyprctl {
    pid_t pid = -1;
    Fd reply;
    std::string message;
    bool done = false;
    int status = 0;
    explicit Hyprctl(const std::string& expression) {
        int descriptors[2];
        if (pipe2(descriptors, O_CLOEXEC | O_NONBLOCK)) throw Failure(2, "could not create hyprctl pipe");
        reply.value = descriptors[0]; Fd writer(descriptors[1]);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, writer.value, STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, writer.value, STDERR_FILENO);
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        std::array<char*, 4> args{const_cast<char*>("hyprctl"), const_cast<char*>("eval"), const_cast<char*>(expression.c_str()), nullptr};
        const int result = posix_spawnp(&pid, "hyprctl", &actions, nullptr, args.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        if (result) { pid = -1; throw Failure(2, "could not run hyprctl: " + std::string(std::strerror(result))); }
    }
    ~Hyprctl() { if (pid > 0 && !done) { kill(pid, SIGKILL); while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {} } }
    void update() {
        char buffer[4096]; ssize_t size;
        while ((size = read(reply.value, buffer, sizeof(buffer))) > 0) {
            if (message.size() < 8192) message.append(buffer, std::min<size_t>(size, 8192 - message.size()));
        }
        if (!done && waitpid(pid, &status, WNOHANG) == pid) done = true;
    }
    bool ok() const { return done && WIFEXITED(status) && WEXITSTATUS(status) == 0; }
    std::string reason() const {
        auto text = message;
        for (auto& c : text) if (static_cast<unsigned char>(c) < 32) c = ' ';
        if (text.empty()) text = "plugin not loaded or hyprctl failed";
        return text;
    }
};
struct Cancellation {
    std::string socket;
    int fd = -1;
    bool armed = false;
    ~Cancellation() {
        if (!armed) return;
        if (fd >= 0) { send(fd, "cancel", 6, MSG_NOSIGNAL | MSG_DONTWAIT); shutdown(fd, SHUT_WR); }
        // Scoped plugin cancellation also covers a helper still starting Qt.
        try {
            Hyprctl cancel("hl.plugin.hyprcapture.shot_cancel(" + luaString(socket) + ")");
            const auto end = Clock::now() + std::chrono::milliseconds(300);
            do { cancel.update(); if (cancel.done) break; poll(nullptr, 0, 10); } while (Clock::now() < end);
        } catch (...) {}
    }
};
void checkWait(Clock::time_point deadline) {
    if (interrupted) throw Failure(interrupted == SIGINT ? 1 : 2, "capture interrupted");
    if (Clock::now() >= deadline) throw Failure(2, "capture timed out");
}
void writeFile(int fd, const char* data, size_t size) {
    while (size) {
        const auto count = write(fd, data, size);
        if (count < 0 && errno == EINTR) { if (interrupted) throw Failure(interrupted == SIGINT ? 1 : 2, "capture interrupted"); continue; }
        if (count <= 0) throw Failure(2, "could not write output: " + std::string(std::strerror(errno)));
        data += count; size -= count;
    }
}
int run(const Options& options) {
    const auto started = Clock::now();
    auto deadline = Clock::time_point::max();
    if (options.timeout > 0 && options.timeout < std::chrono::duration<double>(deadline - started).count())
        deadline = started + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(options.timeout));
    auto root = hyprcapture::shot::runtimeRoot();
    if (root.empty()) throw Failure(2, "private runtime directory unavailable");
    std::string name = root + "/shot-XXXXXX";
    if (!mkdtemp(name.data())) throw Failure(2, "could not create private shot directory");
    Temporary directory{name, true}, socketPath{name + "/result.sock"};
    Fd server(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (server.value < 0) throw Failure(2, "could not create result socket");
    sockaddr_un address{}; address.sun_family = AF_UNIX;
    if (socketPath.path.size() >= sizeof(address.sun_path)) throw Failure(2, "result socket path too long");
    std::memcpy(address.sun_path, socketPath.path.c_str(), socketPath.path.size() + 1);
    if (bind(server.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(server.value, 1))
        throw Failure(2, "could not listen on result socket");
    // Stage all bytes privately, so cancel/crash/truncation never reaches stdout.
    Fd spool(open((name + "/payload").c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600));
    if (spool.value < 0) throw Failure(2, "could not create result spool");
    unlink((name + "/payload").c_str());
    Fd connection;
    Cancellation cancellation{socketPath.path};
    Hyprctl trigger("hl.plugin.hyprcapture.shot(" + luaString(socketPath.path) + "," + luaString(options.mode) + "," + (options.editor ? "true" : "false") + ")");
    cancellation.armed = true;
    std::string header, error;
    uint64_t received = 0, expected = 0;
    unsigned char status = 255;
    bool eof = false;
    for (;;) {
        checkWait(deadline);
        trigger.update();
        if (trigger.done && !trigger.ok()) throw Failure(2, trigger.reason());
        if (connection.value < 0) {
            connection.value = accept4(server.value, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (connection.value >= 0) {
                ucred peer{}; socklen_t size = sizeof(peer);
                if (getsockopt(connection.value, SOL_SOCKET, SO_PEERCRED, &peer, &size) || peer.uid != geteuid()) throw Failure(2, "untrusted result peer");
                cancellation.fd = connection.value;
            } else if (errno != EAGAIN && errno != EINTR && errno != EWOULDBLOCK) throw Failure(2, "could not accept result connection");
            // Lua eval may report errors with exit 0; a successful shot connects synchronously.
            if (connection.value < 0 && trigger.done) throw Failure(2, trigger.reason());
        }
        if (connection.value >= 0 && !eof) {
            char buffer[65536];
            const auto count = recv(connection.value, buffer, sizeof(buffer), MSG_DONTWAIT);
            if (count == 0) eof = true;
            else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) throw Failure(2, "result channel failed");
            else if (count > 0) {
                size_t offset = 0;
                if (header.size() < 9) {
                    offset = std::min<size_t>(9 - header.size(), count); header.append(buffer, offset);
                    if (header.size() == 9) {
                        status = header[0]; expected = hyprcapture::shot::payloadSize(header);
                        if (status > 2 || expected > 1024ULL * 1024 * 1024 || (status == 1 && expected) || (status == 2 && expected > 8192))
                            throw Failure(2, "invalid result header");
                    }
                }
                const auto size = count - offset;
                if (received + size > expected) throw Failure(2, "unexpected result bytes");
                if (status == 0) writeFile(spool.value, buffer + offset, size);
                else if (status == 2) error.append(buffer + offset, size);
                received += size;
            }
        }
        if (eof && trigger.done) break;
        pollfd descriptors[]{{connection.value >= 0 && !eof ? connection.value : server.value, POLLIN, 0}, {trigger.done ? -1 : trigger.reply.value, POLLIN, 0}};
        if (poll(descriptors, 2, 10) < 0 && errno != EINTR) throw Failure(2, "result wait failed");
    }
    if (header.size() != 9 || received != expected) throw Failure(2, "screenshot helper exited without a complete result");
    if (status == 1) { cancellation.armed = false; return 1; }
    if (status == 2) throw Failure(2, error.empty() ? "screenshot helper failed" : error);
    std::array<unsigned char, 8> signature{};
    if (pread(spool.value, signature.data(), 8, 0) != 8 || signature != std::array<unsigned char,8>{137,80,78,71,13,10,26,10})
        throw Failure(2, "screenshot helper returned invalid PNG");
    checkWait(deadline);
    Temporary output;
    Fd outputFd;
    int destination = STDOUT_FILENO;
    if (options.output != "-") {
        auto path = std::filesystem::path(options.output);
        auto parent = path.parent_path(); if (parent.empty()) parent = ".";
        output.path = (parent / ".hyprcapture-shot-XXXXXX").string();
        outputFd.value = mkstemp(output.path.data());
        if (outputFd.value < 0) { output.path.clear(); throw Failure(2, "could not create output temporary file"); }
        destination = outputFd.value;
    }
    if (lseek(spool.value, 0, SEEK_SET) < 0) throw Failure(2, "could not read staged PNG");
    for (;;) {
        checkWait(deadline);
        char buffer[65536]; const auto size = read(spool.value, buffer, sizeof(buffer));
        if (!size) break;
        if (size < 0) throw Failure(2, "could not read staged PNG");
        writeFile(destination, buffer, size);
    }
    if (options.output != "-") {
        if (fsync(destination)) throw Failure(2, "could not sync output file");
        checkWait(deadline);
        if (rename(output.path.c_str(), options.output.c_str())) throw Failure(2, "could not publish output file");
        output.path.clear();
    }
    cancellation.armed = false;
    return 0;
}
}
int main(int argc, char** argv) {
    umask(0077);
    struct sigaction action{}; action.sa_handler = signalHandler; sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr); sigaction(SIGTERM, &action, nullptr);
    signal(SIGPIPE, SIG_IGN);
    try { return run(parse(argc, argv)); }
    catch (const Failure& failure) {
        if (failure.code != 1) {
            std::string reason = failure.what();
            for (auto& c : reason) if (static_cast<unsigned char>(c) < 32) c = ' ';
            std::fprintf(stderr, "hyprcapture-shot: %s\n", reason.c_str());
        }
        return failure.code;
    } catch (const std::exception& error) { std::fprintf(stderr, "hyprcapture-shot: internal error\n"); return 2; }
}
