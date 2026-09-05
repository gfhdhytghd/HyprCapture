#pragma once
#include "window_gpu_sender.hpp"
#include "window_gpu_wire.hpp"
#include <optional>

namespace hyprcapture {
// Render-thread only, with the compositor's EGL context current. Does not
// read pixels or wait for the GPU. The caller owns and must keep the source
// framebuffer immutable until sender Ready, or retire it on sender failure.
// metadata crop/style/time must describe the completed render, not a later
// window state. Image layout fields are filled from the actual export.
std::optional<WindowGpuPacket> exportWindowGpuFrame(unsigned int framebuffer,
                                                   gpuwire::Frame metadata);
}
