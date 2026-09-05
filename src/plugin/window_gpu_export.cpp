#include "window_gpu_export.hpp"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include <array>
#include <fcntl.h>
#include <string_view>
#include <unistd.h>

namespace hyprcapture {
namespace {
bool extension(std::string_view list, std::string_view name) {
    size_t offset = 0;
    while (offset < list.size()) {
        const size_t end = list.find(' ', offset);
        const auto item = list.substr(offset, end == std::string_view::npos ? end : end - offset);
        if (item == name) return true;
        if (end == std::string_view::npos) break;
        offset = end + 1;
    }
    return false;
}
struct Images {
    EGLDisplay display;
    PFNEGLDESTROYIMAGEKHRPROC destroyImage;
    PFNEGLDESTROYSYNCKHRPROC destroySync;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    EGLSyncKHR sync = EGL_NO_SYNC_KHR;
    std::array<int, 4> descriptors{-1,-1,-1,-1};
    ~Images() {
        if (sync != EGL_NO_SYNC_KHR) destroySync(display, sync);
        if (image != EGL_NO_IMAGE_KHR) destroyImage(display, image);
        for (int fd : descriptors) if (fd >= 0) close(fd);
    }
};
}

std::optional<WindowGpuPacket> exportWindowGpuFrame(unsigned int framebuffer, gpuwire::Frame metadata) {
    const auto display = eglGetCurrentDisplay();
    const auto context = eglGetCurrentContext();
    if (!framebuffer || display == EGL_NO_DISPLAY || context == EGL_NO_CONTEXT) return std::nullopt;
    const char* raw = eglQueryString(display, EGL_EXTENSIONS);
    if (!raw || !extension(raw, "EGL_KHR_image_base") || !extension(raw, "EGL_MESA_image_dma_buf_export") ||
        !extension(raw, "EGL_ANDROID_native_fence_sync")) return std::nullopt;
    const auto createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    const auto destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    const auto query = reinterpret_cast<PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC>(eglGetProcAddress("eglExportDMABUFImageQueryMESA"));
    const auto exportImage = reinterpret_cast<PFNEGLEXPORTDMABUFIMAGEMESAPROC>(eglGetProcAddress("eglExportDMABUFImageMESA"));
    const auto createSync = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(eglGetProcAddress("eglCreateSyncKHR"));
    const auto destroySync = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(eglGetProcAddress("eglDestroySyncKHR"));
    const auto duplicateFence = reinterpret_cast<PFNEGLDUPNATIVEFENCEFDANDROIDPROC>(eglGetProcAddress("eglDupNativeFenceFDANDROID"));
    if (!createImage || !destroyImage || !query || !exportImage || !createSync || !destroySync || !duplicateFence) return std::nullopt;

    GLint previous = 0, type = 0, name = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    const bool complete = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
    glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previous));
    if (!complete || type != GL_TEXTURE || name <= 0 || glGetError() != GL_NO_ERROR) return std::nullopt;

    GLint oldTexture = 0, width = 0, height = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(name));
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture));
    if (width <= 0 || height <= 0 || glGetError() != GL_NO_ERROR) return std::nullopt;
    Images images{display, destroyImage, destroySync};
    constexpr EGLint preserve[]{EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    images.image = createImage(display, context, EGL_GL_TEXTURE_2D_KHR,
                              reinterpret_cast<EGLClientBuffer>(static_cast<uintptr_t>(name)), preserve);
    if (images.image == EGL_NO_IMAGE_KHR) return std::nullopt;
    int fourcc = 0, planes = 0;
    std::array<EGLuint64KHR,4> modifiers{};
    std::array<EGLint,4> strides{}, offsets{};
    if (query(display, images.image, &fourcc, &planes, modifiers.data()) != EGL_TRUE || planes != 1 ||
        exportImage(display, images.image, images.descriptors.data(), strides.data(), offsets.data()) != EGL_TRUE ||
        images.descriptors[0] < 0 || strides[0] <= 0 || offsets[0] < 0) return std::nullopt;
    metadata.imageWidth = static_cast<uint32_t>(width);
    metadata.imageHeight = static_cast<uint32_t>(height);
    metadata.fourcc = static_cast<uint32_t>(fourcc);
    metadata.stride = static_cast<uint32_t>(strides[0]);
    metadata.offset = static_cast<uint64_t>(offsets[0]);
    metadata.modifier = modifiers[0];
    WindowGpuPacket packet;
    if (!gpuwire::encode(metadata, packet.header)) return std::nullopt;
    images.sync = createSync(display, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
    if (images.sync == EGL_NO_SYNC_KHR) return std::nullopt;
    glFlush();
    packet.fenceFd = duplicateFence(display, images.sync);
    if (packet.fenceFd < 0 || fcntl(packet.fenceFd, F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(images.descriptors[0], F_SETFD, FD_CLOEXEC) != 0) return std::nullopt;
    packet.imageFd = images.descriptors[0];
    images.descriptors[0] = -1;
    return packet;
}
}
