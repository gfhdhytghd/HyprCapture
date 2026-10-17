#include "ui/annotation_editor.hpp"
#include "ui/hdr_backdrop.hpp"
#include <QApplication>
#include <QColorSpace>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

template<class F> double median(F run) {
    std::vector<double> times;
    for (int i = 0; i < 5; ++i) {
        QElapsedTimer clock; clock.start(); run();
        times.push_back(clock.nsecsElapsed() / 1e6);
    }
    std::sort(times.begin(), times.end());
    return times[times.size() / 2];
}
int main(int argc, char** argv) {
    QTemporaryDir config;
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    QApplication app(argc, argv);
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
    editor.show(); app.processEvents();
    editor.setExternalImagePresentation(true, true);
    auto* canvas = editor.findChild<QWidget*>("annotationCanvas");
    QImage result, pq;
    float peak = 0;
    std::vector<unsigned char> upload(size_t(hdr.width()) * hdr.height() * 4);
    QImage raster(editor.size(), QImage::Format_ARGB32_Premultiplied);
    puts("6144x3456 FP16, median of 5, milliseconds; upload is memcpy only (no Wayland/GPU)");
    for (int count : {0, 1, 5}) {
        if (count) {
            auto* tool = editor.findChild<QToolButton*>("annotationTool5");
            QTest::mouseClick(tool, Qt::LeftButton);
            for (int i = count == 1 ? 0 : 1; i < count; ++i) {
                const QPoint from(40 + 60 * i, 70 + 35 * i), to = from + QPoint(40, 30);
                QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, from);
                QTest::mouseMove(canvas, to);
                QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, to);
            }
        }
        const double compose = median([&] { result = editor.previewRendererOn(hdr)(); });
        const double convert = median([&] { pq = hyprcapture::ui::hdrToPq10(result, &peak, 173); });
        const double copy = median([&] { std::memcpy(upload.data(), pq.constBits(), size_t(pq.sizeInBytes())); });
        const double sdrTime = median([&] { result = editor.resultImage(); });
        editor.setExternalImagePresentation(false);
        const double sdrPaint = median([&] { canvas->render(&raster); });
        editor.setExternalImagePresentation(true, true);
        printf("annotations=%d compose=%.3f pq=%.3f copy=%.3f total=%.3f sdr_export=%.3f sdr_paint_3072x1728=%.3f\n",
               count, compose, convert, copy, compose + convert + copy, sdrTime, sdrPaint);
        const double cold = median([&] {
            // New base identity deliberately invalidates the cache.
            QImage fresh = hdr.copy();
            pq = editor.pqPreviewRendererOn(fresh, 173)(&peak);
        });
        pq = editor.pqPreviewRendererOn(hdr, 173)(&peak);
        const double unchanged = median([&] { pq = editor.pqPreviewRendererOn(hdr, 173)(&peak); });
        // One changing draft over the existing committed objects. Time the same
        // snapshot through both paths, including the modeled upload memcpy.
        auto* tool = editor.findChild<QToolButton*>("annotationTool5");
        QTest::mouseClick(tool, Qt::LeftButton);
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, QPoint(71, 93));
        std::vector<double> oldFrames, newFrames;
        for (int frame = 0; frame < 5; ++frame) {
            QTest::mouseMove(canvas, QPoint(110 + frame * 2, 125 + frame * 2));
            QElapsedTimer clock; clock.start();
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
        std::sort(oldFrames.begin(), oldFrames.end()); std::sort(newFrames.begin(), newFrames.end());
        printf("committed=%d plus_moving_draft old_frame=%.3f new_frame=%.3f unchanged_pq=%.3f cold_with_base_copy=%.3f\n",
               count, oldFrames[2], newFrames[2], unchanged, cold);
        fflush(stdout);
    }
}
