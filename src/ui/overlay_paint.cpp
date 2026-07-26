#include "ui/overlay_paint.hpp"

#include <QImage>
#include <QPainter>

namespace hyprcapture::ui {

QRect clippedSelectionGeometry(const QRect& selection, const QRect& clip) {
    if (!selection.isValid() || !clip.isValid())
        return selection;
    return selection.intersected(clip);
}

void paintClippedImage(QPainter& painter, const QRect& clip, const QRect& destination, const QImage& image) {
    if (!clip.isValid() || !destination.isValid() || image.isNull())
        return;

    painter.save();
    painter.setClipRect(clip, Qt::IntersectClip);
    painter.drawImage(destination, image);
    painter.restore();
}

} // namespace hyprcapture::ui
