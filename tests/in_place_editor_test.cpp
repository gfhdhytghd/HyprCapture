#include "shared/protocol.hpp"
#include "ui/annotation_editor.hpp"
#include "ui/capture_overlay.hpp"
#include "ui/clipboard_utils.hpp"
#include "ui/i18n.hpp"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLocalSocket>
#include <QPainter>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
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

QString sessionJson(const hyprcapture::CaptureDefaults& defaults, bool includeWindow = true) {
    QImage desktop(QSize(kLogicalWidth * 2, kLogicalHeight * 2), QImage::Format_RGBA8888);
    desktop.fill(QColor(17, 29, 53));
    const QString desktopPath = writeArtifact(desktop);
    if (desktopPath.isEmpty())
        return {};

    hyprcapture::CaptureSession session;
    session.id = "in-place-test";
    session.defaults = defaults;
    session.defaults.language = qEnvironmentVariable("HYPRCAPTURE_TEST_LANGUAGE", QStringLiteral("en")).toStdString();
    session.cursorPosition = hyprcapture::Point{150, 150};
    hyprcapture::MonitorInfo monitor;
    monitor.name = "test-2x";
    monitor.logicalGeometry = {0, 0, kLogicalWidth, kLogicalHeight};
    monitor.scale = 2;
    monitor.focused = true;
    monitor.artifactPath = desktopPath.toStdString();
    monitor.artifactWidth = desktop.width();
    monitor.artifactHeight = desktop.height();
    session.monitors.push_back(monitor);

    if (includeWindow) {
        QImage window(kWindowPixels, QImage::Format_RGBA8888);
        window.fill(Qt::transparent);
        {
            QPainter painter(&window);
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
        info.fullGeometry = {100, 100, 200, 120};
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

} // namespace

class InPlaceEditorTest final : public QObject {
    Q_OBJECT

  private slots:
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
        QVERIFY(QMetaObject::invokeMethod(editor, "reselectRequested", Qt::DirectConnection));
        QVERIFY(!editor->isVisible());
        selectRegion(overlay, QPoint(400, 150), QPoint(499, 249));
        QTRY_VERIFY(editor->isVisible());
        QCOMPARE(editor->resultImage().size(), QSize(200, 200));
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
        auto* pin = editor->findChild<QPushButton*>(QStringLiteral("annotationPin"));
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
        QTest::mouseClick(pin, Qt::LeftButton);
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
        auto* save = editor->findChild<QPushButton*>(QStringLiteral("annotationSave"));
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
        const QString expectedError = hyprcapture::ui::uiText("Could not save the image. Choose another output directory or copy it.");
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
        auto* save = editor->findChild<QPushButton*>(QStringLiteral("annotationSave"));
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
        socket.write("error:Injected pin failure\n");
        socket.flush();
        return socket.bytesToWrite() == 0 || socket.waitForBytesWritten(1000) ? 0 : 1;
    }
    // Recording controls may enumerate sound sources. Keep that child process
    // isolated from host devices and prevent it from recursively running tests.
    if (argc > 1 && QString::fromLocal8Bit(argv[1]).startsWith(QStringLiteral("--sound-"))) {
        if (QString::fromLocal8Bit(argv[1]) == QStringLiteral("--sound-list"))
            fputs("{\"outputs\":[],\"inputs\":[],\"windows\":[]}\n", stdout);
        return 0;
    }
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
