#include "ui/capture_output.hpp"
#include "ui/result_thumbnail.hpp"
#include "ui/hdr_image.hpp"
#include "ui/hdr_thumbnail.hpp"
#include <QApplication>
#include <QClipboard>
#include <QTemporaryDir>
#include <QDir>
#include <QFileInfo>
#include <QPushButton>
#include <QLabel>
#include <QGraphicsOpacityEffect>
#include <cassert>
#include <QColorSpace>
#include <QContextMenuEvent>
#include <QtCore/qfloat16.h>
using namespace hyprcapture;
using namespace hyprcapture::ui;
int main(int argc, char** argv) {
    // Keep clipboard checks on Qt's offscreen clipboard, away from the session.
    qputenv("WAYLAND_DISPLAY", "/hyprcapture-save-action-no-display");
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

    // The original two KeepAspectRatio resamples yield 179x112 for 1280x800.
    // Preserve this rounding and the original 24px padding.
    auto *defaultImage = thumbnail.findChild<QLabel *>("thumbnailImage");
    auto *defaultCard = thumbnail.findChild<QWidget *>("thumbnailImageCard");
    assert(defaultImage && defaultCard && defaultImage->geometry() == QRect(0, 0, 179, 112));
    assert(defaultCard->size() == QSize(203, 136) && thumbnail.size() == defaultCard->size());
    assert(!thumbnail.graphicsEffect());
    // Original default geometry with save=false: the action row sits below
    // the image after the 24px card padding and 6px spacing. The row height
    // depends on the environment's font metrics, so derive it.
    thumbnail.setSaveCapture(capture);
    thumbnail.show();
    app.processEvents();
    const QRect defaultRect(defaultImage->mapTo(&thumbnail, QPoint{}), defaultImage->size());
    assert(defaultRect.top() == 0 && defaultRect.size() == QSize(179, 112));
    auto *defaultQuick = thumbnail.findChild<QPushButton *>("thumbnailQuickSave")->parentWidget();
    assert(defaultQuick->height() > 0);
    assert(defaultQuick->mapTo(&thumbnail, QPoint{}).y() == defaultRect.bottom() + 1 + 24 + 6);
    assert(thumbnail.height() - defaultRect.bottom() - 1 == 24 + 6 + defaultQuick->height());

    // The border grows outside the image; neither its pixels nor radius shrink.
    {
        CaptureDefaults borderedStyle;
        borderedStyle.thumbnailBorderSize = 6;
        borderedStyle.thumbnailRounding = 12;
        ResultThumbnail bordered(QPixmap::fromImage(image), {}, {}, {}, 0, false, nullptr, nullptr, borderedStyle);
        auto *label = bordered.findChild<QLabel *>("thumbnailImage");
        auto *card = bordered.findChild<QWidget *>("thumbnailImageCard");
        assert(label && card);
        assert(label->size() == defaultImage->size() + QSize(12, 12));
        assert(label->contentsMargins() == QMargins(6, 6, 6, 6));
        assert(label->contentsRect() == QRect(6, 6, 179, 112));
        assert(label->pixmap().deviceIndependentSize() == QSizeF(179, 112));
        assert(card->size() == defaultCard->size() + QSize(12, 12));
        const auto preview = label->pixmap().toImage();
        assert(preview.pixelColor(0, 0).alpha() == 0);
        assert(preview.pixelColor(89, 56).alpha() == 255);

        const auto checkCopy = [&](const QImage &expected) {
            // Exercise both the menu action and its quick-action connection.
            int copies = 0;
            for (auto *button : bordered.findChildren<QPushButton *>()) {
                if (button->text() != "Copy image")
                    continue;
                QApplication::clipboard()->clear();
                button->click();
                const auto copied = QApplication::clipboard()->image();
                assert(!copied.isNull());
                assert(copied.size() == expected.size());
                for (const QPoint corner : {QPoint(0, 0), QPoint(expected.width() - 1, 0),
                                            QPoint(0, expected.height() - 1), QPoint(expected.width() - 1, expected.height() - 1)})
                    assert(copied.pixelColor(corner).alpha() == 255);
                assert(copied.convertToFormat(QImage::Format_RGBA8888) == expected.convertToFormat(QImage::Format_RGBA8888));
                ++copies;
            }
            assert(copies == 2);
        };
        checkCopy(image);
        // Pending recording results replace the placeholder through this path.
        QImage replacement(960, 600, QImage::Format_RGBA8888);
        replacement.fill(Qt::cyan);
        replacement.setPixelColor(959, 599, Qt::yellow);
        bordered.setImagePixmap(QPixmap::fromImage(replacement));
        assert(label->pixmap().toImage().pixelColor(0, 0).alpha() == 0);
        checkCopy(replacement);
        // Native HDR can clear the QLabel; copying must retain the raster source.
        label->clear();
        checkCopy(replacement);
    }

    // Large radii use circular caps on both wide and tall images. The border
    // must follow the same cap as the SDR image and the HDR mask.
    for (const QSize size : {QSize(160, 40), QSize(40, 160)}) {
        QPixmap source(size);
        source.fill(Qt::red);
        CaptureDefaults style;
        style.thumbnailMaxWidth = 320;
        style.thumbnailMaxHeight = 320;
        style.thumbnailBorderSize = 6;
        style.thumbnailBorderColor = "#00ff00";
        style.thumbnailRounding = 20;
        ResultThumbnail capped(source, {}, {}, {}, 0, false, nullptr, nullptr, style);
        style.thumbnailRounding = 128;
        ResultThumbnail oversized(source, {}, {}, {}, 0, false, nullptr, nullptr, style);
        const auto renderLabel = [](ResultThumbnail &result) {
            auto *label = result.findChild<QLabel *>("thumbnailImage");
            QPixmap rendered(label->size());
            rendered.fill(Qt::transparent);
            label->render(&rendered);
            const auto image = rendered.toImage();
            const auto edge = image.pixelColor(0, image.height() / 2);
            assert(edge.green() == 255 && edge.red() == 0 && edge.blue() == 0 && edge.alpha() > 200);
            return image;
        };
        assert(renderLabel(capped) == renderLabel(oversized));
    }

    for (const char *position : {"top_left", "top_right", "bottom_left", "bottom_right"}) {
        CaptureDefaults style;
        style.thumbnailMaxWidth = 320;
        style.thumbnailMaxHeight = 200;
        style.thumbnailPosition = position;
        style.thumbnailMargin = 11;
        style.thumbnailOpacity = 50;
        style.thumbnailAnimation = false;
        ResultThumbnail custom(QPixmap::fromImage(image), {}, {}, {}, 0, false, nullptr, nullptr, style);
        auto *preview = custom.findChild<QLabel *>("thumbnailImage");
        auto *card = custom.findChild<QWidget *>("thumbnailImageCard");
        const bool left = std::string_view(position).ends_with("left");
        const bool top = std::string_view(position).starts_with("top");
        assert(preview && card && preview->geometry() == QRect(left ? 11 : 0, top ? 11 : 0, 320, 200));
        assert(card->size() == QSize(331, 211));
        auto *opacity = qobject_cast<QGraphicsOpacityEffect *>(custom.graphicsEffect());
        assert(opacity && opacity->opacity() == 0.5);
        const QImage rendered = custom.grab().toImage();
        const auto center = preview->mapTo(&custom, preview->rect().center());
        assert(rendered.pixelColor(center).alpha() >= 126 && rendered.pixelColor(center).alpha() <= 128);

        auto pending = std::make_shared<CaptureSave>();
        pending->defaults = capture->defaults;
        pending->image = image;
        custom.setSaveCapture(pending);
        custom.show();
        app.processEvents();
        const auto anchoredImage = [&] {
            const QRect rect(preview->mapTo(&custom, QPoint{}), preview->size());
            assert((left ? rect.left() : custom.width() - rect.right() - 1) == style.thumbnailMargin);
            auto *quick = custom.findChild<QPushButton *>("thumbnailQuickSave")->parentWidget();
            const int actionsBelow = quick->isVisible() ? quick->height() + 6 : 0;
            assert((top ? rect.top() : custom.height() - rect.bottom() - 1) == style.thumbnailMargin + (top ? 0 : actionsBelow));
            if (quick->isVisible())
                assert(quick->mapTo(&custom, QPoint{}).y() > rect.bottom());
            // Simulate the layer-shell anchor on a fixed screen. The offscreen
            // platform cannot position a layer surface, but lays out all widgets.
            return rect.translated(left ? 0 : 1920 - custom.width(), top ? 0 : 1080 - custom.height());
        };
        const auto unsavedImage = anchoredImage();
        pending->savedPath = dir.filePath("layout-saved.png");
        custom.refreshSaveActions();
        app.processEvents();
        const auto savedImage = anchoredImage();
        assert(top ? savedImage == unsavedImage : savedImage.top() > unsavedImage.top());
        // Also cover the quick row appearing again, not only disappearing.
        pending->savedPath.clear();
        custom.refreshSaveActions();
        app.processEvents();
        assert(anchoredImage() == unsavedImage);
        pending->savedPath = dir.filePath("layout-saved.png");
        custom.refreshSaveActions();
        QContextMenuEvent context(QContextMenuEvent::Mouse, QPoint(20, 20), QPoint(20, 20));
        QApplication::sendEvent(&custom, &context);
        app.processEvents();
        auto *shell = custom.findChild<QWidget *>("thumbnailMenuShell");
        assert(shell);
        auto *mainMenu = shell->findChild<QWidget *>("thumbnailMenu");
        auto *submenu = shell->findChild<QWidget *>("thumbnailOpenWithMenu");
        assert(shell && mainMenu && submenu && shell->isVisible());
        const auto menuEdge = [&] {
            const int x = mainMenu->mapTo(&custom, QPoint{}).x();
            return left ? x : custom.width() - x - mainMenu->width();
        };
        const auto mainEdge = menuEdge();
        const auto menuImage = anchoredImage();
        QPushButton *openWith = nullptr;
        for (auto *button : mainMenu->findChildren<QPushButton *>())
            if (button->text() == "Open with")
                openWith = button;
        assert(openWith && openWith->isEnabled());
        openWith->click();
        app.processEvents();
        assert(submenu->isVisible() && menuEdge() == mainEdge);
        assert(left ? mainMenu->geometry().right() + 6 < submenu->x() : submenu->geometry().right() + 6 < mainMenu->x());
        assert(anchoredImage() == menuImage);
        openWith->click();
        app.processEvents();
        assert(!submenu->isVisible() && menuEdge() == mainEdge);
        assert(anchoredImage() == menuImage);
    }

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
