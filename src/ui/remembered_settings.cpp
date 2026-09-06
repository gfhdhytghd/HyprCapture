#include "ui/remembered_settings.hpp"

#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>
#include <utility>

namespace hyprcapture::ui {
namespace {
QString settingsPath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/hyprcapture/last-settings.ini";
}

// Use the same allowlist in both directions. Newly added configuration fields must
// not silently become persistent (paths, capture targets and launch policy in particular).
void transfer(QSettings& settings, CaptureDefaults& defaults, bool reading) {
    const auto value = [&](const char* key, const QString& fallback) {
        if (reading)
            return settings.value(key, fallback).toString();
        settings.setValue(key, fallback);
        return fallback;
    };
    const auto choice = [&](const char* key, std::string& target, const QStringList& choices) {
        const QString selected = value(key, QString::fromStdString(target));
        if (choices.contains(selected)) target = selected.toStdString();
    };
    const auto number = [&](const char* key, std::int64_t& target, int minimum, int maximum) {
        bool ok = false;
        const auto selected = value(key, QString::number(target)).toLongLong(&ok);
        if (ok && selected >= minimum && selected <= maximum) target = selected;
    };
    const auto enumeration = [&](const char* key, auto& target, auto parse) {
        target = parse(value(key, QString::fromStdString(toString(target))).toStdString(), target);
    };
    enumeration("mode", defaults.mode, parseCaptureMode);
    enumeration("fullscreenScope", defaults.fullscreenScope, parseFullscreenScope);
    enumeration("windowBackground", defaults.windowBackground, parseWindowBackground);
    enumeration("recordWindowBackend", defaults.recordWindowBackend, parseRecordWindowBackend);
    enumeration("recordAudio", defaults.recordAudio, parseRecordAudio);
    const QStringList formats{"mp4", "mov", "webm", "mkv", "gif", "apng", "webp"};
    const QStringList codecs{"auto", "h264", "h265", "av1", "vp9", "ffv1"};
    choice("recordFormat", defaults.recordFormat, formats);
    choice("recordTransparentFormat", defaults.recordTransparentFormat, formats);
    choice("recordCodec", defaults.recordCodec, codecs);
    choice("recordTransparentCodec", defaults.recordTransparentCodec, codecs);
    number("recordFps", defaults.recordFps, 1, 240);
    number("recordMaxSeconds", defaults.recordMaxSeconds, 0, 86400);
    number("recordAudioSystemGain", defaults.recordAudioSystemGain, -61, 24);
    number("recordAudioMicGain", defaults.recordAudioMicGain, -61, 24);
    choice("recordAudioMix", defaults.recordAudioMix, {"manual", "auto-balance", "voice-priority"});
    // Legacy remembered bool did not distinguish a default from user intent.
    // AEC policy now lives separately and is never restored from that key.
    for (auto [key, target] : {std::pair{"recordAudioOutput", &defaults.recordAudioOutput},
                               std::pair{"recordAudioInput", &defaults.recordAudioInput}}) {
        const QString selected = value(key, QString::fromStdString(*target));
        // Window addresses belong to one session; resolve a fresh target next time.
        if (selected.startsWith("window:")) *target = "auto";
        else if (!selected.isEmpty() && selected.size() <= 4096) *target = selected.toStdString();
    }
}
}

bool restoreSettings(CaptureDefaults& defaults) {
    if (!defaults.rememberSettings) return false;
    QSettings settings(settingsPath(), QSettings::IniFormat);
    if (settings.value("version").toInt() != 1 || settings.status() != QSettings::NoError) return false;
    transfer(settings, defaults, true);
    return true;
}

bool saveSettings(const CaptureDefaults& defaults) {
    if (!defaults.rememberSettings) return true;
    QDir directory(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation));
    if (!directory.mkpath("hyprcapture")) return false;
    QSettings settings(settingsPath(), QSettings::IniFormat);
    settings.setValue("version", 1);
    auto snapshot = defaults;
    // Never write transient window addresses to disk either.
    if (snapshot.recordAudioOutput.starts_with("window:")) snapshot.recordAudioOutput = "auto";
    transfer(settings, snapshot, false);
    settings.sync();
    return settings.status() == QSettings::NoError;
}
void restoreAecPreferences(CaptureDefaults& defaults) {
    QSettings s(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)+"/hyprcapture/aec.ini",QSettings::IniFormat);
    if(s.value("version").toInt()!=1)return;
    const int policy=s.value("policy",-1).toInt();
    if(defaults.recordAudioEchoCancellation==-1&&policy>=-1&&policy<=1)defaults.recordAudioEchoCancellation=policy;
    const auto backend=s.value("backend","cpu").toString();
    if(defaults.recordAudioEchoBackend=="cpu"&&(backend=="cpu"||backend=="npu"))defaults.recordAudioEchoBackend=backend.toStdString();
}
void saveAecPreferences(const CaptureDefaults& defaults) {
    QSettings s(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)+"/hyprcapture/aec.ini",QSettings::IniFormat);
    s.setValue("version",1);s.setValue("policy",int(defaults.recordAudioEchoCancellation));
    s.setValue("backend",QString::fromStdString(defaults.recordAudioEchoBackend));s.sync();
}
}
