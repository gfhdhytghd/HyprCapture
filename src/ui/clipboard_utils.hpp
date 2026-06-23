#pragma once

#include <QColor>
#include <QImage>
#include <QList>
#include <QProcessEnvironment>
#include <QString>
#include <QUrl>

class QPixmap;

namespace hyprcapture::ui {

struct ClipboardSnapshotData {
    bool        valid = false;
    bool        empty = true;
    QString     text;
    QString     html;
    QList<QUrl> urls;
    QColor      color;
    QImage      image;
};

QString runtimeFile(const QString& prefix, const QString& suffix);
QString trustedSystemProgram(const QString& name);
QProcessEnvironment trustedProcessEnvironment();
bool isPrivateRuntimePath(const QString& path);
bool isPrivateRuntimeFile(const QString& path, qint64 maxSize);
bool savePrivatePng(const QImage& image, const QString& path);
ClipboardSnapshotData captureClipboardSnapshotData();
QString saveClipboardSnapshotData(const ClipboardSnapshotData& snapshot);
bool saveClipboardSnapshotDataToPath(const ClipboardSnapshotData& snapshot, const QString& path);
QString saveClipboardSnapshot();
bool copyImageToClipboard(const QImage& image);
bool copyImageToClipboardDetached(const QImage& image);
bool copyImageFileToClipboardDetached(const QString& path);
bool copyFileUrlToClipboard(const QString& path);
bool copyPixmapToClipboard(const QPixmap& pixmap);
void discardClipboardSnapshot(const QString& path);
void restoreClipboardSnapshot(const QString& path);

} // namespace hyprcapture::ui
