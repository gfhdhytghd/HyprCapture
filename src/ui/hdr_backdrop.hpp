#pragma once
#include "ui/hdr_gpu.hpp"
#include <QImage>
#include <QRect>
#include <chrono>
#include <functional>
#include <memory>
class QWindow;
namespace hyprcapture::ui {
// A color-managed child surface beneath the transparent Qt controls. Qt's
// raster backing store never receives the HDR pixels.
class HdrBackdrop {
public:
  explicit HdrBackdrop(QWindow *parent, bool aboveParent = false,
                       bool synchronized = false);
  // Call only for an HDR overlay; retain the lease through construction.
  static std::shared_ptr<void> prewarm();
  static bool fp16Available();
  bool setPreparedImage(const std::shared_ptr<const HdrGpuFrame> &,
                        const QRect &);
  ~HdrBackdrop();
  // True accepts the request; FP16 and PQ complete asynchronously.
  bool setImage(const QImage &image, const QRect &destination,
                float referenceNits);
  bool setPqImage(const QImage &image, const QRect &destination, float peakNits,
                  float referenceNits);
  void setGeometry(const QRect &destination);
  void setVisible(bool visible);
  bool ready() const;
  // Pending PQ requests complete after globals/description events arrive.
  void setReadyCallback(std::function<void(bool)> callback);
#ifdef HYPRCAPTURE_HDR_TESTING
  static unsigned globalsRequestsForTest();
  void setReadyForTest(bool ready);
  static std::unique_ptr<HdrBackdrop> unavailableForTest(QWindow *parent);
#endif

private:
  struct Impl;
  std::unique_ptr<Impl> m;
};
QImage hdrToPq10(const QImage &image, float *peakNits = nullptr,
                 float referenceNits = 203, const QPoint &origin = {});
} // namespace hyprcapture::ui
