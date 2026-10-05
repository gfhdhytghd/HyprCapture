#include "ui/annotation_editor.hpp"
#include "ui/material_icon.hpp"

#include <QApplication>
#include <QAbstractButton>
#include <QDialog>
#include <QDialogButtonBox>
#include <QImage>
#include <QListWidget>
#include <QLineEdit>
#include <QTextBlock>
#include <QTextLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTest>
#include <QWheelEvent>

namespace {
QImage image(QSize size = QSize(240, 160), QColor color = Qt::transparent) {
    QImage result(size, QImage::Format_ARGB32_Premultiplied);
    result.fill(color);
    return result;
}
void initialize(AnnotationEditor& editor, const QImage& source) {
    editor.resize(900, 650);
    editor.setImage(source);
    editor.setImageDisplayRect(QRect(QPoint(40, 30), source.size() * 2));
    editor.show();
    QApplication::processEvents();
}
QPoint displayed(const QPoint& native) { return QPoint(40, 30) + native * 2; }
void selectTool(AnnotationEditor& editor, int id) {
    auto* button = editor.findChild<QToolButton*>(QString("annotationTool%1").arg(id));
    QVERIFY(button);
    if (!button->isVisible()) {
        const int group = id == 4 ? 3 : id == 6 ? 5 : id == 10 ? 2 : -1;
        QVERIFY(group >= 0);
        auto* trigger = editor.findChild<QToolButton*>(QString("annotationTool%1").arg(group));
        QVERIFY(trigger);
        QTest::mouseClick(trigger, Qt::LeftButton);
        if (!button->isVisible())
            QTest::mouseClick(trigger, Qt::LeftButton);
        QVERIFY(button->isVisible());
    }
    QTest::mouseClick(button, Qt::LeftButton);
}
void openPanel(AnnotationEditor& editor, const char* triggerName, const char* panelName) {
    auto* trigger = editor.findChild<QAbstractButton*>(QString::fromLatin1(triggerName));
    auto* panel = editor.findChild<QWidget*>(QString::fromLatin1(panelName));
    QVERIFY(trigger);
    QVERIFY(panel);
    if (!panel->isVisible())
        QTest::mouseClick(trigger, Qt::LeftButton);
    if (!panel->isVisible())
        QTest::mouseClick(trigger, Qt::LeftButton);
    QVERIFY(panel->isVisible());
}
void chooseVariant(AnnotationEditor& editor, int group, const char* optionName) {
    const QByteArray trigger = QString("annotationTool%1").arg(group).toLatin1();
    openPanel(editor, trigger.constData(), "annotationVariantPanel");
    auto* option = editor.findChild<QAbstractButton*>(QString::fromLatin1(optionName));
    QVERIFY(option);
    QVERIFY(option->isVisible());
    QTest::mouseClick(option, Qt::LeftButton);
}
void verifyToolbarInside(AnnotationEditor& editor) {
    auto* toolbar = editor.toolbarWidget();
    QVERIFY(toolbar);
    QVERIFY(toolbar->isVisible());
    QVERIFY2(editor.rect().contains(toolbar->geometry()), qPrintable(QString("Toolbar %1,%2 %3x%4 outside %5x%6")
        .arg(toolbar->x()).arg(toolbar->y()).arg(toolbar->width()).arg(toolbar->height()).arg(editor.width()).arg(editor.height())));
    for (auto* button : toolbar->findChildren<QAbstractButton*>()) {
        if (button->isVisible())
            QVERIFY2(editor.rect().contains(QRect(button->mapTo(&editor, QPoint()), button->size())), qPrintable(button->objectName()));
    }
}
void verifyPanelInside(AnnotationEditor& editor, const char* panelName) {
    auto* panel = editor.findChild<QWidget*>(QString::fromLatin1(panelName));
    QVERIFY(panel);
    QVERIFY(panel->isVisible());
    QVERIFY(editor.rect().contains(QRect(panel->mapTo(&editor, QPoint()), panel->size())));
    for (auto* button : panel->findChildren<QAbstractButton*>()) {
        if (button->isVisible())
            QVERIFY2(editor.rect().contains(QRect(button->mapTo(&editor, QPoint()), button->size())), qPrintable(button->objectName()));
    }
}
void drag(AnnotationEditor& editor, const QPoint& from, const QPoint& to) {
    auto* canvas = editor.findChild<QWidget*>("annotationCanvas");
    QVERIFY(canvas);
    QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, displayed(from));
    QTest::mouseMove(canvas, displayed(to));
    QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, displayed(to));
    QApplication::processEvents();
}
void defaults() {
    QSettings settings(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/hyprcapture/editor.ini", QSettings::IniFormat);
    settings.clear();
    settings.sync();
}
}

class AnnotationEditorTest final : public QObject {
    Q_OBJECT
  private slots:
    void materialIconsRenderAtRequestedDeviceResolution() {
        const auto icon = hyprcapture::ui::materialIcon("edit", QColor("#26313d"));
        QVERIFY(!icon.isNull());
        for (qreal scale : {1.0, 1.25, 1.5, 2.0, 3.0}) {
            const QPixmap pixmap = icon.pixmap(QSize(24, 24), scale);
            QCOMPARE(pixmap.size(), QSize(qRound(24 * scale), qRound(24 * scale)));
            QCOMPARE(pixmap.devicePixelRatio(), scale);
            const QImage rendered = pixmap.toImage();
            int covered = 0;
            for (int y = 0; y < rendered.height(); ++y)
                for (int x = 0; x < rendered.width(); ++x)
                    covered += rendered.pixelColor(x, y).alpha() > 0;
            QVERIFY(covered > 25 * scale * scale);
        }
        const auto highResolution = icon.pixmap(QSize(24, 24), 2.0).toImage();
        const auto upscaledBitmap = icon.pixmap(QSize(24, 24), 1.0).toImage().scaled(48, 48, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        QVERIFY(highResolution != upscaledBitmap);
    }
    void init() { defaults(); }

    void transparentExportAndNativeCoordinates() {
        AnnotationEditor editor;
        QImage source = image();
        source.setDevicePixelRatio(2);
        initialize(editor, source);
        QCOMPARE(editor.canvasGeometry(), QRect(40, 30, 480, 320));
        QCOMPARE(editor.resultImage().size(), source.size());
        QCOMPARE(editor.resultImage().devicePixelRatio(), 1.0);
        QCOMPARE(editor.resultImage().pixelColor(100, 100).alpha(), 0);
        selectTool(editor, 5);
        drag(editor, QPoint(20, 30), QPoint(100, 70));
        const QImage result = editor.resultImage();
        QCOMPARE(result.pixelColor(60, 50), QColor("#ff5252"));
        QCOMPARE(result.pixelColor(150, 120).alpha(), 0);
        // Display checkerboard must not appear in exported transparent pixels.
        QVERIFY(editor.grab().toImage().pixelColor(350, 260).alpha() > 0);
    }

    void backgroundUpdatePreservesEditsAndHistory() {
        AnnotationEditor editor;
        const QImage source = image();
        initialize(editor, source);
        selectTool(editor, 5);
        drag(editor, QPoint(20, 30), QPoint(100, 70));
        const QImage edited = editor.resultImage();
        editor.undo();
        QCOMPARE(editor.resultImage(), source);
        editor.redo();
        QCOMPARE(editor.resultImage(), edited);
        const QImage newBackground = image(source.size(), Qt::white);
        editor.setImage(newBackground, true);
        QCOMPARE(editor.resultImage().pixelColor(60, 50), QColor("#ff5252"));
        QCOMPARE(editor.resultImage().pixelColor(150, 120), QColor(Qt::white));
        editor.undo();
        QCOMPARE(editor.resultImage(), newBackground);
        editor.redo();
        QCOMPARE(editor.resultImage().pixelColor(60, 50), QColor("#ff5252"));
        const QImage resized = image(QSize(100, 120), Qt::black);
        editor.setImage(resized, true);
        QCOMPARE(editor.resultImage(), resized);
        editor.undo();
        QCOMPARE(editor.resultImage(), resized);
    }

    void mosaicIsOpaqueAndUndoable() {
        AnnotationEditor editor;
        QImage source = image();
        for (int y = 30; y <= 90; ++y)
            for (int x = 20; x <= 100; ++x)
                source.setPixelColor(x, y, QColor(x % 2 ? 255 : 0, y % 2 ? 255 : 0, 30, (x + y) % 2 ? 60 : 220));
        initialize(editor, source);
        selectTool(editor, 9);
        drag(editor, QPoint(20, 30), QPoint(100, 90));
        const QImage result = editor.resultImage();
        for (int y = 30; y < 90; ++y)
            for (int x = 20; x < 100; ++x)
                QCOMPARE(result.pixelColor(x, y).alpha(), 255);
        QCOMPARE(result.pixelColor(22, 32), result.pixelColor(28, 37));
        QCOMPARE(result.pixelColor(140, 100).alpha(), 0);
        editor.undo();
        QCOMPARE(editor.resultImage(), source);
        editor.redo();
        QCOMPARE(editor.resultImage(), result);
    }

    void shapeVariantsAndClearUndo() {
        AnnotationEditor editor;
        const QImage source = image();
        initialize(editor, source);
        selectTool(editor, 1);
        int rectangleVariants = 0;
        for (auto* button : editor.findChildren<QAbstractButton*>())
            rectangleVariants += button->isVisible() && button->objectName().startsWith("annotationVariantRect");
        QCOMPARE(rectangleVariants, 4);
        chooseVariant(editor, 1, "annotationVariantRectFilled");
        drag(editor, QPoint(20, 30), QPoint(80, 80));
        QCOMPARE(editor.resultImage().pixelColor(50, 55), QColor("#ff5252"));
        selectTool(editor, 3);
        int arrowVariants = 0;
        for (auto* button : editor.findChildren<QAbstractButton*>())
            arrowVariants += button->isVisible() && button->objectName().startsWith("annotationVariantArrow");
        QCOMPARE(arrowVariants, 4);
        chooseVariant(editor, 3, "annotationVariantArrowDoubleCurved");
        drag(editor, QPoint(110, 30), QPoint(210, 80));
        const QImage twoAnnotations = editor.resultImage();
        editor.clearAnnotations();
        QCOMPARE(editor.resultImage(), source);
        editor.undo();
        QCOMPARE(editor.resultImage(), twoAnnotations);
        editor.redo();
        QCOMPARE(editor.resultImage(), source);
    }

    void selectMoveDeleteAndUndo() {
        AnnotationEditor editor;
        initialize(editor, image());
        selectTool(editor, 5);
        drag(editor, QPoint(20, 30), QPoint(100, 70));
        const QImage before = editor.resultImage();
        selectTool(editor, 0);
        drag(editor, QPoint(60, 50), QPoint(90, 80));
        QCOMPARE(editor.resultImage().pixelColor(90, 80), QColor("#ff5252"));
        QCOMPARE(editor.resultImage().pixelColor(60, 50).alpha(), 0);
        editor.undo();
        QCOMPARE(editor.resultImage(), before);
        editor.redo();
        auto* canvas = editor.findChild<QWidget*>("annotationCanvas");
        QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, displayed(QPoint(90, 80)));
        QTest::keyClick(canvas, Qt::Key_Delete);
        QCOMPARE(editor.resultImage(), image());
        editor.undo();
        QCOMPARE(editor.resultImage().pixelColor(90, 80), QColor("#ff5252"));
    }

    void allDrawingToolsExport() {
        AnnotationEditor editor;
        const QImage source = image();
        initialize(editor, source);
        // Each drawing tool must produce pixels at native resolution.
        for (const int tool : {1, 2, 3, 4, 5, 6, 10}) {
            editor.setImage(source);
            selectTool(editor, tool);
            drag(editor, QPoint(20, 30), QPoint(100, 70));
            QVERIFY2(editor.resultImage() != source, qPrintable(QString("Tool %1 exported no annotation").arg(tool)));
            editor.undo();
            QCOMPARE(editor.resultImage(), source);
        }
        selectTool(editor, 8);
        QTest::mouseClick(editor.findChild<QWidget*>("annotationCanvas"), Qt::LeftButton, Qt::NoModifier, displayed(QPoint(50, 50)));
        QVERIFY(editor.resultImage() != source);
        editor.undo();
        QCOMPARE(editor.resultImage(), source);
    }

    void textIsMultilineAndNeverOpensAModalWindow() {
        AnnotationEditor editor;
        const QImage source = image();
        initialize(editor, source);
        selectTool(editor, 7);
        auto* canvas = editor.findChild<QWidget*>("annotationCanvas");
        QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, displayed(QPoint(20, 30)));
        QVERIFY(!QApplication::activeModalWidget());
        auto* input = editor.findChild<QPlainTextEdit*>("annotationTextInput");
        QVERIFY(input && input->isVisible());
        QVERIFY(!input->window()->isModal());
        QTest::keyClicks(input, "TRED");
        QTest::keyClick(input, Qt::Key_Return);
        QTest::keyClicks(input, "second line");
        QCOMPARE(input->toPlainText(), QString("TRED\nsecond line"));
        const auto screenshot = qEnvironmentVariable("HYPRCAPTURE_TEST_TEXT_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            editor.update();
            for (auto* widget : editor.findChildren<QWidget*>()) widget->update();
            QTest::qWait(30);
            QVERIFY(editor.grab().save(screenshot));
        }
        QTest::keyClick(input, Qt::Key_Return, Qt::ControlModifier);
        QVERIFY(!input->isVisible());
        QVERIFY(canvas->isEnabled());
        QVERIFY(editor.resultImage() != source);
        QVERIFY(editor.findChild<QToolButton*>("annotationTool7")->isChecked());
        editor.undo();
        QCOMPARE(editor.resultImage(), source);
    }

    void paletteWidthControlsAndRendering() {
        AnnotationEditor editor;
        initialize(editor, image());
        openPanel(editor, "annotationColorTrigger", "annotationColorPanel");
        auto* panel = editor.findChild<QWidget*>("annotationColorPanel");
        auto* spin = editor.findChild<QSpinBox*>("annotationWidth");
        auto* minus = editor.findChild<QToolButton*>("annotationWidthDecrease");
        auto* plus = editor.findChild<QToolButton*>("annotationWidthIncrease");
        QVERIFY(spin && minus && plus);
        QCOMPARE(spin->buttonSymbols(), QAbstractSpinBox::NoButtons);
        spin->setValue(4);
        QTest::mouseClick(plus, Qt::LeftButton); QCOMPARE(spin->value(), 5);
        QTest::mouseClick(minus, Qt::LeftButton); QCOMPARE(spin->value(), 4);
        for (auto* child : {static_cast<QWidget*>(spin), static_cast<QWidget*>(minus), static_cast<QWidget*>(plus)})
            QVERIFY(panel->rect().contains(QRect(child->mapTo(panel, QPoint()), child->size())));
        const auto screenshot = qEnvironmentVariable("HYPRCAPTURE_TEST_PALETTE_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            for (bool dark : {false, true}) {
                QTest::mouseClick(editor.findChild<QToolButton*>(dark ? "annotationThemeDark" : "annotationThemeLight"), Qt::LeftButton);
                panel->update();
                for (auto* child : panel->findChildren<QWidget*>()) child->update();
                QTest::qWait(20);
                QVERIFY(panel->grab().save(screenshot + (dark ? "-dark.png" : "-light.png")));
            }
        }
    }

    void textBoxMovesResizesAndRetainsFormatting() {
        AnnotationEditor editor;
        const QImage source = image();
        initialize(editor, source);
        selectTool(editor, 7);
        auto* canvas = editor.findChild<QWidget*>("annotationCanvas");
        QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, displayed(QPoint(20, 30)));
        auto* panel = editor.findChild<QWidget*>("annotationTextPanel");
        auto* input = editor.findChild<QPlainTextEdit*>("annotationTextInput");
        auto* handle = editor.findChild<QWidget*>("annotationTextDragHandle");
        auto* size = editor.findChild<QSpinBox*>("annotationTextSize");
        auto* color = editor.findChild<QLineEdit*>("annotationTextColor");
        auto* font = editor.findChild<QPushButton*>("annotationTextFont");
        auto* fonts = editor.findChild<QListWidget*>("annotationTextFontList");
        QVERIFY(panel && input && handle && size && color && font && fonts);
        auto* controls = editor.findChild<QWidget*>("annotationTextControls");
        QVERIFY(controls && controls->isVisible());
        QCOMPARE(controls->parentWidget(), &editor);
        QVERIFY(!controls->geometry().intersects(panel->geometry()));
        QVERIFY(editor.rect().contains(controls->geometry()));
        const QPoint controlsBefore = controls->pos();
        const QRect before = panel->geometry();
        QTest::mousePress(handle, Qt::LeftButton, Qt::NoModifier, QPoint(25, 10));
        QTest::mouseMove(handle, QPoint(55, 30));
        QTest::mouseRelease(handle, Qt::LeftButton, Qt::NoModifier, QPoint(55, 30));
        QCOMPARE(panel->pos(), before.topLeft() + QPoint(30, 20));
        QCOMPARE(controls->pos(), controlsBefore + QPoint(30, 20));
        QVERIFY(!controls->geometry().intersects(panel->geometry()));
        const QSize previous = panel->size();
        const QPoint corner(panel->width()-2, panel->height()-2);
        QTest::mousePress(panel, Qt::LeftButton, Qt::NoModifier, corner);
        QTest::mouseMove(panel, corner + QPoint(60, 35));
        QTest::mouseRelease(panel, Qt::LeftButton, Qt::NoModifier, corner + QPoint(60, 35));
        QCOMPARE(panel->size(), previous + QSize(60, 35));
        QVERIFY(!controls->geometry().intersects(panel->geometry()));
        // The floating bar follows the frame when it switches from below to above.
        panel->move(100, 280);
        QVERIFY(controls->geometry().bottom() < panel->geometry().top());
        QVERIFY(editor.rect().contains(controls->geometry()));
        auto* paletteButton = editor.findChild<QToolButton*>("annotationTextPaletteButton");
        auto* textPalette = editor.findChild<QWidget*>("annotationTextPalette");
        auto* saturation = editor.findChild<QWidget*>("annotationTextSaturationValue");
        auto* hue = editor.findChild<QWidget*>("annotationTextHue");
        QVERIFY(paletteButton && textPalette && saturation && hue);
        QTest::mouseClick(paletteButton, Qt::LeftButton);
        QVERIFY(textPalette->isVisible());
        QVERIFY(editor.rect().contains(textPalette->geometry()));
        const auto paletteScreenshot = qEnvironmentVariable("HYPRCAPTURE_TEST_TEXT_PALETTE_SCREENSHOT");
        if (!paletteScreenshot.isEmpty()) {
            editor.update();
            for (auto* child : editor.findChildren<QWidget*>()) child->update();
            QTest::qWait(20);
            QVERIFY(editor.grab().save(paletteScreenshot));
        }
        QTest::mouseClick(hue, Qt::LeftButton, Qt::NoModifier, QPoint(10, 90));
        QTest::mouseClick(saturation, Qt::LeftButton, Qt::NoModifier, QPoint(120, 60));
        QVERIFY(QColor(color->text()).isValid());
        QCOMPARE(input->palette().color(QPalette::Text), QColor(color->text()));
        size->setValue(30);
        color->setText("#0055ff");
        QTest::mouseClick(font, Qt::LeftButton);
        QVERIFY(fonts->isVisible());
        QVERIFY(!textPalette->isVisible());
        QVERIFY(!QApplication::activeModalWidget());
        const QString family = fonts->item(0)->text();
        QTest::mouseClick(fonts->viewport(), Qt::LeftButton, Qt::NoModifier, fonts->visualItemRect(fonts->item(0)).center());
        QCOMPARE(input->font().family(), family);
        QCOMPARE(input->font().pixelSize(), 30);
        QCOMPARE(input->palette().color(QPalette::Text), QColor("#0055ff"));
        QTest::keyClicks(input, "Visible text wraps inside the resized frame and keeps its chosen font.");
        QApplication::processEvents();
        QVERIFY(input->document()->firstBlock().layout()->lineCount() > 1);
        const QRect box(input->viewport()->mapTo(&editor, QPoint()), input->viewport()->size());
        const QString screenshot = qEnvironmentVariable("HYPRCAPTURE_TEST_STYLED_TEXT_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            editor.update();
            for (auto* widget : editor.findChildren<QWidget*>()) widget->update();
            QTest::qWait(30);
            QVERIFY(editor.grab().save(screenshot));
        }
        QTest::keyClick(input, Qt::Key_Return, Qt::ControlModifier);
        const QImage edited = editor.resultImage();
        QVERIFY(edited != source);
        bool blueText = false;
        for (int y = 0; y < edited.height(); ++y)
            for (int x = 0; x < edited.width(); ++x) {
                const QColor pixel = edited.pixelColor(x,y);
                if (pixel.alpha() > 128 && pixel.blue() > 240 && pixel.red() < 10) blueText = true;
            }
        QVERIFY(blueText);
        editor.undo(); QCOMPARE(editor.resultImage(), source);
        editor.redo(); QCOMPARE(editor.resultImage(), edited);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        selectTool(editor, 0);
        QTest::mouseDClick(canvas, Qt::LeftButton, Qt::NoModifier, box.topLeft() + QPoint(8, 8));
        auto* reopened = editor.findChild<QPlainTextEdit*>("annotationTextInput");
        QVERIFY(reopened && reopened->isVisible());
        QCOMPARE(editor.findChild<QSpinBox*>("annotationTextSize")->value(), 30);
        QCOMPARE(editor.findChild<QLineEdit*>("annotationTextColor")->text(), QString("#0055ff"));
        QCOMPARE(reopened->font().family(), family);
        QCOMPARE(QRect(reopened->viewport()->mapTo(&editor, QPoint()), reopened->viewport()->size()), box);
        QTest::keyClick(reopened, Qt::Key_Escape);
        QCOMPARE(editor.resultImage(), edited);
    }

    void textEscapeAndCancelRemainResponsive() {
        AnnotationEditor editor;
        const QImage source = image();
        initialize(editor, source);
        selectTool(editor, 7);
        auto* canvas = editor.findChild<QWidget*>("annotationCanvas");
        QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, displayed(QPoint(20, 30)));
        auto* input = editor.findChild<QPlainTextEdit*>("annotationTextInput");
        QVERIFY(input);
        QTest::keyClicks(input, "discard this");
        QTest::keyClick(input, Qt::Key_Escape);
        QVERIFY(!input->isVisible());
        QVERIFY(canvas->isEnabled());
        QCOMPARE(editor.resultImage(), source);
        QSignalSpy cancelled(&editor, &AnnotationEditor::cancelRequested);
        QTest::keyClick(canvas, Qt::Key_Escape);
        QCOMPARE(cancelled.count(), 1);
    }

    void fitBringsOffscreenImageIntoViewport() {
        AnnotationEditor editor;
        editor.resize(600, 400);
        editor.setImage(image(QSize(1000, 600), Qt::white));
        editor.setImageDisplayRect(QRect(-500, -300, 1000, 600));
        editor.show();
        QCOMPARE(editor.canvasGeometry(), QRect(-500, -300, 1000, 600));
        editor.fitImage();
        QVERIFY(editor.rect().contains(editor.canvasGeometry()));
        verifyToolbarInside(editor);
        QVERIFY(!editor.canvasGeometry().intersects(editor.toolbarWidget()->geometry()));
        const QImage native = editor.resultImage();
        editor.setImage(image(QSize(1000, 600), Qt::black), true);
        QVERIFY(editor.rect().contains(editor.canvasGeometry()));
        QCOMPARE(editor.resultImage().size(), native.size());
    }

    void toolbarFollowsSelection_data() {
        QTest::addColumn<QRect>("selection");
        QTest::addColumn<bool>("below");
        QTest::newRow("below") << QRect(200, 80, 320, 200) << true;
        QTest::newRow("above-near-bottom") << QRect(200, 500, 320, 180) << false;
        QTest::newRow("left-edge-clamp") << QRect(-150, 30, 320, 200) << true;
        QTest::newRow("right-edge-clamp") << QRect(1030, 30, 320, 200) << true;
    }

    void toolbarFollowsSelection() {
        QFETCH(QRect, selection);
        QFETCH(bool, below);
        AnnotationEditor editor;
        editor.resize(1100, 720);
        editor.setImage(image(selection.size()));
        editor.setImageDisplayRect(selection);
        editor.show();
        QApplication::processEvents();
        QCOMPARE(editor.canvasGeometry(), selection);
        verifyToolbarInside(editor);
        const QRect toolbar = editor.toolbarWidget()->geometry();
        QVERIFY(toolbar.height() <= 64); // The desktop toolbar is one compact strip.
        if (below) {
            QVERIFY(toolbar.top() > selection.bottom());
            QVERIFY(toolbar.top() - selection.bottom() <= 24);
        } else {
            QVERIFY(toolbar.bottom() < selection.top());
            QVERIFY(selection.top() - toolbar.bottom() <= 24);
        }
        QVERIFY(!toolbar.intersects(selection.intersected(editor.rect())));
    }

    void zoomAndPanRepositionToolbarWithoutChangingExport() {
        AnnotationEditor editor;
        editor.resize(1100, 720);
        editor.setImage(image(QSize(240, 160), Qt::white));
        editor.setImageDisplayRect(QRect(280, 200, 480, 320));
        editor.show();
        QApplication::processEvents();
        selectTool(editor, 5);
        auto* canvas = editor.findChild<QWidget*>("annotationCanvas");
        QVERIFY(canvas);
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, QPoint(320, 260));
        QTest::mouseMove(canvas, QPoint(480, 340));
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, QPoint(480, 340));
        const QImage native = editor.resultImage();
        QCOMPARE(native.pixelColor(60, 50), QColor("#ff5252"));
        const QRect initialImage = editor.canvasGeometry();
        const QRect initialToolbar = editor.toolbarWidget()->geometry();
        const QPoint zoomPoint = initialImage.center();
        QWheelEvent zoom(zoomPoint, canvas->mapToGlobal(zoomPoint), {}, QPoint(0, 120),
                         Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(canvas, &zoom);
        QTRY_VERIFY(editor.canvasGeometry().width() > initialImage.width());
        QTRY_VERIFY(editor.toolbarWidget()->geometry() != initialToolbar);
        verifyToolbarInside(editor);
        QCOMPARE(editor.resultImage(), native);

        const QRect zoomedImage = editor.canvasGeometry();
        const QRect zoomedToolbar = editor.toolbarWidget()->geometry();
        const QPoint start = zoomedImage.center();
        const QPoint delta(60, -60);
        QTest::mousePress(canvas, Qt::MiddleButton, Qt::NoModifier, start);
        QTest::mouseMove(canvas, start + delta);
        QTest::mouseRelease(canvas, Qt::MiddleButton, Qt::NoModifier, start + delta);
        QTRY_COMPARE(editor.canvasGeometry().topLeft(), zoomedImage.topLeft() + delta);
        QTRY_VERIFY(editor.toolbarWidget()->geometry() != zoomedToolbar);
        verifyToolbarInside(editor);
        const QRect toolbar = editor.toolbarWidget()->geometry();
        QVERIFY(toolbar.top() > editor.canvasGeometry().bottom());
        QVERIFY(toolbar.top() - editor.canvasGeometry().bottom() <= 24);
        QCOMPARE(editor.resultImage(), native);
    }

    void narrowToolbarWrapsAndKeepsActionsAccessible() {
        AnnotationEditor editor;
        editor.resize(1100, 720);
        editor.setImage(image());
        editor.setImageDisplayRect(QRect(300, 80, 320, 200));
        editor.show();
        QApplication::processEvents();
        const int wideHeight = editor.toolbarWidget()->height();
        for (const int width : {360, 280}) {
            editor.resize(width, 720);
            editor.setImageDisplayRect(QRect(40, 80, 200, 140));
            QApplication::processEvents();
            verifyToolbarInside(editor);
            QVERIFY(editor.toolbarWidget()->height() > wideHeight);
            for (const int tool : {0, 1, 2, 3, 5, 7, 8, 9}) {
                auto* button = editor.findChild<QToolButton*>(QString("annotationTool%1").arg(tool));
                QVERIFY(button);
                QVERIFY(button->isVisible());
            }
            for (const char* name : {"annotationUndo", "annotationRedo", "annotationConfirm", "annotationCancel", "annotationColorTrigger", "annotationMore"}) {
                auto* button = editor.findChild<QAbstractButton*>(QString::fromLatin1(name));
                QVERIFY(button);
                QVERIFY(button->isVisible());
            }
            openPanel(editor, "annotationMore", "annotationMorePanel");
            verifyPanelInside(editor, "annotationMorePanel");
            for (const char* name : {"annotationClear", "annotationFit", "annotationReselect", "annotationPin"}) {
                auto* button = editor.findChild<QAbstractButton*>(QString::fromLatin1(name));
                QVERIFY(button);
                QVERIFY(button->isVisible());
            }
            QTest::mouseClick(editor.findChild<QAbstractButton*>("annotationMore"), Qt::LeftButton);
        }
    }

    void popoversStayInsideNarrowViewport() {
        AnnotationEditor editor;
        editor.resize(280, 600);
        editor.setImage(image());
        editor.setImageDisplayRect(QRect(40, 300, 200, 140));
        editor.show();
        QApplication::processEvents();
        verifyToolbarInside(editor);
        for (const int group : {1, 2, 3, 5}) {
            const QByteArray trigger = QString("annotationTool%1").arg(group).toLatin1();
            openPanel(editor, trigger.constData(), "annotationVariantPanel");
            verifyPanelInside(editor, "annotationVariantPanel");
            QTest::mouseClick(editor.findChild<QAbstractButton*>(QString::fromLatin1(trigger)), Qt::LeftButton);
        }
        openPanel(editor, "annotationColorTrigger", "annotationColorPanel");
        verifyPanelInside(editor, "annotationColorPanel");
        auto* hue = editor.findChild<QWidget*>("annotationHue");
        auto* saturation = editor.findChild<QWidget*>("annotationSaturationValue");
        QVERIFY(hue && hue->isVisible());
        QVERIFY(saturation && saturation->isVisible());
        QVERIFY(editor.rect().contains(QRect(hue->mapTo(&editor, QPoint()), hue->size())));
        QVERIFY(editor.rect().contains(QRect(saturation->mapTo(&editor, QPoint()), saturation->size())));
        auto* color = editor.findChild<QToolButton*>("annotationColor4d94ff");
        QVERIFY(color && color->isVisible());
        QTest::mouseClick(color, Qt::LeftButton);
        QVERIFY(!editor.findChild<QWidget*>("annotationColorPanel")->isVisible());
    }

    void shortcutsAndPreferences() {
        {
            AnnotationEditor editor;
            initialize(editor, image());
            auto* canvas = editor.findChild<QWidget*>("annotationCanvas");
            canvas->setFocus();
            QTest::keyClick(canvas, Qt::Key_D);
            QVERIFY(editor.findChild<QToolButton*>("annotationTool5")->isChecked());
            QVERIFY(!editor.findChild<QWidget*>("annotationVariantPanel")->isVisible());
            QTest::keyClick(canvas, Qt::Key_D, Qt::ShiftModifier);
            QVERIFY(editor.findChild<QToolButton*>("annotationTool1")->isChecked());
            drag(editor, QPoint(20, 30), QPoint(100, 70));
            const QImage edited = editor.resultImage();
            QCOMPARE(edited.pixelColor(60, 50), QColor("#ff5252")); // Shift+D selects a filled rectangle.
            canvas->setFocus();
            QTest::keyClick(canvas, Qt::Key_Z, Qt::ControlModifier);
            QCOMPARE(editor.resultImage(), image());
            QTest::keyClick(canvas, Qt::Key_Y, Qt::ControlModifier);
            QCOMPARE(editor.resultImage(), edited);
            QTest::keyClick(canvas, Qt::Key_Z, Qt::ControlModifier);
            QTest::keyClick(canvas, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
            QCOMPARE(editor.resultImage(), edited);
            openPanel(editor, "annotationColorTrigger", "annotationColorPanel");
            auto* dark = editor.findChild<QToolButton*>("annotationThemeDark");
            QVERIFY(dark);
            QVERIFY(dark->isVisible());
            QTest::mouseClick(dark, Qt::LeftButton);
            editor.findChild<QSpinBox*>("annotationWidth")->setValue(11);
            auto* blue = editor.findChild<QToolButton*>("annotationColor4d94ff");
            QVERIFY(blue);
            QVERIFY(blue->isVisible());
            QTest::mouseClick(blue, Qt::LeftButton);
        }
        AnnotationEditor restored;
        initialize(restored, image());
        QCOMPARE(restored.findChild<QSpinBox*>("annotationWidth")->value(), 11);
        QVERIFY(restored.findChild<QToolButton*>("annotationThemeDark")->isChecked());
        selectTool(restored, 5);
        drag(restored, QPoint(20, 30), QPoint(100, 70));
        QCOMPARE(restored.resultImage().pixelColor(60, 50), QColor("#4d94ff"));
    }
};

int main(int argc, char** argv) {
    QTemporaryDir config;
    if (!config.isValid())
        return 1;
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    QApplication app(argc, argv);
    AnnotationEditorTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "annotation_editor_test.moc"
