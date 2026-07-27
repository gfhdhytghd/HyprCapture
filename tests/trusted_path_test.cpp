#include "shared/trusted_path.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>

namespace {

void require(bool condition, const char* message) {
    if (condition)
        return;

    std::cerr << "trusted path test failed: " << message << '\n';
    std::exit(1);
}

class TempTree {
  public:
    TempTree() {
        std::string pattern = "/tmp/hyprcapture-trusted-path-XXXXXX";
        if (char* result = mkdtemp(pattern.data()))
            root = result;
    }

    ~TempTree() {
        std::error_code ec;
        if (!root.empty())
            std::filesystem::remove_all(root, ec);
    }

    std::filesystem::path root;
};

void setMode(const std::filesystem::path& path, mode_t mode) {
    require(chmod(path.c_str(), mode) == 0, "chmod succeeds");
}

} // namespace

int main() {
    TempTree tree;
    require(!tree.root.empty(), "temporary tree is created");

    const auto boundary = tree.root / "store";
    const auto package = boundary / "hash-package";
    const auto bin = package / "bin";
    const auto executable = bin / "tool";
    std::filesystem::create_directories(bin);
    std::ofstream(executable) << "#!/bin/sh\n";

    setMode(boundary, 0775);
    setMode(package, 0755);
    setMode(bin, 0755);
    setMode(executable, 0755);

    require(hyprcapture::security::parentChainTrusted(executable, boundary), "immutable boundary stops parent permission checks");
    require(hyprcapture::security::trustedExecutablePath(executable.string(), boundary) == executable.string(),
            "trusted executable under boundary is accepted");
    require(!hyprcapture::security::parentChainTrusted(executable, {}), "writable boundary is rejected without an immutable boundary");

    setMode(bin, 0775);
    require(!hyprcapture::security::trustedExecutablePath(executable.string(), boundary), "writable directory below boundary is rejected");
    setMode(bin, 0755);

    setMode(executable, 0775);
    require(!hyprcapture::security::trustedExecutablePath(executable.string(), boundary), "writable executable is rejected");

    std::cout << "hyprcapture trusted path tests passed\n";
    return 0;
}
