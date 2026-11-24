#pragma once

#include <optional>
#include <functional>
#include <spawn.h>
#include <string>
#include <sys/types.h>
#include <vector>

namespace hyprcapture {

// Hyprland uses SA_NOCLDWAIT, so its plugins cannot retrieve child exit
// statuses with waitpid. A shell supervisor owns the child and reports its
// status over a private pipe without changing the compositor's signal policy.
// The child runs with umask 077; the compositor's umask is unchanged.
struct SupervisedProcess {
    pid_t pid = -1;
    int statusFd = -1;
    int spawnError = 0;
};

SupervisedProcess spawnSupervisedProcess(const std::string& shell, const std::vector<std::string>& args,
                                        char* const envp[], posix_spawn_file_actions_t& actions, bool ownProcessGroup = false);
// Consumes statusFd and reaps the supervisor when the caller permits zombies.
// For an owned process group, terminate() can end the wait and kill the
// group. The predicate is checked at least every 50 ms while the child lives.
std::optional<int> waitSupervisedProcess(SupervisedProcess& process, const std::function<bool()>& terminate = {});

} // namespace hyprcapture
