#include "ui/overlay_paint.hpp"

#include <QImage>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace hyprcapture::ui {
namespace {

enum class Corner { TopLeft, TopRight, BottomRight, BottomLeft };

int colorDistance(const QColor& lhs, const QColor& rhs) {
    return std::max(
        {std::abs(lhs.red() - rhs.red()), std::abs(lhs.green() - rhs.green()), std::abs(lhs.blue() - rhs.blue()), std::abs(lhs.alpha() - rhs.alpha())});
}

QColor logicalCornerPixel(const QImage& image, const QSize& logicalSize, Corner corner, int x, int y) {
    const double scaleX = static_cast<double>(image.width()) / std::max(1, logicalSize.width());
    const double scaleY = static_cast<double>(image.height()) / std::max(1, logicalSize.height());
    int          pixelX = std::clamp(static_cast<int>(std::floor((x + 0.5) * scaleX)), 0, image.width() - 1);
    int          pixelY = std::clamp(static_cast<int>(std::floor((y + 0.5) * scaleY)), 0, image.height() - 1);
    if (corner == Corner::TopRight || corner == Corner::BottomRight)
        pixelX = image.width() - 1 - pixelX;
    if (corner == Corner::BottomLeft || corner == Corner::BottomRight)
        pixelY = image.height() - 1 - pixelY;
    return image.pixelColor(pixelX, pixelY);
}

double predictedCornerRun(double radius, int row) {
    const double y = row + 0.5;
    if (y >= radius)
        return 0.0;
    const double dy = radius - y;
    return radius - std::sqrt(std::max(0.0, radius * radius - dy * dy));
}

qreal detectCornerRadius(const QImage& image, const QSize& logicalSize, Corner corner, int maxLogicalRadius) {
    const int limit = std::min({maxLogicalRadius, logicalSize.width() / 4, logicalSize.height() / 4});
    if (image.isNull() || !logicalSize.isValid() || limit < 6)
        return 0.0;

    constexpr int    maskTolerance = 12;
    constexpr int    contrastThreshold = 20;
    const QColor     seed = logicalCornerPixel(image, logicalSize, corner, 0, 0);
    std::vector<int> runs(static_cast<std::size_t>(limit), 0);
    for (int y = 0; y < limit; ++y) {
        while (runs[static_cast<std::size_t>(y)] < limit &&
               colorDistance(logicalCornerPixel(image, logicalSize, corner, runs[static_cast<std::size_t>(y)], y), seed) <= maskTolerance)
            ++runs[static_cast<std::size_t>(y)];
    }

    if (runs.front() < 2 || runs.front() >= limit)
        return 0.0;

    int monotonicBreaks = 0;
    for (int y = 1; y < limit; ++y) {
        if (runs[static_cast<std::size_t>(y)] > runs[static_cast<std::size_t>(y - 1)] + 1)
            ++monotonicBreaks;
    }
    if (monotonicBreaks > std::max(1, limit / 16))
        return 0.0;

    double bestRadius = 0.0;
    double bestError = std::numeric_limits<double>::max();
    for (int radius = 4; radius <= limit; ++radius) {
        double    error = 0.0;
        const int rows = std::min(limit, radius + 3);
        for (int y = 0; y < rows; ++y)
            error += std::abs(runs[static_cast<std::size_t>(y)] - predictedCornerRun(radius, y));
        error /= rows;
        if (error < bestError) {
            bestError = error;
            bestRadius = radius;
        }
    }

    if (bestRadius <= 0.0 || bestError > 1.5 + bestRadius * 0.08)
        return 0.0;

    int       contrastSamples = 0;
    int       contrasting = 0;
    const int firstRow = std::max(0, static_cast<int>(std::floor(bestRadius * 0.15)));
    const int lastRow = std::min(limit - 1, static_cast<int>(std::ceil(bestRadius * 0.85)));
    for (int y = firstRow; y <= lastRow; ++y) {
        const int x = std::clamp(static_cast<int>(std::ceil(predictedCornerRun(bestRadius, y))) + 2, 0, limit - 1);
        ++contrastSamples;
        if (colorDistance(logicalCornerPixel(image, logicalSize, corner, x, y), seed) >= contrastThreshold)
            ++contrasting;
    }
    if (contrastSamples == 0 || contrasting * 100 < contrastSamples * 60)
        return 0.0;

    return bestRadius;
}

qreal boundedRadius(qreal radius, const QRectF& rect) { return std::clamp(radius, 0.0, std::min(rect.width(), rect.height()) / 2.0); }

} // namespace

QRect clippedSelectionGeometry(const QRect& selection, const QRect& clip) {
    if (!selection.isValid() || !clip.isValid())
        return selection;
    return selection.intersected(clip);
}

QRect selectionOutlineGeometry(const QRect& selection, const QRect& clip, const QRect& output) {
    if (!selection.isValid())
        return {};
    if (clip.isValid() && output.isValid() && !clip.intersects(output))
        return {};
    return selection;
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

ScreenCornerRadii detectScreenCornerRadii(const QImage& image, const QSize& logicalSize, int maxLogicalRadius) {
    return {
        .topLeft = detectCornerRadius(image, logicalSize, Corner::TopLeft, maxLogicalRadius),
        .topRight = detectCornerRadius(image, logicalSize, Corner::TopRight, maxLogicalRadius),
        .bottomRight = detectCornerRadius(image, logicalSize, Corner::BottomRight, maxLogicalRadius),
        .bottomLeft = detectCornerRadius(image, logicalSize, Corner::BottomLeft, maxLogicalRadius),
    };
}

QPainterPath screenPreviewPath(const QRectF& rect, const ScreenCornerRadii& radii) {
    const qreal topLeft = boundedRadius(radii.topLeft, rect);
    const qreal topRight = boundedRadius(radii.topRight, rect);
    const qreal bottomRight = boundedRadius(radii.bottomRight, rect);
    const qreal bottomLeft = boundedRadius(radii.bottomLeft, rect);

    QPainterPath path;
    path.moveTo(rect.left() + topLeft, rect.top());
    path.lineTo(rect.right() - topRight, rect.top());
    if (topRight > 0.0)
        path.arcTo(QRectF(rect.right() - 2.0 * topRight, rect.top(), 2.0 * topRight, 2.0 * topRight), 90.0, -90.0);
    else
        path.lineTo(rect.right(), rect.top());
    path.lineTo(rect.right(), rect.bottom() - bottomRight);
    if (bottomRight > 0.0)
        path.arcTo(QRectF(rect.right() - 2.0 * bottomRight, rect.bottom() - 2.0 * bottomRight, 2.0 * bottomRight, 2.0 * bottomRight), 0.0, -90.0);
    else
        path.lineTo(rect.right(), rect.bottom());
    path.lineTo(rect.left() + bottomLeft, rect.bottom());
    if (bottomLeft > 0.0)
        path.arcTo(QRectF(rect.left(), rect.bottom() - 2.0 * bottomLeft, 2.0 * bottomLeft, 2.0 * bottomLeft), 270.0, -90.0);
    else
        path.lineTo(rect.left(), rect.bottom());
    path.lineTo(rect.left(), rect.top() + topLeft);
    if (topLeft > 0.0)
        path.arcTo(QRectF(rect.left(), rect.top(), 2.0 * topLeft, 2.0 * topLeft), 180.0, -90.0);
    else
        path.lineTo(rect.left(), rect.top());
    path.closeSubpath();
    return path;
}

std::vector<int> orderedWindowSelectionCandidates(const std::vector<WindowSelectionCandidate>& windows,
                                                  const QRect& output,
                                                  std::optional<QPoint> cursor) {
    std::vector<WindowSelectionCandidate> filtered;
    filtered.reserve(windows.size());
    for (const auto& window : windows) {
        if (window.index >= 0 && window.geometry.isValid() && output.isValid() && window.geometry.intersects(output) &&
            (!cursor || window.geometry.contains(*cursor)))
            filtered.push_back(window);
    }

    std::stable_sort(filtered.begin(), filtered.end(), [](const auto& left, const auto& right) {
        if (left.zIndex != right.zIndex)
            return left.zIndex > right.zIndex;
        return left.index > right.index;
    });

    std::vector<int> ordered;
    ordered.reserve(filtered.size());
    for (const auto& window : filtered)
        ordered.push_back(window.index);
    return ordered;
}

int windowSelectionStartPosition(const std::vector<int>& orderedCandidates, int focusedWindowIndex) {
    if (orderedCandidates.empty())
        return -1;

    const auto focused = std::find(orderedCandidates.begin(), orderedCandidates.end(), focusedWindowIndex);
    return focused == orderedCandidates.end() ? 0 : static_cast<int>(std::distance(orderedCandidates.begin(), focused));
}

int stepWindowSelectionPosition(int currentPosition, int steps, int candidateCount) {
    if (candidateCount <= 0)
        return -1;
    const int boundedCurrent = std::clamp(currentPosition, 0, candidateCount - 1);
    return std::clamp(boundedCurrent + steps, 0, candidateCount - 1);
}

int consumeWindowWheelSteps(const QPoint& angleDelta, const QPoint& pixelDelta, bool inverted, WindowWheelStepState& state) {
    const bool useAngle = angleDelta.y() != 0;
    const QPoint delta = useAngle ? angleDelta : pixelDelta;
    if (delta.y() == 0 || std::abs(delta.x()) > std::abs(delta.y()))
        return 0;

    const auto source = useAngle ? WindowWheelDeltaSource::Angle : WindowWheelDeltaSource::Pixel;
    if (state.source != source) {
        state.remainder = 0.0;
        state.source = source;
    }

    double vertical = static_cast<double>(delta.y());
    if (inverted)
        vertical = -vertical;
    if (state.remainder != 0.0 && std::signbit(state.remainder) != std::signbit(vertical))
        state.remainder = 0.0;
    state.remainder += vertical;

    const double threshold = useAngle ? 120.0 : 40.0;
    const int physicalSteps = static_cast<int>(state.remainder / threshold);
    if (physicalSteps == 0)
        return 0;

    state.remainder -= physicalSteps * threshold;
    // Qt reports wheel-up as positive. Candidate positions grow toward the
    // bottom of the window stack, so wheel-down produces positive steps.
    return -physicalSteps;
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
