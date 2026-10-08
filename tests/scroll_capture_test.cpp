#include "ui/scroll_capture.hpp"
#include <QApplication>
#include <QPushButton>
#include <QLabel>
#include <QSignalSpy>
#include <QTest>
#include <cstdlib>
#include <iostream>

void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    using namespace hyprcapture::ui;
    const QSize bar(540, 48);
    const QList<QRect> screens{QRect(0, 0, 1920, 1080)};
    const QRect selected(80, 100, 1000, 800);
    auto placed = scrollControlsPlacement(selected, screens, bar);
    require(placed.isValid() && screens.front().contains(placed), "preview on capture monitor");
    require(scrollControlsPlacement(screens.front(), screens, bar).isValid(), "full monitor selection supported by native layer exclusion");
    const QRect negative(-1920,-200,1920,1080);
    placed=scrollControlsPlacement(QRect(-1800,-100,900,600),{negative},bar);
    require(negative.contains(placed),"negative monitor placement");
    require(!scrollControlsPlacement({},screens,bar).isValid(),"invalid selection");
    ScrollCaptureController controller;
    QString error;
    require(!controller.prepare(selected, error) && !error.isEmpty(), "offscreen tests cannot capture the live desktop");
    controller.start();
    require(!controller.isVisible(), "failed preparation cannot start capture");
    auto* finish = controller.findChild<QPushButton*>("scrollCaptureFinish");
    auto* cancel = controller.findChild<QPushButton*>("scrollCaptureCancel");
    require(finish && !finish->isEnabled() && cancel, "no result before a real frame");
    auto* preview=controller.findChild<QLabel*>("thumbnailImage");
    auto* menu=controller.findChild<QWidget*>("thumbnailMenu");
    require(preview && menu && menu->isHidden(), "preview uses the result thumbnail image with collapsed actions");
    require(preview->toolTip().isEmpty(), "preview help cannot spawn a fullscreen tooltip");
    controller.show();
    QTest::mouseClick(preview,Qt::RightButton);
    require(menu->isVisible(), "right click reveals finish and cancel actions");
    QSignalSpy cancelled(&controller, &ScrollCaptureController::cancelled);
    cancel->click();
    cancel->click();
    require(cancelled.count() == 1, "cancel is idempotent");
    return 0;
}
