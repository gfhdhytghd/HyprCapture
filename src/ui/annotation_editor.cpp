#include "ui/annotation_editor.hpp"

#include <QApplication>
#include <QButtonGroup>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QPlainTextEdit>
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
    std::function<QString(const QString&)> requestText;
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
    void fit() { m_zoom = 1.0; m_pan = {}; update(); }
    void fitViewport() { m_fitToViewport = true; fit(); }
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
    }

    void mousePressEvent(QMouseEvent* event) override {
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
            annotation.text = requestText ? requestText({}) : QString();
            if (!annotation.text.trimmed().isEmpty())
                add(annotation);
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
        if (m_panning) {
            m_pan = m_panBefore + event->position() - m_pointerOrigin;
            update();
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
                const QString text = requestText(before.text);
                if (!text.trimmed().isEmpty() && text != before.text) {
                    Annotation after = before;
                    after.text = text;
                    commit({Edit::Kind::Replace, index, before, after});
                    notify();
                }
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
    bool m_fitToViewport = false;
    bool m_spaceDown = false;
    bool m_panning = false;

    QTransform imageTransform() const {
        if (base.isNull())
            return {};
        const QRectF viewport = m_fitToViewport || m_displayRect.isEmpty() ? QRectF(rect().adjusted(12, 12, -12, -150)) : QRectF(m_displayRect);
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

QIcon toolIcon(Tool tool) {
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor("#a7bacf"), 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    switch (tool) {
        case Tool::Select: {
            QPolygonF cursor;
            cursor << QPointF(6, 3) << QPointF(19, 13) << QPointF(12, 14) << QPointF(9, 21);
            painter.drawPolygon(cursor);
            break;
        }
        case Tool::Rectangle: painter.drawRoundedRect(QRectF(3, 5, 18, 14), 2, 2); break;
        case Tool::Ellipse: painter.drawEllipse(QRectF(3, 5, 18, 14)); break;
        case Tool::Arrow: painter.drawLine(QPointF(4, 20), QPointF(20, 4)); drawArrowHead(painter, QPointF(20, 4), QPointF(16, -16), 0.8); break;
        case Tool::Line: painter.drawLine(QPointF(4, 20), QPointF(20, 4)); break;
        case Tool::Pen: {
            QPainterPath path; path.moveTo(3, 17); path.cubicTo(8, 3, 15, 22, 21, 6); painter.drawPath(path); break;
        }
        case Tool::Highlighter: painter.setPen(QPen(QColor("#f3d65c"), 7)); painter.drawLine(QPointF(5, 18), QPointF(19, 6)); break;
        case Tool::Text: { QFont font; font.setPixelSize(21); font.setBold(true); painter.setFont(font); painter.drawText(pixmap.rect(), Qt::AlignCenter, "T"); break; }
        case Tool::Number: painter.drawEllipse(QRectF(3, 3, 18, 18)); painter.drawText(pixmap.rect(), Qt::AlignCenter, "1"); break;
        case Tool::Mosaic:
            for (int y = 4; y <= 16; y += 6) for (int x = 4; x <= 16; x += 6) painter.fillRect(QRect(x, y, 5, 5), ((x + y) % 12 == 8) ? QColor("#8ea7bd") : QColor("#52667b"));
            break;
        case Tool::Spotlight: painter.fillRect(QRect(3, 3, 18, 18), QColor("#52667b")); painter.setBrush(QColor("#dbe7f2")); painter.drawEllipse(QRectF(7, 7, 10, 10)); break;
    }
    return QIcon(pixmap);
}
}

struct AnnotationEditor::Impl {
    explicit Impl(AnnotationEditor* owner) : owner(owner) {}
    AnnotationEditor* owner;
    AnnotationCanvas* canvas = nullptr;
    QFrame* toolbar = nullptr;
    QButtonGroup* tools = nullptr;
    std::vector<QToolButton*> toolButtons;
    QComboBox* variants = nullptr;
    QSpinBox* width = nullptr;
    QLabel* widthLabel = nullptr;
    QPushButton* color = nullptr;
    QPushButton* undo = nullptr;
    QPushButton* redo = nullptr;
    QPushButton* clear = nullptr;
    QPushButton* fit = nullptr;
    QComboBox* theme = nullptr;
    QPushButton* copy = nullptr;
    QPushButton* save = nullptr;
    QPushButton* pin = nullptr;
    QPushButton* reselect = nullptr;
    bool dark = true;
    QSettings* settings = nullptr;

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
    QString variantName(Variant variant) const {
        switch (variant) {
            case Variant::Outline: return owner->tr("Outline");
            case Variant::Filled: return owner->tr("Filled");
            case Variant::Rounded: return owner->tr("Rounded");
            case Variant::RoundedFilled: return owner->tr("Rounded filled");
            case Variant::Straight: return owner->tr("Straight");
            case Variant::Curved: return owner->tr("Curved");
            case Variant::DoubleHeaded: return owner->tr("Double headed");
            case Variant::DoubleCurved: return owner->tr("Double curved");
        }
        return {};
    }
    void updateVariants() {
        const Variant old = canvas->variant;
        variants->blockSignals(true);
        variants->clear();
        const auto add = [&](Variant variant) { variants->addItem(variantName(variant), static_cast<int>(variant)); };
        if (canvas->tool == Tool::Rectangle) { add(Variant::Outline); add(Variant::Filled); add(Variant::Rounded); add(Variant::RoundedFilled); }
        else if (canvas->tool == Tool::Ellipse) { add(Variant::Outline); add(Variant::Filled); }
        else if (canvas->tool == Tool::Arrow) { add(Variant::Straight); add(Variant::Curved); add(Variant::DoubleHeaded); add(Variant::DoubleCurved); }
        if (variants->count()) {
            const int index = variants->findData(static_cast<int>(old));
            variants->setCurrentIndex(std::max(0, index));
            canvas->variant = static_cast<Variant>(variants->currentData().toInt());
        } else
            canvas->variant = Variant::Outline;
        variants->setEnabled(variants->count() > 0);
        variants->setVisible(variants->count() > 0);
        variants->blockSignals(false);
    }
    void updateColor() {
        color->setStyleSheet(QString("background: %1; color: %2; border: 1px solid %3; border-radius: 6px; padding: 4px 9px;")
            .arg(canvas->color.name(), canvas->color.lightness() > 155 ? "#101820" : "#ffffff", dark ? "#617086" : "#aebdcc"));
    }
    void applyTheme() {
        toolbar->setStyleSheet(dark ?
            "QFrame#annotationToolbar { background: #17212f; border: 1px solid #526277; border-radius: 12px; }"
            "QLabel { color: #b7c7da; } QToolButton, QPushButton, QComboBox, QSpinBox { color: #e1eaf5; background: #253348; border: 1px solid #3d5069; border-radius: 6px; padding: 4px 7px; }"
            "QToolButton:checked { background: #314f78; border-color: #7ab8ff; } QToolButton:hover, QPushButton:hover { background: #354964; }"
            "QToolButton:disabled, QPushButton:disabled { color: #5d6f86; } QComboBox QAbstractItemView { color: #e1eaf5; background: #253348; selection-background-color: #314f78; }" :
            "QFrame#annotationToolbar { background: #edf2f7; border: 1px solid #9caec0; border-radius: 12px; }"
            "QLabel { color: #30485f; } QToolButton, QPushButton, QComboBox, QSpinBox { color: #163047; background: #ffffff; border: 1px solid #adbdcc; border-radius: 6px; padding: 4px 7px; }"
            "QToolButton:checked { background: #c6ddf7; border-color: #407dbb; } QToolButton:hover, QPushButton:hover { background: #d8e5f2; }"
            "QToolButton:disabled, QPushButton:disabled { color: #9aa8b6; } QComboBox QAbstractItemView { color: #163047; background: #ffffff; selection-background-color: #c6ddf7; }");
        updateColor();
    }
    void retranslate() {
        for (int index = 0; index < static_cast<int>(toolButtons.size()); ++index) {
            auto* button = toolButtons[index];
            const QString name = toolName(static_cast<Tool>(index));
            static const QStringList keys = {"V", "R", "E", "A", "W", "D", "H", "T", "B", "G", "Shift+B"};
            button->setToolTip(name + " (" + keys[index] + ")");
            button->setAccessibleName(name);
        }
        widthLabel->setText(owner->tr("Width"));
        width->setToolTip(owner->tr("Width"));
        color->setText(owner->tr("Color"));
        undo->setText(owner->tr("Undo"));
        redo->setText(owner->tr("Redo"));
        clear->setText(owner->tr("Clear"));
        fit->setText(owner->tr("Fit"));
        copy->setText(owner->tr("Copy"));
        save->setText(owner->tr("Save"));
        pin->setText(owner->tr("Pin"));
        reselect->setText(owner->tr("Reselect"));
        theme->setItemText(0, owner->tr("Dark"));
        theme->setItemText(1, owner->tr("Light"));
        theme->setToolTip(owner->tr("Theme"));
        canvas->setToolTip(owner->tr("Drag to draw. Select to move or delete annotations. Wheel to zoom; Space+drag to pan."));
        updateVariants();
        toolbar->adjustSize();
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
    ui.toolbar = new QFrame(this);
    ui.toolbar->setObjectName("annotationToolbar");
    auto* rows = new QVBoxLayout(ui.toolbar);
    rows->setContentsMargins(10, 9, 10, 9);
    rows->setSpacing(6);
    auto* toolRow = new QHBoxLayout;
    toolRow->setSpacing(4);
    ui.tools = new QButtonGroup(this);
    for (int index = 0; index <= static_cast<int>(Tool::Spotlight); ++index) {
        auto* button = new QToolButton(ui.toolbar);
        button->setObjectName(QString("annotationTool%1").arg(index));
        button->setCheckable(true);
        button->setIcon(toolIcon(static_cast<Tool>(index)));
        button->setIconSize(QSize(22, 22));
        button->setFixedSize(34, 32);
        ui.tools->addButton(button, index);
        ui.toolButtons.push_back(button);
        toolRow->addWidget(button);
    }
    ui.toolButtons[static_cast<int>(Tool::Rectangle)]->setChecked(true);
    ui.variants = new QComboBox(ui.toolbar);
    ui.variants->setObjectName("annotationVariant");
    ui.variants->setMinimumWidth(128);
    toolRow->addWidget(ui.variants, 1);
    rows->addLayout(toolRow);

    auto* styleRow = new QHBoxLayout;
    styleRow->setSpacing(5);
    const QStringList palette = {"#ff5252", "#ff9b43", "#ffdc5a", "#62d889", "#62b5ff", "#ffffff", "#111111"};
    for (const auto& value : palette) {
        auto* swatch = new QToolButton(ui.toolbar);
        swatch->setObjectName("annotationColor" + value.mid(1));
        swatch->setFixedSize(21, 23);
        swatch->setStyleSheet(QString("background: %1; border: 1px solid #79889b; border-radius: 5px;").arg(value));
        swatch->setToolTip(value);
        styleRow->addWidget(swatch);
        connect(swatch, &QToolButton::clicked, this, [this, value] { m_impl->canvas->color = QColor(value); m_impl->updateColor(); m_impl->savePreferences(); });
    }
    ui.color = new QPushButton(ui.toolbar);
    ui.color->setObjectName("annotationColorPicker");
    styleRow->addWidget(ui.color);
    ui.widthLabel = new QLabel(ui.toolbar);
    styleRow->addWidget(ui.widthLabel);
    ui.width = new QSpinBox(ui.toolbar);
    ui.width->setObjectName("annotationWidth");
    ui.width->setRange(1, 24);
    ui.width->setValue(static_cast<int>(ui.canvas->strokeWidth));
    ui.width->setFixedWidth(60);
    styleRow->addWidget(ui.width);
    ui.theme = new QComboBox(ui.toolbar);
    ui.theme->setObjectName("annotationTheme");
    ui.theme->addItems({QString(), QString()});
    ui.theme->setCurrentIndex(ui.dark ? 0 : 1);
    styleRow->addWidget(ui.theme);
    styleRow->addStretch();
    rows->addLayout(styleRow);

    auto* actionRow = new QHBoxLayout;
    actionRow->setSpacing(5);
    const auto button = [&](const char* name) { auto* item = new QPushButton(ui.toolbar); item->setObjectName(name); actionRow->addWidget(item); return item; };
    ui.undo = button("annotationUndo");
    ui.redo = button("annotationRedo");
    ui.clear = button("annotationClear");
    ui.fit = button("annotationFit");
    actionRow->addStretch();
    ui.reselect = button("annotationReselect");
    ui.pin = button("annotationPin");
    ui.save = button("annotationSave");
    ui.copy = button("annotationCopy");
    rows->addLayout(actionRow);

    ui.canvas->changed = [this] {
        m_impl->undo->setEnabled(m_impl->canvas->canUndo());
        m_impl->redo->setEnabled(m_impl->canvas->canRedo());
        m_impl->clear->setEnabled(m_impl->canvas->hasAnnotations());
        emit annotationsChanged();
    };
    ui.canvas->requestText = [this](const QString& existing) {
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Text annotation"));
        auto* layout = new QVBoxLayout(&dialog);
        layout->addWidget(new QLabel(tr("Enter text (multiple lines supported):"), &dialog));
        auto* input = new QPlainTextEdit(&dialog);
        input->setPlainText(existing);
        input->setMinimumSize(320, 140);
        layout->addWidget(input);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        input->setFocus();
        return dialog.exec() == QDialog::Accepted ? input->toPlainText() : existing;
    };
    connect(ui.tools, &QButtonGroup::idClicked, this, [this](int id) { m_impl->canvas->selectTool(static_cast<Tool>(id)); m_impl->updateVariants(); QResizeEvent resize(size(), size()); resizeEvent(&resize); });
    connect(ui.variants, &QComboBox::currentIndexChanged, this, [this] { if (m_impl->variants->currentIndex() >= 0) m_impl->canvas->variant = static_cast<Variant>(m_impl->variants->currentData().toInt()); });
    connect(ui.width, &QSpinBox::valueChanged, this, [this](int value) { m_impl->canvas->strokeWidth = value; m_impl->savePreferences(); });
    connect(ui.theme, &QComboBox::currentIndexChanged, this, [this](int index) { m_impl->dark = index == 0; m_impl->applyTheme(); m_impl->savePreferences(); });
    connect(ui.color, &QPushButton::clicked, this, [this] {
        const QColor selected = QColorDialog::getColor(m_impl->canvas->color, this, tr("Color"));
        if (selected.isValid()) { m_impl->canvas->color = selected; m_impl->updateColor(); m_impl->savePreferences(); }
    });
    connect(ui.undo, &QPushButton::clicked, this, &AnnotationEditor::undo);
    connect(ui.redo, &QPushButton::clicked, this, &AnnotationEditor::redo);
    connect(ui.clear, &QPushButton::clicked, this, &AnnotationEditor::clearAnnotations);
    connect(ui.fit, &QPushButton::clicked, this, &AnnotationEditor::fitImage);
    connect(ui.copy, &QPushButton::clicked, this, &AnnotationEditor::copyRequested);
    connect(ui.save, &QPushButton::clicked, this, &AnnotationEditor::saveRequested);
    connect(ui.pin, &QPushButton::clicked, this, &AnnotationEditor::pinRequested);
    connect(ui.reselect, &QPushButton::clicked, this, &AnnotationEditor::reselectRequested);
    const auto shortcut = [this](const QKeySequence& sequence, std::function<void()> callback) {
        auto* key = new QShortcut(sequence, this);
        key->setContext(Qt::WidgetWithChildrenShortcut);
        connect(key, &QShortcut::activated, this, std::move(callback));
    };
    shortcut(QKeySequence::Undo, [this] { undo(); });
    shortcut(QKeySequence("Ctrl+Shift+Z"), [this] { redo(); });
    shortcut(QKeySequence("Ctrl+Y"), [this] { redo(); });
    shortcut(QKeySequence::Copy, [this] { emit copyRequested(); });
    shortcut(QKeySequence::Save, [this] { emit saveRequested(); });
    shortcut(QKeySequence("Ctrl+P"), [this] { emit pinRequested(); });
    shortcut(QKeySequence("Ctrl+0"), [this] { fitImage(); });
    const auto toolShortcut = [&](const QString& key, Tool tool) {
        shortcut(QKeySequence(key), [this, tool] {
            if (!QApplication::activeModalWidget())
                m_impl->toolButtons[static_cast<int>(tool)]->click();
        });
    };
    toolShortcut("V", Tool::Select);
    toolShortcut("R", Tool::Rectangle);
    toolShortcut("E", Tool::Ellipse);
    toolShortcut("A", Tool::Arrow);
    toolShortcut("W", Tool::Line);
    toolShortcut("D", Tool::Pen);
    toolShortcut("H", Tool::Highlighter);
    toolShortcut("T", Tool::Text);
    toolShortcut("B", Tool::Number);
    toolShortcut("G", Tool::Mosaic);
    toolShortcut("Shift+B", Tool::Spotlight);
    shortcut(QKeySequence("Shift+D"), [this] {
        if (QApplication::activeModalWidget())
            return;
        m_impl->toolButtons[static_cast<int>(Tool::Rectangle)]->click();
        m_impl->variants->setCurrentIndex(m_impl->variants->findData(static_cast<int>(Variant::Filled)));
    });
    ui.retranslate();
    ui.applyTheme();
    ui.undo->setEnabled(false);
    ui.redo->setEnabled(false);
    ui.clear->setEnabled(false);
}

AnnotationEditor::~AnnotationEditor() = default;
void AnnotationEditor::setImage(const QImage& image, bool preserveAnnotations) { m_impl->canvas->setImage(image, preserveAnnotations); }
QImage AnnotationEditor::resultImage() const { return m_impl->canvas->result(); }
QRect AnnotationEditor::canvasGeometry() const { return m_impl->canvas->displayRect(); }
QWidget* AnnotationEditor::toolbarWidget() const { return m_impl->toolbar; }
void AnnotationEditor::undo() { m_impl->canvas->undo(); }
void AnnotationEditor::redo() { m_impl->canvas->redo(); }
void AnnotationEditor::clearAnnotations() { m_impl->canvas->clearAnnotations(); }
void AnnotationEditor::fitImage() { m_impl->canvas->fitViewport(); }
void AnnotationEditor::setImageDisplayRect(const QRect& rect) { m_impl->canvas->setDisplayRect(rect); }
void AnnotationEditor::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    m_impl->canvas->setGeometry(rect());
    const QSize toolbarSize = m_impl->toolbar->sizeHint();
    m_impl->toolbar->setGeometry(std::max(0, (width() - toolbarSize.width()) / 2), std::max(0, height() - toolbarSize.height() - 18), toolbarSize.width(), toolbarSize.height());
    m_impl->toolbar->raise();
}
void AnnotationEditor::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange && m_impl->toolbar) {
        m_impl->retranslate();
        QResizeEvent resize(size(), size());
        resizeEvent(&resize);
    }
}
