#include "ui/watermark.hpp"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QRegularExpression>
#include <QSvgRenderer>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace hyprcapture::ui {
namespace {

enum class BuiltinWatermark { None, Hypercam2, Hyprcam2, ActivateLinux };

constexpr qint64 kMaxWatermarkFileBytes = 16 * 1024 * 1024;
constexpr qint64 kMaxWatermarkBytes = 128 * 1024 * 1024;
constexpr int kMaxWatermarkDimension = 32768;

struct Length {
    double value = 0.0;
    bool percent = false;
    bool valid = false;
};

constexpr const char* kActivateLinuxSvg =
    R"SVG(<svg width="340" height="120" viewBox="0 0 340 120" xmlns="http://www.w3.org/2000/svg">
  <g fill="#c4c4c4" fill-opacity="0.7" font-family="sans-serif">
    <text x="20" y="52" font-size="24">Activate Linux</text>
    <text x="20" y="76" font-size="16">Go to Settings to activate Linux.</text>
  </g>
</svg>)SVG";

constexpr const char* kHypercamJpegBase64 =
    R"BASE64(/9j/4AAQSkZJRgABAQEAYABgAAD/4QBARXhpZgAASUkqAAgAAAABAGmHBAABAAAAGgAAAAAAAAACAAKgCQABAAAA4AEAAAOgCQAB
AAAALgAAAAAAAAD/2wBDAAMCAgMCAgMDAwMEAwMEBQgFBQQEBQoHBwYIDAoMDAsKCwsNDhIQDQ4RDgsLEBYQERMUFRUVDA8XGBYU
GBIUFRT/2wBDAQMEBAUEBQkFBQkUDQsNFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBT/
wAARCAAuAeADASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQID
AAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZn
aGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx
8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJB
UQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3
eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6
/9oADAMBAAIRAxEAPwD9RxeKcNlSuF4X5icnGRgnjOPyNScFQRtI+U5HQg1+Qn/BXH4seNvAH7S+hWHhrxj4g8P2T+FbeZ7fS9Vu
LZHkN3d5YhHGTgKPoo9K/V/wJI8/grQJJZGkkfT7eRmY8klATQBrt8pb7xAx0Unr06Z9/wBKnaTaqkpgltpz296/M/8A4LM/ETxT
4AHwgHhnxHq3h/7X/a32n+y72W287Z9h2bvLYZxubH1NfXf7EWsX+v8A7Kfw01LUru4v9Ru9HEstxcSF3kOTyxPU9KAPdPIX1b86
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)BASE64";

constexpr const char* kHyprcamJpegBase64 =
    R"BASE64(/9j/4AAQSkZJRgABAQEAYABgAAD/2wBDAP//////////////////////////////////////////////////////////////
////////////////////////2wBDAf//////////////////////////////////////////////////////////////////
////////////////////wAARCAAuAeADASIAAhEBAxEB/8QAFwABAQEBAAAAAAAAAAAAAAAAAAIBA//EACsQAAICAQIFAwQC
AwAAAAAAAAABAhEhMYESMkFRcWHB0QMikaFCsVKC8P/EABQBAQAAAAAAAAAAAAAAAAAAAAD/xAAUEQEAAAAAAAAAAAAAAAAA
AAAA/9oADAMBAAIRAxEAPwDoDlNu9enydQAOc3pv7Fx0QGgAAAAAAAAAAAAAOclK8XXk6AAAABMnRqdqwNAAAAAAAAAAAA5Q
bb1enwB1AMlowNBzg9dvc6AAcoN3q9Pg6gAAAAAAAAALrU55k+yWLTA6A5tNU02++S07SYGgAAAAAAAAAACJOt7MUbV2/wAg
dAc4SvH7OgAAAcp67fJ1Rynrt8nRAR9Tpv7FLlXgmfTf2KXKvAERcm6tmtuLy7wZDXb4E9dvkC28N+hCcm9X8Fvlfgn6fXb3
ApvhS6kq2uevQ2ei8iMU0rQGRleH+TZSrHcpJLRET6AKlV8XSxBt3bK/h/r7Ew6gJNp4ZbdKznPmOjp6gQrlbusmptOnb9Qo
taNUYpO6ec0AmnreL0NgnV3jsJ6LybDlW/8AYEcVvXhQtpr7uIyKt5OnDHsAb+2/S/0c05Pqzq+V+H/REOoFN8KzkhW1z0VP
TcyKTWUAjLNa51Kk63sUloiZ9N/YAk2r4jIa7fBa5V4Ihrt8AdSZcr8FEy5X4AiLq8N6aFcT/wAWZ9Prt7nQDlDXb4Ni3f3O
vODIa7fBs11A6HO25YeLDmmqzobBUr70BLb4qvsa+JK+JmPn3XsVPTcDU7VnO5PRsuPL+SYa7fAFO+DPp7CGj8my5WZDR+QL
BjdBO1YEW2+sV6/8jOJp68SF8bpYXqZJJPHYDo9G/QiPFK/uot8r8E/T67e4GNyTq2a+JK+Ix8+69ip6bgUnas0iGm5YGUnq
jG0sbUJOt7oyKvLzYCEaz+iwAAAAxq8MlKSuqLAEKOW3r0osACIxaeewlFt2uxYAxq1XoTFNXfUsAY0mSlJKsFgCIxrXU2Ub
89CiZLF28AT9yVYqhDqTxOq9KLiqV9wMlFt3gtq8M0AQlJXVBRzb16UWAMaT1MimsOqKAEcLTuNbjhbauq9CwBlYpdqJjFq7
LAGNJ4ZKUlhUWAIUay9bNav2KAEVJKsCMWnb7FgAY1aaNAERi1dlgARGLTt9imrwzTnN9AJSXFXS2dtCUqKA4y5t0W1J4dEv
n3R1AxKlSJjFp2+xYAEOObj+ywBDUnV1XoVVKjQBHC07jW5jjJ5dHQAY1ar0JjFq76lgDm4tyvGqKkrVIoATFUqKAAiUW6oJ
TSrBYAlcXWtigAAAA//Z)BASE64";

QString normalizedBuiltinId(const std::string& value) {
    auto id = QString::fromStdString(value).trimmed().toLower();
    if (id.startsWith(QLatin1String("builtin:")))
        id = id.mid(8);
    id.replace(QLatin1Char('_'), QLatin1Char('-'));
    id.replace(QLatin1Char(' '), QLatin1Char('-'));
    return id;
}

bool watermarkDisabled(const std::string& value) {
    const auto id = normalizedBuiltinId(value);
    return id.isEmpty() || id == QLatin1String("0") || id == QLatin1String("off") || id == QLatin1String("false") || id == QLatin1String("none") ||
           id == QLatin1String("disabled");
}

BuiltinWatermark builtinWatermarkFor(const std::string& value) {
    const auto id = normalizedBuiltinId(value);
    const auto basename = QFileInfo(id).fileName();
    if (id == QLatin1String("hyprcam") || id == QLatin1String("hyprcam2") || id == QLatin1String("hyprcam-2") ||
        id == QLatin1String("unregistered-hyprcam") || id == QLatin1String("unregistered-hyprcam-2") || id == QLatin1String("unregistered-hyprcam-2.jpg") ||
        basename == QLatin1String("unregistered-hyprcam-2") || basename == QLatin1String("unregistered-hyprcam-2.jpg"))
        return BuiltinWatermark::Hyprcam2;
    if (id == QLatin1String("hypercam") || id == QLatin1String("hypercam2") || id == QLatin1String("hypercam-2") || id == QLatin1String("unregistered") ||
        id == QLatin1String("unregistered-hypercam") || id == QLatin1String("unregistered-hypercam-2") || id == QLatin1String("unregistered-hypercam-2.jpg") ||
        basename == QLatin1String("unregistered") || basename == QLatin1String("unregistered-hypercam-2") ||
        basename == QLatin1String("unregistered-hypercam-2.jpg"))
        return BuiltinWatermark::Hypercam2;
    if (id == QLatin1String("activate-linux") || id == QLatin1String("activatelinux") || id == QLatin1String("linux-activation"))
        return BuiltinWatermark::ActivateLinux;
    return BuiltinWatermark::None;
}

bool isDefaultWatermarkWidth(const std::string& value) {
    auto normalized = QString::fromStdString(value).trimmed().toLower();
    normalized.remove(QLatin1Char(' '));
    return normalized == QLatin1String("20%");
}

Length parseLength(QString token) {
    token = token.trimmed().toLower();
    Length length;
    if (token.endsWith(QLatin1Char('%'))) {
        length.percent = true;
        token.chop(1);
    } else if (token.endsWith(QLatin1String("px"))) {
        token.chop(2);
    }

    bool ok = false;
    length.value = token.trimmed().toDouble(&ok);
    length.valid = ok;
    return length;
}

double resolveLength(const Length& length, double reference) { return length.percent ? reference * length.value / 100.0 : length.value; }

bool imageSizeWithinBounds(const QSize& size) {
    if (size.isEmpty() || size.width() <= 0 || size.height() <= 0 || size.width() > kMaxWatermarkDimension || size.height() > kMaxWatermarkDimension)
        return false;

    const auto width = static_cast<qint64>(size.width());
    const auto height = static_cast<qint64>(size.height());
    if (width > std::numeric_limits<qint64>::max() / height)
        return false;
    const auto pixels = width * height;
    if (pixels > std::numeric_limits<qint64>::max() / 4)
        return false;
    return pixels * 4 <= kMaxWatermarkBytes;
}

QSize scaledWatermarkSize(const QSize& sourceSize, int targetWidth) {
    if (sourceSize.isEmpty() || sourceSize.width() <= 0 || sourceSize.height() <= 0 || targetWidth <= 0 || targetWidth > kMaxWatermarkDimension)
        return {};

    const double targetHeight = std::round(static_cast<double>(targetWidth) * sourceSize.height() / sourceSize.width());
    if (!std::isfinite(targetHeight) || targetHeight < 1.0 || targetHeight > kMaxWatermarkDimension)
        return {};

    const QSize size(targetWidth, std::max(1, static_cast<int>(targetHeight)));
    return imageSizeWithinBounds(size) ? size : QSize{};
}

std::array<Length, 2> parseVec2(QString value) {
    std::array<Length, 2> result{Length{}, Length{}};
    static const QRegularExpression tokenPattern(QStringLiteral(R"([-+]?(?:\d+(?:\.\d*)?|\.\d+)\s*(?:%|px)?)"));
    auto it = tokenPattern.globalMatch(value);
    for (int i = 0; i < 2 && it.hasNext(); ++i)
        result[static_cast<std::size_t>(i)] = parseLength(it.next().captured(0));
    return result;
}

int targetWatermarkWidth(const QImage& target, const CaptureDefaults& defaults, BuiltinWatermark builtin) {
    const auto width = parseLength(QString::fromStdString(defaults.watermarkWidth));
    if (!width.valid)
        return 0;

    const double pixels = resolveLength(width, target.width());
    if (pixels <= 0.0 || !std::isfinite(pixels))
        return 0;

    const int maxWidth = target.width() > kMaxWatermarkDimension / 4 ? kMaxWatermarkDimension : std::clamp(target.width() * 4, 1, kMaxWatermarkDimension);
    int targetWidth = std::clamp(static_cast<int>(std::round(pixels)), 1, maxWidth);
    if ((builtin == BuiltinWatermark::Hypercam2 || builtin == BuiltinWatermark::Hyprcam2) && isDefaultWatermarkWidth(defaults.watermarkWidth)) {
        constexpr double kCamAspect = 480.0 / 46.0;
        const int widthForReadableHeight = static_cast<int>(std::round(std::max(46.0, target.height() * 0.065) * kCamAspect));
        const int defaultMaxWidth = std::max(1, static_cast<int>(std::round(target.width() * 0.8)));
        targetWidth = std::max(targetWidth, std::min(widthForReadableHeight, defaultMaxWidth));
    }
    return targetWidth;
}

QImage scaledRasterWatermark(QImage image, int targetWidth) {
    if (image.isNull() || targetWidth <= 0 || image.width() <= 0 || image.height() <= 0)
        return {};

    const QSize targetSize = scaledWatermarkSize(image.size(), targetWidth);
    if (targetSize.isEmpty())
        return {};

    image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (image.size() == targetSize)
        return image;
    return image.scaled(targetSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

QImage renderSvgWatermark(const QByteArray& svg, int targetWidth) {
    QSvgRenderer renderer(svg);
    if (!renderer.isValid() || targetWidth <= 0)
        return {};

    QSize sourceSize = renderer.defaultSize();
    if (!sourceSize.isValid() || sourceSize.isEmpty())
        sourceSize = QSize(340, 120);

    const QSize targetSize = scaledWatermarkSize(sourceSize, targetWidth);
    if (targetSize.isEmpty())
        return {};

    QImage image(targetSize, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    renderer.render(&painter, QRectF(0, 0, targetSize.width(), targetSize.height()));
    return image;
}

QImage loadBuiltinWatermark(BuiltinWatermark builtin, int targetWidth) {
    if (builtin == BuiltinWatermark::ActivateLinux)
        return renderSvgWatermark(QByteArray(kActivateLinuxSvg), targetWidth);
    if (builtin == BuiltinWatermark::Hypercam2 || builtin == BuiltinWatermark::Hyprcam2) {
        QByteArray encoded(builtin == BuiltinWatermark::Hyprcam2 ? kHyprcamJpegBase64 : kHypercamJpegBase64);
        encoded.replace("\n", "");
        encoded.replace("\r", "");
        encoded.replace(" ", "");
        return scaledRasterWatermark(QImage::fromData(QByteArray::fromBase64(encoded), "JPG"), targetWidth);
    }
    return {};
}

QImage loadFileWatermark(const std::string& configuredPath, int targetWidth) {
    const auto path = QString::fromStdString(expandUserPath(configuredPath).string());
    if (path.isEmpty() || !QFileInfo::exists(path))
        return {};

    const QFileInfo info(path);
    if (!info.isFile() || info.size() < 0 || info.size() > kMaxWatermarkFileBytes)
        return {};

    const auto suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("svg"))
        return {};

    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize targetSize = scaledWatermarkSize(reader.size(), targetWidth);
    if (targetSize.isEmpty())
        return {};

    const int previousAllocationLimit = QImageReader::allocationLimit();
    QImageReader::setAllocationLimit(static_cast<int>(kMaxWatermarkBytes / (1024 * 1024)));
    reader.setScaledSize(targetSize);
    const QImage image = reader.read();
    QImageReader::setAllocationLimit(previousAllocationLimit);
    return scaledRasterWatermark(image, targetWidth);
}

QImage loadWatermark(const CaptureDefaults& defaults, int targetWidth, BuiltinWatermark builtin) {
    if (builtin != BuiltinWatermark::None)
        return loadBuiltinWatermark(builtin, targetWidth);
    return loadFileWatermark(defaults.watermark, targetWidth);
}

QPoint watermarkPosition(const QSize& targetSize, const QSize& watermarkSize, WatermarkPosition position, const QString& offsetConfig) {
    double x = 0.0;
    double y = 0.0;
    const double centerX = (targetSize.width() - watermarkSize.width()) / 2.0;
    const double centerY = (targetSize.height() - watermarkSize.height()) / 2.0;
    const double right = targetSize.width() - watermarkSize.width();
    const double bottom = targetSize.height() - watermarkSize.height();

    switch (position) {
    case WatermarkPosition::UpLeft:
        x = 0.0;
        y = 0.0;
        break;
    case WatermarkPosition::UpMiddle:
        x = centerX;
        y = 0.0;
        break;
    case WatermarkPosition::UpRight:
        x = right;
        y = 0.0;
        break;
    case WatermarkPosition::LeftMiddle:
        x = 0.0;
        y = centerY;
        break;
    case WatermarkPosition::Central:
        x = centerX;
        y = centerY;
        break;
    case WatermarkPosition::RightMiddle:
        x = right;
        y = centerY;
        break;
    case WatermarkPosition::DownLeft:
        x = 0.0;
        y = bottom;
        break;
    case WatermarkPosition::DownMiddle:
        x = centerX;
        y = bottom;
        break;
    case WatermarkPosition::DownRight:
        x = right;
        y = bottom;
        break;
    }

    const auto offset = parseVec2(offsetConfig);
    if (offset[0].valid)
        x += resolveLength(offset[0], targetSize.width());
    if (offset[1].valid)
        y += resolveLength(offset[1], targetSize.height());

    return QPoint(static_cast<int>(std::round(x)), static_cast<int>(std::round(y)));
}

} // namespace

void applyWatermark(QImage& image, const CaptureDefaults& defaults) {
    if (image.isNull() || watermarkDisabled(defaults.watermark))
        return;

    const BuiltinWatermark builtin = builtinWatermarkFor(defaults.watermark);
    const int targetWidth = targetWatermarkWidth(image, defaults, builtin);
    if (targetWidth <= 0)
        return;

    const QImage watermark = loadWatermark(defaults, targetWidth, builtin);
    if (watermark.isNull())
        return;

    if (image.format() != QImage::Format_ARGB32_Premultiplied)
        image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    QPainter painter(&image);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(watermarkPosition(image.size(), watermark.size(), defaults.watermarkPosition, QString::fromStdString(defaults.watermarkOffset)),
                      watermark);
}

} // namespace hyprcapture::ui
