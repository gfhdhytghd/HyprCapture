#include "shared/shot_channel.hpp"
#include <cstdlib>
#include <iostream>
#include <fcntl.h>

void require(bool value, const char* reason) {
    if (!value) { std::cerr << reason << '\n'; std::exit(1); }
}
int main() {
    using namespace hyprcapture::shot;
    const auto root = runtimeRoot();
    require(!root.empty(), "private runtime root");
    auto directory = root + "/shot-path-test-XXXXXX";
    require(mkdtemp(directory.data()), "invocation directory");
    const auto path = directory + "/result.sock";
    const int server = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    require(server >= 0, "socket");
    sockaddr_un address{}; address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    require(bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && listen(server, 1) == 0, "listen");
    chmod(path.c_str(), 0600);
    require(privateSocket(path), "accept private owner socket");
    const int client = connectResult(path);
    require(client >= 0, "connect validated owner socket"); close(client);
    require(!privateSocket("relative") && !privateSocket(directory + "/../" + std::filesystem::path(directory).filename().string() + "/result.sock"), "reject relative and traversal");
    chmod(path.c_str(), 0620); require(!privateSocket(path), "reject writable socket"); chmod(path.c_str(), 0600);
    chmod(directory.c_str(), 0720); require(!privateSocket(path), "reject writable parent"); chmod(directory.c_str(), 0700);
    const auto alias = directory + "/alias";
    require(symlink(path.c_str(), alias.c_str()) == 0 && !privateSocket(alias), "reject symlink socket"); unlink(alias.c_str());
    close(server); unlink(path.c_str());
    int fd = open(path.c_str(), O_CREAT | O_WRONLY, 0600); require(fd >= 0, "regular file fixture"); close(fd);
    require(!privateSocket(path), "reject non-socket"); unlink(path.c_str()); rmdir(directory.c_str());
    const auto encoded = header(0, 0x123456789abcdefULL);
    require(encoded.size() == 9 && payloadSize(encoded) == 0x123456789abcdefULL, "bounded endian framing");
}
