#pragma once
#include <QImage>
#include <QRegion>
#include <array>
#include <memory>

namespace hyprcapture::ui {
// UI-thread owned. A Wayland buffer retains this mapping until release, even
// after its surface/pool is destroyed. Busy storage must never be overwritten.
struct HdrShmBuffer {
  ~HdrShmBuffer();
  int fd = -1;
  void *data = nullptr;
  QSize size;
  qsizetype stride = 0, bytes = 0;
  bool busy = false;
  QRegion stale;
};
struct HdrShmUpdate {
  std::shared_ptr<HdrShmBuffer> buffer;
  QRegion damage;
  qsizetype copiedBytes = 0;
  bool allocated = false;
  bool blocked = false;
};
class HdrShmPool {
public:
  // Null buffer means all three are still in flight (or allocation failed).
  // Damage accumulates across dropped frames and across each slot's age.
  HdrShmUpdate prepare(const QImage &rgb30, const QRegion &damage);
  void reset();
  unsigned allocations() const { return m_allocations; }

private:
  std::array<std::shared_ptr<HdrShmBuffer>, 3> m_buffers;
  QSize m_size;
  qint64 m_imageKey = 0;
  QRegion m_pendingDamage;
  unsigned m_allocations = 0;
};
} // namespace hyprcapture::ui
