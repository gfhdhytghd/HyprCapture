#include "shared/config.hpp"
#include "ui/watermark.hpp"

#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QRect>
#include <QTemporaryDir>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {

bool imageDiffers(const QImage& a, const QImage& b) {
    if (a.size() != b.size() || a.format() != b.format() || a.sizeInBytes() != b.sizeInBytes())
        return true;
    return std::memcmp(a.constBits(), b.constBits(), static_cast<std::size_t>(a.sizeInBytes())) != 0;
}

QRect diffBounds(const QImage& a, const QImage& b) {
    QRect bounds;
    for (int y = 0; y < std::min(a.height(), b.height()); ++y) {
        for (int x = 0; x < std::min(a.width(), b.width()); ++x) {
            if (a.pixel(x, y) != b.pixel(x, y))
                bounds = bounds.united(QRect(x, y, 1, 1));
        }
    }
    return bounds;
}

void require(bool condition, const char* message) {
    if (condition)
        return;

    std::cerr << "watermark test failed: " << message << '\n';
    std::exit(1);
}

} // namespace

int main(int argc, char** argv) {
    using namespace hyprcapture;

    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    QImage base(QSize(800, 600), QImage::Format_ARGB32_Premultiplied);
    base.fill(Qt::white);

    CaptureDefaults defaults;
    defaults.watermark = "activate-linux";
    defaults.watermarkPosition = WatermarkPosition::Central;
    defaults.watermarkWidth = "25%";
    defaults.watermarkOffset = "0 0";

    auto activate = base;
    ui::applyWatermark(activate, defaults);
    require(imageDiffers(base, activate), "activate-linux changes image");

    defaults.watermark = "hypercam2";
    defaults.watermarkPosition = WatermarkPosition::DownRight;
    defaults.watermarkWidth = "20%";
    defaults.watermarkOffset = "-10px -10px";

    auto hypercam = base;
    ui::applyWatermark(hypercam, defaults);
    require(imageDiffers(base, hypercam), "hypercam changes image");
    require(diffBounds(base, hypercam).height() >= 32, "hypercam minimum rendered height");

    defaults.watermark = "/missing/unregistered hypercam 2.jpg";
    auto hypercamPathAlias = base;
    ui::applyWatermark(hypercamPathAlias, defaults);
    require(!imageDiffers(hypercam, hypercamPathAlias), "hypercam filename alias renders hypercam preset");

    defaults.watermark = "hyprcam2";
    auto hyprcam = base;
    ui::applyWatermark(hyprcam, defaults);
    require(imageDiffers(base, hyprcam), "hyprcam changes image");
    require(imageDiffers(hypercam, hyprcam), "hyprcam is a separate built-in preset");

    defaults.watermark = "/missing/unregistered hyprcam 2.jpg";
    auto hyprcamPathAlias = base;
    ui::applyWatermark(hyprcamPathAlias, defaults);
    require(!imageDiffers(hyprcam, hyprcamPathAlias), "hyprcam filename alias renders hyprcam preset");

    QTemporaryDir tempDir;
    require(tempDir.isValid(), "temporary watermark dir");
    const QString svgPath = tempDir.filePath(QStringLiteral("external.svg"));
    QFile         svg(svgPath);
    require(svg.open(QIODevice::WriteOnly), "external svg opens");
    svg.write(R"(<svg width="20" height="20" xmlns="http://www.w3.org/2000/svg"><rect width="20" height="20" fill="red"/></svg>)");
    svg.close();

    defaults.watermark = svgPath.toStdString();
    auto externalSvg = base;
    ui::applyWatermark(externalSvg, defaults);
    require(!imageDiffers(base, externalSvg), "external svg watermark is ignored");

    defaults.watermark.clear();
    auto disabled = base;
    ui::applyWatermark(disabled, defaults);
    require(!imageDiffers(base, disabled), "disabled watermark leaves image unchanged");

    std::cout << "hyprcapture watermark tests passed\n";
    return 0;
}
