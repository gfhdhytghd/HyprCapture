#include "ui/scroll_capture.hpp"
#include "ui/hdr_thumbnail.hpp"
#include <QtCore/qfloat16.h>
#include <QTemporaryDir>
#include <QFile>
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
    QImage hdr(16,8,QImage::Format_RGBA16FPx4_Premultiplied);
    hdr.fill(Qt::transparent);
    auto* values=reinterpret_cast<qfloat16*>(hdr.bits());
    values[0]=qfloat16(6.f); values[1]=qfloat16(-.125f); values[2]=qfloat16(2.f); values[3]=qfloat16(.5f);
    const auto hdrPath=saveHdrThumbnail(hdr);
    require(!hdrPath.isEmpty(),"private HDR thumbnail transport saved");
    const auto transported=takeHdrThumbnail(hdrPath);
    require(transported==hdr && !QFile::exists(hdrPath),"HDR transport preserves highlights, gamut and alpha and consumes its file");
    QTemporaryDir temp;
    QFile malformed(temp.filePath("invalid")); malformed.open(QIODevice::WriteOnly); malformed.write(QByteArray(32,'x')); malformed.close(); malformed.setPermissions(QFile::ReadOwner|QFile::WriteOwner);
    require(takeHdrThumbnail(malformed.fileName()).isNull(),"malformed HDR thumbnail rejected");
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
