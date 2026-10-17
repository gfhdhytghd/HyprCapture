#pragma once

#include <atomic>

#include "shared/config.hpp"
#include "ui/overlay_paint.hpp"
#include "ui/scroll_stitcher.hpp"
#include "ui/session_lock_guard.hpp"

#include <QImage>
#include <QMainWindow>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QString>
#include <functional>
#include <chrono>
#include <memory>
#include <optional>
#include <vector>

class QButtonGroup;
class QCheckBox;
class QEnterEvent;
class QGraphicsOpacityEffect;
class QLabel;
class QPainter;
class QPushButton;
class QResizeEvent;
class QShowEvent;
class QWheelEvent;
class QPropertyAnimation;
class QScreen;
class QTimer;
class InlineSelect;
class AudioMeter;
class QSlider;
class QProcess;
class QThreadPool;
class AnnotationEditor;

namespace hyprcapture::ui {
struct ClipboardSnapshotData;
class ScrollCaptureController;
class HdrBackdrop;
class HdrUpload;
}

class CaptureOverlay final : public QMainWindow, public hyprcapture::ui::SessionLockParticipant {
    Q_OBJECT
    Q_PROPERTY(double overlayOpacity READ overlayOpacity WRITE setOverlayOpacity)

  public:
    explicit CaptureOverlay(hyprcapture::CaptureDefaults defaults, bool quick, bool record, bool recordActive, QString sessionJson, QWidget* parent = nullptr);
    CaptureOverlay(const CaptureOverlay& source, const QRect& overlayGeometry, bool active, QWidget* parent = nullptr);
    ~CaptureOverlay() override;
    void abortForSessionLock() override;

    QScreen* overlayScreen() const;
    hyprcapture::OverlayScope overlayScope() const;
    bool isOverlayActive() const;
    bool isEditing() const { return m_editing; }
    void setOverlayActive(bool active);
    void adoptInteractionState(const CaptureOverlay& source);

  signals:
    void activationRequested();
    void finishingStarted();
    void editingChanged(bool editing);
    void scrollingChanged(bool scrolling);

  protected:
    bool event(QEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    enum class ConfirmDragMode { None, NewSelection, MoveSelection, ResizeLeft, ResizeTop, ResizeRight, ResizeBottom, ResizeTopLeft, ResizeTopRight, ResizeBottomRight, ResizeBottomLeft };

    struct MonitorArtifact {
        QRect   logicalGeometry;
        QImage  image;
        QImage  hdrImage;
        std::shared_ptr<hyprcapture::ui::HdrUpload> hdrUpload;
        float   referenceNits = 203;
        QImage  cursorImage;
        QImage  hdrCursorImage;
        QString name;
        int     transform = 0;
        bool    focused = false;
        bool    hdrActive = false;
        std::optional<int> workspaceWindowCount;
        QString singleWorkspaceWindowClass;
        QString singleWorkspaceWindowTitle;
        hyprcapture::ui::ScreenCornerRadii previewCornerRadii;
    };

    struct WindowArtifact {
        QRect   fullGeometry;
        QRect   visibleGeometry;
        QRect   selectionGeometry;
        std::optional<double> selectionRounding;
        QRect   selectionClipGeometry;
        bool    stagePreview = false;
        double  rounding = 0.0;
        double  roundingPower = 2.0;
        double  borderSize = 0.0;
        QImage  image;
        QImage  hdrImage;
        QImage  realBackground;
        QImage  hdrRealBackground;
        bool    realBackgroundAttempted = false;
        QString address;
        QString title;
        QString appClass;
        int     zIndex = 0;
        bool    focused = false;
        bool    fullscreen = false;
    };

    void prepareHdrBackdrops();
    void queuePqBackdrop(const QRect& geometry);
    bool hdrStartupBlocked();
    void updateHdrEditBackdrop();
    bool canKeepHdrDesktopInEditor() const;
    bool hasHdrBackdrop(const MonitorArtifact& artifact) const;
    struct HdrMonitorBackdrop {
        QRect geometry;
        std::unique_ptr<hyprcapture::ui::HdrBackdrop> surface;
        bool fp16Submitted = false, pqQueued = false;
    };
    std::vector<HdrMonitorBackdrop> m_hdrBackdrops;
    std::shared_ptr<void> m_hdrNativeWarmup;
    QTimer* m_hdrStartupTimer = nullptr;
    bool m_hdrStartupWaiting = false;
    std::chrono::steady_clock::time_point m_hdrStartupDeadline{};
    friend class InPlaceEditorTest;
    std::unique_ptr<hyprcapture::ui::HdrBackdrop> m_hdrEditBackdrop;
    qint64 m_hdrEditCacheKey = 0;
    qint64 m_hdrEditPqCacheKey = 0;
    QThreadPool* m_hdrEditPreviewPool = nullptr;
    bool m_hdrEditPreviewBusy = false;
    bool m_hdrEditPreviewDirty = false;
    void buildToolbar();
    void ensureRecordControls();
    void initializeOverlay(const QRect& overlayGeometry);
    bool requestActivation();
    void parseSessionJson(const QString& json);
    void captureScreensBeforeOverlay();
    void ensureDesktopImage();
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
    void beginInPlaceEdit(const QImage& capturedImage = {});
    void beginScrollCapture();
    void refreshInPlaceImage();
    QImage scrollWindowBackground(const QImage& frame, const QRect& geometry);
    QImage renderScrollResultImage(bool hdr = false);
    void leaveInPlaceEdit();
    void exportInPlaceImage();
    void pinInPlaceImage();
    void cancelCapture();
    QString prepareRecordingRequest();
    void launchRecordingCountdown(const QString& requestPath);
    void startPreparedRecording(const QString& requestPath);
    bool startRecording(const QString& requestPath);
    bool stopRecording();
    void renderAndSaveCapture();
    void saveImage(const QImage& image,
                   const QImage& hdrImage,
                   hyprcapture::ui::ClipboardSnapshotData clipboardSnapshot,
                   const QString& outputPath,
                   const QString& restoreClipboardPath,
                   bool thumbnailStarted,
                   hyprcapture::CaptureMode mode,
                   hyprcapture::FilenameMetadata filenameMetadata);
    QImage renderResultImage(bool hdr = false);
    QImage renderDesktopRectAtDisplayResolution(const QRect& globalRect, bool hdr = false) const;
    void paintDesktop(QPainter& painter, const QRect& target, bool preserveClip = false) const;
    static void paintCursorSnapshot(QPainter& painter, const QRect& outputRect, const QRect& globalRect, bool hdr,
                                    const std::vector<MonitorArtifact>& monitors, bool includeCursor);
    static QImage renderDesktopSnapshot(const QRect& globalRect, bool hdr, const std::vector<MonitorArtifact>& monitors,
                                        bool hasHdrScreen, bool includeCursor);
    std::function<QImage()> windowResultRenderer(const WindowArtifact& window, bool hdr) const;
    std::function<QImage()> hdrResultRenderer() const;
    void queueHdrEditBase();
    void startHdrEditBase();
    void invalidateHdrEditBase();
    quint64 m_hdrBaseGeneration = 0;
    bool m_hdrBasePending = false;
    bool m_hdrBaseRunning = false;
    bool m_hdrBaseFailed = false;
    std::function<QImage()> m_hdrBaseLatest;
    bool m_firstEditorPaintPending = false;
    bool m_hdrExportPending = false;
    std::shared_ptr<std::atomic_bool> m_hdrBaseCancelled;
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
    double windowFrameRadius(const WindowArtifact& window, bool selection = true) const;
    bool hydrateWindowArtifact(WindowArtifact& window);
    bool windowWheelSelectionEnabled() const;
    std::vector<int> currentMonitorWindowCandidates() const;
    int windowCycleTargetIndex() const;
    void resetWindowCycle(bool clearSelectedWindow = false);
    QString windowCycleStatusText() const;
    int rawHoveredWindowIndex() const;
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
    void showThumbnail(const QString& previewPath, const QString& targetPath, const QString& restoreClipboardPath, const QImage& hdrPreview = {});
    double overlayOpacity() const;
    void setOverlayOpacity(double opacity);
    void fadeOutThen(std::function<void()> finished);
    void runOverlayFade(double start, double end, std::function<void()> finished);

    hyprcapture::CaptureDefaults m_defaults;
    hyprcapture::CaptureMode     m_mode;
    bool                      m_quick = false;
    bool                      m_record = false;
    bool                      m_recordActive = false;
    bool                      m_scrolling = false;
    bool                      m_scrollResult = false;
    QImage                    m_scrollOriginal, m_scrollFirstFrame, m_scrollHdrOriginal, m_scrollSdrImage;
    float                     m_scrollReferenceNits = 203;
    QRect                     m_scrollGeometry;
    hyprcapture::ui::ScrollLayout m_scrollLayout;
    QString                   m_recordError;
    bool                      m_sessionDecoded = false;
    bool                      m_regionCaptureAvailable = false;
    int                       m_scrollSessionVersion = 0;
    hyprcapture::ui::ScrollCaptureController* m_scrollController = nullptr;
    bool                      m_hymissionOverviewSession = false;
    bool                      m_hymissionStageSession = false;
    bool                      m_hymissionCaptureInputSuppressed = false;
    bool                      m_confirmBeforeCapture = false;
    bool                      m_pendingConfirm = false;
    ConfirmDragMode           m_confirmDragMode = ConfirmDragMode::None;
    bool                      m_dragging = false;
    bool                      m_finishing = false;
    bool                      m_editing = false;
    AnnotationEditor*         m_editor = nullptr;
    QImage                    m_editedOutput;
    QImage                    m_editHdrBase;
    QImage                    m_editedHdrOutput;
    QRect                     m_editImageRect;
    bool                      m_fadeOutStarted = false;
    bool                      m_overlayActive = true;
    bool                      m_firstPaintPending = true;
    bool                      m_firstVisiblePaintPending = true;
    bool                      m_firstUpdatePending = true;
    // A mapped overlay must already contain the frozen desktop, not a transparent warm-up frame.
    double                    m_overlayOpacity = 1.0;
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
    void saveRememberedSettings();
    void refreshSoundDevices();
    void updateSoundMeter();
    void refreshAecStatus(bool retest = false);
    QWidget* m_aecOptions = nullptr;
    InlineSelect* m_echoCancellation = nullptr;
    InlineSelect* m_echoBackend = nullptr;
    QLabel* m_aecStatus = nullptr;
    QPushButton* m_aecTest = nullptr;
    QString m_aecTestResult;
    bool m_aecChecking = false;
    QWidget* m_soundMixer = nullptr;
    InlineSelect* m_soundPreset = nullptr;
    QSlider* m_systemGain = nullptr;
    QSlider* m_micGain = nullptr;
    AudioMeter* m_systemMeter = nullptr;
    AudioMeter* m_micMeter = nullptr;
    QProcess* m_meterProcess = nullptr;
    QString m_meterKey;

    QWidget*      m_soundOptions = nullptr;
    InlineSelect* m_soundMode = nullptr;
    InlineSelect* m_soundOutput = nullptr;
    InlineSelect* m_soundInput = nullptr;
    bool m_soundDevicesLoading = false;
    QWidget*      m_recordOptions = nullptr;
    InlineSelect* m_recordCodec = nullptr;
    InlineSelect* m_recordFormat = nullptr;
    InlineSelect* m_recordFps = nullptr;
    InlineSelect* m_recordDuration = nullptr;
    InlineSelect* m_recordBackend = nullptr;
    QLabel*       m_recordWarning = nullptr;
    QWidget* m_dynamicRangeControl = nullptr;
    QPushButton* m_sdrButton = nullptr;
    QPushButton* m_hdrButton = nullptr;
    bool m_hasHdrScreen = false;
    QPushButton*  m_recordToggle = nullptr;
    QPushButton*  m_confirmButton = nullptr;
    QLabel*      m_status = nullptr;
    QImage       m_desktopImage;
    QRect        m_desktopGeometry;
    QRect        m_overlayLogicalGeometry;
    int          m_sessionMonitorCount = 0;
    int          m_sessionWindowCount = 0;
    int          m_selectedWindowIndex = -1;
    bool         m_windowCycleActive = false;
    int          m_windowCyclePosition = -1;
    std::vector<int> m_windowCycleCandidates;
    hyprcapture::ui::WindowWheelStepState m_windowWheelStepState;
    std::vector<MonitorArtifact> m_monitorArtifacts;
    std::vector<WindowArtifact>  m_windowArtifacts;
};
