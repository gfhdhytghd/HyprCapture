#include "ui/annotation_editor.hpp"
#include "ui/hdr_backdrop.hpp"
#include "ui/hdr_shm.hpp"
#include <QApplication>
#include <QColorSpace>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QSlider>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>
#include <QToolButton>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

template <class F> double median(F run) {
  std::vector<double> times;
  for (int i = 0; i < 5; ++i) {
    QElapsedTimer clock;
    clock.start();
    run();
    times.push_back(clock.nsecsElapsed() / 1e6);
  }
  std::sort(times.begin(), times.end());
  return times[times.size() / 2];
}
static int interactionBenchmark() {
  AnnotationEditor editor;
  editor.resize(3072, 1728);
  QImage sdr(6144, 3456, QImage::Format_ARGB32_Premultiplied);
  sdr.fill(QColor(110, 120, 130));
  QImage hdr(sdr.size(), QImage::Format_RGBA16FPx4_Premultiplied);
  hdr.fill(Qt::white);
  editor.setHdrEnabled(true);
  editor.setImage(sdr);
  editor.setImageDisplayRect({0, 0, 3072, 1728});
  editor.show();
  QApplication::processEvents();
  editor.setExternalImagePresentation(true, true);
  auto *canvas = editor.findChild<QWidget *>("annotationCanvas");
  QTest::mouseClick(editor.findChild<QToolButton *>("annotationTool5"),
                    Qt::LeftButton);
  editor.pqPreviewRendererOn(hdr,
                             173)(nullptr); // Warm immutable base conversion.
  QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, {71, 93});
  puts("frame,worker_queue_us,render_convert_us,main_queue_us,allocate_map_us,"
       "copy_us,unmap_close_us,cpu_total_us");
  for (int frame = 0; frame < 12; ++frame) {
    QElapsedTimer clock;
    clock.start();
    QTest::mouseMove(canvas, {110 + frame * 2, 125 + frame * 2}, 0);
    auto render = editor.pqPreviewRendererOn(hdr, 173);
    const auto queued = clock.nsecsElapsed();
    QEventLoop loop;
    QThreadPool::globalInstance()->start([&, render, queued, frame] {
      const auto began = clock.nsecsElapsed();
      auto pq = render(nullptr);
      const auto rendered = clock.nsecsElapsed();
      QMetaObject::invokeMethod(
          &loop,
          [&, pq, began, rendered, queued, frame] {
            const auto received = clock.nsecsElapsed();
            const int fd =
                memfd_create("hyprcapture-benchmark-legacy", MFD_CLOEXEC);
            if (fd < 0 || ftruncate(fd, pq.sizeInBytes()) < 0)
              std::abort();
            void *data = mmap(nullptr, pq.sizeInBytes(), PROT_READ | PROT_WRITE,
                              MAP_SHARED, fd, 0);
            if (data == MAP_FAILED)
              std::abort();
            const auto allocated = clock.nsecsElapsed();
            std::memcpy(data, pq.constBits(), size_t(pq.sizeInBytes()));
            const auto copied = clock.nsecsElapsed();
            munmap(data, pq.sizeInBytes());
            close(fd);
            const auto finished = clock.nsecsElapsed();
            printf("%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n", frame,
                   (began - queued) / 1e3, (rendered - began) / 1e3,
                   (received - rendered) / 1e3, (allocated - received) / 1e3,
                   (copied - allocated) / 1e3, (finished - copied) / 1e3,
                   finished / 1e3);
            loop.quit();
          },
          Qt::QueuedConnection);
    });
    loop.exec();
  }
  puts("CPU submission only; includes real legacy memfd "
       "allocation/map/full-copy/unmap. No native Wayland commit or "
       "presentation.");
  return 0;
}
static int layeredBenchmark(bool fallback, bool sdrMode = false,
                            bool spotlight = false) {
  AnnotationEditor editor;
  editor.resize(3072, 1728);
  QImage sdr(6144, 3456, QImage::Format_ARGB32_Premultiplied);
  sdr.fill(QColor(110, 120, 130));
  QImage hdr(sdr.size(), QImage::Format_RGBA16FPx4_Premultiplied);
  hdr.fill(Qt::white);
  editor.setHdrEnabled(!sdrMode);
  editor.setImage(sdr);
  editor.setImageDisplayRect({0, 0, 3072, 1728});
  editor.show();
  QApplication::processEvents();
  editor.setExternalImagePresentation(!sdrMode, fallback);
  auto *canvas = editor.findChild<QWidget *>("annotationCanvas");
  editor
      .findChild<QToolButton *>(spotlight ? "annotationTool10"
                                          : "annotationTool5")
      ->click();
  editor.findChild<QSlider *>("annotationHdrGain")
      ->setValue(fallback ? 400 : 100);
  hyprcapture::ui::HdrShmPool pool;
  std::vector<std::shared_ptr<hyprcapture::ui::HdrShmBuffer>> inFlight;
  if (fallback) {
    auto first = editor.pqPreviewFrameRendererOn(hdr, 173)();
    QElapsedTimer startup;
    startup.start();
    for (int i = 0; i < 3; ++i) {
      auto slot = pool.prepare(first.pixels, first.damage);
      if (!slot.buffer)
        return 1;
      slot.buffer->busy = true;
      inFlight.push_back(slot.buffer);
    }
    fprintf(stderr, "cold_three_buffer_allocate_map_copy_us=%.3f bytes=%lld\n",
            startup.nsecsElapsed() / 1e3,
            static_cast<long long>(first.pixels.sizeInBytes()) * 3);
    for (auto &b : inFlight)
      b->busy = false;
    inFlight.clear();
  }
  QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, {71, 93});
  puts("frame,worker_queue_us,render_convert_us,main_queue_us,pool_prepare_us,"
       "copied_bytes,allocated,cpu_total_us");
  for (int frame = 0; frame < 12; ++frame) {
    if (frame >= 2 && fallback)
      inFlight[size_t(frame - 2)]->busy = false;
    QElapsedTimer clock;
    clock.start();
    QTest::mouseMove(canvas, {110 + frame * 2, 125 + frame * 2}, 0);
    if (!fallback) {
      if (editor.needsHdrAnnotationComposition())
        return 1;
      QApplication::sendPostedEvents(nullptr, QEvent::UpdateRequest);
      QApplication::processEvents();
      printf("%d,0,0,0,0,0,0,%.3f\n", frame, clock.nsecsElapsed() / 1e3);
      continue;
    }
    auto render = editor.pqPreviewFrameRendererOn(hdr, 173);
    const auto queued = clock.nsecsElapsed();
    QEventLoop loop;
    QThreadPool::globalInstance()->start([&, render, queued, frame] {
      const auto began = clock.nsecsElapsed();
      auto result = render();
      const auto rendered = clock.nsecsElapsed();
      QMetaObject::invokeMethod(
          &loop,
          [&, result, began, rendered, queued, frame] {
            const auto received = clock.nsecsElapsed();
            auto update = pool.prepare(result.pixels, result.damage);
            const auto finished = clock.nsecsElapsed();
            if (!update.buffer)
              std::abort();
            update.buffer->busy = true;
            inFlight.push_back(update.buffer);
            printf("%d,%.3f,%.3f,%.3f,%.3f,%lld,%d,%.3f\n", frame,
                   (began - queued) / 1e3, (rendered - began) / 1e3,
                   (received - rendered) / 1e3, (finished - received) / 1e3,
                   static_cast<long long>(update.copiedBytes), update.allocated,
                   finished / 1e3);
            loop.quit();
          },
          Qt::QueuedConnection);
    });
    loop.exec();
  }
  puts("Offscreen CPU submission only; native commit/flush/presentation "
       "require updated helper acceptance. Pool uses actual production memfd "
       "mappings and simulated two-frame releases.");
  return 0;
}
int main(int argc, char **argv) {
  QTemporaryDir config;
  qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
  QApplication app(argc, argv);
  if (argc > 1 && QByteArray(argv[1]) == "--interaction")
    return interactionBenchmark();
  if (argc > 1 && QByteArray(argv[1]) == "--layered")
    return layeredBenchmark(false);
  if (argc > 1 && QByteArray(argv[1]) == "--hdr-fallback")
    return layeredBenchmark(true);
  if (argc > 1 && QByteArray(argv[1]) == "--sdr")
    return layeredBenchmark(false, true);
  if (argc > 1 && QByteArray(argv[1]) == "--spotlight")
    return layeredBenchmark(false, false, true);
  AnnotationEditor editor;
  editor.resize(3072, 1728);
  QImage sdr(6144, 3456, QImage::Format_ARGB32_Premultiplied);
  sdr.fill(QColor(110, 120, 130));
  QImage hdr(sdr.size(), QImage::Format_RGBA16FPx4_Premultiplied);
  hdr.fill(QColor::fromRgbF(0.75, 0.5, 0.25));
  hdr.setColorSpace(QColorSpace::SRgbLinear);
  editor.setHdrEnabled(true);
  editor.setImage(sdr);
  editor.setImageDisplayRect({0, 0, 3072, 1728});
  editor.show();
  app.processEvents();
  editor.setExternalImagePresentation(true, true);
  auto *canvas = editor.findChild<QWidget *>("annotationCanvas");
  QImage result, pq;
  float peak = 0;
  std::vector<unsigned char> upload(size_t(hdr.width()) * hdr.height() * 4);
  QImage raster(editor.size(), QImage::Format_ARGB32_Premultiplied);
  puts("6144x3456 FP16, median of 5, milliseconds; upload is memcpy only (no "
       "Wayland/GPU)");
  for (int count : {0, 1, 5}) {
    if (count) {
      auto *tool = editor.findChild<QToolButton *>("annotationTool5");
      QTest::mouseClick(tool, Qt::LeftButton);
      for (int i = count == 1 ? 0 : 1; i < count; ++i) {
        const QPoint from(40 + 60 * i, 70 + 35 * i), to = from + QPoint(40, 30);
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, from);
        QTest::mouseMove(canvas, to);
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, to);
      }
    }
    const double compose =
        median([&] { result = editor.previewRendererOn(hdr)(); });
    const double convert =
        median([&] { pq = hyprcapture::ui::hdrToPq10(result, &peak, 173); });
    const double copy = median([&] {
      std::memcpy(upload.data(), pq.constBits(), size_t(pq.sizeInBytes()));
    });
    const double sdrTime = median([&] { result = editor.resultImage(); });
    editor.setExternalImagePresentation(false);
    const double sdrPaint = median([&] { canvas->render(&raster); });
    editor.setExternalImagePresentation(true, true);
    printf("annotations=%d compose=%.3f pq=%.3f copy=%.3f total=%.3f "
           "sdr_export=%.3f sdr_paint_3072x1728=%.3f\n",
           count, compose, convert, copy, compose + convert + copy, sdrTime,
           sdrPaint);
    const double cold = median([&] {
      // New base identity deliberately invalidates the cache.
      QImage fresh = hdr.copy();
      pq = editor.pqPreviewRendererOn(fresh, 173)(&peak);
    });
    pq = editor.pqPreviewRendererOn(hdr, 173)(&peak);
    const double unchanged =
        median([&] { pq = editor.pqPreviewRendererOn(hdr, 173)(&peak); });
    // One changing draft over the existing committed objects. Time the same
    // snapshot through both paths, including the modeled upload memcpy.
    auto *tool = editor.findChild<QToolButton *>("annotationTool5");
    QTest::mouseClick(tool, Qt::LeftButton);
    QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, QPoint(71, 93));
    std::vector<double> oldFrames, newFrames;
    for (int frame = 0; frame < 5; ++frame) {
      QTest::mouseMove(canvas, QPoint(110 + frame * 2, 125 + frame * 2));
      QElapsedTimer clock;
      clock.start();
      result = editor.previewRendererOn(hdr)();
      pq = hyprcapture::ui::hdrToPq10(result, &peak, 173);
      std::memcpy(upload.data(), pq.constBits(), size_t(pq.sizeInBytes()));
      oldFrames.push_back(clock.nsecsElapsed() / 1e6);
      clock.restart();
      pq = editor.pqPreviewRendererOn(hdr, 173)(&peak);
      std::memcpy(upload.data(), pq.constBits(), size_t(pq.sizeInBytes()));
      newFrames.push_back(clock.nsecsElapsed() / 1e6);
    }
    QTest::keyClick(canvas, Qt::Key_Escape);
    std::sort(oldFrames.begin(), oldFrames.end());
    std::sort(newFrames.begin(), newFrames.end());
    printf("committed=%d plus_moving_draft old_frame=%.3f new_frame=%.3f "
           "unchanged_pq=%.3f cold_with_base_copy=%.3f\n",
           count, oldFrames[2], newFrames[2], unchanged, cold);
    fflush(stdout);
  }
}
