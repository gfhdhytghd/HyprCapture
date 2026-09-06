#include <QScrollArea>
#include <QSlider>
#include "ui/audio_meter.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCoreApplication>
#include "ui/capture_overlay.hpp"

#include "ui/clipboard_utils.hpp"
#include "ui/overlay_paint.hpp"
#include "ui/result_thumbnail.hpp"
#include "ui/screenshot_notification.hpp"
#include "ui/watermark.hpp"
#include "shared/protocol.hpp"
#include "shared/audio_source.hpp"

#include <LayerShellQt/Window>

#include <QApplication>
#include <QByteArray>
#include <QButtonGroup>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEnterEvent>
#include <QFile>
#include <QFrame>
#include <QFileInfo>
#include <QGuiApplication>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QIcon>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QMetaObject>
#include <QPalette>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QProcess>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QSizePolicy>
#include <QStringList>
#include <QStyleHints>
#include <QSvgRenderer>
#include <QThread>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QTimer>
#include <QTransform>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <utility>

class InlineSelect final : public QWidget {
  public:
    explicit InlineSelect(QWidget* popupParent, QWidget* parent = nullptr);

    void addItems(const QStringList& items);
    void setPrefix(const QString& prefix);
    void setLabels(const std::map<QString, QString>& labels) { m_labels = labels; addItems(m_items); setCurrentText(m_current); }
    void setOnOpening(std::function<void()> callback) { m_onOpening = std::move(callback); }
    void setCompactWidth(int width) { if (m_compactWidth == width) return; m_compactWidth = width; addItems(m_items); setCurrentText(m_current); }

    void setSelectionHint(const QString& hint) { if (m_hint == hint) return; m_hint = hint; setCurrentText(m_current); }
    void setOnChanged(std::function<void()> onChanged);
    void setCurrentText(const QString& text);
    QString currentText() const;
    void setControlVisible(bool visible);
    void hidePopup();
    bool isPopupVisible() const;

  private:
    QString buttonText(const QString& text) const;
    void updateButtonIcon();
    void showPopup();
    void positionPopup();
    void choose(const QString& text);

    QWidget*     m_popupParent = nullptr;
    QPushButton* m_button = nullptr;
    QFrame*      m_panel = nullptr;
    QVBoxLayout* m_panelLayout = nullptr;
    int          m_buttonWidth = 0;
    QStringList  m_items;
    QString      m_current;
    QString      m_prefix;
    QString      m_hint;
    std::map<QString, QString> m_labels;
    std::function<void()> m_onOpening;
    int m_compactWidth = 0;

    std::function<void()> m_onChanged;
};

namespace {

InlineSelect* g_openSelect = nullptr;
constexpr double kWindowFrameFallbackRadius = 8.0;
constexpr int kOverlayFadeDurationMs = 100;
constexpr int kModeIconSize = 24;
constexpr int kCancelIconSize = 16;
constexpr int kConfirmIconSize = 20;
constexpr int kSelectArrowIconSize = 12;
constexpr int kSelectionHandleHitRadius = 10;
constexpr int kSelectionHandlePaintSize = 8;
constexpr int kMaxArtifactDimension = 32768;
constexpr qint64 kMaxArtifactBytes = 512LL * 1024LL * 1024LL;
constexpr qint64 kMaxSessionArtifactBytes = 768LL * 1024LL * 1024LL;
constexpr qint64 kMaxSessionJsonBytes = 8LL * 1024LL * 1024LL;
constexpr int kMaxLogicalCoordinate = 1'000'000;
constexpr int kMaxSessionMonitors = 64;
constexpr int kMaxSessionWindows = 512;
constexpr int kMaxRecordCountdownSeconds = 60;
constexpr int kThumbnailPreviewMaxWidth = 720;
constexpr int kThumbnailPreviewMaxHeight = 480;

struct CaptureOutputResult {
    QString savedPath;
    QString restoreClipboardPath;
    bool    showThumbnail = false;
    bool    clipboardRequested = false;
    bool    clipboardCopied = false;
};

struct DispatchCommandResult {
    bool    success = false;
    QString error;
};

struct TransparentAutoChoice {
    QString format;
    QString codec;
    QString warning;
};

const char* kFullscreenSvg = R"(<svg viewBox="0 0 1024 1024" xmlns="http://www.w3.org/2000/svg"><path d="M128 266.666667v490.666666a53.393333 53.393333 0 0 0 53.333333 53.333334h661.333334a53.393333 53.393333 0 0 0 53.333333-53.333334V266.666667a53.393333 53.393333 0 0 0-53.333333-53.333334H181.333333a53.393333 53.393333 0 0 0-53.333333 53.333334z m725.333333 0v490.666666a10.666667 10.666667 0 0 1-10.666666 10.666667H181.333333a10.666667 10.666667 0 0 1-10.666666-10.666667V266.666667a10.666667 10.666667 0 0 1 10.666666-10.666667h661.333334a10.666667 10.666667 0 0 1 10.666666 10.666667z m-597.333333 608a21.333333 21.333333 0 0 1-21.333333 21.333333H96a53.393333 53.393333 0 0 1-53.333333-53.333333v-138.666667a21.333333 21.333333 0 0 1 42.666666 0v138.666667a10.666667 10.666667 0 0 0 10.666667 10.666666h138.666667a21.333333 21.333333 0 0 1 21.333333 21.333334zM42.666667 320V181.333333a53.393333 53.393333 0 0 1 53.333333-53.333333h138.666667a21.333333 21.333333 0 0 1 0 42.666667H96a10.666667 10.666667 0 0 0-10.666667 10.666666v138.666667a21.333333 21.333333 0 0 1-42.666666 0z m938.666666-138.666667v138.666667a21.333333 21.333333 0 0 1-42.666666 0V181.333333a10.666667 10.666667 0 0 0-10.666667-10.666666h-138.666667a21.333333 21.333333 0 0 1 0-42.666667h138.666667a53.393333 53.393333 0 0 1 53.333333 53.333333z m0 522.666667v138.666667a53.393333 53.393333 0 0 1-53.333333 53.333333h-138.666667a21.333333 21.333333 0 0 1 0-42.666667h138.666667a10.666667 10.666667 0 0 0 10.666667-10.666666v-138.666667a21.333333 21.333333 0 0 1 42.666666 0z" fill="#000000"/></svg>)";
const char* kWindowSvg = R"(<svg viewBox="0 0 1024 1024" xmlns="http://www.w3.org/2000/svg"><path d="M808.125883 243.195881 134.874315 243.195881c-30.608112 0-55.513338 24.905226-55.513338 55.520501l0 505.178641c0 30.615275 24.905226 55.520501 55.513338 55.520501L808.125883 859.415524c30.607088 0 55.512315-24.905226 55.512315-55.520501L863.638197 298.716382C863.638197 268.101107 838.733994 243.195881 808.125883 243.195881zM835.629283 803.895023c0 15.167444-12.338003 27.510564-27.503401 27.510564L134.874315 831.405587c-15.167444 0-27.504424-12.343119-27.504424-27.510564L107.369891 383.246591l728.259392 0L835.629283 803.895023zM835.629283 355.236654 107.370915 355.236654l0-56.519248c0-15.173584 12.33698-27.510564 27.504424-27.510564L808.125883 271.206842c15.165398 0 27.503401 12.33698 27.503401 27.510564L835.629283 355.236654zM920.166655 131.156132 274.924002 131.156132c-30.608112 0-55.513338 24.905226-55.513338 55.514361l0 28.515451c0 7.734148 6.263657 14.004969 14.005992 14.004969 7.740288 0 14.005992-6.27082 14.005992-14.004969l0-28.515451c0-15.167444 12.33698-27.504424 27.503401-27.504424L920.167678 159.166069c15.165398 0 27.503401 12.33698 27.503401 27.504424l0 519.188726c0 15.167444-12.338003 27.511587-27.503401 27.511587l-28.516474 0c-7.739265 0-14.004969 6.27082-14.004969 14.004969 0 7.736195 6.263657 14.007015 14.004969 14.007015l28.516474 0c30.607088 0 55.512315-24.905226 55.512315-55.521524L975.679993 186.670493C975.67897 156.061358 950.773743 131.156132 920.166655 131.156132zM219.410664 299.216779l-56.019875 0c-7.740288 0-14.005992 6.27082-14.005992 13.998829 0 7.740288 6.263657 14.011108 14.005992 14.011108l56.019875 0c7.740288 0 14.005992-6.27082 14.005992-14.011108C233.415632 305.487599 227.151975 299.216779 219.410664 299.216779zM331.450413 299.216779l-56.019875 0c-7.741311 0-14.005992 6.27082-14.005992 13.998829 0 7.740288 6.262634 14.011108 14.005992 14.011108l56.019875 0c7.739265 0 14.004969-6.27082 14.004969-14.011108C345.455381 305.487599 339.191724 299.216779 331.450413 299.216779zM443.490162 299.216779l-56.018851 0c-7.741311 0-14.007015 6.27082-14.007015 13.998829 0 7.740288 6.263657 14.011108 14.007015 14.011108l56.018851 0c7.740288 0 14.005992-6.27082 14.005992-14.011108C457.49513 305.487599 451.231473 299.216779 443.490162 299.216779z" fill="#000000"/></svg>)";
const char* kRegionSvg = R"(<svg viewBox="0 0 1024 1024" xmlns="http://www.w3.org/2000/svg"><path d="M960 256V64H768v64H256V64H64v192h64v512H64v192h192v-64h512v64h192V768h-64V256z m-128 512h-64v64H256v-64h-64V256h64v-64h512v64h64z" fill="#000000"/></svg>)";
const char* kCancelSvg = R"(<svg viewBox="0 0 1024 1024" xmlns="http://www.w3.org/2000/svg"><path d="M883.8304 41.01546667L512.00213333 412.84693333 140.1696 41.01546667c-27.38026667-27.3792-71.77386667-27.3792-99.1552 0-27.37813333 27.3792-27.37813333 71.77066667 0 99.1552l371.8336 371.83146666L41.0144 883.82933333c-27.37813333 27.38026667-27.37813333 71.776 0 99.15413334 27.38133333 27.38133333 71.776 27.38133333 99.1552 0L512.00213333 611.15733333l371.82933334 371.82613334c27.37813333 27.38133333 71.77386667 27.38133333 99.15306666 0 27.3792-27.37813333 27.3792-71.77386667 0-99.15413334L611.15733333 512.00213333 982.98453333 140.17066667c27.3792-27.38133333 27.3792-71.776 0-99.1552-27.3792-27.38133333-71.7696-27.38133333-99.15413333 0z m0 0" fill="#333333"/></svg>)";
const char* kConfirmSvg = R"(<svg viewBox="0 0 1024 1024" xmlns="http://www.w3.org/2000/svg"><path d="M398.5 741.1 190.9 533.5c-22.2-22.2-22.2-58.2 0-80.4s58.2-22.2 80.4 0l127.2 127.2 354.2-354.2c22.2-22.2 58.2-22.2 80.4 0s22.2 58.2 0 80.4L438.7 701c-11.1 11.1-25.6 16.6-40.2 16.6s-29.1-5.5-40-16.5z" fill="#333333"/></svg>)";
const char* kRecordSvg = R"(<svg viewBox="0 0 1024 1024" xmlns="http://www.w3.org/2000/svg"><path d="M507.6 508.2m-229.8 0a229.8 229.8 0 1 0 459.6 0 229.8 229.8 0 1 0-459.6 0Z" fill="#1c1c1c"/><path d="M507.6 952.9c-245.2 0-444.7-199.5-444.7-444.6S262.4 63.6 507.6 63.6s444.7 199.5 444.7 444.7-199.5 444.6-444.7 444.6z m0-837.2C291.2 115.7 115 291.8 115 508.3c0 216.5 176.1 392.6 392.7 392.6s392.7-176.1 392.7-392.6c-0.1-216.5-176.2-392.6-392.8-392.6z" fill="#1c1c1c"/></svg>)";
const char* kSelectArrowSvg = R"(<svg viewBox="0 0 1024 1024" xmlns="http://www.w3.org/2000/svg"><path d="M827.733333 411.733333L526.933333 712.533333c-8.533333 8.533333-21.333333 8.533333-29.866666 0L196.266667 411.733333c-17.066667-17.066667-17.066667-42.666667 0-59.733333 17.066667-17.066667 42.666667-17.066667 59.733333 0l256 256 256-256c17.066667-17.066667 42.666667-17.066667 59.733333 0s17.066667 42.666667 0 59.733333z"/></svg>)";

QString qString(const std::string& value) {
    return QString::fromStdString(value);
}

QString normalizedChoice(QString value) {
    value = value.trimmed().toLower();
    value.replace(QLatin1Char('_'), QLatin1Char('-'));
    value.replace(QLatin1Char('.'), QLatin1Char('-'));
    return value;
}

QString normalizedRecordFormat(QString value) {
    return qString(hyprcapture::normalizeRecordFormat(value.toStdString()));
}

bool isImageAnimationRecordFormat(const QString& format) {
    return hyprcapture::recordFormatIsImageAnimation(format.toStdString());
}

QString recordFormatFromTemplate(const std::string& filenameTemplate) {
    const QString suffix = QFileInfo(qString(filenameTemplate)).suffix().toLower();
    if (suffix == "webm" || suffix == "mkv" || suffix == "mp4" || suffix == "mov" || suffix == "gif" || suffix == "apng" || suffix == "webp")
        return suffix;
    return QStringLiteral("mp4");
}

QStringList animationDurationChoices() {
    return QStringList{"3s", "5s", "10s", "15s", "30s"};
}

int animationDurationSeconds(QString value) {
    value = value.trimmed().toLower();
    if (value.endsWith(QLatin1Char('s')))
        value.chop(1);
    bool ok = false;
    const int seconds = value.toInt(&ok);
    if (ok && (seconds == 3 || seconds == 5 || seconds == 10 || seconds == 15 || seconds == 30))
        return seconds;
    return 5;
}

QString animationDurationChoice(std::int64_t seconds) {
    return QStringLiteral("%1s").arg(animationDurationSeconds(QString::number(seconds)));
}

QString defaultRecordFormat(const hyprcapture::CaptureDefaults& defaults) {
    const QString configured = normalizedRecordFormat(qString(defaults.recordFormat));
    if (!configured.isEmpty())
        return configured;
    return recordFormatFromTemplate(defaults.recordFilenameTemplate);
}

bool solidAlphaBackground(hyprcapture::WindowBackground background) {
    return background == hyprcapture::WindowBackground::White || background == hyprcapture::WindowBackground::Black ||
        background == hyprcapture::WindowBackground::FollowSystem;
}

bool solidAlphaRecordingRequested(const hyprcapture::CaptureDefaults& defaults, hyprcapture::WindowBackground background) {
    return defaults.recordSolidAlpha && solidAlphaBackground(background);
}

bool alphaRecordingRequested(const hyprcapture::CaptureDefaults& defaults, hyprcapture::WindowBackground background) {
    return background == hyprcapture::WindowBackground::Transparent || solidAlphaRecordingRequested(defaults, background);
}

bool transparencyRequired(hyprcapture::WindowBackground background) {
    return background == hyprcapture::WindowBackground::Transparent;
}

QString defaultRecordFormatForBackground(const hyprcapture::CaptureDefaults& defaults, hyprcapture::WindowBackground background) {
    if (transparencyRequired(background))
        return normalizedRecordFormat(qString(defaults.recordTransparentFormat));
    return defaultRecordFormat(defaults);
}

QStringList recordFpsChoices(const hyprcapture::CaptureDefaults& defaults) {
    QStringList choices;
    const QStringList tokens = qString(defaults.recordFpsOptions).split(QRegularExpression(QStringLiteral("[,;\\s]+")), Qt::SkipEmptyParts);
    for (const QString& token : tokens) {
        bool ok = false;
        const int fps = token.toInt(&ok);
        if (!ok || fps < 1 || fps > 240)
            continue;
        const QString value = QString::number(fps);
        if (!choices.contains(value))
            choices.push_back(value);
    }

    const QString current = QString::number(std::clamp<std::int64_t>(defaults.recordFps, 1, 240));
    if (!choices.contains(current))
        choices.push_back(current);
    if (choices.isEmpty())
        choices = QStringList{"15", "24", "30", "60"};
    return choices;
}

QString codecChoiceFromConfig(const std::string& codec) {
    const QString value = normalizedChoice(qString(codec));
    if (value == "libx264" || value == "h264" || value == "h264-vaapi" || value == "h264-nvenc")
        return QStringLiteral("h264");
    if (value == "libx265" || value == "h265" || value == "hevc" || value == "hevc-vaapi" || value == "h265-vaapi" ||
        value == "hevc-nvenc" || value == "h265-nvenc")
        return QStringLiteral("h265");
    if (value == "libsvtav1" || value == "libaom-av1" || value == "librav1e" || value == "av1" || value == "av1-vaapi" ||
        value == "av1-nvenc")
        return QStringLiteral("av1");
    if (value == "libvpx-vp9" || value == "vp9" || value == "vp9-vaapi")
        return QStringLiteral("vp9");
    if (value == "ffv1")
        return QStringLiteral("ffv1");
    return QStringLiteral("auto");
}

QString defaultRecordCodecForBackground(const hyprcapture::CaptureDefaults& defaults, hyprcapture::WindowBackground background) {
    if (transparencyRequired(background))
        return codecChoiceFromConfig(defaults.recordTransparentCodec);
    return codecChoiceFromConfig(defaults.recordCodec);
}

QString codecConfigFromChoice(const QString& choice) {
    const QString value = normalizedChoice(choice);
    if (value == "h264")
        return QStringLiteral("h264");
    if (value == "h265")
        return QStringLiteral("h265");
    if (value == "av1")
        return QStringLiteral("av1");
    if (value == "vp9")
        return QStringLiteral("vp9");
    if (value == "ffv1")
        return QStringLiteral("ffv1");
    return QStringLiteral("auto");
}

QString firstWritableRenderDevice() {
    for (int minor = 128; minor <= 143; ++minor) {
        const QString path = QStringLiteral("/dev/dri/renderD%1").arg(minor);
        const QFileInfo info(path);
        if (info.exists() && info.isReadable() && info.isWritable())
            return path;
    }
    return {};
}

QByteArray alphaProbeFrame() {
    QByteArray frame;
    frame.resize(16 * 16 * 4);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const qsizetype i = static_cast<qsizetype>(y * 16 + x) * 4;
            frame[i] = static_cast<char>(x < 8 ? 255 : 0);
            frame[i + 1] = static_cast<char>(y < 8 ? 255 : 0);
            frame[i + 2] = static_cast<char>(128);
            frame[i + 3] = static_cast<char>((x + y) % 3 == 0 ? 0 : ((x + y) % 3 == 1 ? 128 : 255));
        }
    }
    return frame;
}

bool runProbeProcess(const QString& program, const QStringList& args, QByteArray* stdoutData = nullptr) {
    if (program.isEmpty())
        return false;
    QProcess process;
    process.setProgram(program);
    process.setArguments(args);
    process.setProcessEnvironment(hyprcapture::ui::trustedProcessEnvironment());
    process.start();
    if (!process.waitForStarted(1000))
        return false;
    if (!process.waitForFinished(3000)) {
        process.kill();
        process.waitForFinished(500);
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return false;
    if (stdoutData)
        *stdoutData = process.readAllStandardOutput();
    return true;
}

bool decodedFrameHasAlpha(const QString& path) {
    const QString ffmpeg = hyprcapture::ui::trustedSystemProgram(QStringLiteral("ffmpeg"));
    QByteArray decoded;
    if (!runProbeProcess(ffmpeg,
                         QStringList{QStringLiteral("-hide_banner"),
                                     QStringLiteral("-loglevel"),
                                     QStringLiteral("error"),
                                     QStringLiteral("-i"),
                                     path,
                                     QStringLiteral("-frames:v"),
                                     QStringLiteral("1"),
                                     QStringLiteral("-f"),
                                     QStringLiteral("rawvideo"),
                                     QStringLiteral("-pix_fmt"),
                                     QStringLiteral("rgba"),
                                     QStringLiteral("-")},
                         &decoded))
        return false;
    if (decoded.size() < 16 * 16 * 4)
        return false;
    for (qsizetype i = 3; i < decoded.size(); i += 4) {
        if (static_cast<unsigned char>(decoded[i]) < 250)
            return true;
    }
    return false;
}

bool webmHasAlphaMode(const QString& path) {
    const QString ffprobe = hyprcapture::ui::trustedSystemProgram(QStringLiteral("ffprobe"));
    QByteArray output;
    if (!runProbeProcess(ffprobe,
                         QStringList{QStringLiteral("-hide_banner"),
                                     QStringLiteral("-loglevel"),
                                     QStringLiteral("error"),
                                     QStringLiteral("-show_entries"),
                                     QStringLiteral("stream_tags=alpha_mode"),
                                     QStringLiteral("-of"),
                                     QStringLiteral("default=nw=1:nk=1"),
                                     path},
                         &output))
        return false;
    return QString::fromUtf8(output).trimmed() == QStringLiteral("1");
}

bool alphaProbeSucceeded(const QString& format, const QString& codecChoice) {
    const QString normalizedFormat = normalizedRecordFormat(format);
    const QString normalizedCodec = normalizedChoice(codecChoice);
    static std::map<QString, bool> cache;
    const QString key = normalizedFormat + QLatin1Char('|') + normalizedCodec;
    if (const auto it = cache.find(key); it != cache.end())
        return it->second;

    const QString ffmpeg = hyprcapture::ui::trustedSystemProgram(QStringLiteral("ffmpeg"));
    QTemporaryDir dir;
    if (ffmpeg.isEmpty() || !dir.isValid()) {
        cache[key] = false;
        return false;
    }

    const QString inputPath = dir.filePath(QStringLiteral("input.rgba"));
    QFile input(inputPath);
    if (!input.open(QIODevice::WriteOnly) || input.write(alphaProbeFrame()) != 16 * 16 * 4) {
        cache[key] = false;
        return false;
    }
    input.close();

    const QString outputPath = dir.filePath(QStringLiteral("output.") + normalizedFormat);
    QStringList args{QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-y")};
    if (normalizedCodec == "vp9-vaapi") {
        const QString renderDevice = firstWritableRenderDevice();
        if (renderDevice.isEmpty()) {
            cache[key] = false;
            return false;
        }
        args << QStringLiteral("-vaapi_device") << renderDevice;
    }
    args << QStringLiteral("-f") << QStringLiteral("rawvideo") << QStringLiteral("-pix_fmt") << QStringLiteral("rgba") << QStringLiteral("-video_size")
         << QStringLiteral("16x16") << QStringLiteral("-framerate") << QStringLiteral("1") << QStringLiteral("-i") << inputPath << QStringLiteral("-frames:v")
         << QStringLiteral("1") << QStringLiteral("-an");

    if (normalizedCodec == "vp9-vaapi") {
        args << QStringLiteral("-vf") << QStringLiteral("format=rgba,hwupload,scale_vaapi=format=nv12") << QStringLiteral("-c:v")
             << QStringLiteral("vp9_vaapi") << QStringLiteral("-qp") << QStringLiteral("23") << QStringLiteral("-quality") << QStringLiteral("7");
    } else if (normalizedCodec == "vp9") {
        args << QStringLiteral("-c:v") << QStringLiteral("libvpx-vp9") << QStringLiteral("-pix_fmt") << QStringLiteral("yuva420p")
             << QStringLiteral("-deadline") << QStringLiteral("realtime") << QStringLiteral("-cpu-used") << QStringLiteral("8") << QStringLiteral("-b:v")
             << QStringLiteral("0") << QStringLiteral("-crf") << QStringLiteral("32");
    } else if (normalizedCodec == "ffv1") {
        args << QStringLiteral("-c:v") << QStringLiteral("ffv1") << QStringLiteral("-level") << QStringLiteral("3") << QStringLiteral("-pix_fmt")
             << QStringLiteral("rgba");
    } else {
        cache[key] = false;
        return false;
    }
    args << outputPath;

    const bool encoded = runProbeProcess(ffmpeg, args);
    const bool preservesAlpha = encoded && (normalizedFormat == "webm" ? webmHasAlphaMode(outputPath) : decodedFrameHasAlpha(outputPath));
    cache[key] = preservesAlpha;
    return preservesAlpha;
}

TransparentAutoChoice transparentAutoChoiceForFormat(QString format) {
    format = normalizedRecordFormat(format);
    if (format == "webm") {
        for (const QString& codec : QStringList{QStringLiteral("vp9-vaapi")}) {
            if (alphaProbeSucceeded(format, codec))
                return {.format = format, .codec = codec};
        }
        const QString fallback = alphaProbeSucceeded(format, QStringLiteral("vp9")) ? QStringLiteral("vp9") : QStringLiteral("auto");
        return {.format = format, .codec = fallback, .warning = QStringLiteral("no hardware alpha encoder detected; using CPU vp9")};
    }
    if (format == "mkv") {
        const QString fallback = alphaProbeSucceeded(format, QStringLiteral("ffv1")) ? QStringLiteral("ffv1") : QStringLiteral("auto");
        return {.format = format, .codec = fallback, .warning = QStringLiteral("no hardware alpha encoder detected; using CPU ffv1")};
    }
    return {.format = format, .codec = QStringLiteral("auto")};
}

QString recordTemplateWithFormat(const std::string& filenameTemplate, const QString& format) {
    QString value = qString(filenameTemplate);
    if (value.trimmed().isEmpty())
        value = QStringLiteral("Recording-%Y-%m-%d-%H%M%S");
    QFileInfo info(value);
    const QString suffix = info.suffix();
    if (!suffix.isEmpty())
        value.chop(suffix.size() + 1);
    return value + QLatin1Char('.') + normalizedRecordFormat(format);
}

bool timingEnabled() {
    return qEnvironmentVariableIsSet("HYPRCAPTURE_TIMING") || qEnvironmentVariableIsSet("HYPRCAPTURE_TIMING_FILE");
}

void traceTiming(const QString& event, qint64 elapsedMs = -1) {
    if (!timingEnabled())
        return;

    QString line = QStringLiteral("%1 pid=%2 %3")
                       .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs))
                       .arg(QCoreApplication::applicationPid())
                       .arg(event);
    if (elapsedMs >= 0)
        line += QStringLiteral(" elapsed_ms=%1").arg(elapsedMs);
    line += QLatin1Char('\n');

    const QString path = qEnvironmentVariable("HYPRCAPTURE_TIMING_FILE");
    if (!path.isEmpty()) {
        QFile file(path);
        if (hyprcapture::ui::isPrivateRuntimePath(path) && file.open(QIODevice::WriteOnly | QIODevice::Append))
            file.write(line.toUtf8());
        return;
    }

    fputs(line.toLocal8Bit().constData(), stderr);
}

bool savePng(const QImage& image, const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return false;
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        file.remove();
        return false;
    }

    QImageWriter writer(&file, "PNG");
    writer.setQuality(75);
    if (!writer.write(image)) {
        file.remove();
        return false;
    }
    return true;
}

bool writePrivateTextFile(const QString& path, const QByteArray& bytes) {
    if (path.isEmpty() || bytes.isEmpty() || !hyprcapture::ui::isPrivateRuntimePath(path))
        return false;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return false;
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        file.remove();
        return false;
    }
    if (file.write(bytes) != bytes.size()) {
        file.remove();
        return false;
    }
    return true;
}

QString compactProcessErrorText(const QByteArray& stderrData, const QByteArray& stdoutData, const QString& fallback) {
    QString error = QString::fromUtf8(stderrData).trimmed();
    if (error.isEmpty())
        error = QString::fromUtf8(stdoutData).trimmed();
    if (error.startsWith(QStringLiteral("[hyprcapture] ")))
        error = error.mid(QStringLiteral("[hyprcapture] ").size()).trimmed();
    if (error.isEmpty())
        error = fallback;
    return error.left(160);
}

bool hyprctlOutputHasError(const QByteArray& stderrData, const QByteArray& stdoutData) {
    const QString stderrText = QString::fromUtf8(stderrData).trimmed();
    const QString stdoutText = QString::fromUtf8(stdoutData).trimmed();
    return stderrText.startsWith(QStringLiteral("error:"), Qt::CaseInsensitive) ||
        stdoutText.startsWith(QStringLiteral("error:"), Qt::CaseInsensitive);
}

DispatchCommandResult evalHyprcaptureExpression(const QString& expression) {
    const QString hyprctl = hyprcapture::ui::trustedSystemProgram(QStringLiteral("hyprctl"));
    if (hyprctl.isEmpty() || expression.isEmpty())
        return {.success = false, .error = QStringLiteral("hyprctl unavailable")};

    QProcess process;
    process.setProgram(hyprctl);
    process.setArguments(QStringList{QStringLiteral("eval"), expression});
    process.setProcessEnvironment(hyprcapture::ui::trustedProcessEnvironment());
    process.start();
    if (!process.waitForStarted(1000))
        return {.success = false, .error = QStringLiteral("hyprctl start failed")};
    if (!process.waitForFinished(5000)) {
        process.kill();
        process.waitForFinished(500);
        return {.success = false, .error = QStringLiteral("hyprctl timeout")};
    }
    const QByteArray stderrData = process.readAllStandardError();
    const QByteArray stdoutData = process.readAllStandardOutput();
    if (process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0 && !hyprctlOutputHasError(stderrData, stdoutData))
        return {.success = true};
    return {.success = false, .error = compactProcessErrorText(stderrData, stdoutData, QStringLiteral("eval failed"))};
}

DispatchCommandResult runHymissionCaptureInputCommand(const QString& action, const QString& token) {
    const QString hyprctl = hyprcapture::ui::trustedSystemProgram(QStringLiteral("hyprctl"));
    if (hyprctl.isEmpty() || action.isEmpty() || token.isEmpty())
        return {.success = false, .error = QStringLiteral("hyprctl unavailable")};

    QProcess process;
    process.setProgram(hyprctl);
    process.setArguments(QStringList{QStringLiteral("hymission-capture-input"), action, token});
    process.setProcessEnvironment(hyprcapture::ui::trustedProcessEnvironment());
    process.start();
    if (!process.waitForStarted(1000))
        return {.success = false, .error = QStringLiteral("hyprctl start failed")};
    if (!process.waitForFinished(1000)) {
        process.kill();
        process.waitForFinished(500);
        return {.success = false, .error = QStringLiteral("hyprctl timeout")};
    }

    const QByteArray stderrData = process.readAllStandardError();
    const QByteArray stdoutData = process.readAllStandardOutput();
    const QString    stdoutText = QString::fromUtf8(stdoutData).trimmed();
    if (process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0 && stdoutText.startsWith(QStringLiteral("ok")) &&
        !hyprctlOutputHasError(stderrData, stdoutData))
        return {.success = true};

    return {.success = false, .error = compactProcessErrorText(stderrData, stdoutData, QStringLiteral("hymission capture input failed"))};
}

QString luaStringLiteral(const QString& value) {
    const QJsonArray array{value};
    const QString json = QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
    return json.size() >= 2 ? json.mid(1, json.size() - 2) : QStringLiteral("\"\"");
}

DispatchCommandResult evalHyprcaptureLuaFunction(const QString& function, const QString& argument = {}) {
    if (function.isEmpty())
        return {.success = false, .error = QStringLiteral("lua function missing")};

    QString expression = QStringLiteral("hl.plugin.hyprcapture.%1(").arg(function);
    if (!argument.isEmpty())
        expression += luaStringLiteral(argument);
    expression += QStringLiteral(")");
    return evalHyprcaptureExpression(expression);
}

DispatchCommandResult dispatchRecordingStart(const QString& requestPath) {
    if (requestPath.isEmpty())
        return {.success = false, .error = QStringLiteral("record request missing")};

    return evalHyprcaptureLuaFunction(QStringLiteral("record_start"), requestPath);
}

DispatchCommandResult dispatchWindowCapture(const QString& requestPath) {
    if (requestPath.isEmpty())
        return {.success = false, .error = QStringLiteral("window capture request missing")};

    return evalHyprcaptureLuaFunction(QStringLiteral("window_capture"), requestPath);
}

DispatchCommandResult dispatchRecordingStop() {
    return evalHyprcaptureLuaFunction(QStringLiteral("record_stop"));
}

QString uniqueOutputPath(const QDir& dir, const QString& rawFilename) {
    QFileInfo info(QFileInfo(rawFilename).fileName());
    QString   base = info.completeBaseName();
    QString   suffix = info.suffix();
    if (base.isEmpty())
        base = QStringLiteral("Screenshot");
    if (suffix.isEmpty())
        suffix = QStringLiteral("png");

    for (int i = 0; i < 1000; ++i) {
        const QString filename = i == 0 ? QStringLiteral("%1.%2").arg(base, suffix) : QStringLiteral("%1-%2.%3").arg(base).arg(i).arg(suffix);
        const QString path = dir.filePath(filename);
        if (!QFileInfo::exists(path))
            return path;
    }

    return dir.filePath(QStringLiteral("%1-%2.%3").arg(base).arg(QDateTime::currentMSecsSinceEpoch()).arg(suffix));
}

void cleanupArtifactFiles(const QStringList& paths) {
    QStringList dirs;
    for (const QString& path : paths) {
        if (!hyprcapture::ui::isPrivateRuntimePath(path))
            continue;
        QFile::remove(path);
        const QString dir = QFileInfo(path).absolutePath();
        if (!dirs.contains(dir))
            dirs.push_back(dir);
    }

    for (const QString& dir : dirs) {
        if (!hyprcapture::ui::isPrivateRuntimePath(dir))
            continue;
        QDir parent(QFileInfo(dir).absolutePath());
        parent.rmdir(QFileInfo(dir).fileName());
    }
}

CaptureOutputResult writeCaptureOutput(const QImage& image,
                                        const hyprcapture::CaptureDefaults& defaults,
                                        const hyprcapture::ui::ClipboardSnapshotData& clipboardSnapshot,
                                        const QString& outputPath,
                                        const QString& restoreClipboardPath) {
    CaptureOutputResult result;
    result.showThumbnail = defaults.showThumbnail;

    if (defaults.clipboard && defaults.showThumbnail && !restoreClipboardPath.isEmpty()) {
        QElapsedTimer timer;
        timer.start();
        if (hyprcapture::ui::saveClipboardSnapshotDataToPath(clipboardSnapshot, restoreClipboardPath))
            result.restoreClipboardPath = restoreClipboardPath;
        traceTiming(QStringLiteral("clipboard_snapshot_save"), timer.elapsed());
    }

    if (!outputPath.isEmpty()) {
        QElapsedTimer timer;
        timer.start();
        if (savePng(image, outputPath))
            result.savedPath = outputPath;
        traceTiming(defaults.save ? QStringLiteral("image_save") : QStringLiteral("thumbnail_full_temp_save"), timer.elapsed());
    }

    if (result.showThumbnail && result.savedPath.isEmpty()) {
        QElapsedTimer timer;
        timer.start();
        const QString path = hyprcapture::ui::runtimeFile("thumbnail", ".png");
        if (hyprcapture::ui::savePrivatePng(image, path))
            result.savedPath = path;
        traceTiming(QStringLiteral("thumbnail_temp_save"), timer.elapsed());
    }

    result.clipboardRequested = defaults.clipboard;
    if (defaults.clipboard) {
        QElapsedTimer timer;
        timer.start();
        if (result.savedPath.isEmpty() || !hyprcapture::ui::copyImageFileToClipboardDetached(result.savedPath))
            result.clipboardCopied = hyprcapture::ui::copyImageToClipboardDetached(image);
        else
            result.clipboardCopied = true;
        traceTiming(QStringLiteral("clipboard_copy_start"), timer.elapsed());
    }

    return result;
}

QString plannedCaptureOutputPath(const hyprcapture::CaptureDefaults& defaults, const QString& windowClass, const QString& windowTitle) {
    if (!defaults.save)
        return {};

    const auto dirPath = hyprcapture::expandUserPath(defaults.saveDir);
    QDir       dir(QString::fromStdString(dirPath.string()));
    if (!dir.exists())
        dir.mkpath(".");
    return uniqueOutputPath(
        dir,
        QString::fromStdString(hyprcapture::makeTimestampedFilename(defaults.filenameTemplate, windowClass.toStdString(), windowTitle.toStdString())));
}

QString thumbnailTargetPath(const hyprcapture::CaptureDefaults& defaults, const QString& plannedOutputPath) {
    if (defaults.save)
        return plannedOutputPath;
    if (!defaults.showThumbnail)
        return {};
    return hyprcapture::ui::runtimeFile("thumbnail-full", ".png");
}

QString thumbnailDeleteRoot(const hyprcapture::CaptureDefaults& defaults) {
    if (!defaults.save)
        return {};
    return QString::fromStdString(hyprcapture::expandUserPath(defaults.saveDir).string());
}

QString saveThumbnailPreview(const QImage& image) {
    if (image.isNull())
        return {};

    QElapsedTimer timer;
    timer.start();
    const QSize maxSize(kThumbnailPreviewMaxWidth, kThumbnailPreviewMaxHeight);
    QImage preview = image;
    if (preview.width() > maxSize.width() || preview.height() > maxSize.height())
        preview = preview.scaled(maxSize, Qt::KeepAspectRatio, Qt::FastTransformation);

    const QString path = hyprcapture::ui::runtimeFile("thumbnail-preview", ".png");
    if (!hyprcapture::ui::savePrivatePng(preview, path))
        return {};
    traceTiming(QStringLiteral("thumbnail_preview_save"), timer.elapsed());
    return path;
}

double maxScreenDevicePixelRatio() {
    double dpr = 1.0;
    for (const auto* screen : QGuiApplication::screens())
        dpr = std::max(dpr, screen ? screen->devicePixelRatio() : 1.0);
    return dpr;
}

QIcon iconFromSvg(const char* svg, int logicalSize = kModeIconSize, double rotationDegrees = 0.0) {
    QSvgRenderer renderer{QByteArray(svg)};
    const double dpr = maxScreenDevicePixelRatio();
    QPixmap pixmap(QSize(std::max(1, static_cast<int>(std::ceil(logicalSize * dpr))),
                         std::max(1, static_cast<int>(std::ceil(logicalSize * dpr)))));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.translate(logicalSize / 2.0, logicalSize / 2.0);
    if (rotationDegrees != 0.0)
        painter.rotate(rotationDegrees);
    renderer.render(&painter, QRectF(-logicalSize / 2.0, -logicalSize / 2.0, logicalSize, logicalSize));
    return QIcon(pixmap);
}

QColor followSystemColor() {
    const auto scheme = QGuiApplication::styleHints()->colorScheme();
    if (scheme == Qt::ColorScheme::Light)
        return QColor(245, 245, 245);
    if (scheme == Qt::ColorScheme::Dark)
        return QColor(17, 19, 23);

    const QColor window = QApplication::palette().color(QPalette::Window);
    const double luminance = 0.2126 * window.redF() + 0.7152 * window.greenF() + 0.0722 * window.blueF();
    return luminance >= 0.5 ? QColor(245, 245, 245) : QColor(17, 19, 23);
}

QString cssRgba(QColor color, int alpha = -1) {
    if (alpha >= 0)
        color.setAlpha(alpha);
    return QStringLiteral("rgba(%1,%2,%3,%4)").arg(color.red()).arg(color.green()).arg(color.blue()).arg(color.alpha());
}

QColor mixedColor(QColor a, QColor b, double amount) {
    const auto mix = [&](int av, int bv) { return static_cast<int>(std::round(av * (1.0 - amount) + bv * amount)); };
    return QColor(mix(a.red(), b.red()), mix(a.green(), b.green()), mix(a.blue(), b.blue()), mix(a.alpha(), b.alpha()));
}

QString toolbarStyleSheet(const QPalette& palette) {
    const QColor window = palette.color(QPalette::Window);
    const QColor button = palette.color(QPalette::Button);
    const QColor text = palette.color(QPalette::WindowText);
    const QColor highlight = palette.color(QPalette::Highlight);
    const QColor highlightedText = palette.color(QPalette::HighlightedText);
    const QColor border = mixedColor(text, window, 0.55);
    const QColor hover = mixedColor(button, highlight, 0.16);
    const QColor checked = mixedColor(button, highlight, 0.32);
    const QColor modeChecked = mixedColor(text, window, 0.72);
    const QColor recordArmed = mixedColor(text, window, 0.68);
    const QColor recordChecked(212, 48, 62);
    const QColor recordPressed(176, 35, 48);

    return QStringLiteral(
               "#toolbar { background: %1; border: 1px solid %2; border-radius: 8px; }"
               "QPushButton { color: %3; background: transparent; padding: 6px 10px; border: none; border-radius: 5px; outline: none; }"
               "QPushButton:hover { background: %4; }"
               "QPushButton:checked { color: %3; background: %5; }"
               "QPushButton:pressed { color: %6; background: %7; }"
               "QPushButton#captureModeButton { padding: 4px 6px; background: transparent; border: none; outline: none; }"
               "QPushButton#captureModeButton:hover { background: transparent; }"
               "QPushButton#captureModeButton:checked { background: %8; border-radius: 7px; }"
               "QPushButton#captureModeButton:pressed { background: %8; border-radius: 7px; }"
               "QPushButton#recordToggleButton { padding: 4px 6px; background: transparent; border: none; outline: none; }"
               "QPushButton#recordToggleButton:hover { background: transparent; }"
               "QPushButton#recordToggleButton:checked { background: %9; border-radius: 7px; }"
               "QPushButton#recordToggleButton:pressed { background: %9; border-radius: 7px; }"
               "QPushButton#recordActiveButton { padding: 4px 6px; background: transparent; border: none; outline: none; }"
               "QPushButton#recordActiveButton:hover { background: transparent; }"
               "QPushButton#recordActiveButton:checked { background: %10; border-radius: 7px; }"
               "QPushButton#recordActiveButton:pressed { background: %11; border-radius: 7px; }"
               "QLabel { color: %3; }")
        .arg(cssRgba(window, 238),
             cssRgba(border, 150),
             cssRgba(text),
             cssRgba(hover, 180),
             cssRgba(checked, 220),
             cssRgba(highlightedText),
             cssRgba(highlight),
             cssRgba(modeChecked),
             cssRgba(recordArmed, 190),
             cssRgba(recordChecked, 220),
             cssRgba(recordPressed, 230));
}

QString popupStyleSheet(const QPalette& palette) {
    const QColor window = palette.color(QPalette::Window);
    const QColor button = palette.color(QPalette::Button);
    const QColor text = palette.color(QPalette::WindowText);
    const QColor highlight = palette.color(QPalette::Highlight);
    const QColor highlightedText = palette.color(QPalette::HighlightedText);
    const QColor border = mixedColor(text, window, 0.55);
    const QColor hover = mixedColor(button, highlight, 0.16);

    return QStringLiteral(
               "#inlineSelectPopup { background: %1; border: 1px solid %2; border-radius: 7px; }"
               "#inlineSelectPopup QPushButton { color: %3; background: transparent; padding: 7px 12px; border: none; border-radius: 5px; text-align: left; }"
               "#inlineSelectPopup QPushButton:hover { background: %4; }"
               "#inlineSelectPopup QPushButton:checked { color: %5; background: %6; }")
        .arg(cssRgba(window, 246), cssRgba(border, 150), cssRgba(text), cssRgba(hover, 210), cssRgba(highlightedText), cssRgba(highlight));
}

bool imageSizeWithinBounds(const QSize& size) {
    if (size.isEmpty() || size.width() <= 0 || size.height() <= 0 || size.width() > kMaxArtifactDimension || size.height() > kMaxArtifactDimension)
        return false;

    const auto width = static_cast<qint64>(size.width());
    const auto height = static_cast<qint64>(size.height());
    if (width > std::numeric_limits<qint64>::max() / height)
        return false;
    const auto pixels = width * height;
    if (pixels > std::numeric_limits<qint64>::max() / 4)
        return false;
    return pixels * 4 <= kMaxArtifactBytes;
}

QImage boundedImage(const QSize& size, QImage::Format format) {
    if (!imageSizeWithinBounds(size))
        return {};
    return QImage(size, format);
}

QSize boundedScaledSize(int width, int height, double scaleX, double scaleY) {
    if (width <= 0 || height <= 0 || !std::isfinite(scaleX) || !std::isfinite(scaleY) || scaleX <= 0.0 || scaleY <= 0.0)
        return {};

    const double scaledWidth = std::ceil(width * scaleX);
    const double scaledHeight = std::ceil(height * scaleY);
    if (!std::isfinite(scaledWidth) || !std::isfinite(scaledHeight) || scaledWidth < 1.0 || scaledHeight < 1.0 ||
        scaledWidth > kMaxArtifactDimension || scaledHeight > kMaxArtifactDimension)
        return {};

    const QSize size(static_cast<int>(scaledWidth), static_cast<int>(scaledHeight));
    return imageSizeWithinBounds(size) ? size : QSize{};
}

bool boundedDoubleToInt(double value, int minimum, int maximum, bool ceilValue, int& out) {
    if (!std::isfinite(value))
        return false;
    const double rounded = ceilValue ? std::ceil(value) : std::floor(value);
    if (rounded < minimum || rounded > maximum)
        return false;
    out = static_cast<int>(rounded);
    return true;
}

QRect protocolRect(const hyprcapture::Rect& rect) {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    if (!boundedDoubleToInt(rect.x, -kMaxLogicalCoordinate, kMaxLogicalCoordinate, false, x) ||
        !boundedDoubleToInt(rect.y, -kMaxLogicalCoordinate, kMaxLogicalCoordinate, false, y) ||
        !boundedDoubleToInt(rect.width, 1, kMaxArtifactDimension, true, width) ||
        !boundedDoubleToInt(rect.height, 1, kMaxArtifactDimension, true, height))
        return {};

    return QRect(QPoint(x, y), QSize(width, height));
}

bool protocolPoint(const hyprcapture::Point& input, QPoint& point) {
    int x = 0;
    int y = 0;
    if (!boundedDoubleToInt(input.x, -kMaxLogicalCoordinate, kMaxLogicalCoordinate, false, x) ||
        !boundedDoubleToInt(input.y, -kMaxLogicalCoordinate, kMaxLogicalCoordinate, false, y))
        return false;

    point = QPoint(x, y);
    return true;
}

QImage loadRawRgba(const QString& path, int width, int height, bool topDown, qint64& remainingSessionBytes) {
    QFile file(path);
    if (path.isEmpty() || width <= 0 || height <= 0 || width > kMaxArtifactDimension || height > kMaxArtifactDimension)
        return {};

    const qint64 expected = static_cast<qint64>(width) * static_cast<qint64>(height) * 4;
    if (expected <= 0 || expected > kMaxArtifactBytes || expected > remainingSessionBytes || !hyprcapture::ui::isPrivateRuntimeFile(path, expected) ||
        !file.open(QIODevice::ReadOnly))
        return {};

    const QByteArray bytes = file.readAll();
    if (bytes.size() != expected)
        return {};

    QImage image(reinterpret_cast<const uchar*>(bytes.constData()), width, height, width * 4, QImage::Format_RGBA8888);
    QImage copy = image.copy();
    remainingSessionBytes -= expected;
    return topDown ? copy : copy.flipped(Qt::Vertical);
}

int inverseRotationDegreesForMonitorTransform(int transform) {
    switch (transform) {
        case 1:
        case 5: return 90;
        case 2:
        case 6: return 180;
        case 3:
        case 7: return -90;
        default: return 0;
    }
}

QImage rotateMonitorArtifactToLogicalOrientation(const QImage& image, int transform) {
    if (transform == 0 || image.isNull())
        return image;

    QTransform imageTransform;
    imageTransform.rotate(inverseRotationDegreesForMonitorTransform(transform));
    return image.transformed(imageTransform, Qt::FastTransformation).convertToFormat(QImage::Format_RGBA8888);
}

QImage normalizeMonitorArtifactImage(const QImage& image, int transform, const QRect& logicalGeometry) {
    Q_UNUSED(logicalGeometry)
    if (image.isNull() || transform == 0)
        return image;

    return rotateMonitorArtifactToLogicalOrientation(image, transform);
}

QRect projectedImageRect(const QRect& logicalRect, const QRect& fullGeometry, const QSize& imageSize) {
    if (!logicalRect.isValid() || !fullGeometry.isValid() || imageSize.isEmpty())
        return {};

    const double scaleX = static_cast<double>(imageSize.width()) / std::max(1, fullGeometry.width());
    const double scaleY = static_cast<double>(imageSize.height()) / std::max(1, fullGeometry.height());
    QRect rect(QPoint(static_cast<int>(std::floor((logicalRect.x() - fullGeometry.x()) * scaleX)),
                      static_cast<int>(std::floor((logicalRect.y() - fullGeometry.y()) * scaleY))),
               QSize(std::max(1, static_cast<int>(std::ceil(logicalRect.width() * scaleX))),
                     std::max(1, static_cast<int>(std::ceil(logicalRect.height() * scaleY)))));

    // Some Hyprland fake-render paths draw the window below its nominal crop and
    // the plugin shifts the readback up. Keep the expected visible size instead
    // of losing the bottom rows when that makes the projected top negative.
    if (rect.x() < 0)
        rect.moveLeft(0);
    if (rect.y() < 0)
        rect.moveTop(0);
    return rect.intersected(QRect(QPoint(0, 0), imageSize));
}

QRect artifactRectToLogicalRect(const QRect& artifactRect, const QSize& artifactSize, const QRect& fullGeometry) {
    if (!artifactRect.isValid() || artifactSize.isEmpty() || !fullGeometry.isValid())
        return {};

    const double scaleX = static_cast<double>(fullGeometry.width()) / std::max(1, artifactSize.width());
    const double scaleY = static_cast<double>(fullGeometry.height()) / std::max(1, artifactSize.height());
    const int x1 = fullGeometry.x() + static_cast<int>(std::floor(artifactRect.x() * scaleX));
    const int y1 = fullGeometry.y() + static_cast<int>(std::floor(artifactRect.y() * scaleY));
    const int x2 = fullGeometry.x() + static_cast<int>(std::ceil((artifactRect.x() + artifactRect.width()) * scaleX));
    const int y2 = fullGeometry.y() + static_cast<int>(std::ceil((artifactRect.y() + artifactRect.height()) * scaleY));
    return QRect(QPoint(x1, y1), QSize(std::max(1, x2 - x1), std::max(1, y2 - y1))).intersected(fullGeometry);
}

QRect logicalRectToImageRect(const QRect& logicalRect, const QRect& logicalGeometry, const QSize& imageSize) {
    const QRect clipped = logicalRect.intersected(logicalGeometry);
    if (!clipped.isValid() || !logicalGeometry.isValid() || imageSize.isEmpty())
        return {};

    const double scaleX = static_cast<double>(imageSize.width()) / std::max(1, logicalGeometry.width());
    const double scaleY = static_cast<double>(imageSize.height()) / std::max(1, logicalGeometry.height());
    const int x1 = static_cast<int>(std::floor((clipped.x() - logicalGeometry.x()) * scaleX));
    const int y1 = static_cast<int>(std::floor((clipped.y() - logicalGeometry.y()) * scaleY));
    const int x2 = static_cast<int>(std::ceil((clipped.x() + clipped.width() - logicalGeometry.x()) * scaleX));
    const int y2 = static_cast<int>(std::ceil((clipped.y() + clipped.height() - logicalGeometry.y()) * scaleY));
    return QRect(QPoint(x1, y1), QSize(std::max(1, x2 - x1), std::max(1, y2 - y1))).intersected(QRect(QPoint(0, 0), imageSize));
}

QRect logicalRectToOutputRect(const QRect& logicalRect, const QRect& outputLogicalGeometry, double scaleX, double scaleY) {
    if (!logicalRect.isValid() || !outputLogicalGeometry.isValid() || scaleX <= 0.0 || scaleY <= 0.0)
        return {};

    const int x1 = static_cast<int>(std::floor((logicalRect.x() - outputLogicalGeometry.x()) * scaleX));
    const int y1 = static_cast<int>(std::floor((logicalRect.y() - outputLogicalGeometry.y()) * scaleY));
    const int x2 = static_cast<int>(std::ceil((logicalRect.x() + logicalRect.width() - outputLogicalGeometry.x()) * scaleX));
    const int y2 = static_cast<int>(std::ceil((logicalRect.y() + logicalRect.height() - outputLogicalGeometry.y()) * scaleY));
    return QRect(QPoint(x1, y1), QSize(std::max(1, x2 - x1), std::max(1, y2 - y1)));
}

QPointF superellipsePoint(const QPointF& center, double radius, double power, double signX, double signY, double theta) {
    const double exponent = 2.0 / std::clamp(power, 1.0, 10.0);
    const double x = std::pow(std::max(0.0, std::cos(theta)), exponent) * radius;
    const double y = std::pow(std::max(0.0, std::sin(theta)), exponent) * radius;
    return {center.x() + signX * x, center.y() + signY * y};
}

void appendSuperellipseCorner(QPainterPath& path,
                              const QPointF& center,
                              double radius,
                              double power,
                              double signX,
                              double signY,
                              double startTheta,
                              double endTheta) {
    const int steps = std::clamp(static_cast<int>(std::ceil(radius / 2.0)), 8, 32);
    for (int i = 1; i <= steps; ++i) {
        const double t = static_cast<double>(i) / steps;
        path.lineTo(superellipsePoint(center, radius, power, signX, signY, startTheta + (endTheta - startTheta) * t));
    }
}

QPainterPath roundedWindowFramePath(const QRectF& rawRect, double radius, double power) {
    const QRectF rect = rawRect.normalized();
    QPainterPath path;
    if (!rect.isValid())
        return path;

    radius = std::clamp(radius, 0.0, std::min(rect.width(), rect.height()) / 2.0);
    if (radius <= 0.0) {
        path.addRect(rect);
        return path;
    }

    constexpr double halfPi = 1.57079632679489661923;
    const double left = rect.left();
    const double top = rect.top();
    const double right = rect.right();
    const double bottom = rect.bottom();

    path.moveTo(left + radius, top);
    path.lineTo(right - radius, top);
    appendSuperellipseCorner(path, {right - radius, top + radius}, radius, power, 1.0, -1.0, halfPi, 0.0);
    path.lineTo(right, bottom - radius);
    appendSuperellipseCorner(path, {right - radius, bottom - radius}, radius, power, 1.0, 1.0, 0.0, halfPi);
    path.lineTo(left + radius, bottom);
    appendSuperellipseCorner(path, {left + radius, bottom - radius}, radius, power, -1.0, 1.0, halfPi, 0.0);
    path.lineTo(left, top + radius);
    appendSuperellipseCorner(path, {left + radius, top + radius}, radius, power, -1.0, -1.0, 0.0, halfPi);
    path.closeSubpath();
    return path;
}

bool paintWindowBackground(QImage& background,
                           hyprcapture::WindowBackground bg,
                           const QImage& desktopImage,
                           const QRect& desktopSource) {
    if (background.isNull() || bg == hyprcapture::WindowBackground::Transparent)
        return false;

    QPainter backgroundPainter(&background);
    if (bg == hyprcapture::WindowBackground::Real) {
        if (desktopImage.isNull() || !desktopSource.isValid())
            return false;
        backgroundPainter.drawImage(background.rect(), desktopImage, desktopSource);
        return true;
    }

    if (bg == hyprcapture::WindowBackground::White)
        backgroundPainter.fillRect(background.rect(), Qt::white);
    else if (bg == hyprcapture::WindowBackground::Black)
        backgroundPainter.fillRect(background.rect(), Qt::black);
    else if (bg == hyprcapture::WindowBackground::FollowSystem)
        backgroundPainter.fillRect(background.rect(), followSystemColor());
    else
        backgroundPainter.fillRect(background.rect(), QColor(30, 34, 38));
    return true;
}

void reconstructRealWindowBackground(QImage& background, const QImage& artifact, const QRect& artifactSource) {
    if (background.format() != QImage::Format_RGBA8888 || artifact.format() != QImage::Format_RGBA8888 || background.isNull() || artifact.isNull())
        return;

    // The desktop snapshot already contains the selected window. Invert the
    // source-over blend so the window artifact is not composited twice.
    for (int y = 0; y < background.height(); ++y) {
        auto* dst = background.scanLine(y);
        const int sy = artifactSource.y() + y;
        if (sy < 0 || sy >= artifact.height())
            continue;

        const auto* src = artifact.constScanLine(sy);
        for (int x = 0; x < background.width(); ++x) {
            const int sx = artifactSource.x() + x;
            if (sx < 0 || sx >= artifact.width())
                continue;

            auto* dstPx = dst + static_cast<qsizetype>(x) * 4;
            const auto* srcPx = src + static_cast<qsizetype>(sx) * 4;
            const int alpha = srcPx[3];
            if (alpha <= 0 || alpha >= 255)
                continue;

            const int inverseAlpha = 255 - alpha;
            for (int channel = 0; channel < 3; ++channel) {
                const int value = (dstPx[channel] * 255 - srcPx[channel] * alpha + inverseAlpha / 2) / inverseAlpha;
                dstPx[channel] = static_cast<uchar>(std::clamp(value, 0, 255));
            }
            dstPx[3] = 255;
        }
    }
}

void clipWindowBackgroundToFrame(QImage& background,
                                 const QRect& capturedLogicalGeometry,
                                 const QRect& visibleLogicalGeometry,
                                 double rounding,
                                 double roundingPower) {
    if (background.isNull() || !capturedLogicalGeometry.isValid() || !visibleLogicalGeometry.isValid())
        return;

    const QRect frame = logicalRectToImageRect(visibleLogicalGeometry, capturedLogicalGeometry, background.size());
    if (!frame.isValid()) {
        background.fill(Qt::transparent);
        return;
    }

    const double scaleX = static_cast<double>(background.width()) / std::max(1, capturedLogicalGeometry.width());
    const double scaleY = static_cast<double>(background.height()) / std::max(1, capturedLogicalGeometry.height());
    const double radius = std::max(0.0, rounding) * std::min(scaleX, scaleY);

    QImage mask = boundedImage(background.size(), QImage::Format_ARGB32_Premultiplied);
    if (mask.isNull()) {
        background.fill(Qt::transparent);
        return;
    }
    mask.fill(Qt::transparent);
    {
        QPainter maskPainter(&mask);
        maskPainter.setRenderHint(QPainter::Antialiasing, true);
        maskPainter.fillPath(roundedWindowFramePath(QRectF(frame), radius, roundingPower), Qt::white);
    }

    QPainter backgroundPainter(&background);
    backgroundPainter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    backgroundPainter.drawImage(QPoint(0, 0), mask);
}

int transparentPixelsInRow(const QImage& image, const QRect& span, int y) {
    if (image.format() != QImage::Format_RGBA8888 || y < 0 || y >= image.height())
        return 0;

    int transparent = 0;
    const auto* row = image.constScanLine(y);
    for (int x = span.left(); x <= span.right(); ++x) {
        const auto* px = row + static_cast<qsizetype>(x) * 4;
        if (px[3] == 0)
            ++transparent;
    }
    return transparent;
}

void copyPatchPixel(QImage& target, const QImage& patch, int x, int y) {
    auto* dst = target.scanLine(y) + static_cast<qsizetype>(x) * 4;
    const auto* src = patch.constScanLine(y) + static_cast<qsizetype>(x) * 4;
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
    dst[3] = 255;
}

void repairMissingWindowTail(QImage& image, const QRect& fullGeometry, const QRect& visibleGeometry, const QImage& desktopImage, const QRect& desktopGeometry) {
    if (image.isNull() || !fullGeometry.isValid() || !visibleGeometry.isValid() || desktopImage.isNull() || !desktopGeometry.isValid())
        return;

    if (image.format() != QImage::Format_RGBA8888)
        image = image.convertToFormat(QImage::Format_RGBA8888);

    const QRect visibleImageRect = projectedImageRect(visibleGeometry, fullGeometry, image.size());
    if (!visibleImageRect.isValid() || visibleImageRect.width() < 8 || visibleImageRect.height() < 8)
        return;

    int tailStart = -1;
    for (int y = visibleImageRect.bottom(); y >= visibleImageRect.top(); --y) {
        const int transparent = transparentPixelsInRow(image, visibleImageRect, y);
        if (transparent * 10 >= visibleImageRect.width() * 9)
            tailStart = y;
        else
            break;
    }
    if (tailStart < 0 || visibleImageRect.bottom() - tailStart + 1 < 8)
        return;

    QImage visiblePatch(image.size(), QImage::Format_RGBA8888);
    visiblePatch.fill(Qt::transparent);
    {
        QPainter patchPainter(&visiblePatch);
        patchPainter.drawImage(visibleImageRect, desktopImage, QRect(visibleGeometry.topLeft() - desktopGeometry.topLeft(), visibleGeometry.size()));
    }

    const QRect visibleTailRect(visibleImageRect.left(), tailStart, visibleImageRect.width(), visibleImageRect.bottom() - tailStart + 1);
    for (int y = visibleTailRect.top(); y <= visibleTailRect.bottom(); ++y) {
        for (int x = visibleTailRect.left(); x <= visibleTailRect.right(); ++x) {
            auto* dst = image.scanLine(y) + static_cast<qsizetype>(x) * 4;
            if (dst[3] != 0)
                continue;
            const auto* src = visiblePatch.constScanLine(y) + static_cast<qsizetype>(x) * 4;
            if (src[3] == 0)
                continue;
            copyPatchPixel(image, visiblePatch, x, y);
        }
    }
}

} // namespace

InlineSelect::InlineSelect(QWidget* popupParent, QWidget* parent) : QWidget(parent), m_popupParent(popupParent) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_button = new QPushButton(this);
    m_button->setCheckable(true);
    m_button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_button->setLayoutDirection(Qt::RightToLeft);
    m_button->setIconSize(QSize(kSelectArrowIconSize, kSelectArrowIconSize));
    layout->addWidget(m_button);
    connect(m_button, &QPushButton::clicked, this, [this] {
        if (isPopupVisible())
            hidePopup();
        else
            showPopup();
    });

    m_panel = new QFrame(m_popupParent);
    m_panel->setObjectName("inlineSelectPopup");
    m_panel->setAttribute(Qt::WA_StyledBackground);
    m_panel->setStyleSheet(popupStyleSheet(QApplication::palette()));
    m_panel->hide();

    auto* panelLayout = new QVBoxLayout(m_panel);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(m_panel);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content = new QWidget(scroll);
    scroll->setWidget(content);
    panelLayout->addWidget(scroll);
    m_panelLayout = new QVBoxLayout(content);
    m_panelLayout->setContentsMargins(5, 5, 5, 5);
    m_panelLayout->setSpacing(2);
    updateButtonIcon();
}

void InlineSelect::addItems(const QStringList& items) {
    m_items = items;
    while (auto* item = m_panelLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    for (const auto& item : m_items) {
        const auto found = m_labels.find(item);
        const auto label = found == m_labels.end() ? item : found->second;
        auto* button = new QPushButton(m_button->fontMetrics().elidedText(label, Qt::ElideRight, 380), m_panelLayout->parentWidget());
        button->setToolTip(label);
        button->setProperty("value", item);
        button->setCheckable(true);
        connect(button, &QPushButton::clicked, this, [this, item] { choose(item); });
        m_panelLayout->addWidget(button);
        // A live refresh must make new items participate in sizeHint now,
        // before the popup is sized; Qt otherwise defers their visibility.
        button->show();
    }
    m_panelLayout->invalidate();
    m_panelLayout->activate();

    int width = 0;
    const auto metrics = m_button->fontMetrics();
    for (const auto& item : m_items)
        width = std::max(width, metrics.horizontalAdvance(buttonText(item)) + 42);
    m_buttonWidth = m_compactWidth > 0 ? std::min(width, m_compactWidth) : width;
    m_button->setMinimumWidth(m_buttonWidth);
    m_button->setMaximumWidth(m_compactWidth > 0 ? m_compactWidth : QWIDGETSIZE_MAX);
    m_panel->setMinimumWidth(std::min(width, 440));

    if (m_current.isEmpty() && !m_items.isEmpty())
        setCurrentText(m_items.first());
    else setCurrentText(m_current);
    if (isPopupVisible()) positionPopup();
}

void InlineSelect::setPrefix(const QString& prefix) {
    m_prefix = prefix;
    if (!m_current.isEmpty())
        m_button->setText(buttonText(m_current));
}

void InlineSelect::setOnChanged(std::function<void()> onChanged) {
    m_onChanged = std::move(onChanged);
}

void InlineSelect::setCurrentText(const QString& text) {
    m_current = text;
    const auto fullText = buttonText(text);
    m_button->setToolTip(fullText + (m_hint.isEmpty() ? QString{} : "\n" + m_hint));
    m_button->setText(m_compactWidth > 0 ? m_button->fontMetrics().elidedText(fullText, Qt::ElideRight, std::max(30, m_compactWidth - 42)) : fullText);
    for (auto* button : m_panel->findChildren<QPushButton*>())
        button->setChecked(button->property("value").toString() == text);
}

QString InlineSelect::currentText() const {
    return m_current;
}

void InlineSelect::setControlVisible(bool visible) {
    if (!visible) {
        hidePopup();
        m_button->hide();
        m_button->setMinimumSize(0, 0);
        m_button->setMaximumSize(0, 0);
        setFixedSize(0, 0);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        setVisible(false);
        updateGeometry();
        return;
    }

    setVisible(visible);
    setMinimumSize(0, 0);
    setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_button->show();
    m_button->setMinimumWidth(m_buttonWidth);
    m_button->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    adjustSize();
    updateGeometry();
}

void InlineSelect::hidePopup() {
    m_panel->hide();
    m_button->setChecked(false);
    updateButtonIcon();
    if (g_openSelect == this)
        g_openSelect = nullptr;
}

bool InlineSelect::isPopupVisible() const {
    return m_panel->isVisible();
}

QString InlineSelect::buttonText(const QString& text) const {
    const auto found = m_labels.find(text);
    const auto label = found == m_labels.end() ? text : found->second;
    if (m_prefix.isEmpty()) return label;
    return m_prefix + QStringLiteral(": ") + label;
}

void InlineSelect::updateButtonIcon() {
    if (!m_button)
        return;
    const double rotationDegrees = isPopupVisible() ? 180.0 : 0.0;
    m_button->setIcon(iconFromSvg(kSelectArrowSvg, kSelectArrowIconSize, rotationDegrees));
}

void InlineSelect::positionPopup() {
    m_panel->setFixedSize(std::min(std::max(160, m_panel->minimumWidth()), std::max(1, m_popupParent->width() - 16)),
                          std::min(std::max(40, m_panelLayout->sizeHint().height() + 4), std::min(360, std::max(1, m_popupParent->height() - 16))));
    QPoint pos = mapTo(m_popupParent, QPoint(0, height() + 5));
    if (pos.x() + m_panel->width() > m_popupParent->width() - 8)
        pos.setX(std::max(8, m_popupParent->width() - m_panel->width() - 8));
    if (pos.y() + m_panel->height() > m_popupParent->height() - 8)
        pos.setY(std::max(8, mapTo(m_popupParent, QPoint(0, 0)).y() - m_panel->height() - 5));
    m_panel->move(pos);
}

void InlineSelect::showPopup() {
    if (m_onOpening) m_onOpening();
    if (g_openSelect && g_openSelect != this)
        g_openSelect->hidePopup();
    positionPopup();
    m_panel->raise();
    m_panel->show();
    m_button->setChecked(true);
    updateButtonIcon();
    g_openSelect = this;
}

void InlineSelect::choose(const QString& text) {
    setCurrentText(text);
    hidePopup();
    if (m_onChanged)
        m_onChanged();
}

CaptureOverlay::CaptureOverlay(hyprcapture::CaptureDefaults defaults, bool quick, bool record, bool recordActive, QString sessionJson, QWidget* parent)
    : QMainWindow(parent), m_defaults(std::move(defaults)), m_mode(m_defaults.mode), m_quick(quick), m_record(record), m_recordActive(recordActive) {
    QElapsedTimer constructorTimer;
    constructorTimer.start();
    QElapsedTimer parseTimer;
    parseTimer.start();
    parseSessionJson(sessionJson);
    m_confirmBeforeCapture = m_defaults.confirmBeforeCapture && !m_quick && !m_record && !m_recordActive;
    beginHymissionCaptureInputSuppression();
    traceTiming(QStringLiteral("parse_session"), parseTimer.elapsed());

    QElapsedTimer preCaptureTimer;
    preCaptureTimer.start();
    captureScreensBeforeOverlay();
    traceTiming(QStringLiteral("prepare_desktop"), preCaptureTimer.elapsed());

    initializeOverlay(preferredOverlayLogicalGeometry());
    traceTiming(QStringLiteral("overlay_construct"), constructorTimer.elapsed());
    if (m_quick)
        QTimer::singleShot(0, this, &CaptureOverlay::finishCapture);
}

CaptureOverlay::CaptureOverlay(const CaptureOverlay& source, const QRect& overlayGeometry, bool active, QWidget* parent)
    : QMainWindow(parent),
      m_defaults(source.m_defaults),
      m_mode(source.m_mode),
      m_quick(source.m_quick),
      m_record(source.m_record),
      m_recordActive(source.m_recordActive),
      m_recordError(source.m_recordError),
      m_sessionDecoded(source.m_sessionDecoded),
      m_hymissionOverviewSession(source.m_hymissionOverviewSession),
      m_confirmBeforeCapture(source.m_confirmBeforeCapture),
      m_overlayActive(active),
      m_cursorLogicalPosition(source.m_cursorLogicalPosition),
      m_hasCursorLogicalPosition(source.m_hasCursorLogicalPosition),
      m_recordFormatAuto(source.m_recordFormatAuto),
      m_recordCodecAuto(source.m_recordCodecAuto),
      m_desktopImage(source.m_desktopImage),
      m_desktopGeometry(source.m_desktopGeometry),
      m_sessionMonitorCount(source.m_sessionMonitorCount),
      m_sessionWindowCount(source.m_sessionWindowCount),
      m_monitorArtifacts(source.m_monitorArtifacts),
      m_windowArtifacts(source.m_windowArtifacts) {
    initializeOverlay(overlayGeometry);
}

void CaptureOverlay::initializeOverlay(const QRect& overlayGeometry) {
    setWindowFlags(Qt::FramelessWindowHint | Qt::Tool);
    setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);

    m_overlayLogicalGeometry = overlayGeometry.isValid() ? overlayGeometry : QRect(0, 0, 1280, 720);
    setGeometry(m_overlayLogicalGeometry);

    QElapsedTimer toolbarTimer;
    toolbarTimer.start();
    buildToolbar();
    traceTiming(QStringLiteral("build_toolbar"), toolbarTimer.elapsed());

    winId();
    QScreen* targetScreen = screenForOverlayGeometry(m_overlayLogicalGeometry);
    if (targetScreen && windowHandle())
        windowHandle()->setScreen(targetScreen);
    if (auto* layerWindow = LayerShellQt::Window::get(windowHandle())) {
        layerWindow->setScope("hyprcapture-ui");
        layerWindow->setLayer(LayerShellQt::Window::LayerOverlay);
        if (targetScreen)
            layerWindow->setScreen(targetScreen);
        layerWindow->setAnchors(LayerShellQt::Window::Anchors{LayerShellQt::Window::AnchorTop} | LayerShellQt::Window::AnchorBottom |
                                LayerShellQt::Window::AnchorLeft | LayerShellQt::Window::AnchorRight);
        layerWindow->setExclusiveZone(-1);
        layerWindow->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityExclusive);
        layerWindow->setActivateOnShow(true);
        layerWindow->setDesiredSize(QSize(0, 0));
    }
    if (m_toolbar)
        m_toolbar->setVisible(m_overlayActive);
}

CaptureOverlay::~CaptureOverlay() {
    if (m_meterProcess) { m_meterProcess->disconnect(this); m_meterProcess->kill(); m_meterProcess->waitForFinished(1000); }
    endHymissionCaptureInputSuppression();
}

QScreen* CaptureOverlay::overlayScreen() const {
    return screenForOverlayGeometry(m_overlayLogicalGeometry);
}

hyprcapture::OverlayScope CaptureOverlay::overlayScope() const {
    return m_defaults.overlayScope;
}

bool CaptureOverlay::isOverlayActive() const {
    return m_overlayActive;
}

void CaptureOverlay::setOverlayActive(bool active) {
    if (m_overlayActive == active)
        return;

    m_overlayActive = active;
    hideOptionPopups();
    if (!active) {
        clearPendingConfirm();
        m_dragStart = {};
        m_dragEnd = {};
        m_fullscreenClientSelected = false;
    }
    if (m_toolbar)
        m_toolbar->setVisible(active);
    if (active) {
        setCursor(Qt::CrossCursor);
        raise();
        activateWindow();
        refreshInitialCursorPosition();
    }
    update();
}

void CaptureOverlay::adoptInteractionState(const CaptureOverlay& source) {
    if (&source == this)
        return;

    m_mode = source.m_mode;
    m_record = source.m_record;
    m_recordError = source.m_recordError;
    m_recordFormatAuto = source.m_recordFormatAuto;
    m_recordCodecAuto = source.m_recordCodecAuto;
    m_defaults.recordAudioMix = source.m_defaults.recordAudioMix;
    m_defaults.recordAudioSystemGain = source.m_defaults.recordAudioSystemGain;
    m_defaults.recordAudioMicGain = source.m_defaults.recordAudioMicGain;
    if (m_soundPreset) m_soundPreset->setCurrentText(qString(m_defaults.recordAudioMix));
    if (m_systemGain) m_systemGain->setValue(m_defaults.recordAudioSystemGain);
    if (m_micGain) m_micGain->setValue(m_defaults.recordAudioMicGain);
    m_defaults.recordAudio = source.m_defaults.recordAudio;
    m_defaults.recordAudioOutput = source.m_defaults.recordAudioOutput;
    m_defaults.recordAudioInput = source.m_defaults.recordAudioInput;
    if (m_soundMode) m_soundMode->setCurrentText(qString(hyprcapture::toString(m_defaults.recordAudio)));
    if (m_soundOutput) m_soundOutput->setCurrentText(qString(m_defaults.recordAudioOutput));
    if (m_soundInput) m_soundInput->setCurrentText(qString(m_defaults.recordAudioInput));

    if (m_recordToggle)
        m_recordToggle->setChecked(m_record);
    if (m_fullscreenScope)
        m_fullscreenScope->setCurrentText(qString(hyprcapture::toString(source.currentFullscreenScope())));
    if (m_windowBackground)
        m_windowBackground->setCurrentText(qString(hyprcapture::toString(source.currentWindowBackground())));
    if (m_recordFormat)
        m_recordFormat->setCurrentText(source.currentRecordFormat());
    if (m_recordCodec)
        m_recordCodec->setCurrentText(source.currentRecordCodec());
    if (m_recordFps)
        m_recordFps->setCurrentText(QString::number(source.currentRecordFps()));
    if (m_recordDuration)
        m_recordDuration->setCurrentText(animationDurationChoice(source.currentRecordMaxSeconds()));
    if (m_recordBackend)
        m_recordBackend->setCurrentText(qString(hyprcapture::toString(source.currentRecordBackend())));
    clearPendingConfirm();
    updateToolbarControlsForMode();
    updateRecordWarning();
    updateStatus();
}

bool CaptureOverlay::requestActivation() {
    if (!m_overlayActive)
        emit activationRequested();
    return m_overlayActive;
}

void CaptureOverlay::parseSessionJson(const QString& json) {
    const QByteArray encoded = json.toUtf8();
    const auto       decoded = hyprcapture::decodeSessionJson(std::string(encoded.constData(), static_cast<std::size_t>(encoded.size())));
    if (!decoded)
        return;

    m_sessionDecoded = true;
    m_defaults = decoded->defaults;
    m_mode = m_defaults.mode;
    if (decoded->cursorPosition) {
        QPoint cursorPosition;
        if (protocolPoint(*decoded->cursorPosition, cursorPosition)) {
            m_cursorLogicalPosition = cursorPosition;
            m_hasCursorLogicalPosition = true;
        }
    }

    m_sessionMonitorCount = std::min(static_cast<int>(decoded->monitors.size()), kMaxSessionMonitors);
    m_sessionWindowCount = std::min(static_cast<int>(decoded->windows.size()), kMaxSessionWindows);
    QStringList artifactFiles;
    qint64 remainingArtifactBytes = kMaxSessionArtifactBytes;

    for (std::size_t i = 0; i < decoded->monitors.size() && i < kMaxSessionMonitors; ++i) {
        const auto& info = decoded->monitors[i];
        MonitorArtifact artifact;
        artifact.name = qString(info.name);
        artifact.logicalGeometry = protocolRect(info.logicalGeometry);
        artifact.transform = info.transform;
        artifact.focused = info.focused;
        artifact.workspaceWindowCount = info.workspaceWindowCount;
        artifact.singleWorkspaceWindowClass = qString(info.singleWorkspaceWindowClass);
        artifact.singleWorkspaceWindowTitle = qString(info.singleWorkspaceWindowTitle);
        const QString artifactPath = qString(info.artifactPath);
        const QString cursorArtifactPath = qString(info.cursorArtifactPath);
        artifact.image = loadRawRgba(artifactPath, info.artifactWidth, info.artifactHeight, info.artifactTopDown, remainingArtifactBytes);
        artifact.image = normalizeMonitorArtifactImage(artifact.image, artifact.transform, artifact.logicalGeometry);
        artifact.previewCornerRadii =
            hyprcapture::ui::detectScreenCornerRadii(artifact.image, artifact.logicalGeometry.size());
        artifact.cursorImage =
            loadRawRgba(cursorArtifactPath,
                        info.cursorArtifactWidth,
                        info.cursorArtifactHeight,
                        info.cursorArtifactTopDown,
                        remainingArtifactBytes);
        artifact.cursorImage = normalizeMonitorArtifactImage(artifact.cursorImage, artifact.transform, artifact.logicalGeometry);
        artifactFiles.push_back(artifactPath);
        artifactFiles.push_back(cursorArtifactPath);
        if (!artifact.logicalGeometry.isValid())
            continue;
        m_desktopGeometry = m_desktopGeometry.united(artifact.logicalGeometry);
        if (!artifact.image.isNull())
            m_monitorArtifacts.push_back(std::move(artifact));
    }

    for (std::size_t i = 0; i < decoded->windows.size() && i < kMaxSessionWindows; ++i) {
        const auto& info = decoded->windows[i];
        WindowArtifact artifact;
        artifact.address = qString(info.address);
        artifact.title = qString(info.title);
        artifact.appClass = qString(info.appClass);
        artifact.zIndex = info.zIndex;
        artifact.focused = info.focused;
        artifact.fullscreen = info.fullscreen;
        artifact.visibleGeometry = protocolRect(info.visibleGeometry);
        artifact.fullGeometry = protocolRect(info.fullGeometry);
        if (info.selectionGeometry) {
            artifact.selectionGeometry = protocolRect(*info.selectionGeometry);
            if (artifact.selectionGeometry.isValid())
                m_hymissionOverviewSession = true;
        }
        if (info.selectionClipGeometry)
            artifact.selectionClipGeometry = protocolRect(*info.selectionClipGeometry);
        artifact.rounding = info.rounding;
        artifact.roundingPower = info.roundingPower;
        artifact.borderSize = info.borderSize;
        const QString artifactPath = qString(info.artifactPath);
        const QString realBackgroundPath = qString(info.realBackgroundPath);
        artifact.image = loadRawRgba(artifactPath, info.artifactWidth, info.artifactHeight, info.artifactTopDown, remainingArtifactBytes);
        artifact.realBackground =
            loadRawRgba(realBackgroundPath, info.realBackgroundWidth, info.realBackgroundHeight, info.realBackgroundTopDown, remainingArtifactBytes);
        artifactFiles.push_back(artifactPath);
        artifactFiles.push_back(realBackgroundPath);
        if (artifact.fullGeometry.isValid())
            m_windowArtifacts.push_back(std::move(artifact));
    }

    cleanupArtifactFiles(artifactFiles);
}

void CaptureOverlay::captureScreensBeforeOverlay() {
    if (!m_desktopGeometry.isValid())
        for (const auto* screen : QGuiApplication::screens())
            m_desktopGeometry = m_desktopGeometry.united(screen->geometry());

    if (!m_desktopGeometry.isValid())
        return;

    if (!m_monitorArtifacts.empty()) {
        double scaleX = 1.0;
        double scaleY = 1.0;
        for (const auto& artifact : m_monitorArtifacts) {
            if (!artifact.image.isNull() && artifact.logicalGeometry.isValid()) {
                scaleX = std::max(scaleX, static_cast<double>(artifact.image.width()) / std::max(1, artifact.logicalGeometry.width()));
                scaleY = std::max(scaleY, static_cast<double>(artifact.image.height()) / std::max(1, artifact.logicalGeometry.height()));
            }
        }

        const QSize imageSize = boundedScaledSize(m_desktopGeometry.width(), m_desktopGeometry.height(), scaleX, scaleY);
        m_desktopImage = boundedImage(imageSize, QImage::Format_RGBA8888);
        if (m_desktopImage.isNull())
            return;
        m_desktopImage.fill(QColor(30, 34, 38));

        QPainter painter(&m_desktopImage);
        for (const auto& artifact : m_monitorArtifacts) {
            const QRect target = logicalRectToOutputRect(artifact.logicalGeometry, m_desktopGeometry, scaleX, scaleY).intersected(m_desktopImage.rect());
            if (target.isValid())
                painter.drawImage(target, artifact.image);
        }
        return;
    }

    const QString grimProgram = hyprcapture::ui::trustedSystemProgram(QStringLiteral("grim"));
    QProcess      grim;
    if (!grimProgram.isEmpty()) {
        grim.setProcessEnvironment(hyprcapture::ui::trustedProcessEnvironment());
        grim.start(grimProgram, {"-t", "png", "-"});
    }
    if (grim.waitForFinished(1500) && grim.exitStatus() == QProcess::NormalExit && grim.exitCode() == 0) {
        QImage grimImage;
        if (grimImage.loadFromData(grim.readAllStandardOutput(), "PNG") && !grimImage.isNull()) {
            const QImage converted = grimImage.convertToFormat(QImage::Format_RGBA8888);
            if (imageSizeWithinBounds(converted.size())) {
                m_desktopImage = converted;
                return;
            }
        }
    }

    const double scale = maxScreenDevicePixelRatio();
    const QSize imageSize = boundedScaledSize(m_desktopGeometry.width(), m_desktopGeometry.height(), scale, scale);
    m_desktopImage = boundedImage(imageSize, QImage::Format_RGBA8888);
    if (m_desktopImage.isNull())
        return;
    m_desktopImage.fill(QColor(30, 34, 38));

    QPainter painter(&m_desktopImage);
    const auto screens = QGuiApplication::screens();
    for (auto* screen : screens) {
        const QPixmap pixmap = screen->grabWindow(0);
        if (pixmap.isNull())
            continue;
        const QRect target = logicalRectToOutputRect(screen->geometry(), m_desktopGeometry, scale, scale).intersected(m_desktopImage.rect());
        if (target.isValid())
            painter.drawPixmap(target, pixmap);
    }
}

QRect CaptureOverlay::preferredOverlayLogicalGeometry() const {
    if (m_hasCursorLogicalPosition) {
        for (const auto& artifact : m_monitorArtifacts) {
            if (artifact.logicalGeometry.contains(m_cursorLogicalPosition))
                return artifact.logicalGeometry;
        }

        if (QScreen* screen = QGuiApplication::screenAt(m_cursorLogicalPosition))
            return screen->geometry();
    }

    for (const auto& artifact : m_monitorArtifacts) {
        if (artifact.focused && artifact.logicalGeometry.isValid())
            return artifact.logicalGeometry;
    }

    if (QScreen* screen = QGuiApplication::screenAt(QCursor::pos()))
        return screen->geometry();
    if (QScreen* screen = QGuiApplication::primaryScreen())
        return screen->geometry();
    return m_desktopGeometry.isValid() ? m_desktopGeometry : QRect(0, 0, 1280, 720);
}

QScreen* CaptureOverlay::screenForOverlayGeometry(const QRect& logicalGeometry) const {
    QScreen* bestScreen = nullptr;
    int      bestArea = 0;
    for (QScreen* screen : QGuiApplication::screens()) {
        if (!screen)
            continue;
        const QRect intersection = logicalGeometry.intersected(screen->geometry());
        const int area = intersection.isValid() ? intersection.width() * intersection.height() : 0;
        if (area > bestArea) {
            bestScreen = screen;
            bestArea = area;
        }
    }

    if (bestScreen)
        return bestScreen;
    if (m_hasCursorLogicalPosition)
        bestScreen = QGuiApplication::screenAt(m_cursorLogicalPosition);
    if (!bestScreen)
        bestScreen = QGuiApplication::screenAt(QCursor::pos());
    return bestScreen ? bestScreen : QGuiApplication::primaryScreen();
}

void CaptureOverlay::buildToolbar() {
    m_toolbar = new QWidget(this);
    m_toolbar->setObjectName("toolbar");
    m_toolbar->setAttribute(Qt::WA_StyledBackground);
    m_toolbar->setStyleSheet(toolbarStyleSheet(QApplication::palette()));
    m_toolbarOpacity = new QGraphicsOpacityEffect(m_toolbar);
    m_toolbarOpacity->setOpacity(m_overlayOpacity);
    m_toolbar->setGraphicsEffect(m_toolbarOpacity);

    auto* rootLayout = new QVBoxLayout(m_toolbar);
    rootLayout->setContentsMargins(10, 7, 10, 7);
    rootLayout->setSpacing(5);
    rootLayout->setSizeConstraint(QLayout::SetFixedSize);

    auto* layout = new QHBoxLayout();
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    rootLayout->addLayout(layout);

    auto* group = new QButtonGroup(this);
    const auto addMode = [&](const QString& tooltip, hyprcapture::CaptureMode mode, const QIcon& icon) {
        auto* button = new QPushButton(m_toolbar);
        button->setObjectName("captureModeButton");
        button->setFlat(true);
        button->setFocusPolicy(Qt::NoFocus);
        button->setIcon(icon);
        button->setIconSize(QSize(kModeIconSize, kModeIconSize));
        button->setFixedSize(36, 32);
        button->setToolTip(tooltip);
        button->setAccessibleName(tooltip);
        button->setCheckable(true);
        button->setChecked(mode == m_mode);
        group->addButton(button);
        layout->addWidget(button);
        if (m_defaults.fushionMode && mode != hyprcapture::CaptureMode::Fullscreen) {
            button->hide();
            button->setFixedSize(0, 0);
            button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        }
        connect(button, &QPushButton::clicked, this, [this, mode] {
            const bool wasActiveMode = m_mode == mode;
            setMode(mode);
            if (mode == hyprcapture::CaptureMode::Fullscreen && (m_defaults.fushionMode || wasActiveMode)) {
                if (confirmBeforeCaptureEnabled())
                    beginPendingConfirm(hyprcapture::CaptureMode::Fullscreen);
                else
                    finishCapture();
            }
        });
    };
    addMode("Fullscreen", hyprcapture::CaptureMode::Fullscreen, iconFromSvg(kFullscreenSvg));
    addMode("Region", hyprcapture::CaptureMode::Region, iconFromSvg(kRegionSvg));
    addMode("Window", hyprcapture::CaptureMode::Window, iconFromSvg(kWindowSvg));

    m_fullscreenScope = new InlineSelect(this, m_toolbar);
    m_fullscreenScope->setPrefix("Full");
    m_fullscreenScope->addItems(QStringList{"all", "current", "per-monitor"});
    m_fullscreenScope->setCurrentText(qString(hyprcapture::toString(m_defaults.fullscreenScope)));
    layout->addWidget(m_fullscreenScope);

    m_windowBackground = new InlineSelect(this, m_toolbar);
    m_windowBackground->setPrefix("Bg");
    m_windowBackground->addItems(QStringList{"follow-system", "white", "black", "real", "transparent"});
    m_windowBackground->setCurrentText(qString(hyprcapture::toString(m_defaults.windowBackground)));
    m_windowBackground->setOnChanged([this] {
        if (m_record)
            applyRecordDefaultsForCurrentBackground();
        updateRecordWarning();
        updateStatus();
    });
    layout->addWidget(m_windowBackground);

    m_recordToggle = new QPushButton(m_toolbar);
    m_recordToggle->setObjectName(m_recordActive ? "recordActiveButton" : "recordToggleButton");
    m_recordToggle->setFlat(true);
    m_recordToggle->setFocusPolicy(Qt::NoFocus);
    m_recordToggle->setIcon(iconFromSvg(kRecordSvg));
    m_recordToggle->setIconSize(QSize(kModeIconSize, kModeIconSize));
    m_recordToggle->setFixedSize(36, 32);
    m_recordToggle->setToolTip(m_recordActive ? "Stop recording" : "Record");
    m_recordToggle->setAccessibleName(m_recordActive ? "Stop recording" : "Record");
    m_recordToggle->setCheckable(true);
    m_recordToggle->setChecked(m_record || m_recordActive);
    layout->addWidget(m_recordToggle);
    connect(m_recordToggle, &QPushButton::clicked, this, [this] {
        if (m_recordActive) {
            m_recordToggle->setChecked(true);
            if (stopRecording()) {
                emit finishingStarted();
                fadeOutThen([this] {
                    endHymissionCaptureInputSuppression();
                    qApp->quit();
                });
            }
            return;
        }

        m_recordError.clear();
        m_record = m_recordToggle->isChecked();
        if (m_record)
            applyRecordDefaultsForCurrentBackground();
        updateRecordOptionsVisibility();
        updateStatus();
        update();
    });

    auto* cancel = new QPushButton(m_toolbar);
    cancel->setFlat(true);
    cancel->setFocusPolicy(Qt::NoFocus);
    cancel->setIcon(iconFromSvg(kCancelSvg));
    cancel->setIconSize(QSize(kCancelIconSize, kCancelIconSize));
    cancel->setFixedSize(36, 32);
    cancel->setToolTip("Cancel");
    cancel->setAccessibleName("Cancel");
    layout->addWidget(cancel);
    connect(cancel, &QPushButton::clicked, this, &CaptureOverlay::cancelCapture);

    m_confirmButton = new QPushButton(m_toolbar);
    m_confirmButton->setFlat(true);
    m_confirmButton->setFocusPolicy(Qt::NoFocus);
    m_confirmButton->setIcon(iconFromSvg(kConfirmSvg, kConfirmIconSize));
    m_confirmButton->setIconSize(QSize(kConfirmIconSize, kConfirmIconSize));
    m_confirmButton->setFixedSize(36, 32);
    m_confirmButton->setToolTip("Capture");
    m_confirmButton->setAccessibleName("Capture");
    layout->addWidget(m_confirmButton);
    connect(m_confirmButton, &QPushButton::clicked, this, &CaptureOverlay::confirmPendingCapture);

    m_status = new QLabel(m_toolbar);
    m_status->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    layout->addWidget(m_status);

    m_recordOptions = new QWidget(m_toolbar);
    auto* recordLayout = new QHBoxLayout(m_recordOptions);
    recordLayout->setContentsMargins(0, 0, 0, 0);
    recordLayout->setSpacing(5);

    const auto onRecordOptionChanged = [this] {
        m_recordError.clear();
        updateRecordOptionsVisibility();
        updateStatus();
    };

    m_recordCodec = new InlineSelect(this, m_recordOptions);
    m_recordCodec->setPrefix("Codec");
    m_recordCodec->addItems(QStringList{"auto", "h264", "h265", "av1", "vp9", "ffv1"});
    m_recordCodec->setCurrentText(defaultRecordCodecForBackground(m_defaults, currentRecordBackground()));
    m_recordCodec->setOnChanged([this, onRecordOptionChanged] {
        m_recordCodecAuto = false;
        onRecordOptionChanged();
    });
    recordLayout->addWidget(m_recordCodec);

    m_recordFormat = new InlineSelect(this, m_recordOptions);
    m_recordFormat->setObjectName("recordFormat");
    m_recordFormat->setPrefix("Format");
    m_recordFormat->addItems(QStringList{"mp4", "mov", "webm", "mkv", "gif", "apng", "webp"});
    m_recordFormat->setCurrentText(defaultRecordFormatForBackground(m_defaults, currentRecordBackground()));
    m_recordFormat->setOnChanged([this, onRecordOptionChanged] {
        m_recordFormatAuto = false;
        if (isImageAnimationRecordFormat(currentRecordFormat())) {
            if (m_recordDuration)
                m_recordDuration->setCurrentText(animationDurationChoice(currentRecordMaxSeconds()));
        } else if (currentRecordAlphaRequested() && m_recordCodec) {
            const QString format = currentRecordFormat();
            if (format == "webm" || format == "mkv") {
                m_recordCodec->setCurrentText(transparentAutoChoiceForFormat(format).codec);
                m_recordCodecAuto = true;
            }
        }
        onRecordOptionChanged();
    });
    recordLayout->addWidget(m_recordFormat);

    m_recordFps = new InlineSelect(this, m_recordOptions);
    m_recordFps->setPrefix("FPS");
    m_recordFps->addItems(recordFpsChoices(m_defaults));
    m_recordFps->setCurrentText(QString::number(std::clamp<std::int64_t>(m_defaults.recordFps, 1, 240)));
    m_recordFps->setOnChanged(onRecordOptionChanged);
    recordLayout->addWidget(m_recordFps);

    m_recordDuration = new InlineSelect(this, m_recordOptions);
    m_recordDuration->setPrefix("Duration");
    m_recordDuration->addItems(animationDurationChoices());
    m_recordDuration->setCurrentText(animationDurationChoice(m_defaults.recordMaxSeconds));
    m_recordDuration->setOnChanged(onRecordOptionChanged);
    recordLayout->addWidget(m_recordDuration);

    m_recordBackend = new InlineSelect(this, m_recordOptions);
    m_recordBackend->setPrefix("Backend");
    m_recordBackend->addItems(QStringList{"auto", "compositor", "gsr-visible"});
    m_recordBackend->setCurrentText(qString(hyprcapture::toString(m_defaults.recordWindowBackend)));
    m_recordBackend->setOnChanged(onRecordOptionChanged);
    recordLayout->addWidget(m_recordBackend);

    rootLayout->addWidget(m_recordOptions, 0, Qt::AlignHCenter);

    m_soundOptions = new QWidget(m_toolbar);
    m_soundOptions->setObjectName("soundOptions");
    auto* soundLayout = new QHBoxLayout(m_soundOptions);
    soundLayout->setContentsMargins(0, 0, 0, 0);
    soundLayout->setSpacing(5);
    m_soundMode = new InlineSelect(this, m_soundOptions);
    m_soundMode->setObjectName("soundMode");
    m_soundMode->setPrefix("Sound");
    m_soundMode->addItems({"off", "system", "microphone", "mix"});
    m_soundMode->setLabels({{"off", "Off"}, {"system", "System"}, {"microphone", "Microphone"}, {"mix", "Mix"}});
    m_soundMode->setCurrentText(qString(hyprcapture::toString(m_defaults.recordAudio)));
    m_soundMode->setOnChanged([this, onRecordOptionChanged] {
        m_defaults.recordAudio = hyprcapture::parseRecordAudio(m_soundMode->currentText().toStdString());
        onRecordOptionChanged();
    });
    soundLayout->addWidget(m_soundMode);
    m_soundOutput = new InlineSelect(this, m_soundOptions);
    m_soundOutput->setObjectName("soundOutput");
    m_soundInput = new InlineSelect(this, m_soundOptions);
    m_soundInput->setObjectName("soundInput");
    m_soundOutput->setPrefix("Source");
    m_soundInput->setPrefix("Mic");
    for (auto* select : {m_soundOutput, m_soundInput}) {
        select->setCompactWidth(210);
        select->addItems({"default"});
        select->setLabels({{"default", "System default"}});
        select->setOnOpening([this] { refreshSoundDevices(); });
        soundLayout->addWidget(select);
    }
    m_soundOutput->addItems({"auto", "default"});
    m_soundOutput->setLabels({{"auto", "Auto"}, {"default", "System default"}});
    m_soundOutput->setCurrentText(qString(m_defaults.recordAudioOutput));
    m_soundInput->setCurrentText(qString(m_defaults.recordAudioInput));
    m_soundOutput->setOnChanged([this, onRecordOptionChanged] {
        m_defaults.recordAudioOutput = m_soundOutput->currentText().toStdString(); onRecordOptionChanged();
    });
    m_soundInput->setOnChanged([this, onRecordOptionChanged] {
        m_defaults.recordAudioInput = m_soundInput->currentText().toStdString(); onRecordOptionChanged();
    });
    m_soundPreset = new InlineSelect(this, m_soundOptions);
    m_soundPreset->setObjectName("soundPreset");
    m_soundPreset->addItems({"manual", "auto-balance", "voice-priority"});
    m_soundPreset->setLabels({{"manual", "Manual"}, {"auto-balance", "Auto balance"}, {"voice-priority", "Voice priority"}});
    m_soundPreset->setCurrentText(qString(m_defaults.recordAudioMix));
    m_soundPreset->setOnChanged([this, onRecordOptionChanged] {
        m_defaults.recordAudioMix = m_soundPreset->currentText().toStdString(); onRecordOptionChanged();
    });
    soundLayout->addWidget(m_soundPreset);
    rootLayout->addWidget(m_soundOptions);
    m_soundMixer = new QWidget(m_toolbar);
    m_soundMixer->setObjectName("soundMixer");
    auto* mixerLayout = new QHBoxLayout(m_soundMixer);
    mixerLayout->setContentsMargins(0, 2, 0, 2); mixerLayout->setSpacing(14);
    auto channel = [this, mixerLayout](const QString& name, QSlider*& slider, AudioMeter*& meter, std::int64_t& gain) {
        auto* strip = new QWidget(m_soundMixer);
        auto* layout = new QVBoxLayout(strip); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(2);
        auto* label = new QLabel(strip);
        slider = new QSlider(Qt::Horizontal, strip);
        slider->setObjectName(name.toLower() + "Gain"); slider->setAccessibleName(name + " gain in dB");
        slider->setRange(-61, 24); slider->setValue(gain); slider->setMinimumWidth(90);
        meter = new AudioMeter(strip); meter->setObjectName(name.toLower() + "Meter"); meter->setGain(gain);
        auto updateGain = [label, meter, name, &gain](int value) {
            gain = value; meter->setGain(value);
            label->setText(name + "   " + (value == -61 ? QString("Mute") : QString::number(value) + " dB"));
        };
        updateGain(gain); connect(slider, &QSlider::valueChanged, this, updateGain);
        layout->addWidget(label); layout->addWidget(slider); layout->addWidget(meter);
        mixerLayout->addWidget(strip, 1);
    };
    channel("Sound", m_systemGain, m_systemMeter, m_defaults.recordAudioSystemGain);
    channel("Mic", m_micGain, m_micMeter, m_defaults.recordAudioMicGain);
    rootLayout->addWidget(m_soundMixer);
    auto* meterLifecycle = new QTimer(this);
    connect(meterLifecycle, &QTimer::timeout, this, &CaptureOverlay::updateSoundMeter);
    meterLifecycle->start(100);
    refreshSoundDevices();


    m_recordWarning = new QLabel(m_toolbar);
    m_recordWarning->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_recordWarning->setStyleSheet(QStringLiteral("color: rgba(242, 170, 55, 255); padding: 2px 4px;"));
    rootLayout->addWidget(m_recordWarning);

    if (m_record && !m_recordActive)
        applyRecordDefaultsForCurrentBackground();
    updateToolbarControlsForMode();
    updateRecordOptionsVisibility();
    updateRecordWarning();
    updateStatus();
    relayoutToolbar();
}

double CaptureOverlay::overlayOpacity() const {
    return m_overlayOpacity;
}

void CaptureOverlay::setOverlayOpacity(double opacity) {
    m_overlayOpacity = std::clamp(opacity, 0.0, 1.0);
    if (m_toolbarOpacity)
        m_toolbarOpacity->setOpacity(m_overlayOpacity);
    update();
}

void CaptureOverlay::runOverlayFade(double start, double end, std::function<void()> finished) {
    if (m_fadeAnimation) {
        m_fadeAnimation->stop();
        m_fadeAnimation->deleteLater();
        m_fadeAnimation = nullptr;
    }

    setOverlayOpacity(start);

    auto* animation = new QPropertyAnimation(this, "overlayOpacity", this);
    m_fadeAnimation = animation;
    animation->setDuration(kOverlayFadeDurationMs);
    animation->setStartValue(start);
    animation->setEndValue(end);
    animation->setEasingCurve(QEasingCurve::InOutCubic);
    connect(animation, &QPropertyAnimation::finished, this, [this, animation, finished = std::move(finished)]() mutable {
        if (m_fadeAnimation == animation)
            m_fadeAnimation = nullptr;
        animation->deleteLater();
        if (finished)
            finished();
    });
    animation->start();
}

void CaptureOverlay::startFadeIn() {
    runOverlayFade(m_overlayOpacity, 1.0, {});
}

void CaptureOverlay::fadeOutThen(std::function<void()> finished) {
    if (m_fadeOutStarted)
        return;
    m_fadeOutStarted = true;

    hideOptionPopups();

    runOverlayFade(m_overlayOpacity, 0.0, [this, finished = std::move(finished)]() mutable {
        hide();
        if (finished)
            finished();
    });
}

void CaptureOverlay::enterEvent(QEnterEvent* event) {
    requestActivation();
    QMainWindow::enterEvent(event);
}

void CaptureOverlay::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    if (!m_fadeOutStarted && m_overlayOpacity < 1.0)
        startFadeIn();
    QTimer::singleShot(0, this, &CaptureOverlay::refreshInitialCursorPosition);
}

void CaptureOverlay::hideOptionPopups() {
    if (m_fullscreenScope)
        m_fullscreenScope->hidePopup();
    if (m_windowBackground)
        m_windowBackground->hidePopup();
    for (auto* select : {m_soundMode, m_soundOutput, m_soundInput, m_soundPreset}) if (select) select->hidePopup();
    if (m_recordCodec)
        m_recordCodec->hidePopup();
    if (m_recordFormat)
        m_recordFormat->hidePopup();
    if (m_recordFps)
        m_recordFps->hidePopup();
    if (m_recordDuration)
        m_recordDuration->hidePopup();
    if (m_recordBackend)
        m_recordBackend->hidePopup();
}

bool CaptureOverlay::confirmBeforeCaptureEnabled() const {
    return m_confirmBeforeCapture;
}

bool CaptureOverlay::pendingConfirmActive() const {
    return confirmBeforeCaptureEnabled() && m_pendingConfirm;
}

bool CaptureOverlay::regionSelectionValid(const QRect& selection) const {
    const QRect clipped = selection.normalized().intersected(regionCaptureBounds());
    return clipped.width() > 4 && clipped.height() > 4;
}

void CaptureOverlay::setSelectionRect(const QRect& rect) {
    const QRect clipped = rect.normalized().intersected(regionCaptureBounds());
    m_dragStart = clipped.topLeft();
    m_dragEnd = clipped.bottomRight();
}

void CaptureOverlay::updateConfirmButtonVisibility() {
    if (!m_confirmButton)
        return;

    const bool targetValid = pendingConfirmActive() &&
        (m_mode == hyprcapture::CaptureMode::Fullscreen ||
         (m_mode == hyprcapture::CaptureMode::Region && regionSelectionValid(normalizedSelection())) ||
         (m_mode == hyprcapture::CaptureMode::Window && selectedWindow()));

    if (!targetValid) {
        m_confirmButton->hide();
        m_confirmButton->setFixedSize(0, 0);
        m_confirmButton->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        return;
    }

    m_confirmButton->setVisible(true);
    m_confirmButton->setFixedSize(36, 32);
    m_confirmButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void CaptureOverlay::beginPendingConfirm(hyprcapture::CaptureMode mode) {
    if (!confirmBeforeCaptureEnabled()) {
        finishCapture();
        return;
    }

    m_mode = mode;
    m_pendingConfirm = true;
    m_confirmDragMode = ConfirmDragMode::None;
    m_dragging = false;
    if (mode != hyprcapture::CaptureMode::Window) {
        m_selectedWindowIndex = -1;
        resetWindowCycle();
    }
    updateToolbarControlsForMode();
    updateStatus();
    update();
}

void CaptureOverlay::clearPendingConfirm() {
    m_pendingConfirm = false;
    m_confirmDragMode = ConfirmDragMode::None;
    m_dragging = false;
    m_selectedWindowIndex = -1;
    resetWindowCycle();
    setCursor(Qt::CrossCursor);
    updateConfirmButtonVisibility();
}

void CaptureOverlay::confirmPendingCapture() {
    if (!pendingConfirmActive())
        return;

    if (m_mode == hyprcapture::CaptureMode::Region && !regionSelectionValid(normalizedSelection()))
        return;
    if (m_mode == hyprcapture::CaptureMode::Window && !selectedWindow())
        return;

    m_pendingConfirm = false;
    m_confirmDragMode = ConfirmDragMode::None;
    updateConfirmButtonVisibility();
    finishCapture();
}

CaptureOverlay::ConfirmDragMode CaptureOverlay::confirmRegionDragModeAt(const QPoint& point) const {
    const QRect selection = normalizedSelection().intersected(regionCaptureBounds());
    if (!regionSelectionValid(selection))
        return ConfirmDragMode::NewSelection;

    const int hit = kSelectionHandleHitRadius;
    const bool nearLeft = std::abs(point.x() - selection.left()) <= hit && point.y() >= selection.top() - hit && point.y() <= selection.bottom() + hit;
    const bool nearRight = std::abs(point.x() - selection.right()) <= hit && point.y() >= selection.top() - hit && point.y() <= selection.bottom() + hit;
    const bool nearTop = std::abs(point.y() - selection.top()) <= hit && point.x() >= selection.left() - hit && point.x() <= selection.right() + hit;
    const bool nearBottom = std::abs(point.y() - selection.bottom()) <= hit && point.x() >= selection.left() - hit && point.x() <= selection.right() + hit;

    if (nearLeft && nearTop)
        return ConfirmDragMode::ResizeTopLeft;
    if (nearRight && nearTop)
        return ConfirmDragMode::ResizeTopRight;
    if (nearRight && nearBottom)
        return ConfirmDragMode::ResizeBottomRight;
    if (nearLeft && nearBottom)
        return ConfirmDragMode::ResizeBottomLeft;
    if (nearLeft)
        return ConfirmDragMode::ResizeLeft;
    if (nearTop)
        return ConfirmDragMode::ResizeTop;
    if (nearRight)
        return ConfirmDragMode::ResizeRight;
    if (nearBottom)
        return ConfirmDragMode::ResizeBottom;
    if (selection.contains(point))
        return ConfirmDragMode::MoveSelection;
    return ConfirmDragMode::NewSelection;
}

Qt::CursorShape CaptureOverlay::cursorForConfirmDragMode(ConfirmDragMode mode) const {
    switch (mode) {
        case ConfirmDragMode::MoveSelection: return Qt::SizeAllCursor;
        case ConfirmDragMode::ResizeLeft:
        case ConfirmDragMode::ResizeRight: return Qt::SizeHorCursor;
        case ConfirmDragMode::ResizeTop:
        case ConfirmDragMode::ResizeBottom: return Qt::SizeVerCursor;
        case ConfirmDragMode::ResizeTopLeft:
        case ConfirmDragMode::ResizeBottomRight: return Qt::SizeFDiagCursor;
        case ConfirmDragMode::ResizeTopRight:
        case ConfirmDragMode::ResizeBottomLeft: return Qt::SizeBDiagCursor;
        case ConfirmDragMode::NewSelection:
        case ConfirmDragMode::None: return Qt::CrossCursor;
    }
    return Qt::CrossCursor;
}

void CaptureOverlay::updateConfirmCursor(const QPoint& point) {
    if (!pendingConfirmActive() || m_mode != hyprcapture::CaptureMode::Region) {
        setCursor(Qt::CrossCursor);
        return;
    }

    const ConfirmDragMode mode = m_dragging ? m_confirmDragMode : confirmRegionDragModeAt(point);
    setCursor(cursorForConfirmDragMode(mode));
}

QRect CaptureOverlay::regionSelectionForDrag(const QPoint& point) const {
    const QRect bounds = regionCaptureBounds();
    const QPoint clamped = clampedToRect(point, bounds);
    QRect selection = m_confirmDragStartSelection;

    if (m_confirmDragMode == ConfirmDragMode::NewSelection)
        return QRect(m_confirmDragStart, clamped).normalized().intersected(bounds);

    if (m_confirmDragMode == ConfirmDragMode::MoveSelection) {
        QRect moved = selection.translated(clamped - m_confirmDragStart);
        if (moved.left() < bounds.left())
            moved.moveLeft(bounds.left());
        if (moved.top() < bounds.top())
            moved.moveTop(bounds.top());
        if (moved.right() > bounds.right())
            moved.moveRight(bounds.right());
        if (moved.bottom() > bounds.bottom())
            moved.moveBottom(bounds.bottom());
        return moved.intersected(bounds);
    }

    int left = selection.left();
    int top = selection.top();
    int right = selection.right();
    int bottom = selection.bottom();
    switch (m_confirmDragMode) {
        case ConfirmDragMode::ResizeLeft:
        case ConfirmDragMode::ResizeTopLeft:
        case ConfirmDragMode::ResizeBottomLeft: left = clamped.x(); break;
        default: break;
    }
    switch (m_confirmDragMode) {
        case ConfirmDragMode::ResizeTop:
        case ConfirmDragMode::ResizeTopLeft:
        case ConfirmDragMode::ResizeTopRight: top = clamped.y(); break;
        default: break;
    }
    switch (m_confirmDragMode) {
        case ConfirmDragMode::ResizeRight:
        case ConfirmDragMode::ResizeTopRight:
        case ConfirmDragMode::ResizeBottomRight: right = clamped.x(); break;
        default: break;
    }
    switch (m_confirmDragMode) {
        case ConfirmDragMode::ResizeBottom:
        case ConfirmDragMode::ResizeBottomRight:
        case ConfirmDragMode::ResizeBottomLeft: bottom = clamped.y(); break;
        default: break;
    }

    return QRect(QPoint(left, top), QPoint(right, bottom)).normalized().intersected(bounds);
}

void CaptureOverlay::setMode(hyprcapture::CaptureMode mode) {
    hideOptionPopups();

    clearPendingConfirm();
    m_fullscreenClientSelected = false;
    m_mode = mode;
    if (m_record)
        applyRecordDefaultsForCurrentBackground();
    updateToolbarControlsForMode();
    updateStatus();
    update();
}

void CaptureOverlay::updateToolbarControlsForMode() {
    if (m_fullscreenScope) {
        const bool visible = hasMultipleMonitors() && (m_defaults.fushionMode || m_mode == hyprcapture::CaptureMode::Fullscreen);
        m_fullscreenScope->setControlVisible(visible);
    }

    if (m_windowBackground) {
        const bool visible = m_defaults.fushionMode || m_mode == hyprcapture::CaptureMode::Window;
        m_windowBackground->setControlVisible(visible);
    }

    updateRecordOptionsVisibility();
    updateConfirmButtonVisibility();
    relayoutToolbar();
}

int CaptureOverlay::monitorCount() const {
    const int qtScreens = QGuiApplication::screens().size();
    if (qtScreens > 0)
        return qtScreens;
    return m_sessionMonitorCount;
}

bool CaptureOverlay::hasMultipleMonitors() const {
    return monitorCount() > 1;
}

hyprcapture::FullscreenScope CaptureOverlay::currentFullscreenScope() const {
    if (!m_fullscreenScope || !hasMultipleMonitors())
        return hyprcapture::FullscreenScope::All;
    return hyprcapture::parseFullscreenScope(m_fullscreenScope->currentText().toStdString(), m_defaults.fullscreenScope);
}

hyprcapture::WindowBackground CaptureOverlay::currentWindowBackground() const {
    if (!m_windowBackground)
        return m_defaults.windowBackground;
    return hyprcapture::parseWindowBackground(m_windowBackground->currentText().toStdString(), m_defaults.windowBackground);
}

hyprcapture::WindowBackground CaptureOverlay::currentRecordBackground() const {
    if (m_mode != hyprcapture::CaptureMode::Window)
        return hyprcapture::WindowBackground::FollowSystem;
    return currentWindowBackground();
}

bool CaptureOverlay::currentRecordTransparencyRequired() const {
    return m_mode == hyprcapture::CaptureMode::Window && transparencyRequired(currentWindowBackground());
}

bool CaptureOverlay::currentRecordAlphaRequested() const {
    return m_mode == hyprcapture::CaptureMode::Window && alphaRecordingRequested(m_defaults, currentWindowBackground());
}

QString CaptureOverlay::currentRecordFormat() const {
    if (!m_recordFormat)
        return defaultRecordFormatForBackground(m_defaults, currentRecordBackground());
    return normalizedRecordFormat(m_recordFormat->currentText());
}

QString CaptureOverlay::currentRecordCodec() const {
    if (!m_recordCodec)
        return defaultRecordCodecForBackground(m_defaults, currentRecordBackground());
    return normalizedChoice(m_recordCodec->currentText());
}

int CaptureOverlay::currentRecordFps() const {
    if (!m_recordFps)
        return std::clamp<std::int64_t>(m_defaults.recordFps, 1, 240);
    bool ok = false;
    const int fps = m_recordFps->currentText().toInt(&ok);
    return ok ? std::clamp(fps, 1, 240) : std::clamp<std::int64_t>(m_defaults.recordFps, 1, 240);
}

int CaptureOverlay::currentRecordMaxSeconds() const {
    if (isImageAnimationRecordFormat(currentRecordFormat()))
        return animationDurationSeconds(m_recordDuration ? m_recordDuration->currentText() : animationDurationChoice(m_defaults.recordMaxSeconds));
    return std::clamp<std::int64_t>(m_defaults.recordMaxSeconds, 0, 24 * 60 * 60);
}

int CaptureOverlay::currentRecordCountdownSeconds() const {
    return std::clamp<std::int64_t>(m_defaults.recordCountdownSeconds, 0, kMaxRecordCountdownSeconds);
}

hyprcapture::RecordWindowBackend CaptureOverlay::currentRecordBackend() const {
    if (!m_recordBackend)
        return m_defaults.recordWindowBackend;
    return hyprcapture::parseRecordWindowBackend(m_recordBackend->currentText().toStdString(), m_defaults.recordWindowBackend);
}

void CaptureOverlay::applyRecordDefaultsForCurrentBackground() {
    const auto background = currentRecordBackground();

    if (background != hyprcapture::WindowBackground::Transparent) {
        if (m_recordFormatAuto && m_recordFormat)
            m_recordFormat->setCurrentText(defaultRecordFormatForBackground(m_defaults, background));
        if (m_recordCodecAuto && m_recordCodec)
            m_recordCodec->setCurrentText(defaultRecordCodecForBackground(m_defaults, background));
        return;
    }

    const QString configuredFormat = defaultRecordFormatForBackground(m_defaults, background);
    const QString configuredCodec = defaultRecordCodecForBackground(m_defaults, background);
    const QString targetFormat = m_recordFormatAuto ? configuredFormat : currentRecordFormat();
    if (configuredCodec == "auto") {
        const auto choice = transparentAutoChoiceForFormat(targetFormat);
        if (m_recordFormatAuto && m_recordFormat)
            m_recordFormat->setCurrentText(choice.format);
        if (m_recordCodecAuto && m_recordCodec)
            m_recordCodec->setCurrentText(choice.codec);
        return;
    }

    if (m_recordFormatAuto && m_recordFormat)
        m_recordFormat->setCurrentText(configuredFormat);
    if (m_recordCodecAuto && m_recordCodec)
        m_recordCodec->setCurrentText(configuredCodec);
}

QString CaptureOverlay::recordOptionsConflict() const {
    if (!m_record)
        return {};

    const bool alphaRequested = currentRecordTransparencyRequired();
    const QString format = currentRecordFormat();
    const QString codec = currentRecordCodec();
    const bool imageAnimation = isImageAnimationRecordFormat(format);

    if (imageAnimation)
        return {};

    if (alphaRequested && currentRecordBackend() == hyprcapture::RecordWindowBackend::GsrVisible)
        return QStringLiteral("selected backend does not support transparency");
    if (alphaRequested && format == "mp4")
        return QStringLiteral("mp4 does not support transparency");
    if (alphaRequested && format == "mov")
        return QStringLiteral("mov alpha is not supported by this encoder");
    if (format == "webm" && alphaRequested && codec != "auto" && codec != "vp9")
        return QStringLiteral("webm transparency requires vp9");
    if (format == "webm" && codec != "auto" && codec != "vp9" && codec != "av1")
        return QStringLiteral("webm requires vp9 or av1");
    if (format == "mkv" && alphaRequested && codec != "auto" && codec != "ffv1")
        return QStringLiteral("mkv transparency requires ffv1");
    if ((format == "mp4" || format == "mov") && (codec == "vp9" || codec == "ffv1"))
        return format + QStringLiteral(" requires h264, h265, or av1");
    return {};
}

QString CaptureOverlay::recordOptionsWarning() const {
    if (!m_record)
        return {};

    const QString format = currentRecordFormat();
    const QString codec = currentRecordCodec();
    if (isImageAnimationRecordFormat(format)) {
        if (format == "apng") {
            if (currentRecordMaxSeconds() >= 10)
                return QStringLiteral("apng recordings of 10s or longer can create very large files");
            return QStringLiteral("apng records a 60 fps mkv intermediate before transcoding");
        }
        if (format == "gif" && currentRecordAlphaRequested())
            return QStringLiteral("gif has limited transparency; using compositor readback for a fixed-duration animation");
        return QStringLiteral("animation formats use compositor readback; keep area and fps modest");
    }

    if (!currentRecordAlphaRequested())
        return {};

    if (format == "webm" && (codec == "auto" || codec == "vp9")) {
        if (!alphaProbeSucceeded(format, QStringLiteral("vp9-vaapi")))
            return QStringLiteral("no hardware alpha encoder detected; using CPU vp9");
    }
    if (format == "mkv" && (codec == "auto" || codec == "ffv1"))
        return QStringLiteral("no hardware alpha encoder detected; using CPU ffv1");
    return {};
}

void CaptureOverlay::updateRecordWarning() {
    if (!m_recordWarning)
        return;

    const QString conflict = recordOptionsConflict();
    const QString warning = conflict.isEmpty() ? recordOptionsWarning() : QString{};
    if (conflict.isEmpty() && warning.isEmpty()) {
        m_recordWarning->clear();
        m_recordWarning->hide();
        m_recordWarning->setFixedSize(0, 0);
        m_recordWarning->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        relayoutToolbar();
        return;
    }

    m_recordWarning->setText(QStringLiteral("⚠️: ") + (conflict.isEmpty() ? warning : conflict));
    m_recordWarning->setVisible(true);
    m_recordWarning->setMinimumSize(0, 0);
    m_recordWarning->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    m_recordWarning->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_recordWarning->adjustSize();
    relayoutToolbar();
}

void CaptureOverlay::refreshSoundDevices() {
    if (m_soundDevicesLoading || !m_soundInput) return;
    m_soundDevicesLoading = true;
    auto* process = new QProcess(this);
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this, process](int code, QProcess::ExitStatus status) {
        m_soundDevicesLoading = false;
        const auto object = QJsonDocument::fromJson(process->readAllStandardOutput()).object();
        if (code == 0 && status == QProcess::NormalExit) {
            const auto fill = [](InlineSelect* select, const QJsonArray& devices, bool output) {
                const auto current = select->currentText();
                QStringList names{"default"};
                std::map<QString, QString> labels{{"default", "System default"}};
                if (output) { names.prepend("auto"); labels["auto"] = "Auto"; }
                for (const auto& value : devices) {
                    const auto device = value.toObject();
                    const auto name = device.value("name").toString();
                    if (name.isEmpty() || names.contains(name)) continue;
                    names << name;
                    auto description = device.value("description").toString();
                    labels[name] = description.isEmpty() ? name : description;
                }
                if (!names.contains(current)) { names << current; labels[current] = current + " (unavailable)"; }
                select->setLabels(labels); select->addItems(names); select->setCurrentText(current);
            };
            auto outputs = object.value("outputs").toArray();
            for (const auto& window : object.value("windows").toArray()) outputs.append(window);
            fill(m_soundOutput, outputs, true);
            fill(m_soundInput, object.value("inputs").toArray(), false);
            m_soundOptions->setToolTip({});
        } else m_soundOptions->setToolTip("Audio devices unavailable; video recording remains available");
        process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) { m_soundDevicesLoading = false; process->deleteLater(); }
    });
    QTimer::singleShot(4000, process, [process] { if (process->state() != QProcess::NotRunning) process->kill(); });
    process->start(QCoreApplication::applicationFilePath(), {"--sound-list"});
}

void CaptureOverlay::updateSoundMeter() {
    const bool wanted = isVisible() && m_overlayActive && m_soundMixer && m_soundMixer->isVisible();
    const auto* target = selectedWindow();
    if (!target) target = hoveredWindow();
    const auto source = hyprcapture::audio::resolveOutput(m_defaults.recordAudioOutput, m_mode, target ? target->address.toStdString() : "");
    if (m_soundOutput) {
        const bool windowSource = source.starts_with("window:");
        QString hint;
        if (m_defaults.recordAudioOutput == "auto") hint = windowSource ? (target ? "Auto: " + target->title : "Auto: choose a window") : "Auto: system default output";
        if (windowSource) hint += (hint.isEmpty() ? "" : "\n") + QString("Captures the window application's audio; windows sharing a process may share audio.");
        m_soundOutput->setSelectionHint(hint);
    }
    // Preview both channels independently of which channels will be recorded.
    const QStringList args{"--sound-meter", "mix",
                           qString(source), qString(m_defaults.recordAudioInput)};
    const QString key = args.join(QChar(0x1f));
    if (m_meterProcess && (!wanted || key != m_meterKey)) {
        m_meterProcess->disconnect(this);
        m_meterProcess->kill();
        connect(m_meterProcess, &QProcess::finished, m_meterProcess, &QObject::deleteLater);
        if (m_meterProcess->state() == QProcess::NotRunning) m_meterProcess->deleteLater();
        m_meterProcess = nullptr; m_meterKey.clear();
        m_systemMeter->reset(); m_micMeter->reset();
    }
    if (!wanted) { m_meterKey.clear(); return; }
    if (m_meterProcess || m_meterKey == key) return;
    m_meterKey = key;
    auto* process = new QProcess(this); m_meterProcess = process;
    auto buffer = std::make_shared<QByteArray>();
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process, buffer] {
        buffer->append(process->readAllStandardOutput());
        while (buffer->contains('\n')) {
            const int end = buffer->indexOf('\n');
            const auto object = QJsonDocument::fromJson(buffer->left(end)).object(); buffer->remove(0, end + 1);
            if (object.contains("error")) m_systemMeter->setToolTip(object["error"].toString());
            if (!object.contains("levels")) continue;
            const auto levels = object["levels"].toObject();
            auto apply = [&levels](AudioMeter* meter, const char* role) {
                const auto data = levels[role].toObject();
                meter->setLevels(data["peak"].toDouble(), data["rms"].toDouble(), data["available"].toBool());
            };
            apply(m_systemMeter, "System"); apply(m_micMeter, "Microphone");
        }
        if (buffer->size() > 8192) buffer->clear();
    });
    auto failed = [this, process] {
        if (m_meterProcess == process) { m_meterProcess = nullptr; m_systemMeter->reset(); m_micMeter->reset(); }
        process->deleteLater();
    };
    connect(process, &QProcess::finished, this, [failed](int, QProcess::ExitStatus) { failed(); });
    connect(process, &QProcess::errorOccurred, this, [failed](QProcess::ProcessError error) { if (error == QProcess::FailedToStart) failed(); });
    process->start(QCoreApplication::applicationFilePath(), args);
}

void CaptureOverlay::updateRecordOptionsVisibility() {
    if (!m_recordOptions)
        return;

    const bool visible = m_record && !m_recordActive;
    const bool imageAnimation = visible && isImageAnimationRecordFormat(currentRecordFormat());
    if (imageAnimation && m_recordDuration)
        m_recordDuration->setCurrentText(animationDurationChoice(currentRecordMaxSeconds()));

    const auto updateSelect = [](InlineSelect* select, bool selectVisible) {
        if (select)
            select->setControlVisible(selectVisible);
    };
    updateSelect(m_recordCodec, visible && !imageAnimation);
    updateSelect(m_recordFormat, visible);
    updateSelect(m_recordFps, visible);
    updateSelect(m_recordDuration, imageAnimation);
    updateSelect(m_recordBackend, visible && !imageAnimation);
    updateSelect(m_soundMode, visible);
    updateSelect(m_soundOutput, visible);
    updateSelect(m_soundInput, visible);
    m_soundOutput->setEnabled(!imageAnimation);
    m_soundInput->setEnabled(!imageAnimation);
    m_soundOptions->setVisible(visible);
    updateSelect(m_soundPreset, visible);
    m_soundPreset->setEnabled(!imageAnimation);
    const bool manual = visible && !imageAnimation && m_defaults.recordAudioMix == "manual";
    m_soundMixer->setVisible(manual);
    m_systemGain->setEnabled(true);
    m_micGain->setEnabled(true);
    m_soundMode->setEnabled(!imageAnimation);
    m_soundMode->setToolTip(imageAnimation ? "Animation formats do not support sound" : "Recording sound");
    if (imageAnimation) m_soundMode->setCurrentText("Not supported");
    else m_soundMode->setCurrentText(qString(hyprcapture::toString(m_defaults.recordAudio)));


    m_recordOptions->setVisible(visible);
    m_recordOptions->setSizePolicy(visible ? QSizePolicy::Fixed : QSizePolicy::Ignored, visible ? QSizePolicy::Fixed : QSizePolicy::Ignored);
    if (!visible) {
        m_recordOptions->setFixedSize(0, 0);
    } else {
        m_recordOptions->setMinimumSize(0, 0);
        m_recordOptions->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        m_recordOptions->adjustSize();
    }

    updateRecordWarning();
}

hyprcapture::DecorationPolicy CaptureOverlay::currentWindowBorder() const {
    return m_defaults.windowBorder;
}

hyprcapture::DecorationPolicy CaptureOverlay::currentWindowShadow() const {
    return m_defaults.windowShadow;
}

void CaptureOverlay::paintDesktop(QPainter& painter, const QRect& target) const {
    if (!target.isValid())
        return;

    if (!m_monitorArtifacts.empty()) {
        painter.save();
        painter.setClipRect(target);
        painter.fillRect(target, QColor(30, 34, 38));

        const QRect globalTarget = localToDesktopLogicalRect(target);
        for (const auto& artifact : m_monitorArtifacts) {
            if (artifact.image.isNull() || !artifact.logicalGeometry.isValid())
                continue;

            if (!globalTarget.intersects(artifact.logicalGeometry))
                continue;

            const QRect destination = globalToLocalRect(artifact.logicalGeometry);
            hyprcapture::ui::paintClippedImage(painter, target, destination, artifact.image);
        }
        painter.restore();
        return;
    }

    if (m_desktopImage.isNull()) {
        painter.fillRect(target, QColor(30, 34, 38));
        return;
    }

    const QRect destination = globalToLocalRect(m_desktopGeometry);
    if (destination.isValid())
        hyprcapture::ui::paintClippedImage(painter, target, destination, m_desktopImage);
    else
        painter.fillRect(target, QColor(30, 34, 38));
}

void CaptureOverlay::paintCursorLayers(QPainter& painter, const QRect& outputRect, const QRect& globalRect) const {
    if (!m_defaults.includeCursor || !outputRect.isValid() || !globalRect.isValid())
        return;

    for (const auto& artifact : m_monitorArtifacts) {
        if (artifact.cursorImage.isNull() || !artifact.logicalGeometry.isValid())
            continue;

        const QRect logicalPart = globalRect.intersected(artifact.logicalGeometry);
        const QRect source =
            hyprcapture::ui::mapLogicalRectToPixels(logicalPart, artifact.logicalGeometry, artifact.cursorImage.rect());
        const QRect target = hyprcapture::ui::mapLogicalRectToPixels(logicalPart, globalRect, outputRect);
        if (!source.isValid() || !target.isValid())
            continue;
        hyprcapture::ui::paintImageLayer(painter, target, artifact.cursorImage, source);
    }
}

void CaptureOverlay::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(rect(), Qt::transparent);
    if (!m_overlayActive)
        return;
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setOpacity(m_overlayOpacity);

    paintDesktop(painter, rect());
    painter.fillRect(rect(), QColor(0, 0, 0, 80));

    const bool fusionGesture = m_defaults.fushionMode && m_mode != hyprcapture::CaptureMode::Fullscreen;
    const bool fusionTargetPreview = fusionGesture && !pendingConfirmActive();
    const QRect sel = normalizedSelection().intersected(regionCaptureBounds());
    const bool selectionLargeEnough = sel.width() > 4 && sel.height() > 4;
    const bool selectionVisible = selectionLargeEnough || (m_dragging && !fusionTargetPreview);
    const auto drawFullscreenPreview = [this, &painter](const QRect& cap) {
        if (!cap.isValid())
            return;
        constexpr qreal penWidth = 2.0;
        constexpr qreal inset = penWidth / 2.0;
        auto            radii = fullscreenPreviewCornerRadii(cap);
        radii.topLeft = std::max(0.0, radii.topLeft - inset);
        radii.topRight = std::max(0.0, radii.topRight - inset);
        radii.bottomRight = std::max(0.0, radii.bottomRight - inset);
        radii.bottomLeft = std::max(0.0, radii.bottomLeft - inset);
        painter.setPen(QPen(QColor(255, 255, 255, 230), penWidth));
        painter.setBrush(Qt::NoBrush);
        const QRectF frame = QRectF(cap).adjusted(inset, inset, -inset, -inset);
        painter.drawPath(hyprcapture::ui::screenPreviewPath(frame, radii));
    };
    if ((m_mode == hyprcapture::CaptureMode::Region || fusionGesture) && selectionVisible) {
        paintDesktop(painter, sel);
        painter.setPen(QPen(QColor(255, 255, 255, 230), 2));
        painter.drawRect(sel.adjusted(0, 0, -1, -1));
        if (pendingConfirmActive() && m_mode == hyprcapture::CaptureMode::Region && regionSelectionValid(sel)) {
            painter.setPen(QPen(QColor(30, 34, 38, 220), 1));
            painter.setBrush(QColor(255, 255, 255, 235));
            const int half = kSelectionHandlePaintSize / 2;
            const QPoint points[] = {sel.topLeft(), QPoint(sel.center().x(), sel.top()), sel.topRight(), QPoint(sel.right(), sel.center().y()),
                                     sel.bottomRight(), QPoint(sel.center().x(), sel.bottom()), sel.bottomLeft(), QPoint(sel.left(), sel.center().y())};
            for (const QPoint& point : points)
                painter.drawRect(QRect(point - QPoint(half, half), QSize(kSelectionHandlePaintSize, kSelectionHandlePaintSize)));
        }
    } else if (pendingConfirmActive() && m_mode == hyprcapture::CaptureMode::Fullscreen) {
        const QRect cap = fullscreenCaptureRect();
        if (cap.isValid()) {
            paintDesktop(painter, cap);
            drawFullscreenPreview(cap);
        }
    } else if (fusionTargetPreview && !hoveredWindow()) {
        const QPoint localCursor = globalToLocalRect(QRect(cursorLogicalPosition(), QSize(1, 1))).topLeft();
        const QRect  cap = localScreenRectAt(localCursor);
        drawFullscreenPreview(cap);
    } else if (m_mode == hyprcapture::CaptureMode::Window || fusionGesture) {
        const auto* window = hoveredWindow();
        const auto* selected = selectedWindow();
        for (const auto& candidate : m_windowArtifacts) {
            const QRect outline =
                hyprcapture::ui::selectionOutlineGeometry(candidate.selectionGeometry.isValid() ? candidate.selectionGeometry : windowFrameGeometry(candidate),
                                                          candidate.selectionClipGeometry,
                                                          m_overlayLogicalGeometry);
            if (!outline.isValid())
                continue;
            const QRect target = globalToLocalRect(outline);
            const bool isSelected = &candidate == selected;
            const bool isHovered = &candidate == window;
            const int penWidth = isSelected ? 3 : (isHovered ? 2 : 1);
            QPen pen(isSelected ? QColor(255, 255, 255, 245) : (isHovered ? QColor(255, 255, 255, 210) : QColor(255, 255, 255, 110)), penWidth);
            pen.setJoinStyle(Qt::RoundJoin);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            const QRectF alignedTarget = QRectF(target).adjusted(0.5, 0.5, -0.5, -0.5);
            painter.drawPath(roundedWindowFramePath(alignedTarget, windowFrameRadius(candidate), candidate.roundingPower));
        }
    }
}

void CaptureOverlay::mousePressEvent(QMouseEvent* event) {
    if (!requestActivation())
        return;
    if (m_toolbar->geometry().contains(event->pos()))
        return;
    rememberCursorLocalPosition(event->position());
    hideOptionPopups();
    if (event->button() != Qt::LeftButton)
        return;

    if (pendingConfirmActive()) {
        if (m_mode == hyprcapture::CaptureMode::Region) {
            m_confirmDragMode = confirmRegionDragModeAt(event->pos());
            setCursor(cursorForConfirmDragMode(m_confirmDragMode));
            m_confirmDragStart = clampedToRect(event->pos(), regionCaptureBounds());
            m_confirmDragStartSelection = normalizedSelection().intersected(regionCaptureBounds());
            m_dragging = true;
            if (m_confirmDragMode == ConfirmDragMode::NewSelection) {
                m_dragStart = m_confirmDragStart;
                m_dragEnd = m_confirmDragStart;
            }
            updateStatus();
            update();
            return;
        }

        if (m_mode == hyprcapture::CaptureMode::Window) {
            m_confirmDragMode = ConfirmDragMode::NewSelection;
            m_confirmDragStart = clampedToRect(event->pos(), regionCaptureBounds());
            m_confirmDragStartSelection = QRect{};
            m_dragStart = m_confirmDragStart;
            m_dragEnd = m_confirmDragStart;
            m_dragging = true;
            setCursor(Qt::CrossCursor);
            updateStatus();
            update();
            return;
        }

        return;
    }

    if (m_defaults.fushionMode && m_mode != hyprcapture::CaptureMode::Fullscreen) {
        if (m_record)
            m_recordError.clear();
        m_mode = hyprcapture::CaptureMode::Region;
        m_dragging = true;
        m_dragStart = event->pos();
        m_dragEnd = clampedToRect(event->pos(), regionCaptureBounds());
        updateStatus();
        update();
        return;
    }

    if (m_mode == hyprcapture::CaptureMode::Fullscreen) {
        if (confirmBeforeCaptureEnabled())
            beginPendingConfirm(hyprcapture::CaptureMode::Fullscreen);
        else
            finishCapture();
        return;
    }

    if (m_mode == hyprcapture::CaptureMode::Window)
        return;

    m_dragging = true;
    if (m_record)
        m_recordError.clear();
    m_dragStart = event->pos();
    m_dragEnd = clampedToRect(event->pos(), regionCaptureBounds());
    update();
}

void CaptureOverlay::mouseMoveEvent(QMouseEvent* event) {
    if (!requestActivation())
        return;
    rememberCursorLocalPosition(event->position());
    if (pendingConfirmActive()) {
        if (m_mode == hyprcapture::CaptureMode::Region) {
            updateConfirmCursor(event->pos());
            if (m_dragging)
                setSelectionRect(regionSelectionForDrag(event->pos()));
            updateStatus();
            update();
            return;
        }
        if (m_mode == hyprcapture::CaptureMode::Window) {
            if (m_dragging) {
                m_dragEnd = clampedToRect(event->pos(), regionCaptureBounds());
                if (regionSelectionValid(normalizedSelection())) {
                    m_mode = hyprcapture::CaptureMode::Region;
                    m_selectedWindowIndex = -1;
                    resetWindowCycle();
                    updateToolbarControlsForMode();
                    updateConfirmCursor(event->pos());
                }
            }
            updateStatus();
            update();
            return;
        }
    }

    if (m_defaults.fushionMode && m_mode != hyprcapture::CaptureMode::Fullscreen) {
        if (m_dragging)
            m_dragEnd = clampedToRect(event->pos(), regionCaptureBounds());
        updateStatus();
        update();
        return;
    }

    if (m_mode == hyprcapture::CaptureMode::Window) {
        updateStatus();
        update();
        return;
    }
    if (!m_dragging)
        return;
    m_dragEnd = clampedToRect(event->pos(), regionCaptureBounds());
    update();
}

void CaptureOverlay::mouseReleaseEvent(QMouseEvent* event) {
    if (!requestActivation())
        return;
    rememberCursorLocalPosition(event->position());
    if (event->button() != Qt::LeftButton)
        return;

    if (pendingConfirmActive()) {
        if (m_mode == hyprcapture::CaptureMode::Region && m_dragging) {
            m_dragging = false;
            setSelectionRect(regionSelectionForDrag(event->pos()));
            m_confirmDragMode = ConfirmDragMode::None;
            if (!regionSelectionValid(normalizedSelection()))
                m_pendingConfirm = false;
            updateConfirmCursor(event->pos());
            updateStatus();
            update();
            return;
        }

        if (m_mode == hyprcapture::CaptureMode::Window) {
            if (m_dragging) {
                m_dragging = false;
                m_dragEnd = clampedToRect(event->pos(), regionCaptureBounds());
                if (regionSelectionValid(normalizedSelection())) {
                    m_mode = hyprcapture::CaptureMode::Region;
                    m_confirmDragMode = ConfirmDragMode::None;
                    m_selectedWindowIndex = -1;
                    resetWindowCycle();
                    updateToolbarControlsForMode();
                    updateConfirmCursor(event->pos());
                    updateStatus();
                    update();
                    return;
                }
                m_confirmDragMode = ConfirmDragMode::None;
            }

            const int index = rawHoveredWindowIndex();
            resetWindowCycle();
            if (index >= 0)
                m_selectedWindowIndex = index;
            if (selectedWindowUsesOverviewSelection()) {
                m_pendingConfirm = false;
                finishCapture();
                return;
            }
            updateStatus();
            update();
            return;
        }

        return;
    }

    if (m_defaults.fushionMode && m_mode != hyprcapture::CaptureMode::Fullscreen) {
        if (!m_dragging)
            return;

        m_dragging = false;
        m_dragEnd = clampedToRect(event->pos(), regionCaptureBounds());
        const QRect selection = normalizedSelection().intersected(regionCaptureBounds());
        if (selection.width() > 4 && selection.height() > 4) {
            m_mode = hyprcapture::CaptureMode::Region;
            resetWindowCycle(true);
            if (confirmBeforeCaptureEnabled())
                beginPendingConfirm(hyprcapture::CaptureMode::Region);
            else
                finishCapture();
            return;
        }

        const int windowIndex = hoveredWindowIndex();
        if (windowIndex >= 0) {
            m_mode = hyprcapture::CaptureMode::Window;
            m_selectedWindowIndex = windowIndex;
            if (m_defaults.captureFullscreenClientsAsMonitor && m_windowArtifacts[static_cast<std::size_t>(windowIndex)].fullscreen) {
                m_fullscreenClientSelected = true;
                m_mode = hyprcapture::CaptureMode::Fullscreen;
                if (m_fullscreenScope)
                    m_fullscreenScope->setCurrentText(QStringLiteral("current"));
                if (confirmBeforeCaptureEnabled())
                    beginPendingConfirm(hyprcapture::CaptureMode::Fullscreen);
                else
                    finishCapture();
                return;
            }
            if (confirmBeforeCaptureEnabled() && !selectedWindowUsesOverviewSelection())
                beginPendingConfirm(hyprcapture::CaptureMode::Window);
            else
                finishCapture();
            return;
        }

        m_fullscreenClientSelected = false;
        m_selectedWindowIndex = -1;
        m_mode = hyprcapture::CaptureMode::Fullscreen;
        if (m_fullscreenScope)
            m_fullscreenScope->setCurrentText(QStringLiteral("current"));
        if (confirmBeforeCaptureEnabled())
            beginPendingConfirm(hyprcapture::CaptureMode::Fullscreen);
        else
            finishCapture();
        return;
    }

    if (m_mode == hyprcapture::CaptureMode::Window) {
        const int windowIndex = hoveredWindowIndex();
        if (windowIndex >= 0)
            m_selectedWindowIndex = windowIndex;
        if (windowIndex >= 0 && m_defaults.captureFullscreenClientsAsMonitor &&
            m_windowArtifacts[static_cast<std::size_t>(windowIndex)].fullscreen) {
            m_fullscreenClientSelected = true;
            m_mode = hyprcapture::CaptureMode::Fullscreen;
            if (m_fullscreenScope)
                m_fullscreenScope->setCurrentText(QStringLiteral("current"));
            if (confirmBeforeCaptureEnabled())
                beginPendingConfirm(hyprcapture::CaptureMode::Fullscreen);
            else
                finishCapture();
            return;
        }
        if (confirmBeforeCaptureEnabled() && !selectedWindowUsesOverviewSelection()) {
            if (m_selectedWindowIndex >= 0)
                beginPendingConfirm(hyprcapture::CaptureMode::Window);
        } else {
            finishCapture();
        }
        return;
    }
    m_dragging = false;
    m_dragEnd = clampedToRect(event->pos(), regionCaptureBounds());
    const QRect selection = normalizedSelection().intersected(regionCaptureBounds());
    if (m_mode != hyprcapture::CaptureMode::Region || (selection.width() > 4 && selection.height() > 4)) {
        if (confirmBeforeCaptureEnabled())
            beginPendingConfirm(m_mode);
        else
            finishCapture();
    }
}

void CaptureOverlay::wheelEvent(QWheelEvent* event) {
    if (!requestActivation()) {
        event->accept();
        return;
    }

    const QPoint localPosition = event->position().toPoint();
    if (m_toolbar && m_toolbar->geometry().contains(localPosition)) {
        QMainWindow::wheelEvent(event);
        return;
    }

    rememberCursorLocalPosition(event->position());
    if (!windowWheelSelectionEnabled()) {
        QMainWindow::wheelEvent(event);
        return;
    }

    const QPoint angleDelta = event->angleDelta();
    const QPoint pixelDelta = event->pixelDelta();
    const QPoint effectiveDelta = angleDelta.y() != 0 ? angleDelta : pixelDelta;
    if (effectiveDelta.y() == 0 || std::abs(effectiveDelta.x()) > std::abs(effectiveDelta.y())) {
        QMainWindow::wheelEvent(event);
        return;
    }

    const auto candidates = currentMonitorWindowCandidates();
    if (candidates.size() < 2) {
        resetWindowCycle(m_windowCycleActive);
        QMainWindow::wheelEvent(event);
        return;
    }

    if (candidates != m_windowCycleCandidates) {
        const bool clearSelectedWindow = m_windowCycleActive;
        resetWindowCycle(clearSelectedWindow);
        m_windowCycleCandidates = candidates;
    }
    if (event->phase() == Qt::ScrollBegin)
        m_windowWheelStepState = {};

    const int steps = hyprcapture::ui::consumeWindowWheelSteps(angleDelta, pixelDelta, event->inverted(), m_windowWheelStepState);
    event->accept();
    if (steps == 0)
        return;

    if (!m_windowCycleActive) {
        int focusedWindowIndex = -1;
        for (const int candidate : m_windowCycleCandidates) {
            if (candidate >= 0 && candidate < static_cast<int>(m_windowArtifacts.size()) &&
                m_windowArtifacts[static_cast<std::size_t>(candidate)].focused) {
                focusedWindowIndex = candidate;
                break;
            }
        }
        m_windowCyclePosition =
            hyprcapture::ui::windowSelectionStartPosition(m_windowCycleCandidates, focusedWindowIndex);
        m_windowCycleActive = m_windowCyclePosition >= 0;
    }

    m_windowCyclePosition = hyprcapture::ui::stepWindowSelectionPosition(
        m_windowCyclePosition, steps, static_cast<int>(m_windowCycleCandidates.size()));
    const int targetIndex = windowCycleTargetIndex();
    if (targetIndex < 0) {
        resetWindowCycle(true);
        updateStatus();
        update();
        return;
    }

    m_selectedWindowIndex = targetIndex;
    updateStatus();
    update();
}

void CaptureOverlay::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        cancelCapture();
    } else if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (pendingConfirmActive()) {
            confirmPendingCapture();
            return;
        }

        if (m_defaults.fushionMode && m_mode != hyprcapture::CaptureMode::Fullscreen) {
            const QRect selection = normalizedSelection().intersected(regionCaptureBounds());
            if (selection.width() > 4 && selection.height() > 4) {
                m_mode = hyprcapture::CaptureMode::Region;
                if (confirmBeforeCaptureEnabled()) {
                    beginPendingConfirm(hyprcapture::CaptureMode::Region);
                    return;
                }
            } else if (const int windowIndex = hoveredWindowIndex(); windowIndex >= 0) {
                m_mode = hyprcapture::CaptureMode::Window;
                m_selectedWindowIndex = windowIndex;
                if (confirmBeforeCaptureEnabled()) {
                    beginPendingConfirm(hyprcapture::CaptureMode::Window);
                    return;
                }
            } else {
                return;
            }
        } else if (confirmBeforeCaptureEnabled()) {
            if (m_mode == hyprcapture::CaptureMode::Window) {
                const int windowIndex = hoveredWindowIndex();
                if (windowIndex < 0)
                    return;
                m_selectedWindowIndex = windowIndex;
            } else if (m_mode == hyprcapture::CaptureMode::Region && !regionSelectionValid(normalizedSelection())) {
                return;
            }
            beginPendingConfirm(m_mode);
            return;
        }
        finishCapture();
    }
}

void CaptureOverlay::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    relayoutToolbar();
}

QRect CaptureOverlay::normalizedSelection() const {
    return QRect(m_dragStart, m_dragEnd).normalized();
}

QRect CaptureOverlay::captureRectForMode() const {
    if (m_mode == hyprcapture::CaptureMode::Region && normalizedSelection().isValid())
        return normalizedSelection().intersected(regionCaptureBounds());
    if (m_mode == hyprcapture::CaptureMode::Window) {
        const auto* window = selectedWindow() ? selectedWindow() : hoveredWindow();
        if (window)
            return globalToLocalRect(windowFrameGeometry(*window));
        return {};
    }
    return fullscreenCaptureRect();
}

QRect CaptureOverlay::fullscreenCaptureRect() const {
    if (currentFullscreenScope() == hyprcapture::FullscreenScope::Current)
        return localScreenRectAt(globalToLocalRect(QRect(cursorLogicalPosition(), QSize(1, 1))).topLeft());
    if (m_desktopGeometry.isValid())
        return globalToLocalRect(m_desktopGeometry);
    return rect();
}

hyprcapture::ui::ScreenCornerRadii CaptureOverlay::fullscreenPreviewCornerRadii(const QRect& localRect) const {
    const QString configured = QString::fromStdString(m_defaults.fullscreenPreviewRounding).trimmed();
    if (configured.compare(QStringLiteral("auto"), Qt::CaseInsensitive) != 0) {
        bool ok = false;
        const double radius = configured.toDouble(&ok);
        if (ok && std::isfinite(radius)) {
            const double bounded = std::clamp(radius, 0.0, 512.0);
            return {.topLeft = bounded, .topRight = bounded, .bottomRight = bounded, .bottomLeft = bounded};
        }
    }

    const QRect globalRect = localToDesktopLogicalRect(localRect);
    const auto artifact = std::ranges::find_if(m_monitorArtifacts, [&globalRect](const MonitorArtifact& candidate) {
        return candidate.logicalGeometry == globalRect;
    });
    return artifact == m_monitorArtifacts.end() ? hyprcapture::ui::ScreenCornerRadii{} : artifact->previewCornerRadii;
}

QRect CaptureOverlay::regionCaptureBounds() const {
    return rect();
}

QRect CaptureOverlay::localScreenRectAt(const QPoint& localPos) const {
    if (!m_monitorArtifacts.empty()) {
        const QPoint logicalPoint = localToDesktopLogicalPoint(localPos);
        for (const auto& artifact : m_monitorArtifacts) {
            if (artifact.logicalGeometry.contains(logicalPoint))
                return globalToLocalRect(artifact.logicalGeometry).intersected(rect());
        }
    }

    QScreen* screen = QGuiApplication::screenAt(mapToGlobal(localPos));
    if (!screen)
        screen = QGuiApplication::screenAt(cursorLogicalPosition());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    return screen ? globalToLocalRect(screen->geometry()).intersected(rect()) : rect();
}

QPoint CaptureOverlay::clampedToRect(const QPoint& point, const QRect& bounds) const {
    if (!bounds.isValid())
        return point;
    return QPoint(std::clamp(point.x(), bounds.left(), bounds.right()), std::clamp(point.y(), bounds.top(), bounds.bottom()));
}

QRect CaptureOverlay::globalToLocalRect(const QRect& rect) const {
    if (m_overlayLogicalGeometry.isValid())
        return QRect(rect.topLeft() - m_overlayLogicalGeometry.topLeft(), rect.size());
    return QRect(mapFromGlobal(rect.topLeft()), rect.size());
}

QRect CaptureOverlay::localToDesktopLogicalRect(const QRect& rect) const {
    if (m_overlayLogicalGeometry.isValid())
        return QRect(m_overlayLogicalGeometry.topLeft() + rect.topLeft(), rect.size());
    return QRect(QPoint(mapToGlobal(rect.topLeft())), rect.size());
}

QPoint CaptureOverlay::localToDesktopLogicalPoint(const QPoint& point) const {
    if (m_overlayLogicalGeometry.isValid())
        return m_overlayLogicalGeometry.topLeft() + point;
    return mapToGlobal(point);
}

QRect CaptureOverlay::desktopSourceRectForGlobalRect(const QRect& rect) const {
    if (m_desktopImage.isNull() || !m_desktopGeometry.isValid() || !rect.isValid())
        return {};

    const QRect clipped = rect.intersected(m_desktopGeometry);
    if (!clipped.isValid())
        return {};

    const double scaleX = static_cast<double>(m_desktopImage.width()) / std::max(1, m_desktopGeometry.width());
    const double scaleY = static_cast<double>(m_desktopImage.height()) / std::max(1, m_desktopGeometry.height());
    return logicalRectToOutputRect(clipped, m_desktopGeometry, scaleX, scaleY).intersected(m_desktopImage.rect());
}

QRect CaptureOverlay::localToDesktopSourceRect(const QRect& rect) const {
    return desktopSourceRectForGlobalRect(localToDesktopLogicalRect(rect));
}

QPoint CaptureOverlay::cursorLogicalPosition() const {
    if (m_hasCursorLogicalPosition)
        return m_cursorLogicalPosition;
    const QPoint local = mapFromGlobal(QCursor::pos());
    if (rect().contains(local))
        return localToDesktopLogicalPoint(local);
    return QCursor::pos();
}

void CaptureOverlay::rememberCursorPosition(const QPointF& globalPosition) {
    int x = 0;
    int y = 0;
    if (!boundedDoubleToInt(globalPosition.x(), -kMaxLogicalCoordinate, kMaxLogicalCoordinate, false, x) ||
        !boundedDoubleToInt(globalPosition.y(), -kMaxLogicalCoordinate, kMaxLogicalCoordinate, false, y))
        return;

    m_cursorLogicalPosition = QPoint(x, y);
    m_hasCursorLogicalPosition = true;
}

void CaptureOverlay::rememberCursorLocalPosition(const QPointF& localPosition) {
    rememberCursorPosition(localToDesktopLogicalPoint(QPoint(static_cast<int>(std::floor(localPosition.x())), static_cast<int>(std::floor(localPosition.y())))));
}

void CaptureOverlay::refreshInitialCursorPosition() {
    if (!m_hasCursorLogicalPosition) {
        const QPoint local = mapFromGlobal(QCursor::pos());
        if (rect().contains(local))
            rememberCursorLocalPosition(local);
        else
            rememberCursorPosition(QCursor::pos());
    }
    updateStatus();
    update();
}

QRect CaptureOverlay::windowFrameGeometry(const WindowArtifact& window) const {
    QRect frame = window.visibleGeometry.isValid() ? window.visibleGeometry : window.fullGeometry;
    const int border = std::max(0, static_cast<int>(std::lround(window.borderSize)));
    if (border > 0)
        frame = frame.adjusted(-border, -border, border, border);
    return frame;
}

QRect CaptureOverlay::windowSelectionGeometry(const WindowArtifact& window) const {
    const QRect selection = window.selectionGeometry.isValid() ? window.selectionGeometry : windowFrameGeometry(window);
    return hyprcapture::ui::clippedSelectionGeometry(selection, window.selectionClipGeometry);
}

bool CaptureOverlay::hasOverviewSelectionGeometry(const WindowArtifact& window) const {
    return window.selectionGeometry.isValid() && window.selectionGeometry != windowFrameGeometry(window);
}

bool CaptureOverlay::selectedWindowUsesOverviewSelection() const {
    const auto* window = selectedWindow();
    return window && hasOverviewSelectionGeometry(*window);
}

void CaptureOverlay::beginHymissionCaptureInputSuppression() {
    if (!m_hymissionOverviewSession || m_hymissionCaptureInputSuppressed || !m_hymissionCaptureInputToken.isEmpty())
        return;

    m_hymissionCaptureInputToken =
        QStringLiteral("hyprcapture-%1-%2").arg(QCoreApplication::applicationPid()).arg(QDateTime::currentMSecsSinceEpoch());
    const auto result = runHymissionCaptureInputCommand(QStringLiteral("begin"), m_hymissionCaptureInputToken);
    if (result.success) {
        m_hymissionCaptureInputSuppressed = true;
        return;
    }

    m_hymissionCaptureInputToken.clear();
}

void CaptureOverlay::endHymissionCaptureInputSuppression() {
    if (!m_hymissionCaptureInputSuppressed || m_hymissionCaptureInputToken.isEmpty())
        return;

    const QString token = m_hymissionCaptureInputToken;
    m_hymissionCaptureInputSuppressed = false;
    m_hymissionCaptureInputToken.clear();
    (void)runHymissionCaptureInputCommand(QStringLiteral("end"), token);
}

double CaptureOverlay::windowFrameRadius(const WindowArtifact& window) const {
    if (window.rounding <= 0.0)
        return 0.0;

    const double power = std::clamp(window.roundingPower, 1.0, 10.0);
    const double border = std::max(0.0, window.borderSize);
    const double correction = border * (std::sqrt(2.0) - 1.0) * std::max(2.0 - power, 0.0);
    return std::max(0.0, window.rounding + border - correction);
}

int CaptureOverlay::rawHoveredWindowIndex() const {
    const QPoint global = cursorLogicalPosition();
    for (const int candidate : currentMonitorWindowCandidates()) {
        if (candidate >= 0 && candidate < static_cast<int>(m_windowArtifacts.size()) &&
            windowSelectionGeometry(m_windowArtifacts[static_cast<std::size_t>(candidate)]).contains(global))
            return candidate;
    }
    return -1;
}

int CaptureOverlay::hoveredWindowIndex() const {
    const int cycled = windowCycleTargetIndex();
    return cycled >= 0 ? cycled : rawHoveredWindowIndex();
}

const CaptureOverlay::WindowArtifact* CaptureOverlay::hoveredWindow() const {
    const int index = hoveredWindowIndex();
    if (index < 0)
        return nullptr;
    return &m_windowArtifacts[static_cast<std::size_t>(index)];
}

CaptureOverlay::WindowArtifact* CaptureOverlay::hoveredWindow() {
    return const_cast<WindowArtifact*>(std::as_const(*this).hoveredWindow());
}

const CaptureOverlay::WindowArtifact* CaptureOverlay::selectedWindow() const {
    if (m_selectedWindowIndex < 0 || m_selectedWindowIndex >= static_cast<int>(m_windowArtifacts.size()))
        return nullptr;
    return &m_windowArtifacts[static_cast<std::size_t>(m_selectedWindowIndex)];
}

CaptureOverlay::WindowArtifact* CaptureOverlay::selectedWindow() {
    return const_cast<WindowArtifact*>(std::as_const(*this).selectedWindow());
}

const CaptureOverlay::WindowArtifact* CaptureOverlay::filenameWindow() const {
    if (const auto* selected = selectedWindow()) {
        if (m_mode == hyprcapture::CaptureMode::Window || (m_mode == hyprcapture::CaptureMode::Fullscreen && m_fullscreenClientSelected))
            return selected;
    }
    const auto focused = std::ranges::find_if(m_windowArtifacts, [](const WindowArtifact& window) { return window.focused; });
    return focused == m_windowArtifacts.end() ? nullptr : &*focused;
}

hyprcapture::FilenameMetadata CaptureOverlay::resolvedFilenameMetadata() const {
    const auto metadataForWindow = [](const WindowArtifact* window) {
        return window ? hyprcapture::FilenameMetadata{.windowClass = window->appClass.toStdString(), .windowTitle = window->title.toStdString()}
                      : hyprcapture::FilenameMetadata{};
    };

    const auto* selected = selectedWindow();
    if (!selected && m_mode == hyprcapture::CaptureMode::Window)
        selected = hoveredWindow();

    std::size_t fullscreenWindowCount = 0;
    hyprcapture::FilenameMetadata singleFullscreenWindow;
    bool fullscreenMetadataComplete = true;
    const QRect fullscreenGeometry =
        m_mode == hyprcapture::CaptureMode::Fullscreen ? localToDesktopLogicalRect(fullscreenCaptureRect()) : QRect{};
    if (m_mode == hyprcapture::CaptureMode::Fullscreen) {
        bool matchedMonitor = false;
        for (const auto& monitor : m_monitorArtifacts) {
            if (!monitor.logicalGeometry.intersects(fullscreenGeometry))
                continue;
            matchedMonitor = true;
            if (!monitor.workspaceWindowCount) {
                fullscreenMetadataComplete = false;
                break;
            }
            fullscreenWindowCount += static_cast<std::size_t>(*monitor.workspaceWindowCount);
            if (*monitor.workspaceWindowCount == 1) {
                singleFullscreenWindow = {
                    .windowClass = monitor.singleWorkspaceWindowClass.toStdString(),
                    .windowTitle = monitor.singleWorkspaceWindowTitle.toStdString(),
                };
            }
        }
        fullscreenMetadataComplete = fullscreenMetadataComplete && matchedMonitor;
    }

    if (!fullscreenMetadataComplete) {
        fullscreenWindowCount = 0;
        singleFullscreenWindow = {};
    }

    return hyprcapture::resolveFilenameMetadata(m_defaults.dynamicWindowMetadata,
                                                m_mode,
                                                metadataForWindow(selected),
                                                metadataForWindow(filenameWindow()),
                                                fullscreenWindowCount,
                                                singleFullscreenWindow);
}

bool CaptureOverlay::hydrateWindowArtifact(WindowArtifact& window) {
    if (!window.image.isNull())
        return true;
    if (window.address.isEmpty() || !window.visibleGeometry.isValid())
        return false;

    hyprcapture::RecordingRequest request;
    request.id = hyprcapture::makeSessionId();
    request.defaults = m_defaults;
    request.defaults.mode = hyprcapture::CaptureMode::Window;
    request.defaults.fullscreenScope = currentFullscreenScope();
    request.defaults.windowBackground = currentWindowBackground();
    request.defaults.windowBorder = currentWindowBorder();
    request.defaults.windowShadow = currentWindowShadow();
    request.mode = hyprcapture::CaptureMode::Window;
    request.windowAddress = window.address.toStdString();

    const QRect globalRect = windowFrameGeometry(window);
    if (!globalRect.isValid())
        return false;
    request.targetGeometry = {.x = static_cast<double>(globalRect.x()),
                              .y = static_cast<double>(globalRect.y()),
                              .width = static_cast<double>(globalRect.width()),
                              .height = static_cast<double>(globalRect.height())};

    const QString requestPath = hyprcapture::ui::runtimeFile(QStringLiteral("window-capture-request"), QStringLiteral(".json"));
    const std::string json = hyprcapture::encodeRecordingRequestJson(request);
    if (!writePrivateTextFile(requestPath, QByteArray(json.data(), static_cast<qsizetype>(json.size()))))
        return false;

    const auto captured = dispatchWindowCapture(requestPath);
    if (!captured.success) {
        QFile::remove(requestPath);
        return false;
    }

    QFile response(requestPath);
    if (!hyprcapture::ui::isPrivateRuntimeFile(requestPath, kMaxSessionJsonBytes) || !response.open(QIODevice::ReadOnly)) {
        QFile::remove(requestPath);
        return false;
    }
    const QByteArray responseBytes = response.readAll();
    response.close();
    QFile::remove(requestPath);

    const auto decoded =
        hyprcapture::decodeSessionJson(std::string(responseBytes.constData(), static_cast<std::size_t>(responseBytes.size())));
    if (!decoded)
        return false;

    QStringList artifactFiles;
    qint64 remainingArtifactBytes = kMaxSessionArtifactBytes;
    for (const auto& info : decoded->windows) {
        if (qString(info.address) != window.address)
            continue;

        WindowArtifact capturedWindow;
        capturedWindow.address = qString(info.address);
        capturedWindow.title = qString(info.title);
        capturedWindow.appClass = qString(info.appClass);
        capturedWindow.zIndex = window.zIndex;
        capturedWindow.focused = info.focused;
        capturedWindow.fullscreen = info.fullscreen;
        capturedWindow.visibleGeometry = protocolRect(info.visibleGeometry);
        capturedWindow.fullGeometry = protocolRect(info.fullGeometry);
        capturedWindow.selectionGeometry = window.selectionGeometry;
        capturedWindow.selectionClipGeometry = window.selectionClipGeometry;
        capturedWindow.rounding = info.rounding;
        capturedWindow.roundingPower = info.roundingPower;
        capturedWindow.borderSize = info.borderSize;

        const QString artifactPath = qString(info.artifactPath);
        const QString realBackgroundPath = qString(info.realBackgroundPath);
        capturedWindow.image = loadRawRgba(artifactPath, info.artifactWidth, info.artifactHeight, info.artifactTopDown, remainingArtifactBytes);
        capturedWindow.realBackground =
            loadRawRgba(realBackgroundPath, info.realBackgroundWidth, info.realBackgroundHeight, info.realBackgroundTopDown, remainingArtifactBytes);
        artifactFiles.push_back(artifactPath);
        artifactFiles.push_back(realBackgroundPath);
        cleanupArtifactFiles(artifactFiles);

        if (capturedWindow.image.isNull() || !capturedWindow.fullGeometry.isValid())
            return false;

        window = std::move(capturedWindow);
        return true;
    }

    cleanupArtifactFiles(artifactFiles);
    return false;
}

bool CaptureOverlay::windowCaptureAvailable() const {
    return !m_windowArtifacts.empty();
}

bool CaptureOverlay::windowWheelSelectionEnabled() const {
    if (!m_defaults.windowWheelScroll)
        return false;
    if (pendingConfirmActive())
        return m_mode == hyprcapture::CaptureMode::Window;
    return m_mode == hyprcapture::CaptureMode::Window ||
        (m_defaults.fushionMode && m_mode != hyprcapture::CaptureMode::Fullscreen);
}

std::vector<int> CaptureOverlay::currentMonitorWindowCandidates() const {
    std::vector<hyprcapture::ui::WindowSelectionCandidate> windows;
    windows.reserve(m_windowArtifacts.size());
    for (std::size_t i = 0; i < m_windowArtifacts.size(); ++i) {
        const auto& window = m_windowArtifacts[i];
        windows.push_back({
            .index = static_cast<int>(i),
            .geometry = windowSelectionGeometry(window),
            .zIndex = window.zIndex,
        });
    }
    const std::optional<QPoint> cursor = m_defaults.windowWheelScope == hyprcapture::WindowWheelScope::UnderCursor ?
        std::optional<QPoint>{cursorLogicalPosition()} : std::nullopt;
    return hyprcapture::ui::orderedWindowSelectionCandidates(windows, m_overlayLogicalGeometry, cursor);
}

int CaptureOverlay::windowCycleTargetIndex() const {
    if (!m_windowCycleActive || m_windowCyclePosition < 0 ||
        m_windowCyclePosition >= static_cast<int>(m_windowCycleCandidates.size()))
        return -1;
    const int index = m_windowCycleCandidates[static_cast<std::size_t>(m_windowCyclePosition)];
    return index >= 0 && index < static_cast<int>(m_windowArtifacts.size()) ? index : -1;
}

void CaptureOverlay::resetWindowCycle(bool clearSelectedWindow) {
    if (clearSelectedWindow)
        m_selectedWindowIndex = -1;
    m_windowCycleActive = false;
    m_windowCyclePosition = -1;
    m_windowCycleCandidates.clear();
    m_windowWheelStepState = {};
}

QString CaptureOverlay::windowCycleStatusText() const {
    const int index = windowCycleTargetIndex();
    if (!m_status || index < 0 || m_windowCycleCandidates.size() < 2)
        return {};

    const auto& window = m_windowArtifacts[static_cast<std::size_t>(index)];
    QString title = window.title.trimmed();
    if (title.isEmpty())
        title = window.appClass.trimmed();
    if (title.isEmpty())
        title = QStringLiteral("unknown");

    const QString prefix = QStringLiteral("%1/%2 · ").arg(m_windowCyclePosition + 1).arg(m_windowCycleCandidates.size());
    const int availableWidth = std::clamp(width() / 3, 120, 320);
    const int titleWidth = std::max(60, availableWidth - m_status->fontMetrics().horizontalAdvance(prefix));
    return prefix + m_status->fontMetrics().elidedText(title, Qt::ElideRight, titleWidth);
}

void CaptureOverlay::updateStatus() {
    if (!m_status)
        return;

    const auto setStatusText = [this](const QString& text) {
        if (text.isEmpty()) {
            m_status->clear();
            m_status->hide();
            m_status->setFixedSize(0, 0);
            m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
            return;
        }

        m_status->setVisible(true);
        m_status->setMinimumSize(0, 0);
        m_status->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        m_status->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        m_status->setText(text);
        m_status->adjustSize();
    };
    const auto setMissingWindowCaptureStatus = [&] {
        if (!m_sessionDecoded)
            setStatusText("open via hyprcapture dispatcher");
        else if (m_sessionMonitorCount == 0 && m_sessionWindowCount == 0)
            setStatusText("plugin reload needed");
        else if (m_sessionWindowCount > 0)
            setStatusText("window artifact failed");
        else
            setStatusText("no visible windows");
    };

    if (m_recordActive) {
        setStatusText(QString{});
        relayoutToolbar();
        return;
    }

    if (!m_recordError.isEmpty()) {
        setStatusText(m_recordError);
        relayoutToolbar();
        return;
    }

    if (pendingConfirmActive()) {
        if (m_mode == hyprcapture::CaptureMode::Window && !windowCaptureAvailable()) {
            setMissingWindowCaptureStatus();
        } else if (m_mode == hyprcapture::CaptureMode::Window && !selectedWindow()) {
            setStatusText("choose window");
        } else if (m_mode == hyprcapture::CaptureMode::Region && !regionSelectionValid(normalizedSelection())) {
            setStatusText("choose area");
        } else if (const QString cycleStatus = windowCycleStatusText(); !cycleStatus.isEmpty()) {
            setStatusText(cycleStatus);
        } else {
            setStatusText(QString{});
        }
        updateConfirmButtonVisibility();
        relayoutToolbar();
        return;
    }

    updateConfirmButtonVisibility();

    if (const QString cycleStatus = windowCycleStatusText(); !cycleStatus.isEmpty()) {
        setStatusText(cycleStatus);
        relayoutToolbar();
        return;
    }

    if (m_mode != hyprcapture::CaptureMode::Window) {
        setStatusText(QString{});
        relayoutToolbar();
        return;
    }

    if (!windowCaptureAvailable()) {
        setMissingWindowCaptureStatus();
        relayoutToolbar();
        return;
    }

    if (selectedWindow() || hoveredWindow())
        setStatusText(QString{});
    else
        setStatusText("choose window");
    relayoutToolbar();
}

void CaptureOverlay::relayoutToolbar() {
    if (!m_toolbar)
        return;

    if (m_soundOptions && m_soundOutput && m_soundInput) {
        const int count = (m_soundOutput->isVisible() ? 1 : 0) + (m_soundInput->isVisible() ? 1 : 0);
        const int deviceWidth = std::clamp((width() - 64 - 220) / std::max(1, count), 45, 210);
        m_soundMode->setPrefix(width() < 500 ? "" : "Sound");
        m_soundOutput->setPrefix(width() < 500 ? "" : "Source");
        m_soundInput->setPrefix(width() < 500 ? "" : "Mic");
        m_soundMode->setCompactWidth(width() < 500 ? 75 : 155);
        m_soundPreset->setCompactWidth(width() < 500 ? 85 : 130);
        if (m_soundMixer) m_soundMixer->setFixedWidth(std::min(550, std::max(1, width() - 52)));
        m_soundOutput->setCompactWidth(deviceWidth);
        m_soundInput->setCompactWidth(deviceWidth);
        m_soundOptions->setFixedWidth(std::min(m_soundOptions->sizeHint().width(), std::max(1, width() - 52)));
    }
    m_toolbar->setMinimumWidth(0);
    m_toolbar->setMaximumWidth(QWIDGETSIZE_MAX);
    m_toolbar->adjustSize();
    const int maxWidth = std::max(1, width() - 32);
    if (m_toolbar->width() > maxWidth)
        m_toolbar->setFixedWidth(maxWidth);
    else
        m_toolbar->setFixedWidth(m_toolbar->sizeHint().width());
    const int y = std::max(16, height() - m_toolbar->height() - 40);
    m_toolbar->move(std::max(16, (width() - m_toolbar->width()) / 2), y);
}

QImage CaptureOverlay::renderDesktopRectAtDisplayResolution(const QRect& globalRect) const {
    if (!globalRect.isValid() || m_monitorArtifacts.empty())
        return {};

    struct DrawPart {
        const MonitorArtifact* artifact = nullptr;
        QRect                  logical;
        QRect                  source;
        QRect                  target;
    };

    std::vector<DrawPart> drawParts;
    QRect                 outputBounds;
    const auto artifactAxisScale = [](const MonitorArtifact& artifact, bool horizontal) {
        if (artifact.image.isNull() || !artifact.logicalGeometry.isValid())
            return 0.0;
        const int logicalSize = horizontal ? artifact.logicalGeometry.width() : artifact.logicalGeometry.height();
        const int imageSize = horizontal ? artifact.image.width() : artifact.image.height();
        return logicalSize > 0 ? static_cast<double>(imageSize) / logicalSize : 0.0;
    };
    const auto nativeAxisOffsetForLogicalPosition = [this, &artifactAxisScale](int axisStart, int logicalPosition, bool horizontal) {
        if (logicalPosition <= axisStart)
            return 0;

        std::vector<int> points{axisStart, logicalPosition};
        for (const auto& artifact : m_monitorArtifacts) {
            if (artifact.image.isNull() || !artifact.logicalGeometry.isValid())
                continue;
            const int start = horizontal ? artifact.logicalGeometry.left() : artifact.logicalGeometry.top();
            const int end = horizontal ? artifact.logicalGeometry.right() + 1 : artifact.logicalGeometry.bottom() + 1;
            if (start > axisStart && start < logicalPosition)
                points.push_back(start);
            if (end > axisStart && end < logicalPosition)
                points.push_back(end);
        }

        std::sort(points.begin(), points.end());
        points.erase(std::unique(points.begin(), points.end()), points.end());

        double offset = 0.0;
        for (std::size_t i = 0; i + 1 < points.size(); ++i) {
            const int begin = points[i];
            const int end = points[i + 1];
            if (end <= begin)
                continue;

            const double midpoint = (static_cast<double>(begin) + static_cast<double>(end)) / 2.0;
            double scale = 1.0;
            bool covered = false;
            for (const auto& artifact : m_monitorArtifacts) {
                if (artifact.image.isNull() || !artifact.logicalGeometry.isValid())
                    continue;
                const int artifactStart = horizontal ? artifact.logicalGeometry.left() : artifact.logicalGeometry.top();
                const int artifactEnd = horizontal ? artifact.logicalGeometry.right() + 1 : artifact.logicalGeometry.bottom() + 1;
                if (midpoint < artifactStart || midpoint >= artifactEnd)
                    continue;

                const double axisScale = artifactAxisScale(artifact, horizontal);
                if (axisScale <= 0.0)
                    continue;
                scale = covered ? std::max(scale, axisScale) : axisScale;
                covered = true;
            }
            offset += static_cast<double>(end - begin) * scale;
        }

        return static_cast<int>(std::floor(offset));
    };
    for (const auto& artifact : m_monitorArtifacts) {
        if (artifact.image.isNull() || !artifact.logicalGeometry.isValid())
            continue;

        const QRect logicalPart = globalRect.intersected(artifact.logicalGeometry);
        if (!logicalPart.isValid())
            continue;

        const QRect source = logicalRectToImageRect(logicalPart, artifact.logicalGeometry, artifact.image.size());
        if (!source.isValid())
            continue;

        const QPoint targetTopLeft(nativeAxisOffsetForLogicalPosition(globalRect.left(), logicalPart.left(), true),
                                   nativeAxisOffsetForLogicalPosition(globalRect.top(), logicalPart.top(), false));
        const QRect target(targetTopLeft, source.size());
        outputBounds = outputBounds.isValid() ? outputBounds.united(target) : target;
        drawParts.push_back({.artifact = &artifact, .logical = logicalPart, .source = source, .target = target});
    }
    if (!outputBounds.isValid() || drawParts.empty())
        return {};

    QImage image = boundedImage(outputBounds.size(), QImage::Format_ARGB32_Premultiplied);
    if (image.isNull())
        return {};
    image.fill(QColor(30, 34, 38));

    QPainter painter(&image);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    for (const auto& part : drawParts) {
        const QRect target = part.target.translated(-outputBounds.topLeft()).intersected(image.rect());
        if (target.isValid())
            painter.drawImage(target, part.artifact->image, part.source);
    }
    if (m_defaults.includeCursor) {
        for (const auto& part : drawParts) {
            if (part.artifact->cursorImage.isNull())
                continue;
            const QRect source = logicalRectToImageRect(part.logical, part.artifact->logicalGeometry, part.artifact->cursorImage.size());
            const QRect target = part.target.translated(-outputBounds.topLeft()).intersected(image.rect());
            hyprcapture::ui::paintImageLayer(painter, target, part.artifact->cursorImage, source);
        }
    }

    return image;
}

QImage CaptureOverlay::renderResultImage() {
    const auto bg = currentWindowBackground();
    if (m_mode == hyprcapture::CaptureMode::Window) {
        auto* windowArtifact = selectedWindow() ? selectedWindow() : hoveredWindow();
        if (!windowArtifact)
            return {};
        if (windowArtifact->image.isNull() && !hydrateWindowArtifact(*windowArtifact)) {
            if (hasOverviewSelectionGeometry(*windowArtifact))
                return {};
            return renderDesktopRectAtDisplayResolution(windowFrameGeometry(*windowArtifact));
        }

        QRect artifactSource = windowArtifact->image.rect();
        const bool cropDecorations = currentWindowBorder() == hyprcapture::DecorationPolicy::Remove || currentWindowShadow() == hyprcapture::DecorationPolicy::Remove;
        if (cropDecorations && windowArtifact->visibleGeometry.isValid() && windowArtifact->fullGeometry.contains(windowArtifact->visibleGeometry)) {
            const double scaleX = static_cast<double>(windowArtifact->image.width()) / std::max(1, windowArtifact->fullGeometry.width());
            const double scaleY = static_cast<double>(windowArtifact->image.height()) / std::max(1, windowArtifact->fullGeometry.height());
            artifactSource = QRect(QPoint(static_cast<int>(std::floor((windowArtifact->visibleGeometry.x() - windowArtifact->fullGeometry.x()) * scaleX)),
                                          static_cast<int>(std::floor((windowArtifact->visibleGeometry.y() - windowArtifact->fullGeometry.y()) * scaleY))),
                                   QSize(std::max(1, static_cast<int>(std::ceil(windowArtifact->visibleGeometry.width() * scaleX))),
                                         std::max(1, static_cast<int>(std::ceil(windowArtifact->visibleGeometry.height() * scaleY)))))
                                 .intersected(windowArtifact->image.rect());
        }

        QImage repairedArtifact = windowArtifact->image;
        repairMissingWindowTail(repairedArtifact, windowArtifact->fullGeometry, windowArtifact->visibleGeometry, m_desktopImage, m_desktopGeometry);

        QImage image = boundedImage(artifactSource.size().expandedTo(QSize(1, 1)), QImage::Format_ARGB32_Premultiplied);
        if (image.isNull())
            return {};
        image.fill(Qt::transparent);

        QPainter painter(&image);
        QImage background = boundedImage(image.size(), QImage::Format_RGBA8888);
        if (background.isNull())
            return {};
        background.fill(Qt::transparent);
        const QRect logicalSource = artifactRectToLogicalRect(artifactSource, repairedArtifact.size(), windowArtifact->fullGeometry);
        const QRect desktopSource = desktopSourceRectForGlobalRect(logicalSource);
        const QImage maskArtifact = repairedArtifact.format() == QImage::Format_RGBA8888 ? repairedArtifact : repairedArtifact.convertToFormat(QImage::Format_RGBA8888);
        bool         paintedBackground = false;
        if (bg == hyprcapture::WindowBackground::Real && !windowArtifact->realBackground.isNull() && logicalSource.isValid()) {
            const QRect backgroundSource = projectedImageRect(logicalSource, windowArtifact->fullGeometry, windowArtifact->realBackground.size());
            if (backgroundSource.isValid()) {
                QPainter backgroundPainter(&background);
                backgroundPainter.drawImage(background.rect(), windowArtifact->realBackground, backgroundSource);
                paintedBackground = true;
            }
        }
        if (!paintedBackground && paintWindowBackground(background, bg, m_desktopImage, desktopSource)) {
            if (bg == hyprcapture::WindowBackground::Real)
                reconstructRealWindowBackground(background, maskArtifact, artifactSource);
            paintedBackground = true;
        }
        if (paintedBackground) {
            clipWindowBackgroundToFrame(background,
                                        logicalSource,
                                        windowArtifact->visibleGeometry,
                                        windowArtifact->rounding,
                                        windowArtifact->roundingPower);
            painter.drawImage(QPoint(0, 0), background);
        }

        painter.drawImage(image.rect(), repairedArtifact, artifactSource);
        paintCursorLayers(painter, image.rect(), logicalSource);
        return image;
    }

    const QRect cap = captureRectForMode();
    if (!cap.isValid())
        return {};

    if (!m_monitorArtifacts.empty()) {
        const QImage highResolution = renderDesktopRectAtDisplayResolution(localToDesktopLogicalRect(cap));
        if (!highResolution.isNull())
            return highResolution;
    }

    const QRect desktopSource = localToDesktopSourceRect(cap);
    const QSize outputSize = desktopSource.isValid() ? desktopSource.size() : cap.size();
    QImage image = boundedImage(outputSize.expandedTo(QSize(1, 1)), QImage::Format_ARGB32_Premultiplied);
    if (image.isNull())
        return {};
    image.fill(Qt::transparent);

    QPainter painter(&image);

    if (m_mode != hyprcapture::CaptureMode::Window && !m_desktopImage.isNull() && desktopSource.isValid()) {
        painter.drawImage(image.rect(), m_desktopImage, desktopSource);
    } else if (m_mode == hyprcapture::CaptureMode::Window && bg == hyprcapture::WindowBackground::Real && !m_desktopImage.isNull()) {
        painter.drawImage(image.rect(), m_desktopImage, desktopSource);
    } else if (m_mode == hyprcapture::CaptureMode::Window && bg != hyprcapture::WindowBackground::Transparent) {
        if (bg == hyprcapture::WindowBackground::White)
            painter.fillRect(image.rect(), Qt::white);
        else if (bg == hyprcapture::WindowBackground::Black)
            painter.fillRect(image.rect(), Qt::black);
        else if (bg == hyprcapture::WindowBackground::FollowSystem)
            painter.fillRect(image.rect(), followSystemColor());
        else
            painter.fillRect(image.rect(), QColor(30, 34, 38));
    } else {
        painter.fillRect(image.rect(), QColor(30, 34, 38));
    }

    return image;
}

QString CaptureOverlay::prepareRecordingRequest() {
    m_recordError.clear();
    QRect cap = captureRectForMode();
    if (!cap.isValid()) {
        m_recordError = QStringLiteral("invalid record target");
        return {};
    }

    hyprcapture::RecordingRequest request;
    request.id = hyprcapture::makeSessionId();
    request.defaults = m_defaults;
    request.defaults.mode = m_mode;
    request.defaults.fullscreenScope = currentFullscreenScope();
    request.defaults.windowBackground = currentRecordBackground();
    if (m_mode != hyprcapture::CaptureMode::Window)
        request.defaults.recordSolidAlpha = false;
    request.defaults.windowBorder = currentWindowBorder();
    request.defaults.windowShadow = currentWindowShadow();
    const QString recordFormat = currentRecordFormat();
    const bool imageAnimation = isImageAnimationRecordFormat(recordFormat);
    request.defaults.recordFormat = recordFormat.toStdString();
    request.defaults.recordFilenameTemplate = recordTemplateWithFormat(m_defaults.recordFilenameTemplate, recordFormat).toStdString();
    request.defaults.recordAudio = imageAnimation ? hyprcapture::RecordAudio::Off : m_defaults.recordAudio;
    request.defaults.recordAudioOutput = m_defaults.recordAudioOutput;
    request.defaults.recordAudioInput = m_defaults.recordAudioInput;
    request.defaults.recordCodec = imageAnimation ? recordFormat.toStdString() : codecConfigFromChoice(currentRecordCodec()).toStdString();
    request.defaults.recordFps = currentRecordFps();
    request.defaults.recordMaxSeconds = currentRecordMaxSeconds();
    request.defaults.recordWindowBackend = imageAnimation ? hyprcapture::RecordWindowBackend::Compositor : currentRecordBackend();
    request.mode = m_mode;

    const QString conflict = recordOptionsConflict();
    if (!conflict.isEmpty()) {
        updateRecordWarning();
        return {};
    }

    if (m_mode == hyprcapture::CaptureMode::Window) {
        const auto* window = selectedWindow() ? selectedWindow() : hoveredWindow();
        if (!window || window->address.isEmpty()) {
            m_recordError = QStringLiteral("invalid record window");
            return {};
        }
        request.windowAddress = window->address.toStdString();
        cap = globalToLocalRect(windowFrameGeometry(*window));
    }

    const QRect globalRect = localToDesktopLogicalRect(cap);
    if (!globalRect.isValid()) {
        m_recordError = QStringLiteral("invalid record geometry");
        return {};
    }
    request.targetGeometry = {.x = static_cast<double>(globalRect.x()),
                              .y = static_cast<double>(globalRect.y()),
                              .width = static_cast<double>(globalRect.width()),
                              .height = static_cast<double>(globalRect.height())};

    const QString requestPath = hyprcapture::ui::runtimeFile(QStringLiteral("record-request"), QStringLiteral(".json"));
    const std::string json = hyprcapture::encodeRecordingRequestJson(request);
    if (!writePrivateTextFile(requestPath, QByteArray(json.data(), static_cast<qsizetype>(json.size())))) {
        m_recordError = QStringLiteral("record request write failed");
        return {};
    }

    return requestPath;
}

bool CaptureOverlay::startRecording(const QString& requestPath) {
    if (requestPath.isEmpty()) {
        m_recordError = QStringLiteral("invalid record request");
        return false;
    }

    const auto started = dispatchRecordingStart(requestPath);
    if (!started.success) {
        m_recordError = started.error.isEmpty() ? QStringLiteral("record start failed") : started.error;
        QFile::remove(requestPath);
    }
    return started.success;
}

void CaptureOverlay::startPreparedRecording(const QString& requestPath) {
    hide();
    qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
    QTimer::singleShot(120, this, [this, requestPath] {
        if (!startRecording(requestPath)) {
            show();
            if (m_toolbar) {
                m_toolbar->setEnabled(true);
                m_toolbar->show();
            }
            setCursor(Qt::CrossCursor);
            m_finishing = false;
            updateRecordOptionsVisibility();
            updateStatus();
            return;
        }
        traceTiming(QStringLiteral("record_start"));
        endHymissionCaptureInputSuppression();
        qApp->quit();
    });
}

void CaptureOverlay::launchRecordingCountdown(const QString& requestPath) {
    const int countdownSeconds = currentRecordCountdownSeconds();
    if (countdownSeconds <= 0) {
        startPreparedRecording(requestPath);
        return;
    }

    hide();
    qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
    const QStringList args{QStringLiteral("--record-countdown-request"), requestPath, QStringLiteral("--record-countdown-seconds"), QString::number(countdownSeconds)};
    if (!QProcess::startDetached(QCoreApplication::applicationFilePath(), args)) {
        show();
        QFile::remove(requestPath);
        m_recordError = QStringLiteral("record countdown start failed");
        m_finishing = false;
        updateRecordOptionsVisibility();
        updateStatus();
        return;
    }

    traceTiming(QStringLiteral("record_countdown_start"));
    endHymissionCaptureInputSuppression();
    qApp->quit();
}

bool CaptureOverlay::stopRecording() {
    return dispatchRecordingStop().success;
}

void CaptureOverlay::finishCapture() {
    if (m_finishing)
        return;
    m_finishing = true;

    if (m_record) {
        const QString requestPath = prepareRecordingRequest();
        if (requestPath.isEmpty()) {
            m_finishing = false;
            updateStatus();
            return;
        }
        emit finishingStarted();
        launchRecordingCountdown(requestPath);
        return;
    }

    emit finishingStarted();
    if (m_mode == hyprcapture::CaptureMode::Window) {
        traceTiming(QStringLiteral("fade_start"));
        fadeOutThen([this] { renderAndSaveCapture(); });
        return;
    }

    renderAndSaveCapture();
}

void CaptureOverlay::renderAndSaveCapture() {
    QElapsedTimer renderTimer;
    renderTimer.start();
    auto image = renderResultImage();
    traceTiming(QStringLiteral("render_result"), renderTimer.elapsed());
    if (image.isNull()) {
        m_finishing = false;
        endHymissionCaptureInputSuppression();
        if (isVisible())
            updateStatus();
        else
            qApp->quit();
        return;
    }

    QElapsedTimer watermarkTimer;
    watermarkTimer.start();
    hyprcapture::ui::applyWatermark(image, m_defaults);
    traceTiming(QStringLiteral("apply_watermark"), watermarkTimer.elapsed());

    const auto filenameMetadata = resolvedFilenameMetadata();
    const QString plannedOutputPath = plannedCaptureOutputPath(m_defaults,
                                                               QString::fromStdString(filenameMetadata.windowClass),
                                                               QString::fromStdString(filenameMetadata.windowTitle));
    const QString targetPath = thumbnailTargetPath(m_defaults, plannedOutputPath);
    const QString restoreClipboardPath =
        (m_defaults.clipboard && m_defaults.showThumbnail) ? hyprcapture::ui::runtimeFile("clipboard", ".json") : QString{};
    bool thumbnailStarted = false;
    if (m_defaults.showThumbnail) {
        const QString previewPath = saveThumbnailPreview(image);
        if (!previewPath.isEmpty()) {
            showThumbnail(previewPath, targetPath, restoreClipboardPath);
            thumbnailStarted = true;
        }
    }

    if (!m_fadeOutStarted) {
        traceTiming(QStringLiteral("fade_start"));
        fadeOutThen({});
    }

    hyprcapture::ui::ClipboardSnapshotData clipboardSnapshot;
    if (m_defaults.clipboard && m_defaults.showThumbnail && !restoreClipboardPath.isEmpty()) {
        QElapsedTimer snapshotTimer;
        snapshotTimer.start();
        clipboardSnapshot = hyprcapture::ui::captureClipboardSnapshotData();
        traceTiming(QStringLiteral("clipboard_snapshot_collect"), snapshotTimer.elapsed());
    }

    saveImage(image,
              clipboardSnapshot,
              m_defaults.save ? plannedOutputPath : targetPath,
              restoreClipboardPath,
              thumbnailStarted,
              m_mode,
              filenameMetadata);
}

void CaptureOverlay::saveImage(const QImage& image,
                               hyprcapture::ui::ClipboardSnapshotData clipboardSnapshot,
                               const QString& outputPath,
                               const QString& restoreClipboardPath,
                               bool thumbnailStarted,
                               hyprcapture::CaptureMode mode,
                               hyprcapture::FilenameMetadata filenameMetadata) {
    const auto defaults = m_defaults;
    auto*      worker = QThread::create([this,
                                         image,
                                         defaults,
                                         outputPath,
                                         restoreClipboardPath,
                                         thumbnailStarted,
                                         mode,
                                         filenameMetadata,
                                         clipboardSnapshot = std::move(clipboardSnapshot)] {
        QElapsedTimer totalTimer;
        totalTimer.start();
        const CaptureOutputResult result = writeCaptureOutput(image, defaults, clipboardSnapshot, outputPath, restoreClipboardPath);
        traceTiming(QStringLiteral("output_worker_total"), totalTimer.elapsed());
        QMetaObject::invokeMethod(
            this,
            [this, image, result, thumbnailStarted, defaults, mode, filenameMetadata] {
                traceTiming(QStringLiteral("output_ready"));
                endHymissionCaptureInputSuppression();
                if (result.clipboardRequested && !result.clipboardCopied)
                    hyprcapture::ui::copyImageToClipboard(image);
                if (defaults.save)
                    hyprcapture::ui::showScreenshotNotification(defaults, mode, filenameMetadata, result.savedPath);
                if (result.showThumbnail && !thumbnailStarted) {
                    showThumbnail(result.savedPath, result.savedPath, result.restoreClipboardPath);
                    qApp->quit();
                    return;
                }
                qApp->quit();
            },
            Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void CaptureOverlay::showThumbnail(const QString& previewPath, const QString& targetPath, const QString& restoreClipboardPath) {
    if (previewPath.isEmpty())
        return;

    QStringList args{"--thumbnail-window", previewPath, "--thumbnail-timeout-ms", QString::number(m_defaults.thumbnailTimeoutMs)};
    args << "--thumbnail-monitor" << qString(m_defaults.thumbnailMonitor);
    if (!targetPath.isEmpty())
        args << "--thumbnail-target" << targetPath;
    const QString deleteRoot = thumbnailDeleteRoot(m_defaults);
    if (!deleteRoot.isEmpty())
        args << "--thumbnail-delete-root" << deleteRoot;
    if (!restoreClipboardPath.isEmpty())
        args << "--restore-clipboard" << restoreClipboardPath;
    QProcess::startDetached(QCoreApplication::applicationFilePath(), args);
    traceTiming(QStringLiteral("thumbnail_started"));
    endHymissionCaptureInputSuppression();
}

void CaptureOverlay::cancelCapture() {
    emit finishingStarted();
    fadeOutThen([this] {
        endHymissionCaptureInputSuppression();
        qApp->quit();
    });
}
