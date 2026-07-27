#include "shared/config.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <utility>

namespace hyprcapture {
namespace {

std::string normalized(std::string_view value) {
    std::string out(value);
    std::ranges::transform(out, out.begin(), [](unsigned char c) {
        if (c == '_' || c == ' ')
            return '-';
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

std::filesystem::path homeConfigPath(std::string_view relativePath) {
    if (const char* configHome = std::getenv("XDG_CONFIG_HOME"); configHome && *configHome)
        return std::filesystem::path(configHome) / relativePath;
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".config" / relativePath;
    return {};
}

std::string unescapeUserDirValue(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    bool escaped = false;
    for (const char ch : value) {
        if (escaped) {
            out.push_back(ch);
            escaped = false;
        } else if (ch == '\\') {
            escaped = true;
        } else {
            out.push_back(ch);
        }
    }
    if (escaped)
        out.push_back('\\');
    return out;
}

std::string expandHomeVariable(std::string value) {
    const char* home = std::getenv("HOME");
    if (!home || !*home)
        return value;

    if (value == "$HOME")
        return home;
    if (value.starts_with("$HOME/"))
        return (std::filesystem::path(home) / value.substr(6)).string();
    if (value == "${HOME}")
        return home;
    if (value.starts_with("${HOME}/"))
        return (std::filesystem::path(home) / value.substr(8)).string();
    return value;
}

std::optional<std::filesystem::path> configuredXdgUserDir(std::string_view variable) {
    const auto path = homeConfigPath("user-dirs.dirs");
    if (path.empty())
        return std::nullopt;

    std::ifstream file(path);
    if (!file)
        return std::nullopt;

    const std::string key(variable);
    std::string       line;
    while (std::getline(file, line)) {
        if (!line.starts_with(key + "=\""))
            continue;
        const auto valueBegin = key.size() + 2;
        const auto valueEnd = line.find('"', valueBegin);
        if (valueEnd == std::string::npos)
            return std::nullopt;
        const auto value = expandHomeVariable(unescapeUserDirValue(std::string_view(line).substr(valueBegin, valueEnd - valueBegin)));
        if (!value.empty())
            return std::filesystem::path(value);
    }
    return std::nullopt;
}

std::filesystem::path fallbackXdgUserDir(std::string_view variable) {
    const char* home = std::getenv("HOME");
    if (!home || !*home)
        return {};
    if (variable == "XDG_PICTURES_DIR")
        return std::filesystem::path(home) / "Pictures";
    if (variable == "XDG_VIDEOS_DIR")
        return std::filesystem::path(home) / "Videos";
    return {};
}

std::optional<std::pair<std::string_view, std::string_view>> splitLeadingXdgVariable(std::string_view path) {
    if (!path.starts_with("$XDG_") && !path.starts_with("${XDG_"))
        return std::nullopt;

    if (path.starts_with("${")) {
        const auto end = path.find('}');
        if (end == std::string_view::npos)
            return std::nullopt;
        return std::pair{path.substr(2, end - 2), path.substr(end + 1)};
    }

    const auto slash = path.find('/');
    if (slash == std::string_view::npos)
        return std::pair{path.substr(1), std::string_view{}};
    return std::pair{path.substr(1, slash - 1), path.substr(slash)};
}

} // namespace

CaptureMode parseCaptureMode(std::string_view value, CaptureMode fallback) {
    const auto v = normalized(value);
    if (v == "full" || v == "fullscreen")
        return CaptureMode::Fullscreen;
    if (v == "region" || v == "selection")
        return CaptureMode::Region;
    if (v == "window")
        return CaptureMode::Window;
    return fallback;
}

FullscreenScope parseFullscreenScope(std::string_view value, FullscreenScope fallback) {
    const auto v = normalized(value);
    if (v == "all" || v == "all-monitors")
        return FullscreenScope::All;
    if (v == "current" || v == "current-monitor")
        return FullscreenScope::Current;
    if (v == "per-monitor" || v == "each")
        return FullscreenScope::PerMonitor;
    return fallback;
}

OverlayScope parseOverlayScope(std::string_view value, OverlayScope fallback) {
    const auto v = normalized(value);
    if (v == "fix" || v == "fixed" || v == "launch")
        return OverlayScope::Fix;
    if (v == "focus" || v == "focused" || v == "forcus")
        return OverlayScope::Focus;
    if (v == "all" || v == "all-monitors")
        return OverlayScope::All;
    return fallback;
}

WindowBackground parseWindowBackground(std::string_view value, WindowBackground fallback) {
    const auto v = normalized(value);
    if (v == "white")
        return WindowBackground::White;
    if (v == "black")
        return WindowBackground::Black;
    if (v == "follow-system" || v == "system")
        return WindowBackground::FollowSystem;
    if (v == "real" || v == "real-background")
        return WindowBackground::Real;
    if (v == "transparent" || v == "alpha")
        return WindowBackground::Transparent;
    return fallback;
}

DecorationPolicy parseDecorationPolicy(std::string_view value, DecorationPolicy fallback) {
    const auto v = normalized(value);
    if (v == "keep" || v == "preserve")
        return DecorationPolicy::Keep;
    if (v == "remove" || v == "strip")
        return DecorationPolicy::Remove;
    return fallback;
}

RecordWindowBackend parseRecordWindowBackend(std::string_view value, RecordWindowBackend fallback) {
    const auto v = normalized(value);
    if (v == "compositor" || v == "hyprcapture" || v == "exact")
        return RecordWindowBackend::Compositor;
    if (v == "gsr-visible" || v == "visible-gsr" || v == "gsr" || v == "region")
        return RecordWindowBackend::GsrVisible;
    return fallback;
}

NotificationBackend parseNotificationBackend(std::string_view value, NotificationBackend fallback) {
    const auto v = normalized(value);
    if (v == "hyprland" || v == "overlay")
        return NotificationBackend::Hyprland;
    if (v == "system" || v == "libnotify" || v == "dbus")
        return NotificationBackend::System;
    return fallback;
}

std::string normalizeRecordFormat(std::string_view value) {
    const auto v = normalized(value);
    if (v == "mkv" || v == "matroska")
        return "mkv";
    if (v == "webm")
        return "webm";
    if (v == "mov" || v == "quicktime")
        return "mov";
    if (v == "gif")
        return "gif";
    if (v == "apng")
        return "apng";
    if (v == "webp")
        return "webp";
    if (v == "mp4" || v == "mpeg-4")
        return "mp4";
    return "mp4";
}

bool recordFormatIsImageAnimation(std::string_view value) {
    const auto format = normalizeRecordFormat(value);
    return format == "gif" || format == "apng" || format == "webp";
}

WatermarkPosition parseWatermarkPosition(std::string_view value, WatermarkPosition fallback) {
    const auto v = normalized(value);
    if (v == "up-left" || v == "top-left" || v == "upper-left")
        return WatermarkPosition::UpLeft;
    if (v == "up-middle" || v == "up-center" || v == "top-middle" || v == "top-center" || v == "upper-middle" || v == "upper-center")
        return WatermarkPosition::UpMiddle;
    if (v == "up-right" || v == "top-right" || v == "upper-right")
        return WatermarkPosition::UpRight;
    if (v == "left-middle" || v == "middle-left" || v == "left-center" || v == "center-left")
        return WatermarkPosition::LeftMiddle;
    if (v == "central" || v == "center" || v == "middle" || v == "middle-middle" || v == "center-center")
        return WatermarkPosition::Central;
    if (v == "right-middle" || v == "right-meddle" || v == "middle-right" || v == "right-center" || v == "center-right")
        return WatermarkPosition::RightMiddle;
    if (v == "down-left" || v == "bottom-left" || v == "lower-left")
        return WatermarkPosition::DownLeft;
    if (v == "down-middle" || v == "down-center" || v == "bottom-middle" || v == "bottom-center" || v == "lower-middle" || v == "lower-center")
        return WatermarkPosition::DownMiddle;
    if (v == "down-right" || v == "bottom-right" || v == "lower-right")
        return WatermarkPosition::DownRight;
    return fallback;
}

std::string toString(CaptureMode value) {
    switch (value) {
        case CaptureMode::Fullscreen: return "fullscreen";
        case CaptureMode::Region: return "region";
        case CaptureMode::Window: return "window";
    }
    return "region";
}

std::string toString(FullscreenScope value) {
    switch (value) {
        case FullscreenScope::All: return "all";
        case FullscreenScope::Current: return "current";
        case FullscreenScope::PerMonitor: return "per-monitor";
    }
    return "all";
}

std::string toString(OverlayScope value) {
    switch (value) {
        case OverlayScope::Fix: return "fix";
        case OverlayScope::Focus: return "focus";
        case OverlayScope::All: return "all";
    }
    return "fix";
}

std::string toString(WindowBackground value) {
    switch (value) {
        case WindowBackground::White: return "white";
        case WindowBackground::Black: return "black";
        case WindowBackground::FollowSystem: return "follow-system";
        case WindowBackground::Real: return "real";
        case WindowBackground::Transparent: return "transparent";
    }
    return "follow-system";
}

std::string toString(DecorationPolicy value) {
    switch (value) {
        case DecorationPolicy::Keep: return "keep";
        case DecorationPolicy::Remove: return "remove";
    }
    return "keep";
}

std::string toString(RecordWindowBackend value) {
    switch (value) {
        case RecordWindowBackend::Compositor: return "compositor";
        case RecordWindowBackend::GsrVisible: return "gsr-visible";
    }
    return "compositor";
}

std::string toString(NotificationBackend value) {
    switch (value) {
        case NotificationBackend::Hyprland: return "hyprland";
        case NotificationBackend::System: return "system";
    }
    return "hyprland";
}

std::string toString(WatermarkPosition value) {
    switch (value) {
        case WatermarkPosition::UpLeft: return "up-left";
        case WatermarkPosition::UpMiddle: return "up-middle";
        case WatermarkPosition::UpRight: return "up-right";
        case WatermarkPosition::LeftMiddle: return "left-middle";
        case WatermarkPosition::Central: return "central";
        case WatermarkPosition::RightMiddle: return "right-middle";
        case WatermarkPosition::DownLeft: return "down-left";
        case WatermarkPosition::DownMiddle: return "down-middle";
        case WatermarkPosition::DownRight: return "down-right";
    }
    return "central";
}

std::filesystem::path expandUserPath(std::string_view path) {
    if (path.empty())
        return {};

    std::string p(path);
    if (p == "~" || p.starts_with("~/")) {
        const char* home = std::getenv("HOME");
        if (home && *home)
            return std::filesystem::path(home) / p.substr(p == "~" ? 1 : 2);
    }
    if (const auto xdg = splitLeadingXdgVariable(p)) {
        auto dir = configuredXdgUserDir(xdg->first).value_or(fallbackXdgUserDir(xdg->first));
        if (!dir.empty()) {
            std::string suffix(xdg->second);
            while (suffix.starts_with('/'))
                suffix.erase(suffix.begin());
            return suffix.empty() ? dir : dir / suffix;
        }
    }
    return std::filesystem::path(p);
}

std::string sanitizeFilenameVariable(std::string_view value) {
    std::string out;
    out.reserve(std::min<std::size_t>(value.size(), 128));
    bool pendingSeparator = false;
    for (std::size_t i = 0; i < value.size();) {
        const auto ch = static_cast<unsigned char>(value[i]);
        const bool safeAscii = std::isalnum(ch) || ch == '.' || ch == '_' || ch == '-';
        std::size_t utf8Length = 0;
        if (ch >= 0xC2 && ch <= 0xDF)
            utf8Length = 2;
        else if (ch >= 0xE0 && ch <= 0xEF)
            utf8Length = 3;
        else if (ch >= 0xF0 && ch <= 0xF4)
            utf8Length = 4;
        if (utf8Length > 0 && (i + utf8Length > value.size() || !std::ranges::all_of(value.substr(i + 1, utf8Length - 1), [](unsigned char continuation) {
                return continuation >= 0x80 && continuation <= 0xBF;
            })))
            utf8Length = 0;

        if (safeAscii || utf8Length > 0) {
            if (pendingSeparator && !out.empty() && out.back() != '-')
                out.push_back('-');
            pendingSeparator = false;
            if (out.size() + std::max<std::size_t>(utf8Length, 1) > 128)
                break;
            out.append(value.substr(i, std::max<std::size_t>(utf8Length, 1)));
        } else {
            pendingSeparator = true;
        }
        i += std::max<std::size_t>(utf8Length, 1);
    }

    while (!out.empty() && (out.front() == '.' || out.front() == '-'))
        out.erase(out.begin());
    while (!out.empty() && (out.back() == '.' || out.back() == '-'))
        out.pop_back();
    return out.empty() ? "unknown" : out;
}

FilenameMetadata resolveFilenameMetadata(bool dynamicWindowMetadata,
                                         CaptureMode mode,
                                         const FilenameMetadata& selectedWindow,
                                         const FilenameMetadata& legacyWindow,
                                         std::size_t fullscreenWindowCount,
                                         const FilenameMetadata& singleFullscreenWindow) {
    if (!dynamicWindowMetadata)
        return legacyWindow;

    switch (mode) {
        case CaptureMode::Region:
            return {.windowClass = "region", .windowTitle = "region"};
        case CaptureMode::Window:
            return selectedWindow;
        case CaptureMode::Fullscreen:
            if (fullscreenWindowCount == 1)
                return singleFullscreenWindow;
            return {.windowClass = "fullscreen", .windowTitle = "fullscreen"};
    }
    return {};
}

std::string makeTimestampedFilename(std::string_view filenameTemplate, std::string_view windowClass, std::string_view windowTitle) {
    const std::string sanitizedClass = sanitizeFilenameVariable(windowClass);
    const std::string sanitizedTitle = sanitizeFilenameVariable(windowTitle);

    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm {};
    localtime_r(&t, &tm);

    std::ostringstream out;
    out << std::put_time(&tm, std::string(filenameTemplate).c_str());
    auto filename = out.str();
    const auto replaceVariable = [&filename](std::string_view variable, const std::string& value) {
        std::size_t position = 0;
        while ((position = filename.find(variable, position)) != std::string::npos) {
            filename.replace(position, variable.size(), value);
            position += value.size();
        }
    };
    replaceVariable("{window_class}", sanitizedClass);
    replaceVariable("{window_title}", sanitizedTitle);
    filename = std::filesystem::path(filename).filename().string();
    if (filename.empty() || filename == "." || filename == "..")
        return "Screenshot.png";
    return filename;
}

} // namespace hyprcapture
