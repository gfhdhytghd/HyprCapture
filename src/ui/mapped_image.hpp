#pragma once

#include <QFile>
#include <QImage>
#include <sys/mman.h>

namespace hyprcapture::ui {

// Only completed private compositor artifacts may be mapped: their producer
// must never modify/truncate them after publishing the session. Unlinking is
// safe; the mapping remains alive until the final implicitly shared image dies.
inline QImage mapRawRgba(QFile& file, int width, int height) {
    if (width <= 0 || height <= 0 || width > 32768 || height > 32768)
        return {};
    const qint64 bytes = static_cast<qint64>(width) * height * 4;
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
    return QImage(static_cast<const uchar*>(data), width, height, static_cast<qsizetype>(width) * 4,
                  QImage::Format_RGBA8888, [](void* context) {
                      auto* mapping = static_cast<Mapping*>(context);
                      munmap(mapping->data, mapping->bytes);
                      delete mapping;
                  }, mapping);
}

} // namespace hyprcapture::ui
