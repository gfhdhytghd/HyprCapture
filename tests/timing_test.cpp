#include "plugin/timing.hpp"

#include <atomic>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

using namespace hyprcapture;

namespace {
std::string contents(const std::filesystem::path& path) {
    std::ifstream file(path);
    return {std::istreambuf_iterator<char>(file), {}};
}
} // namespace

int main() {
    unsetenv("HYPRCAPTURE_TIMING");
    unsetenv("HYPRCAPTURE_TIMING_FILE");
    assert(!timingEnabled());
    char temporary[] = "/tmp/hyprcapture-timing-test.XXXXXX";
    assert(mkdtemp(temporary));
    const auto root = std::filesystem::path(temporary);
    assert(setenv("TMPDIR", temporary, 1) == 0);
    const auto privateRoot = root / ("hyprcapture-" + std::to_string(geteuid()));
    assert(std::filesystem::create_directory(privateRoot));
    assert(chmod(privateRoot.c_str(), 0700) == 0);
    const auto first = privateRoot / "first.log";
    const auto second = privateRoot / "second.log";

    refreshTimingConfig(false, first.string());
    traceTiming("disabled");
    assert(!std::filesystem::exists(first));
    refreshTimingConfig(true, first.string());
    std::thread encoder([] {
        assert(timingEnabled());
        ScopedTiming timing("record.encoder_write");
    });
    encoder.join();
    assert(contents(first).find("event=record.encoder_write") != std::string::npos);

    // All worker operations link without any Hyprland configuration symbols.
    // Exercise concurrent main-thread publications and encoder readers too.
    std::atomic<bool> ready = false;
    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) {
        workers.emplace_back([&] {
            while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
            for (int j = 0; j < 250; ++j) {
                ScopedTiming timing("concurrent.encoder_write");
                traceTiming("concurrent.frame");
            }
        });
    }
    ready.store(true, std::memory_order_release);
    for (int i = 0; i < 500; ++i)
        refreshTimingConfig(i % 3 != 0, (i % 2 ? first : second).string());
    for (auto& worker : workers) worker.join();

    refreshTimingConfig(true, second.string());
    traceTiming("new.path");
    assert(contents(second).find("event=new.path") != std::string::npos);
    assert(contents(first).find("event=new.path") == std::string::npos);
    refreshTimingConfig(false, second.string());
    const auto before = contents(second);
    std::thread disabled([] { assert(!timingEnabled()); traceTiming("disabled.worker"); });
    disabled.join();
    assert(contents(second) == before);

    assert(setenv("HYPRCAPTURE_TIMING_FILE", second.c_str(), 1) == 0);
    refreshTimingConfig(false, "");
    assert(timingEnabled());
    traceTiming("environment.path");
    assert(contents(second).find("event=environment.path") != std::string::npos);
    // An explicit configuration path takes precedence over the environment.
    refreshTimingConfig(false, first.string());
    traceTiming("configured.path");
    assert(contents(first).find("event=configured.path") != std::string::npos);
    unsetenv("HYPRCAPTURE_TIMING_FILE");
    assert(setenv("HYPRCAPTURE_TIMING", "1", 1) == 0);
    refreshTimingConfig(false, first.string());
    assert(timingEnabled());
    unsetenv("HYPRCAPTURE_TIMING");
    refreshTimingConfig(false, "");
    assert(!timingEnabled());
    std::filesystem::remove_all(root);
}
