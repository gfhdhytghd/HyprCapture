#include "ui/scroll_capture.hpp"
#include "ui/clipboard_utils.hpp"
#include "ui/i18n.hpp"

#include <LayerShellQt/Window>
#include "shared/protocol.hpp"
#include "ui/mapped_image.hpp"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QCloseEvent>
#include <QGuiApplication>
#include <QHBoxLayout>

#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QRegion>
#include <QScreen>
#include <QWindow>
#include <algorithm>
#include <cmath>

namespace hyprcapture::ui {
namespace {
constexpr QSize controlsSize{540, 48};
constexpr qint64 maxFramePixels = 32LL * 1024 * 1024;
constexpr int maxDimension = 16384;
}

QRect scrollControlsPlacement(const QRect& capture, const QList<QRect>& screens, const QSize& controls) {
    if (!capture.isValid() || !controls.isValid())
        return {};
    for (const auto& screen : screens) {
        const QRegion free = QRegion(screen.adjusted(4, 4, -4, -4)).subtracted(QRegion(capture.adjusted(-6, -6, 6, 6)));
        for (const QRect& area : free) {
            if (area.width() >= controls.width() && area.height() >= controls.height())
                return QRect(area.topLeft(), controls);
        }
    }
    return {};
}

ScrollCaptureController::ScrollCaptureController()
    : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus) {
    setObjectName(QStringLiteral("scrollCaptureBar"));
    setWindowTitle(uiText("Scrolling capture"));
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFixedSize(controlsSize);
    const auto background = palette().color(QPalette::Window).name();
    const auto border = palette().color(QPalette::Mid).name();
    setStyleSheet(QStringLiteral("QWidget#scrollCaptureBar { background: %1; border: 1px solid %2; border-radius: 8px; }")
                      .arg(background, border));
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 5, 10, 5);
    layout->setSpacing(8);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("scrollCaptureStatus"));
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(m_status, 1);
    m_finish = new QPushButton(uiText("Finish"), this);
    m_finish->setObjectName(QStringLiteral("scrollCaptureFinish"));
    m_finish->setEnabled(false);
    m_finish->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(m_finish);
    auto* cancelButton = new QPushButton(uiText("Cancel"), this);
    cancelButton->setObjectName(QStringLiteral("scrollCaptureCancel"));
    cancelButton->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(cancelButton);
    connect(m_finish, &QPushButton::clicked, this, &ScrollCaptureController::finish);
    connect(cancelButton, &QPushButton::clicked, this, &ScrollCaptureController::cancel);
    m_interval.setSingleShot(true);
    m_timeout.setSingleShot(true);
    m_sessionTimeout.setSingleShot(true);
    connect(&m_interval, &QTimer::timeout, this, &ScrollCaptureController::sample);
    connect(&m_timeout, &QTimer::timeout, this, [this] { stop(uiText("Screen capture timed out")); });
    connect(&m_sessionTimeout, &QTimer::timeout, this, [this] { stop(uiText("Capture limit reached; finish or cancel")); });
    m_process = new QProcess(this);
    m_process->setProcessEnvironment(trustedProcessEnvironment());
    connect(m_process, &QProcess::readyReadStandardOutput, this, [this] {
        const auto bytes = m_process->readAllStandardOutput();
        constexpr qint64 limit = 64 * 1024;
        if (m_pixels.size() + bytes.size() > limit) {
            stop(uiText("Screen capture returned an invalid image"));
            return;
        }
        m_pixels += bytes;
    });
    connect(m_process, &QProcess::readyReadStandardError, this, [this] {
        // Drain without retaining unbounded backend diagnostics.
        m_stderr += m_process->readAllStandardError().left(std::max<qsizetype>(0, 4096 - m_stderr.size()));
    });
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (m_running && error == QProcess::FailedToStart)
            stop(uiText("Could not start screen capture"));
    });
    connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int code, QProcess::ExitStatus status) {
        m_timeout.stop();
        if (!m_running)
            return;
        if (code != 0 || status != QProcess::NormalExit) {
            stop(uiText("Screen capture failed; finish or cancel"));
            return;
        }
        QFile response(m_requestPath);
        if (!isPrivateRuntimeFile(m_requestPath, 64 * 1024) || !response.open(QIODevice::ReadOnly)) {
            stop(uiText("Screen capture failed; finish or cancel"));
            return;
        }
        const auto session = decodeSessionJson(response.readAll().toStdString());
        response.close();
        QFile::remove(m_requestPath);
        QImage frame;
        const QString artifact = m_requestPath + QStringLiteral(".rgba");
        if (session && session->monitors.size() == 1) {
            const auto& info = session->monitors.front();
            const QSize size(info.artifactWidth, info.artifactHeight);
            const qint64 bytes = static_cast<qint64>(size.width()) * size.height() * 4;
            if (QString::fromStdString(info.artifactPath) == artifact && info.artifactTopDown &&
                size == m_expectedSize && bytes > 0 && bytes <= maxFramePixels * 4 && isPrivateRuntimeFile(artifact, bytes)) {
                QFile file(artifact);
                if (file.open(QIODevice::ReadOnly) && file.size() == bytes) {
                    frame = mapRawRgba(file, size.width(), size.height());
                    if (frame.isNull()) {
                        const auto data = file.readAll();
                        if (data.size() == bytes)
                            frame = QImage(reinterpret_cast<const uchar*>(data.constData()), size.width(), size.height(),
                                           size.width() * 4, QImage::Format_RGBA8888).copy();
                    }
                }
            }
        }
        QFile::remove(artifact);
        if (frame.isNull()) {
            stop(uiText("Screen capture returned an invalid image"));
            return;
        }
        receiveFrame(frame);
        m_pixels.clear();
        if (m_running)
            m_interval.start(180);
    });
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] { stop(); });
    setStatus(uiText("Preparing scrolling capture…"));
}

ScrollCaptureController::~ScrollCaptureController() {
    stop();
    if (m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(1000);
    }
}

bool ScrollCaptureController::prepare(const QRect& capture, QString& error) {
    if (!QGuiApplication::platformName().startsWith(QStringLiteral("wayland"))) {
        error = uiText("Scrolling capture requires a Wayland session");
        return false;
    }
    m_hyprctl = trustedSystemProgram(QStringLiteral("hyprctl"));
    if (m_hyprctl.isEmpty()) {
        error = uiText("Scrolling capture requires the HyprCapture plugin");
        return false;
    }
    if (capture.width() < 64 || capture.height() < 96) {
        error = uiText("Select a larger scrolling area");
        return false;
    }
    const auto screens = QGuiApplication::screens();
    QList<QRect> geometries;
    for (auto* screen : screens) {
        geometries.push_back(screen->geometry());
        if (screen->geometry().contains(capture))
            m_captureScreen = screen;
    }
    if (!m_captureScreen) {
        error = uiText("Select a scrolling area within one monitor");
        return false;
    }
    // Prefer controls on the capture monitor when an unused band is available.
    geometries.removeAll(m_captureScreen->geometry());
    geometries.prepend(m_captureScreen->geometry());
    m_controls = scrollControlsPlacement(capture, geometries, size());
    if (!m_controls.isValid()) {
        error = uiText("Leave space above or below the selection for the capture controls");
        return false;
    }
    for (auto* screen : screens) {
        if (screen->geometry().contains(m_controls))
            m_controlsScreen = screen;
    }
    if (!m_controlsScreen)
        return false;
    m_capture = capture;
    m_scale = m_captureScreen->devicePixelRatio();
    const qreal pixelWidth = capture.width() * m_scale;
    const qreal pixelHeight = capture.height() * m_scale;
    if (!std::isfinite(m_scale) || m_scale <= 0.0 || pixelWidth > maxDimension || pixelHeight > maxDimension || pixelWidth * pixelHeight > maxFramePixels) {
        error = uiText("Select a smaller scrolling area");
        return false;
    }
    const auto templatePath = runtimeFile(QStringLiteral("scroll"), QStringLiteral("-XXXXXX"));
    if (templatePath.isEmpty()) {
        error = uiText("Could not start screen capture");
        return false;
    }
    m_exchange = std::make_unique<QTemporaryDir>(templatePath);
    if (!m_exchange->isValid()) {
        error = uiText("Could not start screen capture");
        return false;
    }
    m_requestPath = m_exchange->filePath(QStringLiteral("frame.json"));
    m_expectedSize = QSize(qRound(pixelWidth), qRound(pixelHeight));
    for (auto* screen : {m_captureScreen.data(), m_controlsScreen.data()}) {
        connect(screen, &QScreen::geometryChanged, this, [this] { stop(uiText("Display changed; finish or cancel")); });
        connect(screen, &QScreen::logicalDotsPerInchChanged, this, [this] { stop(uiText("Display changed; finish or cancel")); });
        connect(screen, &QObject::destroyed, this, [this] {
            if (!QCoreApplication::closingDown()) cancel();
        });
    }
    return true;
}

void ScrollCaptureController::configureLayer() {
    winId();
    windowHandle()->setScreen(m_controlsScreen);
    if (auto* layer = LayerShellQt::Window::get(windowHandle())) {
        layer->setScreen(m_controlsScreen);
        layer->setScope(QStringLiteral("hyprcapture-scroll"));
        layer->setLayer(LayerShellQt::Window::LayerOverlay);
        layer->setAnchors(LayerShellQt::Window::Anchors{LayerShellQt::Window::AnchorTop} | LayerShellQt::Window::AnchorLeft);
        layer->setExclusiveZone(-1);
        layer->setDesiredSize(size());
        const QPoint local = m_controls.topLeft() - m_controlsScreen->geometry().topLeft();
        layer->setMargins(QMargins(local.x(), local.y(), 0, 0));
        layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
        layer->setActivateOnShow(false);
        layer->setCloseOnDismissed(false);
    }
    move(m_controls.topLeft());
}

void ScrollCaptureController::start() {
    if (m_done || !m_captureScreen || !m_controlsScreen || m_hyprctl.isEmpty())
        return;
    configureLayer();
    show();
    m_running = true;
    m_sessionTimeout.start(5 * 60 * 1000);
    // The selector surfaces have just been hidden. Two stable live frames are
    // required below, so the frozen overlay cannot seed the stitched result.
    m_interval.start(150);
}

void ScrollCaptureController::sample() {
    if (!m_running || !m_captureScreen || m_process->state() != QProcess::NotRunning)
        return;
    m_pixels.clear();
    m_stderr.clear();
    RecordingRequest request;
    request.id = makeSessionId();
    request.mode = CaptureMode::Region;
    request.targetGeometry = {.x = static_cast<double>(m_capture.x()), .y = static_cast<double>(m_capture.y()),
                              .width = static_cast<double>(m_capture.width()), .height = static_cast<double>(m_capture.height())};
    QFile file(m_requestPath);
    const auto bytes = QByteArray::fromStdString(encodeRecordingRequestJson(request));
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) || file.write(bytes) != bytes.size()) {
        stop(uiText("Could not start screen capture"));
        return;
    }
    file.close();
    const auto literal = QString::fromUtf8(QJsonDocument(QJsonArray{m_requestPath}).toJson(QJsonDocument::Compact));
    const auto expression = QStringLiteral("hl.plugin.hyprcapture.region_capture(%1)").arg(literal.mid(1, literal.size() - 2));
    m_process->start(m_hyprctl, {QStringLiteral("eval"), expression});
    m_timeout.start(5000);
}

void ScrollCaptureController::receiveFrame(const QImage& frame) {
    if (m_frameSize.isEmpty())
        m_frameSize = frame.size();
    if (frame.size() != m_frameSize) {
        stop(uiText("Display changed; finish or cancel"));
        return;
    }
    const bool stable = ScrollStitcher::stable(m_candidate, frame);
    m_candidate = frame;
    if (!stable) {
        setStatus(uiText("Waiting for scrolling to stop…"));
        return;
    }
    const auto result = m_stitcher.append(frame);
    switch (result.status) {
        case ScrollStitcher::Status::Started:
        case ScrollStitcher::Status::Appended:
            m_finish->setEnabled(true);
            emit progress(m_stitcher.size(), m_stitcher.frameCount());
            [[fallthrough]];
        case ScrollStitcher::Status::Unchanged:
            setStatus(uiText("Scroll down slowly, then finish"));
            break;
        case ScrollStitcher::Status::NoOverlap:
        case ScrollStitcher::Status::Ambiguous:
            setStatus(uiText("Cannot align; scroll back slightly"));
            break;
        case ScrollStitcher::Status::LimitReached:
            stop(uiText("Capture limit reached; finish or cancel"));
            break;
        case ScrollStitcher::Status::SizeChanged:
            stop(uiText("Display changed; finish or cancel"));
            break;
        case ScrollStitcher::Status::InvalidFrame:
            stop(uiText("Screen capture returned an invalid image"));
            break;
    }
}

void ScrollCaptureController::setStatus(const QString& text) {
    const QString dimensions = m_stitcher.empty() ? uiText("Scrolling capture")
        : QStringLiteral("%1 × %2 · %3").arg(m_stitcher.size().width()).arg(m_stitcher.size().height())
              .arg(uiText("%1 captures").arg(m_stitcher.frameCount()));
    m_status->setText(dimensions + QLatin1Char('\n') + m_status->fontMetrics().elidedText(text, Qt::ElideRight, 310));
    m_status->setToolTip(text + QLatin1Char('\n') + uiText("Scroll down inside the selection. Keep overlap and exclude fixed headers."));
    m_status->setAccessibleName(dimensions + QLatin1Char(' ') + text);
}

void ScrollCaptureController::stop(const QString& reason) {
    m_running = false;
    m_interval.stop();
    m_timeout.stop();
    m_sessionTimeout.stop();
    // Remove the exchange before killing the client: a late compositor reply
    // cannot recreate files after its private parent directory has disappeared.
    m_exchange.reset();
    if (m_process && m_process->state() != QProcess::NotRunning)
        m_process->kill();
    if (!reason.isEmpty())
        setStatus(reason);
}

void ScrollCaptureController::finish() {
    if (m_done || m_stitcher.empty())
        return;
    stop();
    const auto image = m_stitcher.image();
    if (image.isNull()) {
        setStatus(uiText("Could not prepare the long screenshot"));
        return;
    }
    m_done = true;
    hide();
    emit completed(image);
}

void ScrollCaptureController::cancel() {
    if (m_done)
        return;
    stop();
    m_done = true;
    hide();
    emit cancelled();
}

void ScrollCaptureController::closeEvent(QCloseEvent* event) {
    cancel();
    event->accept();
}

} // namespace hyprcapture::ui
