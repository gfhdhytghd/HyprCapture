#include "shared/protocol.hpp"
#include "ui/annotation_editor.hpp"
#include "ui/capture_overlay.hpp"
#include "ui/clipboard_utils.hpp"
#include "ui/i18n.hpp"
#include "ui/hdr_image.hpp"
#include "ui/hdr_thumbnail.hpp"
#include <QtCore/qfloat16.h>
#include <QColorSpace>

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
#include <QSemaphore>
#include <QThreadPool>
#include <QWindow>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QWheelEvent>
#include <QScreen>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>
#include "shared/hdr_color.hpp"
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
    void hdrStartupSurvivesCloseDuringConversion() {
        const QString timingPath = hyprcapture::ui::runtimeFile("hdr-startup-timing", ".log");
        QFile timing(timingPath);
        QVERIFY(timing.open(QIODevice::WriteOnly | QIODevice::NewOnly));
        QVERIFY(timing.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        timing.close();
        const auto previousTiming = qgetenv("HYPRCAPTURE_TIMING_FILE");
        qputenv("HYPRCAPTURE_TIMING_FILE", timingPath.toUtf8());
        const auto resetTiming = qScopeGuard([&] {
            if (previousTiming.isNull()) qunsetenv("HYPRCAPTURE_TIMING_FILE");
            else qputenv("HYPRCAPTURE_TIMING_FILE", previousTiming);
            QFile::remove(timingPath);
        });

        // Hold the worker so first paint and destruction must finish before
        // conversion can even start. The conversion then uses unlinked mapped
        // pixels after the overlay is gone, and its queued result is discarded.
        auto* pool = QThreadPool::globalInstance();
        pool->waitForDone();
        const int previousThreads = pool->maxThreadCount();
        pool->setMaxThreadCount(1);
        QSemaphore entered, release;
        pool->start(QRunnable::create([&] { entered.release(); release.acquire(); }));
        const auto resetPool = qScopeGuard([&] {
            release.release();
            pool->waitForDone();
            pool->setMaxThreadCount(previousThreads);
        });
        QVERIFY(entered.tryAcquire(1, 5000));
        hyprcapture::CaptureDefaults defaults;
        auto session = hyprcapture::decodeSessionJson(sessionJson(defaults, false).toStdString());
        QVERIFY(session);
        auto& monitor = session->monitors.front();
        const QString path = hyprcapture::ui::runtimeFile("hdr-startup", ".rgba");
        QImage linear(64, 64, QImage::Format_RGBA16FPx4);
        linear.fill(Qt::white);
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::NewOnly));
        QVERIFY(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        QCOMPARE(file.write(reinterpret_cast<const char*>(linear.constBits()), linear.sizeInBytes()), linear.sizeInBytes());
        file.close();
        monitor.artifactPath = path.toStdString();
        monitor.artifactWidth = linear.width(); monitor.artifactHeight = linear.height();
        monitor.artifactFormat = "rgba16f-linear-srgb";
        {
            CaptureOverlay overlay(defaults, false, false, false, QString::fromStdString(hyprcapture::encodeSessionJson(*session)));
            QVERIFY(!QFile::exists(path));
            overlay.show();
            QTRY_VERIFY(overlay.windowHandle() && overlay.windowHandle()->isExposed());
            overlay.repaint();
            QVERIFY(timing.open(QIODevice::ReadOnly));
            const auto before = timing.readAll();
            timing.close();
            QVERIFY(before.contains("overlay.first_paint"));
            QVERIFY(!before.contains("overlay.hdr_backdrop_convert"));
        }
        release.release();
        QVERIFY(pool->waitForDone(5000));
        QCoreApplication::processEvents();
        QVERIFY(timing.open(QIODevice::ReadOnly));
        QVERIFY(timing.readAll().contains("overlay.hdr_backdrop_convert"));
    }

    void dynamicRangeControl() {
        for (bool hasHdrScreen : {false, true}) {
            for (bool visible : {false, true}) {
                for (bool defaultHdr : {false, true}) {
                    hyprcapture::CaptureDefaults defaults;
                    defaults.showHdrControl = visible;
                    defaults.screenshotFormat = defaultHdr ? "heic" : "png";
                    auto session = hyprcapture::decodeSessionJson(sessionJson(defaults, false).toStdString());
                    QVERIFY(session);
                    session->monitors.front().hdrCapable = true; // Capability alone must not enable HDR.
                    // The focused/captured screen is SDR. An unfocused HDR
                    // screen with no artifact must still enable the control.
                    if (hasHdrScreen) {
                        hyprcapture::MonitorInfo other;
                        other.name = "unfocused-HDR";
                        other.logicalGeometry = {800,0,800,600};
                        other.hdrCapable = true;
                        other.hdrActive = true;
                        session->monitors.push_back(other);
                    }
                    CaptureOverlay overlay(defaults,false,false,false,QString::fromStdString(hyprcapture::encodeSessionJson(*session)));
                    overlay.show();
                    QCoreApplication::processEvents();
                    auto* control = overlay.findChild<QWidget*>("dynamicRangeControl");
                    auto* sdr = overlay.findChild<QPushButton*>("screenshotSdr");
                    auto* hdr = overlay.findChild<QPushButton*>("screenshotHdr");
                    QVERIFY(control && sdr && hdr);
                    QCOMPARE(sdr->text(),QString("SDR")); QCOMPARE(hdr->text(),QString("HDR"));
                    QCOMPARE(control->isVisible(),visible && hasHdrScreen);
                    const bool available = hasHdrScreen && hyprcapture::ui::hdrHeicAvailable();
                    QCOMPARE(control->isEnabled(),available);
                    QCOMPARE(hdr->isChecked(),available && defaultHdr);
                    QCOMPARE(sdr->isChecked(),!available || !defaultHdr);
                    if (available && visible) {
                        QTest::mouseClick(hdr,Qt::LeftButton);
                        QVERIFY(hdr->isChecked() && !sdr->isChecked());
                        QTest::mouseClick(sdr,Qt::LeftButton);
                        QVERIFY(sdr->isChecked() && !hdr->isChecked());
                    }
                    overlay.hide();
                }
            }
        }
    }

    void hdrArtifactPreviewAndEditing_data() {
        QTest::addColumn<bool>("both");
        QTest::addColumn<bool>("save");
        QTest::addColumn<QString>("clipboardFormat");
        QTest::addColumn<QString>("dragFormat");
        QTest::newRow("heic-only") << false << true << QString("heic") << QString("heic");
        QTest::newRow("both-sdr-clipboard") << true << true << QString("sdr") << QString("sdr");
        QTest::newRow("heic-clipboard-no-save") << false << false << QString("heic") << QString("heic");
        QTest::newRow("private-sdr-drag") << false << true << QString("heic") << QString("sdr");
    }
    void hdrArtifactPreviewAndEditing() {
        QFETCH(bool, both); QFETCH(bool, save); QFETCH(QString, clipboardFormat); QFETCH(QString, dragFormat);
        if (!hyprcapture::ui::hdrHeicAvailable()) QSKIP("HEIC encoder unavailable");
        const QString copied = qEnvironmentVariable("HYPRCAPTURE_TEST_CLIPBOARD");
        QFile::remove(copied);
        QTemporaryDir output;
        QVERIFY(output.isValid());
        hyprcapture::CaptureDefaults defaults;
        defaults.mode = hyprcapture::CaptureMode::Fullscreen;
        defaults.fullscreenScope = hyprcapture::FullscreenScope::Current;
        defaults.inPlaceEditToolbar = true;
        defaults.clipboard = true; defaults.save = save;
        defaults.hdrSaveSdr = both; defaults.hdrClipboardFormat = clipboardFormat.toStdString();
        defaults.showThumbnail = true;
        defaults.hdrDragFormat = dragFormat.toStdString();
        const QString thumbnail = output.filePath("thumbnail-preview");
        qputenv("HYPRCAPTURE_TEST_THUMBNAIL", thumbnail.toUtf8());
        const auto resetThumbnail = qScopeGuard([] { qunsetenv("HYPRCAPTURE_TEST_THUMBNAIL"); });
        defaults.screenshotFormat = hyprcapture::ui::hdrHeicAvailable() ? "heic" : "png";
        defaults.saveDir = output.path().toStdString();
        QImage linear(QSize(kLogicalWidth, kLogicalHeight) * 2, QImage::Format_RGBA16FPx4);
        for (int y = 0; y < linear.height(); ++y) {
            auto* row = reinterpret_cast<qfloat16*>(linear.scanLine(y));
            for (int x = 0; x < linear.width(); ++x) {
                const float v = x < linear.width() / 2 ? 0.18F * 203 / 80 : 1000.0F / 80;
                for (int c = 0; c < 3; ++c) row[x * 4 + c] = qfloat16(v);
                row[x * 4 + 3] = qfloat16(1.0F);
            }
        }
        const QString path = hyprcapture::ui::runtimeFile("hdr-ui-test", ".rgba");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::NewOnly));
        QVERIFY(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        QCOMPARE(file.write(reinterpret_cast<const char*>(linear.constBits()), linear.sizeInBytes()), linear.sizeInBytes());
        file.close();
        hyprcapture::CaptureSession session;
        session.id = "hdr-ui-test"; session.defaults = defaults;
        hyprcapture::MonitorInfo monitor;
        monitor.name = "HDR"; monitor.focused = true; monitor.hdrCapable = true; monitor.hdrActive = true;
        monitor.logicalGeometry = {0, 0, kLogicalWidth, kLogicalHeight}; monitor.scale = 2;
        monitor.artifactFormat = "rgba16f-linear-srgb"; monitor.artifactReferenceNits = 203;
        monitor.artifactPath = path.toStdString(); monitor.artifactWidth = linear.width(); monitor.artifactHeight = linear.height();
        session.monitors.push_back(monitor);
        CaptureOverlay overlay(defaults, false, false, false, QString::fromStdString(hyprcapture::encodeSessionJson(session)));
        QVERIFY(!QFile::exists(path)); // mapped float pixels must survive unlink
        overlay.show();
        QTest::qWait(30);
        auto* full = modeButton(overlay, "fullscreen");
        QVERIFY(full);
        QTest::mouseClick(full, Qt::LeftButton);
        auto* editor = overlay.findChild<AnnotationEditor*>("inPlaceEditor");
        QTRY_VERIFY(editor && editor->isVisible());
        QCOMPARE(editor->hdrEnabled(), defaults.screenshotFormat == "heic");
        if (defaults.screenshotFormat == "heic") {
            auto* sdr = overlay.findChild<QPushButton*>("screenshotSdr");
            auto* hdr = overlay.findChild<QPushButton*>("screenshotHdr");
            QVERIFY(sdr && hdr);
            sdr->click();
            QVERIFY(!editor->hdrEnabled());
            hdr->click();
            QVERIFY(editor->hdrEnabled());
        }
        const auto result = editor->resultImage();
        QCOMPARE(result.size(), linear.size());
        const auto expected = hyprcapture::ui::hdrToSdr(linear);
        QCOMPARE(result.pixelColor(100, 100), expected.pixelColor(100, 100));
        QCOMPARE(result.pixelColor(1500, 100), expected.pixelColor(1500, 100));
        const auto untouched = editor->resultImageOn(linear).convertToFormat(QImage::Format_RGBA16FPx4);
        QCOMPARE(float(reinterpret_cast<const qfloat16*>(untouched.constScanLine(100))[1500 * 4]), 12.5F);
        QVERIFY(QMetaObject::invokeMethod(editor, "confirmRequested", Qt::DirectConnection));
        if (save) {
            QTRY_VERIFY_WITH_TIMEOUT(!QDir(output.path()).entryList({"*.heic"}, QDir::Files).isEmpty(), 15000);
            const auto files = QDir(output.path()).entryList({"*.heic"}, QDir::Files);
            QCOMPARE(files.size(), 1);
            QFile heic(QDir(output.path()).filePath(files.front()));
            QVERIFY(heic.open(QIODevice::ReadOnly));
            QTRY_VERIFY_WITH_TIMEOUT(heic.size() > 100, 15000);
            const auto signature = heic.read(12);
            QCOMPARE(signature.mid(4, 4), QByteArray("ftyp"));
        }
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(copied).size() > 100, 15000);
        QFile clip(copied); QVERIFY(clip.open(QIODevice::ReadOnly));
        const QByteArray header = clip.read(12);
        if (clipboardFormat == "heic") QCOMPARE(header.mid(4,4), QByteArray("ftyp"));
        else QVERIFY(header.startsWith(QByteArray::fromHex("89504e47")));
        QFile hdrAck(thumbnail + ".hdr");
        QTRY_VERIFY_WITH_TIMEOUT(hdrAck.size() > 0, 5000);
        QVERIFY(hdrAck.open(QIODevice::ReadOnly));
        QVERIFY(hdrAck.readAll().toFloat() >= 12.4F); // no clipping to an SDR preview
        QFile dragArgs(thumbnail + ".drag");
        // The mock creates the file before writing the SDR drag path.
        QTRY_VERIFY_WITH_TIMEOUT(dragArgs.exists() && (dragFormat != "sdr" || dragArgs.size() > 0), 5000);
        QVERIFY(dragArgs.open(QIODevice::ReadOnly));
        const QString dragPath = QString::fromUtf8(dragArgs.readAll());
        if (dragFormat == "sdr") {
            QVERIFY(dragPath.endsWith(".png"));
            QVERIFY(!QImage(dragPath).isNull());
            if (!both) { QVERIFY(hyprcapture::ui::isPrivateRuntimePath(dragPath)); QFile::remove(dragPath); }
        } else QVERIFY(dragPath.isEmpty());
        QCOMPARE(QDir(output.path()).entryList({"*.png"}, QDir::Files).size(), both && save ? 1 : 0);
        QCOMPARE(QDir(output.path()).entryList({"*.heic"}, QDir::Files).size(), save ? 1 : 0);

    }

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
        expected.setColorSpace(QColorSpace::SRgb);
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

    void windowEditorMaskExcludesRoundedCorners_data() {
        QTest::addColumn<bool>("stage");
        QTest::newRow("desktop") << false;
        QTest::newRow("stage") << true;
    }
    void windowEditorMaskExcludesRoundedCorners() {
        QFETCH(bool, stage);
        hyprcapture::CaptureDefaults defaults;
        defaults.mode = hyprcapture::CaptureMode::Window;
        defaults.fushionMode = false;
        defaults.inPlaceEditToolbar = true;
        const QRect window(100,100,200,120);
        auto session = hyprcapture::decodeSessionJson(sessionJson(defaults, true, QSize(800,600), window,
                                                                 stage ? QRect(100,100,100,60) : QRect{}).toStdString());
        QVERIFY(session);
        session->windows.front().rounding = 40;
        session->windows.front().selectionRounding = 20;
        CaptureOverlay overlay(defaults, false, false, false, QString::fromStdString(hyprcapture::encodeSessionJson(*session)));
        overlay.show();
        QTest::qWait(30);
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, stage ? QPoint(150,130) : window.center());
        auto* editor = overlay.findChild<AnnotationEditor*>("inPlaceEditor");
        QTRY_VERIFY(editor && editor->isVisible());
        const QRect canvas(400,300,300,180);
        editor->setImageDisplayRect(canvas);
        QImage parent(overlay.size(), QImage::Format_ARGB32_Premultiplied);
        parent.fill(Qt::transparent);
        QPainter painter(&parent);
        overlay.render(&painter, QPoint(), QRegion(), QWidget::DrawWindowBackground);
        painter.end();
        QCOMPARE(parent.pixelColor(canvas.center()), QColor(17,29,53));
        // Native radius 40 becomes 60 on this canvas, regardless of Stage's
        // thumbnail radius. These pixels lie outside/inside that curved edge.
        QVERIFY(parent.pixelColor(canvas.topLeft() + QPoint(10,10)).blue() < 53);
        QCOMPARE(parent.pixelColor(canvas.topLeft() + QPoint(45,20)), QColor(17,29,53));
        QVERIFY(parent.pixelColor(canvas.bottomRight() - QPoint(10,10)).blue() < 53);
    }

    void editorMaskFollowsCanvasBeforeHdrSurfaceIsReady() {
        hyprcapture::CaptureDefaults defaults;
        defaults.mode = hyprcapture::CaptureMode::Region;
        defaults.inPlaceEditToolbar = true;
        CaptureOverlay overlay(defaults, false, false, false, sessionJson(defaults));
        overlay.show();
        QTest::qWait(30);
        selectRegion(overlay, QPoint(200,180), QPoint(349,279));
        auto* editor = overlay.findChild<AnnotationEditor*>("inPlaceEditor");
        QVERIFY(editor && editor->isVisible());
        editor->setImageDisplayRect(QRect(600,400,150,100));
        QImage parent(overlay.size(), QImage::Format_ARGB32_Premultiplied);
        parent.fill(Qt::transparent);
        QPainter painter(&parent);
        // Exclude the opaque SDR editor child: this is the mask over native HDR.
        overlay.render(&painter, QPoint(), QRegion(), QWidget::DrawWindowBackground);
        painter.end();
        QCOMPARE(parent.pixelColor(650,450), QColor(17,29,53));
        QVERIFY(parent.pixelColor(550,450).blue() < 53);
        QVERIFY(parent.pixelColor(250,220).blue() < 53);
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
        defaults.fushionMode = false; // Exercise the independent window outline.
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

    void windowHighlightUsesSelectionGeometry_data() {
        QTest::addColumn<QString>("preview");
        QTest::addColumn<bool>("fusion");
        for (const auto& preview : {QString("desktop"), QString("overview"), QString("stage-scaled"), QString("stage-rendered")}) {
            QTest::newRow(qPrintable(preview + "-window")) << preview << false;
            QTest::newRow(qPrintable(preview + "-fusion")) << preview << true;
        }
    }
    void windowHighlightUsesSelectionGeometry() {
        QFETCH(QString, preview);
        QFETCH(bool, fusion);
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
        defaults.fushionMode = fusion;
        defaults.confirmBeforeCapture = true;
        const QRect nativeWindow(300, 220, 300, 220);
        const QRect thumbnail(20, 80, 90, 66);
        const bool overview = preview != "desktop";
        auto session = hyprcapture::decodeSessionJson(sessionJson(defaults, true, QSize(800, 600), nativeWindow,
                                                                 overview ? thumbnail : QRect{}).toStdString());
        QVERIFY(session);
        auto& window = session->windows.front();
        window.zIndex = 1;
        window.rounding = overview ? 40 : 0;
        window.stagePreview = preview.startsWith("stage");
        if (preview == "stage-rendered") {
            window.rounding = 100;
            window.selectionRounding = 12;
        }
        if (overview)
            window.selectionClipGeometry->width /= 2;
        if (overview) {
            auto other = window;
            other.address = "0xother";
            other.focused = false;
            other.zIndex = 0;
            other.fullGeometry.x += 400;
            other.visibleGeometry.x += 400;
            other.selectionGeometry->x += 120;
            other.selectionClipGeometry->x += 120;
            session->windows.push_back(other);
        }
        const QRect target = overview ? thumbnail : nativeWindow;
        session->cursorPosition = hyprcapture::Point{double(target.x() + target.width() / 4), double(target.center().y())};
        CaptureOverlay overlay(defaults, false, false, false, QString::fromStdString(hyprcapture::encodeSessionJson(*session)));
        QSignalSpy finishing(&overlay, &CaptureOverlay::finishingStarted);
        overlay.show();
        QTest::qWait(30);
        QImage image = overlay.grab().toImage();
        const auto colorAt = [&](const QPoint& point) {
            return image.pixelColor(qRound(point.x() * image.devicePixelRatio()), qRound(point.y() * image.devicePixelRatio()));
        };
        const QColor desktop(17, 29, 53);
        QCOMPARE(colorAt(QPoint(target.x() + target.width() / 4, target.center().y())), desktop);
        if (overview) {
            QVERIFY(colorAt(nativeWindow.center()).blue() < desktop.blue());
            QVERIFY(colorAt(QPoint(thumbnail.right() - 5, thumbnail.center().y())).blue() < desktop.blue());
            QVERIFY(colorAt(thumbnail.topLeft() + QPoint(2, 2)).blue() < desktop.blue());
            if (preview.startsWith("stage"))
                QCOMPARE(colorAt(thumbnail.topLeft() + QPoint(14, 4)), desktop);
            else
                QVERIFY(colorAt(thumbnail.topLeft() + QPoint(14, 4)).blue() < desktop.blue());
            QVERIFY(colorAt(QPoint(thumbnail.right(), thumbnail.center().y())).red() < 70);
        }
        QTest::keyClick(&overlay, Qt::Key_Return);
        QCOMPARE(finishing.count(), 0);
        QVERIFY(!overlay.findChild<AnnotationEditor*>("inPlaceEditor"));
        image = overlay.grab().toImage();
        QCOMPARE(colorAt(QPoint(target.x() + target.width() / 4, target.center().y())), desktop);
        if (overview) {
            const QPoint cursor(target.x() + target.width() / 4, target.center().y());
            QWheelEvent wheel(cursor, overlay.mapToGlobal(cursor), {}, QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(&overlay, &wheel);
            image = overlay.grab().toImage();
            QVERIFY(colorAt(cursor).blue() < desktop.blue());
            QCOMPARE(colorAt(cursor + QPoint(120, 0)), desktop);
            QVERIFY(colorAt(nativeWindow.center()).blue() < desktop.blue());
            QVERIFY(colorAt(QPoint(thumbnail.right() + 120, thumbnail.center().y())).red() < 70);
            QCOMPARE(finishing.count(), 0);
        }
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
        defaults.fushionMode = false; // Fusion intentionally falls back to a monitor click.
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

// Explicit live-only fixture: uses its own Qt events and private artifacts,
// never saves a screenshot or touches the user's clipboard/settings.
int hdrEditorLive(int argc,char** argv) {
    qputenv("QT_WAYLAND_SHELL_INTEGRATION","layer-shell");
    QApplication app(argc,argv);
    auto* screen=app.primaryScreen();
    if(!screen || !app.platformName().startsWith("wayland")) return 77;
    const QRect geometry=screen->geometry();
    QImage linear(512,256,QImage::Format_RGBA16FPx4);
    linear.setColorSpace(QColorSpace::SRgbLinear);
    const float nits[]={0,69,97,173,205,359,500,630};
    for(int y=0;y<linear.height();++y) {
        auto* row=reinterpret_cast<uint16_t*>(linear.scanLine(y));
        for(int x=0;x<linear.width();++x) {
            for(int c=0;c<3;++c) row[x*4+c]=hyprcapture::color::floatToHalf(nits[x/64]/80);
            row[x*4+3]=hyprcapture::color::floatToHalf(1);
        }
    }
    const QString path=hyprcapture::ui::runtimeFile("hdr-editor-live",".rgba");
    QFile file(path);
    if(!file.open(QIODevice::WriteOnly|QIODevice::NewOnly)) return 1;
    file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner);
    file.write(reinterpret_cast<const char*>(linear.constBits()),linear.sizeInBytes());file.close();
    hyprcapture::CaptureDefaults defaults;
    defaults.mode=hyprcapture::CaptureMode::Fullscreen;
    defaults.inPlaceEditToolbar=true;
    defaults.save=false;defaults.clipboard=false;defaults.showThumbnail=false;
    defaults.screenshotNotification=false;defaults.rememberSettings=false;defaults.fushionMode=false;
    hyprcapture::CaptureSession session;
    session.id="hdr-editor-live";session.defaults=defaults;
    hyprcapture::MonitorInfo monitor;
    monitor.name=screen->name().toStdString();monitor.focused=true;monitor.hdrCapable=true;monitor.hdrActive=true;
    monitor.logicalGeometry={double(geometry.x()),double(geometry.y()),double(geometry.width()),double(geometry.height())};
    monitor.artifactPath=path.toStdString();monitor.artifactWidth=512;monitor.artifactHeight=256;
    monitor.artifactFormat="rgba16f-linear-srgb";monitor.artifactReferenceNits=173;
    session.monitors.push_back(monitor);
    CaptureOverlay overlay(defaults,false,false,false,QString::fromStdString(hyprcapture::encodeSessionJson(session)));
    overlay.show();QTest::qWait(1000);
    QTest::keyClick(&overlay,Qt::Key_Return);QTest::qWait(600);
    auto* editor=overlay.findChild<AnnotationEditor*>("inPlaceEditor");
    if(!editor || !editor->isVisible() || !editor->externalImagePresentation()) return 2;
    if (QString::fromLocal8Bit(argv[1]) == "--hdr-editor-regression") {
        const QString output = qEnvironmentVariable("HYPRCAPTURE_HDR_EDITOR_METADATA");
        if (output.isEmpty()) return 3;
        const auto capture = [&](const QString& suffix) {
            QProcess process;
            process.start("grim", {"-o", screen->name(), output + suffix + ".png"});
            return process.waitForFinished(5000) && process.exitCode() == 0;
        };
        editor->setImageDisplayRect(QRect(160, 130, 512, 256));
        QTest::qWait(300);
        if (!capture("-large")) return 4;
        editor->setImageDisplayRect(QRect(160, 130, 256, 128));
        QTest::qWait(300);
        if (!capture("-small")) return 4;
        const QImage smallPreview(output + "-small.png");
        const qreal captureScale = qreal(smallPreview.width()) / geometry.width();
        const auto sample = [&](const QImage& image, QPoint point) {
            return image.pixelColor(QPoint(qRound(point.x() * captureScale), qRound(point.y() * captureScale)));
        };
        // The fixture's brightest band ends at x=416. Outside the image,
        // only the dimmed desktop should remain, including below its bottom.
        if (smallPreview.isNull() || sample(smallPreview, {400, 194}).red() < 230 ||
            sample(smallPreview, {436, 194}).red() > 220 ||
            sample(smallPreview, {380, 262}).red() > 220 ||
            sample(smallPreview, {geometry.width() / 8 + 4, 350}).red() < 80) return 7;
        editor->setImageDisplayRect(QRect(240, 180, 384, 192));
        QTest::qWait(300);
        auto* canvas = editor->findChild<QWidget*>("annotationCanvas");
        auto* pen = editor->findChild<QToolButton*>("annotationTool5");
        if (!canvas || !pen) return 5;
        const QImage before = editor->resultImage();
        QTest::mouseClick(pen, Qt::LeftButton);
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, QPoint(280, 240));
        QTest::mouseMove(canvas, QPoint(560, 280));
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, QPoint(560, 280));
        QTest::qWait(300);
        if (editor->resultImage() == before || !capture("-ink")) return 6;
        const QImage inkPreview(output + "-ink.png");
        const QColor ink = sample(inkPreview, {420, 260});
        if (inkPreview.isNull() || ink.red() < 100 || ink.red() < ink.green() * 1.6) return 8;
        puts("HDR editor resize, move and annotation fixture completed");
        return 0;
    }
    const auto rect=editor->canvasGeometry().translated(geometry.topLeft());
    QFile metadata(qEnvironmentVariable("HYPRCAPTURE_HDR_EDITOR_METADATA"));
    if(!metadata.open(QIODevice::WriteOnly)) return 3;
    metadata.write(QJsonDocument(QJsonObject{{"x",rect.x()},{"y",rect.y()},{"width",rect.width()},{"height",rect.height()},
                                           {"native_hdr",editor->externalImagePresentation()}}).toJson());metadata.close();
    QProcess capture;
    capture.start("hyprctl",{"-i","0","eval","hl.plugin.hyprcapture.open(\"fullscreen\")"});
    if(!capture.waitForFinished(5000) || capture.exitCode()!=0) return 4;
    QTest::qWait(400);
    overlay.hide();
    return 0;
}

int main(int argc, char** argv) {
    if(argc>1 && (QString::fromLocal8Bit(argv[1])=="--hdr-editor-live" || QString::fromLocal8Bit(argv[1])=="--hdr-editor-regression")) return hdrEditorLive(argc,argv);
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
        const auto arguments=child.arguments();
        const int hdrIndex=arguments.indexOf("--thumbnail-hdr-preview");
        if(hdrIndex>=0 && hdrIndex+1<arguments.size()) {
            const auto native=hyprcapture::ui::takeHdrThumbnail(arguments.at(hdrIndex+1));
            if(native.isNull()) return 2;
            float peak=0;
            for(int y=0;y<native.height();++y) {
                const auto* pixels=reinterpret_cast<const qfloat16*>(native.constScanLine(y));
                for(int x=0;x<native.width();++x) for(int c=0;c<3;++c) peak=std::max(peak,float(pixels[x*4+c]));
            }
            QFile ack(target+".hdr");
            if(!ack.open(QIODevice::WriteOnly)) return 3;
            ack.write(QByteArray::number(peak)); ack.close();
        }
        QFile argsFile(target + ".drag");
        if (argsFile.open(QIODevice::WriteOnly)) {
            const auto args = child.arguments();
            const int index = args.indexOf("--thumbnail-drag-target");
            if (index >= 0 && index + 1 < args.size()) argsFile.write(args.at(index + 1).toUtf8());
        }
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
