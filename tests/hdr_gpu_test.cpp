#include "shared/hdr_color.hpp"
#include "ui/hdr_backdrop.hpp"
#include "ui/hdr_gpu.hpp"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <QColorSpace>
#include <QGuiApplication>
// clang-format off
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
// clang-format on
#include <chrono>
#include <cmath>
#include <cstdio>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <unistd.h>
#include <vector>
#include <string_view>
using namespace hyprcapture;
static QImage fixture() {
  QImage image(96, 48, QImage::Format_RGBA16FPx4);
  image.setColorSpace(QColorSpace::SRgbLinear);
  const float values[][4] = {{0, 0, 0, 1},
                             {1, 1, 1, 1},
                             {2.5375, 2.5375, 2.5375, 1},
                             {5, 0, 0, 1},
                             {-.486375, 2.2535, -.170125, 1},
                             {12.5, 0, 0, 1},
                             {3, .3, 1.2, .5},
                             {3, .5, 1.2, 0}};
  for (int y = 0; y < 48; ++y) {
    auto *row = (uint16_t *)image.scanLine(y);
    for (int x = 0; x < 96; ++x)
      for (int c = 0; c < 4; ++c)
        row[x * 4 + c] = color::floatToHalf(values[(x / 12 + y / 12) % 8][c]);
  }
  return image;
}
int main(int argc, char **argv) {
  QGuiApplication app(argc, argv);
  if (argc > 1 && std::string_view(argv[1]) == "--benchmark") {
    auto base=fixture();auto image=base.scaled(2880,1800,Qt::IgnoreAspectRatio,Qt::FastTransformation);
    puts("trial,operation,ms");
    for(int trial=0;trial<5;++trial){
      auto begin=std::chrono::steady_clock::now();
      {auto request=ui::HdrUpload::request(image,252);if(!request->waitUntil(begin+std::chrono::seconds(5))||request->state()!=ui::HdrUpload::State::Ready)return 1;}
      printf("%d,fp16_complete,%g\n",trial,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count());
      begin=std::chrono::steady_clock::now();
      auto pq=ui::hdrToPq10(image,nullptr,252);if(pq.isNull())return 1;
      printf("%d,cpu_pq,%g\n",trial,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count());
    }
    return 0;
  }

  int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
  if (fd < 0)
    return 77;
  auto *gbm = gbm_create_device(fd);
  if (!gbm)
    return 77;
  auto platform = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress(
      "eglGetPlatformDisplayEXT");
  auto create =
      (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
  auto destroy =
      (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
  auto target = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress(
      "glEGLImageTargetTexture2DOES");
  if (!platform || !create || !destroy || !target)
    return 77;
  auto display = platform(EGL_PLATFORM_GBM_KHR, gbm, nullptr);
  if (!eglInitialize(display, nullptr, nullptr))
    return 77;
  eglBindAPI(EGL_OPENGL_ES_API);
  EGLConfig config;
  EGLint n;
  const EGLint attrs[]{EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
  if (!eglChooseConfig(display, attrs, &config, 1, &n) || !n)
    return 77;
  const EGLint ctxAttrs[]{EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  auto ctx = eglCreateContext(display, config, EGL_NO_CONTEXT, ctxAttrs);
  if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
    return 77;
  auto original = fixture();
  for (float reference : {80.F, 203.F, 252.F})
    for (bool premultiplied : {false, true}) {
      auto image = premultiplied ? original.convertToFormat(
                                       QImage::Format_RGBA16FPx4_Premultiplied)
                                 : original;
      float peak;
      auto pq = ui::hdrToPq10(image, &peak, reference);
      auto upload = ui::HdrUpload::request(image, reference);
      // Editing/dropping the caller's pixels after submission must not mutate
      // upload.
      image.fill(Qt::black);
      if (!upload->waitUntil(std::chrono::steady_clock::now() +
                             std::chrono::seconds(5)) ||
          upload->state() != ui::HdrUpload::State::Ready) {
        fprintf(stderr, "GPU upload failed\n");
        return 1;
      }
      auto frame = upload->result();
      if (std::abs(frame->peakNits - peak) > .1F) {
        fprintf(stderr, "peak mismatch %f vs %f\n", frame->peakNits, peak);
        return 1;
      }
      const EGLint a[]{EGL_WIDTH,
                       frame->width,
                       EGL_HEIGHT,
                       frame->height,
                       EGL_LINUX_DRM_FOURCC_EXT,
                       DRM_FORMAT_ABGR16161616F,
                       EGL_DMA_BUF_PLANE0_FD_EXT,
                       frame->fd,
                       EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                       frame->offset,
                       EGL_DMA_BUF_PLANE0_PITCH_EXT,
                       frame->stride,
                       EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
                       (EGLint)frame->modifier,
                       EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
                       (EGLint)(frame->modifier >> 32),
                       EGL_NONE};
      auto eglImage =
          create(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, a);
      if (eglImage == EGL_NO_IMAGE_KHR)
        return 1;
      GLuint texture, fb;
      glGenTextures(1, &texture);
      glBindTexture(GL_TEXTURE_2D, texture);
      target(GL_TEXTURE_2D, eglImage);
      glGenFramebuffers(1, &fb);
      glBindFramebuffer(GL_FRAMEBUFFER, fb);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, texture, 0);
      if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return 1;
      std::vector<uint16_t> out(size_t(frame->width) * frame->height * 4);
      glReadPixels(0, 0, frame->width, frame->height, GL_RGBA, GL_HALF_FLOAT,
                   out.data());
      if (glGetError() != GL_NO_ERROR)
        return 1;
      int worst = 0;
      for (int y = 0; y < frame->height; ++y)
        for (int x = 0; x < frame->width; ++x) {
          auto *v = &out[(size_t(y) * frame->width + x) * 4];
          if (v[3] != color::floatToHalf(1))
            return 1;
          std::array<float, 3> rgb{color::halfToFloat(v[0]),
                                   color::halfToFloat(v[1]),
                                   color::halfToFloat(v[2])};
          auto code = ((const uint32_t *)pq.constScanLine(y))[x];
          for (int c = 0; c < 3; ++c) {
            int expected = (code >> (20 - c * 10)) & 1023;
            int actual = std::lround(color::nitsToPq(rgb[c] * 80) * 1023);
            if (std::abs(actual-expected)>worst) {
              worst=std::abs(actual-expected);
              if(worst>2)fprintf(stderr,"worst x=%d y=%d c=%d expected=%d actual=%d outRGB=%g,%g,%g\n",x,y,c,expected,actual,rgb[0],rgb[1],rgb[2]);
            }
          }
        }
      printf(
          "reference=%g premultiplied=%d PQ_max_code_delta=%d peak_delta=%g\n",
          reference, premultiplied, worst, frame->peakNits - peak);
      if (worst > 2)
        return 1;
      glDeleteFramebuffers(1, &fb);
      glDeleteTextures(1, &texture);
      destroy(display, eglImage);
    }
  // Cancellation does not wait for driver execution; only weak queued jobs
  // remain.
  auto image = original.scaled(2880, 1800);
  auto start = std::chrono::steady_clock::now();
  {
    auto cancelled = ui::HdrUpload::request(image, 203);
  }
  if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(100))
    return 1;
  eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroyContext(display, ctx);
  eglTerminate(display);
  gbm_device_destroy(gbm);
  close(fd);
  return 0;
}
