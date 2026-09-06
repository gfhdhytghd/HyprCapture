#include "plugin/recording_codec.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using hyprcapture::selectRecordingCodec;

int main(int argc, char** argv) {
    // Also expose the production candidate order for manual hardware probes.
    if (argc == 3) {
        for (const auto& codec : hyprcapture::recordingCodecCandidates("auto", std::stoi(argv[1]), std::stoi(argv[2])))
            std::cout << codec << '\n';
        return 0;
    }

    std::vector<std::string> attempted;
    auto choose = [&](const std::string& family, int width, int height, const std::vector<std::string>& available) {
        attempted.clear();
        return selectRecordingCodec(family, width, height, [&](const std::string& codec, int w, int h) {
            assert(w == width && h == height); // Probe physical canvas dimensions unchanged.
            attempted.push_back(codec);
            return std::ranges::find(available, codec) != available.end();
        });
    };

    assert(choose("auto", 5204, 3356, {"hevc_nvenc", "libx264"}) == "hevc_nvenc");
    assert(attempted == std::vector<std::string>{"hevc_nvenc"});
    assert(choose("auto", 3356, 5204, {"hevc_vaapi", "libx264"}) == "hevc_vaapi");
    assert((attempted == std::vector<std::string>{"hevc_nvenc", "hevc_vaapi"}));
    assert(choose("AUTO", 4096, 4096, {"h264_nvenc", "hevc_nvenc"}) == "h264_nvenc");
    assert(choose("auto", 4098, 2160, {"h264_nvenc", "hevc_nvenc"}) == "hevc_nvenc");
    assert(choose("auto", 1920, 1080, {"hevc_nvenc", "libx264"}) == "hevc_nvenc");
    assert((attempted == std::vector<std::string>{"h264_nvenc", "h264_vaapi", "hevc_nvenc"}));

    // Software fallback is permitted, including canvases above both HW limits.
    assert(choose("auto", 8208, 5352, {"libx264"}) == "libx264");
    assert(attempted.size() == 5 && attempted.back() == "libx264");
    assert(choose("", 5204, 3356, {"h264_vaapi", "libx264"}) == "h264_vaapi");
    // User-selected families and implementation names retain their semantics.
    assert(choose("h264", 5204, 3356, {"hevc_nvenc", "libx264"}) == "libx264");
    assert((attempted == std::vector<std::string>{"h264_nvenc", "h264_vaapi", "libx264"}));
    assert(choose("h265", 5204, 3356, {"libx265"}) == "libx265");
    assert(choose("hevc", 1920, 1080, {"hevc_nvenc"}) == "hevc_nvenc");
    assert(choose("av1", 5204, 3356, {"libsvtav1"}) == "libsvtav1");
    assert(choose("vp9", 5204, 3356, {"libvpx-vp9"}) == "libvpx-vp9");
    assert(choose("ffv1", 5204, 3356, {"ffv1"}) == "ffv1");
    assert(choose("hevc_nvenc", 5204, 3356, {"hevc_nvenc"}) == "hevc_nvenc");
    assert(choose("auto", 5204, 3356, {}) == "libx264");
}
