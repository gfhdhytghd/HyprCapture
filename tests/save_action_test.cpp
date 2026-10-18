#include "ui/capture_output.hpp"
#include "ui/result_thumbnail.hpp"
#include "ui/hdr_image.hpp"
#include "ui/hdr_thumbnail.hpp"
#include <QApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QFileInfo>
#include <QPushButton>
#include <cassert>
#include <QColorSpace>
#include <QContextMenuEvent>
#include <QtCore/qfloat16.h>
using namespace hyprcapture;
using namespace hyprcapture::ui;
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir dir;
    assert(dir.isValid());
    QImage image(1280, 800, QImage::Format_RGBA8888);
    image.fill(QColor(120, 40, 90));
    image.setColorSpace(QColorSpace::SRgb);
    image.setPixelColor(1279, 31, Qt::green);
    auto capture = std::make_shared<CaptureSave>();
    capture->defaults.save = false;
    capture->defaults.clipboard = false;
    capture->defaults.screenshotNotification = false;
    capture->defaults.saveDir = dir.path().toStdString();
    capture->defaults.filenameTemplate = "{window_title}.png";
    capture->metadata.windowTitle = "Current result";
    capture->image = image;
    ResultThumbnail thumbnail(QPixmap::fromImage(image), {}, {}, {}, 0);
    ResultThumbnail second(QPixmap::fromImage(image), {}, {}, {}, 0);
    thumbnail.setSaveCapture(capture);
    second.setSaveCapture(capture);
    QObject::connect(&thumbnail, &ResultThumbnail::captureSaved, &second, &ResultThumbnail::refreshSaveActions);
    auto* save = thumbnail.findChild<QPushButton*>("thumbnailSave");
    assert(save && save->isEnabled());
    for (auto* button : thumbnail.findChildren<QPushButton*>())
        if (button->text() == "Show in folder" || button->text() == "Open") assert(!button->isEnabled());
    const auto review = qEnvironmentVariable("HYPRCAPTURE_SAVE_UI_REVIEW");
    if (!review.isEmpty()) {
        thumbnail.show();
        app.processEvents();
        assert(thumbnail.grab().save(review + "-unsaved.png"));
        QContextMenuEvent menu(QContextMenuEvent::Mouse, QPoint(20, 20));
        QApplication::sendEvent(&thumbnail, &menu);
        app.processEvents();
        assert(thumbnail.grab().save(review + "-menu.png"));
    }
    save->click();
    if (!review.isEmpty()) {
        app.processEvents();
        assert(thumbnail.grab().save(review + "-saved.png"));
    }
    assert(!capture->savedPath.isEmpty() && !save->isEnabled());
    assert(!second.findChild<QPushButton*>("thumbnailSave")->isEnabled());
    const auto saved = capture->savedPath;
    assert(QImage(saved).convertToFormat(QImage::Format_RGBA8888) == image);
    assert(QFileInfo(saved).fileName() == "Current-result.png");
    assert(!(QFileInfo(saved).permissions() & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ReadOther | QFileDevice::WriteOther)));
    for (auto* button : thumbnail.findChildren<QPushButton*>())
        if (button->text() == "Show in folder" || button->text() == "Open") assert(button->isEnabled());
    assert(capture->save() && capture->savedPath == saved);
    save->click();
    assert(QDir(dir.path()).entryList(QDir::Files).size() == 1);
    auto alreadySaved = std::make_shared<CaptureSave>();
    alreadySaved->defaults = capture->defaults;
    alreadySaved->defaults.save = true;
    alreadySaved->savedPath = saved;
    second.setSaveCapture(alreadySaved);
    assert(!second.findChild<QPushButton*>("thumbnailSave")->isEnabled());
    assert(alreadySaved->save() && QDir(dir.path()).entryList(QDir::Files).size() == 1);
    // A distinct capture still uses collision-safe naming.
    auto next = *capture;
    next.savedPath.clear();
    assert(next.save() && next.savedPath != saved);
    assert(QDir(dir.path()).entryList(QDir::Files).size() == 2);
    // Failed writes leave the same action retryable.
    auto failed = next;
    failed.savedPath.clear();
    failed.defaults.saveDir = (saved + "/not-a-directory").toStdString();
    assert(!failed.save() && failed.savedPath.isEmpty() && !failed.error.isEmpty());
    failed.defaults.saveDir = dir.path().toStdString();
    assert(failed.save());

    if (hdrHeicAvailable()) {
        CaptureSave hdr;
        hdr.defaults = capture->defaults;
        hdr.defaults.screenshotFormat = "heic";
        hdr.defaults.hdrSaveSdr = true;
        hdr.defaults.filenameTemplate = "HDR.png";
        hdr.image = image;
        hdr.hdrImage = sdrToHdr(image);
        reinterpret_cast<qfloat16*>(hdr.hdrImage.scanLine(0))[0] = qfloat16(4.0f);
        // Lossless handoff must retain pixels beyond the preview's size bound.
        const auto transport = saveHdrThumbnail(hdr.hdrImage, true);
        assert(!transport.isEmpty());
        const auto restored = takeHdrThumbnail(transport, true);
        assert(restored.size() == image.size() && !QFileInfo::exists(transport));
        hdr.hdrImage = restored;
        assert(hdr.save());
        assert(QFileInfo(hdr.savedPath).size() > 0);
        assert(QImage(QFileInfo(hdr.savedPath).absolutePath() + "/HDR.png").convertToFormat(QImage::Format_RGBA8888) == image);
        const auto files = QDir(dir.path()).entryList(QDir::Files);
        assert(hdr.save() && QDir(dir.path()).entryList(QDir::Files) == files);
        // Automatic and manual export use the same encoder and SDR companion.
        auto defaults = hdr.defaults;
        defaults.save = true;
        defaults.clipboard = false;
        defaults.showThumbnail = false;
        const auto automatic = writeCaptureOutput(image, restored, defaults, {}, dir.filePath("automatic.heic"), {});
        assert(automatic.savedPath == dir.filePath("automatic.heic") && automatic.saveError.isEmpty());
        QFile manualFile(hdr.savedPath), autoFile(automatic.savedPath);
        assert(manualFile.open(QIODevice::ReadOnly) && autoFile.open(QIODevice::ReadOnly));
        assert(manualFile.readAll() == autoFile.readAll());
        CaptureSave hdrOnly = hdr;
        hdrOnly.savedPath.clear();
        hdrOnly.defaults.hdrSaveSdr = false;
        hdrOnly.defaults.hdrDragFormat = "sdr";
        hdrOnly.defaults.filenameTemplate = "HDR-only.png";
        assert(hdrOnly.save());
        assert(!QFileInfo::exists(dir.filePath("HDR-only.png")));
        auto dragDefaults = hdrOnly.defaults;
        dragDefaults.save = true;
        const auto dragPath = sdrCaptureOutputPath(dragDefaults, hdrOnly.savedPath);
        assert(QImage(dragPath).size() == image.size());
        QFile::remove(dragPath);
    }
}
