#include "audio/helper.hpp"
#include "shared/config.hpp"
#include "ui/capture_overlay.hpp"
#include "ui/clipboard_utils.hpp"
#include "ui/result_thumbnail.hpp"

#include <LayerShellQt/Shell>
#include <LayerShellQt/Window>

#include <QApplication>
#include <QCommandLineParser>
#include <QCursor>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPaintEvent>
#include <QPainter>
#include <QPixmap>
#include <QProcess>
#include <QScreen>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace {

std::vector<QScreen*> thumbnailScreens(QString selector) {
    selector = selector.trimmed();
    if (selector.compare(QStringLiteral("all"), Qt::CaseInsensitive) == 0) {
        const auto screens = QGuiApplication::screens();
        return {screens.begin(), screens.end()};
    }
    if (selector.compare(QStringLiteral("primary"), Qt::CaseInsensitive) == 0) {
        if (QScreen* primary = QGuiApplication::primaryScreen())
            return {primary};
        return {nullptr};
    }
    if (!selector.isEmpty() && selector.compare(QStringLiteral("active"), Qt::CaseInsensitive) != 0) {
        for (QScreen* screen : QGuiApplication::screens()) {
            if (screen && screen->name().compare(selector, Qt::CaseInsensitive) == 0)
                return {screen};
        }
    }
    if (QScreen* active = QGuiApplication::screenAt(QCursor::pos()))
        return {active};
    return {QGuiApplication::primaryScreen()};
}

constexpr qint64 MAX_SESSION_JSON_BYTES = 8LL * 1024LL * 1024LL;
constexpr qint64 MAX_THUMBNAIL_SOURCE_BYTES = 128LL * 1024LL * 1024LL;
constexpr int    MAX_THUMBNAIL_DIMENSION = 32768;
constexpr int    THUMBNAIL_DECODE_MAX_WIDTH = 720;
constexpr int    THUMBNAIL_DECODE_MAX_HEIGHT = 480;
constexpr int    MAX_THUMBNAIL_TIMEOUT_MS = 60 * 60 * 1000;
constexpr int    MAX_RECORD_COUNTDOWN_SECONDS = 60;
constexpr qint64 MAX_RECORD_REQUEST_BYTES = 64LL * 1024LL;
constexpr int    APNG_TRANSCODE_FPS = 60;

bool flagValue(const QCommandLineParser& parser, const QString& name, bool fallback) {
    const auto value = parser.value(name);
    if (value.isEmpty())
        return fallback;
    return value != "0" && value != "false";
}

bool hasArgument(int argc, char** argv, const char* name) {
    for (int i = 1; i < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == QLatin1String(name))
            return true;
    }
    return false;
}

int boundedInt(QString value, int fallback, int minimum, int maximum) {
    bool ok = false;
    const int parsed = value.toInt(&ok);
    if (!ok)
        return fallback;
    return std::clamp(parsed, minimum, maximum);
}

QPixmap loadThumbnailPixmap(const QString& path) {
    const QFileInfo info(path);
    if (path.isEmpty() || !info.exists() || !info.isFile() || !info.isReadable() || info.size() < 0 || info.size() > MAX_THUMBNAIL_SOURCE_BYTES)
        return {};

    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize sourceSize = reader.size();
    if (sourceSize.isEmpty() || sourceSize.width() <= 0 || sourceSize.height() <= 0 || sourceSize.width() > MAX_THUMBNAIL_DIMENSION ||
        sourceSize.height() > MAX_THUMBNAIL_DIMENSION)
        return {};

    reader.setScaledSize(sourceSize.scaled(THUMBNAIL_DECODE_MAX_WIDTH, THUMBNAIL_DECODE_MAX_HEIGHT, Qt::KeepAspectRatio));
    const QImage image = reader.read();
    if (image.isNull())
        return {};
    return QPixmap::fromImage(image);
}

bool pathIsUnderDirectory(const QString& path, const QString& root) {
    const QString pathCanonical = QFileInfo(path).canonicalFilePath();
    const QString rootCanonical = QFileInfo(root).canonicalFilePath();
    return !pathCanonical.isEmpty() && !rootCanonical.isEmpty() && pathCanonical.startsWith(rootCanonical + QLatin1Char('/'));
}

bool trustedDirectory(const QString& path) {
    const QByteArray native = QFile::encodeName(QFileInfo(path).canonicalFilePath());
    struct stat      st {};
    return !native.isEmpty() && stat(native.constData(), &st) == 0 && S_ISDIR(st.st_mode) && (st.st_uid == 0 || st.st_uid == geteuid()) &&
        (st.st_mode & 0022) == 0;
}

struct DispatchCommandResult {
    bool    success = false;
    QString error;
};

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

DispatchCommandResult runHyprctl(const QStringList& args, const QString& fallbackError) {
    const QString hyprctl = hyprcapture::ui::trustedSystemProgram(QStringLiteral("hyprctl"));
    if (hyprctl.isEmpty())
        return {.success = false, .error = QStringLiteral("hyprctl unavailable")};

    QProcess process;
    process.setProgram(hyprctl);
    process.setArguments(args);
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
    return {.success = false, .error = compactProcessErrorText(stderrData, stdoutData, fallbackError)};
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
    return runHyprctl(QStringList{QStringLiteral("eval"), expression}, QStringLiteral("eval failed"));
}

DispatchCommandResult dispatchRecordingStart(const QString& requestPath) {
    if (requestPath.isEmpty())
        return {.success = false, .error = QStringLiteral("record request missing")};
    if (!hyprcapture::ui::isPrivateRuntimeFile(requestPath, MAX_RECORD_REQUEST_BYTES))
        return {.success = false, .error = QStringLiteral("invalid record request")};

    return evalHyprcaptureLuaFunction(QStringLiteral("record_start"), requestPath);
}

class RecordingCountdownDisplay final : public QWidget {
  public:
    explicit RecordingCountdownDisplay(QScreen* screen) : QWidget(nullptr) {
        setWindowTitle(QStringLiteral("HyprCapture Countdown"));
        setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setFocusPolicy(Qt::NoFocus);

        setGeometry(screen ? screen->geometry() : QRect(0, 0, 1280, 720));

        winId();
        if (screen && windowHandle())
            windowHandle()->setScreen(screen);
        if (auto* layerWindow = LayerShellQt::Window::get(windowHandle())) {
            if (screen)
                layerWindow->setScreen(screen);
            layerWindow->setScope("hyprcapture-countdown");
            layerWindow->setLayer(LayerShellQt::Window::LayerOverlay);
            layerWindow->setAnchors(LayerShellQt::Window::Anchors{LayerShellQt::Window::AnchorTop} | LayerShellQt::Window::AnchorBottom |
                                    LayerShellQt::Window::AnchorLeft | LayerShellQt::Window::AnchorRight);
            layerWindow->setExclusiveZone(-1);
            layerWindow->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
            layerWindow->setDesiredSize(QSize(0, 0));
        }

    }

    void setRemaining(int remaining) {
        m_remaining = remaining;
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect(), Qt::transparent);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

        const int minDim = std::max(1, std::min(width(), height()));
        const int maxDiameter = std::max(64, std::min(280, minDim - 32));
        const int diameter = std::clamp(minDim / 5, 64, maxDiameter);
        const QPoint center = rect().center();
        const QRectF badge(center.x() - diameter / 2.0, center.y() - diameter / 2.0, diameter, diameter);
        const QString text = QString::number(m_remaining);

        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 150));
        painter.drawEllipse(badge);

        QFont font = painter.font();
        font.setBold(true);
        font.setPointSize(std::clamp(diameter * 52 / 100, 42, 128));
        painter.setFont(font);
        painter.setPen(QColor(0, 0, 0, 180));
        painter.drawText(badge.translated(0, diameter * 0.04), Qt::AlignCenter, text);
        painter.setPen(QColor(255, 255, 255, 245));
        painter.drawText(badge, Qt::AlignCenter, text);
    }

  private:
    int m_remaining = 0;
};

class RecordingCountdownController final : public QObject {
  public:
    RecordingCountdownController(QString requestPath, int seconds)
        : m_requestPath(std::move(requestPath)), m_remaining(std::clamp(seconds, 1, MAX_RECORD_COUNTDOWN_SECONDS)) {
        for (QScreen* screen : QGuiApplication::screens()) {
            if (screen)
                m_displays.push_back(std::make_unique<RecordingCountdownDisplay>(screen));
        }
        if (m_displays.empty())
            m_displays.push_back(std::make_unique<RecordingCountdownDisplay>(QGuiApplication::primaryScreen()));
        connect(&m_timer, &QTimer::timeout, this, [this] { advance(); });
        m_timer.setInterval(1000);
    }

    void start() {
        for (auto& display : m_displays) {
            display->setRemaining(m_remaining);
            display->show();
            display->raise();
        }
        m_timer.start();
    }

  private:
    void advance() {
        --m_remaining;
        if (m_remaining <= 0) {
            startRecording();
            return;
        }
        for (auto& display : m_displays)
            display->setRemaining(m_remaining);
    }

    void startRecording() {
        m_timer.stop();
        for (auto& display : m_displays)
            display->hide();
        qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
        QTimer::singleShot(120, this, [this] {
            const auto result = dispatchRecordingStart(m_requestPath);
            if (!result.success)
                QFile::remove(m_requestPath);
            qApp->quit();
        });
    }

    QString m_requestPath;
    int     m_remaining = 0;
    QTimer  m_timer;
    std::vector<std::unique_ptr<RecordingCountdownDisplay>> m_displays;
};

bool isTrustedRecordingResultPath(const QString& path, const hyprcapture::CaptureDefaults& defaults) {
    const QFileInfo info(path);
    if (path.isEmpty() || !info.exists() || !info.isFile() || !info.isReadable() || info.ownerId() != geteuid())
        return false;

    const QByteArray native = QFile::encodeName(info.canonicalFilePath());
    struct stat      st {};
    if (native.isEmpty() || stat(native.constData(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0022) != 0)
        return false;

    const QString recordRoot = QString::fromStdString(hyprcapture::expandUserPath(defaults.recordSaveDir).string());
    return trustedDirectory(recordRoot) && pathIsUnderDirectory(path, recordRoot);
}

bool isTrustedRecordingOutputPath(const QString& path, const hyprcapture::CaptureDefaults& defaults) {
    const QFileInfo info(path);
    if (path.isEmpty() || !info.isAbsolute() || info.fileName().isEmpty() || info.fileName() == QLatin1String(".") || info.fileName() == QLatin1String(".."))
        return false;
    if (info.exists())
        return false;

    const QString recordRoot = QString::fromStdString(hyprcapture::expandUserPath(defaults.recordSaveDir).string());
    if (!trustedDirectory(recordRoot))
        return false;

    const QString rootCanonical = QFileInfo(recordRoot).canonicalFilePath();
    const QString parentCanonical = QFileInfo(info.absolutePath()).canonicalFilePath();
    return !rootCanonical.isEmpty() && !parentCanonical.isEmpty() &&
        (parentCanonical == rootCanonical || parentCanonical.startsWith(rootCanonical + QLatin1Char('/')));
}

void setOwnerOnlyPermissions(const QString& path) {
    const QByteArray native = QFile::encodeName(path);
    if (!native.isEmpty())
        chmod(native.constData(), 0600);
}

QPixmap recordingThumbnailPixmap(bool showPlayButton) {
    constexpr QSize logicalSize(180, 120);
    qreal dpr = 1.0;
    for (const QScreen* screen : QGuiApplication::screens()) {
        if (screen)
            dpr = std::max(dpr, screen->devicePixelRatio());
    }
    dpr = std::clamp(dpr, qreal{1.0}, qreal{4.0});
    QPixmap pixmap(QSize(static_cast<int>(std::ceil(logicalSize.width() * dpr)), static_cast<int>(std::ceil(logicalSize.height() * dpr))));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    const QRectF rect(0.5, 0.5, logicalSize.width() - 1.0, logicalSize.height() - 1.0);
    painter.setPen(QPen(QColor(235, 238, 242, 95), 1.0));
    painter.setBrush(QColor(28, 31, 36, 238));
    painter.drawRoundedRect(rect, 8.0, 8.0);

    if (showPlayButton) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(235, 238, 242, 235));
        const QPointF center(logicalSize.width() / 2.0, logicalSize.height() / 2.0);
        QPolygonF play;
        play << QPointF(center.x() - 13.0, center.y() - 18.0) << QPointF(center.x() - 13.0, center.y() + 18.0) << QPointF(center.x() + 20.0, center.y());
        painter.drawPolygon(play);
    }
    return pixmap;
}

class ResultThumbnailCollection {
  public:
    ResultThumbnailCollection(const QPixmap& pixmap,
                              const QString& path,
                              const QString& restoreClipboardPath,
                              const QString& deleteRoot,
                              int timeoutMs,
                              bool copyFile,
                              const std::vector<QScreen*>& screens) {
        for (QScreen* screen : screens) {
            m_thumbnails.push_back(std::make_unique<ResultThumbnail>(
                pixmap, path, restoreClipboardPath, deleteRoot, timeoutMs, copyFile, screen));
        }
        if (m_thumbnails.empty())
            m_thumbnails.push_back(std::make_unique<ResultThumbnail>(
                pixmap, path, restoreClipboardPath, deleteRoot, timeoutMs, copyFile, nullptr));
        connectSwipeSynchronization();
    }

    void show() {
        for (const auto& thumbnail : m_thumbnails)
            thumbnail->show();
    }

    void setImagePixmap(const QPixmap& pixmap) {
        for (const auto& thumbnail : m_thumbnails)
            thumbnail->setImagePixmap(pixmap);
    }

    void setTranscodeProgress(double progress) {
        for (const auto& thumbnail : m_thumbnails)
            thumbnail->setTranscodeProgress(progress);
    }

    void finishTranscodeProgress(bool success, int timeoutMs) {
        for (const auto& thumbnail : m_thumbnails)
            thumbnail->finishTranscodeProgress(success, timeoutMs);
    }

  private:
    void connectSwipeSynchronization() {
        if (m_thumbnails.size() < 2)
            return;

        for (const auto& sourceOwner : m_thumbnails) {
            ResultThumbnail* source = sourceOwner.get();
            QObject::connect(source, &ResultThumbnail::swipeOffsetChanged, source, [this, source](const QPointF& offset) {
                for (const auto& thumbnail : m_thumbnails) {
                    if (thumbnail.get() != source)
                        thumbnail->applySynchronizedSwipeOffset(offset);
                }
            });
            QObject::connect(source, &ResultThumbnail::swipeResetStarted, source, [this, source] {
                for (const auto& thumbnail : m_thumbnails) {
                    if (thumbnail.get() != source)
                        thumbnail->resetSynchronizedSwipe();
                }
            });
            QObject::connect(source, &ResultThumbnail::swipeActionStarted, source, [this, source](bool deleteRequested) {
                for (const auto& thumbnail : m_thumbnails) {
                    if (thumbnail.get() != source)
                        thumbnail->animateSynchronizedSwipeOut(deleteRequested);
                }
            });
        }
    }

    std::vector<std::unique_ptr<ResultThumbnail>> m_thumbnails;
};

int showRecordingResult(const hyprcapture::CaptureDefaults& defaults, const QString& path) {
    if (!isTrustedRecordingResultPath(path, defaults))
        return 1;

    const QString canonicalPath = QFileInfo(path).canonicalFilePath();
    QString       restoreClipboardPath;
    if (defaults.clipboard && defaults.showThumbnail)
        restoreClipboardPath = hyprcapture::ui::saveClipboardSnapshot();

    if (defaults.clipboard)
        hyprcapture::ui::copyFileUrlToClipboard(canonicalPath);

    if (!defaults.showThumbnail) {
        hyprcapture::ui::discardClipboardSnapshot(restoreClipboardPath);
        return 0;
    }

    const QString deleteRoot = QString::fromStdString(hyprcapture::expandUserPath(defaults.recordSaveDir).string());
    ResultThumbnailCollection thumbnails(recordingThumbnailPixmap(true),
                                         canonicalPath,
                                         restoreClipboardPath,
                                         deleteRoot,
                                         static_cast<int>(defaults.thumbnailTimeoutMs),
                                         true,
                                         thumbnailScreens(QString::fromStdString(defaults.thumbnailMonitor)));
    thumbnails.show();
    return qApp->exec();
}

struct TranscodeState {
    QProcess* process = nullptr;
    std::unique_ptr<ResultThumbnailCollection> thumbnails;
    QByteArray stdoutBuffer;
    QByteArray stderrBuffer;
    QString inputPath;
    QString outputPath;
    QString restoreClipboardPath;
    hyprcapture::CaptureDefaults defaults;
    int durationMs = 1;
};

void updateTranscodeProgress(const std::shared_ptr<TranscodeState>& state) {
    if (!state || !state->process)
        return;

    state->stdoutBuffer += state->process->readAllStandardOutput();
    while (true) {
        const int newline = state->stdoutBuffer.indexOf('\n');
        if (newline < 0)
            break;

        const QByteArray line = state->stdoutBuffer.left(newline).trimmed();
        state->stdoutBuffer.remove(0, newline + 1);
        const int separator = line.indexOf('=');
        if (separator <= 0)
            continue;

        const QByteArray key = line.left(separator);
        const QByteArray value = line.mid(separator + 1);
        double progress = -1.0;
        if (key == "out_time_us" || key == "out_time_ms") {
            bool ok = false;
            const qlonglong micros = value.toLongLong(&ok);
            if (ok && state->durationMs > 0)
                progress = static_cast<double>(micros) / (static_cast<double>(state->durationMs) * 1000.0);
        } else if (key == "progress" && value == "end") {
            progress = 1.0;
        }

        if (progress >= 0.0 && state->thumbnails)
            state->thumbnails->setTranscodeProgress(std::clamp(progress, 0.0, 1.0));
    }
}

int showRecordingTranscode(const hyprcapture::CaptureDefaults& defaults, const QString& inputPath, const QString& outputPath, bool preserveAlpha, int durationMs) {
    if (!isTrustedRecordingResultPath(inputPath, defaults) || !isTrustedRecordingOutputPath(outputPath, defaults))
        return 1;

    const QString ffmpeg = hyprcapture::ui::trustedSystemProgram(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty())
        return 1;

    const QString canonicalInput = QFileInfo(inputPath).canonicalFilePath();
    const QString cleanOutput = QDir::cleanPath(outputPath);
    const QString deleteRoot = QString::fromStdString(hyprcapture::expandUserPath(defaults.recordSaveDir).string());

    auto state = std::make_shared<TranscodeState>();
    state->inputPath = canonicalInput;
    state->outputPath = cleanOutput;
    state->defaults = defaults;
    state->durationMs = std::max(1, durationMs);

    if (defaults.clipboard && defaults.showThumbnail)
        state->restoreClipboardPath = hyprcapture::ui::saveClipboardSnapshot();

    if (defaults.showThumbnail) {
        state->thumbnails = std::make_unique<ResultThumbnailCollection>(
            recordingThumbnailPixmap(false),
            cleanOutput,
            state->restoreClipboardPath,
            deleteRoot,
            0,
            true,
            thumbnailScreens(QString::fromStdString(defaults.thumbnailMonitor)));
        state->thumbnails->setTranscodeProgress(0.0);
        state->thumbnails->show();
    }

    state->process = new QProcess(qApp);
    state->process->setProgram(ffmpeg);
    state->process->setArguments({QStringLiteral("-hide_banner"),
                                  QStringLiteral("-loglevel"),
                                  QStringLiteral("error"),
                                  QStringLiteral("-nostdin"),
                                  QStringLiteral("-y"),
                                  QStringLiteral("-progress"),
                                  QStringLiteral("pipe:1"),
                                  QStringLiteral("-i"),
                                  canonicalInput,
                                  QStringLiteral("-an"),
                                  QStringLiteral("-c:v"),
                                  QStringLiteral("apng"),
                                  QStringLiteral("-pix_fmt"),
                                  preserveAlpha ? QStringLiteral("rgba") : QStringLiteral("rgb24"),
                                  QStringLiteral("-pred"),
                                  QStringLiteral("none"),
                                  QStringLiteral("-plays"),
                                  QStringLiteral("0"),
                                  QStringLiteral("-r"),
                                  QString::number(APNG_TRANSCODE_FPS),
                                  cleanOutput});
    state->process->setProcessEnvironment(hyprcapture::ui::trustedProcessEnvironment());
    state->process->setProcessChannelMode(QProcess::SeparateChannels);

    QObject::connect(state->process, &QProcess::readyReadStandardOutput, qApp, [state] {
        updateTranscodeProgress(state);
    });
    QObject::connect(state->process, &QProcess::readyReadStandardError, qApp, [state] {
        state->stderrBuffer += state->process->readAllStandardError();
        constexpr qsizetype maxErrorBytes = 16 * 1024;
        if (state->stderrBuffer.size() > maxErrorBytes)
            state->stderrBuffer = state->stderrBuffer.right(maxErrorBytes);
    });
    QObject::connect(state->process, &QProcess::finished, qApp, [state](int exitCode, QProcess::ExitStatus exitStatus) {
        updateTranscodeProgress(state);
        const bool success = exitStatus == QProcess::NormalExit && exitCode == 0 && QFileInfo(state->outputPath).exists();

        QFile::remove(state->inputPath);
        if (success) {
            setOwnerOnlyPermissions(state->outputPath);
            if (state->defaults.clipboard)
                hyprcapture::ui::copyFileUrlToClipboard(state->outputPath);
            if (state->thumbnails) {
                state->thumbnails->setTranscodeProgress(1.0);
                state->thumbnails->setImagePixmap(recordingThumbnailPixmap(true));
                state->thumbnails->finishTranscodeProgress(true, static_cast<int>(state->defaults.thumbnailTimeoutMs));
            } else {
                qApp->quit();
            }
            return;
        }

        QFile::remove(state->outputPath);
        if (state->thumbnails) {
            state->thumbnails->finishTranscodeProgress(false, state->defaults.thumbnailTimeoutMs > 0 ? static_cast<int>(state->defaults.thumbnailTimeoutMs) : 5000);
        } else {
            hyprcapture::ui::discardClipboardSnapshot(state->restoreClipboardPath);
            qApp->quit();
        }
    });

    state->process->start();
    if (!state->process->waitForStarted(1000)) {
        QFile::remove(canonicalInput);
        QFile::remove(cleanOutput);
        hyprcapture::ui::discardClipboardSnapshot(state->restoreClipboardPath);
        if (state->thumbnails)
            state->thumbnails->finishTranscodeProgress(false, 5000);
        else
            return 1;
    }

    return qApp->exec();
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && (std::string_view(argv[1]) == "--sound-list" || std::string_view(argv[1]) == "--sound-capture" || std::string_view(argv[1]) == "--sound-finalize"))
        return hyprcapture::audio::runHelper(argc, argv);
    qputenv("QT_WAYLAND_SHELL_INTEGRATION", "layer-shell");
#if LAYERSHELLQTINTERFACE_ENABLE_DEPRECATED_SINCE(6, 6)
    LayerShellQt::Shell::useLayerShell();
#endif

    QApplication app(argc, argv);
    QApplication::setApplicationName("hyprcapture-ui");

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOptions({
        {{"m", "mode"}, "Capture mode.", "mode", "region"},
        {"fullscreen-scope", "Fullscreen scope.", "scope", "all"},
        {"overlay-scope", "Overlay monitor scope.", "scope", "fix"},
        {"window-background", "Window background.", "background", "follow-system"},
        {"window-border", "Window border policy.", "policy", "keep"},
        {"window-shadow", "Window shadow policy.", "policy", "keep"},
        {"notification-backend", "Notification backend: hyprland or system.", "backend", "hyprland"},
        {"save", "Save output.", "0|1", "1"},
        {"clipboard", "Copy output.", "0|1", "1"},
        {"thumbnail", "Show thumbnail.", "0|1", "1"},
        {"screenshot-notification", "Show a notification after a successful screenshot.", "0|1", "1"},
        {"include-cursor", "Include cursor.", "0|1", "0"},
        {"confirm-before-capture", "Require explicit confirmation after target selection for normal open captures.", "0|1", "0"},
        {{"fushion-mode", "fusion-mode"}, "Enable fushion toolbar behavior.", "0|1", "0"},
        {"capture-fullscreen-clients-as-monitor", "Capture fullscreen clients as their monitor in window/fusion mode.", "0|1", "0"},
        {"dynamic-window-metadata", "Use capture-aware window metadata in screenshot filename templates.", "0|1", "1"},
        {"window-wheel-scroll", "Enable wheel-based window selection.", "0|1", "1"},
        {"window-wheel-scope", "Window wheel selection scope: workspace or under-cursor.", "scope", "workspace"},
        {"fullscreen-preview-rounding", "Fullscreen preview rounding: auto, 0, or logical pixels.", "rounding", "auto"},
        {"save-dir", "Save directory.", "path", "$XDG_PICTURES_DIR/Screenshots"},
        {"filename-template", "Filename strftime template with {window_class}/{window_title}.", "template", "Screenshot-%Y-%m-%d-%H%M%S.png"},
        {"notification-title-template", "Screenshot notification title template.", "template", "Screenshot captured"},
        {"notification-body-template", "Screenshot notification body template.", "template", "Saved {filename} ({window_title})"},
        {"record-save-dir", "Recording save directory.", "path", "$XDG_VIDEOS_DIR/Screenrecords"},
        {"record-filename-template", "Recording filename strftime template.", "template", "Recording-%Y-%m-%d-%H%M%S.mp4"},
        {"record-format", "Recording format.", "format", "mp4"},
        {"record-transparent-format", "Transparent window recording container format.", "format", "webm"},
        {"record-audio", "Recording sound: off, system, microphone, mix.", "mode", "off"},
        {"record-audio-output", "System sound output device.", "device", "default"},
        {"record-audio-input", "Microphone source.", "device", "default"},
        {"record-codec", "Recording codec.", "codec", "auto"},
        {"record-transparent-codec", "Transparent window recording codec.", "codec", "auto"},
        {"record-solid-alpha", "Keep alpha outside follow-system/white/black window recording content when supported.", "0|1", "0"},
        {"record-preset", "Recording preset.", "preset", "veryfast"},
        {"record-gsr-flags", "Extra gpu-screen-recorder flags.", "flags", ""},
        {"record-window-backend", "Window recording backend.", "backend", "compositor"},
        {"record-fps", "Recording FPS.", "fps", "30"},
        {"record-fps-options", "Recording FPS choices.", "fps-list", "15 24 30 60"},
        {"record-window-fps-limit", "Window recording FPS cap.", "fps", "12"},
        {"record-window-real-bg-fps-limit", "Real background window recording FPS cap.", "fps", "8"},
        {"record-max-seconds", "Recording max seconds.", "seconds", "0"},
        {"record-countdown-seconds", "Recording start countdown seconds.", "seconds", "0"},
        {"thumbnail-timeout-ms", "Thumbnail timeout.", "ms", "5000"},
        {"thumbnail-monitor", "Thumbnail monitor: active, primary, all, or output name.", "monitor", "active"},
        {"watermark", "Watermark image path or built-in id.", "path|id", ""},
        {"watermark-position", "Watermark position.", "position", "central"},
        {"watermark-width", "Watermark width in pixels or screenshot-width percent.", "width", "20%"},
        {"watermark-offset", "Watermark x/y offset in pixels or percent.", "vec2", "0 0"},
        {"session-json", "Compositor session metadata.", "json", "{}"},
        {"session-json-file", "Private compositor session metadata file.", "path"},
        {"thumbnail-window", "Show a normal thumbnail window for an image path.", "path"},
        {"thumbnail-target", "Path opened or deleted by a thumbnail preview.", "path"},
        {"record-countdown-request", "Show an input-transparent recording countdown for a private request file.", "path"},
        {"recording-result", "Handle a completed recording result.", "path"},
        {"recording-transcode-input", "Transcode an intermediate recording input.", "path"},
        {"recording-transcode-output", "Transcode output path.", "path"},
        {"recording-transcode-alpha", "Preserve alpha while transcoding.", "0|1", "0"},
        {"recording-transcode-duration-ms", "Expected transcode duration.", "ms", "1"},
        {"thumbnail-delete-root", "Directory where thumbnail files may be deleted.", "path"},
        {"restore-clipboard", "Clipboard snapshot to restore when deleting the thumbnail image.", "path"},
        {"quick", "Capture immediately."},
        {"record", "Use the overlay to start a compositor-side recording."},
        {"record-active", "Show recording controls as active."},
    });
    parser.process(app);

    if (hasArgument(argc, argv, "--thumbnail-window")) {
        const QString thumbnailPath = parser.value("thumbnail-window");
        const QString thumbnailTarget = parser.value("thumbnail-target").isEmpty() ? thumbnailPath : parser.value("thumbnail-target");
        const QPixmap pixmap = loadThumbnailPixmap(thumbnailPath);
        if (pixmap.isNull())
            return 1;
        ResultThumbnailCollection thumbnails(pixmap,
                                             thumbnailTarget,
                                             parser.value("restore-clipboard"),
                                             parser.value("thumbnail-delete-root"),
                                             boundedInt(parser.value("thumbnail-timeout-ms"), 5000, 0, MAX_THUMBNAIL_TIMEOUT_MS),
                                             false,
                                             thumbnailScreens(parser.value("thumbnail-monitor")));
        thumbnails.show();
        return app.exec();
    }

    hyprcapture::CaptureDefaults defaults;
    defaults.mode = hyprcapture::parseCaptureMode(parser.value("mode").toStdString(), defaults.mode);
    defaults.fullscreenScope = hyprcapture::parseFullscreenScope(parser.value("fullscreen-scope").toStdString(), defaults.fullscreenScope);
    defaults.overlayScope = hyprcapture::parseOverlayScope(parser.value("overlay-scope").toStdString(), defaults.overlayScope);
    defaults.windowBackground = hyprcapture::parseWindowBackground(parser.value("window-background").toStdString(), defaults.windowBackground);
    defaults.windowBorder = hyprcapture::parseDecorationPolicy(parser.value("window-border").toStdString(), defaults.windowBorder);
    defaults.windowShadow = hyprcapture::parseDecorationPolicy(parser.value("window-shadow").toStdString(), defaults.windowShadow);
    defaults.notificationBackend =
        hyprcapture::parseNotificationBackend(parser.value("notification-backend").toStdString(), defaults.notificationBackend);
    defaults.save = flagValue(parser, "save", defaults.save);
    defaults.clipboard = flagValue(parser, "clipboard", defaults.clipboard);
    defaults.showThumbnail = flagValue(parser, "thumbnail", defaults.showThumbnail);
    defaults.screenshotNotification = flagValue(parser, "screenshot-notification", defaults.screenshotNotification);
    defaults.includeCursor = flagValue(parser, "include-cursor", defaults.includeCursor);
    defaults.confirmBeforeCapture = flagValue(parser, "confirm-before-capture", defaults.confirmBeforeCapture);
    defaults.fushionMode = flagValue(parser, "fushion-mode", defaults.fushionMode);
    defaults.captureFullscreenClientsAsMonitor =
        flagValue(parser, "capture-fullscreen-clients-as-monitor", defaults.captureFullscreenClientsAsMonitor);
    defaults.dynamicWindowMetadata = flagValue(parser, "dynamic-window-metadata", defaults.dynamicWindowMetadata);
    defaults.windowWheelScroll = flagValue(parser, "window-wheel-scroll", defaults.windowWheelScroll);
    defaults.windowWheelScope =
        hyprcapture::parseWindowWheelScope(parser.value("window-wheel-scope").toStdString(), defaults.windowWheelScope);
    defaults.fullscreenPreviewRounding = parser.value("fullscreen-preview-rounding").toStdString();
    defaults.saveDir = parser.value("save-dir").toStdString();
    defaults.filenameTemplate = parser.value("filename-template").toStdString();
    defaults.notificationTitleTemplate = parser.value("notification-title-template").toStdString();
    defaults.notificationBodyTemplate = parser.value("notification-body-template").toStdString();
    defaults.recordSaveDir = parser.value("record-save-dir").toStdString();
    defaults.recordFilenameTemplate = parser.value("record-filename-template").toStdString();
    defaults.recordFormat = parser.value("record-format").toStdString();
    defaults.recordTransparentFormat = parser.value("record-transparent-format").toStdString();
    defaults.recordAudio = hyprcapture::parseRecordAudio(parser.value("record-audio").toStdString());
    defaults.recordAudioOutput = parser.value("record-audio-output").toStdString();
    defaults.recordAudioInput = parser.value("record-audio-input").toStdString();
    defaults.recordCodec = parser.value("record-codec").toStdString();
    defaults.recordTransparentCodec = parser.value("record-transparent-codec").toStdString();
    defaults.recordSolidAlpha = flagValue(parser, "record-solid-alpha", defaults.recordSolidAlpha);
    defaults.recordPreset = parser.value("record-preset").toStdString();
    defaults.recordGsrFlags = parser.value("record-gsr-flags").toStdString();
    defaults.recordWindowBackend = hyprcapture::parseRecordWindowBackend(parser.value("record-window-backend").toStdString(), defaults.recordWindowBackend);
    defaults.recordFps = boundedInt(parser.value("record-fps"), defaults.recordFps, 1, 240);
    defaults.recordFpsOptions = parser.value("record-fps-options").toStdString();
    defaults.recordWindowFpsLimit = boundedInt(parser.value("record-window-fps-limit"), defaults.recordWindowFpsLimit, 0, 240);
    defaults.recordWindowRealBgFpsLimit = boundedInt(parser.value("record-window-real-bg-fps-limit"), defaults.recordWindowRealBgFpsLimit, 0, 240);
    defaults.recordMaxSeconds = boundedInt(parser.value("record-max-seconds"), defaults.recordMaxSeconds, 0, 24 * 60 * 60);
    defaults.recordCountdownSeconds = boundedInt(parser.value("record-countdown-seconds"), defaults.recordCountdownSeconds, 0, MAX_RECORD_COUNTDOWN_SECONDS);
    defaults.thumbnailTimeoutMs = boundedInt(parser.value("thumbnail-timeout-ms"), defaults.thumbnailTimeoutMs, 0, MAX_THUMBNAIL_TIMEOUT_MS);
    defaults.thumbnailMonitor = parser.value("thumbnail-monitor").toStdString();
    defaults.watermark = parser.value("watermark").toStdString();
    defaults.watermarkPosition = hyprcapture::parseWatermarkPosition(parser.value("watermark-position").toStdString(), defaults.watermarkPosition);
    defaults.watermarkWidth = parser.value("watermark-width").toStdString();
    defaults.watermarkOffset = parser.value("watermark-offset").toStdString();

    if (hasArgument(argc, argv, "--record-countdown-request")) {
        const QString requestPath = parser.value("record-countdown-request");
        if (!hyprcapture::ui::isPrivateRuntimeFile(requestPath, MAX_RECORD_REQUEST_BYTES))
            return 1;
        RecordingCountdownController countdown(requestPath, defaults.recordCountdownSeconds);
        countdown.start();
        return app.exec();
    }

    if (hasArgument(argc, argv, "--recording-transcode-input"))
        return showRecordingTranscode(defaults,
                                      parser.value("recording-transcode-input"),
                                      parser.value("recording-transcode-output"),
                                      flagValue(parser, "recording-transcode-alpha", false),
                                      boundedInt(parser.value("recording-transcode-duration-ms"), 1, 1, 24 * 60 * 60 * 1000));

    if (hasArgument(argc, argv, "--recording-result"))
        return showRecordingResult(defaults, parser.value("recording-result"));

    QString sessionJson = parser.value("session-json");
    if (parser.isSet("session-json-file")) {
        const QString path = parser.value("session-json-file");
        QFile         file(path);
        if (hyprcapture::ui::isPrivateRuntimeFile(path, MAX_SESSION_JSON_BYTES) && file.open(QIODevice::ReadOnly))
            sessionJson = QString::fromUtf8(file.readAll());
        if (hyprcapture::ui::isPrivateRuntimePath(path))
            QFile::remove(path);
    }

    const bool quick = parser.isSet("quick");
    CaptureOverlay overlay(defaults, quick, parser.isSet("record"), parser.isSet("record-active"), sessionJson);
    const auto     overlayScope = overlay.overlayScope();

    std::vector<std::unique_ptr<CaptureOverlay>> extraOverlays;
    std::vector<CaptureOverlay*>                 overlays{&overlay};
    if (!quick && overlayScope != hyprcapture::OverlayScope::Fix) {
        QScreen* launchScreen = overlay.overlayScreen();
        for (QScreen* screen : QGuiApplication::screens()) {
            if (!screen || screen == launchScreen)
                continue;
            const bool active = overlayScope == hyprcapture::OverlayScope::All;
            auto       extra = std::make_unique<CaptureOverlay>(overlay, screen->geometry(), active);
            overlays.push_back(extra.get());
            extraOverlays.push_back(std::move(extra));
        }
    }

    CaptureOverlay* activeOverlay = &overlay;
    if (overlayScope == hyprcapture::OverlayScope::Focus) {
        for (CaptureOverlay* candidate : overlays) {
            QObject::connect(candidate, &CaptureOverlay::activationRequested, &app, [&, candidate] {
                if (activeOverlay == candidate)
                    return;
                candidate->adoptInteractionState(*activeOverlay);
                activeOverlay->setOverlayActive(false);
                candidate->setOverlayActive(true);
                activeOverlay = candidate;
            });
        }
    }
    if (overlays.size() > 1) {
        for (CaptureOverlay* candidate : overlays) {
            QObject::connect(candidate, &CaptureOverlay::finishingStarted, &app, [&, candidate] {
                for (CaptureOverlay* peer : overlays)
                    if (peer != candidate)
                        peer->setOverlayActive(false);
            });
        }
    }

    for (CaptureOverlay* candidate : overlays) {
        candidate->show();
        candidate->raise();
    }
    overlay.activateWindow();
    overlay.raise();

    return app.exec();
}
