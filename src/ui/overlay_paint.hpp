#pragma once

#include <QPainterPath>
#include <QRect>

class QImage;
class QPainter;

namespace hyprcapture::ui {

struct ScreenCornerRadii {
    qreal topLeft = 0.0;
    qreal topRight = 0.0;
    qreal bottomRight = 0.0;
    qreal bottomLeft = 0.0;
};

QRect             clippedSelectionGeometry(const QRect& selection, const QRect& clip);
QRect             selectionOutlineGeometry(const QRect& selection, const QRect& clip, const QRect& output);
QRect             mapLogicalRectToPixels(const QRect& logicalRect, const QRect& logicalBounds, const QRect& pixelBounds);
ScreenCornerRadii detectScreenCornerRadii(const QImage& image, const QSize& logicalSize, int maxLogicalRadius = 96);
QPainterPath      screenPreviewPath(const QRectF& rect, const ScreenCornerRadii& radii);

// Paint using one stable image-to-destination mapping, then reveal only clip.
// Re-cropping and re-scaling the image for every changing clip makes the
// sampling grid move when the scale is fractional.
void paintClippedImage(QPainter& painter, const QRect& clip, const QRect& destination, const QImage& image);
void paintImageLayer(QPainter& painter, const QRect& destination, const QImage& image, const QRect& source);

} // namespace hyprcapture::ui
