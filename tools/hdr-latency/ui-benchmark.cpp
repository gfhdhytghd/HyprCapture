#include "shared/hdr_color.hpp"
#include "ui/annotation_editor.hpp"
#include "ui/hdr_backdrop.hpp"
#include "ui/hdr_image.hpp"
#include "ui/mapped_image.hpp"
#include "ui/scroll_stitcher.hpp"
#include <QApplication>
#include <QColorSpace>
#include <QFile>
#include <QPainter>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <opencv2/core.hpp>
#include <opencv2/core/ocl.hpp>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>
using namespace hyprcapture;
using Clock = std::chrono::steady_clock;
static void publish(const unsigned char *data, size_t bytes) {
  int fd =
      memfd_create("scroll-publish-bench", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (fd < 0)
    std::abort();
  size_t offset = 0;
  while (offset < bytes) {
    auto n = write(fd, data + offset, bytes - offset);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      std::abort();
    offset += n;
  }
  if (fcntl(fd, F_ADD_SEALS,
            F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL))
    std::abort();
  close(fd);
}
template <class F>
void measure(const char *name, int w, int h, int trial, F f) {
  auto t = Clock::now();
  f();
  printf("%dx%d,%d,%s,%.3f\n", w, h, trial, name,
         std::chrono::duration<double, std::milli>(Clock::now() - t).count());
  fflush(stdout);
}
int main(int argc, char **argv) {
  QApplication app(argc, argv);
  cv::setNumThreads(1);
  cv::ocl::setUseOpenCL(false);
  puts("size,trial,event,ms");
  for (auto size : {QSize(2880, 1800), QSize(6144, 3456)}) {
    int w = size.width(), h = size.height();
    QImage hdr(size, QImage::Format_RGBA16FPx4);
    hdr.setColorSpace(QColorSpace::SRgbLinear);
    QImage sdr(size, QImage::Format_RGBA8888_Premultiplied);
    unsigned seed = 42;
    for (int y = 0; y < h; ++y) {
      auto *r = (uint16_t *)hdr.scanLine(y);
      auto *b = sdr.scanLine(y);
      for (int x = 0; x < w; ++x) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        for (int c = 0; c < 3; ++c) {
          auto v = (seed >> (c * 8)) & 255;
          b[x * 4 + c] = v;
          r[x * 4 + c] = color::floatToHalf(float(v) / 32 - 0.5F);
        }
        b[x * 4 + 3] = 255;
        r[x * 4 + 3] = color::floatToHalf(1);
      }
    }
    // Reproducible textured scrolling frame; all allocations outside measured
    // stitching.
    auto shifted = sdr.copy();
    {
      QPainter p(&shifted);
      p.drawImage(0, -h / 4, sdr);
    }
    QImage hdrPlane = hdr;
    hdrPlane.reinterpretAsFormat(QImage::Format_RGBA16FPx4_Premultiplied);
    auto shiftedHdr = hdrPlane.copy();
    {
      QPainter p(&shiftedHdr);
      p.drawImage(0, -h / 4, hdrPlane);
    }
    int transportFd =
        memfd_create("scroll-bench", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (transportFd < 0 ||
        ftruncate(transportFd, sdr.sizeInBytes() + hdr.sizeInBytes()))
      std::abort();
    if (write(transportFd, sdr.constBits(), sdr.sizeInBytes()) !=
            sdr.sizeInBytes() ||
        write(transportFd, hdr.constBits(), hdr.sizeInBytes()) !=
            hdr.sizeInBytes())
      std::abort();
    if (fcntl(transportFd, F_ADD_SEALS,
              F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL))
      std::abort();
    std::vector<unsigned char> payload(sdr.sizeInBytes() + hdr.sizeInBytes());
    memcpy(payload.data(), sdr.constBits(), sdr.sizeInBytes());
    memcpy(payload.data() + sdr.sizeInBytes(), hdr.constBits(),
           hdr.sizeInBytes());
    for (int trial = 0; trial < 5; ++trial) {
      QImage pq, preview, out;
      measure("SDR_memfd_write_seal", w, h, trial,
              [&] { publish(payload.data(), sdr.sizeInBytes()); });
      measure("HDR_memfd_write_seal", w, h, trial,
              [&] { publish(payload.data(), payload.size()); });
      if (qint64(w) * h * 12 <= 128LL * 1024 * 1024)
        measure("HDR_map_planes", w, h, trial, [&] {
          QFile f;
          if (!f.open(transportFd, QIODevice::ReadOnly,
                      QFileDevice::DontCloseHandle))
            std::abort();
          auto planes = ui::mapScrollSdrHdr(f, w, h);
          if (planes.first.isNull() || planes.second.isNull())
            std::abort();
        });
      measure("hdrToPq10", w, h, trial, [&] { pq = ui::hdrToPq10(hdr); });
      measure("hdrToSdr", w, h, trial, [&] { out = ui::hdrToSdr(hdr); });
      measure("FP16_QPainter_base", w, h, trial, [&] {
        QImage base(size, QImage::Format_RGBA16FPx4);
        base.fill(Qt::transparent);
        QPainter p(&base);
        p.drawImage(0, 0, hdr);
        out = std::move(base);
      });
      measure("pq_memfd_copy", w, h, trial, [&] {
        int fd = memfd_create("hdr-bench", MFD_CLOEXEC);
        if (fd < 0 || ftruncate(fd, pq.sizeInBytes()))
          std::abort();
        auto *ptr = mmap(nullptr, pq.sizeInBytes(), PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, 0);
        if (ptr == MAP_FAILED)
          std::abort();
        memcpy(ptr, pq.constBits(), pq.sizeInBytes());
        munmap(ptr, pq.sizeInBytes());
        close(fd);
      });
      measure("fp16_memfd_copy", w, h, trial, [&] {
        int fd = memfd_create("hdr-bench", MFD_CLOEXEC);
        if (fd < 0 || ftruncate(fd, hdr.sizeInBytes()))
          std::abort();
        auto *ptr = mmap(nullptr, hdr.sizeInBytes(), PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, 0);
        if (ptr == MAP_FAILED)
          std::abort();
        memcpy(ptr, hdr.constBits(), hdr.sizeInBytes());
        munmap(ptr, hdr.sizeInBytes());
        close(fd);
      });
      for (bool withHdr : {false, true}) {
        ui::ScrollStitcher::Limits
            limits; // 6K is deliberately offline: exceeds live HDR 16M and
                    // transport 128MiB limits.
        ui::ScrollStitcher stitcher(limits);
        if (withHdr && qint64(w) * h > 16LL * 1024 * 1024) {
          auto result = stitcher.append(sdr, {}, hdrPlane);
          if (result.status != ui::ScrollStitcher::Status::LimitReached)
            std::abort();
          fprintf(stderr,
                  "%dx%d HDR stitch baseline rejected by existing 16M pixel "
                  "limit (expected)\n",
                  w, h);
          continue;
        }
        measure(withHdr ? "HDR_baseline_append" : "SDR_baseline_append", w, h,
                trial, [&] {
                  auto result =
                      stitcher.append(sdr, {}, withHdr ? hdrPlane : QImage{});
                  if (result.status != ui::ScrollStitcher::Status::Started)
                    std::abort();
                });
        measure(withHdr ? "HDR_baseline_previews" : "SDR_baseline_previews", w,
                h, trial, [&] {
                  preview = stitcher.preview({720, 480});
                  out = stitcher.preview({720, 480}, true);
                });
        if (withHdr)
          measure("HDR_thumbnail_PQ", w, h, trial, [&] {
            auto thumbnail = ui::hdrToPq10(out);
            if (thumbnail.isNull())
              std::abort();
          });
        measure(withHdr ? "HDR_shifted_append" : "SDR_shifted_append", w, h,
                trial, [&] {
                  auto result = stitcher.append(
                      shifted, {}, withHdr ? shiftedHdr : QImage{});
                  if (result.status != ui::ScrollStitcher::Status::Appended) {
                    fprintf(stderr, "unexpected stitch status %d\n",
                            int(result.status));
                    std::abort();
                  }
                });
        measure(withHdr ? "HDR_shifted_previews" : "SDR_shifted_previews", w, h,
                trial, [&] {
                  preview = stitcher.preview({720, 480});
                  out = stitcher.preview({720, 480}, true);
                });
      }
      AnnotationEditor editor;
      editor.resize(1440, 900);
      editor.setImage(sdr);
      editor.setHdrEnabled(true);
      measure("editor_hdr_renderer", w, h, trial, [&] {
        auto renderer = editor.previewRendererOn(hdr);
        out = renderer();
      });
      ui::ScrollLayout layout{.content = QRect(QPoint{}, size),
                              .frameSize = size};
      measure("editor_live_install", w, h, trial, [&] {
        auto snapshot = editor.snapshot();
        editor.replaceCaptureImage(sdr, QRect(0, 0, w / 2, h / 2));
        editor.setScrollImage(sdr, layout, true);
        editor.setLiveScrollPresentation(true);
      });
    }
    close(transportFd);
  }
}
