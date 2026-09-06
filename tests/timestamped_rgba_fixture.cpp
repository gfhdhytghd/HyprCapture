#include "plugin/timestamped_rgba.hpp"
#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <unistd.h>
#include <vector>

int main(int argc, char** argv) {
    using namespace hyprcapture;
    signal(SIGPIPE, SIG_IGN);
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
