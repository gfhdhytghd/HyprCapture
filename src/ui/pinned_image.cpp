#include "ui/pinned_image.hpp"

#include "ui/clipboard_utils.hpp"

#include <LayerShellQt/Window>

#include <QApplication>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QCursor>
#include <QFile>
#include <QImageReader>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPointer>
#include <QRegion>
#include <QScreen>
#include <QWheelEvent>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr qint64 kMaxSourceBytes = 128LL * 1024 * 1024;
constexpr qint64 kMaxImagePixels = 64LL * 1024 * 1024;
constexpr int kMaxDimension = 32768;
constexpr int kCloseButtonSize = 28;

QString pinText(const char* text) {
    return QCoreApplication::translate("PinnedImage", text);
}

class PinSurface;

struct PinState {
    QImage image;
    QPointF origin;
    qreal zoom = 1.0;
    QSizeF logicalImageSize;
    std::vector<QPointer<PinSurface>> surfaces;
    bool closing = false;

    QRectF imageRect() const {
        return QRectF(origin, logicalImageSize * zoom);
    }
    QRectF closeRect() const {
        const auto image = imageRect();
        return QRectF(image.right() - kCloseButtonSize - 4, image.top() + 4, kCloseButtonSize, kCloseButtonSize);
    }
    void refresh();
    void close();
};

// Layer-shell surfaces cannot be freely moved between outputs. Each pin has a
// transparent surface on each output; every surface paints the same image in
// desktop coordinates and limits input to the image's local visible portion.
// This also allows one image to straddle two outputs while being dragged.
class PinSurface final : public QWidget {
  public:
    PinSurface(std::shared_ptr<PinState> state, QScreen* screen, QWidget* owner = nullptr)
        : QWidget(owner, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint), m_state(std::move(state)) {
        setObjectName(QStringLiteral("hyprcapturePinnedImage"));
        setWindowTitle(pinText("Pinned image"));
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        setAccessibleDescription(pinText("Drag to move · Scroll to zoom · Esc to close"));
        setTargetScreen(screen);
    }

    void refresh() {
        if (!m_screen)
            return;
        const QRectF localImage = m_state->imageRect().translated(-m_screen->geometry().topLeft());
        const QRect visibleImage = localImage.toAlignedRect().intersected(rect());
        const QRect visibleClose = m_state->closeRect().translated(-m_screen->geometry().topLeft()).toAlignedRect().intersected(rect());
        // An empty window mask clears the mask and would accept input across
        // the entire output. Use a nonempty region outside the surface instead.
        const QRegion input = QRegion(visibleImage) | QRegion(visibleClose);
        // Set QWindow's input region directly. A QWidget mask also clips the
        // backing-store repaint, preventing us from clearing the old position.
        const QRegion mask = input.isEmpty() ? QRegion(QRect(-2, -2, 1, 1)) : input;
        if (windowHandle()->mask() != mask) windowHandle()->setMask(mask);
        const QRect painted = input.isEmpty() ? QRect() : visibleImage.united(visibleClose).adjusted(-2, -2, 2, 2).intersected(rect());
        const QRegion damage = QRegion(m_paintedBounds) | QRegion(painted);
        m_paintedBounds = painted;
        if (!damage.isEmpty()) update(damage);
    }

    QScreen* targetScreen() const { return m_screen.data(); }

    void setTargetScreen(QScreen* screen) {
        QObject::disconnect(m_geometryConnection);
        m_screen = screen;
        configureScreen();
        m_geometryConnection = connect(screen, &QScreen::geometryChanged, this, [this] {
            configureScreen();
            refresh();
        });
    }

  protected:
    void paintEvent(QPaintEvent* event) override {
        if (!m_screen)
            return;
        QPainter painter(this);
        painter.setClipRegion(event->region());
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect(), Qt::transparent);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.translate(-m_screen->geometry().topLeft());
        painter.drawImage(m_state->imageRect(), m_state->image);

        const QRectF close = m_state->closeRect();
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(m_hoverClose ? QColor(210, 45, 55, 235) : QColor(30, 32, 36, 205));
        painter.drawRoundedRect(close, 7, 7);
        painter.setPen(QPen(Qt::white, 1.8, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(close.topLeft() + QPointF(9, 9), close.bottomRight() - QPointF(9, 9));
        painter.drawLine(close.topRight() + QPointF(-9, 9), close.bottomLeft() + QPointF(9, -9));
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(event);
            return;
        }
        // Hover must not steal focus from the desktop behind the pin.
        // Explicit clicks opt in so Esc remains available after interaction.
        if (auto* layer = LayerShellQt::Window::get(windowHandle()))
            layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityOnDemand);
        windowHandle()->requestActivate();
        setFocus(Qt::MouseFocusReason);
        m_pressClose = m_state->closeRect().contains(desktopPosition(event->position()));
        m_dragging = !m_pressClose;
        m_dragOffset = desktopPosition(event->position()) - m_state->origin;
        if (m_dragging)
            setCursor(Qt::ClosedHandCursor);
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        const QPointF position = desktopPosition(event->position());
        if (m_dragging && (event->buttons() & Qt::LeftButton)) {
            m_state->origin = position - m_dragOffset;
            m_state->refresh();
        } else {
            const bool hoverClose = m_state->closeRect().contains(position);
            setCursor(hoverClose ? Qt::ArrowCursor : Qt::OpenHandCursor);
            if (hoverClose != m_hoverClose) {
                m_hoverClose = hoverClose;
                update(closeDamage());
            }
        }
        event->accept();
    }

    void leaveEvent(QEvent* event) override {
        if (m_hoverClose) { m_hoverClose = false; update(closeDamage()); }
        if (!m_dragging) {
            if (auto* layer = LayerShellQt::Window::get(windowHandle()))
                layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
        }
        QWidget::leaveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) {
            QWidget::mouseReleaseEvent(event);
            return;
        }
        const bool closeRequested = m_pressClose && m_state->closeRect().contains(desktopPosition(event->position()));
        m_pressClose = false;
        m_dragging = false;
        unsetCursor();
        if (closeRequested)
            m_state->close();
        event->accept();
    }

    void wheelEvent(QWheelEvent* event) override {
        qreal delta = event->angleDelta().y() / 120.0;
        if (qFuzzyIsNull(delta))
            delta = event->pixelDelta().y() / 80.0;
        if (qFuzzyIsNull(delta)) {
            event->ignore();
            return;
        }
        const qreal longestSide = std::max(m_state->logicalImageSize.width(), m_state->logicalImageSize.height());
        const qreal minimumZoom = std::min<qreal>(1, 48.0 / longestSide);
        const qreal zoom = std::clamp(m_state->zoom * std::pow(1.15, delta), minimumZoom, qreal{8.0});
        const QPointF anchor = desktopPosition(event->position());
        m_state->origin = anchor - (anchor - m_state->origin) * (zoom / m_state->zoom);
        m_state->zoom = zoom;
        m_state->refresh();
        event->accept();
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Escape) {
            m_state->close();
            event->accept();
            return;
        }
        QWidget::keyPressEvent(event);
    }

    void closeEvent(QCloseEvent* event) override {
        m_state->close();
        event->accept();
    }

  private:
    QRect closeDamage() const {
        if (!m_screen) return {};
        return m_state->closeRect().translated(-m_screen->geometry().topLeft()).toAlignedRect().adjusted(-2, -2, 2, 2).intersected(rect());
    }
    QPointF desktopPosition(const QPointF& local) const {
        return local + (m_screen ? m_screen->geometry().topLeft() : QPoint());
    }

    void configureScreen() {
        if (!m_screen)
            return;
        resize(m_screen->geometry().size());
        winId();
        windowHandle()->setScreen(m_screen);
        if (auto* layer = LayerShellQt::Window::get(windowHandle())) {
            layer->setScreen(m_screen);
            layer->setScope(QStringLiteral("hyprcapture-pin"));
            layer->setLayer(LayerShellQt::Window::LayerOverlay);
            layer->setAnchors(LayerShellQt::Window::Anchors{LayerShellQt::Window::AnchorTop} |
                              LayerShellQt::Window::AnchorBottom | LayerShellQt::Window::AnchorLeft | LayerShellQt::Window::AnchorRight);
            layer->setExclusiveZone(-1);
            layer->setMargins(QMargins());
            layer->setDesiredSize(QSize(0, 0));
            layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
            layer->setActivateOnShow(false);
            layer->setCloseOnDismissed(false);
        }
    }

    std::shared_ptr<PinState> m_state;
    QPointer<QScreen> m_screen;
    QMetaObject::Connection m_geometryConnection;
    QPointF m_dragOffset;
    QRect m_paintedBounds;
    bool m_dragging = false;
    bool m_pressClose = false;
    bool m_hoverClose = false;
};

void PinState::refresh() {
    for (const auto& surface : surfaces) {
        if (surface)
            surface->refresh();
    }
}

void PinState::close() {
    if (closing)
        return;
    closing = true;
    for (const auto& surface : surfaces) {
        if (surface)
            surface->hide();
    }
    qApp->quit();
}

bool consumeImageSource(const QString& path, const struct stat& opened) {
    if (!hyprcapture::ui::isPrivateRuntimePath(path) || opened.st_uid != geteuid())
        return false;
    struct stat current {};
    const QByteArray native = QFile::encodeName(path);
    if (lstat(native.constData(), &current) != 0 || !S_ISREG(current.st_mode) || current.st_uid != geteuid() ||
        current.st_dev != opened.st_dev || current.st_ino != opened.st_ino || current.st_nlink != 1)
        return false;
    return QFile::remove(path);
}

} // namespace

QWidget* createPinnedImage(const QString& path, bool consumePrivateRuntimeSource, QString* error, QScreen* targetScreen) {
    if (error)
        error->clear();
    const auto fail = [error](const char* message) -> QWidget* {
        if (error)
            *error = pinText(message);
        return nullptr;
    };
    QFile source(path);
    if (!source.open(QIODevice::ReadOnly))
        return fail("Unable to open pinned image");
    struct stat opened {};
    if (fstat(source.handle(), &opened) != 0 || !S_ISREG(opened.st_mode) || opened.st_size < 1 || opened.st_size > kMaxSourceBytes)
        return fail("Invalid pinned image");
    QImageReader reader(&source, "png");
    reader.setAutoTransform(true);
    const QSize imageSize = reader.size();
    if (imageSize.isEmpty() || imageSize.width() > kMaxDimension || imageSize.height() > kMaxDimension ||
        qint64(imageSize.width()) * imageSize.height() > kMaxImagePixels)
        return fail("Pinned image is too large");
    QImage image = reader.read();
    if (image.isNull())
        return fail("Unable to read pinned image");
    const auto screens = QGuiApplication::screens();
    if (screens.isEmpty())
        return fail("No screen available for pinned image");
    if (!screens.contains(targetScreen))
        targetScreen = QGuiApplication::screenAt(QCursor::pos());
    if (!targetScreen)
        targetScreen = QGuiApplication::primaryScreen();
    if (!targetScreen)
        targetScreen = screens.front();

    auto state = std::make_shared<PinState>();
    state->image = std::move(image);
    // Saved screenshots contain physical pixels but PNG need not retain Qt's
    // DPR. Start at the target output's native pixel density, then fit as needed.
    state->logicalImageSize = QSizeF(state->image.size()) / std::max<qreal>(1, targetScreen->devicePixelRatio());
    const QRect available = targetScreen->availableGeometry();
    state->zoom = std::min({qreal{1}, available.width() * 0.65 / state->logicalImageSize.width(),
                           available.height() * 0.65 / state->logicalImageSize.height()});
    state->origin = QPointF(available.center()) - QPointF(state->logicalImageSize.width(), state->logicalImageSize.height()) * (state->zoom / 2);

    auto* root = new PinSurface(state, targetScreen);
    state->surfaces.emplace_back(root);
    for (QScreen* screen : screens) {
        if (screen != targetScreen)
            state->surfaces.emplace_back(new PinSurface(state, screen, root));
    }
    QObject::connect(qApp, &QGuiApplication::screenAdded, root, [state, root](QScreen* screen) {
        for (const auto& existing : state->surfaces) {
            if (existing && existing->targetScreen() == screen && existing->isVisible())
                return;
        }
        auto* surface = new PinSurface(state, screen, root);
        state->surfaces.emplace_back(surface);
        surface->refresh();
        surface->show();
    });
    QObject::connect(qApp, &QGuiApplication::screenRemoved, root, [state, root](QScreen* screen) {
        const auto remaining = QGuiApplication::screens();
        if (root->targetScreen() == screen && !remaining.isEmpty()) {
            QScreen* replacement = remaining.front();
            for (const auto& surface : state->surfaces) {
                if (surface && surface != root && surface->targetScreen() == replacement)
                    surface->hide();
            }
            root->setTargetScreen(replacement);
            root->show();
        }
        for (const auto& surface : state->surfaces) {
            if (surface && surface->targetScreen() == screen)
                surface->hide();
        }
        if (!remaining.isEmpty() && !remaining.front()->geometry().intersects(state->imageRect().toAlignedRect())) {
            bool visibleOnRemaining = false;
            for (QScreen* output : remaining)
                visibleOnRemaining = visibleOnRemaining || output->geometry().intersects(state->imageRect().toAlignedRect());
            if (!visibleOnRemaining)
                state->origin = remaining.front()->geometry().topLeft() + QPoint(24, 24);
        }
        state->refresh();
    });
    state->refresh();
    for (const auto& surface : state->surfaces) {
        if (surface)
            surface->show();
    }
    if (consumePrivateRuntimeSource)
        consumeImageSource(path, opened);
    return root;
}
