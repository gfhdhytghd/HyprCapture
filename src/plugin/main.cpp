#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <string>
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

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include "plugin/artifact_capture.hpp"
#include "plugin/notification.hpp"
#include "plugin/recording.hpp"
#include "plugin/session_launcher.hpp"

inline HANDLE g_pluginHandle = nullptr;

namespace {

constexpr const char* kOverlayLayerRuleName = "hyprcapture-ui-no-compositor-anim";
constexpr auto        kMinDispatchInterval = std::chrono::milliseconds(750);
constexpr std::array  kLuaFunctionNames = {
    "open",
    "quick",
    "record",
    "record_toggle",
    "record_stop",
    "record_start",
    "window_capture",
    "export_pipe",
    "record_start_dispatcher",
    "window_capture_dispatcher",
    "record_stop_dispatcher",
    "cancel",
    "dispatch",
};

std::chrono::steady_clock::time_point g_lastCaptureDispatch {};
std::chrono::steady_clock::time_point g_lastQuickRejectNotification {};
std::deque<std::string>               g_configValueNames;

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
    addStringConfig("window_background", "Window capture background mode", "follow-system");
    addStringConfig("window_border", "Window capture border policy", "keep");
    addStringConfig("window_shadow", "Window capture shadow policy", "keep");
    addStringConfig("notification_backend", "Backend for non-error notifications (hyprland or system)", "hyprland");
    addBoolConfig("save", "Save captures to disk", true);
    addBoolConfig("clipboard", "Copy captures to the clipboard", true);
    addBoolConfig("show_thumbnail", "Show a result thumbnail after capture", true);
    addBoolConfig("include_cursor", "Include the cursor in captures", false);
    addBoolConfig("allow_quick", "Enable no-confirmation quick capture dispatchers", false);
    addBoolConfig("confirm_before_capture", "Require explicit confirmation after target selection for normal open captures", false);
    addBoolConfig("fusion_mode", "Fuse region and window interactions in one overlay", false);
    addBoolConfig("fushion_mode", "Legacy alias for fusion_mode", false);
    addBoolConfig("capture_fullscreen_clients_as_monitor", "Capture a fullscreen client as its whole monitor in window and fusion modes", false);
    addStringConfig("save_dir", "Capture output directory", "$XDG_PICTURES_DIR/Screenshots");
    addStringConfig("filename_template", "Screenshot filename strftime template with window metadata variables", "Screenshot-%Y-%m-%d-%H%M%S.png");
    addStringConfig("record_save_dir", "Recording output directory", "$XDG_VIDEOS_DIR/Screenrecords");
    addStringConfig("record_filename_template", "Recording filename strftime template", "Recording-%Y-%m-%d-%H%M%S.mp4");
    addStringConfig("record_format", "Default recording format", "mp4");
    addStringConfig("record_transparent_format", "Default transparent recording container", "webm");
    addIntConfig("record_fps", "Recording frame rate", 30);
    addStringConfig("record_fps_options", "Recording frame rate choices", "15 24 30 60");
    addIntConfig("record_window_fps_limit", "Compositor window recording FPS cap", 12);
    addIntConfig("record_window_real_bg_fps_limit", "Real-background window recording FPS cap", 8);
    addStringConfig("record_codec", "Default recording codec", "libx264");
    addStringConfig("record_transparent_codec", "Default transparent recording codec", "auto");
    addBoolConfig("record_solid_alpha", "Keep alpha outside follow-system/white/black window recording content when the encoder supports it", false);
    addStringConfig("record_preset", "FFmpeg preset", "veryfast");
    addStringConfig("record_gsr_flags", "Extra gpu-screen-recorder flags", "");
    addStringConfig("record_window_backend", "Window recording backend", "compositor");
    addIntConfig("record_max_seconds", "Optional automatic recording stop in seconds", 0);
    addIntConfig("record_countdown_seconds", "Recording start countdown in seconds", 0);
    addIntConfig("thumbnail_timeout_ms", "Thumbnail auto-close timeout in milliseconds", 5000);
    addStringConfig("thumbnail_monitor", "Thumbnail target monitor: active, primary, or an output name", "active");
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
    defaults.windowBackground =
        hyprcapture::parseWindowBackground(configString("window_background", hyprcapture::toString(defaults.windowBackground)), defaults.windowBackground);
    defaults.windowBorder = hyprcapture::parseDecorationPolicy(configString("window_border", hyprcapture::toString(defaults.windowBorder)), defaults.windowBorder);
    defaults.windowShadow = hyprcapture::parseDecorationPolicy(configString("window_shadow", hyprcapture::toString(defaults.windowShadow)), defaults.windowShadow);
    defaults.save = configBool("save", defaults.save);
    defaults.clipboard = configBool("clipboard", defaults.clipboard);
    defaults.showThumbnail = configBool("show_thumbnail", defaults.showThumbnail);
    defaults.includeCursor = configBool("include_cursor", defaults.includeCursor);
    defaults.allowQuick = configBool("allow_quick", defaults.allowQuick);
    defaults.confirmBeforeCapture = configBool("confirm_before_capture", defaults.confirmBeforeCapture);
    defaults.fushionMode = configBool("fusion_mode", defaults.fushionMode) || configBool("fushion_mode", defaults.fushionMode);
    defaults.captureFullscreenClientsAsMonitor =
        configBool("capture_fullscreen_clients_as_monitor", defaults.captureFullscreenClientsAsMonitor);
    defaults.saveDir = configString("save_dir", defaults.saveDir);
    defaults.filenameTemplate = configString("filename_template", defaults.filenameTemplate);
    defaults.helper = configString("helper", defaults.helper);
    defaults.recordSaveDir = configString("record_save_dir", defaults.recordSaveDir);
    defaults.recordFilenameTemplate = configString("record_filename_template", defaults.recordFilenameTemplate);
    defaults.recordFormat = configString("record_format", defaults.recordFormat);
    defaults.recordTransparentFormat = configString("record_transparent_format", defaults.recordTransparentFormat);
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
}

SDispatchResult openCapture(const std::string& args, bool quick, bool record) {
    const auto now = std::chrono::steady_clock::now();
    if (g_lastCaptureDispatch.time_since_epoch().count() != 0 && now - g_lastCaptureDispatch < kMinDispatchInterval) {
        const std::string error = "capture dispatch rate-limited";
        hyprcapture::notifyUser(error, hyprcapture::NotificationLevel::Error, 2500);
        return {.success = false, .error = error};
    }

    auto defaults = readDefaults();
    if (quick && !defaults.allowQuick) {
        const std::string error = "hyprcapture:quick disabled; set plugin:hyprcapture:allow_quick = 1 to enable no-confirmation capture";
        if (g_lastQuickRejectNotification.time_since_epoch().count() == 0 || now - g_lastQuickRejectNotification >= kMinDispatchInterval) {
            g_lastQuickRejectNotification = now;
            hyprcapture::notifyUser(error, hyprcapture::NotificationLevel::Error, 5000);
        }
        return {.success = false, .error = error};
    }

    installOverlayLayerRule();

    const auto requestedMode = hyprcapture::parseCaptureMode(args, defaults.mode);
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
    const auto result = hyprcapture::startRecordingFromRequestFile(args);
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

SDispatchResult dispatchCancel(const std::string&) {
    return {.success = true};
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

std::string normalizeHyprcaptureDispatcher(std::string dispatcher) {
    if (dispatcher == "open" || dispatcher == "hyprcapture.open")
        return "hyprcapture:open";
    if (dispatcher == "quick" || dispatcher == "hyprcapture.quick")
        return "hyprcapture:quick";
    if (dispatcher == "record" || dispatcher == "hyprcapture.record")
        return "hyprcapture:record";
    if (dispatcher == "record_toggle" || dispatcher == "recordToggle" || dispatcher == "hyprcapture.record_toggle" ||
        dispatcher == "hyprcapture.recordToggle")
        return "hyprcapture:record-toggle";
    if (dispatcher == "record_stop" || dispatcher == "recordStop" || dispatcher == "hyprcapture.record_stop" ||
        dispatcher == "hyprcapture.recordStop")
        return "hyprcapture:record-stop";
    if (dispatcher == "record_start" || dispatcher == "recordStart" || dispatcher == "hyprcapture.record_start" ||
        dispatcher == "hyprcapture.recordStart")
        return "hyprcapture:record-start";
    if (dispatcher == "window_capture" || dispatcher == "windowCapture" || dispatcher == "hyprcapture.window_capture" ||
        dispatcher == "hyprcapture.windowCapture")
        return "hyprcapture:window-capture";
    if (dispatcher == "export_pipe" || dispatcher == "exportPipe" || dispatcher == "hyprcapture.export_pipe" || dispatcher == "hyprcapture.exportPipe")
        return "hyprcapture:export-pipe";
    if (dispatcher == "cancel" || dispatcher == "hyprcapture.cancel")
        return "hyprcapture:cancel";
    return dispatcher;
}

int luaOpen(lua_State* L) {
    return luaDispatchResult(L, dispatchOpen(luaOptionalString(L, 1)));
}

int luaQuick(lua_State* L) {
    return luaDispatchResult(L, dispatchQuick(luaOptionalString(L, 1)));
}

int luaRecord(lua_State* L) {
    return luaDispatchResult(L, dispatchRecord(luaOptionalString(L, 1)));
}

int luaRecordToggle(lua_State* L) {
    return luaDispatchResult(L, dispatchRecordToggle(luaOptionalString(L, 1)));
}

int luaRecordStop(lua_State* L) {
    return luaDispatchResult(L, dispatchRecordStop(""));
}

int luaRecordStart(lua_State* L) {
    return luaDispatchResult(L, dispatchRecordStart(luaOptionalString(L, 1)));
}

int luaWindowCapture(lua_State* L) {
    return luaDispatchResult(L, dispatchWindowCapture(luaOptionalString(L, 1)));
}

int luaExportPipe(lua_State* L) {
    return luaDispatchResult(L, dispatchExportPipe(luaOptionalString(L, 1)));
}

int luaRecordStartDispatcherCallback(lua_State* L) {
    const char* path = lua_tostring(L, lua_upvalueindex(1));
    return luaDispatchResult(L, dispatchRecordStart(path ? path : ""));
}

int luaRecordStartDispatcher(lua_State* L) {
    const std::string path = luaOptionalString(L, 1);
    lua_pushstring(L, path.c_str());
    lua_pushcclosure(L, luaRecordStartDispatcherCallback, 1);
    return 1;
}

int luaWindowCaptureDispatcherCallback(lua_State* L) {
    const char* path = lua_tostring(L, lua_upvalueindex(1));
    return luaDispatchResult(L, dispatchWindowCapture(path ? path : ""));
}

int luaWindowCaptureDispatcher(lua_State* L) {
    const std::string path = luaOptionalString(L, 1);
    lua_pushstring(L, path.c_str());
    lua_pushcclosure(L, luaWindowCaptureDispatcherCallback, 1);
    return 1;
}

int luaCancel(lua_State* L) {
    return luaDispatchResult(L, dispatchCancel(""));
}

int luaRecordStopDispatcherCallback(lua_State* L) {
    return luaDispatchResult(L, dispatchRecordStop(""));
}

int luaRecordStopDispatcher(lua_State* L) {
    lua_pushcclosure(L, luaRecordStopDispatcherCallback, 0);
    return 1;
}

int luaDispatch(lua_State* L) {
    const std::string dispatcher = normalizeHyprcaptureDispatcher(luaL_checkstring(L, 1));
    const std::string args       = luaOptionalString(L, 2);

    if (dispatcher == "hyprcapture:open")
        return luaDispatchResult(L, dispatchOpen(args));
    if (dispatcher == "hyprcapture:quick")
        return luaDispatchResult(L, dispatchQuick(args));
    if (dispatcher == "hyprcapture:record")
        return luaDispatchResult(L, dispatchRecord(args));
    if (dispatcher == "hyprcapture:record-toggle")
        return luaDispatchResult(L, dispatchRecordToggle(args));
    if (dispatcher == "hyprcapture:record-stop")
        return luaDispatchResult(L, dispatchRecordStop(args));
    if (dispatcher == "hyprcapture:record-start")
        return luaDispatchResult(L, dispatchRecordStart(args));
    if (dispatcher == "hyprcapture:window-capture")
        return luaDispatchResult(L, dispatchWindowCapture(args));
    if (dispatcher == "hyprcapture:export-pipe")
        return luaDispatchResult(L, dispatchExportPipe(args));
    if (dispatcher == "hyprcapture:cancel")
        return luaDispatchResult(L, dispatchCancel(args));

    lua_pushstring(L, ("unknown hyprcapture dispatcher: " + dispatcher).c_str());
    return lua_error(L);
}

void unregisterLuaFunctions() {
    if (!g_pluginHandle || !Config::mgr() || Config::mgr()->type() != Config::CONFIG_LUA)
        return;

    for (const auto* name : kLuaFunctionNames)
        HyprlandAPI::removeLuaFunction(g_pluginHandle, "hyprcapture", name);
}

} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    g_pluginHandle = handle;

    registerConfigValues();

    const auto registerDispatcher = [&](const char* name, auto handler) {
        if (!HyprlandAPI::addDispatcherV2(g_pluginHandle, name, handler)) {
            hyprcapture::notifyUser(std::string("failed to register dispatcher ") + name, hyprcapture::NotificationLevel::Error, 5000);
        }
    };

    registerDispatcher("hyprcapture:open", dispatchOpen);
    registerDispatcher("hyprcapture:quick", dispatchQuick);
    registerDispatcher("hyprcapture:record", dispatchRecord);
    registerDispatcher("hyprcapture:record-toggle", dispatchRecordToggle);
    registerDispatcher("hyprcapture:record-stop", dispatchRecordStop);
    registerDispatcher("hyprcapture:record-start", dispatchRecordStart);
    registerDispatcher("hyprcapture:window-capture", dispatchWindowCapture);
    registerDispatcher("hyprcapture:export-pipe", dispatchExportPipe);
    registerDispatcher("hyprcapture:cancel", dispatchCancel);

    if (Config::mgr() && Config::mgr()->type() == Config::CONFIG_LUA) {
        const auto registerLuaFunction = [&](const char* name, PLUGIN_LUA_FN fn) {
            HyprlandAPI::addLuaFunction(g_pluginHandle, "hyprcapture", name, fn);
        };

        registerLuaFunction("open", luaOpen);
        registerLuaFunction("quick", luaQuick);
        registerLuaFunction("record", luaRecord);
        registerLuaFunction("record_toggle", luaRecordToggle);
        registerLuaFunction("record_stop", luaRecordStop);
        registerLuaFunction("record_start", luaRecordStart);
        registerLuaFunction("window_capture", luaWindowCapture);
        registerLuaFunction("export_pipe", luaExportPipe);
        registerLuaFunction("record_start_dispatcher", luaRecordStartDispatcher);
        registerLuaFunction("window_capture_dispatcher", luaWindowCaptureDispatcher);
        registerLuaFunction("record_stop_dispatcher", luaRecordStopDispatcher);
        registerLuaFunction("cancel", luaCancel);
        registerLuaFunction("dispatch", luaDispatch);
    }

    if (!HyprlandAPI::reloadConfig())
        hyprcapture::notifyUser("reloadConfig failed", hyprcapture::NotificationLevel::Error, 5000);
    installOverlayLayerRule();

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
        .version = "0.2.4",
    };
}

APICALL EXPORT void PLUGIN_EXIT() {
    unregisterLuaFunctions();
    hyprcapture::shutdownRecording();
    hyprcapture::shutdownArtifactCapture();
    Desktop::Rule::ruleEngine()->unregisterRule(kOverlayLayerRuleName);
    g_pluginHandle = nullptr;
}
