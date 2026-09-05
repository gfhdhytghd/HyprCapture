#pragma once

#include <optional>
#include <spawn.h>
#include <string>
#include <sys/types.h>
#include <vector>

namespace hyprcapture {

// Hyprland uses SA_NOCLDWAIT, so its plugins cannot retrieve child exit
// statuses with waitpid. A shell supervisor owns the child and reports its
// status over a private pipe without changing the compositor's signal policy.
struct SupervisedProcess {
    pid_t pid = -1;
    int statusFd = -1;
    int spawnError = 0;
};

SupervisedProcess spawnSupervisedProcess(const std::string& shell, const std::vector<std::string>& args,
                                        char* const envp[], posix_spawn_file_actions_t& actions);
// Consumes statusFd and reaps the supervisor when the caller permits zombies.
std::optional<int> waitSupervisedProcess(SupervisedProcess& process);

} // namespace hyprcapture
