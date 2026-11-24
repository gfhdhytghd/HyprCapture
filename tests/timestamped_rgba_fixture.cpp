#include "plugin/timestamped_rgba.hpp"
#include "shared/sdr_video.hpp"
#include <algorithm>
#include <csignal>
#include <cerrno>
#include <chrono>
#include <thread>
#include <fcntl.h>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <unistd.h>
#include <vector>

int main(int argc, char** argv) {
    using namespace hyprcapture;
    signal(SIGPIPE, SIG_IGN);
    if (argc > 1 && std::string(argv[1]) == "--blocked-pipe") {
        int fds[2];
        if (pipe2(fds, O_NONBLOCK | O_CLOEXEC) != 0) return 1;
        const unsigned char frame = 1;
        while (::write(fds[1], &frame, 1) == 1) {}
        if (errno != EAGAIN) return 2;
        std::jthread reader([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            unsigned char bytes[4096];
            read(fds[0], bytes, sizeof(bytes));
        });
        const int resumed = writeRecordingBytes(fds[1], {&frame, 1}, {});
        reader.join();
        while (::write(fds[1], &frame, 1) == 1) {}
        std::stop_source stop;
        std::jthread cancel([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            stop.request_stop();
        });
        const int cancelled = writeRecordingBytes(fds[1], {&frame, 1}, stop.get_token());
        close(fds[0]);
        const int disconnected = writeRecordingBytes(fds[1], {&frame, 1}, {});
        close(fds[1]);
        return resumed == 0 && cancelled == -ECANCELED && disconnected == -EPIPE ? 0 : 3;
    }
    if (argc > 1 && std::string(argv[1]) == "--sdr-filter") {
        std::cout << sdrVideoFilter; return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--input-args") {
        for (const auto& s : timestampedRgbaInputArgs()) std::cout << s << '\n';
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--output-args") {
        for (const auto& s : timestampedRgbaOutputArgs()) std::cout << s << '\n';
        return 0;
    }
    const int width = argc > 2 ? std::atoi(argv[1]) : 64;
    const int height = argc > 2 ? std::atoi(argv[2]) : 64;
    TimestampedRgbaWriter writer;
    if (!writer.open(STDOUT_FILENO, width, height, 60)) { std::cerr << writer.error(); return 1; }
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    const std::vector<std::int64_t> timestamps{0, 33333, 200000, 750000, 1500000, recordingTailPts(1500000, 2000000, 60)};
    int index = 0;
    for (auto pts : timestamps) {
        if (index < 5) {
            for (size_t p = 0; p < pixels.size(); p += 4) {
                pixels[p] = 20 + index * 40; pixels[p + 1] = 80; pixels[p + 2] = 150; pixels[p + 3] = 255;
            }
        }
        if (!writer.write(pixels, pts)) { std::cerr << writer.error(); return 2; }
        ++index;
    }
    if (!writer.finish()) { std::cerr << writer.error(); return 3; }
    return 0;
}
