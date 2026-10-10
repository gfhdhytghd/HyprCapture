#include "shared/sdr_resolve_shader.hpp"
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>

int main() {
    const auto display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(display, nullptr, nullptr)) return 77;
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint attrs[]{EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config; EGLint count = 0;
    assert(eglChooseConfig(display, attrs, &config, 1, &count) && count);
    const EGLint surfaceAttrs[]{EGL_WIDTH, 8, EGL_HEIGHT, 1, EGL_NONE};
    const EGLint contextAttrs[]{EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    const auto surface = eglCreatePbufferSurface(display, config, surfaceAttrs);
    const auto context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttrs);
    assert(eglMakeCurrent(display, surface, surface, context));
    const auto compile = [](GLenum type, const char* source) {
        auto shader = glCreateShader(type); glShaderSource(shader, 1, &source, nullptr); glCompileShader(shader);
        GLint ok; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) { char log[2048]; glGetShaderInfoLog(shader, sizeof(log), nullptr, log); fprintf(stderr, "%s\n", log); std::abort(); }
        return shader;
    };
    auto vs = compile(GL_VERTEX_SHADER, hyprcapture::color::sdrResolveVertex);
    auto fs = compile(GL_FRAGMENT_SHADER, hyprcapture::color::sdrResolveFragment);
    auto program = glCreateProgram(); glAttachShader(program, vs); glAttachShader(program, fs); glLinkProgram(program);
    GLint linked; glGetProgramiv(program, GL_LINK_STATUS, &linked); assert(linked);
    GLuint textures[2], framebuffer, vao;
    glGenTextures(2, textures); glGenFramebuffers(1, &framebuffer); glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    float input[32]{};
    const float values[]{0.0025F, 0.1132202148F, 0.4685058594F, 1.140625F, 2.162109375F, 12.5F, 50.0F, 0.2342529297F};
    for (int i = 0; i < 8; ++i) { for (int c = 0; c < 3; ++c) input[i * 4 + c] = values[i]; input[i * 4 + 3] = i == 7 ? 0.5F : 1; }
    glBindTexture(GL_TEXTURE_2D, textures[0]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, 8, 1, 0, GL_RGBA, GL_FLOAT, input);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, textures[1]); glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 8, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[1], 0);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    glUseProgram(program); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, textures[0]);
    glUniform1i(glGetUniformLocation(program, "sourceImage"), 0); glUniform1i(glGetUniformLocation(program, "sourceTF"), 5);
    glUniform1f(glGetUniformLocation(program, "whiteScale"), 80.0F / 172.8F); glUniform1f(glGetUniformLocation(program, "blackLevel"), .0025F);
    glUniform1f(glGetUniformLocation(program, "shoulderKnee"), .98F);
    const float identity[]{1,0,0,0,1,0,0,0,1}; glUniformMatrix3fv(glGetUniformLocation(program, "primaries"), 1, GL_FALSE, identity);
    glViewport(0, 0, 8, 1); glDrawArrays(GL_TRIANGLES, 0, 3);
    unsigned char output[32]; glReadPixels(0, 0, 8, 1, GL_RGBA, GL_UNSIGNED_BYTE, output); assert(glGetError() == GL_NO_ERROR);
    const int expected[]{0,64,128,192,254,255,255,64};
    for (int i = 0; i < 8; ++i) {
        for (int c = 0; c < 3; ++c) { if (std::abs(int(output[i * 4 + c]) - expected[i]) > 1) { fprintf(stderr, "pixel %d channel %d: %d expected %d\n", i,c,output[i*4+c],expected[i]); return 1; } }
        assert(std::abs(int(output[i * 4 + 3]) - (i == 7 ? 128 : 255)) <= 1);
    }
    puts("GPU HDR to SDR: current live 173-nit gray patches, black, white, highlights and premultiplied alpha verified");
    glDeleteTextures(2, textures); glDeleteFramebuffers(1, &framebuffer); glDeleteVertexArrays(1, &vao);
    glDeleteProgram(program); glDeleteShader(vs); glDeleteShader(fs);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); eglDestroyContext(display, context); eglDestroySurface(display, surface); eglTerminate(display);
}
