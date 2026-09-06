#include "supervised_process.hpp"

#include <cerrno>
#include <charconv>
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace hyprcapture {

SupervisedProcess spawnSupervisedProcess(const std::string& shell, const std::vector<std::string>& args,
                                        char* const envp[], posix_spawn_file_actions_t& actions, bool ownProcessGroup) {
    if (args.empty())
        return {.spawnError = EINVAL};
    int pipeFds[2];
    if (pipe2(pipeFds, O_CLOEXEC) != 0)
        return {.spawnError = errno};
    // Keep the write end away from the supervisor's reserved fd 3.
    const int writer = fcntl(pipeFds[1], F_DUPFD_CLOEXEC, 4);
    const int duplicateError = errno;
    close(pipeFds[1]);
    if (writer < 0) {
        close(pipeFds[0]);
        return {.spawnError = duplicateError};
    }

    // Only this fixed script is shell code. Every program argument stays an
    // argv element, including paths containing quotes or shell substitutions.
    std::vector<std::string> command{shell, "-c", "\"$@\" 3>&-; result=$?; printf '%s\\n' \"$result\" >&3; exit \"$result\"",
                                     "hyprcapture-supervisor"};
    command.insert(command.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& arg : command)
        argv.push_back(arg.data());
    argv.push_back(nullptr);

    int error = posix_spawn_file_actions_adddup2(&actions, writer, 3);
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ) && __GLIBC_PREREQ(2, 34)
    if (!error)
        error = posix_spawn_file_actions_addclosefrom_np(&actions, 4);
#else
    if (!error)
        error = posix_spawn_file_actions_addclose(&actions, writer);
    if (!error && pipeFds[0] != 3)
        error = posix_spawn_file_actions_addclose(&actions, pipeFds[0]);
#endif
    posix_spawnattr_t attributes;
    const int attrError = posix_spawnattr_init(&attributes);
    if (!error)
        error = attrError;
    pid_t pid = -1;
    if (!error) {
        sigset_t defaults;
        sigemptyset(&defaults);
        sigaddset(&defaults, SIGCHLD);
        if (ownProcessGroup) {
            sigaddset(&defaults, SIGINT);
            sigaddset(&defaults, SIGTERM);
        }
        error = posix_spawnattr_setsigdefault(&attributes, &defaults);
        if (!error)
            error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGDEF | (ownProcessGroup ? POSIX_SPAWN_SETPGROUP : 0));
        if (!error && ownProcessGroup)
            error = posix_spawnattr_setpgroup(&attributes, 0);
        if (!error)
            error = posix_spawn(&pid, shell.c_str(), &actions, &attributes, argv.data(), envp);
    }
    if (!attrError)
        posix_spawnattr_destroy(&attributes);
    close(writer);
    if (error) {
        close(pipeFds[0]);
        return {.spawnError = error};
    }
    return {.pid = pid, .statusFd = pipeFds[0]};
}

std::optional<int> waitSupervisedProcess(SupervisedProcess& process) {
    std::optional<int> result;
    if (process.statusFd >= 0) {
        char buffer[5]{};
        std::size_t size = 0;
        while (size < sizeof(buffer)) {
            const auto count = read(process.statusFd, buffer + size, sizeof(buffer) - size);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                break;
            size += count;
        }
        close(process.statusFd);
        process.statusFd = -1;
        if (size >= 2 && size <= 4 && buffer[size - 1] == '\n') {
            int code = -1;
            const auto parsed = std::from_chars(buffer, buffer + size - 1, code);
            if (parsed.ec == std::errc{} && parsed.ptr == buffer + size - 1 && code >= 0 && code <= 255)
                result = code;
        }
    }
    if (process.pid > 0) {
        while (waitpid(process.pid, nullptr, 0) < 0 && errno == EINTR) {}
        process.pid = -1;
    }
    return result;
}

} // namespace hyprcapture
