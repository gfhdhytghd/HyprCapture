// Standalone capability/presentation experiment. No HyprCapture plugin needed.
#include "color-management-client.h"
#include "linux-dmabuf-client.h"
#include "presentation-time-client.h"
#include "xdg-shell-client.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
// clang-format off
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
// clang-format on
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <poll.h>
#include <set>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>
#include <wayland-client.h>
using Clock = std::chrono::steady_clock;
static auto start = Clock::now();
static double ms() {
  return std::chrono::duration<double, std::milli>(Clock::now() - start)
      .count();
}
static std::string fourcc(uint32_t f) {
  if (f == 0)
    return "ARGB8888";
  if (f == 1)
    return "XRGB8888";
  char s[5]{};
  for (int i = 0; i < 4; ++i)
    s[i] = char(f >> (i * 8));
  return s;
}
static const char *cmName(const char *kind, uint32_t value) {
  static const char *features[]{"icc_v2_v4",
                                "parametric",
                                "set_primaries",
                                "set_tf_power",
                                "set_luminances",
                                "set_mastering_display_primaries",
                                "extended_target_volume",
                                "windows_scrgb"};
  static const char *tfs[]{"unused", "bt1886",     "gamma22",  "gamma28",
                           "st240",  "ext_linear", "log_100",  "log_316",
                           "xvycc",  "srgb",       "ext_srgb", "st2084_pq",
                           "st428",  "hlg"};
  static const char *primaries[]{"unused",     "srgb",        "pal_m",
                                 "pal",        "ntsc",        "generic_film",
                                 "bt2020",     "cie1931_xyz", "dci_p3",
                                 "display_p3", "adobe_rgb"};
  static const char *intents[]{"perceptual", "relative", "saturation",
                               "absolute", "relative_bpc"};
  if (!strcmp(kind, "feature") && value < std::size(features))
    return features[value];
  if (!strcmp(kind, "tf") && value < std::size(tfs))
    return tfs[value];
  if (!strcmp(kind, "primaries") && value < std::size(primaries))
    return primaries[value];
  if (!strcmp(kind, "intent") && value < std::size(intents))
    return intents[value];
  return "unknown";
}
struct State {
  wl_display *d{};
  wl_compositor *c{};
  wl_shm *shm{};
  zwp_linux_dmabuf_v1 *dma{};
  wp_color_manager_v1 *cm{};
  xdg_wm_base *wm{};
  wp_presentation *pres{};
  std::set<uint32_t> formats, features, tfs, primaries, intents;
  std::set<std::pair<uint32_t, uint64_t>> modifiers;
  bool ready = false, configured = false, frame = false, presented = false,
       discarded = false;
};
static const wl_shm_listener shml{[](void *p, wl_shm *, uint32_t f) {
  auto &s = *(State *)p;
  s.formats.insert(f);
  printf("shm %s 0x%08x\n", fourcc(f).c_str(), f);
}};
static const zwp_linux_dmabuf_v1_listener dmal{
    [](void *, zwp_linux_dmabuf_v1 *, uint32_t f) {
      printf("dmabuf-format %s\n", fourcc(f).c_str());
    },
    [](void *p, zwp_linux_dmabuf_v1 *, uint32_t f, uint32_t hi, uint32_t lo) {
      auto &s = *(State *)p;
      uint64_t m = (uint64_t(hi) << 32) | lo;
      s.modifiers.insert({f, m});
      printf("dmabuf %s modifier=0x%016llx\n", fourcc(f).c_str(),
             (unsigned long long)m);
    }};
static const wp_color_manager_v1_listener cml{
    [](void *p, wp_color_manager_v1 *, uint32_t v) {
      ((State *)p)->intents.insert(v);
      printf("cm intent %u %s\n", v, cmName("intent", v));
    },
    [](void *p, wp_color_manager_v1 *, uint32_t v) {
      ((State *)p)->features.insert(v);
      printf("cm feature %u %s\n", v, cmName("feature", v));
    },
    [](void *p, wp_color_manager_v1 *, uint32_t v) {
      ((State *)p)->tfs.insert(v);
      printf("cm tf %u %s\n", v, cmName("tf", v));
    },
    [](void *p, wp_color_manager_v1 *, uint32_t v) {
      ((State *)p)->primaries.insert(v);
      printf("cm primaries %u %s\n", v, cmName("primaries", v));
    },
    [](void *, wp_color_manager_v1 *) { puts("cm done"); }};
static const xdg_wm_base_listener wml{
    [](void *, xdg_wm_base *w, uint32_t n) { xdg_wm_base_pong(w, n); }};
static const wp_presentation_listener pl{
    [](void *, wp_presentation *, uint32_t c) {
      printf("presentation clock=%u\n", c);
    }};
static const wl_registry_listener reg{
    [](void *p, wl_registry *r, uint32_t id, const char *name, uint32_t ver) {
      auto &s = *(State *)p;
#define BIND(field, type, v)                                                   \
  s.field = (type *)wl_registry_bind(r, id, &type##_interface,                 \
                                     std::min(ver, uint32_t(v)))
      if (!strcmp(name, "wl_compositor")) {
        BIND(c, wl_compositor, 4);
      } else if (!strcmp(name, "wl_shm")) {
        BIND(shm, wl_shm, 1);
        wl_shm_add_listener(s.shm, &shml, p);
      } else if (!strcmp(name, "zwp_linux_dmabuf_v1")) {
        BIND(dma, zwp_linux_dmabuf_v1, 3);
        zwp_linux_dmabuf_v1_add_listener(s.dma, &dmal, p);
      } else if (!strcmp(name, "wp_color_manager_v1")) {
        BIND(cm, wp_color_manager_v1, 1);
        wp_color_manager_v1_add_listener(s.cm, &cml, p);
      } else if (!strcmp(name, "xdg_wm_base")) {
        BIND(wm, xdg_wm_base, 1);
        xdg_wm_base_add_listener(s.wm, &wml, p);
      } else if (!strcmp(name, "wp_presentation")) {
        BIND(pres, wp_presentation, 1);
        wp_presentation_add_listener(s.pres, &pl, p);
      }
#undef BIND
    },
    [](void *, wl_registry *, uint32_t) {}};
static const wp_image_description_v1_listener desc{
    [](void *, wp_image_description_v1 *, uint32_t cause, const char *message) {
      printf("description FAILED %u %s\n", cause, message);
    },
    [](void *p, wp_image_description_v1 *, uint32_t id) {
      ((State *)p)->ready = true;
      printf("description READY %u at %.3f ms\n", id, ms());
    }};
static const xdg_surface_listener xsl{[](void *p, xdg_surface *x, uint32_t n) {
  xdg_surface_ack_configure(x, n);
  ((State *)p)->configured = true;
}};
static const xdg_toplevel_listener tl{
    [](void *, xdg_toplevel *, int32_t, int32_t, wl_array *) {},
    [](void *, xdg_toplevel *) {}};
static const wl_callback_listener fl{[](void *p, wl_callback *cb, uint32_t) {
  ((State *)p)->frame = true;
  printf("FRAME callback %.3f ms (not presentation)\n", ms());
  wl_callback_destroy(cb);
}};
static const wp_presentation_feedback_listener pfl{
    [](void *, struct wp_presentation_feedback *, wl_output *) {},
    [](void *p, struct wp_presentation_feedback *f, uint32_t hi, uint32_t lo,
       uint32_t ns, uint32_t refresh, uint32_t, uint32_t, uint32_t flags) {
      ((State *)p)->presented = true;
      printf("PRESENTED %.3f ms timestamp=%llu.%09u refresh_ns=%u flags=%u\n",
             ms(), (unsigned long long)((uint64_t(hi) << 32) | lo), ns, refresh,
             flags);
      wp_presentation_feedback_destroy(f);
    },
    [](void *p, struct wp_presentation_feedback *f) {
      ((State *)p)->discarded = true;
      puts("DISCARDED");
      wp_presentation_feedback_destroy(f);
    }};
static void fill(void *mem, int stride, int w, int h) {
  for (int y = 0; y < h; ++y) {
    auto *row = (uint16_t *)((char *)mem + stride * y);
    for (int x = 0; x < w; ++x) {
      // Opaque gray patches: 0, 80, 203, 400, 800 nits; saturated patches
      // include negative scRGB.
      const uint16_t gray[]{0, 0x3c00, 0x4113, 0x4500, 0x4900};
      auto v = gray[std::min(4, x * 5 / w)];
      row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = v;
      row[x * 4 + 3] = 0x3c00;
      if (y >= h / 2) {
        row[x * 4] = x < w / 2 ? 0xb800 : 0x4500;
        row[x * 4 + 1] = 0x4100;
        row[x * 4 + 2] = 0xb400;
      }
    }
  }
}
// Intel's GBM CPU map rejects this 64-bit float BO; upload via EGL instead.
static bool upload(gbm_device *dev, gbm_bo *bo, int w, int h) {
  auto getPlatform = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress(
      "eglGetPlatformDisplayEXT");
  auto createImage =
      (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
  auto destroyImage =
      (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
  auto imageTarget = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress(
      "glEGLImageTargetTexture2DOES");
  if (!getPlatform || !createImage || !destroyImage || !imageTarget)
    return false;
  auto d = getPlatform(EGL_PLATFORM_GBM_KHR, dev, nullptr);
  if (!eglInitialize(d, nullptr, nullptr))
    return false;
  eglBindAPI(EGL_OPENGL_ES_API);
  EGLConfig config;
  EGLint count;
  const EGLint attrs[]{EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
  if (!eglChooseConfig(d, attrs, &config, 1, &count) || !count)
    return false;
  const EGLint ctxAttrs[]{EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  auto ctx = eglCreateContext(d, config, EGL_NO_CONTEXT, ctxAttrs);
  if (!eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
    return false;
  printf("GPU %s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
  int fd = gbm_bo_get_fd(bo);
  uint64_t modifier = gbm_bo_get_modifier(bo);
  const EGLint ia[]{EGL_WIDTH,
                    w,
                    EGL_HEIGHT,
                    h,
                    EGL_LINUX_DRM_FOURCC_EXT,
                    DRM_FORMAT_ABGR16161616F,
                    EGL_DMA_BUF_PLANE0_FD_EXT,
                    fd,
                    EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                    (EGLint)gbm_bo_get_offset(bo, 0),
                    EGL_DMA_BUF_PLANE0_PITCH_EXT,
                    (EGLint)gbm_bo_get_stride(bo),
                    EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
                    (EGLint)modifier,
                    EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
                    (EGLint)(modifier >> 32),
                    EGL_NONE};
  auto im = createImage(d, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, ia);
  close(fd);
  if (im == EGL_NO_IMAGE_KHR) {
    fprintf(stderr, "EGL import error %x\n", eglGetError());
    return false;
  }
  GLuint textures[2], fb, vao;
  glGenTextures(2, textures);
  glGenFramebuffers(1, &fb);
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  glBindTexture(GL_TEXTURE_2D, textures[1]);
  imageTarget(GL_TEXTURE_2D, im);
  glBindFramebuffer(GL_FRAMEBUFFER, fb);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         textures[1], 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    return false;
  std::vector<uint16_t> data(size_t(w) * h * 4);
  fill(data.data(), w * 8, w, h);
  auto t = Clock::now();
  glBindTexture(GL_TEXTURE_2D, textures[0]);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT,
               data.data());
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  const char *vs = "#version 300 es\nvoid main(){vec2 "
                   "p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4("
                   "p*2.-1.,0,1);}";
  const char *fs =
      "#version 300 es\nprecision highp float;uniform sampler2D source;out "
      "vec4 c;void main(){c=texelFetch(source,ivec2(gl_FragCoord.xy),0);}";
  auto compile = [](GLenum type, const char *src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
      char log[2048];
      glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
      fprintf(stderr, "%s\n", log);
      std::abort();
    }
    return sh;
  };
  auto v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
  auto prog = glCreateProgram();
  glAttachShader(prog, v);
  glAttachShader(prog, f);
  glLinkProgram(prog);
  GLint ok;
  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (!ok)
    return false;
  glUseProgram(prog);
  glUniform1i(glGetUniformLocation(prog, "source"), 0);
  glViewport(0, 0, w, h);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glFinish();
  auto error = glGetError();
  printf("FP16 upload+shader_setup+copy+finish %.3f ms GL_error=%x\n",
         std::chrono::duration<double, std::milli>(Clock::now() - t).count(),
         error);
  glDeleteProgram(prog);
  glDeleteShader(v);
  glDeleteShader(f);
  glDeleteTextures(2, textures);
  glDeleteFramebuffers(1, &fb);
  glDeleteVertexArrays(1, &vao);
  destroyImage(d, im);
  eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroyContext(d, ctx);
  eglTerminate(d);
  return error == GL_NO_ERROR;
}
int main(int argc, char **argv) {
  bool show = false, param = false;
  std::string node = "/dev/dri/renderD128";
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--show"))
      show = true;
    else if (!strcmp(argv[i], "--parametric"))
      param = true;
    else if (!strcmp(argv[i], "--render-node") && i + 1 < argc)
      node = argv[++i];
    else {
      fprintf(stderr,
              "usage: %s [--show] [--parametric] [--render-node PATH]\n",
              argv[0]);
      return 2;
    }
  }
  State s;
  s.d = wl_display_connect(nullptr);
  if (!s.d)
    return 1;
  auto *registry = wl_display_get_registry(s.d);
  wl_registry_add_listener(registry, &reg, &s);
  if (wl_display_roundtrip(s.d) < 0 || wl_display_roundtrip(s.d) < 0)
    return 1;
  printf(
      "FP16 shm=%d dmabuf=%d ext_linear=%d windows_scrgb=%d\n",
      s.formats.contains(DRM_FORMAT_ABGR16161616F),
      std::any_of(s.modifiers.begin(), s.modifiers.end(),
                  [](auto v) { return v.first == DRM_FORMAT_ABGR16161616F; }),
      s.tfs.contains(5), s.features.contains(7));
  if (!show) {
    wl_display_disconnect(s.d);
    return 0;
  }
  if (!s.cm || !s.wm || !s.c || !s.intents.contains(0))
    return 3;
  wp_image_description_v1 *description = nullptr;
  if (param) {
    if (!s.features.contains(1) || !s.features.contains(4) ||
        !s.tfs.contains(5) || !s.primaries.contains(1))
      return 3;
    auto *cr = wp_color_manager_v1_create_parametric_creator(s.cm);
    wp_image_description_creator_params_v1_set_primaries_named(cr, 1);
    wp_image_description_creator_params_v1_set_tf_named(cr, 5);
    wp_image_description_creator_params_v1_set_luminances(cr, 0, 80, 80);
    description = wp_image_description_creator_params_v1_create(cr);
  } else {
    if (!s.features.contains(7))
      return 3;
    description = wp_color_manager_v1_create_windows_scrgb(s.cm);
  }
  wp_image_description_v1_add_listener(description, &desc, &s);
  if (wl_display_roundtrip(s.d) < 0 || !s.ready)
    return 4;
  constexpr int w = 512, h = 256;
  wl_buffer *buffer = nullptr;
  gbm_device *dev = nullptr;
  gbm_bo *bo = nullptr;
  int fd = -1;
  if (s.formats.contains(DRM_FORMAT_ABGR16161616F)) {
    fd = memfd_create("hdr-fp16-probe", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, w * h * 8))
      return 5;
    void *ptr =
        mmap(nullptr, w * h * 8, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED)
      return 5;
    fill(ptr, w * 8, w, h);
    munmap(ptr, w * h * 8);
    auto *pool = wl_shm_create_pool(s.shm, fd, w * h * 8);
    buffer = wl_shm_pool_create_buffer(pool, 0, w, h, w * 8,
                                       DRM_FORMAT_ABGR16161616F);
    wl_shm_pool_destroy(pool);
    puts("path=shm");
  } else {
    if (!s.dma || !s.modifiers.contains(
                      {DRM_FORMAT_ABGR16161616F, DRM_FORMAT_MOD_LINEAR})) {
      puts("no advertised linear FP16 dmabuf");
      return 5;
    }
    fd = open(node.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
      perror("render node");
      return 5;
    }
    dev = gbm_create_device(fd);
    if (!dev)
      return 5;
    bo = gbm_bo_create(dev, w, h, DRM_FORMAT_ABGR16161616F,
                       GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (!bo) {
      perror("gbm_bo_create");
      return 5;
    }
    if (!upload(dev, bo, w, h))
      return 5;
    int dmafd = gbm_bo_get_fd(bo);
    auto mod = gbm_bo_get_modifier(bo);
    if (dmafd < 0 || !s.modifiers.contains({DRM_FORMAT_ABGR16161616F, mod}))
      return 5;
    auto *params = zwp_linux_dmabuf_v1_create_params(s.dma);
    zwp_linux_buffer_params_v1_add(params, dmafd, 0, gbm_bo_get_offset(bo, 0),
                                   gbm_bo_get_stride(bo), mod >> 32,
                                   mod & 0xffffffff);
    close(dmafd);
    buffer = zwp_linux_buffer_params_v1_create_immed(
        params, w, h, DRM_FORMAT_ABGR16161616F, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    printf("path=dmabuf stride=%u modifier=%llx\n", gbm_bo_get_stride(bo),
           (unsigned long long)mod);
  }
  auto *surf = wl_compositor_create_surface(s.c);
  auto *xs = xdg_wm_base_get_xdg_surface(s.wm, surf);
  xdg_surface_add_listener(xs, &xsl, &s);
  auto *top = xdg_surface_get_toplevel(xs);
  xdg_toplevel_add_listener(top, &tl, &s);
  xdg_toplevel_set_title(top, "HyprCapture FP16 HDR feasibility (3 seconds)");
  xdg_toplevel_set_app_id(top, "hyprcapture-fp16-probe");
  auto *cs = wp_color_manager_v1_get_surface(s.cm, surf);
  wp_color_management_surface_v1_set_image_description(cs, description, 0);
  wl_surface_commit(surf);
  while (!s.configured)
    if (wl_display_dispatch(s.d) < 0)
      return 6;
  auto *cb = wl_surface_frame(surf);
  wl_callback_add_listener(cb, &fl, &s);
  if (s.pres) {
    auto *pf = wp_presentation_feedback(s.pres, surf);
    wp_presentation_feedback_add_listener(pf, &pfl, &s);
  }
  printf("ATTACH %.3f ms\n", ms());
  wl_surface_attach(surf, buffer, 0, 0);
  wl_surface_damage_buffer(surf, 0, 0, w, h);
  wl_surface_commit(surf);
  wl_display_flush(s.d);
  auto end = Clock::now() + std::chrono::seconds(3);
  while (Clock::now() < end) {
    if (wl_display_prepare_read(s.d)) {
      if (wl_display_dispatch_pending(s.d) < 0)
        break;
      continue;
    }
    wl_display_flush(s.d);
    pollfd p{wl_display_get_fd(s.d), POLLIN, 0};
    if (poll(&p, 1, 100) > 0) {
      if (wl_display_read_events(s.d) < 0)
        break;
      if (wl_display_dispatch_pending(s.d) < 0)
        break;
    } else
      wl_display_cancel_read(s.d);
  }
  int error = wl_display_get_error(s.d);
  printf(
      "RESULT ready=%d frame=%d presented=%d discarded=%d display_error=%d\n",
      s.ready, s.frame, s.presented, s.discarded, error);
  // Disconnect releases compositor ownership before GBM storage is destroyed.
  wl_display_disconnect(s.d);
  if (bo)
    gbm_bo_destroy(bo);
  if (dev)
    gbm_device_destroy(dev);
  if (fd >= 0)
    close(fd);
  return error || !s.frame || !s.presented ? 6 : 0;
}
