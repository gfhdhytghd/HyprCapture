#include "ui/annotation_editor.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QImage>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTest>

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
    button->click();
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
        auto* variants = editor.findChild<QComboBox*>("annotationVariant");
        QCOMPARE(variants->count(), 4);
        variants->setCurrentIndex(1);
        drag(editor, QPoint(20, 30), QPoint(80, 80));
        QCOMPARE(editor.resultImage().pixelColor(50, 55), QColor("#ff5252"));
        selectTool(editor, 3);
        QCOMPARE(variants->count(), 4);
        variants->setCurrentIndex(3); // Curved, double headed.
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

    void textIsMultilineAndDialogTypingDoesNotChangeTools() {
        AnnotationEditor editor;
        const QImage source = image();
        initialize(editor, source);
        selectTool(editor, 7);
        QTimer::singleShot(0, &editor, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            auto* input = dialog->findChild<QPlainTextEdit*>();
            QVERIFY(input);
            QTest::keyClicks(input, "TRED");
            QTest::keyClick(input, Qt::Key_Return);
            QTest::keyClicks(input, "second line");
            QCOMPARE(input->toPlainText(), QString("TRED\nsecond line"));
            dialog->accept();
        });
        QTest::mouseClick(editor.findChild<QWidget*>("annotationCanvas"), Qt::LeftButton, Qt::NoModifier, displayed(QPoint(20, 30)));
        QVERIFY(editor.resultImage() != source);
        QVERIFY(editor.findChild<QToolButton*>("annotationTool7")->isChecked());
        editor.undo();
        QCOMPARE(editor.resultImage(), source);
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
        QVERIFY(editor.canvasGeometry().bottom() < 250);
        const QImage native = editor.resultImage();
        editor.setImage(image(QSize(1000, 600), Qt::black), true);
        QVERIFY(editor.rect().contains(editor.canvasGeometry()));
        QCOMPARE(editor.resultImage().size(), native.size());
    }

    void shortcutsAndPreferences() {
        {
            AnnotationEditor editor;
            initialize(editor, image());
            auto* canvas = editor.findChild<QWidget*>("annotationCanvas");
            canvas->setFocus();
            QTest::keyClick(canvas, Qt::Key_D);
            QVERIFY(editor.findChild<QToolButton*>("annotationTool5")->isChecked());
            QVERIFY(!editor.findChild<QComboBox*>("annotationVariant")->isVisible());
            QTest::keyClick(canvas, Qt::Key_D, Qt::ShiftModifier);
            QVERIFY(editor.findChild<QToolButton*>("annotationTool1")->isChecked());
            QCOMPARE(editor.findChild<QComboBox*>("annotationVariant")->currentIndex(), 1);
            drag(editor, QPoint(20, 30), QPoint(100, 70));
            const QImage edited = editor.resultImage();
            canvas->setFocus();
            QTest::keyClick(canvas, Qt::Key_Z, Qt::ControlModifier);
            QCOMPARE(editor.resultImage(), image());
            QTest::keyClick(canvas, Qt::Key_Y, Qt::ControlModifier);
            QCOMPARE(editor.resultImage(), edited);
            QTest::keyClick(canvas, Qt::Key_Z, Qt::ControlModifier);
            QTest::keyClick(canvas, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
            QCOMPARE(editor.resultImage(), edited);
            editor.findChild<QComboBox*>("annotationTheme")->setCurrentIndex(0);
            editor.findChild<QSpinBox*>("annotationWidth")->setValue(11);
            editor.findChild<QToolButton*>("annotationColor62b5ff")->click();
        }
        AnnotationEditor restored;
        initialize(restored, image());
        QCOMPARE(restored.findChild<QSpinBox*>("annotationWidth")->value(), 11);
        QCOMPARE(restored.findChild<QComboBox*>("annotationTheme")->currentIndex(), 0);
        selectTool(restored, 5);
        drag(restored, QPoint(20, 30), QPoint(100, 70));
        QCOMPARE(restored.resultImage().pixelColor(60, 50), QColor("#62b5ff"));
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
