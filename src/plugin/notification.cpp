#include "plugin/notification.hpp"

#include "plugin/session_launcher.hpp"
#include "shared/config.hpp"

#include <algorithm>
#include <typeindex>

#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/shared/Types.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>

extern HANDLE g_pluginHandle;

namespace hyprcapture {
namespace {

NotificationBackend configuredBackend() {
    if (!g_pluginHandle || !Config::mgr())
        return NotificationBackend::Hyprland;

    const auto value = Config::mgr()->getConfigValue("plugin:hyprcapture:notification_backend");
    if (!value.dataptr || !value.type)
        return NotificationBackend::Hyprland;

    const auto type = std::type_index(*value.type);
    if (type == typeid(Config::STRING))
        return parseNotificationBackend(**reinterpret_cast<Config::STRING* const*>(value.dataptr));
    if (type == typeid(Hyprlang::STRING)) {
        const auto raw = *reinterpret_cast<Hyprlang::STRING const*>(value.dataptr);
        return parseNotificationBackend(raw ? raw : "");
    }
    return NotificationBackend::Hyprland;
}

CHyprColor notificationColor(NotificationLevel level) {
    switch (level) {
        case NotificationLevel::Info: return CHyprColor(0.2, 0.8, 0.3, 1.0);
        case NotificationLevel::Warning: return CHyprColor(1.0, 0.72, 0.2, 1.0);
        case NotificationLevel::Error: return CHyprColor(1.0, 0.25, 0.2, 1.0);
    }
    return CHyprColor(1.0, 1.0, 1.0, 1.0);
}

void notifyHyprland(const std::string& message, NotificationLevel level, int timeoutMs) {
    if (g_pluginHandle)
        HyprlandAPI::addNotification(g_pluginHandle, "[hyprcapture] " + message, notificationColor(level), std::max(0, timeoutMs));
}

} // namespace

void notifyUser(const std::string& message, NotificationLevel level, int timeoutMs) {
    // Error feedback must not depend on an external notification daemon or helper.
    if (level == NotificationLevel::Error || configuredBackend() == NotificationBackend::Hyprland) {
        notifyHyprland(message, level, timeoutMs);
        return;
    }

    if (!launchSystemNotification(message, timeoutMs, level == NotificationLevel::Warning).success)
        notifyHyprland(message, level, timeoutMs);
}

} // namespace hyprcapture
