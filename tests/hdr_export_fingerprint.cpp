// Explicit cross-revision fingerprint fixture. Compare on the same Qt/font
// environment; do not bake Qt rasterizer-specific hashes into portable CTest.
#include "ui/annotation_editor.hpp"
#include <QApplication>
#include <QCryptographicHash>
#include <QSlider>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QtCore/qfloat16.h>
#include <cstdio>
int main(int argc, char **argv) {
  QTemporaryDir config;
  qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
  QApplication app(argc, argv);
  for (auto format :
       {QImage::Format_RGBA16FPx4_Premultiplied, QImage::Format_RGBA32FPx4}) {
    AnnotationEditor editor;
    editor.resize(900, 650);
    editor.setHdrEnabled(true);
    QImage sdr(240, 160, QImage::Format_ARGB32_Premultiplied);
    QImage hdr(sdr.size(), QImage::Format_RGBA16FPx4);
    for (int y = 0; y < hdr.height(); ++y) {
      auto *row = reinterpret_cast<qfloat16 *>(hdr.scanLine(y));
      for (int x = 0; x < hdr.width(); ++x) {
        sdr.setPixelColor(x, y, QColor(x % 256, y % 256, (x + y) % 256));
        row[x * 4] = qfloat16(x / 20.F);
        row[x * 4 + 1] = qfloat16(-.15F);
        row[x * 4 + 2] = qfloat16(y / 50.F);
        row[x * 4 + 3] = qfloat16(float((x + y) % 5) / 4);
      }
    }
    hdr = hdr.convertToFormat(format);
    editor.setImage(sdr);
    editor.setImageDisplayRect({40, 30, 480, 320});
    editor.show();
    app.processEvents();
    auto *canvas = editor.findChild<QWidget *>("annotationCanvas");
    const auto print = [&](const char *label) {
      for (bool linear : {false, true}) {
        const auto out =
            linear ? editor.resultImageOn(hdr) : editor.resultImage();
        const auto hash =
            QCryptographicHash::hash(
                QByteArrayView(reinterpret_cast<const char *>(out.constBits()),
                               out.sizeInBytes()),
                QCryptographicHash::Sha256)
                .toHex();
        printf("%d %s %s %s\n", int(format), label, linear ? "hdr" : "sdr",
               hash.constData());
      }
    };
    const auto stroke = [&](int tool, QPoint from, QPoint to) {
      editor.findChild<QToolButton *>(QString("annotationTool%1").arg(tool))
          ->click();
      const auto mapped = [](QPoint p) { return QPoint(40, 30) + p * 2; };
      QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, mapped(from));
      QTest::mouseMove(canvas, mapped(to));
      QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, mapped(to));
    };
    print("base");
    stroke(5, {31, 33}, {101, 81});
    print("normal");
    editor.findChild<QSlider *>("annotationHdrGain")->setValue(400);
    stroke(6, {41, 43}, {151, 101});
    print("gain");
    editor.findChild<QSlider *>("annotationHdrGain")->setValue(100);
    stroke(9, {71, 23}, {161, 111});
    print("mosaic");
    stroke(10, {21, 33}, {201, 131});
    print("spotlight");
    editor.setExternalImagePresentation(true, false);
    print("layered");
    editor.setExternalImagePresentation(true, true);
    print("composited");
    editor.undo();
    print("undo");
    editor.redo();
    print("redo");
  }
}
