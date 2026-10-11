// Opt-in: run only under an isolated nested compositor with the candidate
// plugin.
#include "shared/protocol.hpp"
#include "ui/annotation_editor.hpp"
#include "ui/capture_overlay.hpp"
#include "ui/clipboard_utils.hpp"
#include "ui/hdr_image.hpp"
#include "ui/hdr_backdrop.hpp"
#include <qfloat16.h>
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
#include <QProcessEnvironment>
#include <QPushButton>
#include <QScreen>
#include <QTest>
#include <QTimer>
#include <QTemporaryDir>
#include <QToolButton>
#include <QWheelEvent>
#include <QWindow>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
QImage document(int width, int height, qreal scale) {
  QImage out(qRound(width * scale), qRound(height * scale),
             QImage::Format_ARGB32);
  out.fill(QColor(250, 249, 246,
                   qEnvironmentVariableIsSet("HYPRCAPTURE_SCROLL_FIXTURE_BACKDROP") ? 0 :
                   qEnvironmentVariableIsSet("HYPRCAPTURE_SCROLL_FIXTURE_ALPHA") ? 128 : 255));
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
QImage hdrDocument(int width, int height, qreal scale) {
  auto hdr = hyprcapture::ui::sdrToHdr(document(width,height,scale),153);
  for (int y=0;y<hdr.height();++y) {
    auto* row=reinterpret_cast<qfloat16*>(hdr.scanLine(y));
    for (int x=hdr.width()/2; x<std::min(hdr.width(),hdr.width()/2+qRound(80*scale)); ++x)
      if ((y/qRound(scale))%113<35) {
        row[x*4]=qfloat16(6.f); row[x*4+1]=qfloat16(.3f); row[x*4+2]=qfloat16(3.f);
      }
  }
  return hdr;
}
class Fixture : public QWidget {
public:
  int offset = 400;
  QImage page, hdrPage;
  bool hdrAnnounced = false;
  std::unique_ptr<hyprcapture::ui::HdrBackdrop> hdrSurface;
  Fixture() {
    bool hasOffset = false;
    const int requestedOffset = qEnvironmentVariableIntValue("HYPRCAPTURE_SCROLL_FIXTURE_OFFSET", &hasOffset);
    if (hasOffset) offset = std::clamp(requestedOffset, 0, 2000);
    if (qEnvironmentVariableIsSet("HYPRCAPTURE_SCROLL_FIXTURE_ALPHA"))
      setAttribute(Qt::WA_TranslucentBackground);
    if (qEnvironmentVariableIsSet("HYPRCAPTURE_SCROLL_HDR_FIXTURE"))
      setAttribute(Qt::WA_TranslucentBackground);
    resize(800, 600);
    setWindowTitle("Scroll capture fixture");
  }
  void paintEvent(QPaintEvent *) override {
    const auto scale = windowHandle()->devicePixelRatio();
    if (page.width() != qRound(width() * scale))
      page = document(width(), 3000, scale);
    if (qEnvironmentVariableIsSet("HYPRCAPTURE_SCROLL_HDR_FIXTURE")) {
      if (!hdrSurface) hdrSurface=std::make_unique<hyprcapture::ui::HdrBackdrop>(windowHandle());
      if (hdrPage.size()!=page.size()) hdrPage=hdrDocument(width(),3000,scale);
      const auto hdr=hdrPage.copy(0,qRound(offset*scale),page.width(),qRound(height()*scale));
      QPainter p(this); p.setCompositionMode(QPainter::CompositionMode_Source); p.fillRect(rect(),Qt::transparent);
      if (!hdrSurface->setImage(hdr,rect(),153)) qFatal("native HDR fixture unavailable");
      if (!hdrAnnounced) { hdrAnnounced=true; std::cout << "fixture HDR ready" << std::endl; }
      return;
    }
    QPainter p(this);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    if (qEnvironmentVariableIsSet("HYPRCAPTURE_SCROLL_FIXTURE_BACKDROP")) {
      QImage background(page.width(),qRound(height()*scale),QImage::Format_ARGB32);
      for(int y=0;y<background.height();++y)
        for(int x=0;x<background.width();++x) {
          const int c=200+int(40*std::sin(y/190.0+x/510.0));
          background.setPixel(x,y,qRgb(c,c,c+8));
        }
      p.drawImage(rect(),background);
      p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    }
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
  if (argc > 2)
    return 2;
  QTemporaryDir automaticOutput;
  if (argc == 1 && !automaticOutput.isValid())
    return 2;
  const QString dir = argc == 2 ? QString::fromLocal8Bit(argv[1]) : automaticOutput.path();
  if (argc == 1) automaticOutput.setAutoRemove(false);
  std::cout << "Live test artifacts: " << dir.toStdString() << std::endl;
  QDir().mkpath(dir);
  auto *screen = app.primaryScreen();
  if (!screen)
    return 77;
  QProcess child;
  const bool checkGestureFocus = qEnvironmentVariableIsSet("HYPRCAPTURE_SCROLL_POINTER_MOTION") &&
                                 qEnvironmentVariableIsSet("HYPRCAPTURE_SCROLL_NATIVE_FINGER");
  if (checkGestureFocus) {
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("WAYLAND_DEBUG", "client");
    child.setProcessEnvironment(environment);
  }
  child.setStandardOutputFile(dir + "/fixture.log");
  child.setStandardErrorFile(dir + "/fixture-wire.log");
  const QString gtkFixture = qEnvironmentVariable("HYPRCAPTURE_SCROLL_GTK_FIXTURE");
  if (!gtkFixture.isEmpty()) {
    const QString pagePath = dir + "/fixture-page.png";
    if (!document(800, 3000, screen->devicePixelRatio()).save(pagePath))
      return 2;
    child.start(gtkFixture, {pagePath});
  } else
    child.start(app.applicationFilePath(), {"--fixture"});
  if (!child.waitForStarted())
    return 2;
  bool passed = false;
  const bool continuous = qEnvironmentVariableIsSet("HYPRCAPTURE_SCROLL_CONTINUOUS");
  bool continuousDone = !continuous, continuousGrowth = false;
  const bool partialRegion = qEnvironmentVariableIsSet("HYPRCAPTURE_SCROLL_PARTIAL_REGION");
  const bool backdropFixture = qEnvironmentVariableIsSet("HYPRCAPTURE_SCROLL_FIXTURE_BACKDROP");
  const bool variableNativeInput = qEnvironmentVariableIsSet("HYPRCAPTURE_SCROLL_NATIVE_VARIABLE");
  bool nativeInputFinished = false;
  std::array<int, 6> offsets = (partialRegion || qEnvironmentVariableIsSet("HYPRCAPTURE_SCROLL_WINDOW")) ? std::array<int, 6>{460,340,400,480,520,340}
                                                   : std::array<int, 6>{520,200,450,700,920,200};
  const int minimumOffset = *std::min_element(offsets.begin(), offsets.end());
  const int maximumOffset = *std::max_element(offsets.begin(), offsets.end());
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
  QTimer::singleShot(qEnvironmentVariableIsSet("HYPRCAPTURE_SCROLL_HDR_FIXTURE") ? 45000 : 25000, &app, [&] { fail("live test timeout"); });
  QElapsedTimer startupDeadline; startupDeadline.start();
  QTimer startup; startup.start(qEnvironmentVariableIsSet("HYPRCAPTURE_SCROLL_HDR_FIXTURE") ? 5000 : 1200);
  QObject::connect(&startup,&QTimer::timeout,&app,[&] {
    QProcess query;
    query.start("hyprctl", {"-j", "clients"});
    query.waitForFinished();
    QJsonObject client;
    for (const auto &v :
         QJsonDocument::fromJson(query.readAllStandardOutput()).array())
      if (v.toObject()["pid"].toInt() == child.processId())
        client = v.toObject();
    QFile readiness(dir+"/fixture.log");
    const bool hdrReady=!qEnvironmentVariableIsSet("HYPRCAPTURE_SCROLL_HDR_FIXTURE") ||
        (readiness.open(QIODevice::ReadOnly) && readiness.readAll().contains("fixture HDR ready"));
    if (client.isEmpty() || !hdrReady) {
      if (child.state()==QProcess::Running && startupDeadline.elapsed()<15000) { startup.setInterval(100); return; }
      startup.stop();
      fail(QString("fixture startup unavailable: state=%1 exit=%2").arg(int(child.state())).arg(child.exitCode()));
      return;
    }
    startup.stop();
    const auto at = client["at"].toArray(), sz = client["size"].toArray();
    const QRect fixtureGeometry(at[0].toInt(), at[1].toInt(), sz[0].toInt(), sz[1].toInt());
    const QRect capture = partialRegion ? fixtureGeometry.adjusted(180,150,-280,-200) : fixtureGeometry;
    const QPoint inset = capture.topLeft() - fixtureGeometry.topLeft();
    auto full = document(fixtureGeometry.width(), 3000, scale);
    if (qEnvironmentVariableIsSet("HYPRCAPTURE_SCROLL_HDR_FIXTURE"))
      full = hyprcapture::ui::hdrToSdr(hdrDocument(fixtureGeometry.width(),3000,scale),153);
    auto expected = full.copy(qRound(inset.x() * scale), qRound((minimumOffset + inset.y()) * scale),
                              qRound(capture.width() * scale),
                              qRound((capture.height() + maximumOffset - minimumOffset) * scale));
    const bool alphaFixture = qEnvironmentVariableIsSet("HYPRCAPTURE_SCROLL_FIXTURE_ALPHA");
    if (alphaFixture) {
      QImage background(expected.size(), QImage::Format_ARGB32);
      background.fill(Qt::white);
      QPainter painter(&background);
      painter.drawImage(QPoint{}, expected);
      painter.end();
      expected = background;
    }
    hyprcapture::CaptureDefaults defaults;
    defaults.windowBackground = hyprcapture::WindowBackground::White;
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
    defaults.saveDir = dir.toStdString();
    defaults.filenameTemplate = "exported.png";
    if (qEnvironmentVariableIsSet("HYPRCAPTURE_SCROLL_HDR")) {
      defaults.screenshotFormat = "heic";
      defaults.hdrSaveSdr = true;
    }
    hyprcapture::CaptureSession session;
    session.id = "scroll-live";
    session.regionCaptureAvailable = true;
    session.scrollSessionVersion = qEnvironmentVariableIsSet("HYPRCAPTURE_SCROLL_HDR") ? 2 : 1;
    session.defaults = defaults;
    QImage frozen(qRound(screen->geometry().width() * scale),
                  qRound(screen->geometry().height() * scale),
                  QImage::Format_RGBA8888);
    frozen.fill(Qt::darkGray);
    {
      QPainter p(&frozen);
      p.drawImage(QRect(qRound((fixtureGeometry.x() - screen->geometry().x()) * scale),
                        qRound((fixtureGeometry.y() - screen->geometry().y()) * scale),
                        full.width(), qRound(fixtureGeometry.height() * scale)),
                  full,
                  QRect(0, qRound(400 * scale), full.width(),
                        qRound(fixtureGeometry.height() * scale)));
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
    monitor.artifactReferenceNits = 153;
    monitor.hdrCapable = qEnvironmentVariableIsSet("HYPRCAPTURE_SCROLL_HDR");
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
    QTimer::singleShot(400, &app, [&, overlay, capture, expected, windowMode, alphaFixture] {
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
          [&, controller, capture, editor](const QSize &size, int count) {
            if (qEnvironmentVariableIsSet("HYPRCAPTURE_SCROLL_HDR")) {
              auto* preview=controller->findChild<QLabel*>("thumbnailImage");
              // The SDR pixmap is cleared only after the native HDR surface
              // has successfully accepted its floating-point image.
              if (!preview || !preview->pixmap().isNull()) {
                fail("scroll thumbnail did not enter native HDR presentation"); return;
              }
            }
            if (!continuousDone && size.height() > controller->resultLayout().frameSize.height())
              continuousGrowth = true;
            const int current =
                qRound(controller->resultLayout().viewportY / scale) + 400;
            if (phase == 0)
              std::cout << "progress: " << current << " " << size.width() << "x"
                        << size.height() << std::endl;
            if (phase == 0 && variableNativeInput) {
              if (!nativeInputFinished || current <= 400 || current >= 520) return;
              offsets[0] = current;
            }
            const auto& expectedOffsets = offsets;
            if (phase >= 6 || current != expectedOffsets[phase])
              return;
            if (phase == 0 && continuous && (!continuousDone || !continuousGrowth)) {
              if (continuousDone) fail("preview did not grow during continuous input");
              return;
            }
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
                                 [&, controller, editor] {
                                   if (editor->toolbarWidget()->isHidden()) {
                                     fail("annotations did not restore after idle");
                                     return;
                                   }
                                   controller->finish();
                                 });
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
           windowMode, alphaFixture](const QImage &actual) mutable {
            QPoint padding;
            if (windowMode) {
              padding = QPoint((actual.width() - expected.width()) / 2,
                               (actual.height() - expected.height()) / 2);
              if (padding.x() < 0 || padding.y() < 0 || padding.x() > 128 ||
                  padding.y() > 128) {
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
            int maximumError = 0;
            int foregroundPixels = 0;
            // Compositor borders, shadows and rounded corners belong to the
            // native window. Compare its document interior independently.
            const QRect verified = windowMode ? expected.rect().adjusted(padding.x()+32,padding.y()+32,-padding.x()-32,-padding.y()-32) : expected.rect();
            if (windowMode && actual.size()==expected.size()) {
              for(int y=verified.top();y<=verified.bottom();++y) for(int x=verified.left();x<=verified.right();++x) {
                const auto a=actual.pixelColor(x,y), e=expected.pixelColor(x,y);
                maximumError=std::max({maximumError,std::abs(a.red()-e.red()),std::abs(a.green()-e.green()),std::abs(a.blue()-e.blue()),std::abs(a.alpha()-e.alpha())});
              }
            }
            if (backdropFixture && actual.size() == expected.size()) {
              // The wallpaper does not move with the document. Validate every
              // opaque glyph/marker against the known document coordinates;
              // unit tests separately verify retained background strip pixels.
              for(int y=0;y<expected.height();++y)
                for(int x=0;x<expected.width();++x) {
                  const auto e=expected.pixelColor(x,y), a=actual.pixelColor(x,y);
                  if(e.alpha()!=255) continue;
                  ++foregroundPixels;
                  maximumError=std::max({maximumError,std::abs(a.red()-e.red()),
                                        std::abs(a.green()-e.green()),std::abs(a.blue()-e.blue())});
                }
            }
            if (alphaFixture && actual.size() == expected.size()) {
              for (int y = 0; y < actual.height(); ++y)
                for (int x = 0; x < actual.width(); ++x) {
                  const auto a = actual.pixelColor(x, y), e = expected.pixelColor(x, y);
                  if (a.alpha() != e.alpha()) {
                    fail("window background alpha mismatch");
                    return;
                  }
                  maximumError = std::max({maximumError, std::abs(a.red()-e.red()),
                                          std::abs(a.green()-e.green()), std::abs(a.blue()-e.blue())});
                }
            }
            const bool hdrFixture = qEnvironmentVariableIsSet("HYPRCAPTURE_SCROLL_HDR_FIXTURE");
            if (hdrFixture && actual.size()==expected.size()) {
              for(int y=verified.top();y<=verified.bottom();++y) for(int x=verified.left();x<=verified.right();++x) {
                const auto a=actual.pixelColor(x,y), e=expected.pixelColor(x,y);
                maximumError=std::max({maximumError,std::abs(a.red()-e.red()),std::abs(a.green()-e.green()),std::abs(a.blue()-e.blue())});
              }
              const auto hdr=overlay->findChild<hyprcapture::ui::ScrollCaptureController*>()->hdrResult();
              float peak=0;
              for(int y=0;y<hdr.height();++y) {
                const auto* row=reinterpret_cast<const qfloat16*>(hdr.constScanLine(y));
                for(int x=0;x<hdr.width();++x) peak=std::max(peak,float(row[x*4]));
              }
              if(hdr.size()!=actual.size() || peak<5.8f) { fail("native HDR highlights missing from stitched result"); return; }
            }
            if (actual.size() != expected.size() ||
                (hdrFixture ? maximumError>(windowMode ? 5 : 2) : windowMode ? maximumError!=0 : backdropFixture ? foregroundPixels<1000 || maximumError!=0 : actual != expected)) {
              fail(QString("native pixels mismatch: %1x%2 expected %3x%4")
                       .arg(actual.width())
                       .arg(actual.height())
                       .arg(expected.width())
                       .arg(expected.height()));
              return;
            }
            QTimer::singleShot(350, overlay, [&, overlay, expected = actual.convertToFormat(QImage::Format_ARGB32_Premultiplied), padding, maximumError, alphaFixture, windowMode, hdrFixture] {
              auto *ed =
                  overlay->findChild<AnnotationEditor *>("inPlaceEditor");
              if (qEnvironmentVariableIsSet("HYPRCAPTURE_SCROLL_HDR")) {
                auto *sdr = overlay->findChild<QPushButton *>("screenshotSdr");
                auto *hdr = overlay->findChild<QPushButton *>("screenshotHdr");
                if (!sdr || sdr->isChecked() || !hdr || !hdr->isEnabled() || !hdr->isChecked() || !ed->hdrEnabled()) {
                  fail("scrolling result lost HDR export mode");
                  return;
                }
              }
              auto marked = ed->resultImage();
              marked.save(dir + "/annotated.png");
              if (marked.pixelColor(qRound(130 * scale) + padding.x(),
                                    qRound((480 - minimumOffset) * scale) + padding.y()) !=
                  QColor("#ff5252")) {
                fail("annotation document position mismatch");
                return;
              }
              ed->undo();
              if (ed->resultImage() != expected) {
                fail("undo did not restore native pixels");
                return;
              }
              ed->redo();
              if (ed->resultImage() != marked) {
                fail("redo changed annotations");
                return;
              }
              if (alphaFixture) {
                auto* selector=overlay->findChild<QWidget*>("windowBackground");
                auto* trigger=selector ? selector->findChild<QPushButton*>() : nullptr;
                if (!trigger || !trigger->isVisible()) {
                  fail("finished window capture lost its background control"); return;
                }
                const auto choose = [&](const QString& value) {
                  QTest::mouseClick(trigger, Qt::LeftButton);
                  for (auto* button : overlay->findChildren<QPushButton*>())
                    if (button->isVisible() && button->property("value").toString()==value) {
                      QTest::mouseClick(button, Qt::LeftButton); return true;
                    }
                  return false;
                };
                if (!choose("transparent")) { fail("transparent background unavailable"); return; }
                const auto raw=ed->resultImage();
                if (raw.size()!=marked.size() || raw.pixelColor(padding+QPoint(3,3)).alpha()!=128 ||
                    raw.pixelColor(padding+QPoint(qRound(130*scale),qRound((480-minimumOffset)*scale)))!=QColor("#ff5252")) {
                  fail("background change lost native alpha, dimensions or annotations"); return;
                }
                if (!choose("black")) { fail("black background unavailable"); return; }
                const auto black=ed->resultImage().pixelColor(padding+QPoint(3,3));
                if (black.alpha()!=255 || black.red()>=200) {
                  fail("black background did not recompose native pixels"); return;
                }
                if (qEnvironmentVariableIsSet("HYPRCAPTURE_SCROLL_REAL_BG")) {
                  if (!choose("real")) { fail("real background unavailable"); return; }
                  QImage realExpected(raw.size(), QImage::Format_ARGB32);
                  realExpected.fill(Qt::transparent);
                  {
                    QPainter painter(&realExpected);
                    painter.fillRect(realExpected.rect().adjusted(padding.x(),padding.y(),-padding.x(),-padding.y()), QColor("#204060"));
                    painter.drawImage(QPoint{}, raw);
                  }
                  const auto realActual = ed->resultImage();
                  realActual.save(dir + "/real-background.png");
                  realExpected.save(dir + "/real-background-expected.png");
                  // Editor alpha conversion and raster composition can differ
                  // by one channel value at antialiased glyphs. Larger errors
                  // indicate a wrong/contaminated background.
                  int maxError = 0;
                  for (int y = 0; y < realActual.height(); ++y)
                    for (int x = 0; x < realActual.width(); ++x) {
                      const auto a = realActual.pixelColor(x,y), b = realExpected.pixelColor(x,y);
                      maxError = std::max({maxError, std::abs(a.red()-b.red()), std::abs(a.green()-b.green()),
                                           std::abs(a.blue()-b.blue()), std::abs(a.alpha()-b.alpha())});
                    }
                  qInfo("real background maximum channel error=%d", maxError);
                  if (realActual.size() != realExpected.size() || maxError > 1) {
                    fail("native real background did not recompose the full scrolling result"); return;
                  }
                }
                if (!choose("white") || ed->resultImage()!=marked) {
                  ed->resultImage().save(dir + "/background-roundtrip.png");
                  fail("background round trip changed the result"); return;
                }
                ed->undo();
                if (ed->resultImage().convertToFormat(QImage::Format_ARGB32)!=expected) {
                  fail("background change damaged annotation undo"); return;
                }
                ed->redo();
                if (ed->resultImage()!=marked) { fail("background change damaged redo"); return; }
              } else if (!windowMode) {
                auto* selector=overlay->findChild<QWidget*>("windowBackground");
                if (selector && selector->parentWidget()->isVisible()) {
                  fail("finished region capture displays an empty capture toolbar"); return;
                }
              }
              QFile report(dir + "/result.json");
              if (!report.open(QIODevice::WriteOnly)) {
                fail("report failed");
                return;
              }
              report.write(
                  QJsonDocument(QJsonObject{{"pixel_exact", !windowMode && !backdropFixture && maximumError == 0},
                                            {"window_content_verified", windowMode},
                                            {"hdr_fixture", hdrFixture},
                                            {"hdr_preview_native", qEnvironmentVariableIsSet("HYPRCAPTURE_SCROLL_HDR")},
                                            {"stationary_backdrop", backdropFixture},
                                            {"foreground_pixels_exact", backdropFixture && maximumError == 0},
                                            {"maximum_rgb_error", maximumError},
                                            {"alpha_verified", alphaFixture},
                                            {"background_controls_round_trip", alphaFixture},
                                            {"real_background_recomposed", qEnvironmentVariableIsSet("HYPRCAPTURE_SCROLL_REAL_BG")},
                                            {"continuous_input", continuous},
                                            {"grew_during_input", continuousGrowth},
                                            {"annotation_exact", true},
                                            {"undo_redo", true},
                                            {"scale", scale},
                                            {"partial_region", partialRegion},
                                            {"width", expected.width()},
                                            {"height", expected.height()},
                                            {"phases", phase}})
                      .toJson());
              passed = true;
              if (qEnvironmentVariableIsSet("HYPRCAPTURE_SCROLL_HDR")) {
                auto *confirm = ed->findChild<QPushButton *>("annotationConfirm");
                if (!confirm) { fail("capture confirmation unavailable"); return; }
                confirm->click();
                return; // The normal export path quits after its worker completes.
              }
              app.quit();
            });
          });
      QTimer::singleShot(400, controller, [&, controller, capture] {
        if (continuous) {
          auto* timer = new QTimer(controller);
          auto remaining = std::make_shared<int>((offsets[0] - 400) / 3);
          QObject::connect(timer, &QTimer::timeout, controller, [&, controller, capture, timer, remaining] {
            controller->scroll(capture.center(), 3, 0, true, false);
            if (--*remaining == 0) {
              continuousDone = true;
              timer->stop();
              timer->deleteLater();
            }
          });
          timer->start(32);
          QTimer::singleShot(250, controller, [&, controller] {
            auto* editor = controller->parentWidget()->findChild<AnnotationEditor *>("inPlaceEditor");
            if (!editor || !editor->toolbarWidget()->isHidden())
              fail("annotations remained interactive during continuous input");
          });
          return;
        }
        if (qEnvironmentVariableIsSet("HYPRCAPTURE_SCROLL_NATIVE_INPUT")) {
          auto *input = new QProcess(controller);
          auto *screen = QGuiApplication::primaryScreen();
          input->start(
              qEnvironmentVariable("HYPRCAPTURE_SCROLL_NATIVE_INPUT"),
              {QString::number(capture.center().x() - screen->geometry().x()),
               QString::number(capture.center().y() - screen->geometry().y()),
               QString::number(screen->geometry().width()),
               QString::number(screen->geometry().height()), QString::number(offsets[0] - 400)});
          QObject::connect(
              input, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
              input, &QObject::deleteLater);
          QObject::connect(input, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), &app,
                           [&](int code, QProcess::ExitStatus status) {
                             nativeInputFinished = true;
                             if (code || status != QProcess::NormalExit) fail("native input driver failed");
                           });
        } else
          controller->scroll(capture.center(), offsets[0] - 400, (offsets[0] - 400) * 8, false, false);
      });
    });
  });
  const int result = app.exec();
  if (passed && checkGestureFocus) {
    QFile wire(dir + "/fixture-wire.log");
    if (!wire.open(QIODevice::ReadOnly)) {
      std::cerr << "native gesture trace unavailable" << std::endl; passed=false;
    }
    const auto events = wire.readAll();
    const auto begin = events.indexOf(".axis_source(1)");
    const auto end = events.indexOf(".axis_stop(", begin);
    if (begin < 0 || end < begin || events.mid(begin, end - begin).contains(".leave(")) {
      std::cerr << "pointer motion interrupted the native touchpad gesture" << std::endl;
      passed = false;
    }
  }
  if (passed && qEnvironmentVariableIsSet("HYPRCAPTURE_SCROLL_HDR") &&
      !qEnvironmentVariableIsSet("HYPRCAPTURE_SCROLL_CANCEL")) {
    const auto exported = QImage(dir + "/exported.png").convertToFormat(QImage::Format_ARGB32);
    const auto marked = QImage(dir + "/annotated.png").convertToFormat(QImage::Format_ARGB32);
    if (!QFile::exists(dir + "/exported.heic")) {
      std::cerr << "HDR HEIC export missing" << std::endl; passed=false;
    }
    bool pixelsEqual = !exported.isNull() && exported.size() == marked.size();
    for (int y = 0; pixelsEqual && y < exported.height(); ++y)
      pixelsEqual = std::memcmp(exported.constScanLine(y), marked.constScanLine(y),
                                size_t(exported.width()) * 4) == 0;
    // The export deliberately adds sRGB metadata; compare decoded pixels.
    if (!pixelsEqual) {
      std::cerr << "SDR export did not preserve the annotated scrolling result" << std::endl;
      passed = false;
    }
  }
  child.terminate();
  child.waitForFinished(2000);
  if (child.state() != QProcess::NotRunning) {
    child.kill();
    child.waitForFinished();
  }
  return passed ? 0 : (result ? result : 1);
}
