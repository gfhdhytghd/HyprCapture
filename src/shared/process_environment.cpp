#include "shared/process_environment.hpp"

#include <algorithm>

namespace hyprcapture {

bool desktopEnvironmentNameAllowed(std::string_view name) {
    static constexpr std::string_view allowed[] = {
        "HOME", "USER", "LOGNAME", "LANG",
        "XDG_RUNTIME_DIR", "XDG_CURRENT_DESKTOP", "XDG_SESSION_TYPE",
        "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_DATA_DIRS", "DESKTOP_SESSION",
        "WAYLAND_DISPLAY", "DISPLAY", "DBUS_SESSION_BUS_ADDRESS", "PULSE_SERVER", "PULSE_COOKIE",
        "QT_QPA_PLATFORM", "QT_QPA_PLATFORMTHEME", "QT_SCALE_FACTOR",
        "QT_AUTO_SCREEN_SCALE_FACTOR", "QT_ENABLE_HIGHDPI_SCALING",
        "HYPRCAPTURE_TIMING", "HYPRLAND_INSTANCE_SIGNATURE",
    };
    return std::find(std::begin(allowed), std::end(allowed), name) != std::end(allowed) || name.starts_with("LC_");
}

} // namespace hyprcapture
