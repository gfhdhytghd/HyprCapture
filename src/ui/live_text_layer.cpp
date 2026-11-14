#include "ui/live_text_layer.hpp"
#include <QDebug>
#include "shared/ocr_engine.hpp"
#include "ui/clipboard_utils.hpp"
#include "ui/hdr_image.hpp"
#include "ui/i18n.hpp"
#include "ui/screenshot_notification.hpp"
#include <QApplication>
#include <QContextMenuEvent>
#include <QFile>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScopeGuard>
#include <QTextBoundaryFinder>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <tuple>

namespace hyprcapture::ui {
namespace {
// All OCR animation durations (milliseconds) live here.
constexpr struct {
    int dim = 180, scan = 1100, stagger = 14, staggerCap = 350, reveal = 160, glow = 450;
    int off = 140, copy = 220, flashIn = 120, flashHold = 220, flashOut = 280;
    int toastIn = 160, toastHold = 1200, toastOut = 220, shake = 240;
} timing;
auto configKey(const OcrConfig& c) { return std::tuple(c.mode, c.engine, c.languages, c.command, c.output, c.timeout, c.apiUrl, c.apiKey); }
QString ocrErrorText(const std::string& error) {
    const auto text = QString::fromStdString(error);
    for (const auto& [prefix, pattern] : {
             std::pair{"OCR API returned HTTP ", "OCR API returned HTTP %1"},
             {"OCR API unreachable: ", "OCR API unreachable: %1"},
             {"OCR API: ", "OCR API: %1"}}) {
        const auto start = QString::fromLatin1(prefix);
        if (text.startsWith(start)) return uiText(pattern).arg(text.mid(start.size()));
    }
    return uiText(text);
}
QPointF polygonCentroid(const QPainterPath& path) {
    const auto polygon = path.toFillPolygon();
    qreal twiceArea = 0; QPointF weighted;
    for (int i = 0; i < polygon.size(); ++i) {
        const auto a = polygon[i], b = polygon[(i+1)%polygon.size()];
        const qreal cross = a.x()*b.y()-b.x()*a.y();
        twiceArea += cross; weighted += (a+b)*cross;
    }
    return qFuzzyIsNull(twiceArea) ? path.boundingRect().center() : weighted/(3*twiceArea);
}
qreal outCubic(qreal t) { return QEasingCurve(QEasingCurve::OutCubic).valueForProgress(std::clamp(t, 0.0, 1.0)); }
}

LiveTextLayer::LiveTextLayer(QWidget* canvas) : QWidget(canvas) {
    setObjectName("liveTextLayer");
    setAttribute(Qt::WA_TranslucentBackground);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setCursor(Qt::ArrowCursor);
    canvas->installEventFilter(this);
    setGeometry(canvas->rect());
    for (const auto& [animation,name] : {std::pair{&m_dimAnimation,"ocrDim"}, {&m_scanAnimation,"ocrScan"},
         {&m_phaseAnimation,"ocrPhase"}, {&m_copyAnimation,"ocrCopy"}, {&m_toastAnimation,"ocrToast"}, {&m_shakeAnimation,"ocrShake"}}) {
        animation->setParent(this); animation->setObjectName(name);
    }
    hide();
}
LiveTextLayer::~LiveTextLayer() { cancelJob(); }
bool LiveTextLayer::reducedMotion() const {
    return qEnvironmentVariableIntValue("QT_ANIMATIONS_DISABLED") == 1 ||
           qEnvironmentVariableIntValue("HYPRCAPTURE_REDUCED_MOTION") == 1;
}
bool LiveTextLayer::fallback() const {
    return (m_config.mode == "text_layer" || m_config.mode == "both") && m_config.engine == "command" && m_config.output == "text";
}
bool LiveTextLayer::copyOnly() const { return m_config.mode == "capture" || fallback(); }
QString LiveTextLayer::tooltip() const {
    return fallback() ? uiText("Copy all text (engine has no text positions)") : uiText("Live text (O)");
}
void LiveTextLayer::configure(const OcrConfig& config, NotificationBackend backend, bool allowed) {
    if (configKey(config) != configKey(m_config) || allowed != m_allowed) {
        stop(); m_cached = false; m_results = {}; m_layout = {};
    }
    m_config = config; m_backend = backend; m_allowed = allowed;
    emit stateChanged();
}
void LiveTextLayer::setSource(const QImage& image) {
    if (image.cacheKey() == m_base.cacheKey()) return;
    stop(); m_base = image; m_base.setDevicePixelRatio(1);
    m_cached = false; m_results = {}; m_layout = {};
}
void LiveTextLayer::setView(const QTransform& transform) {
    const QRect before = imageRect(); m_transform = transform;
    update(before.united(imageRect()));
}
QRect LiveTextLayer::imageRect() const { return m_transform.mapRect(QRectF(m_base.rect())).toAlignedRect().intersected(rect()).adjusted(-2,-2,2,2); }
void LiveTextLayer::repaintImage() { update(imageRect()); }
void LiveTextLayer::animate(QVariantAnimation& animation, int duration, QEasingCurve::Type easing,
                            std::function<void(qreal)> frame, std::function<void()> finished) {
    animation.stop(); animation.disconnect(this);
    if (reducedMotion() || duration == 0) { frame(1); if (finished) finished(); return; }
    animation.setStartValue(0.0); animation.setEndValue(1.0); animation.setDuration(duration); animation.setEasingCurve(easing);
    connect(&animation, &QVariantAnimation::valueChanged, this, [frame](const QVariant& value) { frame(value.toReal()); });
    if (finished) connect(&animation, &QVariantAnimation::finished, this, std::move(finished));
    animation.start();
}
void LiveTextLayer::toggle() {
    if (!available() || m_base.isNull()) return;
    if (m_on || m_busy) { turnOff(); return; }
    m_on = true; m_opacity = 1; m_anchor = m_caret = -1;
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    show(); raise(); setFocus(Qt::ShortcutFocusReason);
    emit stateChanged();
    const qreal targetDim = copyOnly() ? .15 : .25;
    const qreal startDim = m_dim;
    animate(m_dimAnimation, timing.dim, QEasingCurve::OutCubic, [this, startDim, targetDim](qreal t) { m_dim = startDim + (targetDim-startDim)*t; repaintImage(); });
    if (m_cached && !copyOnly()) { m_phase = Phase::Idle; m_reveal = 1; repaintImage(); return; }
    m_phase = Phase::Scan; m_reveal = 0; m_busy = true; m_pendingReveal = false;
    finishScan(); startJob();
    emit stateChanged();
}
void LiveTextLayer::cancelJob() {
    ++m_generation;
    if (m_worker.joinable()) { m_worker.request_stop(); m_worker.join(); }
    m_busy = false;
}
void LiveTextLayer::turnOff(bool instant) {
    cancelJob(); m_pendingReveal = false; m_selecting = false; m_clickClock.invalidate();
    m_scanAnimation.stop(); m_phaseAnimation.stop(); m_copyAnimation.stop(); m_dimAnimation.stop();
    emit iconFrame(0,0,false);
    if (!m_on && m_phase == Phase::Idle) return;
    m_phase = Phase::Fade;
    const qreal from = m_opacity;
    animate(m_phaseAnimation, instant ? 0 : timing.off, QEasingCurve::OutCubic,
        [this, from](qreal t) { m_opacity = from * (1-t); repaintImage(); }, [this] {
            m_on = false; m_phase = Phase::Idle; m_dim = 0; m_opacity = 1;
            m_anchor = m_caret = -1;
            setAttribute(Qt::WA_TransparentForMouseEvents);
            setCursor(Qt::ArrowCursor); emit stateChanged();
            if (m_toast.isEmpty()) hide(); else repaintImage();
        });
}
void LiveTextLayer::stop() {
    turnOff(true);
    m_toastAnimation.stop(); m_shakeAnimation.stop(); m_toast.clear();
    hide(); emit iconFrame(0,0,false);
}
void LiveTextLayer::startJob() {
    cancelJob(); m_busy = true;
    const auto generation = m_generation;
    const auto base = m_base; const auto config = m_config;
    m_worker = std::jthread([this, base, config, generation](std::stop_token stop) {
        OcrResponse response;
        const QString path = runtimeFile("ocr", ".png");
        const auto cleanup = qScopeGuard([&] { if (!path.isEmpty()) QFile::remove(path); });
        if (stop.stop_requested()) return;
        const QImage sdr = isHdrImage(base) ? hdrToSdr(base) : base;
        if (path.isEmpty() || !savePrivatePng(sdr, path)) response.error = "Could not write OCR image";
        else response = runOcrEngine(QFile::encodeName(path).toStdString(), config, stop);
        // Delete before publishing, including on cancellation and parser failure.
        if (!path.isEmpty()) QFile::remove(path);
        if (stop.stop_requested()) return;
        QMetaObject::invokeMethod(this, [this, generation, response = std::move(response)]() mutable { receive(generation, std::move(response)); }, Qt::QueuedConnection);
    });
}
void LiveTextLayer::receive(quint64 generation, OcrResponse response) {
    if (generation != m_generation || !m_on) return;
    m_busy = false;
    if (!response) { fail(ocrErrorText(response.error)); return; }
    if (!response.warning.empty()) qWarning("%s", response.warning.c_str());
    m_results = std::move(response); m_layout.setResults(m_results.results);
    if (m_layout.text.trimmed().isEmpty()) {
        turnOff(); shake(); toast(uiText("No text found")); emit recognized(); return;
    }
    if (!copyOnly() && !ocrSupportsTextLayer(m_results.results)) {
        fail(uiText("Text layer needs an engine with text positions")); return;
    }
    m_cached = true; m_pendingReveal = true;
    if (reducedMotion() || m_scanAnimation.state() != QAbstractAnimation::Running) reveal();
    emit stateChanged(); emit recognized();
}
void LiveTextLayer::finishScan() {
    if (!m_on || m_phase != Phase::Scan) return;
    if (m_pendingReveal) { reveal(); return; }
    if (reducedMotion()) { repaintImage(); return; }
    animate(m_scanAnimation, timing.scan, QEasingCurve::InOutSine, [this](qreal t) {
        m_scan = t; repaintImage(); emit iconFrame(t*360,0,true);
    }, [this] { finishScan(); });
}
void LiveTextLayer::reveal() {
    m_pendingReveal = false; emit iconFrame(0,0,false);
    if (copyOnly()) {
        m_phase = Phase::CopyFlash;
        const QString text = QString::fromStdString(joinOcrText(m_results.results));
        if (!copyTextToClipboardDetached(text)) { fail(uiText("Could not copy text to the clipboard. Check that wl-copy is installed.")); return; }
        const int count = text.toUcs4().size();
        QString message = fallback() && !m_fallbackWarned ? uiText("Engine has no text positions — copied all text") : uiText("Copied %1 characters").arg(count);
        if (fallback()) m_fallbackWarned = true;
        const int total = timing.flashIn + timing.flashHold + timing.flashOut;
        animate(m_phaseAnimation, total, QEasingCurve::Linear, [this, total](qreal t) {
            const qreal elapsed = t * total;
            m_reveal = elapsed < timing.flashIn ? outCubic(elapsed/timing.flashIn) :
                       elapsed < timing.flashIn+timing.flashHold ? 1 : 1-outCubic((elapsed-timing.flashIn-timing.flashHold)/timing.flashOut);
            m_opacity = m_reveal; repaintImage();
        }, [this, message] { turnOff(true); toast(message); });
        return;
    }
    m_phase = Phase::Reveal;
    const int lastRow = m_layout.runs.empty() ? 0 : m_layout.runs.back().row;
    const int stagger = std::min(timing.staggerCap, lastRow*timing.stagger);
    animate(m_phaseAnimation, stagger + timing.glow, QEasingCurve::Linear,
        [this](qreal t) { m_reveal = t; repaintImage(); }, [this] { m_phase = Phase::Idle; m_reveal = 1; repaintImage(); });
}
void LiveTextLayer::fail(const QString& error) {
    turnOff(); shake(); emit feedback(error, true);
    showNotification(m_backend, uiText("OCR failed"), error);
}
void LiveTextLayer::shake() {
    animate(m_shakeAnimation, timing.shake, QEasingCurve::Linear, [this](qreal t) {
        emit iconFrame(0, reducedMotion() ? 0 : 3*std::sin(t*4*std::numbers::pi), false);
    }, [this] { emit iconFrame(0,0,false); });
}
QRect LiveTextLayer::toastRect() const {
    const int width = std::min(this->width()-16, fontMetrics().horizontalAdvance(m_toast)+32);
    return QRect((this->width()-width)/2, height()-60, width, 40);
}
void LiveTextLayer::toast(const QString& text) {
    const QRect old = toastRect(); m_toast = text; update(old.united(toastRect()));
    setAttribute(Qt::WA_TransparentForMouseEvents, !m_on); show(); raise();
    emit feedback(text, false);
    const int total = timing.toastIn + timing.toastHold + timing.toastOut;
    animate(m_toastAnimation, total, QEasingCurve::Linear, [this, total](qreal t) {
        const qreal elapsed = t*total;
        m_toastOpacity = elapsed < timing.toastIn ? outCubic(elapsed/timing.toastIn) :
                         elapsed < timing.toastIn+timing.toastHold ? 1 : 1-outCubic((elapsed-timing.toastIn-timing.toastHold)/timing.toastOut);
        m_toastSlide = 8*(1-outCubic(elapsed/timing.toastIn)); update(toastRect());
    }, [this] { const QRect dirty = toastRect(); m_toast.clear(); update(dirty); if (!m_on) hide(); });
}
QString LiveTextLayer::selectedText() const {
    return m_anchor < 0 || m_caret < 0 ? QString{} : m_layout.text.mid(std::min(m_anchor,m_caret), std::abs(m_anchor-m_caret));
}
void LiveTextLayer::selectAll() { m_anchor = 0; m_caret = m_layout.text.size(); repaintImage(); }
void LiveTextLayer::copySelection(bool all) {
    const QString text = all ? m_layout.text : selectedText();
    if (text.isEmpty()) return;
    if (!copyTextToClipboardDetached(text)) { fail(uiText("Could not copy text to the clipboard. Check that wl-copy is installed.")); return; }
    animate(m_copyAnimation, timing.copy, QEasingCurve::OutCubic, [this](qreal t) { m_copyPulse = .4*(1-t); repaintImage(); });
    toast(uiText("Copied"));
}

void LiveTextLayer::paintEvent(QPaintEvent*) {
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
    if (m_on && !m_base.isNull()) {
        const QPainterPath image = [&] { QPainterPath path; path.addRect(m_transform.mapRect(QRectF(m_base.rect()))); return path; }();
        p.setOpacity(m_opacity);
        QPainterPath holes;
        if (m_phase != Phase::Scan) for (const auto& run : m_layout.runs) holes.addPath(m_transform.map(run.area));
        p.fillPath(image.subtracted(holes), QColor(0,0,0,qRound(m_dim*255)));
        if (m_phase == Phase::Scan && !reducedMotion()) {
            p.save(); p.setClipPath(image);
            const auto bounds = image.boundingRect();
            const double diagonal = std::hypot(bounds.width(),bounds.height());
            const QPointF direction(bounds.width()/diagonal, bounds.height()/diagonal);
            const double width = .3*diagonal;
            const QPointF center = bounds.topLeft()+direction*((diagonal+width)*m_scan-width/2);
            QLinearGradient gradient(center-direction*width/2, center+direction*width/2);
            gradient.setColorAt(0,QColor(255,255,255,0)); gradient.setColorAt(.25,QColor(255,255,255,15));
            gradient.setColorAt(.5,QColor(255,255,255,46)); gradient.setColorAt(.75,QColor(255,255,255,15)); gradient.setColorAt(1,QColor(255,255,255,0));
            p.fillPath(image,gradient); p.restore();
        }
        const int count = m_layout.runs.size();
        const int lastRow = m_layout.runs.empty() ? 0 : m_layout.runs.back().row;
        const int stagger = std::min(timing.staggerCap,lastRow*timing.stagger);
        const qreal elapsed = m_reveal*(stagger+timing.glow);
        if (m_phase != Phase::Scan) for (int i = 0; i < count; ++i) {
            const auto& run = m_layout.runs[i];
            const qreal delay = std::min(run.row*timing.stagger,timing.staggerCap);
            const qreal age = elapsed-delay;
            const bool revealing = m_phase == Phase::Reveal;
            const qreal alpha = revealing ? outCubic(age/timing.reveal) : 1;
            const qreal glow = !revealing ? .55 : age < 0 ? 0 : age < timing.glow/2 ? .9*outCubic(age/(timing.glow/2.0)) : .9-.35*outCubic((age-timing.glow/2.0)/(timing.glow/2.0));
            auto area = m_transform.map(run.area);
            // The un-revealed polygon remains dimmed; holes must not reveal
            // later lines before their staggered fill begins.
            if (revealing) p.fillPath(area,QColor(0,0,0,qRound(255*m_dim*(1-alpha))));
            const QPointF center = polygonCentroid(area);
            QTransform scale; scale.translate(center.x(),center.y()); scale.scale(.97+.03*alpha,.97+.03*alpha); scale.translate(-center.x(),-center.y());
            area = scale.map(area);
            p.fillPath(area,QColor(255,255,255,qRound(24*alpha)));
            p.setPen(QPen(QColor(255,255,255,qRound(255*glow)),1,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin)); p.setBrush(Qt::NoBrush); p.drawPath(area);
        }
        QColor selected = palette().color(QPalette::Highlight); selected.setAlphaF(std::min(.95,.45+m_copyPulse));
        p.fillPath(m_transform.map(m_layout.selection(m_anchor,m_caret)),selected);
    }
    if (!m_toast.isEmpty()) {
        p.setOpacity(m_toastOpacity); QRectF pill = toastRect().adjusted(0,4,0,-4); pill.translate(0,m_toastSlide);
        QColor background = palette().color(QPalette::Window); background.setAlphaF(.92);
        p.setPen(Qt::NoPen); p.setBrush(background); p.drawRoundedRect(pill,pill.height()/2,pill.height()/2);
        p.setPen(palette().color(QPalette::WindowText)); p.drawText(pill,Qt::AlignCenter,m_toast);
    }
}

bool LiveTextLayer::eventFilter(QObject* object, QEvent* event) {
    if (object == parentWidget() && event->type() == QEvent::Resize) setGeometry(parentWidget()->rect());
    if (object == parentWidget() && m_on && (event->type() == QEvent::TouchBegin || event->type() == QEvent::TouchUpdate ||
        event->type() == QEvent::TouchEnd || event->type() == QEvent::TouchCancel)) {
        event->accept(); return true; // Never let touch gestures draw through the text layer.
    }
    return QWidget::eventFilter(object,event);
}
bool LiveTextLayer::event(QEvent* event) {
    if (event->type() == QEvent::ShortcutOverride && m_on) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->matches(QKeySequence::Copy) || key->matches(QKeySequence::SelectAll)) { event->accept(); return true; }
    }
    return QWidget::event(event);
}
void LiveTextLayer::forward(QEvent* event) { QApplication::sendEvent(parentWidget(),event); }
void LiveTextLayer::mousePressEvent(QMouseEvent* event) {
    setFocus(Qt::MouseFocusReason);
    if (event->button()==Qt::MiddleButton || (m_space && event->button()==Qt::LeftButton)) { m_panning=true; forward(event); return; }
    if (event->button()!=Qt::LeftButton) { event->accept(); return; }
    const QPointF point = m_transform.inverted().map(event->position());
    const double tolerance = 10/std::max(.001,std::hypot(m_transform.m11(),m_transform.m12()));
    const int offset = m_layout.hit(point,tolerance);
    if (m_clickClock.isValid() && m_clickClock.elapsed() < QApplication::doubleClickInterval() && QLineF(m_clickPosition,event->position()).length() < QApplication::startDragDistance()) {
        const auto [begin,end] = m_layout.lineAt(offset); m_anchor=begin; m_caret=end; m_selecting=false; m_clickClock.invalidate();
    } else { m_anchor=m_caret=offset; m_selecting=offset>=0; }
    repaintImage(); event->accept();
}
void LiveTextLayer::mouseMoveEvent(QMouseEvent* event) {
    if (m_panning) { forward(event); return; }
    const QPointF point = m_transform.inverted().map(event->position());
    setCursor(m_layout.contains(point) ? Qt::IBeamCursor : Qt::ArrowCursor);
    if (m_selecting) { m_caret=m_layout.hit(point,0,true); repaintImage(); }
    event->accept();
}
void LiveTextLayer::mouseReleaseEvent(QMouseEvent* event) {
    if (m_panning) { m_panning=false; forward(event); return; }
    if (m_selecting) { m_caret=m_layout.hit(m_transform.inverted().map(event->position()),0,true); m_selecting=false; repaintImage(); }
    event->accept();
}
void LiveTextLayer::mouseDoubleClickEvent(QMouseEvent* event) {
    const int position = m_layout.hit(m_transform.inverted().map(event->position()),0);
    const auto [begin,end] = m_layout.wordAt(position);
    if (position>=0) { m_anchor=begin; m_caret=end; m_clickClock.start(); m_clickPosition=event->position(); }
    m_selecting=false; repaintImage(); event->accept();
}
void LiveTextLayer::wheelEvent(QWheelEvent* event) {
    if (event->modifiers() & Qt::ControlModifier) forward(event);
    else event->accept(); // Plain scrolling must not start a new scrolling capture.
}
void LiveTextLayer::keyPressEvent(QKeyEvent* event) {
    if (event->matches(QKeySequence::Copy)) copySelection();
    else if (event->matches(QKeySequence::SelectAll)) selectAll();
    else if (event->key()==Qt::Key_Space) { m_space=true; forward(event); }
    else if (event->key()==Qt::Key_Escape) turnOff();
    // No drawing or deletion keys pass into the annotation canvas.
    event->accept();
}
void LiveTextLayer::keyReleaseEvent(QKeyEvent* event) { if (event->key()==Qt::Key_Space) { m_space=false; forward(event); } event->accept(); }
void LiveTextLayer::contextMenuEvent(QContextMenuEvent* event) {
    auto* menu = new QMenu(this); menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->addAction(uiText("Copy"),this,[this] { copySelection(); })->setEnabled(!selectedText().isEmpty());
    menu->addAction(uiText("Copy all text"),this,[this] { copySelection(true); })->setEnabled(!m_layout.text.isEmpty());
    menu->addAction(uiText("Select all"),this,&LiveTextLayer::selectAll)->setEnabled(!m_layout.text.isEmpty());
    menu->popup(event->globalPos()); event->accept();
}
} // namespace hyprcapture::ui
