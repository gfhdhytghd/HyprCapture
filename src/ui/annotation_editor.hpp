#pragma once

#include <QImage>
#include <QRect>
#include <QWidget>
#include <memory>
#include "ui/scroll_stitcher.hpp"

struct AnnotationSnapshot;

// An image editor embedded in the capture overlay. Annotation coordinates are
// native image pixels, independent of display scale, zoom and the export path.
class AnnotationEditor final : public QWidget {
    Q_OBJECT

  public:
    explicit AnnotationEditor(QWidget* parent = nullptr);
    ~AnnotationEditor() override;

    // Existing edits and history survive background changes of the same size.
    void setImage(const QImage& image, bool preserveAnnotations = false);
    QImage resultImage() const;
    void setRegionResizeBounds(const QRect& bounds);
    void replaceCaptureImage(const QImage& image, const QRect& displayRect);

    std::shared_ptr<const AnnotationSnapshot> snapshot() const;
    void restore(const std::shared_ptr<const AnnotationSnapshot>& snapshot);
    // Original object coordinates and history remain immutable as the canvas grows.
    void setScrollImage(const QImage& image, const hyprcapture::ui::ScrollLayout& layout, bool live);
    void setScrollEnabled(bool enabled);
    void setAnnotationPresentation(bool ready, qreal opacity = 1, qreal offset = 0);
    QImage annotatedPreview(const QImage& preview, const hyprcapture::ui::ScrollLayout& layout) const;
    QRect canvasGeometry() const;
    QWidget* toolbarWidget() const;
    QRect toolbarGeometry() const;
    // The capture controls remain parented by the overlay; reserve and return
    // their position as part of the same floating toolbar cluster.
    void setCaptureToolbarSize(const QSize& size);
    QRect captureToolbarGeometry() const;
    void undo();
    void redo();
    void clearAnnotations();
    void fitImage();
    // Empty rect restores automatic fit. Coordinates are relative to this widget.
    void setImageDisplayRect(const QRect& rect);

  signals:
    void scrollRequested(const QPointF& globalPosition, double delta, int discrete, bool finger, bool inverted);
    void confirmRequested();
    void cancelRequested();
    void pinRequested();
    void reselectRequested();
    void annotationsChanged();
    void captureRectChangeRequested(const QRect& rect);
    void toolbarGeometryChanged(const QRect& geometry);

  protected:
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void hideEvent(QHideEvent* event) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
