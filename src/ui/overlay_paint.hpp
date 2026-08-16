#pragma once

#include <QPainterPath>
#include <QRect>

#include <optional>
#include <vector>

class QImage;
class QPainter;

namespace hyprcapture::ui {

struct ScreenCornerRadii {
    qreal topLeft = 0.0;
    qreal topRight = 0.0;
    qreal bottomRight = 0.0;
    qreal bottomLeft = 0.0;
};

struct WindowSelectionCandidate {
    int   index = -1;
    QRect geometry;
    int   zIndex = 0;
};

enum class WindowWheelDeltaSource { None, Angle, Pixel };

struct WindowWheelStepState {
    double                 remainder = 0.0;
    WindowWheelDeltaSource source = WindowWheelDeltaSource::None;
};

QRect             clippedSelectionGeometry(const QRect& selection, const QRect& clip);
QRect             selectionOutlineGeometry(const QRect& selection, const QRect& clip, const QRect& output);
QRect             mapLogicalRectToPixels(const QRect& logicalRect, const QRect& logicalBounds, const QRect& pixelBounds);
ScreenCornerRadii detectScreenCornerRadii(const QImage& image, const QSize& logicalSize, int maxLogicalRadius = 96);
QPainterPath      screenPreviewPath(const QRectF& rect, const ScreenCornerRadii& radii);
std::vector<int>  orderedWindowSelectionCandidates(const std::vector<WindowSelectionCandidate>& windows,
                                                   const QRect& output,
                                                   std::optional<QPoint> cursor = std::nullopt);
int               windowSelectionStartPosition(const std::vector<int>& orderedCandidates, int focusedWindowIndex);
int               stepWindowSelectionPosition(int currentPosition, int steps, int candidateCount);
int               consumeWindowWheelSteps(const QPoint& angleDelta, const QPoint& pixelDelta, bool inverted, WindowWheelStepState& state);

// Paint using one stable image-to-destination mapping, then reveal only clip.
// Re-cropping and re-scaling the image for every changing clip makes the
// sampling grid move when the scale is fractional.
void paintClippedImage(QPainter& painter, const QRect& clip, const QRect& destination, const QImage& image);
void paintImageLayer(QPainter& painter, const QRect& destination, const QImage& image, const QRect& source);

} // namespace hyprcapture::ui
