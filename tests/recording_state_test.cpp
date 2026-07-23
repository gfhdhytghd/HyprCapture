#include "plugin/recording_state.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (condition)
        return;
    std::cerr << "recording state test failed: " << message << '\n';
    std::exit(1);
}

std::string querySocket(const std::filesystem::path& path) {
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    require(fd >= 0, "client socket created");

    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    const auto native = path.string();
    require(native.size() < sizeof(address.sun_path), "test socket path fits");
    std::memcpy(address.sun_path, native.c_str(), native.size() + 1);
    require(connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0, "client connected");

    std::string              response;
    std::array<char, 1024>   buffer {};
    while (true) {
        const ssize_t bytes = read(fd, buffer.data(), buffer.size());
        if (bytes < 0 && errno == EINTR)
            continue;
        require(bytes >= 0, "socket response read");
        if (bytes == 0)
            break;
        response.append(buffer.data(), static_cast<std::size_t>(bytes));
    }
    close(fd);
    return response;
}

nlohmann::json queryJson(const std::filesystem::path& path) {
    const auto response = querySocket(path);
    require(response.ends_with('\n'), "response is newline terminated");
    return nlohmann::json::parse(response);
}

} // namespace

int main() {
    using namespace hyprcapture;

    char directoryTemplate[] = "/tmp/hyprcapture-state-test-XXXXXX";
    const auto* directory = mkdtemp(directoryTemplate);
    require(directory != nullptr, "private test directory created");
    require(chmod(directory, 0700) == 0, "private test directory permissions set");

    const auto socketPath = std::filesystem::path(directory) / "recording.sock";
    RecordingStateServer server;
    std::string          error;
    require(server.start(socketPath, &error), "server started");
    require(server.running(), "server reports running");
    require(server.socketPath() == socketPath, "server exposes socket path");

    RecordingStateServer competingServer;
    require(!competingServer.start(socketPath, &error), "active socket is not replaced");

    struct stat socketStat {};
    require(lstat(socketPath.c_str(), &socketStat) == 0 && S_ISSOCK(socketStat.st_mode), "socket exists");
    require((socketStat.st_mode & 0777) == 0600, "socket is owner-only");

    auto state = queryJson(socketPath);
    require(state["version"] == 1, "protocol version");
    require(state["active"] == false && state["phase"] == "inactive", "initial inactive state");
    require(state["backend"] == "" && state["output"] == "" && state["elapsed"] == 0 && state["elapsedMs"] == 0, "inactive fields");

    server.begin("gpu-screen-recorder", "/tmp/video.mp4", "fullscreen", "mp4");
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    state = queryJson(socketPath);
    require(state["active"] == true && state["phase"] == "recording", "active recording state");
    require(state["backend"] == "gpu-screen-recorder", "backend exposed");
    require(state["output"] == "/tmp/video.mp4", "output exposed");
    require(state["mode"] == "fullscreen" && state["format"] == "mp4", "recording metadata exposed");
    require(state["elapsed"].get<double>() > 0.0, "elapsed seconds exposed");
    require(state["elapsedMs"].get<long long>() >= 10, "elapsed time advances");
    require(!state["startedAt"].get<std::string>().empty(), "wall clock start exposed");

    std::vector<std::thread> clients;
    std::array<bool, 8>      clientOk {};
    for (std::size_t i = 0; i < clientOk.size(); ++i) {
        clients.emplace_back([&, i] {
            const auto concurrentState = queryJson(socketPath);
            clientOk[i] = concurrentState["active"] == true && concurrentState["backend"] == "gpu-screen-recorder";
        });
    }
    for (auto& client : clients)
        client.join();
    for (const bool ok : clientOk)
        require(ok, "concurrent client received state");

    server.beginFinalizing();
    const auto finalizing = queryJson(socketPath);
    require(finalizing["active"] == false && finalizing["phase"] == "finalizing", "finalizing state");
    const auto frozenElapsed = finalizing["elapsedMs"].get<long long>();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    require(queryJson(socketPath)["elapsedMs"].get<long long>() == frozenElapsed, "finalizing elapsed time is frozen");

    server.clear();
    state = queryJson(socketPath);
    require(state["phase"] == "inactive" && state["output"] == "" && state["elapsedMs"] == 0, "state clears");

    server.stop();
    require(!server.running(), "server reports stopped");
    require(!std::filesystem::exists(socketPath), "socket removed on stop");

    const int staleFd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    require(staleFd >= 0, "stale socket created");
    sockaddr_un staleAddress {};
    staleAddress.sun_family = AF_UNIX;
    const auto staleNative = socketPath.string();
    std::memcpy(staleAddress.sun_path, staleNative.c_str(), staleNative.size() + 1);
    require(bind(staleFd, reinterpret_cast<const sockaddr*>(&staleAddress), sizeof(staleAddress)) == 0, "stale socket bound");
    close(staleFd);
    require(server.start(socketPath, &error), "user-owned stale socket replaced");
    require(queryJson(socketPath)["phase"] == "inactive", "replacement server responds");
    server.stop();

    std::filesystem::remove(directory);

    std::cout << "hyprcapture recording state tests passed\n";
    return 0;
}
