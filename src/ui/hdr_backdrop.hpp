#pragma once
#include "ui/hdr_gpu.hpp"
#include <QImage>
#include <QRect>
#include <QRegion>
#include <optional>
#include <chrono>
#include <functional>
#include <memory>
class QWindow;
namespace hyprcapture::ui {
struct HdrPresentationLuminances {
  uint32_t encodingMaximum = 0, reference = 0, masteringMaximum = 0;
};
// Keep pixel encoding units separate from the captured content's peak.
HdrPresentationLuminances hdrPresentationLuminances(bool fp16, float peakNits,
                                                   float referenceNits);
// A color-managed child surface beneath the transparent Qt controls. Qt's
// raster backing store never receives the HDR pixels.
class HdrBackdrop {
public:
  explicit HdrBackdrop(QWindow *parent, bool aboveParent = false,
                       bool synchronized = false);
  // Call only for an HDR overlay; retain the lease through construction.
  static std::shared_ptr<void> prewarm();
  static bool fp16Available();
  // Optional timing probe on Qt's parent surface; no new surface or pixels.
  static void traceParentFrame(QWindow* parent);
  bool setPreparedImage(const std::shared_ptr<const HdrGpuFrame> &,
                        const QRect &);
  ~HdrBackdrop();
  // True accepts the request; FP16 and PQ complete asynchronously.
  bool setImage(const QImage &image, const QRect &destination,
                float referenceNits);
  bool setPqImage(const QImage &image, const QRect &destination, float peakNits,
                  float referenceNits, std::optional<QRegion> damage = std::nullopt);
  void setGeometry(const QRect &destination);
  // Sibling ordering is applied with the next parent commit.
  void stackAbove(const HdrBackdrop& lower);
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
