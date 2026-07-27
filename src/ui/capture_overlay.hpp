#pragma once

#include "shared/config.hpp"
#include "ui/overlay_paint.hpp"

#include <QImage>
#include <QMainWindow>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QString>
#include <functional>
#include <optional>
#include <vector>

class QButtonGroup;
class QEnterEvent;
class QGraphicsOpacityEffect;
class QLabel;
class QPainter;
class QPushButton;
class QResizeEvent;
class QShowEvent;
class QPropertyAnimation;
class QScreen;
class InlineSelect;

namespace hyprcapture::ui {
struct ClipboardSnapshotData;
}

class CaptureOverlay final : public QMainWindow {
    Q_OBJECT
    Q_PROPERTY(double overlayOpacity READ overlayOpacity WRITE setOverlayOpacity)

  public:
    explicit CaptureOverlay(hyprcapture::CaptureDefaults defaults, bool quick, bool record, bool recordActive, QString sessionJson, QWidget* parent = nullptr);
    CaptureOverlay(const CaptureOverlay& source, const QRect& overlayGeometry, bool active, QWidget* parent = nullptr);
    ~CaptureOverlay() override;

    QScreen* overlayScreen() const;
    hyprcapture::OverlayScope overlayScope() const;
    bool isOverlayActive() const;
    void setOverlayActive(bool active);
    void adoptInteractionState(const CaptureOverlay& source);

  signals:
    void activationRequested();
    void finishingStarted();

  protected:
    void enterEvent(QEnterEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    enum class ConfirmDragMode { None, NewSelection, MoveSelection, ResizeLeft, ResizeTop, ResizeRight, ResizeBottom, ResizeTopLeft, ResizeTopRight, ResizeBottomRight, ResizeBottomLeft };

    struct MonitorArtifact {
        QRect   logicalGeometry;
        QImage  image;
        QImage  cursorImage;
        QString name;
        int     transform = 0;
        bool    focused = false;
        std::optional<int> workspaceWindowCount;
        QString singleWorkspaceWindowClass;
        QString singleWorkspaceWindowTitle;
        hyprcapture::ui::ScreenCornerRadii previewCornerRadii;
    };

    struct WindowArtifact {
        QRect   fullGeometry;
        QRect   visibleGeometry;
        QRect   selectionGeometry;
        QRect   selectionClipGeometry;
        double  rounding = 0.0;
        double  roundingPower = 2.0;
        double  borderSize = 0.0;
        QImage  image;
        QImage  realBackground;
        QString address;
        QString title;
        QString appClass;
        bool    focused = false;
        bool    fullscreen = false;
    };

    void buildToolbar();
    void initializeOverlay(const QRect& overlayGeometry);
    bool requestActivation();
    void parseSessionJson(const QString& json);
    void captureScreensBeforeOverlay();
    QRect preferredOverlayLogicalGeometry() const;
    QScreen* screenForOverlayGeometry(const QRect& logicalGeometry) const;
    void setMode(hyprcapture::CaptureMode mode);
    void updateToolbarControlsForMode();
    void beginPendingConfirm(hyprcapture::CaptureMode mode);
    void clearPendingConfirm();
    void confirmPendingCapture();
    bool confirmBeforeCaptureEnabled() const;
    bool pendingConfirmActive() const;
    void updateConfirmButtonVisibility();
    void setSelectionRect(const QRect& rect);
    bool regionSelectionValid(const QRect& selection) const;
    ConfirmDragMode confirmRegionDragModeAt(const QPoint& point) const;
    Qt::CursorShape cursorForConfirmDragMode(ConfirmDragMode mode) const;
    void updateConfirmCursor(const QPoint& point);
    QRect regionSelectionForDrag(const QPoint& point) const;
    void finishCapture();
    void cancelCapture();
    QString prepareRecordingRequest();
    void launchRecordingCountdown(const QString& requestPath);
    void startPreparedRecording(const QString& requestPath);
    bool startRecording(const QString& requestPath);
    bool stopRecording();
    void renderAndSaveCapture();
    void saveImage(const QImage& image,
                   hyprcapture::ui::ClipboardSnapshotData clipboardSnapshot,
                   const QString& outputPath,
                   const QString& restoreClipboardPath,
                   bool thumbnailStarted);
    QImage renderResultImage();
    QImage renderDesktopRectAtDisplayResolution(const QRect& globalRect) const;
    void paintDesktop(QPainter& painter, const QRect& target) const;
    void paintCursorLayers(QPainter& painter, const QRect& outputRect, const QRect& globalRect) const;
    QRect normalizedSelection() const;
    QRect captureRectForMode() const;
    QRect fullscreenCaptureRect() const;
    hyprcapture::ui::ScreenCornerRadii fullscreenPreviewCornerRadii(const QRect& localRect) const;
    QRect regionCaptureBounds() const;
    QRect localScreenRectAt(const QPoint& localPos) const;
    QPoint clampedToRect(const QPoint& point, const QRect& bounds) const;
    QRect globalToLocalRect(const QRect& rect) const;
    QRect localToDesktopLogicalRect(const QRect& rect) const;
    QPoint localToDesktopLogicalPoint(const QPoint& point) const;
    QRect desktopSourceRectForGlobalRect(const QRect& rect) const;
    QRect localToDesktopSourceRect(const QRect& rect) const;
    QPoint cursorLogicalPosition() const;
    void rememberCursorPosition(const QPointF& globalPosition);
    void rememberCursorLocalPosition(const QPointF& localPosition);
    void refreshInitialCursorPosition();
    int monitorCount() const;
    bool hasMultipleMonitors() const;
    void hideOptionPopups();
    hyprcapture::FullscreenScope currentFullscreenScope() const;
    hyprcapture::WindowBackground currentWindowBackground() const;
    hyprcapture::WindowBackground currentRecordBackground() const;
    bool currentRecordTransparencyRequired() const;
    bool currentRecordAlphaRequested() const;
    QString currentRecordFormat() const;
    QString currentRecordCodec() const;
    int currentRecordFps() const;
    int currentRecordMaxSeconds() const;
    int currentRecordCountdownSeconds() const;
    hyprcapture::RecordWindowBackend currentRecordBackend() const;
    void applyRecordDefaultsForCurrentBackground();
    QString recordOptionsConflict() const;
    QString recordOptionsWarning() const;
    void updateRecordOptionsVisibility();
    void updateRecordWarning();
    hyprcapture::DecorationPolicy currentWindowBorder() const;
    hyprcapture::DecorationPolicy currentWindowShadow() const;
    QRect windowFrameGeometry(const WindowArtifact& window) const;
    QRect windowSelectionGeometry(const WindowArtifact& window) const;
    bool hasOverviewSelectionGeometry(const WindowArtifact& window) const;
    bool selectedWindowUsesOverviewSelection() const;
    void beginHymissionCaptureInputSuppression();
    void endHymissionCaptureInputSuppression();
    double windowFrameRadius(const WindowArtifact& window) const;
    bool hydrateWindowArtifact(WindowArtifact& window);
    int hoveredWindowIndex() const;
    WindowArtifact* hoveredWindow();
    const WindowArtifact* hoveredWindow() const;
    WindowArtifact* selectedWindow();
    const WindowArtifact* selectedWindow() const;
    const WindowArtifact* filenameWindow() const;
    hyprcapture::FilenameMetadata resolvedFilenameMetadata() const;
    bool windowCaptureAvailable() const;
    void updateStatus();
    void relayoutToolbar();
    void showThumbnail(const QString& previewPath, const QString& targetPath, const QString& restoreClipboardPath);
    double overlayOpacity() const;
    void setOverlayOpacity(double opacity);
    void startFadeIn();
    void fadeOutThen(std::function<void()> finished);
    void runOverlayFade(double start, double end, std::function<void()> finished);

    hyprcapture::CaptureDefaults m_defaults;
    hyprcapture::CaptureMode     m_mode;
    bool                      m_quick = false;
    bool                      m_record = false;
    bool                      m_recordActive = false;
    QString                   m_recordError;
    bool                      m_sessionDecoded = false;
    bool                      m_hymissionOverviewSession = false;
    bool                      m_hymissionCaptureInputSuppressed = false;
    bool                      m_confirmBeforeCapture = false;
    bool                      m_pendingConfirm = false;
    ConfirmDragMode           m_confirmDragMode = ConfirmDragMode::None;
    bool                      m_dragging = false;
    bool                      m_finishing = false;
    bool                      m_fadeOutStarted = false;
    bool                      m_overlayActive = true;
    double                    m_overlayOpacity = 0.0;
    QPoint                    m_dragStart;
    QPoint                    m_dragEnd;
    QPoint                    m_confirmDragStart;
    QRect                     m_confirmDragStartSelection;
    QPoint                    m_cursorLogicalPosition;
    bool                      m_hasCursorLogicalPosition = false;
    bool                      m_recordFormatAuto = true;
    bool                      m_recordCodecAuto = true;
    bool                      m_fullscreenClientSelected = false;
    QString                   m_hymissionCaptureInputToken;

    QWidget*     m_toolbar = nullptr;
    QGraphicsOpacityEffect* m_toolbarOpacity = nullptr;
    QPropertyAnimation* m_fadeAnimation = nullptr;
    InlineSelect* m_fullscreenScope = nullptr;
    InlineSelect* m_windowBackground = nullptr;
    QWidget*      m_recordOptions = nullptr;
    InlineSelect* m_recordCodec = nullptr;
    InlineSelect* m_recordFormat = nullptr;
    InlineSelect* m_recordFps = nullptr;
    InlineSelect* m_recordDuration = nullptr;
    InlineSelect* m_recordBackend = nullptr;
    QLabel*       m_recordWarning = nullptr;
    QPushButton*  m_recordToggle = nullptr;
    QPushButton*  m_confirmButton = nullptr;
    QLabel*      m_status = nullptr;
    QImage       m_desktopImage;
    QRect        m_desktopGeometry;
    QRect        m_overlayLogicalGeometry;
    int          m_sessionMonitorCount = 0;
    int          m_sessionWindowCount = 0;
    int          m_selectedWindowIndex = -1;
    std::vector<MonitorArtifact> m_monitorArtifacts;
    std::vector<WindowArtifact>  m_windowArtifacts;
};
