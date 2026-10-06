// Opt-in compositor test. Run only in an isolated nested Wayland session:
// HYPRCAPTURE_SCROLL_LIVE_TEST=1 .../hyprcapture-scroll-live-test OUTPUT_DIR
// Its fixture, capture and saved PNG are real; it does not use the clipboard.
#include "shared/protocol.hpp"
#include "ui/annotation_editor.hpp"
#include "ui/capture_overlay.hpp"
#include "ui/clipboard_utils.hpp"
#include "ui/scroll_capture.hpp"

#include <LayerShellQt/Shell>
#include <LayerShellQt/Window>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QTest>
#include <QTimer>
#include <QWindow>
#include <iostream>

class DocumentFixture final : public QWidget {
  public:
    QImage document;
    int offset = 0;
    qreal scale;
    DocumentFixture(QScreen* screen) : QWidget(nullptr, Qt::FramelessWindowHint), scale(screen->devicePixelRatio()) {
        resize(std::min(800, screen->geometry().width() - 150), std::min(600, screen->geometry().height() - 220));
        document = QImage(qRound(width() * scale), qRound(2000 * scale), QImage::Format_ARGB32);
        document.fill(QColor(250, 249, 246));
        QPainter painter(&document);
        painter.scale(scale, scale);
        painter.setFont(QFont("sans-serif", 13));
        for (int y = 30, row = 0; y < 2000; y += 37, ++row) {
            painter.fillRect(12, y - 18, 24, 24, QColor((row * 71) % 220, (row * 43) % 220, (row * 113) % 220));
            painter.setPen(QColor(22, 30, 43));
            painter.drawText(48, y, QString("Row %1 | HyprCapture scroll test | document value %2").arg(row, 3, 10, QLatin1Char('0')).arg(row * 7919));
        }
        painter.end();
        winId();
        windowHandle()->setScreen(screen);
        if (auto* layer = LayerShellQt::Window::get(windowHandle())) {
            layer->setScreen(screen);
            layer->setScope("hyprcapture-scroll-fixture");
            layer->setLayer(LayerShellQt::Window::LayerTop);
            layer->setAnchors(LayerShellQt::Window::Anchors{LayerShellQt::Window::AnchorTop} | LayerShellQt::Window::AnchorLeft);
            layer->setExclusiveZone(-1);
            layer->setDesiredSize(size());
            layer->setMargins(QMargins(100, 160, 0, 0));
            layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
        }
    }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.drawImage(rect(), document, QRect(0, qRound(offset * scale), document.width(), qRound(height() * scale)));
    }
};

int main(int argc, char** argv) {
    if (!qEnvironmentVariableIsSet("HYPRCAPTURE_SCROLL_LIVE_TEST"))
        return 77;
    if (argc != 2)
        return 2;
    LayerShellQt::Shell::useLayerShell();
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    auto* screen = QGuiApplication::primaryScreen();
    if (!screen || screen->geometry().width() < 800 || screen->geometry().height() < 600)
        return 77;
    const QString outputDir = QString::fromLocal8Bit(argv[1]);
    if (!QDir().mkpath(outputDir))
        return 2;
    const QString outputFile = outputDir + "/scroll-live.png";
    if (QFile::exists(outputFile))
        return 2;
    const QString config = outputDir + "/config";
    qputenv("XDG_CONFIG_HOME", config.toLocal8Bit());
    qputenv("XDG_CACHE_HOME", (outputDir + "/cache").toLocal8Bit());
    bool verifiedEditor = false;
    int captures = 0;
    int error = 0;
    const auto fail = [&](const QString& text) {
        std::cerr << text.toStdString() << '\n';
        error = 1;
        app.exit(1);
    };
    DocumentFixture fixture(screen);
    const QRect selected(screen->geometry().topLeft() + QPoint(100, 160), fixture.size());
    const QImage expected = fixture.document.copy(0, 0, fixture.document.width(), qRound((fixture.height() + 520) * fixture.scale));
    fixture.show();
    QString previousStatus;
    QTimer statusTimer;
    QObject::connect(&statusTimer, &QTimer::timeout, &app, [&] {
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (auto* label = widget->findChild<QLabel*>("scrollCaptureStatus")) {
                if (label->text() != previousStatus) {
                    previousStatus = label->text();
                    std::cout << "status: " << previousStatus.toStdString() << std::endl;
                }
            }
        }
    });
    statusTimer.start(500);
    QTimer::singleShot(40000, &app, [&] { fail("live scrolling test timed out"); });
    QTimer::singleShot(600, &app, [&] {
        hyprcapture::CaptureDefaults defaults;
        defaults.mode = hyprcapture::CaptureMode::Region;
        defaults.inPlaceEditToolbar = true;
        defaults.fushionMode = true;
        defaults.clipboard = false;
        defaults.showThumbnail = false;
        defaults.screenshotNotification = false;
        defaults.rememberSettings = false;
        defaults.saveDir = outputDir.toStdString();
        defaults.filenameTemplate = "scroll-live.png";
        defaults.language = "en";
        hyprcapture::CaptureSession session;
        session.id = "scroll-live-test";
        session.regionCaptureAvailable = true;
        session.defaults = defaults;
        session.cursorPosition = hyprcapture::Point{static_cast<double>(selected.center().x()), static_cast<double>(selected.center().y())};
        QImage frozen(qRound(screen->geometry().width() * fixture.scale), qRound(screen->geometry().height() * fixture.scale), QImage::Format_RGBA8888);
        frozen.fill(Qt::darkGray);
        {
            QPainter painter(&frozen);
            painter.drawImage(QRect(qRound(100 * fixture.scale), qRound(160 * fixture.scale), fixture.document.width(), qRound(600 * fixture.scale)),
                              fixture.document, QRect(0, 0, fixture.document.width(), qRound(600 * fixture.scale)));
        }
        const QString artifact = hyprcapture::ui::runtimeFile("scroll-test", ".rgba");
        QFile file(artifact);
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
            !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
            file.write(reinterpret_cast<const char*>(frozen.constBits()), frozen.sizeInBytes()) != frozen.sizeInBytes()) {
            fail("could not prepare selector fixture"); return;
        }
        file.close();
        hyprcapture::MonitorInfo monitor;
        monitor.name = screen->name().toStdString();
        monitor.logicalGeometry = {static_cast<double>(screen->geometry().x()), static_cast<double>(screen->geometry().y()),
                                   static_cast<double>(screen->geometry().width()), static_cast<double>(screen->geometry().height())};
        monitor.focused = true;
        monitor.scale = fixture.scale;
        monitor.artifactWidth = frozen.width();
        monitor.artifactHeight = frozen.height();
        monitor.artifactPath = artifact.toStdString();
        session.monitors.push_back(monitor);
        auto* overlay = new CaptureOverlay(defaults, false, false, false, QString::fromStdString(hyprcapture::encodeSessionJson(session)));
        QObject::connect(overlay, &CaptureOverlay::scrollingChanged, &app, [overlay](bool scrolling) {
            if (scrolling) overlay->hide(); else overlay->show();
        });
        overlay->show();
        QTimer::singleShot(300, &app, [&, overlay] {
            auto* toggle = overlay->findChild<QPushButton*>("scrollCaptureToggle");
            if (!toggle || !toggle->isVisible()) { fail("missing scrolling capture entry"); return; }
            QTest::mouseClick(toggle, Qt::LeftButton);
            const QRect local = selected.translated(-screen->geometry().topLeft());
            QTest::mousePress(overlay, Qt::LeftButton, Qt::NoModifier, local.topLeft());
            QTest::mouseMove(overlay, local.bottomRight());
            QTest::mouseRelease(overlay, Qt::LeftButton, Qt::NoModifier, local.bottomRight());
            hyprcapture::ui::ScrollCaptureController* controller = nullptr;
            for (auto* widget : QApplication::topLevelWidgets()) {
                if (auto* candidate = qobject_cast<hyprcapture::ui::ScrollCaptureController*>(widget)) controller = candidate;
            }
            if (!controller || !controller->isVisible() || overlay->isVisible()) {
                for (auto* label : overlay->findChildren<QLabel*>()) std::cerr << label->text().toStdString() << '\n';
                fail("selector did not hand input back to the live document"); return;
            }
            QObject::connect(controller, &hyprcapture::ui::ScrollCaptureController::progress, &app, [&, controller](const QSize& size, int count) {
                captures = count;
                std::cout << "captured " << count << " " << size.width() << "x" << size.height() << std::endl;
                const int offsets[] = {120, 300, 520};
                if (count <= 3) {
                    const int offset = offsets[count - 1];
                    QTimer::singleShot(300, &fixture, [&, offset] { fixture.offset = offset; fixture.update(); });
                } else if (count == 4) {
                    QTimer::singleShot(250, controller, [controller] { controller->findChild<QPushButton*>("scrollCaptureFinish")->click(); });
                } else fail("unexpected extra capture");
            });
            QObject::connect(controller, &hyprcapture::ui::ScrollCaptureController::cancelled, &app, [&] { fail("unexpected cancellation"); });
            QObject::connect(controller, &hyprcapture::ui::ScrollCaptureController::completed, &app, [&, overlay](const QImage& image) {
                image.save(outputDir + "/stitched.png");
                expected.save(outputDir + "/expected.png");
                if (image != expected) { fail("real screencopy/stitch pixels differ from document"); return; }
                QTimer::singleShot(300, overlay, [&, overlay] {
                    auto* editor = overlay->findChild<AnnotationEditor*>("inPlaceEditor");
                    if (!editor || !editor->isVisible() || editor->resultImage().convertToFormat(QImage::Format_ARGB32) != expected) {
                        if (editor) editor->resultImage().save(outputDir + "/editor-failed.png");
                        fail("long image did not reach the existing editor intact"); return;
                    }
                    verifiedEditor = true;
                    QTest::keyClick(overlay, Qt::Key_Return);
                });
            });
        });
    });
    const int result = app.exec();
    if (result || error) return 1;
    QImage saved(outputFile);
    if (!verifiedEditor || captures != 4 || saved.convertToFormat(QImage::Format_ARGB32) != expected) {
        std::cerr << "saved long screenshot did not match the original document\n";
        return 1;
    }
    QFile report(outputDir + "/result.json");
    if (report.open(QIODevice::WriteOnly))
        report.write(QJsonDocument(QJsonObject{{"captures", captures}, {"width", saved.width()}, {"height", saved.height()},
                                              {"scale", fixture.scale}, {"pixel_exact", true}, {"editor_verified", verifiedEditor}}).toJson());
    std::cout << "PASS: real region capture, stitch, editor and saved PNG are pixel-exact\n";
    return 0;
}
