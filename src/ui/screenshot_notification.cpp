#include "ui/screenshot_notification.hpp"

#include "ui/clipboard_utils.hpp"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>
#include <QStringList>

#include <algorithm>

namespace hyprcapture::ui {
namespace {

constexpr int kNotificationTimeoutMs = 5000;

bool launchNotification(const QString& program, const QStringList& arguments, int deliveryTimeoutMs) {
    if (program.isEmpty())
        return false;
    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    process.setProcessEnvironment(trustedProcessEnvironment());
    if (deliveryTimeoutMs <= 0)
        return process.startDetached();

    QElapsedTimer elapsed;
    elapsed.start();
    process.start();
    if (!process.waitForStarted(deliveryTimeoutMs))
        return false;
    const int remainingMs = std::max(1, deliveryTimeoutMs - static_cast<int>(elapsed.elapsed()));
    if (process.state() != QProcess::NotRunning && !process.waitForFinished(remainingMs)) {
        process.kill();
        process.waitForFinished(100);
        return false;
    }
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

bool showHyprlandNotification(const QString& title, const QString& body, int deliveryTimeoutMs) {
    QString message = title;
    if (!body.isEmpty()) {
        if (!message.isEmpty())
            message += QLatin1Char('\n');
        message += body;
    }
    return launchNotification(trustedSystemProgram(QStringLiteral("hyprctl")),
                              {QStringLiteral("notify"),
                               QStringLiteral("0"),
                               QString::number(kNotificationTimeoutMs),
                               QStringLiteral("rgb(33cc4d)"),
                               message}, deliveryTimeoutMs);
}

bool showSystemNotification(const QString& title, const QString& body, const QString& imagePath, int deliveryTimeoutMs) {
    QStringList arguments{
        QStringLiteral("--app-name=HyprCapture"),
        QStringLiteral("--urgency=low"),
        QStringLiteral("--expire-time=%1").arg(kNotificationTimeoutMs),
    };
    if (!imagePath.isEmpty()) {
        arguments << QStringLiteral("--icon=%1").arg(imagePath);
        arguments << QStringLiteral("--hint=string:image-path:%1").arg(imagePath);
    } else {
        arguments << QStringLiteral("--icon=camera-photo");
    }
    arguments << (title.isEmpty() ? QStringLiteral("HyprCapture") : title) << body;
    // notify-send returns after the notification daemon replies to Notify;
    // without --wait it does not wait for the popup to expire or be dismissed.
    return launchNotification(trustedSystemProgram(QStringLiteral("notify-send")), arguments, deliveryTimeoutMs);
}

} // namespace

bool showNotification(NotificationBackend backend, const QString& title, const QString& body, const QString& imagePath, int deliveryTimeoutMs) {
    if (backend == NotificationBackend::System && showSystemNotification(title, body, imagePath, deliveryTimeoutMs))
        return true;
    return showHyprlandNotification(title, body, deliveryTimeoutMs);
}

bool showScreenshotNotification(const CaptureDefaults& defaults,
                                CaptureMode mode,
                                const FilenameMetadata& metadata,
                                const QString& savedPath) {
    if (!defaults.screenshotNotification || savedPath.isEmpty())
        return false;

    const QFileInfo fileInfo(savedPath);
    const auto title = QString::fromStdString(formatScreenshotNotificationTemplate(defaults.notificationTitleTemplate,
                                                                                    mode,
                                                                                    metadata.windowClass,
                                                                                    metadata.windowTitle,
                                                                                    fileInfo.fileName().toStdString(),
                                                                                    savedPath.toStdString()));
    const auto body = QString::fromStdString(formatScreenshotNotificationTemplate(defaults.notificationBodyTemplate,
                                                                                   mode,
                                                                                   metadata.windowClass,
                                                                                   metadata.windowTitle,
                                                                                   fileInfo.fileName().toStdString(),
                                                                                   savedPath.toStdString()));

    return showNotification(defaults.notificationBackend, title, body, savedPath);
}

} // namespace hyprcapture::ui
