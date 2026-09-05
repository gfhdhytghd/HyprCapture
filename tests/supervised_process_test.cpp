#include "shared/supervised_process.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

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
    auto process = hyprcapture::spawnSupervisedProcess("/bin/sh", args, environ, actions);
    posix_spawn_file_actions_destroy(&actions);
    require(process.spawnError == 0, "spawn supervisor");
    return hyprcapture::waitSupervisedProcess(process);
}

int main() {
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
        require(run({"/bin/sh", "-c", "exit 0"}) == 0, "successful exit under SA_NOCLDWAIT");
        require(run({"/bin/sh", "-c", "exit 7"}) == 7, "failed exit under SA_NOCLDWAIT");
    }
    require(run({"/bin/sh", "-c", "kill -TERM $$"}).value_or(0) != 0, "signal death is failure");
    require(run({"/nonexistent/hyprcapture-encoder"}) == 127, "exec failure is failure");
    require(!run({"/bin/sh", "-c", "kill -KILL \"$PPID\""}).has_value(), "missing supervisor report is not success");
    const std::string literal = "a b; $(exit 99) `exit 98` ' \"";
    require(run({"/bin/sh", "-c", "test \"$1\" = \"$2\"", "test", literal, literal}) == 0, "arguments remain literal");

    // Existing caller stdin redirection must survive the private status fd.
    int input[2];
    require(pipe2(input, O_CLOEXEC) == 0, "input pipe");
    require(write(input[1], "frame\n", 6) == 6, "write input");
    close(input[1]);
    posix_spawn_file_actions_t actions;
    require(posix_spawn_file_actions_init(&actions) == 0, "input actions");
    posix_spawn_file_actions_adddup2(&actions, input[0], STDIN_FILENO);
    posix_spawn_file_actions_addclose(&actions, input[0]);
    auto process = hyprcapture::spawnSupervisedProcess("/bin/sh", {"/bin/sh", "-c", "read line; test \"$line\" = frame"}, environ, actions);
    posix_spawn_file_actions_destroy(&actions);
    close(input[0]);
    require(process.spawnError == 0 && hyprcapture::waitSupervisedProcess(process) == 0, "raw input preserved");

    require(sigaction(SIGCHLD, &original, nullptr) == 0, "restore signals");
    require(run({"/bin/sh", "-c", "exit 23"}) == 23, "normal parent policy");
    std::cout << "supervised process tests passed\n";
}
