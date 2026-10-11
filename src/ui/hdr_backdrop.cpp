#include "ui/hdr_backdrop.hpp"
#include "shared/hdr_color.hpp"
#include "ui/hdr_image.hpp"
#include <QColorSpace>
#include <QDebug>
#include <QGuiApplication>
#include <QWindow>
#include <array>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>
#ifdef HYPRCAPTURE_HDR_WAYLAND
#include "color-management-client.h"
#include "viewporter-client.h"
#include <qpa/qplatformnativeinterface.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#endif
namespace hyprcapture::ui {
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
struct HdrBackdrop::Impl {
#ifdef HYPRCAPTURE_HDR_WAYLAND
  wl_display *display = nullptr;
  wl_event_queue *queue = nullptr;
  wl_registry *registry = nullptr;
  wl_compositor *compositor = nullptr;
  wl_subcompositor *subcompositor = nullptr;
  wl_shm *shm = nullptr;
  wp_viewporter *viewporter = nullptr;
  wp_color_manager_v1 *manager = nullptr;
  wl_surface *surface = nullptr;
  wl_subsurface *subsurface = nullptr;
  wp_viewport *viewport = nullptr;
  wp_color_management_surface_v1 *colorSurface = nullptr;
  wp_image_description_v1 *description = nullptr;
  wl_buffer *buffer = nullptr;
  bool pq = false, bt2020 = false, parametric = false, luminances = false,
       rgb30 = false, perceptual = false;
  bool descriptionReady = false, visible = false;
  QRect destination;
  QSize imageSize;
  QWindow *parent = nullptr;
  static void global(void *data, wl_registry *r, uint32_t id,
                     const char *interface, uint32_t version) {
    auto &s = *static_cast<Impl *>(data);
    if (!strcmp(interface, "wl_compositor"))
      s.compositor = static_cast<wl_compositor *>(wl_registry_bind(
          r, id, &wl_compositor_interface, std::min(version, 4U)));
    else if (!strcmp(interface, "wl_subcompositor"))
      s.subcompositor = static_cast<wl_subcompositor *>(
          wl_registry_bind(r, id, &wl_subcompositor_interface, 1));
    else if (!strcmp(interface, "wl_shm"))
      s.shm =
          static_cast<wl_shm *>(wl_registry_bind(r, id, &wl_shm_interface, 1));
    else if (!strcmp(interface, "wp_viewporter"))
      s.viewporter = static_cast<wp_viewporter *>(
          wl_registry_bind(r, id, &wp_viewporter_interface, 1));
    else if (!strcmp(interface, "wp_color_manager_v1"))
      s.manager = static_cast<wp_color_manager_v1 *>(
          wl_registry_bind(r, id, &wp_color_manager_v1_interface, 1));
  }
  bool roundtrip() { return wl_display_roundtrip_queue(display, queue) >= 0; }
  void attach() {
    wl_surface_attach(surface, visible ? buffer : nullptr, 0, 0);
    wl_surface_damage(surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(surface);
    wl_display_flush(display);
  }
  ~Impl() {
    if (subsurface)
      wl_subsurface_destroy(subsurface);
    if (viewport)
      wp_viewport_destroy(viewport);
    if (colorSurface)
      wp_color_management_surface_v1_destroy(colorSurface);
    if (surface)
      wl_surface_destroy(surface);
    if (buffer)
      wl_buffer_destroy(buffer);
    if (description)
      wp_image_description_v1_destroy(description);
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
  }
#endif
};
HdrBackdrop::HdrBackdrop(QWindow *parent) : m(std::make_unique<Impl>()) {
#ifdef HYPRCAPTURE_HDR_WAYLAND
  if (!parent || !QGuiApplication::platformName().startsWith("wayland"))
    return;
  auto *native = QGuiApplication::platformNativeInterface();
  m->display = static_cast<wl_display *>(
      native->nativeResourceForIntegration("wl_display"));
  auto *parentSurface = static_cast<wl_surface *>(
      native->nativeResourceForWindow("surface", parent));
  if (!m->display || !parentSurface)
    return;
  m->parent = parent;
  m->queue = wl_display_create_queue(m->display);
  m->registry = wl_display_get_registry(m->display);
  wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(m->registry), m->queue);
  static const wl_registry_listener registryListener{
      Impl::global, [](void *, wl_registry *, uint32_t) {}};
  wl_registry_add_listener(m->registry, &registryListener, m.get());
  if (!m->roundtrip() || !m->manager || !m->viewporter || !m->shm ||
      !m->subcompositor || !m->compositor)
    return;
  static const wl_shm_listener shmListener{
      [](void *d, wl_shm *, uint32_t format) {
        if (format == WL_SHM_FORMAT_XRGB2101010)
          static_cast<Impl *>(d)->rgb30 = true;
      }};
  wl_shm_add_listener(m->shm, &shmListener, m.get());
  static const wp_color_manager_v1_listener managerListener{
      [](void *d, wp_color_manager_v1 *, uint32_t i) {
        if (i == WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL)
          static_cast<Impl *>(d)->perceptual = true;
      },
      [](void *d, wp_color_manager_v1 *, uint32_t f) {
        auto &s = *static_cast<Impl *>(d);
        if (f == WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC)
          s.parametric = true;
        if (f == WP_COLOR_MANAGER_V1_FEATURE_SET_LUMINANCES)
          s.luminances = true;
      },
      [](void *d, wp_color_manager_v1 *, uint32_t tf) {
        if (tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ)
          static_cast<Impl *>(d)->pq = true;
      },
      [](void *d, wp_color_manager_v1 *, uint32_t p) {
        if (p == WP_COLOR_MANAGER_V1_PRIMARIES_BT2020)
          static_cast<Impl *>(d)->bt2020 = true;
      },
      [](void *, wp_color_manager_v1 *) {},
  };
  wp_color_manager_v1_add_listener(m->manager, &managerListener, m.get());
  if (!m->roundtrip() || !m->pq || !m->bt2020 || !m->parametric ||
      !m->luminances || !m->rgb30 || !m->perceptual)
    return;
  m->surface = wl_compositor_create_surface(m->compositor);
  auto *empty = wl_compositor_create_region(m->compositor);
  wl_surface_set_input_region(m->surface, empty);
  wl_region_destroy(empty);
  m->subsurface = wl_subcompositor_get_subsurface(m->subcompositor, m->surface,
                                                  parentSurface);
  wl_subsurface_place_below(m->subsurface, parentSurface);
  wl_subsurface_set_desync(m->subsurface);
  m->viewport = wp_viewporter_get_viewport(m->viewporter, m->surface);
  m->colorSurface = wp_color_manager_v1_get_surface(m->manager, m->surface);
#endif
}
HdrBackdrop::~HdrBackdrop() = default;
bool HdrBackdrop::ready() const {
#ifdef HYPRCAPTURE_HDR_WAYLAND
  return m->surface && m->descriptionReady;
#else
  return false;
#endif
}
bool HdrBackdrop::setImage(const QImage &image, const QRect &destination,
                           float referenceNits) {
#ifdef HYPRCAPTURE_HDR_WAYLAND
  if (!m->surface || image.isNull() || !destination.isValid())
    return false;
  float peak = 0;
  auto pq = hdrToPq10(image, &peak, referenceNits);
  if (pq.isNull())
    return false;
  auto *creator = wp_color_manager_v1_create_parametric_creator(m->manager);
  wp_image_description_creator_params_v1_set_primaries_named(
      creator, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);
  wp_image_description_creator_params_v1_set_tf_named(
      creator, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);
  // Pixels are already display-referred: describe their actual peak, not
  // PQ's 10000-nit coding range, to avoid a second compositor tone map.
  const auto reference = uint32_t(std::clamp(referenceNits, 1.0F, 10000.0F));
  wp_image_description_creator_params_v1_set_luminances(
      creator, 0, std::max(reference, uint32_t(std::ceil(peak))), reference);
  if (m->description)
    wp_image_description_v1_destroy(m->description);
  m->description = wp_image_description_creator_params_v1_create(creator);
  m->descriptionReady = false;
  static const wp_image_description_v1_listener listener{
      [](void *d, wp_image_description_v1 *, uint32_t, const char *) {
        static_cast<Impl *>(d)->descriptionReady = false;
      },
      [](void *d, wp_image_description_v1 *, uint32_t) {
        static_cast<Impl *>(d)->descriptionReady = true;
      }};
  wp_image_description_v1_add_listener(m->description, &listener, m.get());
  if (!m->roundtrip() || !m->descriptionReady)
    return false;
  int fd = memfd_create("hyprcapture-hdr-preview", MFD_CLOEXEC);
  if (fd < 0)
    return false;
  const auto size = pq.sizeInBytes();
  if (size > INT32_MAX || ftruncate(fd, size) < 0) {
    close(fd);
    return false;
  }
  void *memory = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (memory == MAP_FAILED) {
    close(fd);
    return false;
  }
  memcpy(memory, pq.constBits(), size);
  munmap(memory, size);
  auto *pool = wl_shm_create_pool(m->shm, fd, int(size));
  close(fd);
  auto *buffer =
      wl_shm_pool_create_buffer(pool, 0, pq.width(), pq.height(),
                                pq.bytesPerLine(), WL_SHM_FORMAT_XRGB2101010);
  wl_shm_pool_destroy(pool);
  if (m->buffer)
    wl_buffer_destroy(m->buffer);
  m->buffer = buffer;
  wp_color_management_surface_v1_set_image_description(
      m->colorSurface, m->description,
      WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
  m->imageSize = pq.size();
  setGeometry(destination);
  m->visible = true;
  m->attach();
  return true;
#else
  return false;
#endif
}
void HdrBackdrop::setGeometry(const QRect &destination) {
#ifdef HYPRCAPTURE_HDR_WAYLAND
  if (!m->surface || !destination.isValid())
    return;
  m->destination = destination;
  const QRect clipped =
      destination.intersected(QRect(QPoint{}, m->parent->size()));
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
  wl_surface_commit(m->surface);
  wl_display_flush(m->display);
#endif
}
void HdrBackdrop::setVisible(bool visible) {
#ifdef HYPRCAPTURE_HDR_WAYLAND
  if (!ready() || m->visible == visible)
    return;
  m->visible = visible;
  m->attach();
#endif
}
} // namespace hyprcapture::ui
