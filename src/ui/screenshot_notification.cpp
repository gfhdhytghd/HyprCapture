#include "ui/screenshot_notification.hpp"

#include "ui/clipboard_utils.hpp"

#include <QFileInfo>
#include <QProcess>
#include <QStringList>

namespace hyprcapture::ui {
namespace {

constexpr int kNotificationTimeoutMs = 5000;

bool launchDetached(const QString& program, const QStringList& arguments) {
    if (program.isEmpty())
        return false;
    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    process.setProcessEnvironment(trustedProcessEnvironment());
    return process.startDetached();
}

bool showHyprlandNotification(const QString& title, const QString& body) {
    QString message = title;
    if (!body.isEmpty()) {
        if (!message.isEmpty())
            message += QLatin1Char('\n');
        message += body;
    }
    return launchDetached(trustedSystemProgram(QStringLiteral("hyprctl")),
                          {QStringLiteral("notify"),
                           QStringLiteral("0"),
                           QString::number(kNotificationTimeoutMs),
                           QStringLiteral("rgb(33cc4d)"),
                           message});
}

bool showSystemNotification(const QString& title, const QString& body, const QString& imagePath) {
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
    return launchDetached(trustedSystemProgram(QStringLiteral("notify-send")), arguments);
}

} // namespace

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

    if (defaults.notificationBackend == NotificationBackend::System && showSystemNotification(title, body, savedPath))
        return true;
    return showHyprlandNotification(title, body);
}

} // namespace hyprcapture::ui
