#pragma once
#include <QImage>
#include <QString>
namespace hyprcapture::ui {
bool isHdrImage(const QImage& image);
QImage hdrToSdr(const QImage& image, float referenceNits = 203.0F);
QImage sdrToHdr(const QImage& image, float referenceNits = 203.0F);
bool hdrHeicAvailable();
bool saveHdrHeic(const QImage& image, const QString& path, QString* error = nullptr, const QImage& sdrBase = {});
} // namespace hyprcapture::ui
