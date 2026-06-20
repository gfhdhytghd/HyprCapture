#include "ui/result_thumbnail.hpp"

#include "ui/clipboard_utils.hpp"

#include <LayerShellQt/Window>

#include <QApplication>
#include <QCloseEvent>
#include <QContextMenuEvent>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusUnixFileDescriptor>
#include <QCursor>
#include <QDrag>
#include <QEasingCurve>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPalette>
#include <QPainter>
#include <QPropertyAnimation>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QScreen>
#include <QStyleHints>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#pragma push_macro("signals")
#undef signals
#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>
#pragma pop_macro("signals")

namespace {

constexpr int kThumbnailMaxWidth = 180;
constexpr int kThumbnailMaxHeight = 120;
constexpr qreal kThumbnailMaxDevicePixelRatio = 4.0;
constexpr int kThumbnailScreenMargin = 24;
constexpr int kTranscodeProgressRingSize = 64;
constexpr double kSwipeCloseThreshold = 120.0;
constexpr double kSwipeDeleteThreshold = 90.0;
constexpr int kMaxOpenWithApps = 16;

struct OpenWithApp {
    QString id;
    QString name;
    QIcon   icon;
};

QColor interpolateColor(const QColor& from, const QColor& to, double amount) {
    amount = std::clamp(amount, 0.0, 1.0);
    const auto mix = [amount](int a, int b) {
        return static_cast<int>(std::round(a * (1.0 - amount) + b * amount));
    };
    return QColor(mix(from.red(), to.red()), mix(from.green(), to.green()), mix(from.blue(), to.blue()), mix(from.alpha(), to.alpha()));
}

qreal thumbnailTargetDevicePixelRatio(const QPixmap& pixmap) {
    qreal dpr = std::max<qreal>(1.0, pixmap.devicePixelRatio());
    const QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (screen)
        dpr = std::max(dpr, screen->devicePixelRatio());
    return std::clamp(dpr, qreal{1.0}, kThumbnailMaxDevicePixelRatio);
}

QPixmap scaledThumbnailPixmap(const QPixmap& pixmap) {
    if (pixmap.isNull())
        return {};

    const QSizeF logicalPixmapSize = pixmap.deviceIndependentSize();
    QSize targetLogicalSize(std::max(1, static_cast<int>(std::ceil(logicalPixmapSize.width()))),
                            std::max(1, static_cast<int>(std::ceil(logicalPixmapSize.height()))));
    if (logicalPixmapSize.width() > kThumbnailMaxWidth || logicalPixmapSize.height() > kThumbnailMaxHeight)
        targetLogicalSize = targetLogicalSize.scaled(kThumbnailMaxWidth, kThumbnailMaxHeight, Qt::KeepAspectRatio);
    targetLogicalSize = targetLogicalSize.expandedTo(QSize(1, 1));

    qreal dpr = thumbnailTargetDevicePixelRatio(pixmap);
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

class SwipeBackdrop final : public QWidget {
  public:
    explicit SwipeBackdrop(QWidget* parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

    void setDeleteProgress(double progress) {
        m_deleteProgress = std::clamp(progress, 0.0, 1.0);
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        if (m_deleteProgress <= 0.0)
            return;

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QColor idle(92, 94, 98, 130);
        const QColor danger(220, 38, 38, 245);
        const QColor color = interpolateColor(idle, danger, m_deleteProgress);
        painter.setPen(QPen(color, 3.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);

        const QPointF center(width() / 2.0, height() / 2.0);
        const double scale = std::min(width(), height()) / 120.0;
        const double w = 34.0 * scale;
        const double h = 42.0 * scale;
        const QRectF body(center.x() - w / 2.0, center.y() - h / 2.0 + 5.0 * scale, w, h);
        painter.drawRoundedRect(body, 4.0 * scale, 4.0 * scale);
        painter.drawLine(QPointF(body.left() - 5.0 * scale, body.top() - 7.0 * scale), QPointF(body.right() + 5.0 * scale, body.top() - 7.0 * scale));
        painter.drawLine(QPointF(center.x() - 9.0 * scale, body.top() - 14.0 * scale), QPointF(center.x() + 9.0 * scale, body.top() - 14.0 * scale));
        painter.drawLine(QPointF(center.x() - 6.0 * scale, body.top() - 14.0 * scale), QPointF(center.x() - 10.0 * scale, body.top() - 7.0 * scale));
        painter.drawLine(QPointF(center.x() + 6.0 * scale, body.top() - 14.0 * scale), QPointF(center.x() + 10.0 * scale, body.top() - 7.0 * scale));
        painter.drawLine(QPointF(center.x() - 9.0 * scale, body.top() + 9.0 * scale), QPointF(center.x() - 9.0 * scale, body.bottom() - 8.0 * scale));
        painter.drawLine(QPointF(center.x(), body.top() + 9.0 * scale), QPointF(center.x(), body.bottom() - 8.0 * scale));
        painter.drawLine(QPointF(center.x() + 9.0 * scale, body.top() + 9.0 * scale), QPointF(center.x() + 9.0 * scale, body.bottom() - 8.0 * scale));
    }

  private:
    double m_deleteProgress = 0.0;
};

class TranscodeProgressOverlay final : public QWidget {
  public:
    explicit TranscodeProgressOverlay(QWidget* parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        connect(&m_spinTimer, &QTimer::timeout, this, [this] {
            m_rotation = std::fmod(m_rotation + 8.0, 360.0);
            update();
        });
        m_spinTimer.setInterval(33);
    }

    void setProgress(double progress) {
        m_failed = false;
        m_progress = std::clamp(progress, 0.0, 1.0);
        setVisible(true);
        if (!m_spinTimer.isActive())
            m_spinTimer.start();
        update();
    }

    void finish(bool success) {
        m_spinTimer.stop();
        m_progress = success ? 1.0 : m_progress;
        m_failed = !success;
        setVisible(!success);
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        if (!isVisible())
            return;

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillRect(rect(), QColor(0, 0, 0, 105));

        const int available = std::max(32, std::min(width(), height()) - 20);
        const int side = std::min(kTranscodeProgressRingSize, available);
        const QRectF ring((width() - side) / 2.0, (height() - side) / 2.0, side, side);
        const double penWidth = std::clamp(side / 13.0, 4.0, 9.0);

        painter.setPen(QPen(QColor(255, 255, 255, 72), penWidth, Qt::SolidLine, Qt::RoundCap));
        painter.drawArc(ring, 0, 360 * 16);

        if (!m_failed) {
            painter.setPen(QPen(QColor(59, 130, 246, 235), penWidth, Qt::SolidLine, Qt::RoundCap));
            painter.drawArc(ring, 90 * 16, static_cast<int>(-360 * 16 * m_progress));
            painter.setPen(QPen(QColor(147, 197, 253, 245), penWidth, Qt::SolidLine, Qt::RoundCap));
            painter.drawArc(ring, static_cast<int>((90.0 - m_rotation) * 16.0), -62 * 16);
        } else {
            painter.setPen(QPen(QColor(248, 113, 113, 235), penWidth, Qt::SolidLine, Qt::RoundCap));
            painter.drawArc(ring, 0, 360 * 16);
        }

        QFont font = painter.font();
        font.setBold(true);
        font.setPointSize(std::clamp(side / 5, 12, 28));
        painter.setFont(font);
        painter.setPen(QColor(255, 255, 255, 245));
        const QString text = m_failed ? QStringLiteral("Failed") : QStringLiteral("%1%").arg(static_cast<int>(std::round(m_progress * 100.0)));
        painter.drawText(ring, Qt::AlignCenter, text);
    }

  private:
    QTimer m_spinTimer;
    double m_progress = 0.0;
    double m_rotation = 0.0;
    bool   m_failed = false;
};

namespace {

QPointF wheelGestureDelta(QWheelEvent* event) {
    QPointF delta = event->pixelDelta();
    if (delta.isNull())
        delta = QPointF(event->angleDelta()) / 8.0;
    if (event->inverted())
        delta = -delta;
    return delta;
}

bool pathIsUnderDirectory(const QString& path, const QString& root) {
    if (path.isEmpty() || root.isEmpty())
        return false;

    const QString rootCanonical = QFileInfo(root).canonicalFilePath();
    const QFileInfo pathInfo(path);
    const QString pathCanonical = pathInfo.exists() ? pathInfo.canonicalFilePath() : QFileInfo(pathInfo.absolutePath()).canonicalFilePath();
    if (rootCanonical.isEmpty() || pathCanonical.isEmpty())
        return false;

    return pathCanonical == rootCanonical || pathCanonical.startsWith(rootCanonical + QLatin1Char('/'));
}

bool canDeleteThumbnailPath(const QString& path, const QString& deleteRoot) {
    const QFileInfo info(path);
    if (path.isEmpty())
        return false;
    if (hyprcapture::ui::isPrivateRuntimePath(path))
        return true;
    if (!pathIsUnderDirectory(path, deleteRoot))
        return false;
    return !info.exists() || info.isFile();
}

QIcon iconFromGIcon(GIcon* icon) {
    if (!icon || !G_IS_THEMED_ICON(icon))
        return {};

    const char* const* names = g_themed_icon_get_names(G_THEMED_ICON(icon));
    if (!names)
        return {};

    for (int i = 0; names[i]; ++i) {
        const QIcon themed = QIcon::fromTheme(QString::fromUtf8(names[i]));
        if (!themed.isNull())
            return themed;
    }
    return {};
}

void appendAppInfo(std::vector<OpenWithApp>& apps, GAppInfo* app) {
    if (!app || apps.size() >= kMaxOpenWithApps)
        return;

    const char* id = g_app_info_get_id(app);
    if (!id || !*id)
        return;

    const QString appId = QString::fromUtf8(id);
    if (std::any_of(apps.begin(), apps.end(), [&appId](const OpenWithApp& existing) { return existing.id == appId; }))
        return;

    const char* displayName = g_app_info_get_display_name(app);
    if (!displayName || !*displayName)
        displayName = g_app_info_get_name(app);
    apps.push_back({.id = appId, .name = QString::fromUtf8(displayName ? displayName : id), .icon = iconFromGIcon(g_app_info_get_icon(app))});
}

std::vector<OpenWithApp> openWithAppsForPath(const QString& path) {
    std::vector<OpenWithApp> apps;
    const QString canonical = QFileInfo(path).canonicalFilePath();
    if (canonical.isEmpty())
        return apps;

    GFile* file = g_file_new_for_path(QFile::encodeName(canonical).constData());
    if (!file)
        return apps;

    GError* error = nullptr;
    GFileInfo* info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE, G_FILE_QUERY_INFO_NONE, nullptr, &error);
    if (error) {
        g_error_free(error);
        error = nullptr;
    }

    const char* contentType = info ? g_file_info_get_content_type(info) : nullptr;
    if (contentType && *contentType) {
        GAppInfo* defaultApp = g_app_info_get_default_for_type(contentType, false);
        appendAppInfo(apps, defaultApp);
        if (defaultApp)
            g_object_unref(defaultApp);

        GList* allApps = g_app_info_get_all_for_type(contentType);
        for (GList* node = allApps; node && apps.size() < kMaxOpenWithApps; node = node->next)
            appendAppInfo(apps, G_APP_INFO(node->data));
        g_list_free_full(allApps, g_object_unref);
    }

    if (info)
        g_object_unref(info);
    g_object_unref(file);
    return apps;
}

} // namespace

ResultThumbnail::ResultThumbnail(const QPixmap& pixmap, QString path, QString restoreClipboardPath, QString deleteRoot, int timeoutMs, bool copyFile, QWidget* parent)
    : QWidget(parent), m_path(std::move(path)), m_restoreClipboardPath(std::move(restoreClipboardPath)), m_deleteRoot(std::move(deleteRoot)), m_copyFile(copyFile) {
    setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus);
    setFocusPolicy(Qt::NoFocus);
    setObjectName("thumbnail");
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_StyledBackground);

    const auto palette = QApplication::palette();
    const QColor bg = palette.color(QPalette::Window);
    const QColor fg = palette.color(QPalette::WindowText);
    const QColor highlight = palette.color(QPalette::Highlight);
    setStyleSheet(QStringLiteral(
                      "#thumbnail { background: transparent; border: none; }"
                      "#thumbnailImage { background: transparent; border: none; }"
                      "#thumbnailMenu { background: rgba(%1,%2,%3,242); border: 1px solid rgba(%4,%5,%6,90); border-radius: 7px; }"
                      "#thumbnailOpenWithMenu { background: rgba(%1,%2,%3,242); border: 1px solid rgba(%4,%5,%6,90); border-radius: 7px; }"
                      "#thumbnailMenu QPushButton { color: rgba(%4,%5,%6,255); background: transparent; padding: 7px 10px; border: none; border-radius: 5px; text-align: left; }"
                      "#thumbnailOpenWithMenu QPushButton { color: rgba(%4,%5,%6,255); background: transparent; padding: 7px 10px; border: none; border-radius: 5px; text-align: left; }"
                      "#thumbnailMenu QPushButton:hover { background: rgba(%7,%8,%9,75); }"
                      "#thumbnailOpenWithMenu QPushButton:hover { background: rgba(%7,%8,%9,75); }")
                      .arg(bg.red()).arg(bg.green()).arg(bg.blue()).arg(fg.red()).arg(fg.green()).arg(fg.blue()).arg(highlight.red()).arg(highlight.green()).arg(highlight.blue()));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    m_menuShell = new QWidget(this);
    m_menuShell->setObjectName("thumbnailMenuShell");
    auto* shellLayout = new QHBoxLayout(m_menuShell);
    shellLayout->setContentsMargins(0, 0, 0, 0);
    shellLayout->setSpacing(6);

    m_menuPanel = new QWidget(m_menuShell);
    m_menuPanel->setObjectName("thumbnailMenu");
    m_menuPanel->setAttribute(Qt::WA_StyledBackground);
    auto* menuLayout = new QVBoxLayout(m_menuPanel);
    menuLayout->setContentsMargins(6, 6, 6, 6);
    menuLayout->setSpacing(2);
    const auto addAction = [&](const QString& text, auto callback, bool hideMenu = true, const QIcon& icon = {}) {
        auto* buttonWidget = new QPushButton(text, m_menuPanel);
        if (!icon.isNull())
            buttonWidget->setIcon(icon);
        connect(buttonWidget, &QPushButton::clicked, this, [this, hideMenu, callback = std::move(callback)]() mutable {
            if (hideMenu)
                setMenuVisible(false);
            callback();
        });
        menuLayout->addWidget(buttonWidget);
    };
    addAction("Open", [this] {
        if (!m_path.isEmpty() && openPath(m_path))
            close();
    });
    addAction("Open with", [this] {
        if (m_openWithPanel)
            m_openWithPanel->setVisible(!m_openWithPanel->isVisible());
        applyLayerSize();
    }, false);
    addAction(m_copyFile ? "Copy file" : "Copy image", [this] {
        const auto currentPixmap = m_imageLabel->pixmap();
        if (m_copyFile && !m_path.isEmpty())
            hyprcapture::ui::copyFileUrlToClipboard(m_path);
        else if (!currentPixmap.isNull())
            hyprcapture::ui::copyPixmapToClipboard(currentPixmap);
    });
    addAction("Show in folder", [this] {
        if (!m_path.isEmpty())
            openPath(QFileInfo(m_path).absolutePath());
    });
    if (canDeleteThumbnailPath(m_path, m_deleteRoot))
        addAction("Delete", [this] { deleteAndClose(); });
    addAction("Close", [this] { close(); });
    m_menuPanel->setFixedSize(m_menuPanel->sizeHint());

    m_openWithPanel = new QWidget(m_menuShell);
    m_openWithPanel->setObjectName("thumbnailOpenWithMenu");
    m_openWithPanel->setAttribute(Qt::WA_StyledBackground);
    auto* openWithLayout = new QVBoxLayout(m_openWithPanel);
    openWithLayout->setContentsMargins(6, 6, 6, 6);
    openWithLayout->setSpacing(2);
    for (const auto& app : openWithAppsForPath(m_path)) {
        auto* appButton = new QPushButton(app.name, m_openWithPanel);
        if (!app.icon.isNull())
            appButton->setIcon(app.icon);
        connect(appButton, &QPushButton::clicked, this, [this, appId = app.id] {
            setMenuVisible(false);
            if (openWithApp(appId, m_path))
                close();
        });
        openWithLayout->addWidget(appButton);
    }
    auto* otherButton = new QPushButton("Other applications...", m_openWithPanel);
    connect(otherButton, &QPushButton::clicked, this, [this] {
        setMenuVisible(false);
        if (!m_path.isEmpty() && openWithPortal(m_path))
            close();
    });
    openWithLayout->addWidget(otherButton);
    m_openWithPanel->hide();
    shellLayout->addWidget(m_openWithPanel, 0, Qt::AlignBottom);
    shellLayout->addWidget(m_menuPanel, 0, Qt::AlignBottom);

    m_menuShell->hide();
    layout->addWidget(m_menuShell, 0, Qt::AlignRight);

    QPixmap scaledPixmap = scaledThumbnailPixmap(pixmap);
    const QSize scaledLogicalSize = scaledPixmap.deviceIndependentSize().toSize();
    m_card = new QWidget(this);
    m_card->setObjectName("thumbnailImageCard");
    m_card->setFixedSize((scaledLogicalSize + QSize(kThumbnailScreenMargin, kThumbnailScreenMargin)).expandedTo(QSize(1, 1)));

    m_swipeBackdrop = new SwipeBackdrop(m_card);
    m_swipeBackdrop->setGeometry(QRect(QPoint(0, 0), scaledLogicalSize));
    m_swipeBackdrop->lower();

    m_imageLabel = new QLabel(m_card);
    m_imageLabel->setObjectName("thumbnailImage");
    m_imageLabel->setAttribute(Qt::WA_StyledBackground);
    m_imageLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_imageLabel->setPixmap(scaledPixmap);
    m_imageLabel->setGeometry(QRect(QPoint(0, 0), scaledLogicalSize));
    m_transcodeOverlay = new TranscodeProgressOverlay(m_imageLabel);
    m_transcodeOverlay->setGeometry(m_imageLabel->rect());
    m_transcodeOverlay->hide();
    m_imageOrigin = m_imageLabel->pos();
    m_imageLabel->raise();
    layout->addWidget(m_card, 0, Qt::AlignRight);

    adjustSize();
    winId();
    if (auto* layerWindow = LayerShellQt::Window::get(windowHandle())) {
        layerWindow->setScope("hyprcapture-thumbnail");
        layerWindow->setLayer(LayerShellQt::Window::LayerOverlay);
        layerWindow->setAnchors(LayerShellQt::Window::Anchors{LayerShellQt::Window::AnchorRight} | LayerShellQt::Window::AnchorBottom);
        layerWindow->setMargins(QMargins(0, 0, 0, 0));
        layerWindow->setExclusiveZone(0);
        layerWindow->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
        layerWindow->setActivateOnShow(false);
        layerWindow->setDesiredSize(size());
    }

    m_closeTimer.setSingleShot(true);
    connect(&m_closeTimer, &QTimer::timeout, this, &QWidget::close);
    startCloseTimer(timeoutMs);

    m_swipeResetTimer.setSingleShot(true);
    connect(&m_swipeResetTimer, &QTimer::timeout, this, &ResultThumbnail::resetSwipe);
}

void ResultThumbnail::closeEvent(QCloseEvent* event) {
    QWidget::closeEvent(event);
    hyprcapture::ui::discardClipboardSnapshot(m_restoreClipboardPath);
    m_restoreClipboardPath.clear();
    qApp->quit();
}

void ResultThumbnail::mousePressEvent(QMouseEvent* event) {
    if (m_transcodeProgressActive)
        return;
    if (event->button() != Qt::LeftButton)
        return;

    m_dragMoved = false;
    m_draggingFile = false;
    m_pressGlobal = event->globalPosition().toPoint();
}

void ResultThumbnail::mouseMoveEvent(QMouseEvent* event) {
    if (m_transcodeProgressActive)
        return;
    if (!(event->buttons() & Qt::LeftButton) || m_draggingFile)
        return;

    const QPoint current = event->globalPosition().toPoint();
    const QPoint delta = current - m_pressGlobal;
    if (delta.manhattanLength() <= QGuiApplication::styleHints()->startDragDistance())
        return;

    m_dragMoved = true;
    startFileDrag();
}

void ResultThumbnail::mouseReleaseEvent(QMouseEvent* event) {
    if (m_transcodeProgressActive)
        return;
    if (event->button() == Qt::LeftButton && !m_dragMoved && !m_path.isEmpty() && openPath(m_path))
        close();
}

void ResultThumbnail::contextMenuEvent(QContextMenuEvent* event) {
    event->accept();
    if (m_transcodeProgressActive)
        return;
    toggleMenu();
}

void ResultThumbnail::wheelEvent(QWheelEvent* event) {
    if (m_transcodeProgressActive)
        return;
    if (m_swipeCompleting)
        return;

    const QPointF delta = wheelGestureDelta(event);
    if (delta.manhattanLength() <= 0.0)
        return;

    m_closeTimer.stop();
    m_swipeResetTimer.stop();
    m_swipeOffset += delta;
    const int imageWidth = m_imageLabel ? m_imageLabel->width() : m_card->width();
    const int imageHeight = m_imageLabel ? m_imageLabel->height() : m_card->height();

    if (std::abs(m_swipeOffset.x()) >= std::abs(m_swipeOffset.y())) {
        m_swipeOffset.setX(std::clamp(m_swipeOffset.x(), 0.0, static_cast<double>(m_card->width() + 80)));
        m_swipeOffset.setY(0.0);
    } else {
        m_swipeOffset.setX(0.0);
        m_swipeOffset.setY(std::clamp(m_swipeOffset.y(), 0.0, static_cast<double>(m_card->height() + 80)));
    }
    updateSwipeVisual();
    event->accept();

    if (m_swipeOffset.x() >= std::min(kSwipeCloseThreshold, imageWidth * 0.55))
        animateSwipeOut(SwipeAction::Close);
    else if (canDeleteThumbnailPath(m_path, m_deleteRoot) && m_swipeOffset.y() >= std::min(kSwipeDeleteThreshold, imageHeight * 0.55))
        animateSwipeOut(SwipeAction::Delete);
    else
        m_swipeResetTimer.start(180);
}

bool ResultThumbnail::openPath(const QString& path) {
    QProcess process;
    auto environment = hyprcapture::ui::trustedProcessEnvironment();
    environment.remove("QT_WAYLAND_SHELL_INTEGRATION");
    process.setProcessEnvironment(environment);
    const QString program = hyprcapture::ui::trustedSystemProgram(QStringLiteral("xdg-open"));
    if (program.isEmpty())
        return false;
    process.setProgram(program);
    process.setArguments({path});
    return process.startDetached();
}

bool ResultThumbnail::openWithPortal(const QString& path) {
    const QString canonical = QFileInfo(path).canonicalFilePath();
    if (canonical.isEmpty())
        return false;

    QFile file(canonical);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    QVariantMap options;
    options.insert(QStringLiteral("ask"), true);

    QDBusMessage message = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.portal.Desktop"),
                                                          QStringLiteral("/org/freedesktop/portal/desktop"),
                                                          QStringLiteral("org.freedesktop.portal.OpenURI"),
                                                          QStringLiteral("OpenFile"));
    message << QString{} << QVariant::fromValue(QDBusUnixFileDescriptor(file.handle())) << options;

    const QDBusMessage reply = QDBusConnection::sessionBus().call(message, QDBus::Block, 1000);
    return reply.type() == QDBusMessage::ReplyMessage;
}

bool ResultThumbnail::openWithApp(const QString& appId, const QString& path) {
    const QString canonical = QFileInfo(path).canonicalFilePath();
    if (appId.isEmpty() || canonical.isEmpty())
        return false;

    GDesktopAppInfo* app = g_desktop_app_info_new(appId.toUtf8().constData());
    if (!app)
        return false;

    GFile* file = g_file_new_for_path(QFile::encodeName(canonical).constData());
    if (!file) {
        g_object_unref(app);
        return false;
    }

    GList* files = g_list_append(nullptr, file);
    GAppLaunchContext* context = g_app_launch_context_new();
    g_app_launch_context_unsetenv(context, "QT_WAYLAND_SHELL_INTEGRATION");
    GError* error = nullptr;
    const bool launched = g_app_info_launch(G_APP_INFO(app), files, context, &error);
    if (error)
        g_error_free(error);
    g_list_free(files);
    g_object_unref(context);
    g_object_unref(file);
    g_object_unref(app);
    return launched;
}

void ResultThumbnail::toggleMenu() {
    m_closeTimer.stop();
    setMenuVisible(!m_menuShell->isVisible());
}

void ResultThumbnail::setMenuVisible(bool visible) {
    if (m_menuShell)
        m_menuShell->setVisible(visible);
    if (m_openWithPanel && !visible)
        m_openWithPanel->hide();
    applyLayerSize();
    if (!visible && m_closeTimer.interval() > 0)
        m_closeTimer.start();
}

void ResultThumbnail::applyLayerSize() {
    adjustSize();
    if (auto* layerWindow = LayerShellQt::Window::get(windowHandle()))
        layerWindow->setDesiredSize(size());
}

void ResultThumbnail::updateSwipeVisual() {
    if (!m_imageLabel || !m_swipeBackdrop)
        return;

    m_imageLabel->move(m_imageOrigin + QPoint(static_cast<int>(std::round(m_swipeOffset.x())), static_cast<int>(std::round(m_swipeOffset.y()))));
    m_swipeBackdrop->setDeleteProgress(m_swipeOffset.y() / std::min(kSwipeDeleteThreshold, m_imageLabel->height() * 0.55));
}

void ResultThumbnail::resetSwipe() {
    if (m_swipeCompleting || !m_imageLabel)
        return;

    auto* animation = new QPropertyAnimation(m_imageLabel, "pos", this);
    animation->setDuration(160);
    animation->setStartValue(m_imageLabel->pos());
    animation->setEndValue(m_imageOrigin);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(animation, &QPropertyAnimation::finished, this, [this] {
        m_swipeOffset = {};
        if (m_swipeBackdrop)
            m_swipeBackdrop->setDeleteProgress(0.0);
        if (m_closeTimer.interval() > 0)
            m_closeTimer.start();
    });
    animation->start(QAbstractAnimation::DeleteWhenStopped);
}

void ResultThumbnail::animateSwipeOut(SwipeAction action) {
    if (m_swipeCompleting || !m_imageLabel || !m_card)
        return;

    m_swipeCompleting = true;
    m_swipeResetTimer.stop();
    m_closeTimer.stop();

    const QPoint target = action == SwipeAction::Close ? QPoint(m_card->width() + 24, m_imageOrigin.y()) : QPoint(m_imageOrigin.x(), m_card->height() + 24);
    auto* animation = new QPropertyAnimation(m_imageLabel, "pos", this);
    animation->setDuration(190);
    animation->setStartValue(m_imageLabel->pos());
    animation->setEndValue(target);
    animation->setEasingCurve(QEasingCurve::InCubic);
    connect(animation, &QPropertyAnimation::finished, this, [this, action] {
        if (action == SwipeAction::Delete)
            deleteAndClose();
        else
            close();
    });
    animation->start(QAbstractAnimation::DeleteWhenStopped);
}

void ResultThumbnail::deleteAndClose() {
    if (!canDeleteThumbnailPath(m_path, m_deleteRoot))
        return;
    if (!QFile::remove(m_path))
        return;

    restoreClipboard();
    close();
}

void ResultThumbnail::restoreClipboard() const {
    hyprcapture::ui::restoreClipboardSnapshot(m_restoreClipboardPath);
}

void ResultThumbnail::startFileDrag() {
    if (m_transcodeProgressActive)
        return;
    if (m_path.isEmpty())
        return;

    m_draggingFile = true;
    m_closeTimer.stop();

    auto* drag = new QDrag(this);
    auto* mimeData = new QMimeData;
    mimeData->setUrls({QUrl::fromLocalFile(m_path)});

    const auto currentPixmap = m_imageLabel->pixmap();
    if (!currentPixmap.isNull()) {
        mimeData->setImageData(currentPixmap.toImage());
        drag->setPixmap(currentPixmap.scaled(180, 120, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        drag->setHotSpot(QPoint(drag->pixmap().width() / 2, drag->pixmap().height() / 2));
    }

    drag->setMimeData(mimeData);
    const auto action = drag->exec(Qt::CopyAction, Qt::CopyAction);
    m_draggingFile = false;
    if (action != Qt::IgnoreAction)
        close();
}

void ResultThumbnail::enterEvent(QEnterEvent*) {
    m_closeTimer.stop();
}

void ResultThumbnail::leaveEvent(QEvent*) {
    if (m_closeTimer.interval() > 0)
        m_closeTimer.start();
}

void ResultThumbnail::setImagePixmap(const QPixmap& pixmap) {
    if (!m_imageLabel || pixmap.isNull())
        return;
    m_imageLabel->setPixmap(pixmap);
}

void ResultThumbnail::setTranscodeProgress(double progress) {
    m_transcodeProgressActive = true;
    setMenuVisible(false);
    m_closeTimer.stop();
    if (m_transcodeOverlay)
        m_transcodeOverlay->setProgress(progress);
}

void ResultThumbnail::finishTranscodeProgress(bool success, int timeoutMs) {
    m_transcodeProgressActive = !success;
    if (m_transcodeOverlay)
        m_transcodeOverlay->finish(success);
    startCloseTimer(timeoutMs);
}

void ResultThumbnail::startCloseTimer(int timeoutMs) {
    m_closeTimer.stop();
    if (timeoutMs > 0) {
        m_closeTimer.setInterval(timeoutMs);
        m_closeTimer.start();
    }
}
