#pragma once

#include <QFile>
#include <QImage>
#include <sys/mman.h>
#include <memory>
#include <utility>

namespace hyprcapture::ui {

// Only completed private compositor artifacts may be mapped: their producer
// must never modify/truncate them after publishing the session. Unlinking is
// safe; the mapping remains alive until the final implicitly shared image dies.
inline QImage mapRawRgba(QFile& file, int width, int height, QImage::Format format = QImage::Format_RGBA8888) {
    if (width <= 0 || height <= 0 || width > 32768 || height > 32768)
        return {};
    const int bytesPerPixel = format == QImage::Format_RGBA16FPx4 ? 8 : 4;
    const qint64 bytes = static_cast<qint64>(width) * height * bytesPerPixel;
    if (!file.isOpen() || file.size() != bytes)
        return {};
    void* data = mmap(nullptr, static_cast<size_t>(bytes), PROT_READ, MAP_PRIVATE, file.handle(), 0);
    if (data == MAP_FAILED)
        return {};
    struct Mapping {
        void* data;
        size_t bytes;
    };
    auto* mapping = new Mapping{data, static_cast<size_t>(bytes)};
    // The const-data overload is essential: QImage must detach before any
    // writer obtains bits(), rather than writing into this read-only mapping.
    return QImage(static_cast<const uchar*>(data), width, height, static_cast<qsizetype>(width) * bytesPerPixel,
                  format, [](void* context) {
                      auto* mapping = static_cast<Mapping*>(context);
                      munmap(mapping->data, mapping->bytes);
                      delete mapping;
                  }, mapping);
}

inline std::pair<QImage, QImage> mapScrollSdrHdr(QFile& file, int width, int height) {
    if (width <= 0 || height <= 0 || width > 32768 || height > 32768) return {};
    const qint64 pixels = qint64(width) * height, bytes = pixels * 12;
    if (!file.isOpen() || file.size() != bytes || bytes > 128LL * 1024 * 1024) return {};
    void* data = mmap(nullptr, size_t(bytes), PROT_READ, MAP_PRIVATE, file.handle(), 0);
    if (data == MAP_FAILED) return {};
    auto mapping = std::shared_ptr<void>(data, [bytes](void* p) { munmap(p, size_t(bytes)); });
    auto cleanup = [](void* p) { delete static_cast<std::shared_ptr<void>*>(p); };
    QImage sdr(static_cast<const uchar*>(data), width, height, qsizetype(width) * 4,
               QImage::Format_RGBA8888_Premultiplied, cleanup, new std::shared_ptr<void>(mapping));
    QImage hdr(static_cast<const uchar*>(data) + pixels * 4, width, height, qsizetype(width) * 8,
               QImage::Format_RGBA16FPx4_Premultiplied, cleanup, new std::shared_ptr<void>(mapping));
    return {sdr, hdr};
}

} // namespace hyprcapture::ui
