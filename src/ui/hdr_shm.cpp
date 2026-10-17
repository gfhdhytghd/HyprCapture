#include "ui/hdr_shm.hpp"
#include "ui/timing.hpp"
#include <climits>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace hyprcapture::ui {
HdrShmBuffer::~HdrShmBuffer() {
  if (data)
    munmap(data, size_t(bytes));
  if (fd >= 0)
    close(fd);
}
void HdrShmPool::reset() {
  m_buffers = {};
  m_size = {};
  m_imageKey = 0;
  m_pendingDamage = {};
}
HdrShmUpdate HdrShmPool::prepare(const QImage &image, const QRegion &damage) {
  if (image.isNull() || image.format() != QImage::Format_RGB30 ||
      image.sizeInBytes() > INT32_MAX)
    return {};
  if (m_size != image.size()) {
    reset();
    m_size = image.size();
    m_pendingDamage = image.rect();
  }
  if (m_imageKey != image.cacheKey()) {
    const QRegion changed = damage.intersected(image.rect());
    m_pendingDamage |= changed;
    for (auto &b : m_buffers)
      if (b)
        b->stale |= changed;
    m_imageKey = image.cacheKey();
  }
  auto slot = m_buffers.end();
  for (auto i = m_buffers.begin(); i != m_buffers.end(); ++i)
    if (*i && !(*i)->busy) {
      slot = i;
      break;
    }
  if (slot == m_buffers.end())
    for (auto i = m_buffers.begin(); i != m_buffers.end(); ++i)
      if (!*i) {
        slot = i;
        break;
      }
  if (slot == m_buffers.end())
    return {.blocked = true};
  HdrShmUpdate update;
  if (!*slot) {
    ScopedUiTiming timing("hdr.shm_allocate_map");
    auto b = std::make_shared<HdrShmBuffer>();
    b->size = image.size();
    b->stride = image.bytesPerLine();
    b->bytes = image.sizeInBytes();
    b->fd = memfd_create("hyprcapture-hdr-preview", MFD_CLOEXEC);
    if (b->fd < 0 || ftruncate(b->fd, b->bytes) < 0)
      return {};
    b->data = mmap(nullptr, size_t(b->bytes), PROT_READ | PROT_WRITE,
                   MAP_SHARED, b->fd, 0);
    if (b->data == MAP_FAILED) {
      b->data = nullptr;
      return {};
    }
    b->stale = image.rect();
    *slot = b;
    ++m_allocations;
    update.allocated = true;
  }
  auto &b = **slot;
  // Keep damage bookkeeping bounded for long gestures with many dropped frames.
  const QRegion stale =
      b.stale.rectCount() > 64 ? QRegion(b.stale.boundingRect()) : b.stale;
  {
    ScopedUiTiming timing("hdr.shm_copy_damage");
    for (const auto &rect : stale) {
      const qsizetype bytes = qsizetype(rect.width()) * 4;
      for (int y = rect.top(); y <= rect.bottom(); ++y)
        std::memcpy(static_cast<char *>(b.data) + y * b.stride + rect.x() * 4,
                    image.constScanLine(y) + rect.x() * 4, size_t(bytes));
      update.copiedBytes += bytes * rect.height();
    }
  }
  b.stale = {};
  update.buffer = *slot;
  update.damage = m_pendingDamage;
  m_pendingDamage = {};
  return update;
}
} // namespace hyprcapture::ui
