#pragma once
#include "ui/clipboard_utils.hpp"
#include <QDataStream>
#include <QFile>
#include <QImage>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace hyprcapture::ui {
// Private, bounded, lossless transport. PNG remains the SDR fallback.
inline QString saveHdrThumbnail(const QImage &image, bool fullResolution = false) {
  if (image.isNull())
    return {};
  if (fullResolution && (image.width() > 32768 || image.height() > 32768 ||
                         qint64(image.width()) * image.height() * 8 > 512LL * 1024 * 1024))
    return {};
  QImage small = image;
  if (!fullResolution && (small.width() > 1120 || small.height() > 720))
    small = small.scaled(QSize(1120, 720), Qt::KeepAspectRatio,
                         Qt::SmoothTransformation);
  small = small.convertToFormat(QImage::Format_RGBA16FPx4_Premultiplied);
  const auto path = runtimeFile("thumbnail-hdr", ".rgba16f");
  int fd = ::open(QFile::encodeName(path).constData(),
                  O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0)
    return {};
  QFile file;
  if (!file.open(fd, QIODevice::WriteOnly, QFileDevice::AutoCloseHandle)) {
    ::close(fd);
    return {};
  }
  QDataStream stream(&file);
  stream << quint32(0x48435431) << quint32(small.width())
         << quint32(small.height());
  bool ok = stream.status() == QDataStream::Ok;
  for (int y = 0; y < small.height() && ok; ++y)
    ok = file.write(reinterpret_cast<const char *>(small.constScanLine(y)),
                    small.width() * 8) == small.width() * 8;
  file.close();
  if (!ok) {
    QFile::remove(path);
    return {};
  }
  return path;
}
inline QImage takeHdrThumbnail(const QString &path, bool fullResolution = false) {
  int fd = ::open(QFile::encodeName(path).constData(),
                  O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  struct stat st{};
  if (fd < 0)
    return {};
  if (::fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != ::getuid() ||
      (st.st_mode & 0077) || st.st_size < 20 ||
      st.st_size > (fullResolution ? 512LL * 1024 * 1024 + 12 : 1120 * 720 * 8 + 12)) {
    ::close(fd);
    return {};
  }
  QFile file;
  if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
    ::close(fd);
    return {};
  }
  quint32 magic = 0, w = 0, h = 0;
  QDataStream stream(&file);
  stream >> magic >> w >> h;
  if (magic != 0x48435431 || !w || !h || w > (fullResolution ? 32768U : 1120U) || h > (fullResolution ? 32768U : 720U) ||
      st.st_size != 12 + qint64(w) * h * 8)
    return {};
  QImage image(w, h, QImage::Format_RGBA16FPx4_Premultiplied);
  if (image.isNull())
    return {};
  for (quint32 y = 0; y < h; ++y)
    if (file.read(reinterpret_cast<char *>(image.scanLine(y)), w * 8) != w * 8)
      return {};
  file.close();
  QFile::remove(path);
  return image;
}
} // namespace hyprcapture::ui
