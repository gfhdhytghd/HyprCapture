// Opt-in: run only under an isolated nested compositor with the candidate
// plugin.
#include "shared/protocol.hpp"
#include "ui/annotation_editor.hpp"
#include "ui/capture_overlay.hpp"
#include "ui/clipboard_utils.hpp"
#include "ui/scroll_capture.hpp"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QWheelEvent>
#include <QWindow>
#include <iostream>
QImage document(int width, int height, qreal scale) {
  QImage out(qRound(width * scale), qRound(height * scale),
             QImage::Format_ARGB32);
  out.fill(QColor(250, 249, 246));
  QPainter p(&out);
  p.scale(scale, scale);
  p.setFont(QFont("sans-serif", 13));
  for (int y = 30, row = 0; y < height; y += 37, ++row) {
    p.fillRect(12, y - 18, 24, 24,
               QColor(row * 71 % 220, row * 43 % 220, row * 113 % 220));
    p.setPen(QColor(22, 30, 43));
    p.drawText(48, y,
               QString("Row %1 | native scroll fixture | value %2")
                   .arg(row)
                   .arg(row * 7919));
  }
  return out;
}
class Fixture : public QWidget {
public:
  int offset = 400;
  QImage page;
  Fixture() {
    resize(800, 600);
    setWindowTitle("Scroll capture fixture");
  }
  void paintEvent(QPaintEvent *) override {
    const auto scale = windowHandle()->devicePixelRatio();
    if (page.width() != qRound(width() * scale))
      page = document(width(), 3000, scale);
    QPainter p(this);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.drawImage(rect(), page,
                QRect(0, qRound(offset * scale), page.width(),
                      qRound(height() * scale)));
  }
  void wheelEvent(QWheelEvent *e) override {
    offset =
        std::clamp(offset + (e->pixelDelta().isNull() ? -e->angleDelta().y() / 8
                                                      : -e->pixelDelta().y()),
                   0, 2000);
    std::cout << "fixture offset=" << offset << std::endl;
    update();
    e->accept();
  }
};
int main(int argc, char **argv) {
  if (!qEnvironmentVariableIsSet("HYPRCAPTURE_SCROLL_LIVE_TEST"))
    return 77;
  const bool fixture =
      argc > 1 && QString::fromLocal8Bit(argv[1]) == "--fixture";
  if (fixture)
    qputenv("QT_WAYLAND_SHELL_INTEGRATION", "xdg-shell");
  QApplication app(argc, argv);
  app.setQuitOnLastWindowClosed(false);
  if (fixture) {
    QGuiApplication::setDesktopFileName("hyprcapture-scroll-fixture");
    Fixture f;
    f.show();
    return app.exec();
  }
  if (argc != 2)
    return 2;
  const QString dir = QString::fromLocal8Bit(argv[1]);
  QDir().mkpath(dir);
  auto *screen = app.primaryScreen();
  if (!screen)
    return 77;
  QProcess child;
  child.setStandardOutputFile(dir + "/fixture.log");
  child.setStandardErrorFile(dir + "/fixture-wire.log");
  child.start(app.applicationFilePath(), {"--fixture"});
  if (!child.waitForStarted())
    return 2;
  bool passed = false;
  int phase = 0;
  qreal scale = screen->devicePixelRatio();
  {
    QProcess monitors;
    monitors.start("hyprctl", {"-j", "monitors"});
    monitors.waitForFinished();
    for (const auto &value :
         QJsonDocument::fromJson(monitors.readAllStandardOutput()).array())
      if (value.toObject()["name"].toString() == screen->name())
        scale = value.toObject()["scale"].toDouble();
  }
  auto fail = [&](const QString &text) {
    std::cerr << text.toStdString() << std::endl;
    app.exit(1);
  };
  QTimer status;
  status.start(500);
  QObject::connect(&status, &QTimer::timeout, &app, [&] {
    for (auto *w : app.topLevelWidgets())
      for (auto *label : w->findChildren<QLabel *>())
        if (label->parentWidget()->objectName() == "scrollCaptureBar")
          std::cout << label->text().toStdString() << std::endl;
  });
  QTimer::singleShot(25000, &app, [&] { fail("live test timeout"); });
  QTimer::singleShot(1200, &app, [&] {
    QProcess query;
    query.start("hyprctl", {"-j", "clients"});
    query.waitForFinished();
    QJsonObject client;
    for (const auto &v :
         QJsonDocument::fromJson(query.readAllStandardOutput()).array())
      if (v.toObject()["pid"].toInt() == child.processId())
        client = v.toObject();
    if (client.isEmpty()) {
      fail("fixture client not found");
      return;
    }
    const auto at = client["at"].toArray(), sz = client["size"].toArray();
    const QRect capture(at[0].toInt(), at[1].toInt(), sz[0].toInt(),
                        sz[1].toInt());
    const auto full = document(capture.width(), 3000, scale);
    const auto expected = full.copy(0, qRound(200 * scale), full.width(),
                                    qRound((capture.height() + 720) * scale));
    hyprcapture::CaptureDefaults defaults;
    const bool windowMode =
        qEnvironmentVariableIsSet("HYPRCAPTURE_SCROLL_WINDOW");
    defaults.mode = windowMode ? hyprcapture::CaptureMode::Window
                               : hyprcapture::CaptureMode::Region;
    defaults.inPlaceEditToolbar = true;
    defaults.clipboard = false;
    defaults.showThumbnail = false;
    defaults.screenshotNotification = false;
    defaults.rememberSettings = false;
    defaults.language = "en";
    hyprcapture::CaptureSession session;
    session.id = "scroll-live";
    session.regionCaptureAvailable = true;
    session.scrollSessionVersion = 1;
    session.defaults = defaults;
    QImage frozen(qRound(screen->geometry().width() * scale),
                  qRound(screen->geometry().height() * scale),
                  QImage::Format_RGBA8888);
    frozen.fill(Qt::darkGray);
    {
      QPainter p(&frozen);
      p.drawImage(QRect(qRound((capture.x() - screen->geometry().x()) * scale),
                        qRound((capture.y() - screen->geometry().y()) * scale),
                        full.width(), qRound(capture.height() * scale)),
                  full,
                  QRect(0, qRound(400 * scale), full.width(),
                        qRound(capture.height() * scale)));
    }
    const QString artifact =
        hyprcapture::ui::runtimeFile("scroll-test", ".rgba");
    QFile file(artifact);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
      fail("monitor artifact failed");
      return;
    }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(reinterpret_cast<const char *>(frozen.constBits()),
               frozen.sizeInBytes());
    file.close();
    hyprcapture::MonitorInfo monitor;
    monitor.name = screen->name().toStdString();
    monitor.logicalGeometry = {double(screen->geometry().x()),
                               double(screen->geometry().y()),
                               double(screen->geometry().width()),
                               double(screen->geometry().height())};
    monitor.focused = true;
    monitor.scale = scale;
    monitor.artifactPath = artifact.toStdString();
    monitor.artifactWidth = frozen.width();
    monitor.artifactHeight = frozen.height();
    session.monitors.push_back(monitor);
    if (windowMode) {
      const QImage pixels = full.copy(0, qRound(400 * scale), full.width(),
                                      qRound(capture.height() * scale))
                                .convertToFormat(QImage::Format_RGBA8888);
      const QString path =
          hyprcapture::ui::runtimeFile("scroll-test-window", ".rgba");
      QFile output(path);
      if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        fail("window artifact failed");
        return;
      }
      output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
      output.write(reinterpret_cast<const char *>(pixels.constBits()),
                   pixels.sizeInBytes());
      output.close();
      hyprcapture::WindowInfo info;
      info.address = client["address"].toString().toStdString();
      info.appClass = "hyprcapture-scroll-fixture";
      info.focused = true;
      info.visibleGeometry = info.fullGeometry = {
          double(capture.x()), double(capture.y()), double(capture.width()),
          double(capture.height())};
      info.artifactPath = path.toStdString();
      info.artifactWidth = pixels.width();
      info.artifactHeight = pixels.height();
      session.windows.push_back(info);
    }
    auto *overlay = new CaptureOverlay(
        defaults, false, false, false,
        QString::fromStdString(hyprcapture::encodeSessionJson(session)));
    overlay->show();
    QTimer::singleShot(400, &app, [&, overlay, capture, expected, windowMode] {
      const QRect local = capture.translated(-screen->geometry().topLeft());
      if (windowMode)
        QTest::mouseClick(overlay, Qt::LeftButton, Qt::NoModifier,
                          local.center());
      else {
        QTest::mousePress(overlay, Qt::LeftButton, Qt::NoModifier,
                          local.topLeft());
        QTest::mouseMove(overlay, local.bottomRight());
        QTest::mouseRelease(overlay, Qt::LeftButton, Qt::NoModifier,
                            local.bottomRight());
      }
      auto *editor = overlay->findChild<AnnotationEditor *>("inPlaceEditor");
      auto *controller =
          overlay->findChild<hyprcapture::ui::ScrollCaptureController *>();
      if (!editor || !controller) {
        for (auto *label : overlay->findChildren<QLabel *>())
          std::cerr << label->text().toStdString() << std::endl;
        std::cerr << "editor=" << (editor != nullptr)
                  << " capture=" << capture.x() << "," << capture.y() << ","
                  << capture.width() << "," << capture.height()
                  << " overlay=" << overlay->width() << "," << overlay->height()
                  << std::endl;
        fail("selected target did not arm scrolling");
        return;
      }
      auto *pen = editor->findChild<QToolButton *>("annotationTool5");
      if (pen)
        pen->click();
      auto *canvas = editor->findChild<QWidget *>("annotationCanvas");
      const QPoint a = local.topLeft() + QPoint(100, 80), b = a + QPoint(60, 0);
      QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, a);
      QTest::mouseMove(canvas, b);
      QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, b);
      const QImage beforeScroll = editor->resultImage();
      const QRect beforeGeometry = editor->canvasGeometry();
      QObject::connect(
          controller, &hyprcapture::ui::ScrollCaptureController::cancelled,
          &app, [&, overlay, beforeScroll, beforeGeometry] {
            if (!qEnvironmentVariableIsSet("HYPRCAPTURE_SCROLL_CANCEL"))
              return;
            QTimer::singleShot(
                50, overlay, [&, overlay, beforeScroll, beforeGeometry] {
                  auto *ed =
                      overlay->findChild<AnnotationEditor *>("inPlaceEditor");
                  if (ed->resultImage() != beforeScroll ||
                      ed->canvasGeometry() != beforeGeometry) {
                    fail("cancel did not restore editor");
                    return;
                  }
                  ed->undo();
                  if (ed->resultImage() == beforeScroll) {
                    fail("cancel lost undo history");
                    return;
                  }
                  ed->redo();
                  if (ed->resultImage() != beforeScroll) {
                    fail("cancel lost redo history");
                    return;
                  }
                  QFile report(dir + "/result.json");
                  if (!report.open(QIODevice::WriteOnly)) {
                    fail("cancel report failed");
                    return;
                  }
                  report.write(
                      QJsonDocument(
                          QJsonObject{
                              {"cancel_restores_image_geometry_history", true},
                              {"scale", scale}})
                          .toJson());
                  passed = true;
                  app.quit();
                });
          });
      QObject::connect(controller,
                       &hyprcapture::ui::ScrollCaptureController::failed, &app,
                       [&](const QString &text) {
                         std::cerr << text.toStdString() << std::endl;
                       });
      QObject::connect(
          controller, &hyprcapture::ui::ScrollCaptureController::progress, &app,
          [&, controller, capture](const QSize &size, int count) {
            const int current =
                qRound(controller->resultLayout().viewportY / scale) + 400;
            if (phase == 0)
              std::cout << "progress: " << current << " " << size.width() << "x"
                        << size.height() << std::endl;
            const int expectedOffsets[] = {520, 200, 450, 700, 920, 200};
            if (phase >= 6 || current != expectedOffsets[phase])
              return;
            std::cout << "accepted phase=" << phase << " offset=" << current
                      << " height=" << size.height() << " frames=" << count
                      << std::endl;
            ++phase;
            if (phase == 3 &&
                qEnvironmentVariableIsSet("HYPRCAPTURE_SCROLL_CANCEL")) {
              QTimer::singleShot(100, controller,
                                 [controller] { controller->cancel(); });
              return;
            }
            if (phase == 6)
              QTimer::singleShot(350, controller,
                                 [controller] { controller->finish(); });
            else {
              const int next = expectedOffsets[phase], delta = next - current;
              QTimer::singleShot(300, controller, [controller, capture, delta] {
                controller->scroll(capture.center(), delta, delta * 8, false,
                                   false);
              });
            }
          });
      QObject::connect(
          controller, &hyprcapture::ui::ScrollCaptureController::completed,
          &app,
          [&, overlay, expected = QImage(expected),
           windowMode](const QImage &actual) mutable {
            QPoint padding;
            if (windowMode) {
              padding = QPoint((actual.width() - expected.width()) / 2,
                               (actual.height() - expected.height()) / 2);
              if (padding.x() < 0 || padding.y() < 0 || padding.x() > 8 ||
                  padding.y() > 8) {
                fail("unexpected native window bounds");
                return;
              }
              QImage native(actual.size(), QImage::Format_ARGB32);
              native.fill(Qt::transparent);
              {
                QPainter p(&native);
                p.drawImage(padding, expected);
              }
              expected = native;
            }
            actual.save(dir + "/stitched.png");
            expected.save(dir + "/expected.png");
            if (actual != expected) {
              fail(QString("native pixels mismatch: %1x%2 expected %3x%4")
                       .arg(actual.width())
                       .arg(actual.height())
                       .arg(expected.width())
                       .arg(expected.height()));
              return;
            }
            QTimer::singleShot(350, overlay, [&, overlay, expected, padding] {
              auto *ed =
                  overlay->findChild<AnnotationEditor *>("inPlaceEditor");
              auto marked = ed->resultImage();
              marked.save(dir + "/annotated.png");
              if (marked.pixelColor(qRound(130 * scale) + padding.x(),
                                    qRound(280 * scale) + padding.y()) !=
                  QColor("#ff5252")) {
                fail("annotation document position mismatch");
                return;
              }
              ed->undo();
              if (ed->resultImage().convertToFormat(QImage::Format_ARGB32) !=
                  expected) {
                fail("undo did not restore native pixels");
                return;
              }
              ed->redo();
              if (ed->resultImage() != marked) {
                fail("redo changed annotations");
                return;
              }
              QFile report(dir + "/result.json");
              if (!report.open(QIODevice::WriteOnly)) {
                fail("report failed");
                return;
              }
              report.write(
                  QJsonDocument(QJsonObject{{"pixel_exact", true},
                                            {"annotation_exact", true},
                                            {"undo_redo", true},
                                            {"scale", scale},
                                            {"width", expected.width()},
                                            {"height", expected.height()},
                                            {"phases", phase}})
                      .toJson());
              passed = true;
              app.quit();
            });
          });
      QTimer::singleShot(400, controller, [controller, capture] {
        if (qEnvironmentVariableIsSet("HYPRCAPTURE_SCROLL_NATIVE_INPUT")) {
          auto *input = new QProcess(controller);
          auto *screen = QGuiApplication::primaryScreen();
          input->start(
              qEnvironmentVariable("HYPRCAPTURE_SCROLL_NATIVE_INPUT"),
              {QString::number(capture.center().x() - screen->geometry().x()),
               QString::number(capture.center().y() - screen->geometry().y()),
               QString::number(screen->geometry().width()),
               QString::number(screen->geometry().height()), "120"});
          QObject::connect(
              input, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
              input, &QObject::deleteLater);
        } else
          controller->scroll(capture.center(), 120, 960, false, false);
      });
    });
  });
  const int result = app.exec();
  child.terminate();
  child.waitForFinished(2000);
  if (child.state() != QProcess::NotRunning) {
    child.kill();
    child.waitForFinished();
  }
  return passed ? 0 : (result ? result : 1);
}
