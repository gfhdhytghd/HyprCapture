#pragma once

#include <QImage>
#include <QRect>
#include <QWidget>
#include <memory>

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
    QRect canvasGeometry() const;
    QWidget* toolbarWidget() const;
    void undo();
    void redo();
    void clearAnnotations();
    void fitImage();
    // Empty rect restores automatic fit. Coordinates are relative to this widget.
    void setImageDisplayRect(const QRect& rect);

  signals:
    void copyRequested();
    void saveRequested();
    void pinRequested();
    void reselectRequested();
    void annotationsChanged();

  protected:
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
