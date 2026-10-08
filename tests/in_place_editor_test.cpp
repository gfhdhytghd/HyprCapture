#include "shared/protocol.hpp"
#include "ui/annotation_editor.hpp"
#include "ui/capture_overlay.hpp"
#include "ui/clipboard_utils.hpp"
#include "ui/i18n.hpp"

#include <QApplication>
#include <QAbstractButton>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLocalSocket>
#include <QPainter>
#include <QPushButton>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QWheelEvent>
#include <cstdio>

namespace {

constexpr int kLogicalWidth = 800;
constexpr int kLogicalHeight = 600;
const QSize kWindowPixels(400, 240);
const QRect kOpaquePatch(80, 50, 240, 140);

QString writeArtifact(const QImage& image) {
    const QString path = hyprcapture::ui::runtimeFile(QStringLiteral("in-place-test"), QStringLiteral(".rgba"));
    QFile file(path);
    if (path.isEmpty() || !file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner))
        return {};
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    for (int y = 0; y < rgba.height(); ++y) {
        const auto bytes = static_cast<qint64>(rgba.width()) * 4;
        if (file.write(reinterpret_cast<const char*>(rgba.constScanLine(y)), bytes) != bytes) {
            file.remove();
            return {};
        }
    }
    return path;
}

QString sessionJson(const hyprcapture::CaptureDefaults& defaults, bool includeWindow = true,
                    QSize logicalSize = QSize(kLogicalWidth, kLogicalHeight), QRect windowGeometry = QRect(100, 100, 200, 120),
                    QRect stagePreview = {}, bool fullscreen = false) {
    QImage desktop(logicalSize * 2, QImage::Format_RGBA8888);
    desktop.fill(QColor(17, 29, 53));
    const QString desktopPath = writeArtifact(desktop);
    if (desktopPath.isEmpty())
        return {};

    hyprcapture::CaptureSession session;
    session.id = "in-place-test";
    session.defaults = defaults;
    session.defaults.language = qEnvironmentVariable("HYPRCAPTURE_TEST_LANGUAGE", QStringLiteral("en")).toStdString();
    session.cursorPosition = hyprcapture::Point{static_cast<double>(windowGeometry.center().x()), static_cast<double>(windowGeometry.center().y())};
    hyprcapture::MonitorInfo monitor;
    monitor.name = "test-2x";
    monitor.logicalGeometry = {0, 0, static_cast<double>(logicalSize.width()), static_cast<double>(logicalSize.height())};
    monitor.scale = 2;
    monitor.focused = true;
    monitor.artifactPath = desktopPath.toStdString();
    monitor.artifactWidth = desktop.width();
    monitor.artifactHeight = desktop.height();
    session.monitors.push_back(monitor);

    if (includeWindow) {
        QImage window(windowGeometry.size() * 2, QImage::Format_RGBA8888);
        window.fill(Qt::transparent);
        {
            QPainter painter(&window);
            painter.scale(static_cast<double>(window.width()) / kWindowPixels.width(), static_cast<double>(window.height()) / kWindowPixels.height());
            painter.fillRect(QRect(40, 30, 320, 180), QColor(30, 110, 80, 128));
            painter.fillRect(kOpaquePatch, QColor(34, 149, 98));
        }
        const QString windowPath = writeArtifact(window);
        if (windowPath.isEmpty())
            return {};
        hyprcapture::WindowInfo info;
        info.address = "0xtest";
        info.appClass = "test-app";
        info.title = "Native resolution test";
        info.focused = true;
        info.fullscreen = fullscreen;
        if (stagePreview.isValid()) {
            info.stagePreview = true;
            info.selectionGeometry = hyprcapture::Rect{double(stagePreview.x()), double(stagePreview.y()), double(stagePreview.width()), double(stagePreview.height())};
            info.selectionClipGeometry = *info.selectionGeometry;
        }
        info.fullGeometry = {static_cast<double>(windowGeometry.x()), static_cast<double>(windowGeometry.y()),
                             static_cast<double>(windowGeometry.width()), static_cast<double>(windowGeometry.height())};
        info.visibleGeometry = info.fullGeometry;
        info.artifactPath = windowPath.toStdString();
        info.artifactWidth = window.width();
        info.artifactHeight = window.height();
        session.windows.push_back(info);
    }
    return QString::fromStdString(hyprcapture::encodeSessionJson(session));
}

QPushButton* modeButton(CaptureOverlay& overlay, const QString& mode) {
    for (auto* button : overlay.findChildren<QPushButton*>(QStringLiteral("captureModeButton")))
        if (button->property("captureMode").toString() == mode)
            return button;
    return nullptr;
}

bool chooseBackground(CaptureOverlay& overlay, const QString& value) {
    auto* select = overlay.findChild<QWidget*>(QStringLiteral("windowBackground"));
    auto* trigger = select ? select->findChild<QPushButton*>() : nullptr;
    if (!trigger || !trigger->isVisible())
        return false;
    QTest::mouseClick(trigger, Qt::LeftButton);
    for (auto* button : overlay.findChildren<QPushButton*>()) {
        if (button->isVisible() && button->property("value").toString() == value) {
            QTest::mouseClick(button, Qt::LeftButton);
            return true;
        }
    }
    return false;
}

void selectRegion(CaptureOverlay& overlay, const QPoint& start, const QPoint& end) {
    QTest::mousePress(&overlay, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(&overlay, end);
    QTest::mouseRelease(&overlay, Qt::LeftButton, Qt::NoModifier, end);
}

QRect editorToolbarCluster(CaptureOverlay& overlay, AnnotationEditor& editor) {
    auto* capture = overlay.findChild<QWidget*>(QStringLiteral("toolbar"));
    const QRect annotation(editor.toolbarWidget()->mapTo(&overlay, QPoint()), editor.toolbarWidget()->size());
    return capture ? annotation.united(QRect(capture->mapTo(&overlay, QPoint()), capture->size())) : annotation;
}

void verifyEditorToolbarCluster(CaptureOverlay& overlay, AnnotationEditor& editor) {
    const QRect cluster = editorToolbarCluster(overlay, editor);
    const QRect image = editor.canvasGeometry();
    if (auto* capture = overlay.findChild<QWidget*>(QStringLiteral("toolbar")))
        QVERIFY(editor.toolbarGeometry().bottom() < capture->y());
    QVERIFY(overlay.rect().contains(cluster));
    QVERIFY(!cluster.intersects(image));
    const int gap = cluster.top() > image.bottom() ? cluster.top() - image.bottom() : image.top() - cluster.bottom();
    QVERIFY(gap > 0 && gap <= 24);
    for (auto* toolbar : {overlay.findChild<QWidget*>(QStringLiteral("toolbar")), editor.toolbarWidget()}) {
        QVERIFY(toolbar);
        for (auto* button : toolbar->findChildren<QAbstractButton*>()) {
            if (button->isVisible())
                QVERIFY2(overlay.rect().contains(QRect(button->mapTo(&overlay, QPoint()), button->size())), qPrintable(button->objectName()));
        }
    }
}

bool clickEditorAction(AnnotationEditor& editor, const char* name) {
    auto* button = editor.findChild<QAbstractButton*>(QString::fromLatin1(name));
    if (!button)
        return false;
    if (!button->isVisible()) {
        auto* more = editor.findChild<QAbstractButton*>(QStringLiteral("annotationMore"));
        if (!more || !more->isVisible())
            return false;
        QTest::mouseClick(more, Qt::LeftButton);
    }
    if (!button->isVisible())
        return false;
    QTest::mouseClick(button, Qt::LeftButton);
    return true;
}

} // namespace

class InPlaceEditorTest final : public QObject {
    Q_OBJECT

  private slots:
    void rawRowsRemainOwnedAfterArtifactCleanup_data() {
        QTest::addColumn<bool>("topDown");
        QTest::newRow("top-down") << true;
        QTest::newRow("bottom-up") << false;
    }

    void rawRowsRemainOwnedAfterArtifactCleanup() {
        QFETCH(bool, topDown);
        hyprcapture::CaptureDefaults defaults;
        defaults.mode = hyprcapture::CaptureMode::Region;
        defaults.inPlaceEditToolbar = true;
        QImage expected(QSize(kLogicalWidth, kLogicalHeight) * 2, QImage::Format_RGBA8888);
        for (int y = 0; y < expected.height(); ++y)
            for (int x = 0; x < expected.width(); ++x)
                expected.setPixelColor(x, y, QColor(x % 251, y % 253, (x + y) % 255));
        const QString path = writeArtifact(topDown ? expected : expected.flipped(Qt::Vertical));
        QVERIFY(!path.isEmpty());
        hyprcapture::CaptureSession session;
        session.id = "raw-row-ownership-test";
        session.defaults = defaults;
        hyprcapture::MonitorInfo monitor;
        monitor.logicalGeometry = {0, 0, kLogicalWidth, kLogicalHeight};
        monitor.scale = 2;
        monitor.artifactPath = path.toStdString();
        monitor.artifactWidth = expected.width();
        monitor.artifactHeight = expected.height();
        monitor.artifactTopDown = topDown;
        session.monitors.push_back(monitor);
        CaptureOverlay overlay(defaults, false, false, false, QString::fromStdString(hyprcapture::encodeSessionJson(session)));
        QVERIFY(!QFile::exists(path));
        QCOMPARE(overlay.property("overlayOpacity").toDouble(), 1.0);
        overlay.show();
        QTest::qWait(30);
        selectRegion(overlay, QPoint(200, 180), QPoint(349, 279));
        auto* editor = overlay.findChild<AnnotationEditor*>("inPlaceEditor");
        QVERIFY(editor && editor->isVisible());
        QCOMPARE(editor->resultImage().convertToFormat(QImage::Format_RGBA8888), expected.copy(QRect(400, 360, 300, 200)));
    }

    void windowBackgroundRetainsNativeImageAndAnnotations() {
        QTemporaryDir output;
        QVERIFY(output.isValid());
        hyprcapture::CaptureDefaults defaults;
        defaults.mode = hyprcapture::CaptureMode::Window;
        defaults.windowBackground = hyprcapture::WindowBackground::Transparent;
        defaults.inPlaceEditToolbar = true;
        defaults.confirmBeforeCapture = true;
        defaults.saveDir = output.path().toStdString();
        defaults.screenshotNotification = false;
        const QString json = sessionJson(defaults);
        QVERIFY(!json.isEmpty());

        CaptureOverlay overlay(defaults, false, false, false, json);
        QSignalSpy finishing(&overlay, &CaptureOverlay::finishingStarted);
        QApplication::clipboard()->setText(QStringLiteral("clipboard-before-editor"));
        overlay.show();
        QTest::qWait(30);
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, QPoint(150, 150));
        auto* editor = overlay.findChild<AnnotationEditor*>(QStringLiteral("inPlaceEditor"));
        QVERIFY(editor);
        QTRY_VERIFY(editor->isVisible());
        QVERIFY(overlay.isVisible());
        verifyEditorToolbarCluster(overlay, *editor);
        auto* confirm = editor->findChild<QAbstractButton*>(QStringLiteral("annotationConfirm"));
        auto* cancel = editor->findChild<QAbstractButton*>(QStringLiteral("annotationCancel"));
        auto* previousCancel = overlay.findChild<QAbstractButton*>(QStringLiteral("captureCancel"));
        QVERIFY(confirm && confirm->isVisible());
        QVERIFY(cancel && cancel->isVisible());
        QVERIFY(previousCancel && !previousCancel->isVisible());
        for (const char* oldOutput : {"annotationCopy", "annotationSave", "annotationPin"}) {
            auto* button = editor->findChild<QAbstractButton*>(QString::fromLatin1(oldOutput));
            QVERIFY(!button || !button->isVisible());
        }
        QCOMPARE(finishing.count(), 0);
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("clipboard-before-editor"));
        QVERIFY(QDir(output.path()).entryList(QDir::Files).isEmpty());

        const QImage transparent = editor->resultImage();
        QCOMPARE(transparent.size(), kWindowPixels);
        QCOMPARE(transparent.pixelColor(8, 8).alpha(), 0);
        QCOMPARE(transparent.pixelColor(50, 40).alpha(), 128);
        QCOMPARE(transparent.pixelColor(200, 100), QColor(34, 149, 98));

        auto* canvas = editor->findChild<QWidget*>(QStringLiteral("annotationCanvas"));
        auto* pen = editor->findChild<QToolButton*>(QStringLiteral("annotationTool5"));
        QVERIFY(canvas);
        QVERIFY(pen);
        if (qEnvironmentVariable("HYPRCAPTURE_TEST_LANGUAGE") == QStringLiteral("zh_CN")) {
            QCOMPARE(hyprcapture::ui::activeUiLanguage(), QStringLiteral("zh_CN"));
            QCOMPARE(pen->accessibleName(), QStringLiteral("画笔"));
            auto* windowMode = modeButton(overlay, QStringLiteral("window"));
            QVERIFY(windowMode);
            QCOMPARE(windowMode->accessibleName(), QStringLiteral("窗口"));
        }
        QTest::mouseClick(pen, Qt::LeftButton);
        const QRect display = editor->canvasGeometry();
        QVERIFY(display.isValid());
        const auto canvasPoint = [&](const QPoint& imagePoint) {
            const QPoint inEditor(display.x() + imagePoint.x() * display.width() / kWindowPixels.width(),
                                  display.y() + imagePoint.y() * display.height() / kWindowPixels.height());
            return canvas->mapFrom(editor, inEditor);
        };
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, canvasPoint(QPoint(120, 90)));
        QTest::mouseMove(canvas, canvasPoint(QPoint(260, 140)));
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, canvasPoint(QPoint(260, 140)));
        const QImage annotated = editor->resultImage();
        QVERIFY(annotated.copy(kOpaquePatch) != transparent.copy(kOpaquePatch));

        QVERIFY(chooseBackground(overlay, QStringLiteral("white")));
        const QImage white = editor->resultImage();
        QCOMPARE(white.size(), kWindowPixels);
        QCOMPARE(white.pixelColor(8, 8), QColor(Qt::white));
        QCOMPARE(white.copy(kOpaquePatch), annotated.copy(kOpaquePatch));
        QVERIFY(chooseBackground(overlay, QStringLiteral("transparent")));
        QCOMPARE(editor->resultImage(), annotated);
        editor->undo();
        QCOMPARE(editor->resultImage(), transparent);
        editor->redo();
        QCOMPARE(editor->resultImage(), annotated);
        auto* sameMode = modeButton(overlay, QStringLiteral("window"));
        QVERIFY(sameMode);
        QTest::mouseClick(sameMode, Qt::LeftButton);
        QVERIFY(editor->isVisible());
        QCOMPARE(editor->resultImage(), annotated);
        const QString screenshot = qEnvironmentVariable("HYPRCAPTURE_TEST_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            QTest::qWait(30);
            QVERIFY(overlay.grab().save(screenshot));
        }
        QCOMPARE(finishing.count(), 0);
        QVERIFY(QDir(output.path()).entryList(QDir::Files).isEmpty());
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("clipboard-before-editor"));
    }

    void realBackgroundIsHydratedWithoutReplacingForeground() {
        QTemporaryDir commands;
        QVERIFY(commands.isValid());
        hyprcapture::CaptureDefaults defaults;
        defaults.mode = hyprcapture::CaptureMode::Window;
        defaults.windowBackground = hyprcapture::WindowBackground::Transparent;
        defaults.inPlaceEditToolbar = true;
        const QString initial = sessionJson(defaults);
        auto response = hyprcapture::decodeSessionJson(initial.toStdString());
        QVERIFY(response.has_value());
        response->monitors.clear();
        QImage changedForeground(kWindowPixels, QImage::Format_RGBA8888);
        changedForeground.fill(Qt::magenta);
        QImage background(kWindowPixels, QImage::Format_RGBA8888);
        background.fill(QColor(19, 63, 117));
        auto& window = response->windows.front();
        window.artifactPath = writeArtifact(changedForeground).toStdString();
        window.realBackgroundPath = writeArtifact(background).toStdString();
        window.realBackgroundWidth = background.width();
        window.realBackgroundHeight = background.height();
        QFile responseFile(QDir::homePath() + "/.nix-profile/bin/response.json");
        QVERIFY(responseFile.open(QIODevice::WriteOnly));
        const auto bytes = hyprcapture::encodeSessionJson(*response);
        responseFile.write(bytes.data(), bytes.size()); responseFile.close();
        QFile hyprctl(QDir::homePath() + "/.nix-profile/bin/hyprctl");
        const auto removeStub = qScopeGuard([&] { hyprctl.remove(); responseFile.remove(); });
        QVERIFY(hyprctl.open(QIODevice::WriteOnly));
        hyprctl.write("#!/usr/bin/python3\nimport json,pathlib,sys\n"
                      "expr=sys.argv[-1]\n"
                      "if 'window_capture(' in expr:\n"
                      " p=pathlib.Path(json.loads(expr.split('(',1)[1][:-1]))\n"
                      " p.write_bytes(pathlib.Path(__file__).with_name('response.json').read_bytes())\n"
                      "print('ok')\n");
        hyprctl.close();
        QVERIFY(hyprctl.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        CaptureOverlay overlay(defaults, false, false, false, initial);
        overlay.show(); QTest::qWait(30);
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, QPoint(150,150));
        auto* editor = overlay.findChild<AnnotationEditor*>("inPlaceEditor");
        QVERIFY(editor);
        const auto original = editor->resultImage();
        QVERIFY(chooseBackground(overlay, "real"));
        const auto composed = editor->resultImage();
        QCOMPARE(composed.pixelColor(8,8), QColor(19,63,117));
        QCOMPARE(composed.copy(kOpaquePatch), original.copy(kOpaquePatch));
        QVERIFY(chooseBackground(overlay, "transparent"));
        QCOMPARE(editor->resultImage(), original);
    }

    void regionEditorResizesEveryEdgeAndCorner_data() {
        QTest::addColumn<QPoint>("handle");
        QTest::addColumn<QPoint>("delta");
        QTest::addColumn<QRect>("expected");
        QTest::newRow("left") << QPoint(200, 220) << QPoint(-30, 0) << QRect(170, 180, 180, 100);
        QTest::newRow("right") << QPoint(349, 220) << QPoint(30, 0) << QRect(200, 180, 180, 100);
        QTest::newRow("top") << QPoint(270, 180) << QPoint(0, -30) << QRect(200, 150, 150, 130);
        QTest::newRow("bottom") << QPoint(270, 279) << QPoint(0, 30) << QRect(200, 180, 150, 130);
        QTest::newRow("top-left") << QPoint(200, 180) << QPoint(-30, -30) << QRect(170, 150, 180, 130);
        QTest::newRow("top-right") << QPoint(349, 180) << QPoint(30, -30) << QRect(200, 150, 180, 130);
        QTest::newRow("bottom-left") << QPoint(200, 279) << QPoint(-30, 30) << QRect(170, 180, 180, 130);
        QTest::newRow("bottom-right") << QPoint(349, 279) << QPoint(30, 30) << QRect(200, 180, 180, 130);
    }

    void regionEditorResizesEveryEdgeAndCorner() {
        QFETCH(QPoint, handle); QFETCH(QPoint, delta); QFETCH(QRect, expected);
        hyprcapture::CaptureDefaults defaults;
        defaults.mode = hyprcapture::CaptureMode::Region;
        defaults.inPlaceEditToolbar = true;
        CaptureOverlay overlay(defaults, false, false, false, sessionJson(defaults));
        overlay.show(); QTest::qWait(30);
        selectRegion(overlay, QPoint(200, 180), QPoint(349, 279));
        auto* editor = overlay.findChild<AnnotationEditor*>("inPlaceEditor");
        QVERIFY(editor && editor->isVisible());
        auto* canvas = editor->findChild<QWidget*>("annotationCanvas");
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, handle);
        QTest::mouseMove(canvas, handle + delta);
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, handle + delta);
        QCOMPARE(editor->canvasGeometry(), expected);
        QCOMPARE(editor->resultImage().size(), expected.size() * 2);
        verifyEditorToolbarCluster(overlay, *editor);
    }

    void regionResizePreservesAnnotationsAndUndo() {
        hyprcapture::CaptureDefaults defaults;
        defaults.mode = hyprcapture::CaptureMode::Region;
        defaults.inPlaceEditToolbar = true;
        CaptureOverlay overlay(defaults, false, false, false, sessionJson(defaults));
        overlay.show(); QTest::qWait(30);
        selectRegion(overlay, QPoint(200, 180), QPoint(349, 279));
        auto* editor = overlay.findChild<AnnotationEditor*>("inPlaceEditor");
        QVERIFY(editor);
        auto* canvas = editor->findChild<QWidget*>("annotationCanvas");
        auto* pen = editor->findChild<QToolButton*>("annotationTool5");
        QVERIFY(pen); QTest::mouseClick(pen, Qt::LeftButton);
        const auto original = editor->resultImage();
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, QPoint(230, 215));
        QTest::mouseMove(canvas, QPoint(285, 235));
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, QPoint(285, 235));
        const auto annotated = editor->resultImage();
        QVERIFY(annotated != original);
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, QPoint(200, 180));
        QTest::mouseMove(canvas, QPoint(170, 150));
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, QPoint(170, 150));
        QCOMPARE(editor->resultImage().copy(QRect(QPoint(60, 60), annotated.size())), annotated);
        editor->undo();
        QCOMPARE(editor->resultImage().copy(QRect(QPoint(60, 60), original.size())), original);
        editor->redo();
        QCOMPARE(editor->resultImage().copy(QRect(QPoint(60, 60), annotated.size())), annotated);
    }

    void regionResizeClampsAndWorksAfterZoom() {
        hyprcapture::CaptureDefaults defaults;
        defaults.mode = hyprcapture::CaptureMode::Region;
        defaults.inPlaceEditToolbar = true;
        CaptureOverlay overlay(defaults, false, false, false, sessionJson(defaults));
        overlay.show(); QTest::qWait(30);
        selectRegion(overlay, QPoint(200, 180), QPoint(349, 279));
        auto* editor = overlay.findChild<AnnotationEditor*>("inPlaceEditor");
        QVERIFY(editor);
        auto* canvas = editor->findChild<QWidget*>("annotationCanvas");
        const QPoint center = editor->canvasGeometry().center();
        QWheelEvent wheel(center, canvas->mapToGlobal(center), {}, QPoint(0, 120), Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(canvas, &wheel);
        const QRect zoomed = editor->canvasGeometry();
        QVERIFY(zoomed.width() > 150);
        const QPoint handle(zoomed.right(), zoomed.center().y());
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, handle);
        QTest::mouseMove(canvas, handle + QPoint(30, 0));
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, handle + QPoint(30, 0));
        QCOMPARE(editor->canvasGeometry().topLeft(), zoomed.topLeft());
        QVERIFY(std::abs(editor->canvasGeometry().right() - zoomed.right() - 30) <= 2);
        QVERIFY(editor->resultImage().width() > 300);
        // Left edge cannot cross the active capture bounds.
        const QPoint left(editor->canvasGeometry().left(), editor->canvasGeometry().center().y());
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, left);
        QTest::mouseMove(canvas, QPoint(-1000, left.y()));
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, QPoint(-1000, left.y()));
        QVERIFY(editor->resultImage().width() <= 1600);
        QVERIFY(editor->resultImage().width() > 600);
    }

    void stageOutlineScalesRoundingBeforeClipping_data() {
        QTest::addColumn<bool>("authoritative");
        QTest::newRow("legacy-scaled") << false;
        QTest::newRow("rendered-radius") << true;
    }
    void stageOutlineScalesRoundingBeforeClipping() {
        QFETCH(bool, authoritative);
        QTemporaryDir commands;
        QFile hyprctl(commands.filePath("hyprctl"));
        QVERIFY(hyprctl.open(QIODevice::WriteOnly));
        hyprctl.write("#!/bin/sh\nprintf 'ok\\n'\n");
        hyprctl.close();
        QVERIFY(hyprctl.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        const QByteArray oldPath = qgetenv("PATH");
        qputenv("PATH", commands.path().toUtf8());
        const auto restorePath = qScopeGuard([&] { qputenv("PATH", oldPath); });
        hyprcapture::CaptureDefaults defaults;
        defaults.mode = hyprcapture::CaptureMode::Window;
        defaults.windowBackground = hyprcapture::WindowBackground::Transparent;
        const QRect thumbnail(12, 80, 90, 66);
        auto session = hyprcapture::decodeSessionJson(sessionJson(defaults, true, QSize(800,600),
            QRect(1100,300,300,220), thumbnail).toStdString());
        QVERIFY(session.has_value());
        session->windows.front().rounding = authoritative ? 100 : 40;
        if (authoritative)
            session->windows.front().selectionRounding = 12;
        session->windows.front().selectionClipGeometry->width /= 2;
        CaptureOverlay overlay(defaults, false, false, false, QString::fromStdString(hyprcapture::encodeSessionJson(*session)));
        overlay.show(); QTest::qWait(30);
        const auto image = overlay.grab().toImage();
        const auto ratio = image.devicePixelRatio();
        int brightness = 0;
        for (int y = 0; y < 2; ++y) {
            const auto color = image.pixelColor(qRound((thumbnail.x()+20)*ratio), qRound((thumbnail.y()+y)*ratio));
            brightness = std::max(brightness, color.red());
        }
        // Radius 40 becomes 12 at 30% scale. The old native-sized radius
        // curves away here; clipping the preview must not halve it again.
        QVERIFY2(brightness > 70, "Stage outline still uses the native window radius");
    }

    void stageWindowOpensCenteredAtNativeResolution_data() {
        QTest::addColumn<bool>("fullscreen");
        QTest::addColumn<bool>("fusion");
        QTest::newRow("window-regular") << false << false;
        QTest::newRow("window-fullscreen-client") << true << false;
        QTest::newRow("fusion-regular") << false << true;
        QTest::newRow("fusion-fullscreen-client") << true << true;
    }
    void stageWindowOpensCenteredAtNativeResolution() {
        QFETCH(bool, fullscreen);
        QFETCH(bool, fusion);
        // The input-suppression handshake must never touch a real compositor
        // from this offscreen test.
        QTemporaryDir commands;
        QVERIFY(commands.isValid());
        QFile hyprctl(commands.filePath("hyprctl"));
        QVERIFY(hyprctl.open(QIODevice::WriteOnly));
        hyprctl.write("#!/bin/sh\nprintf 'ok\\n'\n");
        hyprctl.close();
        QVERIFY(hyprctl.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        const QByteArray oldPath = qgetenv("PATH");
        qputenv("PATH", commands.path().toUtf8());
        const auto restorePath = qScopeGuard([&] { qputenv("PATH", oldPath); });
        hyprcapture::CaptureDefaults defaults;
        defaults.mode = hyprcapture::CaptureMode::Window;
        defaults.inPlaceEditToolbar = true;
        defaults.captureFullscreenClientsAsMonitor = true;
        defaults.fushionMode = fusion;
        defaults.windowBackground = hyprcapture::WindowBackground::Transparent;
        const QRect nativeWindow(1100, 300, 300, 220);
        const QRect thumbnail(12, 80, 90, 66);
        auto session = hyprcapture::decodeSessionJson(sessionJson(defaults, true, QSize(800, 600), nativeWindow, thumbnail, fullscreen).toStdString());
        QVERIFY(session.has_value());
        session->windows.front().selectionClipGeometry->width /= 2;
        CaptureOverlay overlay(defaults, false, false, false, QString::fromStdString(hyprcapture::encodeSessionJson(*session)));
        overlay.show(); QTest::qWait(30);
        if (!fusion) {
            QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, QPoint(thumbnail.right() - 2, thumbnail.center().y()));
            QVERIFY(!overlay.findChild<AnnotationEditor*>("inPlaceEditor"));
        }
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, QPoint(thumbnail.left() + thumbnail.width() / 4, thumbnail.center().y()));
        auto* editor = overlay.findChild<AnnotationEditor*>("inPlaceEditor");
        QVERIFY(editor && editor->isVisible());
        const QRect canvas = editor->canvasGeometry();
        QVERIFY(overlay.rect().contains(canvas));
        QVERIFY(qAbs(canvas.center().x() - overlay.rect().center().x()) <= 2);
        QVERIFY(canvas.width() > thumbnail.width());
        QCOMPARE(editor->resultImage().size(), nativeWindow.size() * 2);
        QVERIFY(chooseBackground(overlay, QStringLiteral("white")));
        QCOMPARE(editor->canvasGeometry(), canvas);
        QCOMPARE(editor->resultImage().size(), nativeWindow.size() * 2);
    }

    void windowEditorRetainsOffscreenPosition_data() {
        QTest::addColumn<QRect>("window");
        QTest::newRow("left") << QRect(-40, 100, 300, 220);
        QTest::newRow("right-bottom") << QRect(600, 450, 300, 220);
    }
    void windowEditorRetainsOffscreenPosition() {
        QFETCH(QRect, window);
        hyprcapture::CaptureDefaults defaults;
        defaults.mode = hyprcapture::CaptureMode::Window;
        defaults.inPlaceEditToolbar = true;
        CaptureOverlay overlay(defaults, false, false, false, sessionJson(defaults, true, QSize(800, 600), window));
        overlay.show(); QTest::qWait(30);
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, window.intersected(overlay.rect()).center());
        auto* editor = overlay.findChild<AnnotationEditor*>("inPlaceEditor");
        QVERIFY(editor && editor->isVisible());
        QCOMPARE(editor->canvasGeometry(), window);
        QCOMPARE(editor->resultImage().size(), window.size() * 2);
        QVERIFY(chooseBackground(overlay, QStringLiteral("white")));
        QCOMPARE(editor->canvasGeometry(), window);
    }

    void modeChangeAndReselectReturnToSelection() {
        hyprcapture::CaptureDefaults defaults;
        defaults.mode = hyprcapture::CaptureMode::Window;
        defaults.windowBackground = hyprcapture::WindowBackground::Transparent;
        defaults.inPlaceEditToolbar = true;
        const QString json = sessionJson(defaults);
        QVERIFY(!json.isEmpty());
        CaptureOverlay overlay(defaults, false, false, false, json);
        QSignalSpy finishing(&overlay, &CaptureOverlay::finishingStarted);
        overlay.show();
        QTest::qWait(30);
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, QPoint(150, 150));
        auto* editor = overlay.findChild<AnnotationEditor*>(QStringLiteral("inPlaceEditor"));
        QVERIFY(editor);
        QTRY_VERIFY(editor->isVisible());
        auto* region = modeButton(overlay, QStringLiteral("region"));
        QVERIFY(region);
        QTest::mouseClick(region, Qt::LeftButton);
        QVERIFY(!editor->isVisible());
        QVERIFY(overlay.isVisible());
        selectRegion(overlay, QPoint(350, 100), QPoint(449, 199));
        QTRY_VERIFY(editor->isVisible());
        QCOMPARE(editor->resultImage().size(), QSize(200, 200));
        QCOMPARE(editor->resultImage().pixelColor(50, 50), QColor(17, 29, 53));
        QVERIFY(clickEditorAction(*editor, "annotationReselect"));
        QVERIFY(!editor->isVisible());
        selectRegion(overlay, QPoint(400, 150), QPoint(499, 249));
        QTRY_VERIFY(editor->isVisible());
        QCOMPARE(editor->resultImage().size(), QSize(200, 200));
        QCOMPARE(finishing.count(), 0);
    }

    void toolbarClusterTracksCapture_data() {
        QTest::addColumn<QRect>("selection");
        QTest::addColumn<bool>("below");
        QTest::newRow("below-left-edge") << QRect(20, 60, 120, 90) << true;
        QTest::newRow("below-right-edge") << QRect(650, 80, 130, 90) << true;
        QTest::newRow("above-bottom-edge") << QRect(580, 420, 120, 130) << false;
    }

    void toolbarClusterTracksCapture() {
        QFETCH(QRect, selection);
        QFETCH(bool, below);
        hyprcapture::CaptureDefaults defaults;
        defaults.mode = hyprcapture::CaptureMode::Region;
        defaults.inPlaceEditToolbar = true;
        const QString json = sessionJson(defaults);
        QVERIFY(!json.isEmpty());
        CaptureOverlay overlay(defaults, false, false, false, json);
        QSignalSpy finishing(&overlay, &CaptureOverlay::finishingStarted);
        overlay.show();
        QTest::qWait(30);
        selectRegion(overlay, selection.topLeft(), selection.bottomRight());
        auto* editor = overlay.findChild<AnnotationEditor*>(QStringLiteral("inPlaceEditor"));
        QVERIFY(editor);
        QTRY_VERIFY(editor->isVisible());
        QCOMPARE(editor->canvasGeometry(), selection);
        verifyEditorToolbarCluster(overlay, *editor);
        const QRect cluster = editorToolbarCluster(overlay, *editor);
        QVERIFY(below ? cluster.top() > selection.bottom() : cluster.bottom() < selection.top());
        QCOMPARE(editor->resultImage().size(), selection.size() * 2);
        QCOMPARE(finishing.count(), 0);
    }

    void zoomAndPanMoveCaptureToolbarCluster() {
        hyprcapture::CaptureDefaults defaults;
        defaults.mode = hyprcapture::CaptureMode::Window;
        defaults.windowBackground = hyprcapture::WindowBackground::Transparent;
        defaults.inPlaceEditToolbar = true;
        const QString json = sessionJson(defaults);
        QVERIFY(!json.isEmpty());
        CaptureOverlay overlay(defaults, false, false, false, json);
        QSignalSpy finishing(&overlay, &CaptureOverlay::finishingStarted);
        overlay.show();
        QTest::qWait(30);
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, QPoint(150, 150));
        auto* editor = overlay.findChild<AnnotationEditor*>(QStringLiteral("inPlaceEditor"));
        QVERIFY(editor);
        QTRY_VERIFY(editor->isVisible());
        auto* canvas = editor->findChild<QWidget*>(QStringLiteral("annotationCanvas"));
        QVERIFY(canvas);
        const QImage native = editor->resultImage();
        const QRect imageBefore = editor->canvasGeometry();
        const QRect clusterBefore = editorToolbarCluster(overlay, *editor);
        const QPoint zoomPoint = imageBefore.center();
        QWheelEvent zoom(zoomPoint, canvas->mapToGlobal(zoomPoint), {}, QPoint(0, 120),
                         Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(canvas, &zoom);
        QTRY_VERIFY(editor->canvasGeometry().width() > imageBefore.width());
        QTRY_VERIFY(editorToolbarCluster(overlay, *editor) != clusterBefore);
        verifyEditorToolbarCluster(overlay, *editor);
        const QRect zoomedImage = editor->canvasGeometry();
        const QRect zoomedCluster = editorToolbarCluster(overlay, *editor);
        const QPoint start = zoomedImage.center();
        const QPoint delta(40, 50);
        QTest::mousePress(canvas, Qt::MiddleButton, Qt::NoModifier, start);
        QTest::mouseMove(canvas, start + delta);
        QTest::mouseRelease(canvas, Qt::MiddleButton, Qt::NoModifier, start + delta);
        QTRY_COMPARE(editor->canvasGeometry().topLeft(), zoomedImage.topLeft() + delta);
        QTRY_VERIFY(editorToolbarCluster(overlay, *editor) != zoomedCluster);
        verifyEditorToolbarCluster(overlay, *editor);
        QCOMPARE(editor->resultImage(), native);
        QCOMPARE(finishing.count(), 0);
    }

    void disabledOptionRetainsLegacyFinish() {
        hyprcapture::CaptureDefaults defaults;
        defaults.mode = hyprcapture::CaptureMode::Window;
        defaults.inPlaceEditToolbar = false;
        defaults.save = false;
        defaults.clipboard = false;
        defaults.showThumbnail = false;
        defaults.screenshotNotification = false;
        const QString json = sessionJson(defaults);
        QVERIFY(!json.isEmpty());
        CaptureOverlay overlay(defaults, false, false, false, json);
        QSignalSpy finishing(&overlay, &CaptureOverlay::finishingStarted);
        overlay.show();
        QTest::qWait(30);
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, QPoint(150, 150));
        QCOMPARE(finishing.count(), 1);
        auto* editor = overlay.findChild<AnnotationEditor*>(QStringLiteral("inPlaceEditor"));
        QVERIFY(!editor || !editor->isVisible());
        // Scope destruction cancels the fade callback before legacy output runs.
    }

    void missingWindowDoesNotBeginEditingOrOutput() {
        hyprcapture::CaptureDefaults defaults;
        defaults.mode = hyprcapture::CaptureMode::Window;
        defaults.inPlaceEditToolbar = true;
        const QString json = sessionJson(defaults, false);
        QVERIFY(!json.isEmpty());
        CaptureOverlay overlay(defaults, false, false, false, json);
        QSignalSpy finishing(&overlay, &CaptureOverlay::finishingStarted);
        overlay.show();
        QTest::qWait(30);
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, QPoint(150, 150));
        auto* editor = overlay.findChild<AnnotationEditor*>(QStringLiteral("inPlaceEditor"));
        QVERIFY(!editor || !editor->isVisible());
        QVERIFY(overlay.isVisible());
        QCOMPARE(finishing.count(), 0);
    }

    void recordingBypassesEditor() {
        hyprcapture::CaptureDefaults defaults;
        defaults.mode = hyprcapture::CaptureMode::Fullscreen;
        defaults.inPlaceEditToolbar = true;
        defaults.recordCountdownSeconds = 0;
        const QString json = sessionJson(defaults);
        QVERIFY(!json.isEmpty());
        const QString runtime = QFileInfo(hyprcapture::ui::runtimeFile(QStringLiteral("probe"), QStringLiteral(".txt"))).absolutePath();
        const auto before = QDir(runtime).entryList({QStringLiteral("record-request-*.json")}, QDir::Files);
        {
            CaptureOverlay overlay(defaults, false, true, false, json);
            QSignalSpy finishing(&overlay, &CaptureOverlay::finishingStarted);
            overlay.show();
            QTest::qWait(30);
            QTest::keyClick(&overlay, Qt::Key_Return);
            QCOMPARE(finishing.count(), 1);
            auto* editor = overlay.findChild<AnnotationEditor*>(QStringLiteral("inPlaceEditor"));
            QVERIFY(!editor || !editor->isVisible());
            // The compositor dispatch timer runs after 120 ms. Destroy its owner
            // without processing events so this test never starts a recording.
        }
        const auto after = QDir(runtime).entryList({QStringLiteral("record-request-*.json")}, QDir::Files);
        bool requestWritten = false;
        for (const QString& name : after) {
            if (!before.contains(name)) {
                requestWritten = true;
                QFile::remove(QDir(runtime).filePath(name));
            }
        }
        QVERIFY(requestWritten);
    }

    void successfulPinUsesNormalOutputFlow_data() {
        QTest::addColumn<bool>("save");
        QTest::addColumn<bool>("clipboard");
        QTest::addColumn<bool>("thumbnail");
        QTest::newRow("all-outputs") << true << true << true;
        QTest::newRow("clipboard-and-thumbnail") << false << true << true;
        QTest::newRow("outputs-disabled") << false << false << false;
    }
    void successfulPinUsesNormalOutputFlow() {
        QFETCH(bool, save); QFETCH(bool, clipboard); QFETCH(bool, thumbnail);
        QTemporaryDir artifacts, output;
        QVERIFY(artifacts.isValid() && output.isValid());
        const QString pinPath = artifacts.filePath("pinned.png");
        const QString thumbnailPath = artifacts.filePath("thumbnail.png");
        const QString clipboardPath = qEnvironmentVariable("HYPRCAPTURE_TEST_CLIPBOARD");
        QFile::remove(clipboardPath);
        qputenv("HYPRCAPTURE_TEST_PIN_SUCCESS", pinPath.toUtf8());
        qputenv("HYPRCAPTURE_TEST_THUMBNAIL", thumbnailPath.toUtf8());
        const auto reset = qScopeGuard([] {
            qunsetenv("HYPRCAPTURE_TEST_PIN_SUCCESS");
            qunsetenv("HYPRCAPTURE_TEST_THUMBNAIL");
        });
        hyprcapture::CaptureDefaults defaults;
        defaults.mode = hyprcapture::CaptureMode::Window;
        defaults.inPlaceEditToolbar = true;
        defaults.windowBackground = hyprcapture::WindowBackground::Transparent;
        defaults.save = save; defaults.clipboard = clipboard; defaults.showThumbnail = thumbnail;
        defaults.screenshotNotification = false;
        defaults.saveDir = output.path().toStdString();
        CaptureOverlay overlay(defaults, false, false, false, sessionJson(defaults));
        QSignalSpy finishing(&overlay, &CaptureOverlay::finishingStarted);
        overlay.show(); QTest::qWait(30);
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, QPoint(150, 150));
        auto* editor = overlay.findChild<AnnotationEditor*>("inPlaceEditor");
        QVERIFY(editor);
        auto* canvas = editor->findChild<QWidget*>("annotationCanvas");
        auto* pen = editor->findChild<QToolButton*>("annotationTool5");
        QVERIFY(pen); QTest::mouseClick(pen, Qt::LeftButton);
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, QPoint(150, 140));
        QTest::mouseMove(canvas, QPoint(200, 160));
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, QPoint(200, 160));
        const auto expected = editor->resultImage().convertToFormat(QImage::Format_RGBA8888);
        const QRect expectedGeometry(editor->mapToGlobal(editor->canvasGeometry().topLeft()), editor->canvasGeometry().size());
        QVERIFY(clickEditorAction(*editor, "annotationPin"));
        QTRY_COMPARE_WITH_TIMEOUT(finishing.count(), 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!overlay.isVisible(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!QImage(pinPath).isNull(), 2000);
        QCOMPARE(QImage(pinPath).convertToFormat(QImage::Format_RGBA8888), expected);
        QFile geometryFile(pinPath + ".geometry");
        QVERIFY(geometryFile.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(geometryFile.readAll()), QStringLiteral("%1,%2,%3,%4")
            .arg(expectedGeometry.x()).arg(expectedGeometry.y()).arg(expectedGeometry.width()).arg(expectedGeometry.height()));
        const auto saved = QDir(output.path()).entryList({"*.png"}, QDir::Files);
        QCOMPARE(saved.size(), save ? 1 : 0);
        if (save) QCOMPARE(QImage(output.filePath(saved.front())).convertToFormat(QImage::Format_RGBA8888), expected);
        if (clipboard) {
            QTRY_VERIFY_WITH_TIMEOUT(!QImage(clipboardPath).isNull(), 2000);
            QCOMPARE(QImage(clipboardPath).convertToFormat(QImage::Format_RGBA8888), expected);
        } else QVERIFY(!QFileInfo::exists(clipboardPath));
        if (thumbnail) {
            QTRY_VERIFY_WITH_TIMEOUT(!QImage(thumbnailPath).isNull(), 2000);
            QCOMPARE(QImage(thumbnailPath).convertToFormat(QImage::Format_RGBA8888), expected);
        } else QVERIFY(!QFileInfo::exists(thumbnailPath));
        QTest::qWait(30);
    }

    void failedPinPreservesEditorAndAnnotations() {
        QTemporaryDir output;
        QVERIFY(output.isValid());
        hyprcapture::CaptureDefaults defaults;
        defaults.mode = hyprcapture::CaptureMode::Window;
        defaults.windowBackground = hyprcapture::WindowBackground::Transparent;
        defaults.inPlaceEditToolbar = true;
        defaults.saveDir = output.path().toStdString();
        const QString json = sessionJson(defaults);
        QVERIFY(!json.isEmpty());
        CaptureOverlay overlay(defaults, false, false, false, json);
        QSignalSpy finishing(&overlay, &CaptureOverlay::finishingStarted);
        QApplication::clipboard()->setText(QStringLiteral("clipboard-before-pin"));
        overlay.show();
        QTest::qWait(30);
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, QPoint(150, 150));
        auto* editor = overlay.findChild<AnnotationEditor*>(QStringLiteral("inPlaceEditor"));
        QVERIFY(editor);
        QTRY_VERIFY(editor->isVisible());
        auto* canvas = editor->findChild<QWidget*>(QStringLiteral("annotationCanvas"));
        auto* pin = editor->findChild<QAbstractButton*>(QStringLiteral("annotationPin"));
        auto* reselect = editor->findChild<QPushButton*>(QStringLiteral("annotationReselect"));
        QVERIFY(canvas);
        QVERIFY(pin);
        QVERIFY(reselect);
        const QImage original = editor->resultImage();
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, QPoint(150, 140));
        QTest::mouseMove(canvas, QPoint(200, 160));
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, QPoint(200, 160));
        const QImage annotated = editor->resultImage();
        QVERIFY(annotated != original);
        QVERIFY(clickEditorAction(*editor, "annotationPin"));
        QTRY_VERIFY_WITH_TIMEOUT(editor->isEnabled(), 2000);
        QVERIFY(editor->isVisible());
        QVERIFY(overlay.isVisible());
        QVERIFY(reselect->isEnabled());
        QVERIFY(overlay.findChild<QWidget*>(QStringLiteral("toolbar"))->isEnabled());
        bool injectedFailureDisplayed = false;
        for (auto* label : overlay.findChildren<QLabel*>())
            injectedFailureDisplayed |= label->text().contains(QStringLiteral("Injected pin failure"));
        QVERIFY(injectedFailureDisplayed);
        QCOMPARE(editor->resultImage(), annotated);
        QCOMPARE(finishing.count(), 0);
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("clipboard-before-pin"));
        QVERIFY(QDir(output.path()).entryList(QDir::Files).isEmpty());
        const QString runtime = QFileInfo(hyprcapture::ui::runtimeFile(QStringLiteral("probe"), QStringLiteral(".txt"))).absolutePath();
        QVERIFY(QDir(runtime).entryList({QStringLiteral("pin-*.png"), QStringLiteral("pin-ready-*.socket")}, QDir::AllEntries).isEmpty());
        editor->undo();
        QCOMPARE(editor->resultImage(), original);
    }

    void failedSavePreservesEditorAndAnnotations() {
        QTemporaryDir output;
        QVERIFY(output.isValid());
        const QString blockerPath = output.filePath(QStringLiteral("not-a-directory"));
        QFile blocker(blockerPath);
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        QCOMPARE(blocker.write("existing-file"), qint64(13));
        blocker.close();
        hyprcapture::CaptureDefaults defaults;
        defaults.mode = hyprcapture::CaptureMode::Window;
        defaults.windowBackground = hyprcapture::WindowBackground::Transparent;
        defaults.inPlaceEditToolbar = true;
        defaults.clipboard = false;
        defaults.showThumbnail = false;
        defaults.screenshotNotification = false;
        defaults.saveDir = blockerPath.toStdString();
        const QString json = sessionJson(defaults);
        QVERIFY(!json.isEmpty());
        CaptureOverlay overlay(defaults, false, false, false, json);
        QSignalSpy finishing(&overlay, &CaptureOverlay::finishingStarted);
        overlay.show();
        QTest::qWait(30);
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, QPoint(150, 150));
        auto* editor = overlay.findChild<AnnotationEditor*>(QStringLiteral("inPlaceEditor"));
        QVERIFY(editor);
        QTRY_VERIFY(editor->isVisible());
        auto* canvas = editor->findChild<QWidget*>(QStringLiteral("annotationCanvas"));
        auto* save = editor->findChild<QAbstractButton*>(QStringLiteral("annotationConfirm"));
        QVERIFY(canvas);
        QVERIFY(save);
        const QImage original = editor->resultImage();
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, QPoint(150, 140));
        QTest::mouseMove(canvas, QPoint(200, 160));
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, QPoint(200, 160));
        const QImage annotated = editor->resultImage();
        QVERIFY(annotated != original);
        QTest::mouseClick(save, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(editor->isEnabled(), 2000);
        QVERIFY(editor->isVisible());
        QVERIFY(overlay.isVisible());
        QCOMPARE(editor->resultImage(), annotated);
        QCOMPARE(finishing.count(), 0);
        bool saveFailureDisplayed = false;
        const QString expectedError = hyprcapture::ui::uiText("Could not save the image. Check the output directory and try again.");
        for (auto* label : overlay.findChildren<QLabel*>())
            saveFailureDisplayed |= label->isVisible() && !label->text().isEmpty() && label->toolTip() == expectedError;
        QVERIFY(saveFailureDisplayed);
        QCOMPARE(QDir(output.path()).entryList(QDir::Files), QStringList{QStringLiteral("not-a-directory")});
        QVERIFY(blocker.open(QIODevice::ReadOnly));
        QCOMPARE(blocker.readAll(), QByteArrayLiteral("existing-file"));
        editor->undo();
        QCOMPARE(editor->resultImage(), original);
        // Drain the finished worker's deleteLater event before destroying its
        // callback receiver; the failure callback above has already completed.
        QTest::qWait(30);
    }

    void confirmationRespectsDisabledSave_data() {
        QTest::addColumn<bool>("clipboard");
        QTest::newRow("copy-only") << true;
        QTest::newRow("no-save-or-copy") << false;
    }

    void confirmationRespectsDisabledSave() {
        QFETCH(bool, clipboard);
        const QString clipboardPath = qEnvironmentVariable("HYPRCAPTURE_TEST_CLIPBOARD");
        QFile::remove(clipboardPath);
        QTemporaryDir output;
        QVERIFY(output.isValid());
        hyprcapture::CaptureDefaults defaults;
        defaults.mode = hyprcapture::CaptureMode::Window;
        defaults.windowBackground = hyprcapture::WindowBackground::Transparent;
        defaults.inPlaceEditToolbar = true;
        defaults.save = false;
        defaults.clipboard = clipboard;
        defaults.showThumbnail = false;
        defaults.screenshotNotification = false;
        defaults.saveDir = output.path().toStdString();
        const QString json = sessionJson(defaults);
        QVERIFY(!json.isEmpty());
        CaptureOverlay overlay(defaults, false, false, false, json);
        QSignalSpy finishing(&overlay, &CaptureOverlay::finishingStarted);
        QApplication::clipboard()->setText(QStringLiteral("clipboard-before-confirm"));
        overlay.show();
        QTest::qWait(30);
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, QPoint(150, 150));
        auto* editor = overlay.findChild<AnnotationEditor*>(QStringLiteral("inPlaceEditor"));
        QVERIFY(editor);
        QTRY_VERIFY(editor->isVisible());
        const QImage expected = editor->resultImage().convertToFormat(QImage::Format_RGBA8888);
        QVERIFY(clickEditorAction(*editor, "annotationConfirm"));
        QTRY_COMPARE_WITH_TIMEOUT(finishing.count(), 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!overlay.isVisible(), 2000);
        QVERIFY(QDir(output.path()).entryList(QDir::Files).isEmpty());
        if (clipboard) {
            QTRY_VERIFY_WITH_TIMEOUT(!QImage(clipboardPath).isNull(), 2000);
            QCOMPARE(QImage(clipboardPath).convertToFormat(QImage::Format_RGBA8888), expected);
        } else {
            QVERIFY(!QFileInfo::exists(clipboardPath));
            QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("clipboard-before-confirm"));
        }
        QTest::qWait(30);
    }

    void cancelButtonCancelsEntireCaptureWithoutOutput() {
        QTemporaryDir output;
        QVERIFY(output.isValid());
        hyprcapture::CaptureDefaults defaults;
        defaults.mode = hyprcapture::CaptureMode::Window;
        defaults.inPlaceEditToolbar = true;
        defaults.save = true;
        defaults.clipboard = true;
        defaults.showThumbnail = true;
        defaults.saveDir = output.path().toStdString();
        const QString json = sessionJson(defaults);
        QVERIFY(!json.isEmpty());
        CaptureOverlay overlay(defaults, false, false, false, json);
        QSignalSpy finishing(&overlay, &CaptureOverlay::finishingStarted);
        QApplication::clipboard()->setText(QStringLiteral("clipboard-before-cancel"));
        overlay.show();
        QTest::qWait(30);
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, QPoint(150, 150));
        auto* editor = overlay.findChild<AnnotationEditor*>(QStringLiteral("inPlaceEditor"));
        QVERIFY(editor);
        QTRY_VERIFY(editor->isVisible());
        QVERIFY(clickEditorAction(*editor, "annotationCancel"));
        QCOMPARE(finishing.count(), 1);
        QVERIFY(!editor->isEnabled());
        auto* confirm = editor->findChild<QAbstractButton*>(QStringLiteral("annotationConfirm"));
        QVERIFY(confirm);
        QVERIFY(!confirm->isEnabled());
        QTest::mouseClick(confirm, Qt::LeftButton);
        QTest::keyClick(&overlay, Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(!overlay.isVisible(), 2000);
        QCOMPARE(finishing.count(), 1);
        QVERIFY(QDir(output.path()).entryList(QDir::Files).isEmpty());
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("clipboard-before-cancel"));
    }

    void wideRenderedToolbarPreview() {
        const QString screenshot = qEnvironmentVariable("HYPRCAPTURE_TEST_WIDE_SCREENSHOT");
        if (screenshot.isEmpty())
            QSKIP("Set HYPRCAPTURE_TEST_WIDE_SCREENSHOT to render the desktop-width review fixture");
        hyprcapture::CaptureDefaults defaults;
        defaults.mode = hyprcapture::CaptureMode::Window;
        defaults.windowBackground = hyprcapture::WindowBackground::White;
        defaults.inPlaceEditToolbar = true;
        const QRect window(360, 180, 600, 360);
        const QString json = sessionJson(defaults, true, QSize(1440, 900), window);
        QVERIFY(!json.isEmpty());
        CaptureOverlay overlay(defaults, false, false, false, json);
        QSignalSpy finishing(&overlay, &CaptureOverlay::finishingStarted);
        overlay.show();
        QTest::qWait(30);
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, window.center());
        auto* editor = overlay.findChild<AnnotationEditor*>(QStringLiteral("inPlaceEditor"));
        QVERIFY(editor);
        QTRY_VERIFY(editor->isVisible());
        QCOMPARE(editor->canvasGeometry(), window);
        verifyEditorToolbarCluster(overlay, *editor);
        QVERIFY(editor->toolbarWidget()->height() <= 64);
        auto* canvas = editor->findChild<QWidget*>(QStringLiteral("annotationCanvas"));
        auto* pen = editor->findChild<QToolButton*>(QStringLiteral("annotationTool5"));
        QVERIFY(canvas && pen);
        QTest::mouseClick(pen, Qt::LeftButton);
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, window.topLeft() + QPoint(120, 120));
        QTest::mouseMove(canvas, window.topLeft() + QPoint(320, 200));
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, window.topLeft() + QPoint(320, 200));
        QTest::qWait(30);
        QVERIFY(overlay.grab().save(screenshot));
        QCOMPARE(finishing.count(), 0);
        const QString paletteScreenshot = qEnvironmentVariable("HYPRCAPTURE_TEST_PALETTE_SCREENSHOT");
        if (!paletteScreenshot.isEmpty()) {
            QVERIFY(clickEditorAction(*editor, "annotationColorTrigger"));
            auto* panel = editor->findChild<QWidget*>(QStringLiteral("annotationColorPanel"));
            QVERIFY(panel && panel->isVisible());
            QVERIFY(overlay.rect().contains(QRect(panel->mapTo(&overlay, QPoint()), panel->size())));
            const QImage edited = editor->resultImage();
            overlay.update();
            for (auto* widget : overlay.findChildren<QWidget*>()) widget->update();
            QTest::qWait(50);
            const QImage preview = overlay.grab().toImage();
            QCOMPARE(editor->resultImage(), edited);
            QCOMPARE(preview.pixelColor(window.center()), QColor(34, 149, 98));
            QVERIFY(preview.save(paletteScreenshot));
        }
    }

    void successfulSaveExportsNativeEditedPixels() {
        QTemporaryDir output;
        QVERIFY(output.isValid());
        hyprcapture::CaptureDefaults defaults;
        defaults.mode = hyprcapture::CaptureMode::Window;
        defaults.windowBackground = hyprcapture::WindowBackground::Transparent;
        defaults.inPlaceEditToolbar = true;
        defaults.clipboard = false;
        defaults.showThumbnail = false;
        defaults.screenshotNotification = false;
        defaults.filenameTemplate = "edited.png";
        defaults.saveDir = output.path().toStdString();
        const QString json = sessionJson(defaults);
        QVERIFY(!json.isEmpty());
        CaptureOverlay overlay(defaults, false, false, false, json);
        QSignalSpy finishing(&overlay, &CaptureOverlay::finishingStarted);
        QApplication::clipboard()->setText(QStringLiteral("clipboard-before-save"));
        overlay.show();
        QTest::qWait(30);
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, QPoint(150, 150));
        auto* editor = overlay.findChild<AnnotationEditor*>(QStringLiteral("inPlaceEditor"));
        QVERIFY(editor);
        QTRY_VERIFY(editor->isVisible());
        auto* canvas = editor->findChild<QWidget*>(QStringLiteral("annotationCanvas"));
        auto* save = editor->findChild<QAbstractButton*>(QStringLiteral("annotationConfirm"));
        QVERIFY(canvas);
        QVERIFY(save);
        const QImage original = editor->resultImage();
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, QPoint(150, 140));
        QTest::mouseMove(canvas, QPoint(200, 160));
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, QPoint(200, 160));
        const QImage annotated = editor->resultImage();
        QVERIFY(annotated != original);
        QCOMPARE(finishing.count(), 0);
        QVERIFY(QDir(output.path()).entryList(QDir::Files).isEmpty());
        bool exportedBeforeFinishing = false;
        connect(&overlay, &CaptureOverlay::finishingStarted, this, [&] {
            exportedBeforeFinishing = QFileInfo::exists(output.filePath(QStringLiteral("edited.png")));
        });
        QTest::mouseClick(save, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(finishing.count(), 1, 2000);
        QVERIFY(exportedBeforeFinishing);
        const QImage exported(output.filePath(QStringLiteral("edited.png")));
        QCOMPARE(exported.size(), kWindowPixels);
        QCOMPARE(exported.convertToFormat(QImage::Format_RGBA8888), annotated.convertToFormat(QImage::Format_RGBA8888));
        QCOMPARE(exported.pixelColor(8, 8).alpha(), 0);
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("clipboard-before-save"));
        QTest::qWait(30);
    }
};

int main(int argc, char** argv) {
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--pin-image")) {
        QCoreApplication child(argc, argv);
        const QStringList arguments = child.arguments();
        const int option = arguments.indexOf(QStringLiteral("--pin-ready-socket"));
        if (option < 0 || option + 1 >= arguments.size())
            return 1;
        QLocalSocket socket;
        socket.connectToServer(arguments.at(option + 1));
        if (!socket.waitForConnected(1000))
            return 1;
        const QString resultPath = qEnvironmentVariable("HYPRCAPTURE_TEST_PIN_SUCCESS");
        if (!resultPath.isEmpty()) {
            const QImage pinned(arguments.at(2));
            if (pinned.isNull()) return 1;
            const int geometryOption = arguments.indexOf(QStringLiteral("--pin-geometry"));
            if (geometryOption < 0 || geometryOption + 1 >= arguments.size()) return 1;
            QFile geometryFile(resultPath + ".geometry");
            if (!geometryFile.open(QIODevice::WriteOnly)) return 1;
            geometryFile.write(arguments.at(geometryOption + 1).toUtf8());
            geometryFile.close();
            socket.write("ready\n");
            socket.flush();
            if (socket.bytesToWrite() && !socket.waitForBytesWritten(1000)) return 1;
            QByteArray accepted;
            while (!accepted.contains('\n') && socket.waitForReadyRead(2000)) accepted += socket.readAll();
            return accepted.startsWith("accepted\n") && pinned.save(resultPath) ? 0 : 1;
        }
        socket.write("error:Injected pin failure\n");
        socket.flush();
        return socket.bytesToWrite() == 0 || socket.waitForBytesWritten(1000) ? 0 : 1;
    }
    if (argc > 2 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--thumbnail-window")) {
        QCoreApplication child(argc, argv);
        const QString target = qEnvironmentVariable("HYPRCAPTURE_TEST_THUMBNAIL");
        return !target.isEmpty() && QImage(QString::fromLocal8Bit(argv[2])).save(target) ? 0 : 1;
    }
    // Recording controls may enumerate sound sources. Keep that child process
    // isolated from host devices and prevent it from recursively running tests.
    if (argc > 1 && QString::fromLocal8Bit(argv[1]).startsWith(QStringLiteral("--sound-"))) {
        if (QString::fromLocal8Bit(argv[1]) == QStringLiteral("--sound-list"))
            fputs("{\"outputs\":[],\"inputs\":[],\"windows\":[]}\n", stdout);
        return 0;
    }
    // Use an isolated trusted HOME for the clipboard backend double. /tmp
    // is intentionally rejected by the production executable trust policy.
    QTemporaryDir clipboardHome(QDir::homePath() + QStringLiteral("/.hyprcapture-test-XXXXXX"));
    if (!clipboardHome.isValid()) return 1;
    const QString bin = clipboardHome.filePath(QStringLiteral(".nix-profile/bin"));
    if (!QDir().mkpath(bin)) return 1;
    const QString clipboardPath = clipboardHome.filePath(QStringLiteral("copied.png"));
    QFile copyStub(bin + QStringLiteral("/wl-copy"));
    if (!copyStub.open(QIODevice::WriteOnly)) return 1;
    QString quotedPath = clipboardPath;
    quotedPath.replace(QChar(0x27), QStringLiteral("'\\''"));
    QString cat = QStandardPaths::findExecutable(QStringLiteral("cat"));
    const QString shell = QStandardPaths::findExecutable(QStringLiteral("sh"));
    if (cat.isEmpty() || shell.isEmpty()) {
        fputs("clipboard fixture requires cat and sh on PATH\n", stderr);
        return 1;
    }
    cat.replace(QChar(0x27), QStringLiteral("'\\''"));
    copyStub.write((QStringLiteral("#!") + shell + QStringLiteral("\nexec '") + cat +
                    QStringLiteral("' > '") + quotedPath + QStringLiteral("'\n")).toUtf8());
    copyStub.close();
    if (!copyStub.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) return 1;
    qputenv("HOME", clipboardHome.path().toUtf8());
    qputenv("HYPRCAPTURE_TEST_CLIPBOARD", clipboardPath.toUtf8());
    QTemporaryDir environment;
    if (!environment.isValid())
        return 1;
    const QString runtime = environment.filePath(QStringLiteral("runtime"));
    if (!QDir().mkpath(runtime) || !QFile::setPermissions(runtime, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner))
        return 1;
    qputenv("XDG_RUNTIME_DIR", runtime.toUtf8());
    qputenv("XDG_CONFIG_HOME", environment.filePath(QStringLiteral("config")).toUtf8());
    qputenv("XDG_CACHE_HOME", environment.filePath(QStringLiteral("cache")).toUtf8());
    qputenv("XDG_DATA_HOME", environment.filePath(QStringLiteral("data")).toUtf8());
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    hyprcapture::ui::installUiTranslations(qEnvironmentVariable("HYPRCAPTURE_TEST_LANGUAGE", QStringLiteral("en")));
    InPlaceEditorTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "in_place_editor_test.moc"
