#pragma once
#include <QImage>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
namespace hyprcapture::ui {
// Opaque linear BT.2020 pixels; 1.0 = 80 nits, matching PQ fallback units.
// Immutable upload result. GPU storage is recycled only after the consumer
// drops it (the Wayland consumer must first observe wl_buffer.release).
struct HdrGpuFrame {
  int fd = -1, width = 0, height = 0, stride = 0, offset = 0;
  uint64_t modifier = 0;
  float peakNits = 0, referenceNits = 203;
  std::shared_ptr<void> storage;
  ~HdrGpuFrame();
};
class HdrUpload {
public:
  enum class State { Pending, Ready, Failed };
  static void prewarm();
  static void setMainDevice(uint64_t device);
  static int openRenderNode();
  static std::shared_ptr<HdrUpload> request(const QImage &,
                                            float referenceNits);
  State state() const;
  std::shared_ptr<const HdrGpuFrame> result() const;
  bool waitUntil(std::chrono::steady_clock::time_point) const;
#ifdef HYPRCAPTURE_HDR_TESTING
  // Deterministic stalled-driver injection, compiled only in test builds.
  static std::shared_ptr<HdrUpload> pendingForTest(const QImage &);
  void failForTest();
  static bool rendererStartedForTest();
  static bool gpuSupportedForTest();
  static void forceNonLinearForTest();
  static bool allocationFallbackForTest();
  static unsigned nonLinearFallbacksForTest();
#endif
  struct Impl;

private:
  explicit HdrUpload(std::shared_ptr<Impl>);
  std::shared_ptr<Impl> m;
};
} // namespace hyprcapture::ui
