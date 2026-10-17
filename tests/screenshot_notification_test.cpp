#include "ui/screenshot_notification.hpp"
#include "ui/clipboard_utils.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QThread>
#include <cassert>

namespace {
QString directory;
QString behavior;
bool unavailable = false;

QByteArray contents(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

int notificationWorker(const QStringList& arguments) {
    const bool system = arguments.contains(QStringLiteral("--app-name=HyprCapture"));
    const QString mode = qEnvironmentVariable("HC_NOTIFICATION_TEST_BEHAVIOR");
    if (mode == "timeout" || (system && mode == "system-timeout")) QThread::sleep(5);
    if (mode == "failure" || (system && mode == "system-failure")) return 1;
    // Simulate a daemon which acknowledges Notify after a delayed reply.
    if (system) QThread::msleep(150);
    QFile acknowledgement(qEnvironmentVariable("HC_NOTIFICATION_TEST_DIRECTORY") + (system ? "/system" : "/hyprland"));
    if (!acknowledgement.open(QIODevice::WriteOnly)) return 1;
    acknowledgement.write(arguments.join('\n').toUtf8());
    return acknowledgement.flush() ? 0 : 1;
}
} // namespace

// Isolate the notification commands from the actual desktop. The production
// dispatcher runs unchanged against this executable's notification worker.
namespace hyprcapture::ui {
QString trustedSystemProgram(const QString&) { return unavailable ? QString{} : QCoreApplication::applicationFilePath(); }
QProcessEnvironment trustedProcessEnvironment() {
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("HC_NOTIFICATION_TEST_CHILD", "1");
    environment.insert("HC_NOTIFICATION_TEST_DIRECTORY", directory);
    environment.insert("HC_NOTIFICATION_TEST_BEHAVIOR", behavior);
    return environment;
}
} // namespace hyprcapture::ui

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    if (qEnvironmentVariableIsSet("HC_NOTIFICATION_TEST_CHILD")) return notificationWorker(application.arguments());
    QTemporaryDir temporary;
    assert(temporary.isValid());
    directory = temporary.path();
    using namespace hyprcapture;
    using namespace hyprcapture::ui;
    const QString recovery = "Recording retained at: /recordings/source & > recording.mkv";
    assert(showNotification(NotificationBackend::System, "APNG conversion failed", recovery, {}, 1000));
    assert(contents(directory + "/system").contains(recovery.toUtf8()));
    assert(!QFile::exists(directory + "/hyprland"));
    QFile::remove(directory + "/system");

    assert(showNotification(NotificationBackend::Hyprland, "failure", recovery, {}, 1000));
    assert(contents(directory + "/hyprland").contains(recovery.toUtf8()));
    assert(!QFile::exists(directory + "/system"));
    QFile::remove(directory + "/hyprland");

    behavior = "system-failure";
    assert(showNotification(NotificationBackend::System, "failure", recovery, {}, 1000));
    assert(contents(directory + "/hyprland").contains(recovery.toUtf8()));
    QFile::remove(directory + "/hyprland");

    behavior = "system-timeout";
    QElapsedTimer elapsed;
    elapsed.start();
    assert(showNotification(NotificationBackend::System, "timeout", recovery, {}, 100));
    assert(elapsed.elapsed() < 1500);
    assert(!QFile::exists(directory + "/system"));
    assert(QFile::exists(directory + "/hyprland"));
    QFile::remove(directory + "/hyprland");

    behavior = "timeout";
    elapsed.restart();
    assert(!showNotification(NotificationBackend::System, "timeout", recovery, {}, 100));
    assert(elapsed.elapsed() < 1500);
    behavior = "failure";
    assert(!showNotification(NotificationBackend::System, "failure", recovery, {}, 1000));
    unavailable = true;
    assert(!showNotification(NotificationBackend::System, "unavailable", recovery, {}, 1000));
    unavailable = false;

    // Screenshot notifications use the same dispatcher and retain their gate
    // and template behavior. Their default delivery mode remains detached.
    behavior.clear();
    CaptureDefaults defaults;
    defaults.screenshotNotification = false;
    assert(!showScreenshotNotification(defaults, CaptureMode::Region, {}, "/recordings/source.png"));
    assert(!QFile::exists(directory + "/system"));
    defaults.screenshotNotification = true;
    assert(showScreenshotNotification(defaults, CaptureMode::Region, {}, "/recordings/source.png"));
    elapsed.restart();
    while (!contents(directory + "/system").contains("source.png") && elapsed.elapsed() < 2000) QThread::msleep(5);
    assert(contents(directory + "/system").contains("source.png"));
}
