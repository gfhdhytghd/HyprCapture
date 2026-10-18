#include "ui/capture_output.hpp"
#include "ui/hdr_image.hpp"
#include "ui/timing.hpp"
#include <QColorSpace>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImageWriter>
namespace hyprcapture::ui {

bool savePng(const QImage& image, const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly, QFileDevice::ReadOwner | QFileDevice::WriteOwner))
        return false;
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        file.remove();
        return false;
    }

    QImageWriter writer(&file, "PNG");
    writer.setQuality(75);
    QImage tagged = image;
    tagged.setColorSpace(QColorSpace::SRgb);
    if (!writer.write(tagged)) {
        file.remove();
        return false;
    }
    return true;
}

static QString uniqueOutputPath(const QDir& dir, const QString& rawFilename) {
    QFileInfo info(QFileInfo(rawFilename).fileName());
    QString   base = info.completeBaseName();
    QString   suffix = info.suffix();
    if (base.isEmpty())
        base = QStringLiteral("Screenshot");
    if (suffix.isEmpty())
        suffix = QStringLiteral("png");

    for (int i = 0; i < 1000; ++i) {
        const QString filename = i == 0 ? QStringLiteral("%1.%2").arg(base, suffix) : QStringLiteral("%1-%2.%3").arg(base).arg(i).arg(suffix);
        const QString path = dir.filePath(filename);
        if (!QFileInfo::exists(path) && (suffix != "heic" || !QFileInfo::exists(dir.filePath(base + (i == 0 ? QString{} : QStringLiteral("-%1").arg(i)) + ".png"))))
            return path;
    }

    return dir.filePath(QStringLiteral("%1-%2.%3").arg(base).arg(QDateTime::currentMSecsSinceEpoch()).arg(suffix));
}

QString sdrCaptureOutputPath(const hyprcapture::CaptureDefaults& defaults, const QString& outputPath) {
    if (defaults.save && defaults.hdrSaveSdr)
        return QFileInfo(outputPath).absolutePath() + "/" + QFileInfo(outputPath).completeBaseName() + ".png";
    const QString runtime = hyprcapture::ui::runtimeFile("sdr-drag", ".png");
    if (runtime.isEmpty()) return {};
    return QFileInfo(runtime).dir().filePath("sdr-drag-" + QString::number(qHash(outputPath)) + ".png");
}

CaptureOutputResult writeCaptureOutput(const QImage& image,
                                        const QImage& hdrImage,
                                        const hyprcapture::CaptureDefaults& defaults,
                                        const hyprcapture::ui::ClipboardSnapshotData& clipboardSnapshot,
                                        const QString& outputPath,
                                        const QString& restoreClipboardPath) {
    CaptureOutputResult result;
    result.showThumbnail = defaults.showThumbnail;

    if (defaults.clipboard && defaults.showThumbnail && !restoreClipboardPath.isEmpty()) {
        QElapsedTimer timer;
        timer.start();
        if (hyprcapture::ui::saveClipboardSnapshotDataToPath(clipboardSnapshot, restoreClipboardPath))
            result.restoreClipboardPath = restoreClipboardPath;
        traceTiming(QStringLiteral("clipboard_snapshot_save"), timer.elapsed());
    }

    const bool hdr = defaults.screenshotFormat == "heic";
    const bool needSdrFile = hdr && ((defaults.save && defaults.hdrSaveSdr) ||
                                   (defaults.showThumbnail && defaults.hdrDragFormat == "sdr"));
    if (needSdrFile) {
        result.sdrPath = sdrCaptureOutputPath(defaults, outputPath);
        if (!savePng(image, result.sdrPath)) {
            result.saveError = QStringLiteral("Could not save the SDR screenshot copy.");
            return result;
        }
    }

    if (!outputPath.isEmpty()) {
        const bool saved = hdr ? hyprcapture::ui::saveHdrHeic(hdrImage, outputPath, &result.saveError, image)
                               : savePng(image, outputPath);
        if (saved) result.savedPath = outputPath;
        else {
            if (!result.sdrPath.isEmpty()) QFile::remove(result.sdrPath);
            if (result.saveError.isEmpty())
                result.saveError = QStringLiteral("Could not save the image. Check the output directory and try again.");
            return result;
        }
    }

    result.clipboardRequested = defaults.clipboard;
    if (defaults.clipboard) {
        if (hdr && defaults.hdrClipboardFormat == "heic") {
            QString path = result.savedPath;
            const bool temporary = path.isEmpty();
            if (temporary) {
                path = hyprcapture::ui::runtimeFile("clipboard-hdr", ".heic");
                if (!hyprcapture::ui::saveHdrHeic(hdrImage, path, &result.saveError, image)) return result;
            }
            result.clipboardCopied = hyprcapture::ui::copyImageFileToClipboardDetached(path, QStringLiteral("image/heic"), temporary);
            if (temporary && !result.clipboardCopied) QFile::remove(path);
        } else {
            result.clipboardCopied = !hdr && !result.savedPath.isEmpty()
                ? hyprcapture::ui::copyImageFileToClipboardDetached(result.savedPath)
                : hyprcapture::ui::copyImageToClipboardDetached(image);
        }
        if (!result.clipboardCopied && hdr && defaults.hdrClipboardFormat == "heic")
            result.saveError = QStringLiteral("Could not copy HDR HEIC to the clipboard. Check that wl-copy is installed.");
    }

    return result;
}

QString plannedCaptureOutputPath(const hyprcapture::CaptureDefaults& defaults, const QString& windowClass, const QString& windowTitle) {
    if (!defaults.save)
        return {};

    const auto dirPath = hyprcapture::expandUserPath(defaults.saveDir);
    QDir       dir(QString::fromStdString(dirPath.string()));
    if (!dir.exists())
        dir.mkpath(".");
    const QString filename = QString::fromStdString(hyprcapture::makeTimestampedFilename(defaults.filenameTemplate, windowClass.toStdString(), windowTitle.toStdString()));
    return uniqueOutputPath(dir, QFileInfo(filename).completeBaseName() + (defaults.screenshotFormat == "heic" ? ".heic" : ".png"));
}

bool CaptureSave::save() {
    if (!savedPath.isEmpty()) return true;
    error.clear();
    auto outputDefaults = defaults;
    outputDefaults.save = true;
    outputDefaults.clipboard = false;
    outputDefaults.showThumbnail = defaults.showThumbnail;
    const auto path = plannedCaptureOutputPath(outputDefaults, QString::fromStdString(metadata.windowClass), QString::fromStdString(metadata.windowTitle));
    const auto result = writeCaptureOutput(image, hdrImage, outputDefaults, {}, path, {});
    error = result.saveError;
    if (result.savedPath.isEmpty() || !error.isEmpty()) {
        if (error.isEmpty()) error = QStringLiteral("Could not save the image. Check the output directory and try again.");
        return false;
    }
    savedPath = result.savedPath;
    return true;
}
} // namespace hyprcapture::ui
