#include "ui/hdr_backdrop.hpp"
#include "shared/hdr_color.hpp"
#include "ui/hdr_image.hpp"
#include "ui/timing.hpp"
#include <QColorSpace>
#include <QDebug>
#include <QGuiApplication>
#include <QPointer>
#include <QTimer>
#include <QWindow>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <poll.h>
#include <set>
#include <thread>
#include <vector>
#ifdef HYPRCAPTURE_HDR_WAYLAND
#include "color-management-client.h"
#include "linux-dmabuf-client.h"
#include "presentation-time-client.h"
#include "viewporter-client.h"
#include <qpa/qplatformnativeinterface.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include <wayland-client.h>
#endif
namespace hyprcapture::ui {
#ifdef HYPRCAPTURE_HDR_TESTING
static unsigned globalsRequests = 0;
unsigned HdrBackdrop::globalsRequestsForTest() { return globalsRequests; }
#endif
QImage hdrToPq10(const QImage &image, float *peakNits, float referenceNits) {
  if (!isHdrImage(image))
    return {};
  if (!std::isfinite(referenceNits) || referenceNits <= 0)
    referenceNits = 203;
  const auto src = image.convertToFormat(QImage::Format_RGBA16FPx4);
  QImage out(src.size(), QImage::Format_RGB30);
  if (out.isNull())
    return {};
  static const auto table = [] {
    std::array<unsigned, 65536> t{};
    for (unsigned i = 0; i < t.size(); ++i) {
      const float v = color::halfToFloat(i);
      t[i] = std::isfinite(v)
                 ? unsigned(std::lround(color::nitsToPq(v * color::scRgbNits) *
                                        1023))
                 : 0;
    }
    return t;
  }();
  const auto *input = src.constBits();
  auto *output = out.bits();
  const float checkerDark =
      color::srgbToLinear(72.0F / 255) * referenceNits / color::scRgbNits;
  const float checkerLight =
      color::srgbToLinear(94.0F / 255) * referenceNits / color::scRgbNits;
  const int workers = src.width() * qint64(src.height()) >= 1024 * 1024 ? 4 : 1;
  std::array<float, 4> peaks{};
  auto convert = [&](int worker) {
    for (int y = src.height() * worker / workers;
         y < src.height() * (worker + 1) / workers; ++y) {
      const auto *row =
          reinterpret_cast<const uint16_t *>(input + y * src.bytesPerLine());
      auto *dst = reinterpret_cast<uint32_t *>(output + y * out.bytesPerLine());
      for (int x = 0; x < src.width(); ++x) {
        auto rgb = color::scRgbToBt2020({color::halfToFloat(row[x * 4]),
                                         color::halfToFloat(row[x * 4 + 1]),
                                         color::halfToFloat(row[x * 4 + 2])});
        const float rawAlpha = color::halfToFloat(row[x * 4 + 3]);
        const float alpha =
            std::isfinite(rawAlpha) ? std::clamp(rawAlpha, 0.0F, 1.0F) : 0;
        if (alpha < 1) {
          const float checker =
              ((x / 12 + y / 12) & 1) ? checkerDark : checkerLight;
          for (auto &v : rgb)
            v = v * alpha + checker * (1 - alpha);
        }
        unsigned codes[3];
        for (int c = 0; c < 3; ++c) {
          float v =
              std::isfinite(rgb[c]) ? std::clamp(rgb[c], 0.0F, 125.0F) : 0;
          peaks[worker] = std::max(peaks[worker], v * color::scRgbNits);
          codes[c] = table[color::floatToHalf(v)];
        }
        dst[x] = 0xc0000000U | (codes[0] << 20) | (codes[1] << 10) | codes[2];
      }
    }
  };
  std::vector<std::thread> threads;
  for (int i = 1; i < workers; ++i)
    threads.emplace_back(convert, i);
  convert(0);
  for (auto &t : threads)
    t.join();
  if (peakNits)
    *peakNits = *std::max_element(peaks.begin(), peaks.end());
  return out;
}
#ifdef HYPRCAPTURE_HDR_WAYLAND
namespace {
using Deadline = std::chrono::steady_clock::time_point;
struct NativeGlobals;
struct NativeBuffer {
  wl_buffer *buffer = nullptr;
  bool busy = false, retired = false;
  std::shared_ptr<const HdrGpuFrame> frame;
};
struct NativeGlobals : std::enable_shared_from_this<NativeGlobals> {
  wl_display *display = nullptr;
  wl_event_queue *queue = nullptr;
  wl_registry *registry = nullptr;
  wl_compositor *compositor = nullptr;
  wl_subcompositor *subcompositor = nullptr;
  wl_shm *shm = nullptr;
  wp_viewporter *viewporter = nullptr;
  wp_color_manager_v1 *manager = nullptr;
  zwp_linux_dmabuf_v1 *dma = nullptr;
  wp_presentation *presentation = nullptr;
  uint32_t presentationClock = 0;
  bool pq = false, bt2020 = false, srgb = false, linear = false,
       parametric = false, luminances = false, rgb30 = false,
       perceptual = false, closed = false;
  std::set<std::pair<uint32_t, uint64_t>> modifiers;
  struct Format {
    uint32_t format, padding;
    uint64_t modifier;
  };
  std::vector<Format> formatTable;
  struct Description {
    wp_image_description_v1 *object = nullptr;
    bool done = false, ready = false;
  };
  std::map<std::tuple<bool, uint32_t, uint32_t>, std::unique_ptr<Description>>
      descriptions;
  std::vector<std::unique_ptr<NativeBuffer>> buffers;
  std::map<void *, std::function<void()>> clients;
  std::map<zwp_linux_buffer_params_v1 *, std::function<void()>> pendingImports;
  QTimer *timer = nullptr;
  wl_callback *initialSync = nullptr;
  zwp_linux_dmabuf_feedback_v1 *dmaFeedback = nullptr;
  bool globalsReady = false, managerDone = false;
  std::map<void *, std::function<void()>> progress;
  std::shared_ptr<NativeGlobals> draining;
  void updateTimer() {
    const bool active =
        !clients.empty() || !buffers.empty() || !pendingImports.empty();
    if (active && !closed) {
      draining = shared_from_this();
      timer->start();
    } else {
      timer->stop();
      draining.reset();
    }
  }
  void awaitGlobals(bool bound = false) {
    initialSync = wl_display_sync(display);
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(initialSync), queue);
    static const wl_callback_listener first{
        [](void *p, wl_callback *c, uint32_t) {
          auto &s = *static_cast<NativeGlobals *>(p);
          wl_callback_destroy(c);
          s.initialSync = nullptr;
          s.awaitGlobals(true);
        }};
    static const wl_callback_listener second{
        [](void *p, wl_callback *c, uint32_t) {
          auto &s = *static_cast<NativeGlobals *>(p);
          wl_callback_destroy(c);
          s.initialSync = nullptr;
          s.globalsReady = true;
        }};
    wl_callback_add_listener(initialSync, bound ? &second : &first, this);
    wl_display_flush(display);
  }
  bool sync(Deadline deadline) {
    if (!display || closed || std::chrono::steady_clock::now() >= deadline)
      return false;
    bool done = false;
    auto *callback = wl_display_sync(display);
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(callback), queue);
    static const wl_callback_listener listener{
        [](void *p, wl_callback *, uint32_t) {
          *static_cast<bool *>(p) = true;
        }};
    wl_callback_add_listener(callback, &listener, &done);
    wl_display_flush(display);
    while (!done && std::chrono::steady_clock::now() < deadline) {
      if (wl_display_dispatch_queue_pending(display, queue) < 0)
        break;
      if (done)
        break;
      if (wl_display_prepare_read_queue(display, queue) != 0)
        continue;
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                            deadline - std::chrono::steady_clock::now())
                            .count();
      pollfd p{wl_display_get_fd(display), POLLIN, 0};
      if (left > 0 && poll(&p, 1, int(left)) > 0) {
        if (wl_display_read_events(display) < 0)
          break;
      } else
        wl_display_cancel_read(display);
    }
    wl_callback_destroy(callback);
    return done;
  }
  void drain() {
    if (display && !closed)
      wl_display_dispatch_queue_pending(display, queue);
    const auto callbacks = progress;
    for (const auto &[key, f] : callbacks)
      if (progress.contains(key))
        f();
    std::erase_if(buffers, [](const auto &b) {
      if (!b->retired || b->busy)
        return false;
      if (b->buffer)
        wl_buffer_destroy(b->buffer);
      return true;
    });
    updateTimer();
  }
  void retire(NativeBuffer *b) {
    if (!b)
      return;
    b->retired = true;
    drain();
  }
  static void global(void *p, wl_registry *r, uint32_t id, const char *name,
                     uint32_t v) {
    auto &s = *static_cast<NativeGlobals *>(p);
#define BIND(field, type, version)                                             \
  s.field = static_cast<type *>(wl_registry_bind(                              \
      r, id, &type##_interface, std::min(v, uint32_t(version))))
    if (!strcmp(name, "wl_compositor")) {
      BIND(compositor, wl_compositor, 4);
    } else if (!strcmp(name, "wl_subcompositor")) {
      BIND(subcompositor, wl_subcompositor, 1);
    } else if (!strcmp(name, "wl_shm")) {
      BIND(shm, wl_shm, 1);
      static const wl_shm_listener l{[](void *d, wl_shm *, uint32_t f) {
        if (f == WL_SHM_FORMAT_XRGB2101010)
          static_cast<NativeGlobals *>(d)->rgb30 = true;
      }};
      wl_shm_add_listener(s.shm, &l, p);
    } else if (!strcmp(name, "wp_viewporter")) {
      BIND(viewporter, wp_viewporter, 1);
    } else if (!strcmp(name, "wp_color_manager_v1")) {
      BIND(manager, wp_color_manager_v1, 1);
      static const wp_color_manager_v1_listener l{
          [](void *d, wp_color_manager_v1 *, uint32_t i) {
            if (i == WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL)
              static_cast<NativeGlobals *>(d)->perceptual = true;
          },
          [](void *d, wp_color_manager_v1 *, uint32_t f) {
            auto &s = *static_cast<NativeGlobals *>(d);
            s.parametric |= f == WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC;
            s.luminances |= f == WP_COLOR_MANAGER_V1_FEATURE_SET_LUMINANCES;
          },
          [](void *d, wp_color_manager_v1 *, uint32_t f) {
            auto &s = *static_cast<NativeGlobals *>(d);
            s.pq |= f == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ;
            s.linear |= f == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR;
          },
          [](void *d, wp_color_manager_v1 *, uint32_t v) {
            auto &s = *static_cast<NativeGlobals *>(d);
            s.bt2020 |= v == WP_COLOR_MANAGER_V1_PRIMARIES_BT2020;
            s.srgb |= v == WP_COLOR_MANAGER_V1_PRIMARIES_SRGB;
          },
          [](void *p, wp_color_manager_v1 *) {
            static_cast<NativeGlobals *>(p)->managerDone = true;
          }};
      wp_color_manager_v1_add_listener(s.manager, &l, p);
    } else if (!strcmp(name, "zwp_linux_dmabuf_v1") && v >= 3) {
      BIND(dma, zwp_linux_dmabuf_v1, 4);
      static const zwp_linux_dmabuf_v1_listener l{
          [](void *, zwp_linux_dmabuf_v1 *, uint32_t) {},
          [](void *d, zwp_linux_dmabuf_v1 *, uint32_t f, uint32_t hi,
             uint32_t lo) {
            static_cast<NativeGlobals *>(d)->modifiers.insert(
                {f, (uint64_t(hi) << 32) | lo});
          }};
      zwp_linux_dmabuf_v1_add_listener(s.dma, &l, p);
      if (v >= 4) {
        s.dmaFeedback = zwp_linux_dmabuf_v1_get_default_feedback(s.dma);
        static const zwp_linux_dmabuf_feedback_v1_listener feedback{
            [](void *, zwp_linux_dmabuf_feedback_v1 *) {},
            [](void *p, zwp_linux_dmabuf_feedback_v1 *, int32_t fd,
               uint32_t size) {
              auto &s = *static_cast<NativeGlobals *>(p);
              if (size && size % sizeof(Format) == 0) {
                void *data = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
                if (data != MAP_FAILED) {
                  const auto *formats = static_cast<const Format *>(data);
                  s.formatTable.assign(formats,
                                       formats + size / sizeof(Format));
                  munmap(data, size);
                }
              }
              ::close(fd);
            },
            [](void *, zwp_linux_dmabuf_feedback_v1 *, wl_array *a) {
              if (a->size == sizeof(dev_t)) {
                dev_t device;
                memcpy(&device, a->data, sizeof(device));
                HdrUpload::setMainDevice(device);
              }
            },
            [](void *, zwp_linux_dmabuf_feedback_v1 *) {},
            [](void *, zwp_linux_dmabuf_feedback_v1 *, wl_array *) {},
            [](void *p, zwp_linux_dmabuf_feedback_v1 *, wl_array *a) {
              auto &s = *static_cast<NativeGlobals *>(p);
              const auto *indices = static_cast<const uint16_t *>(a->data);
              for (size_t i = 0; i < a->size / sizeof(uint16_t); ++i)
                if (indices[i] < s.formatTable.size()) {
                  const auto &f = s.formatTable[indices[i]];
                  s.modifiers.insert({f.format, f.modifier});
                }
            },
            [](void *, zwp_linux_dmabuf_feedback_v1 *, uint32_t) {}};
        zwp_linux_dmabuf_feedback_v1_add_listener(s.dmaFeedback, &feedback, p);
      }
    } else if (!strcmp(name, "wp_presentation")) {
      BIND(presentation, wp_presentation, 1);
      static const wp_presentation_listener l{
          [](void *p, wp_presentation *, uint32_t clock) {
            static_cast<NativeGlobals *>(p)->presentationClock = clock;
          }};
      wp_presentation_add_listener(s.presentation, &l, p);
    }
#undef BIND
  }
  bool common() const {
    return display && compositor && subcompositor && viewporter && manager &&
           parametric && luminances && perceptual && !closed;
  }
  bool fp16() const {
    return common() && dma && linear && bt2020 &&
           std::ranges::any_of(modifiers,
                               [](const auto &m) {
                                 return m.first == WL_SHM_FORMAT_ABGR16161616F;
                               }) &&
           qEnvironmentVariable("HYPRCAPTURE_HDR_DMABUF") != "0";
  }
  wp_image_description_v1 *description(bool fp16, float peak, float reference,
                                       Deadline deadline) {
    const auto ref = uint32_t(std::clamp(reference, 1.F, 10000.F));
    const auto envelope = std::min(
        10000U,
        std::max(ref,
                 uint32_t(std::ceil(std::max(peak, 203.F) / 203.F)) * 203U));
    auto key = std::tuple{fp16, ref, envelope};
    auto i = descriptions.find(key);
    if (i == descriptions.end()) {
      auto d = std::make_unique<Description>();
      auto *c = wp_color_manager_v1_create_parametric_creator(manager);
      wp_image_description_creator_params_v1_set_primaries_named(
          c, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);
      wp_image_description_creator_params_v1_set_tf_named(
          c, fp16 ? WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR
                  : WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);
      // Same reference and content envelope as the old PQ surface. Hyprland's
      // ext_linear TF range is fixed at 80 nits, independently of this
      // envelope.
      wp_image_description_creator_params_v1_set_luminances(c, 0, envelope,
                                                            ref);
      d->object = wp_image_description_creator_params_v1_create(c);
      static const wp_image_description_v1_listener l{
          [](void *p, wp_image_description_v1 *, uint32_t, const char *) {
            static_cast<Description *>(p)->done = true;
          },
          [](void *p, wp_image_description_v1 *, uint32_t) {
            auto &d = *static_cast<Description *>(p);
            d.done = d.ready = true;
          }};
      wp_image_description_v1_add_listener(d->object, &l, d.get());
      i = descriptions.emplace(key, std::move(d)).first;
    }
    if (!i->second->done && !sync(deadline))
      return nullptr;
    return i->second->ready ? i->second->object : nullptr;
  }
  NativeBuffer *own(wl_buffer *b,
                    std::shared_ptr<const HdrGpuFrame> frame = {}) {
    auto value = std::make_unique<NativeBuffer>();
    value->buffer = b;
    value->frame = std::move(frame);
    auto *result = value.get();
    static const wl_buffer_listener listener{[](void *p, wl_buffer *) {
      static_cast<NativeBuffer *>(p)->busy = false;
    }};
    wl_buffer_add_listener(b, &listener, result);
    buffers.push_back(std::move(value));
    return result;
  }
  void close() {
    if (closed)
      return;
    closed = true;
    // Destruction happens before Qt tears down its wl_display. Retired busy
    // BOs are never recycled without release, even during application shutdown.
    if (timer)
      timer->stop();
    auto copy = clients;
    for (auto &[_, f] : copy)
      f();
    for (auto &[_, cleanup] : pendingImports)
      cleanup();
    pendingImports.clear();
    static auto *unreleased =
        new std::vector<std::shared_ptr<const HdrGpuFrame>>;
    for (auto &b : buffers) {
      if (b->busy && b->frame)
        unreleased->push_back(b->frame);
      wl_buffer_destroy(b->buffer);
    }
    buffers.clear();
    for (auto &[_, d] : descriptions)
      wp_image_description_v1_destroy(d->object);
    descriptions.clear();
    if (presentation)
      wp_presentation_destroy(presentation);
    if (initialSync)
      wl_callback_destroy(initialSync);
    if (dmaFeedback)
      zwp_linux_dmabuf_feedback_v1_destroy(dmaFeedback);
    if (dma)
      zwp_linux_dmabuf_v1_destroy(dma);
    if (manager)
      wp_color_manager_v1_destroy(manager);
    if (viewporter)
      wp_viewporter_destroy(viewporter);
    if (shm)
      wl_shm_destroy(shm);
    if (subcompositor)
      wl_subcompositor_destroy(subcompositor);
    if (compositor)
      wl_compositor_destroy(compositor);
    if (registry)
      wl_registry_destroy(registry);
    if (queue)
      wl_event_queue_destroy(queue);
    display = nullptr;
    draining.reset();
  }
  ~NativeGlobals() {
    close();
    if (timer)
      timer->deleteLater();
  }
  static std::shared_ptr<NativeGlobals> get() {
#ifdef HYPRCAPTURE_HDR_TESTING
    ++globalsRequests;
#endif
    static std::weak_ptr<NativeGlobals> cached;
    if (auto c = cached.lock())
      return c;
    if (!QGuiApplication::instance() ||
        !QGuiApplication::platformName().startsWith("wayland"))
      return {};
    auto c = std::make_shared<NativeGlobals>();
    auto *native = QGuiApplication::platformNativeInterface();
    c->display = static_cast<wl_display *>(
        native->nativeResourceForIntegration("wl_display"));
    if (!c->display)
      return {};
    c->queue = wl_display_create_queue(c->display);
    c->registry = wl_display_get_registry(c->display);
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(c->registry), c->queue);
    static const wl_registry_listener l{global,
                                        [](void *, wl_registry *, uint32_t) {}};
    wl_registry_add_listener(c->registry, &l, c.get());
    c->awaitGlobals();
    c->timer = new QTimer(QCoreApplication::instance());
    c->timer->setObjectName("hyprcaptureHdrDrain");
    c->timer->setInterval(4);
    const std::weak_ptr<NativeGlobals> weak = c;
    QObject::connect(c->timer, &QTimer::timeout, c->timer, [weak] {
      if (auto c = weak.lock())
        c->drain();
    });
    QObject::connect(QCoreApplication::instance(),
                     &QCoreApplication::aboutToQuit, c->timer, [weak] {
                       if (auto c = weak.lock())
                         c->close();
                     });
    cached = c;
    return c;
  }
};
} // namespace
#endif
struct HdrBackdrop::Impl {
  std::function<void(bool)> completed;
#ifdef HYPRCAPTURE_HDR_TESTING
  bool testReady = false;
#endif
#ifdef HYPRCAPTURE_HDR_WAYLAND
  std::shared_ptr<NativeGlobals> globals;
  wl_surface *surface = nullptr;
  wl_subsurface *subsurface = nullptr;
  wp_viewport *viewport = nullptr;
  wp_color_management_surface_v1 *colorSurface = nullptr;
  NativeBuffer *buffer = nullptr;
  bool descriptionReady = false, visible = false, presentationPending = false;
  std::set<struct wp_presentation_feedback *> feedback;
  QRect destination, clippedDestination;
  QSize imageSize;
  QPointer<QWindow> parent;
  bool aboveParent = false, synchronized = false;
  std::function<void()> pending;
  QRect pendingDestination;
  bool requestedVisible = true;
  void activate() {
    auto &g = *globals;
    if (g.clients.contains(this))
      return;
    g.clients[this] = [this] { close(); };
    g.progress[this] = [this] {
      createSurface();
      if (auto retry = pending)
        retry();
    };
    g.updateTimer();
  }
  bool createSurface() {
    if (surface)
      return true;
    if (!parent || !globals || !globals->common())
      return false;
    auto *native = QGuiApplication::platformNativeInterface();
    auto *parentSurface = static_cast<wl_surface *>(
        native->nativeResourceForWindow("surface", parent));
    if (!parentSurface)
      return false;
    auto &g = *globals;
    surface = wl_compositor_create_surface(g.compositor);
    auto *empty = wl_compositor_create_region(g.compositor);
    wl_surface_set_input_region(surface, empty);
    wl_region_destroy(empty);
    subsurface = wl_subcompositor_get_subsurface(g.subcompositor, surface,
                                                 parentSurface);
    if (aboveParent)
      wl_subsurface_place_above(subsurface, parentSurface);
    else
      wl_subsurface_place_below(subsurface, parentSurface);
    if (!synchronized)
      wl_subsurface_set_desync(subsurface);
    viewport = wp_viewporter_get_viewport(g.viewporter, surface);
    colorSurface = wp_color_manager_v1_get_surface(g.manager, surface);
    return true;
  }
  void attach() {
    if (!surface || !globals || globals->closed)
      return;
    if (visible && buffer) {
      buffer->busy = true;
      wl_surface_attach(surface, buffer->buffer, 0, 0);
      if (globals->presentation && !presentationPending) {
        presentationPending = true;
        auto *f = wp_presentation_feedback(globals->presentation, surface);
        feedback.insert(f);
        static const wp_presentation_feedback_listener l{
            [](void *, struct wp_presentation_feedback *, wl_output *) {},
            [](void *p, struct wp_presentation_feedback *f, uint32_t hi,
               uint32_t lo, uint32_t ns, uint32_t, uint32_t, uint32_t,
               uint32_t) {
              auto &s = *static_cast<Impl *>(p);
              traceTiming(QStringLiteral("hdr.presented mode=%1 seconds=%2 "
                                         "nanoseconds=%3 clock=%4")
                              .arg(s.buffer && s.buffer->frame ? "fp16" : "pq")
                              .arg((quint64(hi) << 32) | lo)
                              .arg(ns)
                              .arg(s.globals->presentationClock));
              s.feedback.erase(f);
              wp_presentation_feedback_destroy(f);
            },
            [](void *p, struct wp_presentation_feedback *f) {
              auto &s = *static_cast<Impl *>(p);
              s.presentationPending = false;
              s.feedback.erase(f);
              wp_presentation_feedback_destroy(f);
            }};
        wp_presentation_feedback_add_listener(f, &l, this);
      }
    } else
      wl_surface_attach(surface, nullptr, 0, 0);
    wl_surface_damage(surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(surface);
    wl_display_flush(globals->display);
  }
  void close() {
    pending = {};
    if (globals) {
      globals->progress.erase(this);
      globals->clients.erase(this);
    }
    if (!surface) {
      if (globals)
        globals->updateTimer();
      return;
    }
    for (auto *f : feedback)
      wp_presentation_feedback_destroy(f);
    feedback.clear();
    if (subsurface)
      wl_subsurface_destroy(subsurface);
    if (viewport)
      wp_viewport_destroy(viewport);
    if (colorSurface)
      wp_color_management_surface_v1_destroy(colorSurface);
    wl_surface_destroy(surface);
    surface = nullptr;
    subsurface = nullptr;
    viewport = nullptr;
    colorSurface = nullptr;
    globals->retire(buffer);
    buffer = nullptr;
    globals->updateTimer();
  }
  ~Impl() {
    close();
    if (globals) {
      globals->clients.erase(this);
      globals->progress.erase(this);
      globals->updateTimer();
    }
  }
#endif
};
void HdrBackdrop::prewarm() { HdrUpload::prewarm(); }
bool HdrBackdrop::fp16Available() {
#ifdef HYPRCAPTURE_HDR_WAYLAND
  auto g = NativeGlobals::get();
  return g && g->fp16();
#else
  return false;
#endif
}
HdrBackdrop::HdrBackdrop(QWindow *parent, bool aboveParent, bool synchronized)
    : m(std::make_unique<Impl>()) {
#ifdef HYPRCAPTURE_HDR_WAYLAND
  if (!parent)
    return;
  m->globals = NativeGlobals::get();
  if (!m->globals)
    return;
  m->parent = parent;
  m->aboveParent = aboveParent;
  m->synchronized = synchronized;
  m->activate();
  m->createSurface();
#endif
}
HdrBackdrop::~HdrBackdrop() = default;
void HdrBackdrop::setReadyCallback(std::function<void(bool)> callback) {
  m->completed = std::move(callback);
}
#ifdef HYPRCAPTURE_HDR_TESTING
void HdrBackdrop::setReadyForTest(bool ready) { m->testReady = ready; }
std::unique_ptr<HdrBackdrop> HdrBackdrop::unavailableForTest(QWindow *parent) {
#ifdef HYPRCAPTURE_HDR_WAYLAND
  auto backdrop = std::make_unique<HdrBackdrop>(nullptr);
  auto g = std::make_shared<NativeGlobals>();
  g->globalsReady = true; // registry completed without color-management support
  g->timer = new QTimer(QCoreApplication::instance());
  g->timer->setObjectName("hyprcaptureHdrDrain");
  g->timer->setInterval(4);
  backdrop->m->globals = std::move(g);
  backdrop->m->parent = parent;
  backdrop->m->activate();
  return backdrop;
#else
  return {};
#endif
}
#endif
bool HdrBackdrop::ready() const {
#ifdef HYPRCAPTURE_HDR_TESTING
  if (m->testReady)
    return true;
#endif
#ifdef HYPRCAPTURE_HDR_WAYLAND
  return m->surface && m->buffer && m->descriptionReady && m->globals &&
         !m->globals->closed;
#else
  return false;
#endif
}
bool HdrBackdrop::setPreparedImage(
    const std::shared_ptr<const HdrGpuFrame> &frame, const QRect &destination,
    std::chrono::steady_clock::time_point deadline) {
#ifdef HYPRCAPTURE_HDR_WAYLAND
  if (m->globals && !m->globals->closed && !m->surface) {
    m->activate();
    m->globals->sync(deadline);
    m->createSurface();
  }
  if (!m->surface || !m->globals->fp16() || !frame || frame->fd < 0 ||
      !destination.isValid() ||
      !m->globals->modifiers.contains(
          {WL_SHM_FORMAT_ABGR16161616F, frame->modifier}))
    return false;
  auto &g = *m->globals;
  auto *desc =
      g.description(true, frame->peakNits, frame->referenceNits, deadline);
  if (!desc)
    return false;
  struct Import {
    wl_buffer *buffer = nullptr;
    bool done = false, abandoned = false;
    std::shared_ptr<const HdrGpuFrame> frame;
    NativeGlobals *owner = nullptr;
  };
  auto result = std::make_shared<Import>();
  result->frame = frame;
  result->owner = &g;
  auto *params = zwp_linux_dmabuf_v1_create_params(g.dma);
  auto *listenerData = new std::shared_ptr<Import>(result);
  g.pendingImports[params] = [params, listenerData] {
    zwp_linux_buffer_params_v1_destroy(params);
    delete listenerData;
  };
  static const zwp_linux_buffer_params_v1_listener l{
      [](void *p, zwp_linux_buffer_params_v1 *params, wl_buffer *b) {
        auto *hold = static_cast<std::shared_ptr<Import> *>(p);
        auto &result = **hold;
        result.done = true;
        if (result.abandoned)
          wl_buffer_destroy(b);
        else
          result.buffer = b;
        result.owner->pendingImports.erase(params);
        zwp_linux_buffer_params_v1_destroy(params);
        delete hold;
      },
      [](void *p, zwp_linux_buffer_params_v1 *params) {
        auto *hold = static_cast<std::shared_ptr<Import> *>(p);
        auto &result = **hold;
        result.done = true;
        result.owner->pendingImports.erase(params);
        zwp_linux_buffer_params_v1_destroy(params);
        delete hold;
      }};
  zwp_linux_buffer_params_v1_add_listener(params, &l, listenerData);
  zwp_linux_buffer_params_v1_add(params, frame->fd, 0, frame->offset,
                                 frame->stride, frame->modifier >> 32,
                                 frame->modifier & 0xffffffff);
  zwp_linux_buffer_params_v1_create(params, frame->width, frame->height,
                                    WL_SHM_FORMAT_ABGR16161616F, 0);
  if (!g.sync(deadline) || !result->done || !result->buffer) {
    // Late import completion owns its frame and has no pointer to the widget.
    // Keep the request/listener until its created/failed event, then discard
    // it.
    result->abandoned = true;
    if (result->buffer)
      wl_buffer_destroy(result->buffer);
    return false;
  }
  auto *buffer = g.own(result->buffer, frame);
  g.retire(m->buffer);
  m->buffer = buffer;
  m->descriptionReady = true;
  wp_color_management_surface_v1_set_image_description(
      m->colorSurface, desc, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
  m->imageSize = {frame->width, frame->height};
  m->destination = {};
  m->visible = true;
  traceTiming("hdr.attach_ready mode=fp16");
  setGeometry(destination);
  return true;
#else
  return false;
#endif
}
bool HdrBackdrop::setImage(const QImage &image, const QRect &destination,
                           float reference) {
#ifdef HYPRCAPTURE_HDR_WAYLAND
  if (!m->globals)
    return false;
#else
  return false;
#endif
  float peak = 0;
  auto pq = hdrToPq10(image, &peak, reference);
  return setPqImage(pq, destination, peak, reference);
}
bool HdrBackdrop::setPqImage(const QImage &pq, const QRect &destination,
                             float peak, float reference) {
#ifdef HYPRCAPTURE_HDR_WAYLAND
  m->pending = {};
  const auto fail = [&] {
    setVisible(false);
    m->descriptionReady = false;
    m->close();
    return false;
  };
  if (!m->globals || pq.isNull() || pq.format() != QImage::Format_RGB30 ||
      !destination.isValid())
    return fail();
  auto &g = *m->globals;
  if (g.closed)
    return fail();
  m->activate();
  m->pendingDestination = destination;
  const auto defer = [&] {
    if (m->visible) {
      m->visible = false;
      m->attach();
    }
    m->descriptionReady = false;
    m->pending = [this, pq, peak, reference] {
      const auto destination = m->pendingDestination;
      const bool accepted = setPqImage(pq, destination, peak, reference);
      if (!m->pending) {
        if (m->parent)
          m->parent->requestUpdate();
        if (auto callback = m->completed)
          callback(accepted && ready());
      }
    };
    wl_display_flush(g.display);
    return true;
  };
  if (!g.globalsReady || (g.manager && !g.managerDone))
    return defer();
  if (!g.common() || !g.shm || !g.rgb30 || !g.pq || !g.bt2020 || !m->parent)
    return fail();
  if (!m->createSurface())
    return defer();
  auto *desc =
      g.description(false, peak, reference, std::chrono::steady_clock::now());
  if (!desc) {
    const bool waiting = std::ranges::any_of(
        g.descriptions, [](const auto &d) { return !d.second->done; });
    return waiting ? defer() : fail();
  }
  int fd = memfd_create("hyprcapture-hdr-preview", MFD_CLOEXEC);
  if (fd < 0)
    return fail();
  auto size = pq.sizeInBytes();
  if (size > INT32_MAX || ftruncate(fd, size) < 0) {
    close(fd);
    return fail();
  }
  void *memory = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (memory == MAP_FAILED) {
    close(fd);
    return fail();
  }
  memcpy(memory, pq.constBits(), size);
  munmap(memory, size);
  auto *pool = wl_shm_create_pool(g.shm, fd, int(size));
  close(fd);
  auto *buffer =
      wl_shm_pool_create_buffer(pool, 0, pq.width(), pq.height(),
                                pq.bytesPerLine(), WL_SHM_FORMAT_XRGB2101010);
  wl_shm_pool_destroy(pool);
  g.retire(m->buffer);
  m->buffer = g.own(buffer);
  m->descriptionReady = true;
  wp_color_management_surface_v1_set_image_description(
      m->colorSurface, desc, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
  m->imageSize = pq.size();
  m->destination = {};
  m->visible = m->requestedVisible;
  traceTiming("hdr.attach_ready mode=pq");
  setGeometry(destination);
  return true;
#else
  return false;
#endif
}
void HdrBackdrop::setGeometry(const QRect &destination) {
#ifdef HYPRCAPTURE_HDR_WAYLAND
  if (destination.isValid() && m->pending)
    m->pendingDestination = destination;
  if (!m->surface || !m->parent || !destination.isValid() || m->globals->closed)
    return;
  auto clipped = destination.intersected(QRect(QPoint{}, m->parent->size()));
  if (destination == m->destination && clipped == m->clippedDestination)
    return;
  m->destination = destination;
  m->clippedDestination = clipped;
  if (clipped.isEmpty()) {
    setVisible(false);
    return;
  }
  const double sx = double(m->imageSize.width()) / destination.width(),
               sy = double(m->imageSize.height()) / destination.height();
  wp_viewport_set_source(
      m->viewport, wl_fixed_from_double((clipped.x() - destination.x()) * sx),
      wl_fixed_from_double((clipped.y() - destination.y()) * sy),
      wl_fixed_from_double(clipped.width() * sx),
      wl_fixed_from_double(clipped.height() * sy));
  wl_subsurface_set_position(m->subsurface, clipped.x(), clipped.y());
  wp_viewport_set_destination(m->viewport, clipped.width(), clipped.height());
  m->attach();
#endif
}
void HdrBackdrop::setVisible(bool visible) {
#ifdef HYPRCAPTURE_HDR_WAYLAND
  m->requestedVisible = visible;
  if (!ready() || m->visible == visible)
    return;
  m->visible = visible;
  m->attach();
#endif
}
} // namespace hyprcapture::ui
