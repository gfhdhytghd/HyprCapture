#include "ui/hdr_gpu.hpp"
#include "shared/hdr_linear_shader.hpp"
#include "ui/hdr_image.hpp"
#include "ui/timing.hpp"
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <vector>
#ifdef HYPRCAPTURE_HDR_DMABUF
#include <EGL/egl.h>
#include <EGL/eglext.h>
// clang-format off
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
// clang-format on
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#endif
namespace hyprcapture::ui {
HdrGpuFrame::~HdrGpuFrame() {
  if (fd >= 0)
    close(fd);
}
struct HdrUpload::Impl {
  mutable std::mutex mutex;
  mutable std::condition_variable changed;
  State state = State::Pending;
  std::shared_ptr<const HdrGpuFrame> frame;
  QImage image;
  float reference = 203;
};
HdrUpload::HdrUpload(std::shared_ptr<Impl> s) : m(std::move(s)) {}
HdrUpload::State HdrUpload::state() const {
  std::lock_guard l(m->mutex);
  return m->state;
}
std::shared_ptr<const HdrGpuFrame> HdrUpload::result() const {
  std::lock_guard l(m->mutex);
  return m->frame;
}
bool HdrUpload::waitUntil(
    std::chrono::steady_clock::time_point deadline) const {
  std::unique_lock l(m->mutex);
  return m->changed.wait_until(l, deadline,
                               [&] { return m->state != State::Pending; });
}
#ifdef HYPRCAPTURE_HDR_DMABUF
namespace {
struct Slot {
  gbm_bo *bo = nullptr;
  EGLImageKHR image = EGL_NO_IMAGE_KHR;
  GLuint texture = 0;
  int width = 0, height = 0;
};
// A single process-lifetime executor owns EGL. Closing an overlay never joins
// this thread or waits for driver initialization. Its queue holds weak jobs;
// cancelled requests are skipped, and a rendered result owns only frozen
// pixels.
class Renderer {
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<std::function<void()>> queue;
  EGLDisplay display = EGL_NO_DISPLAY;
  EGLContext context = EGL_NO_CONTEXT;
  gbm_device *device = nullptr;
  int node = -1;
  bool available = false;
  PFNEGLCREATEIMAGEKHRPROC createImage = nullptr;
  PFNEGLDESTROYIMAGEKHRPROC destroyImage = nullptr;
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC imageTarget = nullptr;
  GLuint linear = 0, reduce = 0, vao = 0;
  std::vector<std::unique_ptr<Slot>> pool;
  GLuint shader(GLenum type, const char *source) {
    auto s = glCreateShader(type);
    glShaderSource(s, 1, &source, nullptr);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
      glDeleteShader(s);
      return 0;
    }
    return s;
  }
  GLuint program(const char *fragment) {
    auto v = shader(GL_VERTEX_SHADER, color::hdrLinearVertex),
         f = shader(GL_FRAGMENT_SHADER, fragment);
    auto p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    glDeleteShader(v);
    glDeleteShader(f);
    if (!ok) {
      glDeleteProgram(p);
      return 0;
    }
    return p;
  }
  void destroy(std::unique_ptr<Slot> s) {
    if (s->texture)
      glDeleteTextures(1, &s->texture);
    if (s->image != EGL_NO_IMAGE_KHR)
      destroyImage(display, s->image);
    if (s->bo)
      gbm_bo_destroy(s->bo);
  }
  void initialize() {
    ScopedUiTiming timing("hdr.gpu.initialize");
    node = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (node < 0)
      return;
    device = gbm_create_device(node);
    if (!device)
      return;
    auto platform = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress(
        "eglGetPlatformDisplayEXT");
    createImage =
        (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    destroyImage =
        (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    imageTarget = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress(
        "glEGLImageTargetTexture2DOES");
    if (!platform || !createImage || !destroyImage || !imageTarget)
      return;
    display = platform(EGL_PLATFORM_GBM_KHR, device, nullptr);
    if (!eglInitialize(display, nullptr, nullptr))
      return;
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLConfig config;
    EGLint count;
    const EGLint attributes[]{EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                              EGL_NONE};
    if (!eglChooseConfig(display, attributes, &config, 1, &count) || !count)
      return;
    const EGLint attrs[]{EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, attrs);
    if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context))
      return;
    linear = program(color::hdrLinearFragment);
    reduce = program(color::hdrPeakReduceFragment);
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    available = linear && reduce;
  }
  std::unique_ptr<Slot> slot(int w, int h) {
    for (auto i = pool.begin(); i != pool.end(); ++i)
      if ((*i)->width == w && (*i)->height == h) {
        auto s = std::move(*i);
        pool.erase(i);
        return s;
      }
    auto s = std::make_unique<Slot>();
    s->width = w;
    s->height = h;
    s->bo = gbm_bo_create(device, w, h, DRM_FORMAT_ABGR16161616F,
                          GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (!s->bo)
      return {};
    int fd = gbm_bo_get_fd(s->bo);
    if (fd < 0) {
      destroy(std::move(s));
      return {};
    }
    const auto modifier = gbm_bo_get_modifier(s->bo);
    EGLint a[]{EGL_WIDTH,
               w,
               EGL_HEIGHT,
               h,
               EGL_LINUX_DRM_FOURCC_EXT,
               DRM_FORMAT_ABGR16161616F,
               EGL_DMA_BUF_PLANE0_FD_EXT,
               fd,
               EGL_DMA_BUF_PLANE0_OFFSET_EXT,
               (EGLint)gbm_bo_get_offset(s->bo, 0),
               EGL_DMA_BUF_PLANE0_PITCH_EXT,
               (EGLint)gbm_bo_get_stride(s->bo),
               EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
               (EGLint)modifier,
               EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
               (EGLint)(modifier >> 32),
               EGL_NONE};
    s->image =
        createImage(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, a);
    close(fd);
    if (s->image == EGL_NO_IMAGE_KHR) {
      destroy(std::move(s));
      return {};
    }
    glGenTextures(1, &s->texture);
    glBindTexture(GL_TEXTURE_2D, s->texture);
    imageTarget(GL_TEXTURE_2D, s->image);
    return s;
  }
  std::shared_ptr<const HdrGpuFrame> render(const QImage &image,
                                            float reference) {
    ScopedUiTiming timing("hdr.gpu.upload");
    if (!available || !isHdrImage(image))
      return {};
    GLint maxSize;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
    int w = image.width(), h = image.height();
    if (w > maxSize || h > maxSize || qint64(w) * h > 64LL * 1024 * 1024)
      return {};
    while (glGetError() != GL_NO_ERROR) {
    }
    auto target = slot(w, h);
    if (!target)
      return {};
    GLuint input, fb;
    glGenTextures(1, &input);
    glBindTexture(GL_TEXTURE_2D, input);
    // Only the non-FP16 input needs a conversion. Premultiplied FP16 goes to
    // the shader unchanged; the shader explicitly restores straight channels.
    QImage src = image;
    if (src.format() != QImage::Format_RGBA16FPx4 &&
        src.format() != QImage::Format_RGBA16FPx4_Premultiplied)
      src = src.convertToFormat(QImage::Format_RGBA16FPx4);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, src.bytesPerLine() / 8);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT,
                 src.constBits());
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    std::vector<GLuint> peaks;
    std::vector<QSize> sizes;
    auto texture = [&](int width, int height) {
      GLuint t;
      glGenTextures(1, &t);
      glBindTexture(GL_TEXTURE_2D, t);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGBA,
                   GL_FLOAT, nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      peaks.push_back(t);
      sizes.push_back({width, height});
      return t;
    };
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           target->texture, 0);
    bool ok =
        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (ok) {
      glUseProgram(linear);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, input);
      glUniform1i(glGetUniformLocation(linear, "source"), 0);
      glUniform1f(glGetUniformLocation(linear, "referenceNits"), reference);
      glUniform1i(glGetUniformLocation(linear, "premultiplied"),
                  src.format() == QImage::Format_RGBA16FPx4_Premultiplied);
      glViewport(0, 0, w, h);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      bool firstPass = true;
      do {
        GLuint prev = firstPass ? input : peaks.back();
        QSize prevSize(w, h);
        w = (w + 3) / 4;
        h = (h + 3) / 4;
        auto t = texture(w, h);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, t, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
            GL_FRAMEBUFFER_COMPLETE) {
          ok = false;
          break;
        }
        glUseProgram(reduce);
        glBindTexture(GL_TEXTURE_2D, prev);
        glUniform1i(glGetUniformLocation(reduce, "source"), 0);
        glUniform1i(glGetUniformLocation(reduce, "firstPass"), firstPass);
        glUniform1f(glGetUniformLocation(reduce, "referenceNits"), reference);
        glUniform1i(glGetUniformLocation(reduce, "premultiplied"),
                    src.format() == QImage::Format_RGBA16FPx4_Premultiplied);
        glUniform2i(glGetUniformLocation(reduce, "sourceSize"),
                    prevSize.width(), prevSize.height());
        glViewport(0, 0, w, h);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        firstPass = false;
      } while (w > 1 || h > 1);
    }
    auto fence = ok ? glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0) : nullptr;
    glFlush();
    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    bool done = false;
    while (fence && std::chrono::steady_clock::now() < deadline) {
      auto status =
          glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000);
      if (status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED) {
        done = true;
        break;
      }
      if (status == GL_WAIT_FAILED)
        break;
    }
    float peak[4]{};
    if (done)
      glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, peak);
    ok = done && glGetError() == GL_NO_ERROR;
    if (fence)
      glDeleteSync(fence);
    glDeleteTextures(1, &input);
    glDeleteTextures(peaks.size(), peaks.data());
    glDeleteFramebuffers(1, &fb);
    if (!ok) {
      destroy(std::move(target));
      return {};
    }
    auto frame = std::make_shared<HdrGpuFrame>();
    frame->fd = gbm_bo_get_fd(target->bo);
    if (frame->fd < 0) {
      destroy(std::move(target));
      return {};
    }
    frame->width = target->width;
    frame->height = target->height;
    frame->stride = gbm_bo_get_stride(target->bo);
    frame->offset = gbm_bo_get_offset(target->bo, 0);
    frame->modifier = gbm_bo_get_modifier(target->bo);
    frame->peakNits = peak[0] * 80;
    frame->referenceNits = reference;
    frame->storage = std::shared_ptr<void>(target.release(), [this](void *p) {
      post([this, p] {
        std::unique_ptr<Slot> s(static_cast<Slot *>(p));
        size_t bytes = size_t(s->width) * s->height * 8;
        size_t total = bytes;
        for (auto &i : pool)
          total += size_t(i->width) * i->height * 8;
        if (pool.size() < 4 && total <= 256ULL * 1024 * 1024)
          pool.push_back(std::move(s));
        else
          destroy(std::move(s));
      });
    });
    return frame;
  }

public:
  Renderer() {
    std::thread([this] {
      try {
        initialize();
      } catch (...) {
      }
      for (;;) {
        std::function<void()> work;
        {
          std::unique_lock l(mutex);
          changed.wait(l, [&] { return !queue.empty(); });
          work = std::move(queue.front());
          queue.pop_front();
        }
        try {
          work();
        } catch (...) {
        }
      }
    }).detach();
  }
  void post(std::function<void()> f) {
    {
      std::lock_guard l(mutex);
      queue.push_back(std::move(f));
    }
    changed.notify_one();
  }
  void submit(std::weak_ptr<HdrUpload::Impl> weak) {
    post([this, weak] {
      if (auto s = weak.lock()) {
        std::shared_ptr<const HdrGpuFrame> frame;
        try {
          frame = render(s->image, s->reference);
        } catch (...) {
        }
        std::lock_guard l(s->mutex);
        s->image = {};
        s->frame = std::move(frame);
        s->state =
            s->frame ? HdrUpload::State::Ready : HdrUpload::State::Failed;
        s->changed.notify_all();
      }
    });
  }
};
Renderer &renderer() {
  static auto *r = new Renderer;
  return *r;
}
} // namespace
#endif
void HdrUpload::prewarm() {
#ifdef HYPRCAPTURE_HDR_DMABUF
  if (qEnvironmentVariable("HYPRCAPTURE_HDR_DMABUF") != "0")
    (void)renderer();
#endif
}
std::shared_ptr<HdrUpload> HdrUpload::request(const QImage &image,
                                              float reference) {
  auto state = std::make_shared<Impl>();
  auto result = std::shared_ptr<HdrUpload>(new HdrUpload(state));
  state->image = image;
  state->reference =
      std::isfinite(reference) && reference > 0 ? reference : 203;
#ifdef HYPRCAPTURE_HDR_DMABUF
  if (isHdrImage(image) &&
      qEnvironmentVariable("HYPRCAPTURE_HDR_DMABUF") != "0") {
    renderer().submit(state);
    return result;
  }
#endif
  state->image = {};
  state->state = State::Failed;
  return result;
}
#ifdef HYPRCAPTURE_HDR_TESTING
std::shared_ptr<HdrUpload> HdrUpload::pendingForTest(const QImage &image) {
  auto s = std::make_shared<Impl>();
  s->image = image;
  return std::shared_ptr<HdrUpload>(new HdrUpload(s));
}
void HdrUpload::failForTest() {
  std::lock_guard l(m->mutex);
  m->image = {};
  m->state = State::Failed;
  m->changed.notify_all();
}
#endif

} // namespace hyprcapture::ui
