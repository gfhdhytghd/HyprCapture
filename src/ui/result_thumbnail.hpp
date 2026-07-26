#pragma once

#include <QPoint>
#include <QPointF>
#include <QPointer>
#include <QTimer>
#include <QWidget>

class QLabel;
class QPixmap;
class QScreen;
class QWidget;
class SwipeBackdrop;
class TranscodeProgressOverlay;

class ResultThumbnail final : public QWidget {
    Q_OBJECT

  public:
    ResultThumbnail(const QPixmap& pixmap,
                    QString path,
                    QString restoreClipboardPath,
                    QString deleteRoot,
                    int timeoutMs,
                    bool copyFile = false,
                    QScreen* targetScreen = nullptr,
                    QWidget* parent = nullptr);
    void setImagePixmap(const QPixmap& pixmap);
    void setTranscodeProgress(double progress);
    void finishTranscodeProgress(bool success, int timeoutMs);
    void applySynchronizedSwipeOffset(const QPointF& offset);
    void resetSynchronizedSwipe();
    void animateSynchronizedSwipeOut(bool deleteRequested);

  signals:
    void swipeOffsetChanged(const QPointF& offset);
    void swipeResetStarted();
    void swipeActionStarted(bool deleteRequested);

  protected:
    void closeEvent(QCloseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

  private:
    enum class SwipeAction { Close, Delete };

    bool openPath(const QString& path);
    bool openWithPortal(const QString& path);
    bool openWithApp(const QString& appId, const QString& path);
    void toggleMenu();
    void setMenuVisible(bool visible);
    void applyLayerSize();
    void startFileDrag();
    void updateSwipeVisual();
    void resetSwipe();
    void animateSwipeOut(SwipeAction action, bool finalizeAction);
    void deleteAndClose();
    void restoreClipboard() const;
    void startCloseTimer(int timeoutMs);

    QString m_path;
    QString m_restoreClipboardPath;
    QString m_deleteRoot;
    bool    m_copyFile = false;
    QPointer<QScreen> m_targetScreen;
    QWidget* m_card = nullptr;
    SwipeBackdrop* m_swipeBackdrop = nullptr;
    TranscodeProgressOverlay* m_transcodeOverlay = nullptr;
    QLabel* m_imageLabel = nullptr;
    QWidget* m_menuShell = nullptr;
    QWidget* m_menuPanel = nullptr;
    QWidget* m_openWithPanel = nullptr;
    QPoint  m_pressGlobal;
    QPoint  m_imageOrigin;
    QPointF m_swipeOffset;
    bool    m_dragMoved = false;
    bool    m_draggingFile = false;
    bool    m_swipeCompleting = false;
    bool    m_transcodeProgressActive = false;
    QTimer  m_closeTimer;
    QTimer  m_swipeResetTimer;
};
