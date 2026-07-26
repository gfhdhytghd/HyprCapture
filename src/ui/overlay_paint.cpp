#include "ui/overlay_paint.hpp"

#include <QImage>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace hyprcapture::ui {

QRect clippedSelectionGeometry(const QRect& selection, const QRect& clip) {
    if (!selection.isValid() || !clip.isValid())
        return selection;
    return selection.intersected(clip);
}

QRect mapLogicalRectToPixels(const QRect& logicalRect, const QRect& logicalBounds, const QRect& pixelBounds) {
    const QRect clipped = logicalRect.intersected(logicalBounds);
    if (!clipped.isValid() || !logicalBounds.isValid() || !pixelBounds.isValid())
        return {};

    const double scaleX = static_cast<double>(pixelBounds.width()) / std::max(1, logicalBounds.width());
    const double scaleY = static_cast<double>(pixelBounds.height()) / std::max(1, logicalBounds.height());
    const int x1 = pixelBounds.x() + static_cast<int>(std::floor((clipped.x() - logicalBounds.x()) * scaleX));
    const int y1 = pixelBounds.y() + static_cast<int>(std::floor((clipped.y() - logicalBounds.y()) * scaleY));
    const int x2 = pixelBounds.x() + static_cast<int>(std::ceil((clipped.x() + clipped.width() - logicalBounds.x()) * scaleX));
    const int y2 = pixelBounds.y() + static_cast<int>(std::ceil((clipped.y() + clipped.height() - logicalBounds.y()) * scaleY));
    return QRect(QPoint(x1, y1), QSize(std::max(1, x2 - x1), std::max(1, y2 - y1))).intersected(pixelBounds);
}

void paintClippedImage(QPainter& painter, const QRect& clip, const QRect& destination, const QImage& image) {
    if (!clip.isValid() || !destination.isValid() || image.isNull())
        return;

    painter.save();
    painter.setClipRect(clip, Qt::IntersectClip);
    painter.drawImage(destination, image);
    painter.restore();
}

void paintImageLayer(QPainter& painter, const QRect& destination, const QImage& image, const QRect& source) {
    if (!destination.isValid() || image.isNull() || !source.isValid())
        return;
    painter.drawImage(destination, image, source.intersected(image.rect()));
}

} // namespace hyprcapture::ui
