#include <QTemporaryDir>
#include <QSettings>
#include "ui/remembered_settings.hpp"
#include "shared/protocol.hpp"
#include <QTimer>
#include <QSocketNotifier>
#include <unistd.h>
#include "ui/audio_meter.hpp"
#include <QSlider>

#include "ui/capture_overlay.hpp"
#include "audio/helper.hpp"
#include <QApplication>
#include <QPushButton>
#include <QTest>
#include <QThread>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <iostream>
#include <cstdlib>

void require(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
QPushButton* button(CaptureOverlay& overlay, const char* name) {
    auto* control = overlay.findChild<QWidget*>(name);
    require(control, "control missing");
    auto* result = control->findChild<QPushButton*>();
    require(result, "control button missing");
    return result;
}
void choose(CaptureOverlay& overlay, const char* name, const QString& value) {
    QTest::mouseClick(button(overlay, name), Qt::LeftButton);
    QTest::qWait(20);
    for (auto* option : overlay.findChildren<QPushButton*>()) {
        if (option->isVisible() && option->property("value").toString() == value) {
            QTest::mouseClick(option, Qt::LeftButton);
            QTest::qWait(20);
            return;
        }
    }
    require(false, "visible option missing");
}
int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--sound-list" && !qEnvironmentVariableIsSet("HYPRCAPTURE_TEST_REAL_SOUND")) {
        QThread::msleep(200);
        std::cout << R"({"outputs":[{"name":"test-output","description":"Test speakers"},{"name":"test-hdmi","description":"Test HDMI"}],"inputs":[{"name":"test-input","description":"Test microphone"}],"windows":[{"name":"window:0x123","description":"Window · Test player — Player","pid":123}]})" << '\n';
        return 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--sound-meter" && !qEnvironmentVariableIsSet("HYPRCAPTURE_TEST_REAL_SOUND")) {
        QCoreApplication app(argc, argv);
        QTimer timer;
        QObject::connect(&timer, &QTimer::timeout, [] {
            std::cout << R"({"levels":{"System":{"peak":0.1,"rms":0.05,"available":true},"Microphone":{"peak":0.1,"rms":0.05,"available":true}}})" << std::endl;
        });
        QSocketNotifier stop(STDIN_FILENO, QSocketNotifier::Read);
        QObject::connect(&stop, &QSocketNotifier::activated, [&] { char c; if (read(STDIN_FILENO, &c, 1) <= 0) app.quit(); });
        timer.start(50); return app.exec();
    }
    if (argc > 1 && std::string_view(argv[1]).starts_with("--sound-"))
        return hyprcapture::audio::runHelper(argc, argv);
    QTemporaryDir configDir;
    require(configDir.isValid(), "temporary settings directory");
    qputenv("XDG_CONFIG_HOME", configDir.path().toUtf8());
    qputenv("XDG_CACHE_HOME", (configDir.path()+"/cache").toUtf8());
    qputenv("XDG_DATA_HOME", (configDir.path()+"/data").toUtf8());
    QApplication app(argc, argv);
    QString expectedOutput = "test-output", expectedLabel = "Test speakers";
    if (qEnvironmentVariableIsSet("HYPRCAPTURE_TEST_REAL_SOUND")) {
        QProcess list;
        list.start(QCoreApplication::applicationFilePath(), {"--sound-list"});
        require(list.waitForFinished(4000) && list.exitCode() == 0, "real device enumeration failed");
        const auto outputs = QJsonDocument::fromJson(list.readAllStandardOutput()).object()["outputs"].toArray();
        require(!outputs.isEmpty(), "real output device needed for integration test");
        expectedOutput = outputs.first().toObject()["name"].toString();
        expectedLabel = outputs.first().toObject()["description"].toString();
    }
    hyprcapture::CaptureDefaults defaults;
    defaults.recordAudio = hyprcapture::RecordAudio::Mix;
    defaults.recordAudioInput = "very-long-microphone-device-name-with-spaces-and-UTF8-麦克风";
    CaptureOverlay overlay(defaults, false, true, false, "{}");
    overlay.show();
    QTest::qWait(20);
    QTest::mouseClick(button(overlay, "soundOutput"), Qt::LeftButton);
    QTest::qWait(400);
    QPushButton* discovered = nullptr;
    for (auto* option : overlay.findChildren<QPushButton*>())
        if (option->property("value").toString() == expectedOutput) discovered = option;
    require(discovered && discovered->isVisible(), "async discovery must show device in open popup");
    auto* viewport = discovered->parentWidget()->parentWidget();
    require(viewport->rect().contains(QRect(discovered->mapTo(viewport, QPoint(0, 0)), discovered->size())), "discovered device must fit visible scroll viewport, not just hidden content");
    QTest::mouseClick(discovered, Qt::LeftButton);
    require(button(overlay, "soundOutput")->toolTip().contains(expectedLabel), "discovered device selectable");
    QTest::mouseClick(button(overlay, "soundOutput"), Qt::LeftButton);
    QTest::qWait(400);
    discovered = nullptr;
    for (auto* option : overlay.findChildren<QPushButton*>())
        if (option->property("value").toString() == expectedOutput) discovered = option;
    require(discovered && discovered->isVisible(), "device survives repeated refresh");
    viewport = discovered->parentWidget()->parentWidget();
    require(viewport->rect().contains(QRect(discovered->mapTo(viewport, QPoint(0, 0)), discovered->size())), "reopening must not collapse popup to default row");
    QTest::mouseClick(discovered, Qt::LeftButton);

    choose(overlay, "soundOutput", "auto");
    require(button(overlay, "soundOutput")->toolTip().contains("Auto"), "auto source selectable");
    if (!qEnvironmentVariableIsSet("HYPRCAPTURE_TEST_REAL_SOUND")) {
        choose(overlay, "soundOutput", "window:0x123");
        require(button(overlay, "soundOutput")->toolTip().contains("Test player"), "window source selectable");
        QTest::mouseClick(button(overlay, "soundOutput"), Qt::LeftButton); QTest::qWait(300);
        require(button(overlay, "soundOutput")->toolTip().contains("Test player"), "window source survives refresh");
        QTest::mouseClick(button(overlay, "soundOutput"), Qt::LeftButton);
        choose(overlay, "soundOutput", "auto");
    }
    auto* sound = overlay.findChild<QWidget*>("soundOptions");
    require(sound && sound->isVisible(), "third row visible for video");
    require(button(overlay, "soundMode")->text().contains("Mix"), "initial mix selection");
    require(button(overlay, "soundInput")->toolTip().contains("very-long"), "full device tooltip");
    choose(overlay, "soundMode", "off");
    choose(overlay, "soundPreset", "manual");
    require(overlay.findChild<QWidget*>("soundMixer")->isVisible(), "manual fourth row visible even with Sound off");
    QTest::qWait(300);
    QProcess* preview = nullptr;
    for (auto* process : overlay.findChildren<QProcess*>())
        if (process->arguments().value(0) == "--sound-meter") preview = process;
    require(preview && preview->state() == QProcess::Running && preview->arguments().value(1) == "mix", "Sound off still previews both channels");
    if (!qEnvironmentVariableIsSet("HYPRCAPTURE_TEST_REAL_SOUND"))
        for (const char* name : {"soundMeter", "micMeter"})
            require(std::abs(overlay.findChild<QWidget*>(name)->property("postGainPeak").toDouble() - .1) < .0001, "Off preview receives live levels");
    auto* aec = overlay.findChild<QWidget*>("echoCancellation");
    require(aec && button(overlay, "echoCancellation")->text().contains("Auto") && preview->arguments().value(4) == "-1" && preview->arguments().last() == "cpu", "AEC defaults to automatic CPU in preview");
    choose(overlay, "echoCancellation", "0"); QTest::qWait(1200);
    preview = nullptr;
    for (auto* process : overlay.findChildren<QProcess*>())
        if (process->arguments().value(0) == "--sound-meter" && process->state() == QProcess::Running) preview = process;
    require(preview && preview->arguments().value(4) == "0", "AEC switch reaches helper");
    require(button(overlay, "soundInput")->isVisible() && button(overlay, "soundOutput")->isVisible(), "off keeps all sound options visible");
    choose(overlay, "soundMode", "microphone");
    QTest::qWait(150);
    require(preview->state() == QProcess::Running && preview->arguments().value(1) == "mix", "recording mode leaves preview running");
    require(overlay.findChild<QSlider*>("soundGain")->isEnabled() && overlay.findChild<QSlider*>("micGain")->isEnabled(), "both preview gains stay adjustable");
    require(button(overlay, "soundInput")->isVisible() && button(overlay, "soundOutput")->isVisible(), "microphone keeps all devices visible");
    choose(overlay, "recordFormat", "gif");
    require(!button(overlay, "soundMode")->isEnabled(), "animations disable audio");
    choose(overlay, "recordFormat", "mp4");
    require(button(overlay, "soundMode")->isEnabled() && button(overlay, "soundMode")->toolTip().contains("Microphone"), "video restores selection");
    choose(overlay, "soundMode", "mix");
    choose(overlay, "soundPreset", "manual");
    require(overlay.findChild<QWidget*>("soundMixer")->isVisible(), "manual fourth row visible");
    auto* gain = overlay.findChild<QSlider*>("micGain");
    auto* meter = static_cast<AudioMeter*>(overlay.findChild<QWidget*>("micMeter"));
    require(gain && meter, "manual gain and meter present");
    if (!qEnvironmentVariableIsSet("HYPRCAPTURE_TEST_REAL_SOUND")) {
        QTest::qWait(300);
        require(std::abs(meter->property("postGainPeak").toDouble() - .1) < .0001, "live helper telemetry reaches visible meter");
    } else meter->setLevels(.1, .05, true);
    gain->setValue(6);
    require(std::abs(meter->property("postGainPeak").toDouble() - .199526) < .0001, "meter shows post gain samples");
    gain->setValue(-61);
    require(meter->property("postGainPeak").toDouble() == 0, "mute truly silences meter");
    gain->setValue(6);
    choose(overlay, "soundPreset", "auto-balance");
    require(!overlay.findChild<QWidget*>("soundMixer")->isVisible(), "automatic hides fourth row");
    require(aec->isVisible(), "AEC remains accessible in auto balance");
    choose(overlay, "soundPreset", "voice-priority");
    require(aec->isVisible(), "AEC remains accessible in voice priority");
    choose(overlay, "soundPreset", "manual");
    CaptureOverlay second(overlay, QRect(0, 0, 360, 640), true);
    second.show();
    second.adoptInteractionState(overlay);
    QTest::qWait(150);
    require(button(second, "soundMode")->toolTip().contains("Mix"), "monitor state sync");
    require(button(second, "soundInput")->toolTip().contains("very-long"), "monitor device state sync");
    second.resize(360, 640);
    QTest::qWait(50);
    for (const auto* name : {"soundMode", "soundOutput", "soundInput", "soundPreset"}) {
        auto* b = button(second, name);
        const QRect geometry(b->mapTo(&second, QPoint(0,0)), b->size());
        require(second.rect().contains(geometry), "sound controls within narrow overlay");
    }
    require(second.findChild<QSlider*>("micGain")->value() == 6, "gain survives monitor switch");
    require(button(second, "echoCancellation")->toolTip().contains("Always off"), "AEC setting survives monitor switch");
    for (const char* name : {"soundMixer", "micGain", "soundGain", "micMeter", "soundMeter", "echoCancellation"}) {
        auto* widget = second.findChild<QWidget*>(name);
        require(widget && widget->isVisible(), "manual narrow mixer visible");
        require(second.rect().contains(QRect(widget->mapTo(&second, QPoint(0, 0)), widget->size())), "fourth row fits narrow overlay");
    }
    if (qEnvironmentVariableIsSet("HYPRCAPTURE_TEST_SCREENSHOT")) second.grab().save(qEnvironmentVariable("HYPRCAPTURE_TEST_SCREENSHOT"));
    QTest::mouseClick(button(second, "soundOutput"), Qt::LeftButton);
    QTest::qWait(300);
    for (auto* panel : second.findChildren<QWidget*>("inlineSelectPopup"))
        if (panel->isVisible()) require(second.rect().contains(panel->geometry()), "device popup stays inside overlay after refresh");
    second.hide(); overlay.hide(); QTest::qWait(300);
    for (auto* owner : {&second, &overlay})
        for (auto* process : owner->findChildren<QProcess*>())
            if (process->arguments().value(0) == "--sound-meter") require(process->state() == QProcess::NotRunning, "closing overlay stops live capture");
    hyprcapture::CaptureDefaults remembered;
    remembered.rememberSettings = true;
    // Exercise the plugin session transport as well as a real widget edit + Esc.
    hyprcapture::CaptureSession session;
    session.id = "remember-settings-test";
    session.defaults = remembered;
    const auto encoded = hyprcapture::encodeSessionJson(session);
    const auto decoded = hyprcapture::decodeSessionJson(encoded);
    require(decoded && decoded->defaults.rememberSettings, "remember option survives session transport");
    {
        CaptureOverlay first({}, false, true, false, QString::fromStdString(encoded));
        first.show();
        QTest::qWait(50);
        choose(first, "recordFormat", "mkv");
        choose(first, "soundMode", "microphone");
        // Like focus-mode monitor handoff: only the finishing peer saves.
        CaptureOverlay peer(first, QRect(0, 0, 1280, 720), true);
        peer.adoptInteractionState(first);
        first.setOverlayActive(false);
        peer.show();
        QTest::keyClick(&peer, Qt::Key_Escape);
        QTest::qWait(200);
    }
    {
        CaptureOverlay reopened(remembered, false, true, false, "{}");
        require(button(reopened, "recordFormat")->toolTip().contains("mkv"), "format survives close and reopen");
        require(button(reopened, "soundMode")->toolTip().contains("Microphone"), "sound survives close and reopen");
    }
    {
        auto disabled = remembered;
        disabled.rememberSettings = false;
        CaptureOverlay reopened(disabled, false, true, false, "{}");
        require(button(reopened, "recordFormat")->toolTip().contains("mp4"), "disabled uses configured format");
        require(hyprcapture::ui::saveSettings(disabled), "disabled save is a no-op");
    }
    {
        // Do not process events: quick captures are scheduled for the next event loop.
        CaptureOverlay quick(remembered, true, true, false, "{}");
        require(button(quick, "recordFormat")->toolTip().contains("mp4"), "quick skips remembered settings");
        QMetaObject::invokeMethod(&quick, "finishingStarted", Qt::DirectConnection);
        CaptureOverlay stop(remembered, false, true, true, "{}");
        QMetaObject::invokeMethod(&stop, "finishingStarted", Qt::DirectConnection);
        auto check = remembered;
        require(hyprcapture::ui::restoreSettings(check) && check.recordFormat == "mkv", "quick and stop do not overwrite settings");
    }
    {
        QSettings settings(configDir.path() + "/hyprcapture/last-settings.ini", QSettings::IniFormat);
        settings.setValue("recordFps", -20);
        settings.setValue("recordCodec", "invalid-codec");
        settings.setValue("recordAudioOutput", "window:0x123");
        settings.setValue("saveDir", "/unexpected");
        settings.sync();
        auto check = remembered;
        const auto configuredPath = check.saveDir;
        require(hyprcapture::ui::restoreSettings(check), "read saved state");
        require(check.recordFps == 30 && check.recordCodec == "auto", "invalid values fall back to configured defaults");
        require(check.recordAudioOutput == "auto", "window targets are not restored");
        require(check.saveDir == configuredPath, "unrelated configuration is not restored");
    }
    std::cout << "Remember settings: reopen, Esc, monitor handoff, disabled, quick, stop and invalid state passed\n";
    std::cout << "Sound UI: modes, animation restore, long names, narrow layout and monitor sync passed\n";
}
