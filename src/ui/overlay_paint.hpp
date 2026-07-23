#pragma once

#include <QRect>

class QImage;
class QPainter;

namespace hyprcapture::ui {

// Paint using one stable image-to-destination mapping, then reveal only clip.
// Re-cropping and re-scaling the image for every changing clip makes the
// sampling grid move when the scale is fractional.
void paintClippedImage(QPainter& painter, const QRect& clip, const QRect& destination, const QImage& image);

} // namespace hyprcapture::ui
