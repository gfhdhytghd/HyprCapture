#pragma once
#include <QImage>
#include <QRect>
#include <memory>
class QWindow;
namespace hyprcapture::ui {
// A color-managed child surface beneath the transparent Qt controls. Qt's
// raster backing store never receives the HDR pixels.
class HdrBackdrop {
public:
  explicit HdrBackdrop(QWindow *parent, bool aboveParent = false);
  ~HdrBackdrop();
  bool setImage(const QImage &image, const QRect &destination,
                float referenceNits);
  bool setPqImage(const QImage &image, const QRect &destination,
                  float peakNits, float referenceNits);
  void setGeometry(const QRect &destination);
  void setVisible(bool visible);
  bool ready() const;

private:
  struct Impl;
  std::unique_ptr<Impl> m;
};
QImage hdrToPq10(const QImage &image, float *peakNits = nullptr,
                 float referenceNits = 203);
} // namespace hyprcapture::ui
