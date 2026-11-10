#include "ui/thumbnail_style.hpp"
#include <QApplication>
#include <cstdlib>
#include <iostream>

using namespace hyprcapture;
using namespace hyprcapture::ui::thumbnail;

namespace {
void require(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "thumbnail style test failed: " << message << '\n';
        std::exit(1);
    }
}

// Frozen pre-customization implementations: default output must remain exact.
QString legacyStyle(const QPalette &palette) {
    const QColor bg = palette.color(QPalette::Window);
    const QColor fg = palette.color(QPalette::WindowText);
    const QColor highlight = palette.color(QPalette::Highlight);
    return hyprcapture::ui::thumbnail::imageStyleSheet() +
           QStringLiteral("#thumbnail { background: transparent; border: none; }"
                          "#thumbnailMenu { background: rgba(%1,%2,%3,242); border: 1px solid rgba(%4,%5,%6,90); border-radius: 7px; }"
                          "#thumbnailOpenWithMenu { background: rgba(%1,%2,%3,242); border: 1px solid rgba(%4,%5,%6,90); border-radius: 7px; }"
                          "#thumbnailMenu QPushButton { color: rgba(%4,%5,%6,255); background: transparent; padding: 7px 10px; border: none; border-radius: "
                          "5px; text-align: left; }"
                          "#thumbnailOpenWithMenu QPushButton { color: rgba(%4,%5,%6,255); background: transparent; padding: 7px 10px; border: none; "
                          "border-radius: 5px; text-align: left; }"
                          "#thumbnailMenu QPushButton:disabled { color: rgba(%4,%5,%6,100); }"
                          "#thumbnailMenu QPushButton:hover { background: rgba(%7,%8,%9,75); }"
                          "#thumbnailOpenWithMenu QPushButton:hover { background: rgba(%7,%8,%9,75); }")
               .arg(bg.red())
               .arg(bg.green())
               .arg(bg.blue())
               .arg(fg.red())
               .arg(fg.green())
               .arg(fg.blue())
               .arg(highlight.red())
               .arg(highlight.green())
               .arg(highlight.blue());
}
QPixmap legacyScaledPixmap(const QPixmap &pixmap, const QScreen *targetScreen) {
    if (pixmap.isNull())
        return {};

    const QSizeF logicalPixmapSize = pixmap.deviceIndependentSize();
    QSize targetLogicalSize(std::max(1, static_cast<int>(std::ceil(logicalPixmapSize.width()))),
                            std::max(1, static_cast<int>(std::ceil(logicalPixmapSize.height()))));
    if (logicalPixmapSize.width() > kThumbnailMaxWidth || logicalPixmapSize.height() > kThumbnailMaxHeight)
        targetLogicalSize = targetLogicalSize.scaled(kThumbnailMaxWidth, kThumbnailMaxHeight, Qt::KeepAspectRatio);
    targetLogicalSize = targetLogicalSize.expandedTo(QSize(1, 1));

    qreal dpr = targetDevicePixelRatio(pixmap, targetScreen);
    dpr = std::min(dpr, static_cast<qreal>(pixmap.width()) / targetLogicalSize.width());
    dpr = std::min(dpr, static_cast<qreal>(pixmap.height()) / targetLogicalSize.height());
    dpr = std::clamp(dpr, qreal{1.0}, kThumbnailMaxDevicePixelRatio);

    const QSize targetPhysicalSize(std::max(1, static_cast<int>(std::ceil(targetLogicalSize.width() * dpr))),
                                   std::max(1, static_cast<int>(std::ceil(targetLogicalSize.height() * dpr))));

    QPixmap scaledPixmap = pixmap;
    if (scaledPixmap.size() != targetPhysicalSize || !qFuzzyCompare(scaledPixmap.devicePixelRatio(), dpr)) {
        scaledPixmap = scaledPixmap.scaled(targetPhysicalSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        scaledPixmap.setDevicePixelRatio(dpr);
    }
    return scaledPixmap;
}

} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    for (const auto sameColors : {false, true}) {
        auto palette = app.palette();
        palette.setColor(QPalette::Window, QColor(10, 20, 30, 11));
        palette.setColor(QPalette::WindowText, sameColors ? QColor(10, 20, 30, 12) : QColor(40, 50, 60, 12));
        palette.setColor(QPalette::Highlight, sameColors ? QColor(10, 20, 30, 13) : QColor(70, 80, 90, 13));
        require(resultStyleSheet(palette).toUtf8() == legacyStyle(palette).toUtf8(), "default stylesheet bytes unchanged");
        CaptureDefaults invalidColors;
        invalidColors.thumbnailBackground = "red; } QWidget { color: red";
        invalidColors.thumbnailForeground = "#gg000000";
        invalidColors.thumbnailAccent = "rgba(123)";
        invalidColors.thumbnailBorderColor = "#123";
        require(resultStyleSheet(palette, invalidColors) == legacyStyle(palette), "invalid colors preserve palette defaults");
    }
    for (const auto dpr : {1.0, 1.25, 2.0, 4.0, 5.0}) {
        for (const auto size : {QSize(1, 1), QSize(40, 20), QSize(721, 481), QSize(1280, 800), QSize(900, 100), QSize(100, 900)}) {
            QImage image(size, QImage::Format_ARGB32);
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x)
                    image.setPixelColor(x, y, QColor(x % 256, y % 256, (x + y) % 256, (x * 3 + y) % 256));
            auto pixmap = QPixmap::fromImage(image);
            pixmap.setDevicePixelRatio(dpr);
            const auto before = legacyScaledPixmap(pixmap, nullptr);
            const auto after = scaledPixmap(pixmap, nullptr);
            require(before.devicePixelRatio() == after.devicePixelRatio() && before.toImage() == after.toImage(), "default scaled pixels and DPR unchanged");
        }
    }
    require(scaledPixmap({}, nullptr).isNull(), "null pixmap accepted");
    QPixmap large(800, 400);
    large.fill(Qt::blue);
    require(scaledPixmap(large, nullptr, QSize(320, 200)).deviceIndependentSize() == QSizeF(320, 160), "custom maximum box");
    require(scaledPixmap(large, nullptr, QSize(-1, 9000)).deviceIndependentSize() == QSizeF(48, 24), "maximum box clamped");
    QPixmap small(20, 10);
    small.fill(Qt::green);
    require(scaledPixmap(small, nullptr, QSize(1024, 1024)).size() == small.size(), "never enlarge small images");
    const QColor rgba(0x12, 0x34, 0x56, 0x78);
    require(parseColor("#123456") == QColor(0x12, 0x34, 0x56), "RGB color");
    for (const auto text : {"#12345678", "rgba(12345678)", "0x78123456", " RGBA(12345678) "})
        require(parseColor(text) == rgba, "RGBA and ARGB formats agree");
    for (const auto text : {"", "red", "#fff", "#12345g", "rgba(1234567g)", "0x123456", "#123456789", "#１２３４５６"})
        require(!parseColor(text).isValid(), "invalid color rejected");
    CaptureDefaults custom;
    custom.thumbnailBackground = "#10203040";
    custom.thumbnailForeground = "#50607080";
    custom.thumbnailAccent = "#90a0b0c0";
    custom.thumbnailBorderColor = "0x10203040";
    custom.thumbnailBorderSize = 4;
    custom.thumbnailRounding = 5;
    auto style = resultStyleSheet(app.palette(), custom);
    require(style.contains("background: rgba(16,32,48,64)") && style.contains("color: rgba(80,96,112,128)") &&
                style.contains("background: rgba(144,160,176,192)") && style.contains("border: 4px solid rgba(32,48,64,16)") &&
                style.contains("border-radius: 5px") && style.contains("border-radius: 3px"),
            "explicit colors/alpha/border/radius");
    for (const auto position : {"top_left", "top_right", "bottom_left", "bottom_right"}) {
        custom.thumbnailPosition = position;
        custom.thumbnailMargin = 31;
        const auto alignment = positionAlignment(custom);
        const auto rect = cardImageRect(QSize(180, 120), custom);
        const bool left = std::string_view(position).ends_with("left");
        const bool top = std::string_view(position).starts_with("top");
        require(alignment.testFlag(left ? Qt::AlignLeft : Qt::AlignRight) && alignment.testFlag(top ? Qt::AlignTop : Qt::AlignBottom), "four anchor corners");
        require(rect == QRect(left ? 31 : 0, top ? 31 : 0, 180, 120), "margin follows chosen screen edges");
    }
    require(cardImageRect(QSize(180, 120), {}) == QRect(0, 0, 180, 120), "default image origin unchanged");
    custom.thumbnailMaxWidth = 333;
    custom.thumbnailMaxHeight = 222;
    custom.thumbnailOpacity = 75;
    custom.thumbnailAnimation = false;
    auto args = QStringList{"thumbnail-test"};
    for (const auto &argument : thumbnailStyleArguments(custom))
        args << QString::fromStdString(argument);
    QCommandLineParser parser;
    const auto defaults = thumbnailStyleArguments({});
    for (std::size_t i = 0; i < defaults.size(); i += 2)
        parser.addOption({QString::fromStdString(defaults[i].substr(2)), "Thumbnail style", "value", QString::fromStdString(defaults[i + 1])});
    require(parser.parse(args), "explicit helper arguments accepted");
    CaptureDefaults parsed;
    readStyleOptions(parser, parsed);
    require(thumbnailStyleArguments(parsed) == thumbnailStyleArguments(custom), "helper style arguments round trip");
    require(parser.parse({"thumbnail-test", "--thumbnail-max-width", "-100", "--thumbnail-max-height", "5000", "--thumbnail-position", "unknown",
                          "--thumbnail-opacity", "nan", "--thumbnail-rounding", "-1", "--thumbnail-margin", "900", "--thumbnail-border-size", "999",
                          "--thumbnail-animation", "garbage"}),
            "untrusted CLI parses");
    readStyleOptions(parser, parsed);
    require(parsed.thumbnailMaxWidth == 48 && parsed.thumbnailMaxHeight == 1024 && parsed.thumbnailPosition == "bottom_right" &&
                parsed.thumbnailOpacity == 100 && parsed.thumbnailRounding == 0 && parsed.thumbnailMargin == 512 && parsed.thumbnailBorderSize == 16 &&
                parsed.thumbnailAnimation,
            "UI clamps or defaults invalid helper input");
    for (const char *key : {"thumbnail-border-color", "thumbnail-background", "thumbnail-foreground", "thumbnail-accent"}) {
        require(parser.parse({"thumbnail-test", "--" + QString::fromLatin1(key), QString(140000, 'x')}), "oversized helper color parses");
        readStyleOptions(parser, parsed);
        require(parsed.thumbnailBorderColor.empty() && parsed.thumbnailBackground.empty() && parsed.thumbnailForeground.empty() &&
                    parsed.thumbnailAccent.empty(),
                "UI independently clears oversized helper colors");
    }
    const auto paddedColor = std::string(57, ' ') + "#123456";
    require(normalizedThumbnailColor(paddedColor) == "#123456", "64-byte color input normalizes before transport");
    require(normalizedThumbnailColor(" " + paddedColor).empty(), "65-byte color input rejected before trimming");
    for (const auto text : {"#123456", "#12345678", "rgba(12345678)", "0x78123456", " RGBA(12345678) "}) {
        const auto normalized = normalizedThumbnailColor(text);
        require(!normalized.empty() && normalized.size() <= 14 && parseColor(QString::fromStdString(normalized)) == parseColor(text),
                "shared validation preserves supported color values");
    }
    for (const auto text : {"red", "#fff", "#12345g", "rgba(1234567g)", "0x123456", "#123456789", "#１２３４５６"})
        require(normalizedThumbnailColor(text).empty(), "shared validation clears malformed colors");
    return 0;
}
