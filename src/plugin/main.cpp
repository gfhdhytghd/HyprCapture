#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeindex>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/shared/Types.hpp>
#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/desktop/rule/Engine.hpp>
#include <hyprland/src/desktop/rule/layerRule/LayerRule.hpp>
#include <hyprland/src/desktop/rule/layerRule/LayerRuleEffectContainer.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/event/EventBus.hpp>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include "plugin/artifact_capture.hpp"
#include "plugin/capture_options.hpp"
#include "plugin/config_reload.hpp"
#include "plugin/notification.hpp"
#include "plugin/recording.hpp"
#include "plugin/session_launcher.hpp"
#include "plugin/session_lock.hpp"
#include "plugin/timing.hpp"

inline HANDLE g_pluginHandle = nullptr;

namespace {

constexpr const char* kOverlayLayerRuleName = "hyprcapture-ui-no-compositor-anim";
constexpr auto        kMinDispatchInterval = std::chrono::milliseconds(750);
constexpr std::array  kLuaFunctionNames = {
    "open",
    "open_editor",
    "quick",
    "record",
    "record_toggle",
    "record_stop",
    "record_start",
    "window_capture",
    "region_capture",
    "scroll_session_start",
    "scroll_session_stop",
    "window_stream_start",
    "window_stream_stop",
    "export_pipe",
    "cancel",
    "dispatch",
};

std::chrono::steady_clock::time_point g_lastCaptureDispatch {};
std::chrono::steady_clock::time_point g_lastQuickRejectNotification {};
std::deque<std::string>               g_configValueNames;
WP<Config::IConfigManager>            g_registrationManager;

std::string configName(const std::string& suffix) {
    return "plugin:hyprcapture:" + suffix;
}

const Config::SConfigOptionReply configOption(const std::string& name) {
    if (!g_pluginHandle)
        return {};
    return Config::mgr()->getConfigValue(name);
}

std::string configString(const std::string& suffix, const std::string& fallback) {
    const auto value = configOption(configName(suffix));
    if (!value.dataptr || !value.type)
        return fallback;

    const auto type = std::type_index(*value.type);
    if (type == typeid(Config::STRING))
        return **reinterpret_cast<Config::STRING* const*>(value.dataptr);

    if (type == typeid(Hyprlang::STRING)) {
        const auto raw = *reinterpret_cast<Hyprlang::STRING const*>(value.dataptr);
        return raw ? std::string(raw) : fallback;
    }

    return fallback;
}

std::int64_t configInt(const std::string& suffix, std::int64_t fallback) {
    const auto value = configOption(configName(suffix));
    if (!value.dataptr || !value.type)
        return fallback;

    const auto type = std::type_index(*value.type);
    if (type == typeid(Config::INTEGER))
        return **reinterpret_cast<Config::INTEGER* const*>(value.dataptr);

    if (type == typeid(Config::BOOL))
        return **reinterpret_cast<Config::BOOL* const*>(value.dataptr) ? 1 : 0;

    return fallback;
}

bool configBool(const std::string& suffix, bool fallback) {
    const auto value = configOption(configName(suffix));
    if (!value.dataptr || !value.type)
        return fallback;

    const auto type = std::type_index(*value.type);
    if (type == typeid(Config::BOOL))
        return **reinterpret_cast<Config::BOOL* const*>(value.dataptr);

    if (type == typeid(Config::INTEGER))
        return **reinterpret_cast<Config::INTEGER* const*>(value.dataptr) != 0;

    return fallback;
}

CHyprSignalListener g_timingTickListener;
hyprcapture::ConfigReloadListeners<CHyprSignalListener> g_configReloadListeners;
std::chrono::steady_clock::time_point g_nextTimingRefresh;

void refreshTimingConfig() {
    hyprcapture::refreshTimingConfig(configBool("timing", false), configString("timing_file", ""));
    g_nextTimingRefresh = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
}

void addStringConfig(const char* suffix, const char* description, const char* fallback) {
    const auto& name = g_configValueNames.emplace_back(configName(suffix));
    HyprlandAPI::addConfigValueV2(g_pluginHandle, makeShared<Config::Values::CStringValue>(name.c_str(), description, fallback));
}

void addIntConfig(const char* suffix, const char* description, std::int64_t fallback) {
    const auto& name = g_configValueNames.emplace_back(configName(suffix));
    HyprlandAPI::addConfigValueV2(g_pluginHandle, makeShared<Config::Values::CIntValue>(name.c_str(), description, fallback));
}

void addBoolConfig(const char* suffix, const char* description, bool fallback) {
    const auto& name = g_configValueNames.emplace_back(configName(suffix));
    HyprlandAPI::addConfigValueV2(g_pluginHandle, makeShared<Config::Values::CBoolValue>(name.c_str(), description, fallback));
}

void registerConfigValues() {
    g_configValueNames.clear();

    addStringConfig("default_mode", "Default HyprCapture mode", "region");
    addStringConfig("fullscreen_scope", "Fullscreen capture scope", "all");
    addStringConfig("overlay_scope", "Overlay monitor scope (fix, focus, or all)", "fix");
    addStringConfig("window_background", "Window capture background mode", "follow-system");
    addStringConfig("window_border", "Window capture border policy", "keep");
    addStringConfig("window_shadow", "Window capture shadow policy", "keep");
    addStringConfig("notification_backend", "Backend for non-error notifications (hyprland or system)", "system");
    addBoolConfig("screenshot_notification", "Show a notification after a successful screenshot", true);
    addStringConfig("notification_title_template", "Screenshot notification title template", "Screenshot captured");
    addStringConfig("notification_body_template", "Screenshot notification body template", "Saved {filename} ({window_title})");
    addBoolConfig("save", "Save captures to disk", true);
    addBoolConfig("clipboard", "Copy captures to the clipboard", true);
    addBoolConfig("show_thumbnail", "Show a result thumbnail after capture", true);
    addStringConfig("language", "UI language (auto or a supported locale code)", "auto");
    addBoolConfig("include_cursor", "Include the cursor in captures", false);
    addBoolConfig("remember_settings", "Restore the previous interactive capture settings", false);
    addBoolConfig("allow_quick", "Enable no-confirmation quick capture calls", false);
    addBoolConfig("confirm_before_capture", "Require explicit confirmation after target selection for normal open captures", false);
    addBoolConfig("fusion_mode", "Fuse region and window interactions in one overlay", true);
    addBoolConfig("fushion_mode", "Legacy alias for fusion_mode", false);
    addBoolConfig("capture_fullscreen_clients_as_monitor", "Capture a fullscreen client as its whole monitor in window and fusion modes", false);
    addBoolConfig("dynamic_window_metadata", "Use capture-aware window metadata in screenshot filename templates", true);
    addBoolConfig("window_wheel_scroll", "Enable wheel-based window selection", true);
    addStringConfig("window_wheel_scope", "Window wheel selection scope (workspace or under-cursor)", "workspace");
    addStringConfig("fullscreen_preview_rounding", "Fullscreen preview corner rounding: auto, 0, or logical pixels", "auto");
    addStringConfig("save_dir", "Capture output directory", "$XDG_PICTURES_DIR/Screenshots");
    addStringConfig("screenshot_mode", "Default screenshot dynamic range: sdr or hdr (empty uses legacy screenshot_format)", "");
    addBoolConfig("show_hdr_control", "Show the SDR/HDR segmented control", true);
    addBoolConfig("hdr_save_sdr", "Also save an SDR PNG alongside HDR HEIC", false);
    addStringConfig("hdr_drag_format", "HDR screenshot drag format: heic or sdr", "heic");
    addStringConfig("hdr_clipboard_format", "HDR screenshot clipboard format: heic or sdr", "heic");
    addStringConfig("screenshot_format", "Screenshot format: png (SDR) or heic (HDR, requires libheif/HEVC encoder)", "png");
    addStringConfig("filename_template", "Screenshot filename strftime template with window metadata variables", "Screenshot-%Y-%m-%d-%H%M%S.png");
    addStringConfig("record_save_dir", "Recording output directory", "$XDG_VIDEOS_DIR/Screenrecords");
    addStringConfig("record_filename_template", "Recording filename strftime template", "Recording-%Y-%m-%d-%H%M%S.mp4");
    addStringConfig("record_format", "Default recording format", "mp4");
    addStringConfig("record_transparent_format", "Default transparent recording container", "webm");
    addIntConfig("record_fps", "Recording frame rate", 30);
    addStringConfig("record_fps_options", "Recording frame rate choices", "15 24 30 60");
    addIntConfig("record_window_fps_limit", "Compositor window recording FPS cap", 12);
    addIntConfig("record_window_real_bg_fps_limit", "Real-background window recording FPS cap", 8);
    addIntConfig("record_audio_echo_cancellation", "DTLN microphone AEC: -1 auto, 0 off, 1 on", -1);
    addStringConfig("record_audio_echo_backend", "AEC backend: cpu or experimental npu", "cpu");
    addStringConfig("record_audio_mix", "Audio mixing: manual, auto-balance, voice-priority", "voice-priority");
    addIntConfig("record_audio_system_gain", "System gain in dB (-61=mute, max 24)", 0);
    addIntConfig("record_audio_mic_gain", "Microphone gain in dB (-61=mute, max 24)", 0);
    addStringConfig("record_audio", "Recording sound: off, system, microphone, mix", "off");
    addStringConfig("record_audio_output", "Sound source: auto, default, output device, or window:<address>", "auto");
    addStringConfig("record_audio_input", "Microphone source or default", "default");
    addStringConfig("record_codec", "Default recording codec", "auto");
    addStringConfig("record_transparent_codec", "Default transparent recording codec", "auto");
    addBoolConfig("record_solid_alpha", "Keep alpha outside follow-system/white/black window recording content when the encoder supports it", false);
    addStringConfig("record_preset", "FFmpeg preset", "veryfast");
    addStringConfig("record_gsr_flags", "Extra gpu-screen-recorder flags", "");
    addStringConfig("record_window_backend", "Recording backend (auto, compositor, or gsr-visible)", "auto");
    addIntConfig("record_max_seconds", "Optional automatic recording stop in seconds", 0);
    addIntConfig("record_countdown_seconds", "Recording start countdown in seconds", 0);
    addIntConfig("thumbnail_timeout_ms", "Thumbnail auto-close timeout in milliseconds", 5000);
    addStringConfig("thumbnail_monitor", "Thumbnail target monitor: active, primary, all, or an output name", "active");
    addStringConfig("helper", "Optional helper executable override", "");
    addStringConfig("watermark", "Watermark path or built-in name", "");
    addStringConfig("watermark_position", "Watermark position", "central");
    addStringConfig("watermark_width", "Watermark width", "20%");
    addStringConfig("watermark_offset", "Watermark offset", "0 0");
    addBoolConfig("timing", "Enable HyprCapture timing traces", false);
    addStringConfig("timing_file", "Private timing trace output file", "");
}

SDispatchResult dispatchResult(const hyprcapture::LaunchResult& result) {
    return result.success ? SDispatchResult{.success = true} : SDispatchResult{.success = false, .error = result.error};
}

hyprcapture::CaptureDefaults readDefaults() {
    hyprcapture::CaptureDefaults defaults;
    defaults.mode = hyprcapture::parseCaptureMode(configString("default_mode", hyprcapture::toString(defaults.mode)), defaults.mode);
    defaults.fullscreenScope =
        hyprcapture::parseFullscreenScope(configString("fullscreen_scope", hyprcapture::toString(defaults.fullscreenScope)), defaults.fullscreenScope);
    defaults.overlayScope =
        hyprcapture::parseOverlayScope(configString("overlay_scope", hyprcapture::toString(defaults.overlayScope)), defaults.overlayScope);
    defaults.windowBackground =
        hyprcapture::parseWindowBackground(configString("window_background", hyprcapture::toString(defaults.windowBackground)), defaults.windowBackground);
    defaults.windowBorder = hyprcapture::parseDecorationPolicy(configString("window_border", hyprcapture::toString(defaults.windowBorder)), defaults.windowBorder);
    defaults.windowShadow = hyprcapture::parseDecorationPolicy(configString("window_shadow", hyprcapture::toString(defaults.windowShadow)), defaults.windowShadow);
    defaults.notificationBackend =
        hyprcapture::parseNotificationBackend(configString("notification_backend", hyprcapture::toString(defaults.notificationBackend)), defaults.notificationBackend);
    defaults.save = configBool("save", defaults.save);
    defaults.clipboard = configBool("clipboard", defaults.clipboard);
    defaults.showThumbnail = configBool("show_thumbnail", defaults.showThumbnail);
    defaults.language = configString("language", defaults.language);
    defaults.screenshotNotification = configBool("screenshot_notification", defaults.screenshotNotification);
    defaults.includeCursor = configBool("include_cursor", defaults.includeCursor);
    defaults.rememberSettings = configBool("remember_settings", defaults.rememberSettings);
    defaults.allowQuick = configBool("allow_quick", defaults.allowQuick);
    defaults.confirmBeforeCapture = configBool("confirm_before_capture", defaults.confirmBeforeCapture);
    defaults.fushionMode = configBool("fusion_mode", defaults.fushionMode) || configBool("fushion_mode", false);
    defaults.captureFullscreenClientsAsMonitor =
        configBool("capture_fullscreen_clients_as_monitor", defaults.captureFullscreenClientsAsMonitor);
    defaults.dynamicWindowMetadata = configBool("dynamic_window_metadata", defaults.dynamicWindowMetadata);
    defaults.windowWheelScroll = configBool("window_wheel_scroll", defaults.windowWheelScroll);
    defaults.windowWheelScope =
        hyprcapture::parseWindowWheelScope(configString("window_wheel_scope", hyprcapture::toString(defaults.windowWheelScope)), defaults.windowWheelScope);
    defaults.fullscreenPreviewRounding = configString("fullscreen_preview_rounding", defaults.fullscreenPreviewRounding);
    defaults.saveDir = configString("save_dir", defaults.saveDir);
    defaults.screenshotFormat = configString("screenshot_format", defaults.screenshotFormat);
    if (defaults.screenshotFormat != "heic") defaults.screenshotFormat = "png";
    const auto screenshotMode = configString("screenshot_mode", "");
    if (screenshotMode == "hdr") defaults.screenshotFormat = "heic";
    else if (screenshotMode == "sdr") defaults.screenshotFormat = "png";
    defaults.showHdrControl = configBool("show_hdr_control", defaults.showHdrControl);
    defaults.hdrSaveSdr = configBool("hdr_save_sdr", defaults.hdrSaveSdr);
    defaults.hdrDragFormat = configString("hdr_drag_format", "heic") == "sdr" ? "sdr" : "heic";
    defaults.hdrClipboardFormat = configString("hdr_clipboard_format", "heic") == "sdr" ? "sdr" : "heic";
    defaults.filenameTemplate = configString("filename_template", defaults.filenameTemplate);
    defaults.notificationTitleTemplate = configString("notification_title_template", defaults.notificationTitleTemplate);
    defaults.notificationBodyTemplate = configString("notification_body_template", defaults.notificationBodyTemplate);
    defaults.helper = configString("helper", defaults.helper);
    defaults.recordSaveDir = configString("record_save_dir", defaults.recordSaveDir);
    defaults.recordFilenameTemplate = configString("record_filename_template", defaults.recordFilenameTemplate);
    defaults.recordFormat = configString("record_format", defaults.recordFormat);
    defaults.recordTransparentFormat = configString("record_transparent_format", defaults.recordTransparentFormat);
    defaults.recordAudioEchoCancellation = std::clamp<std::int64_t>(configInt("record_audio_echo_cancellation", -1), -1, 1);
    defaults.recordAudioEchoBackend = configString("record_audio_echo_backend", "cpu");
    if (defaults.recordAudioEchoBackend != "npu") defaults.recordAudioEchoBackend = "cpu";
    defaults.recordAudioMix = configString("record_audio_mix", "voice-priority");
    if (defaults.recordAudioMix != "manual" && defaults.recordAudioMix != "auto-balance" && defaults.recordAudioMix != "voice-priority") defaults.recordAudioMix = "voice-priority";
    defaults.recordAudioSystemGain = std::clamp<std::int64_t>(configInt("record_audio_system_gain", 0), -61, 24);
    defaults.recordAudioMicGain = std::clamp<std::int64_t>(configInt("record_audio_mic_gain", 0), -61, 24);
    defaults.recordAudio = hyprcapture::parseRecordAudio(configString("record_audio", "off"));
    defaults.recordAudioOutput = configString("record_audio_output", "auto");
    defaults.recordAudioInput = configString("record_audio_input", "default");
    defaults.recordCodec = configString("record_codec", defaults.recordCodec);
    defaults.recordTransparentCodec = configString("record_transparent_codec", defaults.recordTransparentCodec);
    defaults.recordSolidAlpha = configBool("record_solid_alpha", defaults.recordSolidAlpha);
    defaults.recordPreset = configString("record_preset", defaults.recordPreset);
    defaults.recordGsrFlags = configString("record_gsr_flags", defaults.recordGsrFlags);
    defaults.recordWindowBackend =
        hyprcapture::parseRecordWindowBackend(configString("record_window_backend", hyprcapture::toString(defaults.recordWindowBackend)), defaults.recordWindowBackend);
    defaults.recordFps = configInt("record_fps", defaults.recordFps);
    defaults.recordFpsOptions = configString("record_fps_options", defaults.recordFpsOptions);
    defaults.recordWindowFpsLimit = configInt("record_window_fps_limit", defaults.recordWindowFpsLimit);
    defaults.recordWindowRealBgFpsLimit = configInt("record_window_real_bg_fps_limit", defaults.recordWindowRealBgFpsLimit);
    defaults.recordMaxSeconds = configInt("record_max_seconds", defaults.recordMaxSeconds);
    defaults.recordCountdownSeconds = std::clamp<std::int64_t>(configInt("record_countdown_seconds", defaults.recordCountdownSeconds), 0, 60);
    defaults.thumbnailTimeoutMs = configInt("thumbnail_timeout_ms", defaults.thumbnailTimeoutMs);
    defaults.thumbnailMonitor = configString("thumbnail_monitor", defaults.thumbnailMonitor);
    defaults.watermark = configString("watermark", defaults.watermark);
    defaults.watermarkPosition =
        hyprcapture::parseWatermarkPosition(configString("watermark_position", hyprcapture::toString(defaults.watermarkPosition)), defaults.watermarkPosition);
    defaults.watermarkWidth = configString("watermark_width", defaults.watermarkWidth);
    defaults.watermarkOffset = configString("watermark_offset", defaults.watermarkOffset);
    return defaults;
}

void installOverlayLayerRule() {
    using namespace Desktop::Rule;

    ruleEngine()->unregisterRule(kOverlayLayerRuleName);

    SP<CLayerRule> rule = makeShared<CLayerRule>(kOverlayLayerRuleName);
    rule->registerMatch(RULE_PROP_NAMESPACE, "^hyprcapture-ui$");
    rule->addEffect(LAYER_RULE_EFFECT_NO_ANIM, "1");
    ruleEngine()->registerRule(SP<IRule>{rule});
    ruleEngine()->updateAllRules();
}

SDispatchResult openCapture(const std::string& args, bool quick, bool record, bool inPlaceEditToolbar = false, const hyprcapture::CaptureOptions& overrides = {}) {
    if (hyprcapture::sessionLocked()) return {.success = false, .error = "session locked"};
    const auto now = std::chrono::steady_clock::now();
    if (g_lastCaptureDispatch.time_since_epoch().count() != 0 && now - g_lastCaptureDispatch < kMinDispatchInterval) {
        const std::string error = "capture dispatch rate-limited";
        hyprcapture::notifyUser(error, hyprcapture::NotificationLevel::Error, 2500);
        return {.success = false, .error = error};
    }

    auto defaults = readDefaults();
    defaults.inPlaceEditToolbar = inPlaceEditToolbar;
    auto requestedMode = defaults.mode;
    hyprcapture::CaptureOptions stringOptions;
    std::string optionsError;
    if (!hyprcapture::parseCaptureArguments(args, requestedMode, stringOptions, optionsError))
        return {.success = false, .error = optionsError};
    stringOptions.apply(defaults);
    overrides.apply(defaults);
    if (quick && !defaults.allowQuick) {
        const std::string error = "quick capture disabled; set plugin.hyprcapture.allow_quick = true to enable no-confirmation capture";
        if (g_lastQuickRejectNotification.time_since_epoch().count() == 0 || now - g_lastQuickRejectNotification >= kMinDispatchInterval) {
            g_lastQuickRejectNotification = now;
            hyprcapture::notifyUser(error, hyprcapture::NotificationLevel::Error, 5000);
        }
        return {.success = false, .error = error};
    }

    installOverlayLayerRule();

    const auto result = hyprcapture::launchHelper(
        {.defaults = defaults, .requestedMode = requestedMode, .quick = quick, .record = record, .recordActive = hyprcapture::isRecordingActive()});
    if (!result.success) {
        hyprcapture::notifyUser(result.error, hyprcapture::NotificationLevel::Error, 5000);
        return {.success = false, .error = result.error};
    }
    g_lastCaptureDispatch = now;
    return {.success = true};
}

SDispatchResult dispatchOpen(const std::string& args) {
    return openCapture(args, false, false);
}

SDispatchResult dispatchOpenEditor(const std::string& args) {
    return openCapture(args, false, false, true);
}

SDispatchResult dispatchQuick(const std::string& args) {
    return openCapture(args, true, false);
}

SDispatchResult dispatchRecord(const std::string& args) {
    if (hyprcapture::isRecordingActive())
        return {.success = false, .error = "recording already active"};
    return openCapture(args, false, true);
}

SDispatchResult dispatchRecordToggle(const std::string& args) {
    if (hyprcapture::isRecordingActive())
        return dispatchResult(hyprcapture::stopRecording("stopped"));
    return dispatchRecord(args);
}

SDispatchResult dispatchRecordStop(const std::string&) {
    return dispatchResult(hyprcapture::stopRecording("stopped"));
}

SDispatchResult dispatchRecordStart(const std::string& args) {
    const auto result = hyprcapture::startRecordingFromRequestFile(args, readDefaults().helper);
    if (!result.success)
        hyprcapture::notifyUser(result.error, hyprcapture::NotificationLevel::Error, 5000);
    return dispatchResult(result);
}

SDispatchResult dispatchWindowCapture(const std::string& args) {
    const auto result = hyprcapture::captureWindowArtifactFromRequestFile(args);
    if (!result.success)
        hyprcapture::notifyUser(result.error, hyprcapture::NotificationLevel::Error, 5000);
    return dispatchResult(result);
}

SDispatchResult dispatchExportPipe(const std::string& args) {
    const auto result = hyprcapture::captureExportPipeFromRequestFile(args);
    if (!result.success)
        hyprcapture::notifyUser(result.error, hyprcapture::NotificationLevel::Error, 5000);
    return dispatchResult(result);
}

SDispatchResult dispatchWindowStreamStart(const std::string& args) {
    return dispatchResult(hyprcapture::startWindowStreamFromRequestFile(args));
}

SDispatchResult dispatchWindowStreamStop(const std::string& args) {
    return dispatchResult(hyprcapture::stopWindowStreamFromRequestFile(args));
}

SDispatchResult dispatchCancel(const std::string&) {
    return dispatchResult(hyprcapture::cancelCaptureHelpers());
}

int luaDispatchResult(lua_State* L, const SDispatchResult& result) {
    if (result.success)
        return 0;

    lua_pushstring(L, result.error.empty() ? "hyprcapture function failed" : result.error.c_str());
    return lua_error(L);
}

std::string luaOptionalString(lua_State* L, int index) {
    if (lua_gettop(L) < index || lua_isnil(L, index))
        return {};

    return luaL_checkstring(L, index);
}

std::string normalizeHyprcaptureAction(std::string action) {
    constexpr std::string_view prefix = "hyprcapture.";
    if (action.starts_with(prefix))
        action.erase(0, prefix.size());

    if (action == "openEditor")
        return "open_editor";
    if (action == "recordToggle")
        return "record_toggle";
    if (action == "recordStop")
        return "record_stop";
    if (action == "recordStart")
        return "record_start";
    if (action == "windowCapture")
        return "window_capture";
    if (action == "exportPipe")
        return "export_pipe";
    if (action == "windowStreamStart")
        return "window_stream_start";
    if (action == "windowStreamStop")
        return "window_stream_stop";

    std::ranges::replace(action, '-', '_');
    return action;
}

int luaCapture(lua_State* L, bool quick, bool editor, int modeIndex = 1, int optionsIndex = 2) {
    // Errors are raised after the parser's C++ strings leave scope.
    bool failed = false;
    {
        hyprcapture::CaptureOptions options;
        std::string error;
        if (lua_gettop(L) >= modeIndex && !lua_isnil(L, modeIndex) && lua_type(L, modeIndex) != LUA_TSTRING)
            error = "hyprcapture capture mode must be a string or nil";
        else
            hyprcapture::parseLuaCaptureOptions(L, optionsIndex, options, error);
        if (error.empty()) {
            const auto args = lua_gettop(L) < modeIndex || lua_isnil(L, modeIndex) ? std::string{} : std::string(lua_tostring(L, modeIndex));
            const auto result = openCapture(args, quick, false, editor, options);
            if (result.success) return 0;
            error = result.error;
        }
        lua_pushstring(L, error.c_str());
        failed = true;
    }
    return failed ? lua_error(L) : 0;
}

int luaOpen(lua_State* L) {
    if (hyprcapture::sessionLocked()) return luaL_error(L, "session locked");
    return luaCapture(L, false, false);
}

int luaOpenEditor(lua_State* L) {
    if (hyprcapture::sessionLocked()) return luaL_error(L, "session locked");
    return luaCapture(L, false, true);
}

int luaQuick(lua_State* L) {
    if (hyprcapture::sessionLocked()) return luaL_error(L, "session locked");
    return luaCapture(L, true, false);
}

int luaRecord(lua_State* L) {
    if (hyprcapture::sessionLocked()) return luaL_error(L, "session locked");
    return luaDispatchResult(L, dispatchRecord(luaOptionalString(L, 1)));
}

int luaRecordToggle(lua_State* L) {
    if (hyprcapture::sessionLocked()) return luaL_error(L, "session locked");
    return luaDispatchResult(L, dispatchRecordToggle(luaOptionalString(L, 1)));
}

int luaRecordStop(lua_State* L) {
    if (hyprcapture::sessionLocked()) return luaL_error(L, "session locked");
    return luaDispatchResult(L, dispatchRecordStop(""));
}

int luaRecordStart(lua_State* L) {
    if (hyprcapture::sessionLocked()) return luaL_error(L, "session locked");
    return luaDispatchResult(L, dispatchRecordStart(luaOptionalString(L, 1)));
}

int luaScrollSessionStart(lua_State* L) {
    if (hyprcapture::sessionLocked()) return luaL_error(L, "session locked");
    return luaDispatchResult(L, dispatchResult(hyprcapture::startScrollSessionFromRequestFile(luaOptionalString(L, 1))));
}

int luaScrollSessionStop(lua_State* L) {
    if (hyprcapture::sessionLocked()) return luaL_error(L, "session locked");
    return luaDispatchResult(L, dispatchResult(hyprcapture::stopScrollSessionFromRequestFile(luaOptionalString(L, 1))));
}

int luaRegionCapture(lua_State* L) {
    if (hyprcapture::sessionLocked()) return luaL_error(L, "session locked");
    return luaDispatchResult(L, dispatchResult(hyprcapture::captureRegionArtifactFromRequestFile(luaOptionalString(L, 1))));
}

int luaWindowCapture(lua_State* L) {
    if (hyprcapture::sessionLocked()) return luaL_error(L, "session locked");
    return luaDispatchResult(L, dispatchWindowCapture(luaOptionalString(L, 1)));
}

int luaExportPipe(lua_State* L) {
    if (hyprcapture::sessionLocked()) return luaL_error(L, "session locked");
    return luaDispatchResult(L, dispatchExportPipe(luaOptionalString(L, 1)));
}

int luaWindowStreamStart(lua_State* L) {
    if (hyprcapture::sessionLocked()) return luaL_error(L, "session locked");
    return luaDispatchResult(L, dispatchWindowStreamStart(luaOptionalString(L, 1)));
}

int luaWindowStreamStop(lua_State* L) {
    if (hyprcapture::sessionLocked()) return luaL_error(L, "session locked");
    return luaDispatchResult(L, dispatchWindowStreamStop(luaOptionalString(L, 1)));
}

int luaCancel(lua_State* L) {
    if (hyprcapture::sessionLocked()) return luaL_error(L, "session locked");
    return luaDispatchResult(L, dispatchCancel(""));
}

int luaDispatch(lua_State* L) {
    if (hyprcapture::sessionLocked()) return luaL_error(L, "session locked");
    const int captureAction = [](const char* raw) {
        const auto action = normalizeHyprcaptureAction(raw);
        return action == "open" ? 1 : action == "open_editor" ? 2 : action == "quick" ? 3 : 0;
    }(luaL_checkstring(L, 1));
    if (captureAction) return luaCapture(L, captureAction == 3, captureAction == 2, 2, 3);
    const std::string action = normalizeHyprcaptureAction(luaL_checkstring(L, 1));
    const std::string args   = luaOptionalString(L, 2);

    if (action == "open")
        return luaDispatchResult(L, dispatchOpen(args));
    if (action == "open_editor")
        return luaDispatchResult(L, dispatchOpenEditor(args));
    if (action == "quick")
        return luaDispatchResult(L, dispatchQuick(args));
    if (action == "record")
        return luaDispatchResult(L, dispatchRecord(args));
    if (action == "record_toggle")
        return luaDispatchResult(L, dispatchRecordToggle(args));
    if (action == "record_stop")
        return luaDispatchResult(L, dispatchRecordStop(args));
    if (action == "record_start")
        return luaDispatchResult(L, dispatchRecordStart(args));
    if(action=="scroll_session_start") return luaDispatchResult(L,dispatchResult(hyprcapture::startScrollSessionFromRequestFile(args)));
    if(action=="scroll_session_stop") return luaDispatchResult(L,dispatchResult(hyprcapture::stopScrollSessionFromRequestFile(args)));
    if (action == "region_capture")
        return luaDispatchResult(L, dispatchResult(hyprcapture::captureRegionArtifactFromRequestFile(args)));
    if (action == "window_capture")
        return luaDispatchResult(L, dispatchWindowCapture(args));
    if (action == "export_pipe")
        return luaDispatchResult(L, dispatchExportPipe(args));
    if (action == "window_stream_start")
        return luaDispatchResult(L, dispatchWindowStreamStart(args));
    if (action == "window_stream_stop")
        return luaDispatchResult(L, dispatchWindowStreamStop(args));
    if (action == "cancel")
        return luaDispatchResult(L, dispatchCancel(args));

    lua_pushstring(L, ("unknown hyprcapture action: " + action).c_str());
    return lua_error(L);
}

void registerLuaFunctions() {
    if (Config::mgr() && Config::mgr()->type() == Config::CONFIG_LUA) {
        const auto registerLuaFunction = [&](const char* name, PLUGIN_LUA_FN fn) {
            if (!HyprlandAPI::addLuaFunction(g_pluginHandle, "hyprcapture", name, fn))
                hyprcapture::notifyUser(std::string("failed to register Lua function hl.plugin.hyprcapture.") + name, hyprcapture::NotificationLevel::Error, 5000);
        };

        registerLuaFunction("open", luaOpen);
        registerLuaFunction("open_editor", luaOpenEditor);
        registerLuaFunction("quick", luaQuick);
        registerLuaFunction("record", luaRecord);
        registerLuaFunction("record_toggle", luaRecordToggle);
        registerLuaFunction("record_stop", luaRecordStop);
        registerLuaFunction("record_start", luaRecordStart);
        registerLuaFunction("window_capture", luaWindowCapture);
        registerLuaFunction("region_capture", luaRegionCapture);
        registerLuaFunction("scroll_session_start", luaScrollSessionStart);
        registerLuaFunction("scroll_session_stop", luaScrollSessionStop);
        registerLuaFunction("export_pipe", luaExportPipe);
        registerLuaFunction("window_stream_start", luaWindowStreamStart);
        registerLuaFunction("window_stream_stop", luaWindowStreamStop);
        registerLuaFunction("cancel", luaCancel);
        registerLuaFunction("dispatch", luaDispatch);
    }
}

void restoreConfigRegistrations() {
    hyprcapture::registerForConfigManager(g_registrationManager, Config::mgr(), [] {
        // Recreate Values V2 after the old manager has gone. Holding their
        // CConfigValue caches across full-reset would make its earlier cache
        // flush look up plugin names before preReload can register them.
        registerConfigValues();
        registerLuaFunctions();
    });
}

void unregisterLuaFunctions() {
    if (!g_pluginHandle || !Config::mgr() || Config::mgr()->type() != Config::CONFIG_LUA)
        return;

    for (const auto* name : kLuaFunctionNames)
        HyprlandAPI::removeLuaFunction(g_pluginHandle, "hyprcapture", name);
}

// Reject a build whose headers do not match the running compositor. The hash is
// computed here because the API's inline client helper may be ELF-interposed by
// another loaded plugin.
void requireMatchingABI() {
    const auto abiPrefix = [](std::string version) { return version.substr(0, version.find_last_of('.')); };
    const std::string client = std::string(GIT_COMMIT_HASH) + "_aq_" + abiPrefix(AQUAMARINE_VERSION) + "_hu_" + abiPrefix(HYPRUTILS_VERSION) + "_hg_" +
        abiPrefix(HYPRGRAPHICS_VERSION) + "_hc_" + abiPrefix(HYPRCURSOR_VERSION) + "_hlg_" + abiPrefix(HYPRLANG_VERSION);
    const std::string runtime = __hyprland_api_get_hash();
    if (runtime != client)
        throw std::runtime_error("[HyprCapture] Hyprland header/runtime ABI mismatch: runtime=" + runtime + "; client=" + client);
}

} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    g_pluginHandle = handle;
    requireMatchingABI();
    hyprcapture::initializeSessionLock();

    registerConfigValues();

    registerLuaFunctions();
    g_registrationManager = Config::mgr();

    if (!HyprlandAPI::reloadConfig())
        hyprcapture::notifyUser("reloadConfig failed", hyprcapture::NotificationLevel::Error, 5000);
    installOverlayLayerRule();

    refreshTimingConfig();
    g_configReloadListeners.listen(Event::bus()->m_events.config, restoreConfigRegistrations, [] {
        installOverlayLayerRule();
        refreshTimingConfig();
    });
    g_timingTickListener = Event::bus()->m_events.tick.listen([] {
        if (std::chrono::steady_clock::now() >= g_nextTimingRefresh)
            refreshTimingConfig();
    });

    std::string stateServerError;
    if (!hyprcapture::initializeRecordingStateServer(&stateServerError))
        hyprcapture::notifyUser(
            "recording state socket unavailable: " + stateServerError,
            hyprcapture::NotificationLevel::Warning,
            5000);

    return {
        .name = "HyprCapture",
        .description = "Hyprland-only screenshot overlay",
        .author = "wilf",
        .version = "0.4.2",
    };
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_timingTickListener.reset();
    g_configReloadListeners.reset();
    hyprcapture::shutdownSessionLock();
    hyprcapture::cancelCaptureHelpers();
    unregisterLuaFunctions();
    g_registrationManager.reset();
    hyprcapture::shutdownRecording();
    hyprcapture::shutdownArtifactCapture();
    Desktop::Rule::ruleEngine()->unregisterRule(kOverlayLayerRuleName);
    g_pluginHandle = nullptr;
}
