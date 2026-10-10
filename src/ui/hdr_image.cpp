#include "ui/hdr_image.hpp"
#include "shared/hdr_color.hpp"
#include <QColorSpace>
#include <QFile>
#include <QPainter>
#include <QtCore/qfloat16.h>
#include <memory>
#include <thread>
#ifdef HYPRCAPTURE_HAVE_HEIF
#include <libheif/heif.h>
#endif
namespace hyprcapture::ui {
bool isHdrImage(const QImage& image) {
    return image.format() == QImage::Format_RGBA16FPx4 || image.format() == QImage::Format_RGBA16FPx4_Premultiplied ||
           image.format() == QImage::Format_RGBA32FPx4 || image.format() == QImage::Format_RGBA32FPx4_Premultiplied;
}
QImage hdrToSdr(const QImage& image, float referenceNits) {
    if (!isHdrImage(image)) {
        QImage output = image;
        output.setColorSpace(QColorSpace::SRgb);
        return output;
    }
    const auto linear = image.convertToFormat(QImage::Format_RGBA16FPx4);
    QImage output(image.size(), QImage::Format_RGBA8888);
    if (linear.isNull() || output.isNull()) return {};
    // Half-float transport has only 65536 possible channel values. Evaluate
    // transfer functions once per value rather than three pow() calls per pixel.
    std::array<float, 65536> values;
    std::array<unsigned char, 65536> direct, gamma, alpha;
    const float scale = color::scRgbNits / referenceNits;
    for (unsigned i = 0; i < values.size(); ++i) {
        const float raw = color::halfToFloat(i);
        values[i] = std::isfinite(raw) ? raw * scale : 0.0F;
        direct[i] = color::byte(color::linearToSrgb(values[i]));
        gamma[i] = color::byte(color::linearToSrgb(raw));
        alpha[i] = color::byte(raw);
    }
    const auto* sourceBits = linear.constBits();
    auto* destinationBits = output.bits();
    const auto sourceStride = linear.bytesPerLine(), destinationStride = output.bytesPerLine();
    const auto convertRows = [&](int begin, int end) {
      for (int y = begin; y < end; ++y) {
        const auto* src = reinterpret_cast<const uint16_t*>(sourceBits + y * sourceStride);
        auto* dst = destinationBits + y * destinationStride;
        for (int x = 0; x < image.width(); ++x, src += 4, dst += 4) {
            const float r = values[src[0]], g = values[src[1]], b = values[src[2]];
            const float peak = std::max({r, g, b});
            if (r >= 0 && g >= 0 && b >= 0) {
                if (referenceNits <= color::scRgbNits || peak <= color::sdrShoulderKnee) {
                    dst[0] = direct[src[0]]; dst[1] = direct[src[1]]; dst[2] = direct[src[2]];
                } else {
                    const float headroom = 1.0F - color::sdrShoulderKnee;
                    const float above = peak - color::sdrShoulderKnee;
                    const float factor = (color::sdrShoulderKnee + headroom * above / (above + headroom)) / peak;
                    for (int c = 0; c < 3; ++c) dst[c] = gamma[color::floatToHalf(values[src[c]] * factor)];
                }
            } else {
                const auto rgb = color::sdrLinear({color::halfToFloat(src[0]), color::halfToFloat(src[1]), color::halfToFloat(src[2])}, referenceNits);
                for (int c = 0; c < 3; ++c) dst[c] = gamma[color::floatToHalf(rgb[c])];
            }
            dst[3] = alpha[src[3]];
        }
      }
    };
    const unsigned workers = image.width() * qint64(image.height()) >= 1024 * 1024 ? std::clamp(std::thread::hardware_concurrency(), 1U, 4U) : 1U;
    std::array<std::thread, 3> threads;
    for (unsigned i = 1; i < workers; ++i) threads[i - 1] = std::thread(convertRows, image.height() * i / workers, image.height() * (i + 1) / workers);
    convertRows(0, image.height() / workers);
    for (unsigned i = 1; i < workers; ++i) threads[i - 1].join();
    output.setColorSpace(QColorSpace::SRgb);
    return output;
}
QImage sdrToHdr(const QImage& image, float referenceNits) {
    if (isHdrImage(image)) return image;
    auto srgb = image;
    if (srgb.colorSpace().isValid() && srgb.colorSpace() != QColorSpace(QColorSpace::SRgb))
        srgb = srgb.convertedToColorSpace(QColorSpace::SRgb);
    srgb = srgb.convertToFormat(QImage::Format_RGBA8888);
    QImage output(image.size(), QImage::Format_RGBA16FPx4);
    if (srgb.isNull() || output.isNull()) return {};
    for (int y = 0; y < image.height(); ++y) {
        const auto* src = srgb.constScanLine(y);
        auto* dst = reinterpret_cast<qfloat16*>(output.scanLine(y));
        for (int x = 0; x < image.width(); ++x, src += 4, dst += 4) {
            for (int c = 0; c < 3; ++c) dst[c] = qfloat16(color::srgbToLinear(src[c] / 255.0F) * referenceNits / color::scRgbNits);
            dst[3] = qfloat16(src[3] / 255.0F);
        }
    }
    output.setColorSpace(QColorSpace::SRgbLinear);
    return output;
}
bool hdrHeicAvailable() {
#ifdef HYPRCAPTURE_HAVE_HEIF
    return heif_have_encoder_for_format(heif_compression_HEVC);
#else
    return false;
#endif
}
bool saveHdrHeic(const QImage& image, const QString& path, QString* error) {
    const auto fail = [&](const QString& message) { if (error) *error = message; return false; };
#ifdef HYPRCAPTURE_HAVE_HEIF
    if (!hdrHeicAvailable()) return fail(QStringLiteral("No HEIC encoder is available"));
    if (!isHdrImage(image)) return fail(QStringLiteral("HDR capture data is unavailable"));
    const auto linear = image.convertToFormat(QImage::Format_RGBA16FPx4);
    if (linear.isNull()) return fail(QStringLiteral("Could not prepare HDR pixels"));
    using Context = std::unique_ptr<heif_context, decltype(&heif_context_free)>;
    using Image = std::unique_ptr<heif_image, decltype(&heif_image_release)>;
    using Encoder = std::unique_ptr<heif_encoder, decltype(&heif_encoder_release)>;
    using Profile = std::unique_ptr<heif_color_profile_nclx, decltype(&heif_nclx_color_profile_free)>;
    using Options = std::unique_ptr<heif_encoding_options, decltype(&heif_encoding_options_free)>;
    Context context(heif_context_alloc(), heif_context_free);
    Profile profile(heif_nclx_color_profile_alloc(), heif_nclx_color_profile_free);
    Options options(heif_encoding_options_alloc(), heif_encoding_options_free);
    if (!context || !profile || !options) return fail(QStringLiteral("Could not allocate HEIC encoder"));
    heif_encoder* rawEncoder = nullptr;
    auto status = heif_context_get_encoder_for_format(context.get(), heif_compression_HEVC, &rawEncoder);
    Encoder encoder(rawEncoder, heif_encoder_release);
    if (status.code != heif_error_Ok) return fail(QString::fromUtf8(status.message));
    heif_encoder_set_lossy_quality(encoder.get(), 95);
    heif_encoder_set_parameter_string(encoder.get(), "preset", "fast");
    heif_encoder_set_parameter_string(encoder.get(), "chroma", "444");
    heif_encoder_set_logging_level(encoder.get(), 0);
    heif_image* rawImage = nullptr;
    status = heif_image_create(image.width(), image.height(), heif_colorspace_RGB, heif_chroma_444, &rawImage);
    Image encoded(rawImage, heif_image_release);
    if (status.code != heif_error_Ok) return fail(QString::fromUtf8(status.message));
    const heif_channel channels[]{heif_channel_R, heif_channel_G, heif_channel_B, heif_channel_Alpha};
    std::array<uint16_t*, 4> planes{};
    std::array<int, 4> strides{};
    for (int c = 0; c < 4; ++c) {
        status = heif_image_add_plane(encoded.get(), channels[c], image.width(), image.height(), 10);
        if (status.code != heif_error_Ok) return fail(QString::fromUtf8(status.message));
        planes[c] = reinterpret_cast<uint16_t*>(heif_image_get_plane(encoded.get(), channels[c], &strides[c]));
        if (!planes[c]) return fail(QStringLiteral("Could not allocate HEIC pixels"));
    }
    double maxNits = 0, sumNits = 0;
    for (int y = 0; y < image.height(); ++y) {
        const auto* src = reinterpret_cast<const qfloat16*>(linear.constScanLine(y));
        for (int x = 0; x < image.width(); ++x, src += 4) {
            auto rgb = color::scRgbToBt2020({float(src[0]), float(src[1]), float(src[2])});
            double luminance = 0;
            for (int c = 0; c < 3; ++c) {
                const auto nits = std::isfinite(rgb[c]) ? std::clamp(rgb[c] * color::scRgbNits, 0.0F, 10000.0F) : 0.0F;
                planes[c][y * (strides[c] / 2) + x] = uint16_t(std::lround(color::nitsToPq(nits) * 1023.0F));
                maxNits = std::max(maxNits, double(nits));
                luminance += nits * (c == 0 ? 0.2627 : c == 1 ? 0.6780 : 0.0593);
            }
            sumNits += luminance;
            planes[3][y * (strides[3] / 2) + x] = uint16_t(std::lround(std::clamp(std::isfinite(float(src[3])) ? float(src[3]) : 0.0F, 0.0F, 1.0F) * 1023.0F));
        }
    }
    heif_nclx_color_profile_set_color_primaries(profile.get(), 9); // BT.2020
    heif_nclx_color_profile_set_transfer_characteristics(profile.get(), 16); // ST 2084 PQ
    heif_nclx_color_profile_set_matrix_coefficients(profile.get(), 0); // RGB input
    profile->full_range_flag = 1;
    status = heif_image_set_nclx_color_profile(encoded.get(), profile.get());
    if (status.code != heif_error_Ok) return fail(QString::fromUtf8(status.message));
    heif_content_light_level light{};
    light.max_content_light_level = uint16_t(std::ceil(maxNits));
    light.max_pic_average_light_level = uint16_t(std::ceil(sumNits / (double(image.width()) * image.height())));
    heif_image_set_content_light_level(encoded.get(), &light);
    // Signal BT.2020 non-constant-luminance YCbCr in the HEVC bitstream/container.
    heif_nclx_color_profile_set_matrix_coefficients(profile.get(), 9);
    options->output_nclx_profile = profile.get();
    status = heif_context_encode_image(context.get(), encoded.get(), encoder.get(), options.get(), nullptr);
    if (status.code != heif_error_Ok) return fail(QString::fromUtf8(status.message));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return fail(file.errorString());
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) { file.remove(); return fail(file.errorString()); }
    heif_writer writer{};
    writer.writer_api_version = 1;
    writer.write = [](heif_context*, const void* data, size_t size, void* opaque) -> heif_error {
        auto* output = static_cast<QFile*>(opaque);
        const auto* bytes = static_cast<const char*>(data);
        while (size) {
            const auto count = output->write(bytes, qint64(size));
            if (count <= 0) return {heif_error_Encoding_error, heif_suberror_Unspecified, "Could not write HEIC file"};
            bytes += count; size -= size_t(count);
        }
        return {heif_error_Ok, heif_suberror_Unspecified, "Success"};
    };
    status = heif_context_write(context.get(), &writer, &file);
    if (status.code != heif_error_Ok || !file.flush()) { file.remove(); return fail(QStringLiteral("Could not write HEIC file")); }
    return true;
#else
    Q_UNUSED(image); Q_UNUSED(path);
    return fail(QStringLiteral("HEIC support was not built"));
#endif
}
} // namespace hyprcapture::ui
