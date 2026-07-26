#pragma once

#include <QRect>

class QImage;
class QPainter;

namespace hyprcapture::ui {

QRect clippedSelectionGeometry(const QRect& selection, const QRect& clip);
QRect mapLogicalRectToPixels(const QRect& logicalRect, const QRect& logicalBounds, const QRect& pixelBounds);

// Paint using one stable image-to-destination mapping, then reveal only clip.
// Re-cropping and re-scaling the image for every changing clip makes the
// sampling grid move when the scale is fractional.
void paintClippedImage(QPainter& painter, const QRect& clip, const QRect& destination, const QImage& image);
void paintImageLayer(QPainter& painter, const QRect& destination, const QImage& image, const QRect& source);

} // namespace hyprcapture::ui
