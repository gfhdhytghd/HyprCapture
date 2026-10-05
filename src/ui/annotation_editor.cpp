#include "ui/annotation_editor.hpp"
#include "ui/material_icon.hpp"

#include <QApplication>
#include <QButtonGroup>
#include <QColorDialog>
#include <QEvent>
#include <QFrame>
#include <QHideEvent>
#include <QLinearGradient>
#include <QMenu>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QSettings>
#include <QStandardPaths>
#include <QShortcut>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace {
enum class Tool { Select, Rectangle, Ellipse, Arrow, Line, Pen, Highlighter, Text, Number, Mosaic, Spotlight };
enum class Variant { Outline, Filled, Rounded, RoundedFilled, Straight, Curved, DoubleHeaded, DoubleCurved };

struct Annotation {
    Tool tool = Tool::Rectangle;
    Variant variant = Variant::Outline;
    QColor color = QColor("#ff5252");
    qreal width = 4.0;
    QVector<QPointF> points;
    QString text;
    int number = 1;
    mutable QImage mosaicCache;
    mutable QRect mosaicBounds;
    mutable int mosaicBlock = 0;
    mutable qint64 mosaicSource = 0;
};

struct Edit {
    enum class Kind { Add, Remove, Replace, Clear } kind = Kind::Add;
    int index = 0;
    Annotation before;
    Annotation after;
    std::vector<Annotation> all;
};

QRectF pointBounds(const Annotation& annotation) {
    if (annotation.points.isEmpty())
        return {};
    QPointF minimum = annotation.points.front(), maximum = minimum;
    for (const auto& point : annotation.points) {
        minimum.setX(std::min(minimum.x(), point.x()));
        minimum.setY(std::min(minimum.y(), point.y()));
        maximum.setX(std::max(maximum.x(), point.x()));
        maximum.setY(std::max(maximum.y(), point.y()));
    }
    return {minimum, maximum};
}

QFont annotationFont(qreal width) {
    QFont font = QApplication::font();
    font.setPixelSize(qRound(width * 4.0 + 12.0));
    font.setWeight(QFont::DemiBold);
    return font;
}

QRectF textBounds(const Annotation& annotation) {
    if (annotation.points.isEmpty())
        return {};
    const QFontMetricsF metrics(annotationFont(annotation.width));
    const auto lines = annotation.text.split('\n');
    qreal width = 1.0;
    for (const auto& line : lines)
        width = std::max(width, metrics.horizontalAdvance(line));
    return {annotation.points.front(), QSizeF(width + 4.0, metrics.lineSpacing() * lines.size())};
}

QPainterPath annotationPath(const Annotation& annotation) {
    QPainterPath path;
    if (annotation.points.isEmpty())
        return path;
    const QRectF bounds = pointBounds(annotation).normalized();
    switch (annotation.tool) {
        case Tool::Rectangle:
        case Tool::Mosaic:
            if (annotation.variant == Variant::Rounded || annotation.variant == Variant::RoundedFilled)
                path.addRoundedRect(bounds, std::min(18.0, bounds.width() * 0.18), std::min(18.0, bounds.height() * 0.18));
            else
                path.addRect(bounds);
            break;
        case Tool::Ellipse:
        case Tool::Spotlight:
            path.addEllipse(bounds);
            break;
        case Tool::Number: {
            const qreal radius = annotation.width * 2.0 + 12.0;
            path.addEllipse(annotation.points.front(), radius, radius);
            break;
        }
        case Tool::Text:
            path.addRect(textBounds(annotation));
            break;
        default:
            path.moveTo(annotation.points.front());
            if (annotation.tool == Tool::Arrow && (annotation.variant == Variant::Curved || annotation.variant == Variant::DoubleCurved) && annotation.points.size() >= 2) {
                const QPointF start = annotation.points.front(), end = annotation.points.back();
                const QPointF difference = end - start;
                path.quadTo((start + end) / 2.0 + QPointF(-difference.y(), difference.x()) * 0.25, end);
            } else {
                for (int i = 1; i < annotation.points.size(); ++i)
                    path.lineTo(annotation.points[i]);
            }
            break;
    }
    return path;
}

void drawArrowHead(QPainter& painter, const QPointF& tip, const QPointF& direction, qreal width) {
    const qreal length = std::hypot(direction.x(), direction.y());
    if (length < 0.001)
        return;
    const QPointF unit = direction / length;
    const QPointF normal(-unit.y(), unit.x());
    const qreal size = width * 2.8 + 9.0;
    QPolygonF triangle;
    triangle << tip << tip - unit * size + normal * size * 0.45 << tip - unit * size - normal * size * 0.45;
    painter.save();
    painter.setBrush(painter.pen().color());
    painter.setPen(Qt::NoPen);
    painter.drawPolygon(triangle);
    painter.restore();
}

// Redaction tiles are opaque, including when the source screenshot has alpha.
// Average each entire source block rather than sampling a single source pixel.
void drawMosaic(QPainter& painter, const QImage& source, const Annotation& annotation) {
    const QRect bounds = pointBounds(annotation).toAlignedRect().intersected(source.rect());
    if (bounds.isEmpty())
        return;
    const int block = std::clamp(qRound(annotation.width * 3.0), 8, 72);
    if (annotation.mosaicCache.isNull() || annotation.mosaicBounds != bounds || annotation.mosaicBlock != block || annotation.mosaicSource != source.cacheKey()) {
        const QSize grid((bounds.width() + block - 1) / block, (bounds.height() + block - 1) / block);
        QImage cache(grid, QImage::Format_RGB32);
        if (cache.isNull()) {
            painter.fillRect(bounds, QColor(36, 36, 36));
            return;
        }
        for (int row = 0; row < grid.height(); ++row) {
            const int y = bounds.top() + row * block;
            for (int column = 0; column < grid.width(); ++column) {
                const int x = bounds.left() + column * block;
                const QRect tile(x, y, std::min(block, bounds.right() - x + 1), std::min(block, bounds.bottom() - y + 1));
                quint64 red = 0, green = 0, blue = 0;
                for (int sy = tile.top(); sy <= tile.bottom(); ++sy) {
                    for (int sx = tile.left(); sx <= tile.right(); ++sx) {
                        const QColor pixel = source.pixelColor(sx, sy);
                        const unsigned alpha = static_cast<unsigned>(pixel.alpha());
                        red += (static_cast<unsigned>(pixel.red()) * alpha + 36U * (255U - alpha)) / 255U;
                        green += (static_cast<unsigned>(pixel.green()) * alpha + 36U * (255U - alpha)) / 255U;
                        blue += (static_cast<unsigned>(pixel.blue()) * alpha + 36U * (255U - alpha)) / 255U;
                    }
                }
                const quint64 count = static_cast<quint64>(tile.width()) * tile.height();
                cache.setPixel(column, row, qRgb(static_cast<int>(red / count), static_cast<int>(green / count), static_cast<int>(blue / count)));
            }
        }
        annotation.mosaicCache = std::move(cache);
        annotation.mosaicBounds = bounds;
        annotation.mosaicBlock = block;
        annotation.mosaicSource = source.cacheKey();
    }
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    // Keep final partial tiles aligned exactly with the covered source pixels.
    painter.setClipRect(bounds, Qt::IntersectClip);
    painter.drawImage(QRect(bounds.topLeft(), QSize(annotation.mosaicCache.width() * block, annotation.mosaicCache.height() * block)), annotation.mosaicCache);
    painter.restore();
}

void drawAnnotation(QPainter& painter, const QImage& source, const Annotation& annotation) {
    if (annotation.points.isEmpty())
        return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(annotation.color, annotation.width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    const QPainterPath path = annotationPath(annotation);
    switch (annotation.tool) {
        case Tool::Mosaic:
            drawMosaic(painter, source, annotation);
            break;
        case Tool::Spotlight: {
            QPainterPath outside;
            outside.addRect(source.rect());
            painter.fillPath(outside.subtracted(path), QColor(0, 0, 0, 150));
            break;
        }
        case Tool::Text: {
            painter.setFont(annotationFont(annotation.width));
            const QFontMetricsF metrics(painter.font());
            qreal y = annotation.points.front().y() + metrics.ascent();
            for (const auto& line : annotation.text.split('\n')) {
                painter.drawText(QPointF(annotation.points.front().x(), y), line);
                y += metrics.lineSpacing();
            }
            break;
        }
        case Tool::Number: {
            painter.fillPath(path, annotation.color);
            painter.setPen(annotation.color.lightness() > 155 ? Qt::black : Qt::white);
            QFont font = annotationFont(annotation.width);
            font.setPixelSize(qRound(annotation.width * 2.0 + 15.0));
            painter.setFont(font);
            painter.drawText(path.boundingRect(), Qt::AlignCenter, QString::number(annotation.number));
            break;
        }
        case Tool::Highlighter: {
            QColor color = annotation.color;
            color.setAlpha(85);
            painter.setPen(QPen(color, annotation.width * 5.0, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
            painter.drawPath(path);
            break;
        }
        case Tool::Rectangle:
        case Tool::Ellipse:
            if (annotation.variant == Variant::Filled || annotation.variant == Variant::RoundedFilled)
                painter.setBrush(annotation.color);
            painter.drawPath(path);
            break;
        case Tool::Arrow:
            painter.drawPath(path);
            if (annotation.points.size() >= 2) {
                const QPointF start = annotation.points.front(), end = annotation.points.back();
                const QPointF difference = end - start;
                const QPointF control = (start + end) / 2.0 + QPointF(-difference.y(), difference.x()) * 0.25;
                const bool curved = annotation.variant == Variant::Curved || annotation.variant == Variant::DoubleCurved;
                drawArrowHead(painter, end, curved ? end - control : difference, annotation.width);
                if (annotation.variant == Variant::DoubleHeaded || annotation.variant == Variant::DoubleCurved)
                    drawArrowHead(painter, start, curved ? start - control : -difference, annotation.width);
            }
            break;
        default:
            if (annotation.points.size() == 1)
                painter.drawPoint(annotation.points.front());
            else
                painter.drawPath(path);
            break;
    }
    painter.restore();
}

// Embedded in the layer-shell surface: never creates a separate modal window
// or a nested event loop that can end up behind the capture overlay on Wayland.
class AnnotationTextInput final : public QPlainTextEdit {
  public:
    using QPlainTextEdit::QPlainTextEdit;
    std::function<void(bool)> finished;
  protected:
    bool event(QEvent* event) override {
        if (event->type() == QEvent::ShortcutOverride) { event->accept(); return true; }
        return QPlainTextEdit::event(event);
    }
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Escape) {
            if (finished) finished(false);
            event->accept();
        } else if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
                   event->modifiers().testFlag(Qt::ControlModifier)) {
            if (finished) finished(true);
            event->accept();
        } else QPlainTextEdit::keyPressEvent(event);
    }
};

class AnnotationCanvas final : public QWidget {
  public:
    explicit AnnotationCanvas(QWidget* parent) : QWidget(parent) {
        setObjectName("annotationCanvas");
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_TranslucentBackground);
        setCursor(Qt::CrossCursor);
    }

    QImage base;
    Tool tool = Tool::Rectangle;
    Variant variant = Variant::Outline;
    QColor color = QColor("#ff5252");
    qreal strokeWidth = 4.0;
    std::function<void()> changed;
    std::function<void()> viewChanged;
    std::function<void(const QRect&)> captureRectChanged;
    QRect resizeBounds;

    void replaceCapture(const QImage& image, const QRect& target) {
        if (base.isNull() || m_displayRect.isEmpty() || image.isNull()) return;
        const QRect oldView = displayRect();
        const QRect previous = m_displayRect;
        const qreal sx = qreal(image.width()) / target.width();
        const qreal sy = qreal(image.height()) / target.height();
        const qreal ax = sx * previous.width() / base.width();
        const qreal ay = sy * previous.height() / base.height();
        const QPointF offset((previous.x() - target.x()) * sx, (previous.y() - target.y()) * sy);
        const auto remap = [&](Annotation& item) {
            for (auto& p : item.points) p = QPointF(p.x() * ax, p.y() * ay) + offset;
            item.width *= std::sqrt(ax * ay);
            item.mosaicCache = {};
            item.mosaicSource = 0;
        };
        for (auto& item : m_annotations) remap(item);
        for (auto& edit : m_history) {
            remap(edit.before); remap(edit.after);
            for (auto& item : edit.all) remap(item);
        }
        // Keep the desktop-to-view transform stable, including a zoomed/panned view.
        m_zoom = qreal(oldView.width()) / previous.width();
        const QPointF desired = QPointF(oldView.topLeft()) + QPointF(target.topLeft() - previous.topLeft()) * m_zoom;
        m_displayRect = target;
        m_fitToViewport = false;
        m_pan = desired - QRectF(target).center() + QPointF(target.width(), target.height()) * (m_zoom / 2);
        base = image;
        base.setDevicePixelRatio(1.0);
        notify();
        if (viewChanged) viewChanged();
    }

    std::function<void()> pressed;
    std::function<void(const QString&, const QPoint&, std::function<void(std::optional<QString>)>)> requestText;
    std::function<void()> copy;
    std::function<void()> save;
    std::function<void()> pin;
    std::function<void()> reselect;

    void setImage(const QImage& image, bool preserve) {
        const bool sameSize = image.size() == base.size();
        base = image;
        base.setDevicePixelRatio(1.0);
        m_draft.reset();
        m_dragBefore.reset();
        if (!preserve || !sameSize) {
            m_annotations.clear();
            m_history.clear();
            m_historyPosition = 0;
            m_selected = -1;
            fit();
        }
        notify();
    }

    QImage result() const {
        if (base.isNull())
            return {};
        QImage output = base.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        if (output.isNull())
            return {};
        QPainter painter(&output);
        for (const auto& annotation : m_annotations)
            drawAnnotation(painter, base, annotation);
        return output;
    }

    QRect displayRect() const { return imageTransform().mapRect(QRectF(base.rect())).toAlignedRect(); }
    void setDisplayRect(const QRect& rect) {
        if (m_displayRect == rect)
            return;
        m_displayRect = rect;
        m_fitToViewport = false;
        fit();
    }
    void fit() { m_zoom = 1.0; m_pan = {}; update(); if (viewChanged) viewChanged(); }
    void fitViewport() { m_fitToViewport = true; fit(); }
    void setViewportBottomInset(int inset) {
        if (m_fitBottomInset == inset)
            return;
        m_fitBottomInset = inset;
        if (m_fitToViewport || m_displayRect.isEmpty()) {
            update();
            if (viewChanged) viewChanged();
        }
    }
    void finishEditing() { finishDraft(); }
    bool canUndo() const { return m_historyPosition > 0; }
    bool canRedo() const { return m_historyPosition < static_cast<int>(m_history.size()); }
    bool hasAnnotations() const { return !m_annotations.empty(); }
    void selectTool(Tool selectedTool) {
        finishDraft();
        tool = selectedTool;
        m_selected = -1;
        setCursor(tool == Tool::Select ? Qt::ArrowCursor : Qt::CrossCursor);
        update();
    }
    void undo() {
        cancelGesture();
        if (!canUndo())
            return;
        const Edit& edit = m_history[--m_historyPosition];
        switch (edit.kind) {
            case Edit::Kind::Add: m_annotations.erase(m_annotations.begin() + edit.index); break;
            case Edit::Kind::Remove: m_annotations.insert(m_annotations.begin() + edit.index, edit.before); break;
            case Edit::Kind::Replace: m_annotations[edit.index] = edit.before; break;
            case Edit::Kind::Clear: m_annotations = edit.all; break;
        }
        m_selected = -1;
        notify();
    }
    void redo() {
        cancelGesture();
        if (!canRedo())
            return;
        const Edit& edit = m_history[m_historyPosition++];
        switch (edit.kind) {
            case Edit::Kind::Add: m_annotations.insert(m_annotations.begin() + edit.index, edit.after); break;
            case Edit::Kind::Remove: m_annotations.erase(m_annotations.begin() + edit.index); break;
            case Edit::Kind::Replace: m_annotations[edit.index] = edit.after; break;
            case Edit::Kind::Clear: m_annotations.clear(); break;
        }
        m_selected = -1;
        notify();
    }
    void clearAnnotations() {
        cancelGesture();
        if (!m_annotations.empty()) {
            Edit edit;
            edit.kind = Edit::Kind::Clear;
            edit.all = m_annotations;
            commit(std::move(edit));
        }
        m_selected = -1;
        notify();
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        if (base.isNull())
            return;
        QPainter painter(this);
        const QTransform transform = imageTransform();
        const QRectF rect = transform.mapRect(QRectF(base.rect()));
        painter.save();
        painter.setClipRect(rect);
        // Checkerboard is only a display aid; result() never paints it.
        constexpr int check = 12;
        const QRect visible = rect.toAlignedRect().intersected(this->rect());
        const int startX = visible.left() - (visible.left() % check);
        const int startY = visible.top() - (visible.top() % check);
        for (int y = startY; y <= visible.bottom(); y += check)
            for (int x = startX; x <= visible.right(); x += check)
                painter.fillRect(QRect(x, y, check, check), ((x / check + y / check) & 1) ? QColor(72, 77, 86) : QColor(94, 100, 111));
        painter.setTransform(transform);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(QPoint(0, 0), base);
        for (const auto& annotation : m_annotations)
            drawAnnotation(painter, base, annotation);
        if (m_draft)
            drawAnnotation(painter, base, *m_draft);
        if (m_selected >= 0 && m_selected < static_cast<int>(m_annotations.size())) {
            const QRectF selectedBounds = annotationPath(m_annotations[m_selected]).boundingRect();
            painter.setPen(QPen(QColor("#69bfff"), 1.5 / transform.m11(), Qt::DashLine));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(selectedBounds.adjusted(-5.0 / transform.m11(), -5.0 / transform.m11(), 5.0 / transform.m11(), 5.0 / transform.m11()));
        }
        painter.restore();
        if (!resizeBounds.isEmpty()) {
            const QRectF frame = QRectF(displayRect()).adjusted(0.5, 0.5, -0.5, -0.5);
            painter.setPen(QPen(QColor("#69bfff"), 1));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(frame);
            painter.setBrush(QColor("#ffffff"));
            for (const QPointF p : {frame.topLeft(), frame.topRight(), frame.bottomLeft(), frame.bottomRight(),
                 QPointF(frame.center().x(), frame.top()), QPointF(frame.center().x(), frame.bottom()),
                 QPointF(frame.left(), frame.center().y()), QPointF(frame.right(), frame.center().y())})
                painter.drawRoundedRect(QRectF(p - QPointF(3, 3), QSizeF(6, 6)), 1.5, 1.5);
        }
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (pressed) pressed();
        if (event->button() == Qt::LeftButton && !m_spaceDown && (m_resizeEdges = resizeEdgesAt(event->position()))) {
            finishEditing();
            m_resizeStart = m_displayRect;
            m_resizeView = displayRect();
            m_resizePointer = event->position();
            event->accept();
            return;
        }
        if (base.isNull() || !displayRect().contains(event->position().toPoint())) {
            event->ignore();
            return;
        }
        if (event->button() == Qt::MiddleButton || (event->button() == Qt::LeftButton && m_spaceDown)) {
            m_panning = true;
            m_pointerOrigin = event->position();
            m_panBefore = m_pan;
            setCursor(Qt::ClosedHandCursor);
            event->accept();
            return;
        }
        if (event->button() != Qt::LeftButton) {
            event->ignore();
            return;
        }
        setFocus(Qt::MouseFocusReason);
        const QPointF point = imagePoint(event->position());
        if (tool == Tool::Select) {
            m_selected = annotationAt(point);
            if (m_selected >= 0) {
                m_dragBefore = m_annotations[m_selected];
                m_pointerOrigin = point;
            }
            update();
            event->accept();
            return;
        }
        Annotation annotation;
        annotation.tool = tool;
        annotation.variant = variant;
        annotation.color = color;
        annotation.width = strokeWidth;
        annotation.points.push_back(point);
        if (tool == Tool::Text) {
            if (requestText) requestText({}, event->position().toPoint(), [this, annotation](std::optional<QString> text) mutable {
                if (!text || text->trimmed().isEmpty()) return;
                annotation.text = *text;
                add(annotation);
            });
            event->accept();
            return;
        }
        if (tool == Tool::Number) {
            for (const auto& item : m_annotations)
                if (item.tool == Tool::Number)
                    annotation.number = std::max(annotation.number, item.number + 1);
            add(annotation);
            event->accept();
            return;
        }
        if (tool != Tool::Pen && tool != Tool::Highlighter)
            annotation.points.push_back(point);
        m_draft = std::move(annotation);
        update();
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (m_resizeEdges) {
            updateResize(event->position());
            event->accept();
            return;
        }
        if (!m_panning && !m_draft && !m_dragBefore) {
            const int edges = resizeEdgesAt(event->position());
            if (edges) { setCursor(resizeCursor(edges)); event->accept(); return; }
            setCursor(m_spaceDown ? Qt::OpenHandCursor : tool == Tool::Select ? Qt::ArrowCursor : Qt::CrossCursor);
        }
        if (m_panning) {
            m_pan = m_panBefore + event->position() - m_pointerOrigin;
            update();
            if (viewChanged) viewChanged();
            event->accept();
            return;
        }
        const QPointF point = imagePoint(event->position());
        if (m_dragBefore && m_selected >= 0) {
            m_annotations[m_selected] = *m_dragBefore;
            for (auto& item : m_annotations[m_selected].points)
                item += point - m_pointerOrigin;
            update();
            event->accept();
            return;
        }
        if (m_draft) {
            if (m_draft->tool == Tool::Pen || m_draft->tool == Tool::Highlighter) {
                if (QLineF(m_draft->points.back(), point).length() >= 0.7 && m_draft->points.size() < 20000)
                    m_draft->points.push_back(point);
            } else
                m_draft->points.back() = point;
            update();
            event->accept();
            return;
        }
        if (tool == Tool::Select && displayRect().contains(event->position().toPoint()))
            setCursor(annotationAt(point) >= 0 ? Qt::SizeAllCursor : Qt::ArrowCursor);
        event->ignore();
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (m_resizeEdges && event->button() == Qt::LeftButton) {
            updateResize(event->position());
            m_resizeEdges = 0;
            event->accept();
            return;
        }
        if (m_panning) {
            m_panning = false;
            setCursor(m_spaceDown ? Qt::OpenHandCursor : tool == Tool::Select ? Qt::ArrowCursor : Qt::CrossCursor);
            event->accept();
            return;
        }
        if (event->button() != Qt::LeftButton) {
            event->ignore();
            return;
        }
        if (m_dragBefore && m_selected >= 0) {
            const Annotation after = m_annotations[m_selected];
            const Annotation before = *m_dragBefore;
            m_dragBefore.reset();
            if (after.points != before.points)
                commit({Edit::Kind::Replace, m_selected, before, after});
            notify();
            event->accept();
            return;
        }
        if (m_draft) {
            finishDraft();
            event->accept();
            return;
        }
        event->ignore();
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (tool == Tool::Select && event->button() == Qt::LeftButton) {
            const int index = annotationAt(imagePoint(event->position()));
            if (index >= 0 && m_annotations[index].tool == Tool::Text && requestText) {
                const Annotation before = m_annotations[index];
                m_dragBefore.reset();
                requestText(before.text, event->position().toPoint(), [this, before, index](std::optional<QString> text) {
                    if (text && !text->trimmed().isEmpty() && *text != before.text && index < static_cast<int>(m_annotations.size())) {
                        Annotation after = before;
                        after.text = *text;
                        commit({Edit::Kind::Replace, index, before, after});
                        notify();
                    }
                });
                event->accept();
                return;
            }
        }
        event->ignore();
    }

    void wheelEvent(QWheelEvent* event) override {
        if (base.isNull() || !displayRect().contains(event->position().toPoint())) {
            event->ignore();
            return;
        }
        const QPointF anchor = imageTransform().inverted().map(event->position());
        m_zoom = std::clamp(m_zoom * std::pow(1.0015, event->angleDelta().y()), 0.2, 12.0);
        const QPointF mapped = imageTransform().map(anchor);
        m_pan += event->position() - mapped;
        update();
        if (viewChanged) viewChanged();
        event->accept();
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
            m_spaceDown = true;
            setCursor(Qt::OpenHandCursor);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
            if (m_selected >= 0) {
                commit({Edit::Kind::Remove, m_selected, m_annotations[m_selected], {}});
                m_selected = -1;
                notify();
            }
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Escape && (m_draft || m_dragBefore || m_selected >= 0)) {
            cancelGesture();
            m_selected = -1;
            update();
            event->accept();
            return;
        }
        QWidget::keyPressEvent(event);
    }

    void keyReleaseEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Space) {
            m_spaceDown = false;
            if (!m_panning)
                setCursor(tool == Tool::Select ? Qt::ArrowCursor : Qt::CrossCursor);
            event->accept();
            return;
        }
        QWidget::keyReleaseEvent(event);
    }

    void focusOutEvent(QFocusEvent* event) override {
        m_spaceDown = false;
        m_panning = false;
        QWidget::focusOutEvent(event);
    }

  private:
    int m_resizeEdges = 0;
    QRect m_resizeStart;
    QRect m_resizeView;
    QPointF m_resizePointer;
    int resizeEdgesAt(QPointF point) const {
        if (resizeBounds.isEmpty() || base.isNull()) return 0;
        const QRectF r(displayRect());
        if (!r.adjusted(-6, -6, 6, 6).contains(point)) return 0;
        int edges = 0;
        if (std::abs(point.x() - r.left()) <= 6) edges |= 1;
        else if (std::abs(point.x() - r.right()) <= 6) edges |= 2;
        if (std::abs(point.y() - r.top()) <= 6) edges |= 4;
        else if (std::abs(point.y() - r.bottom()) <= 6) edges |= 8;
        return edges;
    }
    static Qt::CursorShape resizeCursor(int edges) {
        if (edges == 5 || edges == 10) return Qt::SizeFDiagCursor;
        if (edges == 6 || edges == 9) return Qt::SizeBDiagCursor;
        return edges & 3 ? Qt::SizeHorCursor : Qt::SizeVerCursor;
    }
    void updateResize(QPointF pointer) {
        const QPointF delta = pointer - m_resizePointer;
        const int dx = qRound(delta.x() * m_resizeStart.width() / m_resizeView.width());
        const int dy = qRound(delta.y() * m_resizeStart.height() / m_resizeView.height());
        QRect target = m_resizeStart;
        if (m_resizeEdges & 1) target.setLeft(std::clamp(target.left() + dx, resizeBounds.left(), target.right() - 1));
        if (m_resizeEdges & 2) target.setRight(std::clamp(target.right() + dx, target.left() + 1, resizeBounds.right()));
        if (m_resizeEdges & 4) target.setTop(std::clamp(target.top() + dy, resizeBounds.top(), target.bottom() - 1));
        if (m_resizeEdges & 8) target.setBottom(std::clamp(target.bottom() + dy, target.top() + 1, resizeBounds.bottom()));
        if (target != m_displayRect && captureRectChanged) captureRectChanged(target);
    }
    std::vector<Annotation> m_annotations;
    std::vector<Edit> m_history;
    int m_historyPosition = 0;
    int m_selected = -1;
    std::optional<Annotation> m_draft;
    std::optional<Annotation> m_dragBefore;
    QPointF m_pointerOrigin;
    QPointF m_panBefore;
    QPointF m_pan;
    QRect m_displayRect;
    qreal m_zoom = 1.0;
    int m_fitBottomInset = 74;
    bool m_fitToViewport = false;
    bool m_spaceDown = false;
    bool m_panning = false;

    QTransform imageTransform() const {
        if (base.isNull())
            return {};
        const QRectF viewport = m_fitToViewport || m_displayRect.isEmpty() ? QRectF(rect().adjusted(12, 12, -12, -m_fitBottomInset)) : QRectF(m_displayRect);
        const qreal scale = std::max(0.001, std::min(viewport.width() / base.width(), viewport.height() / base.height())) * m_zoom;
        const QPointF offset = viewport.center() - QPointF(base.width(), base.height()) * (scale / 2.0) + m_pan;
        QTransform transform;
        transform.translate(offset.x(), offset.y());
        transform.scale(scale, scale);
        return transform;
    }
    QPointF imagePoint(const QPointF& point) const {
        const QPointF mapped = imageTransform().inverted().map(point);
        return {std::clamp(mapped.x(), 0.0, static_cast<qreal>(std::max(0, base.width() - 1))), std::clamp(mapped.y(), 0.0, static_cast<qreal>(std::max(0, base.height() - 1)))};
    }
    int annotationAt(const QPointF& point) const {
        const qreal tolerance = 7.0 / imageTransform().m11();
        for (int index = static_cast<int>(m_annotations.size()) - 1; index >= 0; --index) {
            const auto& annotation = m_annotations[index];
            const QPainterPath path = annotationPath(annotation);
            QPainterPathStroker stroker;
            stroker.setWidth(std::max(annotation.width, tolerance * 2.0));
            const bool filled = annotation.tool == Tool::Text || annotation.tool == Tool::Number || annotation.tool == Tool::Mosaic || annotation.tool == Tool::Spotlight || annotation.variant == Variant::Filled || annotation.variant == Variant::RoundedFilled;
            if ((filled && path.contains(point)) || stroker.createStroke(path).contains(point))
                return index;
        }
        return -1;
    }
    void commit(Edit edit) {
        m_history.resize(m_historyPosition);
        switch (edit.kind) {
            case Edit::Kind::Add: m_annotations.insert(m_annotations.begin() + edit.index, edit.after); break;
            case Edit::Kind::Remove: m_annotations.erase(m_annotations.begin() + edit.index); break;
            case Edit::Kind::Replace: m_annotations[edit.index] = edit.after; break;
            case Edit::Kind::Clear: m_annotations.clear(); break;
        }
        m_history.push_back(std::move(edit));
        ++m_historyPosition;
        // A bounded action log avoids unbounded memory after long drawing sessions.
        if (m_history.size() > 1000) {
            m_history.erase(m_history.begin());
            --m_historyPosition;
        }
    }
    void add(const Annotation& annotation) {
        commit({Edit::Kind::Add, static_cast<int>(m_annotations.size()), {}, annotation});
        notify();
    }
    void finishDraft() {
        if (!m_draft)
            return;
        const Annotation annotation = *m_draft;
        m_draft.reset();
        if (annotation.tool == Tool::Pen || annotation.tool == Tool::Highlighter || pointBounds(annotation).width() > 1.0 || pointBounds(annotation).height() > 1.0)
            add(annotation);
        else
            update();
    }
    void cancelGesture() {
        m_draft.reset();
        if (m_dragBefore && m_selected >= 0)
            m_annotations[m_selected] = *m_dragBefore;
        m_dragBefore.reset();
        m_panning = false;
    }
    void notify() { update(); if (changed) changed(); }
};

// Official Material Symbols Rounded, rendered directly from SVG at device DPI.
QIcon toolIcon(Tool tool, Variant variant, bool dark, const QColor&) {
    const QColor ink(dark ? "#e9f0f6" : "#26313d");
    const bool filled = variant == Variant::Filled || variant == Variant::RoundedFilled;
    const char* name = "arrow_selector_tool";
    qreal rotation = 0;
    switch (tool) {
        case Tool::Select: break;
        case Tool::Rectangle:
            name = filled ? "rectangle_fill1" : (variant == Variant::Rounded ? "rounded_corner" : "crop_square");
            break;
        case Tool::Ellipse: name = filled ? "circle_fill1" : "circle"; break;
        case Tool::Arrow:
            name = variant == Variant::Curved ? "trending_up" :
                   variant == Variant::DoubleHeaded ? "open_in_full" :
                   variant == Variant::DoubleCurved ? "swap_calls" : "north_east";
            break;
        case Tool::Line: name = "horizontal_rule"; rotation = -45; break;
        case Tool::Pen: name = "edit"; break;
        case Tool::Highlighter: name = "ink_highlighter"; break;
        case Tool::Text: name = "text_fields"; break;
        case Tool::Number: name = "counter_1"; break;
        case Tool::Mosaic: name = "blur_on"; break;
        case Tool::Spotlight: name = "center_focus_strong"; break;
    }
    return hyprcapture::ui::materialIcon(name, ink, rotation);
}

QIcon actionIcon(const QString& action, bool dark, const QColor&) {
    const QColor ink(dark ? "#e9f0f6" : "#26313d");
    static const std::map<QString, std::string_view> names{
        {"color", "palette"}, {"undo", "undo"}, {"redo", "redo"},
        {"pin", "push_pin"}, {"confirm", "check"}, {"cancel", "close"},
        {"light", "light_mode"}, {"dark", "dark_mode"}, {"fit", "fit_screen"},
        {"clear", "delete"}, {"reselect", "restart_alt"}, {"more", "more_horiz"}};
    const auto found = names.find(action);
    return hyprcapture::ui::materialIcon(found == names.end() ? "more_horiz" : found->second,
                                        ink);
}

void paintStripButton(QWidget* widget, const QString& hint, const QIcon& icon, bool dark, bool checked, bool hover, bool focus) {
    QPainter painter(widget);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF highlight = QRectF(widget->rect()).adjusted(2, 3, -2, -3);
    if (checked || hover)
        painter.fillPath([&] { QPainterPath path; path.addRoundedRect(highlight, 5, 5); return path; }(), QColor(checked ? (dark ? "#315577" : "#dcecff") : (dark ? "#354352" : "#edf3f9")));
    if (focus) {
        painter.setPen(QPen(QColor(dark ? "#91b8db" : "#6291bd"), 1, Qt::DotLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(highlight, 5, 5);
    }
    if (!widget->isEnabled())
        painter.setOpacity(0.38);
    const int labelWidth = hint.isEmpty() ? 0 : 25;
    const int contentWidth = labelWidth + (labelWidth ? 2 : 0) + 22;
    const int left = (widget->width() - contentWidth) / 2;
    if (labelWidth) {
        QFont font = QApplication::font();
        font.setPixelSize(14);
        painter.setFont(font);
        painter.setPen(QColor(dark ? "#e9f0f6" : "#26313d"));
        painter.drawText(QRect(left, (widget->height() - 20) / 2, labelWidth, 20), Qt::AlignCenter, hint + ":");
    }
    icon.paint(&painter, QRect(left + labelWidth + (labelWidth ? 2 : 0), (widget->height() - 22) / 2, 22, 22), Qt::AlignCenter, QIcon::Normal);
    if (widget->property("annotationAccent").isValid()) {
        const QRectF swatch(left + labelWidth + (labelWidth ? 2 : 0) + 2, widget->height() - 5, 18, 3);
        painter.setPen(QPen(QColor(dark ? "#718297" : "#aab8c6"), 0.5));
        painter.setBrush(widget->property("annotationAccent").value<QColor>());
        painter.drawRoundedRect(swatch, 1.5, 1.5);
    }
}

class StripToolButton final : public QToolButton {
  public:
    StripToolButton(QString hint, QWidget* parent) : QToolButton(parent), hint(std::move(hint)) {
        setAttribute(Qt::WA_Hover);
        setCursor(Qt::ArrowCursor);
        setFixedSize(this->hint.isEmpty() ? 36 : 64, 36);
    }
    QString hint;
    bool dark = true;
  protected:
    void paintEvent(QPaintEvent*) override { paintStripButton(this, hint, icon(), dark, isChecked(), underMouse(), hasFocus()); }
};

class StripActionButton final : public QPushButton {
  public:
    StripActionButton(QString hint, QWidget* parent) : QPushButton(parent), hint(std::move(hint)) {
        setAttribute(Qt::WA_Hover);
        setCursor(Qt::ArrowCursor);
        setFixedSize(this->hint.isEmpty() ? 36 : 64, 36);
    }
    QString hint;
    bool dark = true;
  protected:
    void paintEvent(QPaintEvent*) override { paintStripButton(this, hint, icon(), dark, isChecked(), underMouse(), hasFocus()); }
};

// These frames absorb clicks on panel padding so they cannot start a new capture
// through the transparent overlay below them.
class ToolbarFrame final : public QFrame {
  public:
    explicit ToolbarFrame(QWidget* parent) : QFrame(parent) { setAttribute(Qt::WA_StyledBackground); setCursor(Qt::ArrowCursor); }
  protected:
    void mousePressEvent(QMouseEvent* event) override { event->accept(); }
    void mouseReleaseEvent(QMouseEvent* event) override { event->accept(); }
};

class ColorField final : public QWidget {
  public:
    ColorField(bool hueField, QWidget* parent) : QWidget(parent), m_hueField(hueField) {
        setFixedSize(hueField ? QSize(20, 160) : QSize(208, 160));
        setCursor(Qt::CrossCursor);
    }
    std::function<void(const QColor&)> colorPicked;
    void setColor(const QColor& color) {
        if (color.hsvHueF() >= 0)
            m_hue = color.hsvHueF();
        m_saturation = color.hsvSaturationF();
        m_value = color.valueF();
        update();
    }
  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        if (m_hueField) {
            QLinearGradient hue(0, 0, 0, height());
            for (int step = 0; step <= 6; ++step)
                hue.setColorAt(step / 6.0, QColor::fromHsvF(step == 6 ? 0.0 : step / 6.0, 1, 1));
            painter.fillRect(rect(), hue);
            const qreal y = m_hue * (height() - 1);
            painter.setPen(QPen(Qt::black, 3));
            painter.drawRect(QRectF(0, y - 2, width() - 1, 4));
            painter.setPen(QPen(Qt::white, 1));
            painter.drawRect(QRectF(0, y - 2, width() - 1, 4));
        } else {
            painter.fillRect(rect(), QColor::fromHsvF(m_hue, 1, 1));
            QLinearGradient saturation(0, 0, width(), 0);
            saturation.setColorAt(0, Qt::white);
            saturation.setColorAt(1, QColor(255, 255, 255, 0));
            painter.fillRect(rect(), saturation);
            QLinearGradient value(0, 0, 0, height());
            value.setColorAt(0, QColor(0, 0, 0, 0));
            value.setColorAt(1, Qt::black);
            painter.fillRect(rect(), value);
            painter.setRenderHint(QPainter::Antialiasing);
            const QPointF point(m_saturation * (width() - 1), (1.0 - m_value) * (height() - 1));
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(QColor("#26313d"), 3));
            painter.drawEllipse(point, 6, 6);
            painter.setPen(QPen(Qt::white, 1.5));
            painter.drawEllipse(point, 6, 6);
        }
    }
    void mousePressEvent(QMouseEvent* event) override { if (event->button() == Qt::LeftButton) pick(event->position()); else event->ignore(); }
    void mouseMoveEvent(QMouseEvent* event) override { if (event->buttons() & Qt::LeftButton) pick(event->position()); else event->ignore(); }
  private:
    bool m_hueField;
    qreal m_hue = 0, m_saturation = 1, m_value = 1;
    void pick(const QPointF& point) {
        if (m_hueField)
            m_hue = std::clamp(point.y() / std::max(1, height() - 1), 0.0, 0.99999);
        else {
            m_saturation = std::clamp(point.x() / std::max(1, width() - 1), 0.0, 1.0);
            m_value = 1.0 - std::clamp(point.y() / std::max(1, height() - 1), 0.0, 1.0);
        }
        if (colorPicked)
            colorPicked(QColor::fromHsvF(m_hue, m_saturation, m_value));
        update();
    }
};
}

struct AnnotationEditor::Impl {
    struct Choice { int group; Tool tool; Variant variant; QString name; StripToolButton* button = nullptr; };
    explicit Impl(AnnotationEditor* owner) : owner(owner) {}
    AnnotationEditor* owner;
    AnnotationCanvas* canvas = nullptr;
    ToolbarFrame* toolbar = nullptr;
    ToolbarFrame* variantPanel = nullptr;
    ToolbarFrame* colorPanel = nullptr;
    ToolbarFrame* morePanel = nullptr;
    std::vector<StripToolButton*> toolButtons = std::vector<StripToolButton*>(11, nullptr);
    std::vector<Choice> choices;
    std::vector<QAbstractButton*> strip;
    std::map<int, std::pair<Tool, Variant>> remembered = {
        {1, {Tool::Rectangle, Variant::Outline}}, {2, {Tool::Ellipse, Variant::Outline}},
        {3, {Tool::Arrow, Variant::Straight}}, {5, {Tool::Pen, Variant::Outline}}
    };
    QSpinBox* width = nullptr;
    QLabel* widthLabel = nullptr;
    StripToolButton* color = nullptr;
    StripToolButton* customColor = nullptr;
    StripToolButton* themeDark = nullptr;
    StripToolButton* themeLight = nullptr;
    ColorField* saturationValue = nullptr;
    ColorField* hue = nullptr;
    StripActionButton* undo = nullptr;
    StripActionButton* redo = nullptr;
    StripActionButton* clear = nullptr;
    StripActionButton* fit = nullptr;
    StripActionButton* confirm = nullptr;
    StripActionButton* cancel = nullptr;
    StripActionButton* pin = nullptr;
    StripActionButton* reselect = nullptr;
    StripActionButton* more = nullptr;
    int openGroup = -1;
    StripToolButton* popupAnchor = nullptr;
    bool dark = true;
    bool layoutActive = false;
    QSize captureSize;
    QRect captureGeometry;
    QRect clusterGeometry;
    QSettings* settings = nullptr;
    QPointer<QWidget> textPanel;
    std::function<void(bool)> finishText;
    void finishTextEditing(bool accept) {
        auto finish = std::move(finishText);
        finishText = {};
        if (finish) finish(accept);
    }


    void savePreferences() {
        settings->setValue("color", canvas->color.name());
        settings->setValue("width", static_cast<int>(canvas->strokeWidth));
        settings->setValue("dark", dark);
    }
    QString toolName(Tool tool) const {
        switch (tool) {
            case Tool::Select: return owner->tr("Select");
            case Tool::Rectangle: return owner->tr("Rectangle");
            case Tool::Ellipse: return owner->tr("Ellipse");
            case Tool::Arrow: return owner->tr("Arrow");
            case Tool::Line: return owner->tr("Line");
            case Tool::Pen: return owner->tr("Pen");
            case Tool::Highlighter: return owner->tr("Highlighter");
            case Tool::Text: return owner->tr("Text");
            case Tool::Number: return owner->tr("Number");
            case Tool::Mosaic: return owner->tr("Mosaic");
            case Tool::Spotlight: return owner->tr("Spotlight");
        }
        return {};
    }
    QString choiceLabel(const Choice& choice) const {
        switch (choice.variant) {
            case Variant::Filled: return toolName(choice.tool) + " · " + owner->tr("Filled");
            case Variant::Rounded: return owner->tr("Rounded") + " · " + toolName(choice.tool);
            case Variant::RoundedFilled: return owner->tr("Rounded filled");
            case Variant::Curved: return owner->tr("Curved") + " · " + toolName(choice.tool);
            case Variant::DoubleHeaded: return owner->tr("Double headed");
            case Variant::DoubleCurved: return owner->tr("Double curved");
            default: return toolName(choice.tool);
        }
    }
    int groupFor(Tool tool) const {
        switch (tool) {
            case Tool::Rectangle: return 1;
            case Tool::Ellipse: case Tool::Spotlight: return 2;
            case Tool::Arrow: case Tool::Line: return 3;
            case Tool::Pen: case Tool::Highlighter: return 5;
            default: return -1;
        }
    }
    void closePanels() {
        finishTextEditing(false);
        variantPanel->hide();
        colorPanel->hide();
        morePanel->hide();
        openGroup = -1;
    }
    void prepareOutput() { finishTextEditing(true); canvas->finishEditing(); closePanels(); }
    void updateTools() {
        const int selectedGroup = groupFor(canvas->tool);
        for (int id : {0, 1, 2, 3, 5, 7, 8, 9}) {
            auto* button = toolButtons[id];
            const bool group = remembered.contains(id);
            const Tool displayed = group ? remembered[id].first : static_cast<Tool>(id);
            const Variant variant = group ? remembered[id].second : Variant::Outline;
            button->dark = dark;
            button->setIcon(toolIcon(displayed, variant, dark, canvas->color));
            button->setChecked(group ? selectedGroup == id : canvas->tool == displayed);
            button->setAccessibleName(toolName(displayed));
            button->setToolTip(toolName(displayed) + " (" + button->hint + ")");
        }
        for (auto& choice : choices) {
            choice.button->dark = dark;
            choice.button->setIcon(toolIcon(choice.tool, choice.variant, dark, canvas->color));
            choice.button->setChecked(choice.tool == canvas->tool && choice.variant == canvas->variant);
            choice.button->setAccessibleName(choiceLabel(choice));
            choice.button->setToolTip(choiceLabel(choice));
        }
    }
    void activateTool(Tool tool, Variant variant, bool remember = true) {
        canvas->selectTool(tool);
        canvas->variant = variant;
        const int group = groupFor(tool);
        if (remember && group >= 0)
            remembered[group] = {tool, variant};
        closePanels();
        updateTools();
        canvas->setFocus(Qt::ShortcutFocusReason);
        positionToolbar();
    }
    void activateGroup(int group, bool popup) {
        const bool wasOpen = variantPanel->isVisible() && openGroup == group;
        const auto [tool, variant] = remembered[group];
        activateTool(tool, variant, false);
        if (popup && !wasOpen) {
            openGroup = group;
            popupAnchor = toolButtons[group];
            for (auto& choice : choices)
                choice.button->setVisible(choice.group == group);
            variantPanel->adjustSize();
            variantPanel->show();
            positionPopups();
        }
    }
    void toggleColor() {
        const bool wasOpen = colorPanel->isVisible();
        canvas->finishEditing();
        closePanels();
        if (!wasOpen) {
            saturationValue->setColor(canvas->color);
            hue->setColor(canvas->color);
            colorPanel->show();
            positionPopups();
        }
        canvas->setFocus(Qt::ShortcutFocusReason);
    }
    void toggleMore() {
        const bool wasOpen = morePanel->isVisible();
        closePanels();
        if (!wasOpen) {
            morePanel->show();
            positionPopups();
        }
        canvas->setFocus(Qt::ShortcutFocusReason);
    }
    void updateColor() {
        color->dark = dark;
        color->setProperty("annotationAccent", canvas->color);
        color->setIcon(actionIcon("color", dark, canvas->color));
        customColor->dark = dark;
        customColor->setIcon(actionIcon("color", dark, canvas->color));
        saturationValue->setColor(canvas->color);
        hue->setColor(canvas->color);
        updateTools();
    }
    void chooseColor(const QColor& selected, bool close) {
        if (!selected.isValid())
            return;
        canvas->color = selected;
        updateColor();
        savePreferences();
        if (close) {
            closePanels();
            canvas->setFocus(Qt::ShortcutFocusReason);
        }
    }
    void applyTheme() {
        const QString surface = dark ? "#222b36" : "#ffffff";
        const QString border = dark ? "#526171" : "#cbd4de";
        const QString ink = dark ? "#e9f0f6" : "#26313d";
        const QString selected = dark ? "#315577" : "#dcecff";
        for (auto* frame : {toolbar, variantPanel, colorPanel, morePanel})
            frame->setStyleSheet(QString("QFrame#%1 { background: %2; border: 1px solid %3; border-radius: 6px; } QLabel { color: %4; } QSpinBox { color: %4; background: %2; border: 1px solid %3; border-radius: 4px; padding: 1px 2px; selection-background-color: %5; }")
                .arg(frame->objectName(), surface, border, ink, selected));
        for (const auto& [button, action] : std::vector<std::pair<StripActionButton*, QString>>{{undo, "undo"}, {redo, "redo"}, {pin, "pin"}, {confirm, "confirm"}, {cancel, "cancel"}, {clear, "clear"}, {fit, "fit"}, {reselect, "reselect"}, {more, "more"}}) {
            button->dark = dark;
            button->setIcon(actionIcon(action, dark, canvas->color));
            button->update();
        }
        themeDark->dark = dark;
        themeLight->dark = dark;
        themeDark->setChecked(dark);
        themeLight->setChecked(!dark);
        themeDark->setIcon(actionIcon("dark", dark, canvas->color));
        themeLight->setIcon(actionIcon("light", dark, canvas->color));
        updateColor();
        positionToolbar();
    }
    QSize arrangeStrip(int availableWidth) {
        constexpr int horizontalMargin = 6, verticalMargin = 5, spacing = 2;
        int naturalWidth = horizontalMargin * 2 - spacing;
        for (auto* button : strip)
            naturalWidth += button->width() + spacing;
        const int panelWidth = std::max(1, std::min(naturalWidth, availableWidth));
        const int rowWidth = std::max(1, panelWidth - horizontalMargin * 2);
        int x = 0, y = 0;
        for (auto* button : strip) {
            // Keep the final decision controls together when the strip wraps.
            const int requiredWidth = button == cancel
                ? cancel->width() + confirm->width() + more->width() + spacing * 2
                : button->width();
            if (x && x + requiredWidth > rowWidth) {
                x = 0;
                y += 36 + spacing;
            }
            button->move(horizontalMargin + x, verticalMargin + y);
            x += button->width() + spacing;
        }
        return QSize(panelWidth, y + 36 + verticalMargin * 2);
    }
    void positionToolbar() {
        if (layoutActive || !toolbar || !canvas)
            return;
        layoutActive = true;
        const QRect bounds = owner->rect().adjusted(8, 8, -8, -8);
        const QSize panelSize = arrangeStrip(std::max(1, bounds.width()));
        const QSize accessory = captureSize.isEmpty() ? QSize() : QSize(std::min(captureSize.width(), std::max(1, bounds.width())), captureSize.height());
        const int gap = accessory.isEmpty() ? 0 : 6;
        const int clusterHeight = panelSize.height() + accessory.height() + gap;
        canvas->setViewportBottomInset(clusterHeight + 28);
        const QRect imageRect = canvas->displayRect();
        const int clusterWidth = std::max(panelSize.width(), accessory.width());
        const int maximumX = std::max(bounds.left(), bounds.right() - clusterWidth + 1);
        const int x = std::clamp(imageRect.right() - clusterWidth + 1, bounds.left(), maximumX);
        int y = imageRect.bottom() + 9;
        if (y + clusterHeight > bounds.bottom() + 1) {
            const int above = imageRect.top() - clusterHeight - 8;
            y = above >= bounds.top() ? above : bounds.bottom() - clusterHeight + 1;
        }
        y = std::max(bounds.top(), y);
        const QRect oldToolbar = toolbar->geometry();
        const QRect oldCapture = captureGeometry;
        clusterGeometry = QRect(x, y, clusterWidth, clusterHeight);
        captureGeometry = accessory.isEmpty() ? QRect() : QRect(x + clusterWidth - accessory.width(), y + panelSize.height() + gap, accessory.width(), accessory.height());
        toolbar->setGeometry(x + clusterWidth - panelSize.width(), y, panelSize.width(), panelSize.height());
        toolbar->raise();
        positionPopups();
        layoutActive = false;
        if (toolbar->geometry() != oldToolbar || captureGeometry != oldCapture)
            emit owner->toolbarGeometryChanged(toolbar->geometry());
    }
    void placePopup(QWidget* panel, int centerX) {
        const QRect bounds = owner->rect().adjusted(8, 8, -8, -8);
        QSize size = panel->sizeHint().expandedTo(panel->minimumSizeHint());
        if (panel == colorPanel)
            size = QSize(260, 246);
        const int width = std::min(size.width(), std::max(1, bounds.width()));
        const int height = std::min(size.height(), std::max(1, bounds.height()));
        const int maxX = std::max(bounds.left(), bounds.right() - width + 1);
        const int x = std::clamp(centerX - width / 2, bounds.left(), maxX);
        int y = clusterGeometry.top() - height - 8;
        if (y < bounds.top())
            y = clusterGeometry.bottom() + 9;
        y = std::clamp(y, bounds.top(), std::max(bounds.top(), bounds.bottom() - height + 1));
        panel->setGeometry(x, y, width, height);
        panel->raise();
    }
    void positionPopups() {
        if (variantPanel->isVisible() && popupAnchor)
            placePopup(variantPanel, popupAnchor->mapTo(owner, popupAnchor->rect().center()).x());
        if (colorPanel->isVisible())
            placePopup(colorPanel, toolbar->x() + 130);
        if (morePanel->isVisible())
            placePopup(morePanel, more->mapTo(owner, more->rect().center()).x());
    }
    void retranslate() {
        const auto actionLabel = [&](QAbstractButton* button, const QString& label, const QString& key) {
            button->setText(label);
            button->setAccessibleName(label);
            button->setToolTip(key.isEmpty() ? label : label + " (" + key + ")");
        };
        actionLabel(color, owner->tr("Color"), "Q");
        actionLabel(customColor, owner->tr("Color"), {});
        actionLabel(undo, owner->tr("Undo"), "Z");
        actionLabel(redo, owner->tr("Redo"), "X");
        actionLabel(confirm, owner->tr("Capture"), "Enter");
        actionLabel(cancel, owner->tr("Cancel"), "Esc");
        actionLabel(pin, owner->tr("Pin"), "P");
        actionLabel(clear, owner->tr("Clear"), {});
        actionLabel(fit, owner->tr("Fit"), "Ctrl+0");
        actionLabel(reselect, owner->tr("Reselect"), {});
        actionLabel(more, owner->tr("Pin") + " · " + owner->tr("Fit") + " · " + owner->tr("Clear") + " · " + owner->tr("Reselect"), {});
        actionLabel(themeDark, owner->tr("Dark"), {});
        actionLabel(themeLight, owner->tr("Light"), {});
        widthLabel->setText(owner->tr("Width"));
        width->setToolTip(owner->tr("Width"));
        canvas->setToolTip(owner->tr("Drag to draw. Select to move or delete annotations. Wheel to zoom; Space+drag to pan."));
        updateTools();
        morePanel->adjustSize();
        positionToolbar();
    }
};

AnnotationEditor::AnnotationEditor(QWidget* parent) : QWidget(parent), m_impl(std::make_unique<Impl>(this)) {
    setObjectName("annotationEditor");
    setAttribute(Qt::WA_TranslucentBackground);
    auto& ui = *m_impl;
    ui.canvas = new AnnotationCanvas(this);
    ui.settings = new QSettings(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/hyprcapture/editor.ini", QSettings::IniFormat, this);
    const QColor savedColor(ui.settings->value("color", "#ff5252").toString());
    if (savedColor.isValid())
        ui.canvas->color = savedColor;
    ui.canvas->strokeWidth = std::clamp(ui.settings->value("width", 4).toInt(), 1, 24);
    ui.dark = ui.settings->value("dark", QApplication::palette().color(QPalette::Window).lightness() < 128).toBool();
    ui.toolbar = new ToolbarFrame(this);
    ui.toolbar->setObjectName("annotationToolbar");
    ui.variantPanel = new ToolbarFrame(this);
    ui.variantPanel->setObjectName("annotationVariantPanel");
    ui.colorPanel = new ToolbarFrame(this);
    ui.colorPanel->setObjectName("annotationColorPanel");
    ui.morePanel = new ToolbarFrame(this);
    ui.morePanel->setObjectName("annotationMorePanel");
    ui.variantPanel->hide();
    ui.colorPanel->hide();
    ui.morePanel->hide();

    ui.color = new StripToolButton("Q", ui.toolbar);
    ui.color->setObjectName("annotationColorTrigger");
    ui.strip.push_back(ui.color);
    const std::vector<std::pair<int, QString>> mainTools = {{0, "V"}, {1, "R"}, {2, "E"}, {3, "A"}, {5, "D"}, {7, "T"}, {9, "G"}, {8, "B"}};
    for (const auto& [id, key] : mainTools) {
        auto* button = new StripToolButton(key, ui.toolbar);
        button->setObjectName(QString("annotationTool%1").arg(id));
        button->setCheckable(true);
        ui.toolButtons[id] = button;
        ui.strip.push_back(button);
        connect(button, &QToolButton::clicked, this, [this, id] {
            if (m_impl->remembered.contains(id))
                m_impl->activateGroup(id, true);
            else
                m_impl->activateTool(static_cast<Tool>(id), Variant::Outline);
        });
    }
    const auto action = [&](const QString& key, const char* name) {
        auto* button = new StripActionButton(key, ui.toolbar);
        button->setObjectName(name);
        ui.strip.push_back(button);
        return button;
    };
    ui.undo = action("Z", "annotationUndo");
    ui.redo = action("X", "annotationRedo");
    ui.cancel = action({}, "annotationCancel");
    ui.confirm = action({}, "annotationConfirm");
    ui.more = action({}, "annotationMore");

    auto* variantRow = new QHBoxLayout(ui.variantPanel);
    variantRow->setContentsMargins(5, 6, 5, 6);
    variantRow->setSpacing(2);
    const std::vector<Impl::Choice> options = {
        {1, Tool::Rectangle, Variant::Outline, "annotationVariantRectOutline"},
        {1, Tool::Rectangle, Variant::Rounded, "annotationVariantRectRounded"},
        {1, Tool::Rectangle, Variant::Filled, "annotationVariantRectFilled"},
        {1, Tool::Rectangle, Variant::RoundedFilled, "annotationVariantRectRoundedFilled"},
        {2, Tool::Ellipse, Variant::Outline, "annotationVariantEllipseOutline"},
        {2, Tool::Ellipse, Variant::Filled, "annotationVariantEllipseFilled"},
        {2, Tool::Spotlight, Variant::Outline, "annotationTool10"},
        {3, Tool::Arrow, Variant::Straight, "annotationVariantArrowStraight"},
        {3, Tool::Arrow, Variant::Curved, "annotationVariantArrowCurved"},
        {3, Tool::Arrow, Variant::DoubleCurved, "annotationVariantArrowDoubleCurved"},
        {3, Tool::Line, Variant::Outline, "annotationTool4"},
        {3, Tool::Arrow, Variant::DoubleHeaded, "annotationVariantArrowDouble"},
        {5, Tool::Pen, Variant::Outline, "annotationVariantPen"},
        {5, Tool::Highlighter, Variant::Outline, "annotationTool6"}
    };
    for (auto choice : options) {
        choice.button = new StripToolButton({}, ui.variantPanel);
        choice.button->setObjectName(choice.name);
        choice.button->setCheckable(true);
        variantRow->addWidget(choice.button);
        if (choice.tool == Tool::Line || choice.tool == Tool::Highlighter || choice.tool == Tool::Spotlight)
            ui.toolButtons[static_cast<int>(choice.tool)] = choice.button;
        choice.button->hide();
        connect(choice.button, &QToolButton::clicked, this, [this, tool = choice.tool, variant = choice.variant] { m_impl->activateTool(tool, variant); });
        ui.choices.push_back(std::move(choice));
    }

    auto* colorRows = new QVBoxLayout(ui.colorPanel);
    colorRows->setContentsMargins(10, 10, 10, 10);
    colorRows->setSpacing(10);
    auto* colorTop = new QHBoxLayout;
    colorTop->setSpacing(4);
    ui.themeLight = new StripToolButton({}, ui.colorPanel);
    ui.themeLight->setObjectName("annotationThemeLight");
    ui.themeDark = new StripToolButton({}, ui.colorPanel);
    ui.themeDark->setObjectName("annotationThemeDark");
    for (auto* button : {ui.themeLight, ui.themeDark}) {
        button->setCheckable(true);
        button->setFixedSize(28, 26);
        colorTop->addWidget(button);
    }
    colorTop->addStretch();
    ui.widthLabel = new QLabel(ui.colorPanel);
    colorTop->addWidget(ui.widthLabel);
    ui.width = new QSpinBox(ui.colorPanel);
    ui.width->setObjectName("annotationWidth");
    ui.width->setRange(1, 24);
    ui.width->setValue(static_cast<int>(ui.canvas->strokeWidth));
    ui.width->setFixedSize(48, 26);
    colorTop->addWidget(ui.width);
    ui.customColor = new StripToolButton({}, ui.colorPanel);
    ui.customColor->setObjectName("annotationColorPicker");
    ui.customColor->setFixedSize(28, 26);
    colorTop->addWidget(ui.customColor);
    colorRows->addLayout(colorTop);
    auto* hsv = new QHBoxLayout;
    hsv->setSpacing(10);
    ui.saturationValue = new ColorField(false, ui.colorPanel);
    ui.saturationValue->setObjectName("annotationSaturationValue");
    ui.hue = new ColorField(true, ui.colorPanel);
    ui.hue->setObjectName("annotationHue");
    hsv->addWidget(ui.saturationValue);
    hsv->addWidget(ui.hue);
    colorRows->addLayout(hsv);
    auto* swatches = new QHBoxLayout;
    swatches->setSpacing(5);
    for (const QString value : {"#ff4b55", "#ff9d42", "#ffd84d", "#5bd686", "#42c8d8", "#4d94ff", "#a77bff", "#f774ba", "#ffffff", "#202020"}) {
        auto* swatch = new QToolButton(ui.colorPanel);
        swatch->setObjectName("annotationColor" + value.mid(1));
        swatch->setFixedSize(18, 18);
        swatch->setStyleSheet(QString("background: %1; border: 1px solid #79889b; border-radius: 4px;").arg(value));
        swatch->setToolTip(value);
        swatch->setAccessibleName(value);
        swatches->addWidget(swatch);
        connect(swatch, &QToolButton::clicked, this, [this, value] { m_impl->chooseColor(QColor(value), true); });
    }
    swatches->addStretch();
    colorRows->addLayout(swatches);
    colorRows->addStretch();

    auto* moreRows = new QHBoxLayout(ui.morePanel);
    moreRows->setContentsMargins(5, 6, 5, 6);
    moreRows->setSpacing(2);
    const auto overflow = [&](const char* name) {
        auto* button = new StripActionButton({}, ui.morePanel);
        button->setObjectName(name);
        moreRows->addWidget(button);
        return button;
    };
    ui.pin = overflow("annotationPin");
    ui.clear = overflow("annotationClear");
    ui.fit = overflow("annotationFit");
    ui.reselect = overflow("annotationReselect");

    ui.canvas->changed = [this] {
        m_impl->undo->setEnabled(m_impl->canvas->canUndo());
        m_impl->redo->setEnabled(m_impl->canvas->canRedo());
        m_impl->clear->setEnabled(m_impl->canvas->hasAnnotations());
        m_impl->positionToolbar();
        emit annotationsChanged();
    };
    ui.canvas->viewChanged = [this] { m_impl->positionToolbar(); };
    ui.canvas->captureRectChanged = [this](const QRect& rect) { emit captureRectChangeRequested(rect); };
    ui.canvas->pressed = [this] { m_impl->closePanels(); };
    ui.canvas->requestText = [this](const QString& existing, const QPoint& anchor, std::function<void(std::optional<QString>)> completed) {
        m_impl->closePanels();
        auto* panel = new QFrame(this);
        panel->setObjectName("annotationTextPanel");
        panel->setAttribute(Qt::WA_StyledBackground);
        const bool dark = m_impl->dark;
        panel->setStyleSheet(QString("QFrame#annotationTextPanel { background:%1; border:1px solid %2; border-radius:6px; } QPlainTextEdit { background:%1; color:%3; border:0; }")
            .arg(dark ? "#222b36" : "#ffffff", dark ? "#526171" : "#cbd4de", dark ? "#e9f0f6" : "#26313d"));
        auto* layout = new QVBoxLayout(panel);
        layout->setContentsMargins(8, 8, 8, 8);
        auto* input = new AnnotationTextInput(panel);
        input->setObjectName("annotationTextInput");
        input->setAccessibleName(tr("Text annotation"));
        input->setPlaceholderText(tr("Enter text (multiple lines supported):"));
        input->setPlainText(existing);
        layout->addWidget(input);
        auto* actions = new QHBoxLayout;
        actions->addStretch();
        auto* cancelText = new QPushButton(panel);
        auto* acceptText = new QPushButton(panel);
        cancelText->setObjectName("annotationTextCancel");
        acceptText->setObjectName("annotationTextAccept");
        cancelText->setIcon(actionIcon("cancel", dark, {}));
        acceptText->setIcon(actionIcon("confirm", dark, {}));
        cancelText->setToolTip(tr("Cancel") + " (Esc)");
        acceptText->setToolTip(tr("Text annotation") + " (Ctrl+Enter)");
        cancelText->setAccessibleName(tr("Cancel"));
        acceptText->setAccessibleName(tr("Text annotation"));
        actions->addWidget(cancelText); actions->addWidget(acceptText);
        layout->addLayout(actions);
        const QSize size(std::min(340, std::max(1, width() - 16)), std::min(180, std::max(1, height() - 16)));
        panel->setGeometry(std::clamp(anchor.x(), 8, std::max(8, width() - size.width() - 8)),
                           std::clamp(anchor.y(), 8, std::max(8, height() - size.height() - 8)), size.width(), size.height());
        m_impl->textPanel = panel;
        m_impl->canvas->setEnabled(false);
        m_impl->finishText = [this, panel, input, completed = std::move(completed)](bool accept) {
            const QString text = input->toPlainText();
            panel->hide();
            panel->deleteLater();
            m_impl->textPanel = nullptr;
            m_impl->canvas->setEnabled(true);
            m_impl->canvas->setFocus();
            completed(accept ? std::optional<QString>(text) : std::nullopt);
        };
        input->finished = [this](bool accept) { m_impl->finishTextEditing(accept); };
        connect(cancelText, &QPushButton::clicked, this, [this] { m_impl->finishTextEditing(false); });
        connect(acceptText, &QPushButton::clicked, this, [this] { m_impl->finishTextEditing(true); });
        panel->show(); panel->raise(); input->setFocus();
    };
    connect(ui.color, &QToolButton::clicked, this, [this] { m_impl->toggleColor(); });
    connect(ui.more, &QPushButton::clicked, this, [this] { m_impl->toggleMore(); });
    connect(ui.width, &QSpinBox::valueChanged, this, [this](int value) { m_impl->canvas->strokeWidth = value; m_impl->savePreferences(); });
    connect(ui.themeLight, &QToolButton::clicked, this, [this] { m_impl->dark = false; m_impl->applyTheme(); m_impl->savePreferences(); });
    connect(ui.themeDark, &QToolButton::clicked, this, [this] { m_impl->dark = true; m_impl->applyTheme(); m_impl->savePreferences(); });
    connect(ui.customColor, &QToolButton::clicked, this, [this] {
        m_impl->closePanels();
        const QColor selected = QColorDialog::getColor(m_impl->canvas->color, this, tr("Color"));
        m_impl->chooseColor(selected, true);
    });
    ui.saturationValue->colorPicked = [this](const QColor& selected) { m_impl->chooseColor(selected, false); };
    ui.hue->colorPicked = [this](const QColor& selected) { m_impl->chooseColor(selected, false); };
    connect(ui.undo, &QPushButton::clicked, this, [this] { m_impl->closePanels(); undo(); });
    connect(ui.redo, &QPushButton::clicked, this, [this] { m_impl->closePanels(); redo(); });
    connect(ui.clear, &QPushButton::clicked, this, [this] { m_impl->closePanels(); clearAnnotations(); });
    connect(ui.fit, &QPushButton::clicked, this, [this] { m_impl->closePanels(); fitImage(); });
    connect(ui.confirm, &QPushButton::clicked, this, [this] { m_impl->prepareOutput(); emit confirmRequested(); });
    connect(ui.cancel, &QPushButton::clicked, this, [this] { m_impl->closePanels(); emit cancelRequested(); });
    connect(ui.pin, &QPushButton::clicked, this, [this] { m_impl->prepareOutput(); emit pinRequested(); });
    connect(ui.reselect, &QPushButton::clicked, this, [this] { m_impl->prepareOutput(); emit reselectRequested(); });

    const auto shortcut = [this](const QKeySequence& sequence, std::function<void()> callback) {
        auto* key = new QShortcut(sequence, this);
        key->setContext(Qt::WidgetWithChildrenShortcut);
        connect(key, &QShortcut::activated, this, [callback = std::move(callback)] {
            // Color width and text dialogs must retain normal text entry.
            if (QApplication::activeModalWidget() || qobject_cast<QSpinBox*>(QApplication::focusWidget()) || qobject_cast<QPlainTextEdit*>(QApplication::focusWidget()))
                return;
            callback();
        });
    };
    shortcut(QKeySequence::Undo, [this] { undo(); });
    shortcut(QKeySequence("Ctrl+Shift+Z"), [this] { redo(); });
    shortcut(QKeySequence("Ctrl+Y"), [this] { redo(); });
    shortcut(QKeySequence("Ctrl+P"), [this] { m_impl->pin->click(); });
    shortcut(QKeySequence("Ctrl+0"), [this] { fitImage(); });
    shortcut(QKeySequence("Q"), [this] { m_impl->toggleColor(); });
    shortcut(QKeySequence("Z"), [this] { undo(); });
    shortcut(QKeySequence("X"), [this] { redo(); });
    shortcut(QKeySequence("P"), [this] { m_impl->pin->click(); });
    shortcut(QKeySequence(Qt::Key_Return), [this] { m_impl->confirm->click(); });
    shortcut(QKeySequence(Qt::Key_Enter), [this] { m_impl->confirm->click(); });
    shortcut(QKeySequence(Qt::Key_Escape), [this] {
        if (m_impl->variantPanel->isVisible() || m_impl->colorPanel->isVisible() || m_impl->morePanel->isVisible())
            m_impl->closePanels();
        else
            m_impl->cancel->click();
    });
    const auto toolShortcut = [&](const QString& key, Tool tool, Variant variant = Variant::Outline) {
        shortcut(QKeySequence(key), [this, tool, variant] { m_impl->activateTool(tool, variant); });
    };
    toolShortcut("V", Tool::Select);
    for (const auto& [key, group] : std::vector<std::pair<QString, int>>{{"R", 1}, {"E", 2}, {"A", 3}, {"D", 5}})
        shortcut(QKeySequence(key), [this, group] { m_impl->activateGroup(group, false); });
    toolShortcut("W", Tool::Line);
    toolShortcut("H", Tool::Highlighter);
    toolShortcut("T", Tool::Text);
    toolShortcut("B", Tool::Number);
    toolShortcut("G", Tool::Mosaic);
    toolShortcut("Shift+R", Tool::Rectangle, Variant::Rounded);
    toolShortcut("Shift+E", Tool::Ellipse, Variant::Filled);
    toolShortcut("Shift+A", Tool::Arrow, Variant::Curved);
    toolShortcut("Shift+D", Tool::Rectangle, Variant::Filled);
    toolShortcut("Shift+B", Tool::Spotlight);
    toolShortcut("Shift+W", Tool::Arrow, Variant::DoubleCurved);
    ui.retranslate();
    ui.applyTheme();
    ui.undo->setEnabled(false);
    ui.redo->setEnabled(false);
    ui.clear->setEnabled(false);
}

AnnotationEditor::~AnnotationEditor() = default;
void AnnotationEditor::setImage(const QImage& image, bool preserveAnnotations) {
    m_impl->closePanels();
    m_impl->canvas->setImage(image, preserveAnnotations);
    m_impl->positionToolbar();
}
QImage AnnotationEditor::resultImage() const { return m_impl->canvas->result(); }
void AnnotationEditor::setRegionResizeBounds(const QRect& bounds) { m_impl->canvas->resizeBounds = bounds; m_impl->canvas->update(); }
void AnnotationEditor::replaceCaptureImage(const QImage& image, const QRect& displayRect) {
    m_impl->canvas->replaceCapture(image, displayRect);
}

QRect AnnotationEditor::canvasGeometry() const { return m_impl->canvas->displayRect(); }
QWidget* AnnotationEditor::toolbarWidget() const { return m_impl->toolbar; }
QRect AnnotationEditor::toolbarGeometry() const { return m_impl->toolbar->geometry(); }
void AnnotationEditor::setCaptureToolbarSize(const QSize& size) {
    if (m_impl->captureSize == size)
        return;
    m_impl->captureSize = size;
    m_impl->positionToolbar();
}
QRect AnnotationEditor::captureToolbarGeometry() const { return m_impl->captureGeometry; }
void AnnotationEditor::undo() { m_impl->canvas->undo(); }
void AnnotationEditor::redo() { m_impl->canvas->redo(); }
void AnnotationEditor::clearAnnotations() { m_impl->canvas->clearAnnotations(); }
void AnnotationEditor::fitImage() { m_impl->canvas->fitViewport(); }
void AnnotationEditor::setImageDisplayRect(const QRect& rect) { m_impl->canvas->setDisplayRect(rect); }
void AnnotationEditor::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    m_impl->canvas->setGeometry(rect());
    m_impl->positionToolbar();
}
void AnnotationEditor::hideEvent(QHideEvent* event) {
    m_impl->closePanels();
    QWidget::hideEvent(event);
}
void AnnotationEditor::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange && m_impl->toolbar)
        m_impl->retranslate();
}
