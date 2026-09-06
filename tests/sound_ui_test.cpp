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
        std::cout << R"({"outputs":[{"name":"test-output","description":"Test speakers"},{"name":"test-hdmi","description":"Test HDMI"}],"inputs":[{"name":"test-input","description":"Test microphone"}]})" << '\n';
        return 0;
    }
    if (argc > 1 && std::string_view(argv[1]).starts_with("--sound-"))
        return hyprcapture::audio::runHelper(argc, argv);
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
    auto* popup = discovered->parentWidget();
    require(popup->rect().contains(discovered->geometry()), "device option must fit its popup content");
    QTest::mouseClick(discovered, Qt::LeftButton);
    require(button(overlay, "soundOutput")->toolTip().contains(expectedLabel), "discovered device selectable");
    auto* sound = overlay.findChild<QWidget*>("soundOptions");
    require(sound && sound->isVisible(), "third row visible for video");
    require(button(overlay, "soundMode")->text().contains("Mix"), "initial mix selection");
    require(button(overlay, "soundInput")->toolTip().contains("very-long"), "full device tooltip");
    choose(overlay, "soundMode", "off");
    require(button(overlay, "soundInput")->isVisible() && button(overlay, "soundOutput")->isVisible(), "off keeps all sound options visible");
    choose(overlay, "soundMode", "microphone");
    require(button(overlay, "soundInput")->isVisible() && button(overlay, "soundOutput")->isVisible(), "microphone keeps all devices visible");
    choose(overlay, "recordFormat", "gif");
    require(!button(overlay, "soundMode")->isEnabled(), "animations disable audio");
    choose(overlay, "recordFormat", "mp4");
    require(button(overlay, "soundMode")->isEnabled() && button(overlay, "soundMode")->text().contains("Microphone"), "video restores selection");
    choose(overlay, "soundMode", "mix");
    CaptureOverlay second(overlay, QRect(0, 0, 360, 640), true);
    second.show();
    second.adoptInteractionState(overlay);
    QTest::qWait(150);
    require(button(second, "soundMode")->text().contains("Mix"), "monitor state sync");
    require(button(second, "soundInput")->toolTip().contains("very-long"), "monitor device state sync");
    second.resize(360, 640);
    QTest::qWait(50);
    for (const auto* name : {"soundMode", "soundOutput", "soundInput"}) {
        auto* b = button(second, name);
        const QRect geometry(b->mapTo(&second, QPoint(0,0)), b->size());
        require(second.rect().contains(geometry), "sound controls within narrow overlay");
    }
    QTest::mouseClick(button(second, "soundOutput"), Qt::LeftButton);
    QTest::qWait(300);
    for (auto* panel : second.findChildren<QWidget*>("inlineSelectPopup"))
        if (panel->isVisible()) require(second.rect().contains(panel->geometry()), "device popup stays inside overlay after refresh");
    std::cout << "Sound UI: modes, animation restore, long names, narrow layout and monitor sync passed\n";
}
