#include "plugin/window_gpu_export.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>

#include <array>
#include <algorithm>
#include <fcntl.h>
#include <filesystem>
#include <unistd.h>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <vector>

using namespace std::chrono_literals;
using hyprcapture::WindowGpuPacket;
using hyprcapture::gpuwire::Frame;

namespace {
const char* eglError() {
    switch (eglGetError()) {
        case EGL_SUCCESS: return "EGL_SUCCESS";
        case EGL_NOT_INITIALIZED: return "EGL_NOT_INITIALIZED";
        case EGL_BAD_ACCESS: return "EGL_BAD_ACCESS";
        case EGL_BAD_ALLOC: return "EGL_BAD_ALLOC";
        case EGL_BAD_ATTRIBUTE: return "EGL_BAD_ATTRIBUTE";
        case EGL_BAD_CONTEXT: return "EGL_BAD_CONTEXT";
        case EGL_BAD_CONFIG: return "EGL_BAD_CONFIG";
        case EGL_BAD_CURRENT_SURFACE: return "EGL_BAD_CURRENT_SURFACE";
        case EGL_BAD_DISPLAY: return "EGL_BAD_DISPLAY";
        case EGL_BAD_MATCH: return "EGL_BAD_MATCH";
        case EGL_BAD_NATIVE_PIXMAP: return "EGL_BAD_NATIVE_PIXMAP";
        case EGL_BAD_NATIVE_WINDOW: return "EGL_BAD_NATIVE_WINDOW";
        case EGL_BAD_PARAMETER: return "EGL_BAD_PARAMETER";
        case EGL_BAD_SURFACE: return "EGL_BAD_SURFACE";
        default: return "EGL_UNKNOWN";
    }
}

class EglScope {
  public:
    ~EglScope() {
        if (display_ != EGL_NO_DISPLAY) {
            eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (otherContext_ != EGL_NO_CONTEXT) eglDestroyContext(display_, otherContext_);
            if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);
            if (surface_ != EGL_NO_SURFACE) eglDestroySurface(display_, surface_);
            eglTerminate(display_);
        }
    }
    bool create() {
        display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (display_ == EGL_NO_DISPLAY || eglInitialize(display_, &major_, &minor_) != EGL_TRUE) {
            auto query = reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(eglGetProcAddress("eglQueryDevicesEXT"));
            auto get = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
            if (!query || !get) return false;
            std::array<EGLDeviceEXT, 8> devices{}; EGLint count = 0;
            if (query(static_cast<EGLint>(devices.size()), devices.data(), &count) != EGL_TRUE) return false;
            display_ = EGL_NO_DISPLAY;
            for (EGLint i = 0; i < count && display_ == EGL_NO_DISPLAY; ++i)
                display_ = get(EGL_PLATFORM_DEVICE_EXT, devices[static_cast<size_t>(i)], nullptr);
            if (display_ == EGL_NO_DISPLAY || eglInitialize(display_, &major_, &minor_) != EGL_TRUE) return false;
        }
        if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) return false;
        constexpr EGLint configAttrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
            EGL_OPENGL_ES3_BIT_KHR, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8, EGL_NONE};
        EGLConfig config = nullptr; EGLint count = 0;
        if (eglChooseConfig(display_, configAttrs, &config, 1, &count) != EGL_TRUE || count != 1) return false;
        config_ = config;
        constexpr EGLint surfaceAttrs[] = {EGL_WIDTH, 2, EGL_HEIGHT, 2, EGL_NONE};
        surface_ = eglCreatePbufferSurface(display_, config, surfaceAttrs);
        constexpr EGLint contextAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        context_ = eglCreateContext(display_, config, EGL_NO_CONTEXT, contextAttrs);
        return surface_ != EGL_NO_SURFACE && context_ != EGL_NO_CONTEXT &&
               eglMakeCurrent(display_, surface_, surface_, context_) == EGL_TRUE;
    }
    EGLDisplay display() const { return display_; }
    bool switchContext() {
        constexpr EGLint attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        otherContext_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, attrs);
        return otherContext_ != EGL_NO_CONTEXT && eglMakeCurrent(display_, surface_, surface_, otherContext_) == EGL_TRUE;
    }
  private:
    EGLConfig config_ = nullptr;
    EGLContext otherContext_ = EGL_NO_CONTEXT;
    EGLDisplay display_ = EGL_NO_DISPLAY; EGLSurface surface_ = EGL_NO_SURFACE; EGLContext context_ = EGL_NO_CONTEXT;
    EGLint major_ = 0, minor_ = 0;
};

bool hasExtension(EGLDisplay display, const char* name) {
    const char* raw = eglQueryString(display, EGL_EXTENSIONS);
    return raw && std::strstr(raw, name);
}

Frame validMetadata() {
    Frame f{}; f.sequence = 1; f.captureMonotonicNs = 1001; f.geometryEpoch = 7;
    f.logicalWidth = 256; f.logicalHeight = 256; f.imageWidth = 256; f.imageHeight = 256;
    f.cropWidth = 256; f.cropHeight = 256; f.shadowEnabled = false; return f;
}
}

int main() {
    EglScope egl;
    if (!egl.create()) { std::fprintf(stderr, "FAIL EGL init (%s)\n", eglError()); return 1; }
    if (!hasExtension(egl.display(), "EGL_KHR_image_base") ||
        !hasExtension(egl.display(), "EGL_MESA_image_dma_buf_export") ||
        !hasExtension(egl.display(), "EGL_ANDROID_native_fence_sync") ||
        !hasExtension(egl.display(), "EGL_EXT_image_dma_buf_import")) {
        std::fprintf(stderr, "FAIL required EGL export/import/fence extensions unavailable\n"); return 1;
    }

    constexpr int width = 256, height = 256;
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        auto* p = pixels.data() + (static_cast<size_t>(y) * width + x) * 4;
        p[0] = static_cast<unsigned char>((x * 13 + y * 3) & 255);
        p[1] = static_cast<unsigned char>((x * 5 + y * 17) & 255);
        p[2] = static_cast<unsigned char>((x * 29 + y * 7) & 255);
        p[3] = static_cast<unsigned char>((x * 11 + y * 19) & 255);
    }
    GLuint sourceTexture = 0, sourceFbo = 0, drawSentinel = 0, readSentinel = 0;
    GLuint sentinelTexture = 0;
    glGenTextures(1, &sourceTexture); glBindTexture(GL_TEXTURE_2D, sourceTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1); glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glGenFramebuffers(1, &sourceFbo); glBindFramebuffer(GL_FRAMEBUFFER, sourceFbo); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sourceTexture, 0);
    glGenTextures(1, &sentinelTexture); glBindTexture(GL_TEXTURE_2D, sentinelTexture); glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &drawSentinel); glBindFramebuffer(GL_FRAMEBUFFER, drawSentinel); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sentinelTexture, 0);
    glGenFramebuffers(1, &readSentinel); glBindFramebuffer(GL_FRAMEBUFFER, readSentinel); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sentinelTexture, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawSentinel); glBindFramebuffer(GL_READ_FRAMEBUFFER, readSentinel); glBindTexture(GL_TEXTURE_2D, sentinelTexture);
    GLint beforeDraw = 0, beforeRead = 0, beforeTexture = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &beforeDraw); glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &beforeRead); glGetIntegerv(GL_TEXTURE_BINDING_2D, &beforeTexture);

    auto metadata = validMetadata();
    hyprcapture::WindowGpuExportCache cache;
    auto packet = cache.exportFrame(sourceFbo, metadata);
    assert(packet && packet->imageFd >= 0 && packet->fenceFd >= 0);
    GLint afterDraw = 0, afterRead = 0, afterTexture = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &afterDraw); glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &afterRead); glGetIntegerv(GL_TEXTURE_BINDING_2D, &afterTexture);
    assert(afterDraw == beforeDraw && afterRead == beforeRead && afterTexture == beforeTexture);
    Frame decoded{}; assert(hyprcapture::gpuwire::decode(packet->header.data(), packet->header.size(), decoded));
    assert(decoded.imageWidth == width && decoded.imageHeight == height && decoded.cropWidth == width && decoded.cropHeight == height);
    assert(decoded.fourcc == 0x34324241U && decoded.stride >= width * 4 && decoded.offset <= 0x7fffffffULL);
    pollfd fence{packet->fenceFd, POLLIN, 0}; assert(poll(&fence, 1, 1000) == 1 && (fence.revents & POLLIN));

    auto createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    auto destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    auto imageTarget = reinterpret_cast<void (*)(GLenum, void*)>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    assert(createImage && destroyImage && imageTarget);
    const EGLint lo = static_cast<EGLint>(decoded.modifier & 0xffffffffULL), hi = static_cast<EGLint>(decoded.modifier >> 32U);
    const EGLint attrs[] = {EGL_WIDTH, width, EGL_HEIGHT, height, EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(decoded.fourcc),
        EGL_DMA_BUF_PLANE0_FD_EXT, packet->imageFd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, static_cast<EGLint>(decoded.offset),
        EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(decoded.stride), EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, lo,
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, hi, EGL_NONE};
    EGLImageKHR image = createImage(egl.display(), EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs); assert(image != EGL_NO_IMAGE_KHR);
    GLuint importedTexture = 0, importedFbo = 0; glGenTextures(1, &importedTexture); glBindTexture(GL_TEXTURE_2D, importedTexture); imageTarget(GL_TEXTURE_2D, image);
    glGenFramebuffers(1, &importedFbo); glBindFramebuffer(GL_FRAMEBUFFER, importedFbo); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, importedTexture, 0);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE); std::vector<unsigned char> observed(pixels.size());
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, observed.data()); assert(glGetError() == GL_NO_ERROR && observed == pixels);

    assert(cache.imageBuilds() == 1 && cache.capabilityQueries() == 1);
    const auto waitFence = [](const WindowGpuPacket& p) {
        assert((fcntl(p.imageFd, F_GETFD) & FD_CLOEXEC) && (fcntl(p.fenceFd, F_GETFD) & FD_CLOEXEC));
        pollfd ready{p.fenceFd, POLLIN, 0};
        assert(poll(&ready, 1, 1000) == 1 && (ready.revents & POLLIN));
    };
    // Reusing storage must publish changed pixels and alpha through the same
    // import, not an EGL_IMAGE_PRESERVED snapshot of the first frame.
    for (auto& value : pixels) value ^= 0x5a;
    glBindTexture(GL_TEXTURE_2D, sourceTexture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    metadata.sequence = 2;
    auto second = cache.exportFrame(sourceFbo, metadata);
    assert(second && second->imageFd != packet->imageFd && second->fenceFd != packet->fenceFd);
    waitFence(*second);
    glBindFramebuffer(GL_FRAMEBUFFER, importedFbo);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, observed.data());
    assert(glGetError() == GL_NO_ERROR && observed == pixels);
    assert(cache.imageBuilds() == 1 && cache.capabilityQueries() == 1);
    second.reset();

    auto invalid = hyprcapture::exportWindowGpuFrame(0, metadata); assert(!invalid);
    auto invalidCrop = metadata; invalidCrop.cropWidth = 0; assert(!cache.exportFrame(sourceFbo, invalidCrop));
    destroyImage(egl.display(), image); glDeleteFramebuffers(1, &importedFbo); glDeleteTextures(1, &importedTexture);
    // Release old imports before replacing storage. reset is required even
    // when the driver reuses the same texture/FBO name and dimensions.
    packet.reset();
    cache.reset();
    glBindTexture(GL_TEXTURE_2D, sourceTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 128, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    metadata.logicalWidth = metadata.cropWidth = 128;
    metadata.logicalHeight = metadata.cropHeight = 64;
    auto resized = cache.exportFrame(sourceFbo, metadata);
    assert(resized); waitFence(*resized);
    assert(hyprcapture::gpuwire::decode(resized->header.data(), resized->header.size(), decoded));
    assert(decoded.imageWidth == 128 && decoded.imageHeight == 64 && cache.imageBuilds() == 2);
    resized.reset();
    cache.reset();
    glBindTexture(GL_TEXTURE_2D, sourceTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 128, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    auto sameSizeReallocation = cache.exportFrame(sourceFbo, metadata);
    assert(sameSizeReallocation); waitFence(*sameSizeReallocation);
    assert(cache.imageBuilds() == 3 && cache.capabilityQueries() == 1);
    sameSizeReallocation.reset();
    cache.reset();
    // The unchanged wire format forbids non-ABGR8888 images. Never retain a
    // formerly valid export when backing storage changes to an opaque format.
    glBindTexture(GL_TEXTURE_2D, sourceTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 128, 64, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    assert(!cache.exportFrame(sourceFbo, metadata));
    cache.reset();
    glBindTexture(GL_TEXTURE_2D, sourceTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 128, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    const auto openFdCount = [] {
        return std::distance(std::filesystem::directory_iterator("/proc/self/fd"), std::filesystem::directory_iterator{});
    };
    // Isolated export microbenchmark: same texture/layout/fence handling, no
    // compositor reload and no claims about end-to-end capture or encoding.
    const auto measure = [&](bool reuse) {
        std::vector<double> samples;
        for (int i = 0; i < 120; ++i) {
            ++metadata.sequence;
            const auto start = std::chrono::steady_clock::now();
            auto p = reuse ? cache.exportFrame(sourceFbo, metadata) : hyprcapture::exportWindowGpuFrame(sourceFbo, metadata);
            const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
            if (!p) { std::fprintf(stderr, "export failed reuse=%d iteration=%d GL=%x EGL=%s\n", reuse, i, glGetError(), eglError()); std::abort(); }
            waitFence(*p);
            if (i >= 20) samples.push_back(us);
        }
        std::sort(samples.begin(), samples.end());
        return std::array<double, 2>{samples[samples.size() / 2], samples[samples.size() * 95 / 100]};
    };
    const auto transientTiming = measure(false);
    // Warm cached storage, then verify repeated packets close their duplicate
    // image/fence FDs while retaining exactly the cache's own allocation.
    { auto warm = cache.exportFrame(sourceFbo, metadata); assert(warm); waitFence(*warm); }
    const auto beforeFdCount = openFdCount();
    const auto cachedTiming = measure(true);
    assert(openFdCount() == beforeFdCount);
    std::printf("EXPORT_MICROBENCH us transient_p50=%.3f transient_p95=%.3f cached_p50=%.3f cached_p95=%.3f\n",
                transientTiming[0], transientTiming[1], cachedTiming[0], cachedTiming[1]);
    assert(cache.imageBuilds() == 4 && cache.capabilityQueries() == 1);

    cache.reset();
    glDeleteFramebuffers(1, &sourceFbo); glDeleteFramebuffers(1, &drawSentinel); glDeleteFramebuffers(1, &readSentinel); glDeleteTextures(1, &sourceTexture); glDeleteTextures(1, &sentinelTexture);
    assert(egl.switchContext());
    GLuint otherTexture = 0, otherFbo = 0;
    glGenTextures(1, &otherTexture); glBindTexture(GL_TEXTURE_2D, otherTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &otherFbo); glBindFramebuffer(GL_FRAMEBUFFER, otherFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, otherTexture, 0);
    metadata.logicalWidth = metadata.cropWidth = 2;
    metadata.logicalHeight = metadata.cropHeight = 2;
    auto contextFrame = cache.exportFrame(otherFbo, metadata);
    assert(contextFrame); waitFence(*contextFrame);
    assert(cache.capabilityQueries() == 2 && cache.imageBuilds() == 5);
    contextFrame.reset(); cache.reset();
    glDeleteFramebuffers(1, &otherFbo); glDeleteTextures(1, &otherTexture);
    std::puts("PASS GPU export cache reuse/resize/format/native-fence/state/import RGBA exact");
}
