#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace hyprcapture {

// The wire mode is deliberately part of the request, rather than an
// implementation preference.  A requester asking for GPU frames must never
// receive the CPU HCSF stream as an implicit fallback.
enum class WindowStreamTransport {
    Cpu,
    Gpu,
};

struct WindowStreamStartControl {
    std::string id;
    std::string windowAddress;
    std::string socketPath;
    int         fps = 60;
    WindowStreamTransport transport = WindowStreamTransport::Cpu;
};

// Pure protocol decoding: no filesystem, socket, Hyprland, or GL access.
// Runtime ownership/path checks deliberately remain at the plugin boundary.
std::optional<WindowStreamStartControl> decodeWindowStreamStartControl(std::string_view json);
std::optional<std::string> decodeWindowStreamStopControl(std::string_view json);

} // namespace hyprcapture
