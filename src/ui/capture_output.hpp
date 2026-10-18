#pragma once
#include "shared/config.hpp"
#include "ui/clipboard_utils.hpp"
#include <QImage>
#include <QString>

namespace hyprcapture::ui {
struct CaptureOutputResult {
    QString savedPath;
    QString saveError;
    QString sdrPath;
    QString restoreClipboardPath;
    bool    showThumbnail = false;
    bool    clipboardRequested = false;
    bool    clipboardCopied = false;
};

QString plannedCaptureOutputPath(const CaptureDefaults&, const QString& windowClass, const QString& windowTitle);
QString sdrCaptureOutputPath(const CaptureDefaults&, const QString& outputPath);
bool savePng(const QImage&, const QString& path);
CaptureOutputResult writeCaptureOutput(const QImage&, const QImage&, const CaptureDefaults&,
                                      const ClipboardSnapshotData&, const QString&, const QString&);

// One shared instance per capture, including thumbnails on multiple outputs.
struct CaptureSave {
    CaptureDefaults defaults;
    QImage image;
    QImage hdrImage;
    FilenameMetadata metadata;
    QString savedPath;
    QString error;
    bool save();
};
} // namespace hyprcapture::ui
