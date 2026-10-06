#include "shared/session_startup.hpp"

#include <fcntl.h>
#include <iostream>
#include <spawn.h>
#include <sys/wait.h>
#include <vector>

extern char** environ;

namespace {
void require(bool ok, const char* message) {
    if (!ok) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
}

int main(int argc, char** argv) {
    using namespace hyprcapture;
    const std::string path = "/tmp/hyprcapture-1000/test/session.json";
    if (argc == 2 && std::string_view(argv[1]) == "--receive") {
        const auto received = receiveStartupPath(STDIN_FILENO);
        return received == path ? 0 : 1;
    }

    // Exercise the same dup-to-stdin/close-from spawn ordering as the plugin.
    {
        SessionStartupChannel channel;
        require(channel.valid(), "create channel");
        require(fcntl(channel.childFd(), F_GETFD) & FD_CLOEXEC, "channel is close-on-exec by default");
        posix_spawn_file_actions_t actions;
        require(posix_spawn_file_actions_init(&actions) == 0, "init spawn actions");
        require(posix_spawn_file_actions_adddup2(&actions, channel.childFd(), STDIN_FILENO) == 0, "dup startup fd");
#ifdef __GLIBC__
        require(posix_spawn_file_actions_addclosefrom_np(&actions, 3) == 0, "close unrelated fds");
#endif
        char receive[] = "--receive";
        char* childArgs[] = {argv[0], receive, nullptr};
        pid_t pid = -1;
        require(posix_spawn(&pid, argv[0], &actions, nullptr, childArgs, environ) == 0, "spawn waiting child");
        posix_spawn_file_actions_destroy(&actions);
        channel.closeChild();
        require(channel.publish(path), "publish while child initializes");
        int status = 0;
        require(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0, "child receives startup path after exec");
    }
    {
        SessionStartupChannel channel;
        require(channel.publish(path), "queue metadata");
        require(receiveStartupPath(channel.childFd(), 100) == path, "read complete metadata packet");
        require(!channel.publish("relative/session.json"), "reject relative path");
        require(!channel.publish(std::string_view("/tmp/a\0b", 8)), "reject embedded nul");
        require(!channel.publish(std::string(MAX_STARTUP_PATH_BYTES + 1, '/')), "reject oversized path");
        require(channel.publish(std::string(MAX_STARTUP_PATH_BYTES, '/')), "accept exact bound");
        require(receiveStartupPath(channel.childFd(), 100)->size() == MAX_STARTUP_PATH_BYTES, "read exact bound");
    }
    {
        SessionStartupChannel channel;
        const auto start = std::chrono::steady_clock::now();
        require(!receiveStartupPath(channel.childFd(), 20), "missing publisher times out");
        require(std::chrono::steady_clock::now() - start < std::chrono::seconds(1), "bounded timeout");
        channel.closeChild();
        require(!channel.publish(path), "dead helper cannot SIGPIPE the compositor");
    }
    // Peer closes, sends invalid data, or exceeds the packet bound: fail closed.
    for (int test = 0; test < 4; ++test) {
        int fds[2];
        require(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds) == 0, "raw socket pair");
        const std::vector<std::string> packets{"", "relative", std::string("/a\0b", 4), std::string(MAX_STARTUP_PATH_BYTES + 1, '/')};
        if (test > 0)
            require(send(fds[0], packets[test].data(), packets[test].size(), MSG_NOSIGNAL) > 0, "send invalid packet");
        close(fds[0]);
        require(!receiveStartupPath(fds[1], 100), "reject incomplete or invalid startup");
        close(fds[1]);
    }
    {
        SessionStartupChannel channel;
        int count = 0;
        while (count < 10000 && channel.publish(path))
            ++count;
        require(count > 0 && count < 10000, "full queue returns without blocking compositor");
    }
    std::cout << "session startup: spawn, bounds, timeout, peer failure and nonblocking delivery passed\n";
}
