#include "ui/hdr_shm.hpp"
#include <QPainter>
#include <cassert>
#include <cstring>
using namespace hyprcapture::ui;
static void equal(const HdrShmUpdate &update, const QImage &image) {
  assert(update.buffer);
  for (int y = 0; y < image.height(); ++y)
    assert(!std::memcmp(static_cast<const char *>(update.buffer->data) +
                            y * update.buffer->stride,
                        image.constScanLine(y), size_t(image.bytesPerLine())));
}
int main() {
  HdrShmPool pool;
  QImage image(96, 64, QImage::Format_RGB30);
  image.fill(Qt::black);
  auto a = pool.prepare(image, image.rect());
  equal(a, image);
  assert(a.allocated && a.copiedBytes == image.sizeInBytes());
  a.buffer->busy = true;
  const auto original = image.copy();
  const auto edit = [&](QRect rect, QColor color) {
    QPainter p(&image);
    p.fillRect(rect, color);
  };
  const QRect r1(3, 4, 5, 6), r2(35, 25, 7, 9), r3(70, 40, 6, 7),
      r4(9, 50, 5, 5);
  edit(r1, Qt::red);
  auto b = pool.prepare(image, r1);
  equal(b, image);
  b.buffer->busy = true;
  edit(r2, Qt::green);
  auto c = pool.prepare(image, r2);
  equal(c, image);
  c.buffer->busy = true;
  edit(r3, Qt::blue);
  assert(!pool.prepare(image, r3).buffer);
  edit(r4, Qt::white);
  assert(!pool.prepare(image, r4).buffer);
  assert(pool.allocations() == 3);
  equal(a, original); // Queued compositor readers retain immutable old pixels.
  a.buffer->busy = false;
  auto d = pool.prepare(image, {});
  equal(d, image);
  assert(!d.allocated && d.damage.contains(r3) && d.damage.contains(r4));
  assert(d.copiedBytes < image.sizeInBytes());
  d.buffer->busy = true;
  b.buffer->busy = false;
  auto e = pool.prepare(image, {});
  equal(e, image); // Catch up an older rotated slot.
  assert(e.copiedBytes < image.sizeInBytes());
  auto unchanged = pool.prepare(image, {});
  equal(unchanged, image);
  assert(unchanged.copiedBytes == 0 && unchanged.damage.isEmpty());
  auto retained = d.buffer;
  const std::weak_ptr<HdrShmBuffer> weak = retained;
  d = {};
  a = {};
  b = {};
  c = {};
  e = {};
  unchanged = {};
  pool.reset();
  assert(!weak.expired());
  retained.reset();
  assert(weak.expired());
  QImage resized(12, 8, QImage::Format_RGB30);
  resized.fill(Qt::white);
  auto fresh = pool.prepare(resized, {});
  equal(fresh, resized);
  assert(fresh.allocated && fresh.damage == QRegion(resized.rect()));
  assert(!pool.prepare(QImage(1, 1, QImage::Format_ARGB32), {}).buffer);
}
