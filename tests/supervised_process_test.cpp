#include "shared/supervised_process.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

std::string shell = "/bin/sh";
volatile sig_atomic_t signalExit = 0;

void exitOnSignal(int) {
    _exit(signalExit);
}

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

std::optional<int> run(const std::vector<std::string>& args) {
    posix_spawn_file_actions_t actions;
    require(posix_spawn_file_actions_init(&actions) == 0, "file actions");
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    auto process = hyprcapture::spawnSupervisedProcess(shell, args, environ, actions);
    posix_spawn_file_actions_destroy(&actions);
    require(process.spawnError == 0, "spawn supervisor");
    return hyprcapture::waitSupervisedProcess(process);
}

void checkGroupStop(const std::vector<std::string>& args, int signal, int expected) {
    int ready[2];
    require(pipe2(ready, O_CLOEXEC) == 0, "group readiness pipe");
    posix_spawn_file_actions_t actions;
    require(posix_spawn_file_actions_init(&actions) == 0, "group actions");
    posix_spawn_file_actions_adddup2(&actions, ready[1], STDOUT_FILENO);
    auto grouped = hyprcapture::spawnSupervisedProcess(shell, args, environ, actions, true);
    posix_spawn_file_actions_destroy(&actions);
    close(ready[1]);
    require(grouped.spawnError == 0, "spawn process group");
    char readiness[5];
    require(read(ready[0], readiness, sizeof(readiness)) == 5, "group child ready");
    close(ready[0]);
    require(kill(-grouped.pid, signal) == 0, "signal recording process group");
    const auto result = hyprcapture::waitSupervisedProcess(grouped);
    if (result != expected)
        std::cerr << "group signal " << signal << ": expected " << expected
                  << ", got " << (result ? std::to_string(*result) : "no report") << '\n';
    require(result == expected, "group stop preserves exit report");
}

int main(int argc, char** argv) {
    if (argc == 4 && std::string(argv[1]) == "--signal-child") {
        const int signal = std::atoi(argv[2]);
        signalExit = std::atoi(argv[3]);
        struct sigaction action{};
        if (signalExit < 0) {
            require(sigaction(signal, nullptr, &action) == 0 && action.sa_handler == SIG_DFL,
                    "supervisor must not make the child ignore stop signals");
        } else {
            action.sa_handler = exitOnSignal;
            sigemptyset(&action.sa_mask);
            require(sigaction(signal, &action, nullptr) == 0, "child signal disposition");
        }
        require(write(STDOUT_FILENO, "ready", 5) == 5, "child readiness");
        for (;;) pause();
    }
    if (argc == 2) shell = argv[1];
    char executable[4096];
    const auto length = readlink("/proc/self/exe", executable, sizeof(executable));
    require(length > 0 && length < static_cast<ssize_t>(sizeof(executable)), "test executable path");
    const std::string self(executable, length);
    // Reproduce the exact compositor policy and prove ordinary waitpid loses
    // even a successful exit before exercising the replacement.
    struct sigaction original{}, action{};
    action.sa_handler = SIG_DFL;
    action.sa_flags = SA_NOCLDWAIT;
    sigemptyset(&action.sa_mask);
    require(sigaction(SIGCHLD, &action, &original) == 0, "set SA_NOCLDWAIT");
    const auto child = fork();
    require(child >= 0, "fork baseline");
    if (child == 0)
        _exit(0);
    int status = 0;
    require(waitpid(child, &status, 0) == -1 && errno == ECHILD, "baseline must lose exit status");

    for (int i = 0; i < 8; ++i) {
        require(run({shell, "-c", "exit 0"}) == 0, "successful exit under SA_NOCLDWAIT");
        require(run({shell, "-c", "exit 7"}) == 7, "failed exit under SA_NOCLDWAIT");
    }
    require(run({shell, "-c", "kill -TERM $$"}).value_or(0) != 0, "signal death is failure");
    require(run({"/nonexistent/hyprcapture-encoder"}) == 127, "exec failure is failure");
    require(!run({shell, "-c", "kill -KILL \"$PPID\""}).has_value(), "missing supervisor report is not success");
    const std::string literal = "a b; $(exit 99) `exit 98` ' \"";
    require(run({shell, "-c", "test \"$1\" = \"$2\"", "test", literal, literal}) == 0, "arguments remain literal");

    // Existing caller stdin redirection must survive the private status fd.
    int input[2];
    require(pipe2(input, O_CLOEXEC) == 0, "input pipe");
    require(write(input[1], "frame\n", 6) == 6, "write input");
    close(input[1]);
    posix_spawn_file_actions_t actions;
    require(posix_spawn_file_actions_init(&actions) == 0, "input actions");
    posix_spawn_file_actions_adddup2(&actions, input[0], STDIN_FILENO);
    posix_spawn_file_actions_addclose(&actions, input[0]);
    auto process = hyprcapture::spawnSupervisedProcess(shell, {shell, "-c", "read line; test \"$line\" = frame"}, environ, actions);
    posix_spawn_file_actions_destroy(&actions);
    close(input[0]);
    require(process.spawnError == 0 && hyprcapture::waitSupervisedProcess(process) == 0, "raw input preserved");

    // GSR must be signalled as a group without killing the compositor, and the
    // supervisor must still report the child's result under SA_NOCLDWAIT.
    checkGroupStop({shell, "-c", "trap 'exit 0' INT; printf ready; while :; do sleep 1; done"}, SIGINT, 0);
    // A native child makes success, failure and signal death independent of
    // the test shell's handling of an interrupted sleep or shell builtin.
    for (const int signal : {SIGINT, SIGTERM}) {
        for (const int code : {0, 7, -1})
            checkGroupStop({self, "--signal-child", std::to_string(signal), std::to_string(code)},
                           signal, code < 0 ? 128 + signal : code);
    }

    require(sigaction(SIGCHLD, &original, nullptr) == 0, "restore signals");
    require(run({shell, "-c", "exit 23"}) == 23, "normal parent policy");
    std::cout << "supervised process tests passed\n";
}
