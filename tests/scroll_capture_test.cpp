#include "ui/scroll_capture.hpp"
#include <QApplication>
#include <QPushButton>
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
    require(placed.isValid() && screens.front().contains(placed) && !placed.intersects(selected), "controls stay out of captured pixels");
    require(!scrollControlsPlacement(screens.front(), screens, bar).isValid(), "full monitor selection has no free control space");
    const QList<QRect> dual{screens.front(), QRect(-1920, -200, 1920, 1080)};
    placed = scrollControlsPlacement(screens.front(), dual, bar);
    require(placed.isValid() && dual[1].contains(placed), "other monitor with negative origin");
    const QRect negativeSelection(-1800, -100, 900, 600);
    placed = scrollControlsPlacement(negativeSelection, {dual[1]}, bar);
    require(placed.isValid() && dual[1].contains(placed) && !placed.intersects(negativeSelection), "negative selection coordinates");
    placed = scrollControlsPlacement(QRect(0, 58, 1920, 1022), screens, bar);
    require(placed.isValid() && placed.bottom() < 58, "controls fit outside app below a normal desktop bar");
    require(!scrollControlsPlacement({}, screens, bar).isValid(), "invalid selection rejected");
    ScrollCaptureController controller;
    QString error;
    require(!controller.prepare(selected, error) && !error.isEmpty(), "offscreen tests cannot capture the live desktop");
    controller.start();
    require(!controller.isVisible(), "failed preparation cannot start capture");
    auto* finish = controller.findChild<QPushButton*>("scrollCaptureFinish");
    auto* cancel = controller.findChild<QPushButton*>("scrollCaptureCancel");
    require(finish && !finish->isEnabled() && cancel, "no result before a real frame");
    QSignalSpy cancelled(&controller, &ScrollCaptureController::cancelled);
    cancel->click();
    cancel->click();
    require(cancelled.count() == 1, "cancel is idempotent");
    return 0;
}
