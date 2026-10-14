// Offscreen EGL/GLES timings, glFinish at each boundary. No compositor/config
// mutation.
#include "shared/hdr_color.hpp"
#include "shared/sdr_resolve_shader.hpp"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using Clock = std::chrono::steady_clock;
using namespace hyprcapture;
static void check() {
  auto e = glGetError();
  if (e) {
    fprintf(stderr, "GL error %x\n", e);
    std::exit(1);
  }
}
static GLuint compile(GLenum type, const char *s) {
  auto sh = glCreateShader(type);
  glShaderSource(sh, 1, &s, nullptr);
  glCompileShader(sh);
  GLint ok;
  glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048];
    glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
    fprintf(stderr, "%s\n", log);
    std::exit(1);
  }
  return sh;
}
static GLuint program(const char *fs) {
  auto v = compile(GL_VERTEX_SHADER, color::sdrResolveVertex),
       f = compile(GL_FRAGMENT_SHADER, fs);
  auto p = glCreateProgram();
  glAttachShader(p, v);
  glAttachShader(p, f);
  glLinkProgram(p);
  GLint ok;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok)
    std::exit(1);
  glDeleteShader(v);
  glDeleteShader(f);
  return p;
}
static constexpr const char *copy = R"(#version 300 es
precision highp float;uniform sampler2D source;out vec4 c;void main(){c=texelFetch(source,ivec2(gl_FragCoord.xy),0);})";
static constexpr const char *pq = R"(#version 300 es
precision highp float;uniform sampler2D source;out vec4 c;
void main(){ivec2 p=ivec2(gl_FragCoord.xy);vec4 v=texelFetch(source,p,0);
 vec3 rgb=mat3(.6274040,.0690970,.0163916,.3292820,.9195400,.0880132,.0433136,.0113612,.8955950)*v.rgb;
 float s=((p.x/12+p.y/12)%2)==0?72./255.:94./255.;float checker=pow((s+.055)/1.055,2.4)*203./80.;
 rgb=mix(vec3(checker),rgb,clamp(v.a,0.,1.));rgb=clamp(rgb,0.,125.)*.008;
 vec3 t=pow(rgb,vec3(2610./16384.));vec3 n=pow((vec3(3424./4096.)+(2413./128.)*t)/(vec3(1.)+(2392./128.)*t),vec3(2523./32.));
 // GL RGBA 2_10_10_10_REV memory is low-bit R: swap to CPU RGB30 layout.
 c=vec4(n.bgr,1.);})";
template <class F>
static void measure(const char *event, int w, int h, int trial, F f) {
  auto t = Clock::now();
  f();
  glFinish();
  check();
  printf("%dx%d,%d,%s,%.3f\n", w, h, trial, event,
         std::chrono::duration<double, std::milli>(Clock::now() - t).count());
  fflush(stdout);
}
int main() {
  auto platform = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress(
      "eglGetPlatformDisplayEXT");
  if (!platform)
    return 77;
  auto d =
      platform(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
  if (!eglInitialize(d, nullptr, nullptr))
    return 77;
  eglBindAPI(EGL_OPENGL_ES_API);
  EGLConfig cfg;
  EGLint count;
  const EGLint a[]{EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                   EGL_OPENGL_ES3_BIT, EGL_NONE};
  if (!eglChooseConfig(d, a, &cfg, 1, &count) || !count)
    return 1;
  const EGLint ca[]{EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  auto ctx = eglCreateContext(d, cfg, EGL_NO_CONTEXT, ca);
  if (!eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
    return 1;
  fprintf(stderr, "renderer=%s version=%s\n", glGetString(GL_RENDERER),
          glGetString(GL_VERSION));
  auto copyProgram = program(copy), pqProgram = program(pq),
       sdrProgram = program(color::sdrResolveFragment);
  GLuint vao;
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  puts("size,trial,event,ms");
  for (auto wh : {std::pair{2880, 1800}, std::pair{6144, 3456}}) {
    auto [w, h] = wh;
    size_t pixels = size_t(w) * h;
    std::vector<uint16_t> input(pixels * 4);
    const float patches[][4] = {
        {0, 0, 0, 1}, {1, 1, 1, 1},         {2.5375, 2.5375, 2.5375, 1},
        {5, 0, 0, 1}, {-.5, 2.25, -.25, 1}, {3, .3, 1.2, .5}};
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
        for (int c = 0; c < 4; ++c)
          input[(size_t(y) * w + x) * 4 + c] =
              color::floatToHalf(patches[std::min(5, x * 6 / w)][c]);
    GLuint tex[4], fbo[3], pbo;
    glGenTextures(4, tex);
    glGenFramebuffers(3, fbo);
    glGenBuffers(1, &pbo);
    measure("texture_storage_allocate", w, h, -1, [&] {
      for (int i = 0; i < 4; ++i) {
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glTexStorage2D(GL_TEXTURE_2D, 1,
                       i < 2    ? GL_RGBA16F
                       : i == 2 ? GL_RGB10_A2
                                : GL_RGBA8,
                       w, h);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      }
      for (int i = 0; i < 3; ++i) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, tex[i + 1], 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
          std::exit(1);
      }
    });
    auto draw = [&](GLuint p, int fb) {
      glBindFramebuffer(GL_FRAMEBUFFER, fbo[fb]);
      glUseProgram(p);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, tex[0]);
      glUniform1i(glGetUniformLocation(p, "source"), 0);
      glViewport(0, 0, w, h);
      glDrawArrays(GL_TRIANGLES, 0, 3);
    };
    for (int trial = 0; trial < 5; ++trial) {
      measure("FP16_upload", w, h, trial, [&] {
        glBindTexture(GL_TEXTURE_2D, tex[0]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_HALF_FLOAT,
                        input.data());
      });
      measure("FP16_copy_draw", w, h, trial, [&] { draw(copyProgram, 0); });
      measure("PQ_draw", w, h, trial, [&] { draw(pqProgram, 1); });
      if (trial == 0) {
        std::vector<uint32_t> codes(pixels);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[1]);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV,
                     codes.data());
        check();
        unsigned maxDelta = 0;
        for (int j = 0; j < 6; ++j) {
          int x = (j * w) / 6 + w / 12, y = 100;
          auto *v = &input[(size_t(y) * w + x) * 4];
          auto rgb = color::scRgbToBt2020({color::halfToFloat(v[0]),
                                           color::halfToFloat(v[1]),
                                           color::halfToFloat(v[2])});
          float alpha = color::halfToFloat(v[3]);
          float checker =
              color::srgbToLinear(((x / 12 + y / 12) & 1) ? 94.F / 255
                                                          : 72.F / 255) *
              203 / 80;
          auto code = codes[size_t(y) * w + x];
          for (int c = 0; c < 3; ++c) {
            float value =
                std::clamp(rgb[c] * alpha + checker * (1 - alpha), 0.F, 125.F);
            value = color::halfToFloat(color::floatToHalf(value));
            unsigned expected = std::lround(color::nitsToPq(value * 80) * 1023);
            unsigned actual = (code >> (20 - c * 10)) & 1023;
            maxDelta = std::max(
                maxDelta, unsigned(std::abs(int(actual) - int(expected))));
          }
        }
        fprintf(stderr, "%dx%d PQ six patch RGB max_code_delta=%u\n", w, h,
                maxDelta);
        if (maxDelta > 2)
          return 2;
        // Verify direct copy preserves negative and >1 FP16 values.
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[0]);
        std::vector<uint16_t> got(pixels * 4);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_HALF_FLOAT, got.data());
        check();
        if (got != input) {
          fprintf(stderr, "FP16 copy mismatch\n");
          return 2;
        }
        fprintf(stderr, "%dx%d FP16 copy exact (negative, >1 and alpha)\n", w,
                h);
      }
      // Match the product's SDR resolve configuration, which is already on the
      // GPU.
      glUseProgram(sdrProgram);
      glUniform1i(glGetUniformLocation(sdrProgram, "sourceImage"), 0);
      glUniform1i(glGetUniformLocation(sdrProgram, "sourceTF"), 5);
      glUniform1f(glGetUniformLocation(sdrProgram, "whiteScale"), 80.F / 203);
      glUniform1f(glGetUniformLocation(sdrProgram, "blackLevel"), 0);
      glUniform1f(glGetUniformLocation(sdrProgram, "shoulderKnee"), .98F);
      const float id[]{1, 0, 0, 0, 1, 0, 0, 0, 1};
      glUniformMatrix3fv(glGetUniformLocation(sdrProgram, "primaries"), 1,
                         GL_FALSE, id);
      measure("SDR_resolve_draw", w, h, trial, [&] { draw(sdrProgram, 2); });
      for (bool hdr : {false, true}) {
        size_t bytes = pixels * (hdr ? 12 : 4);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
        measure(hdr ? "HDR_PBO_allocate" : "SDR_PBO_allocate", w, h, trial,
                [&] {
                  glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr,
                               GL_STREAM_READ);
                });
        measure(hdr ? "HDR_PBO_read" : "SDR_PBO_read", w, h, trial, [&] {
          glBindFramebuffer(GL_FRAMEBUFFER, fbo[2]);
          glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
          if (hdr) {
            glBindFramebuffer(GL_FRAMEBUFFER, fbo[0]);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_HALF_FLOAT,
                         (void *)(pixels * 4));
          }
        });
        std::vector<unsigned char> output(bytes);
        measure(hdr ? "HDR_PBO_map_copy" : "SDR_PBO_map_copy", w, h, trial,
                [&] {
                  auto *p = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, bytes,
                                             GL_MAP_READ_BIT);
                  if (!p)
                    std::exit(1);
                  memcpy(output.data(), p, bytes);
                  glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
                });
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
      }
      std::vector<uint32_t> read(pixels);
      measure("PQ_readback", w, h, trial, [&] {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[1]);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV,
                     read.data());
      });
    }
    glDeleteTextures(4, tex);
    glDeleteFramebuffers(3, fbo);
    glDeleteBuffers(1, &pbo);
  }
  glDeleteProgram(copyProgram);
  glDeleteProgram(pqProgram);
  glDeleteProgram(sdrProgram);
  glDeleteVertexArrays(1, &vao);
  eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroyContext(d, ctx);
  eglTerminate(d);
}
