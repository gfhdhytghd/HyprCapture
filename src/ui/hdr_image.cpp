#include "ui/hdr_image.hpp"
#include "shared/hdr_color.hpp"
#include <QColorSpace>
#include <QFile>
#include <QPainter>
#include <QtCore/qfloat16.h>
#include <memory>
#include <thread>
#ifdef HYPRCAPTURE_HAVE_ULTRAHDR
#include <ultrahdr_api.h>
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
#ifdef HYPRCAPTURE_HAVE_ULTRAHDR
    // Probe the actual backend once; a libuhdr built without HEIF must not
    // advertise a format that it cannot encode.
    static const bool available = [] {
        auto* encoder = uhdr_create_encoder();
        if (!encoder) return false;
        std::array<qfloat16, 16 * 16 * 4> pixels;
        pixels.fill(qfloat16(1.0F));
        uhdr_raw_image_t probe{};
        probe.fmt = UHDR_IMG_FMT_64bppRGBAHalfFloat;
        probe.cg = UHDR_CG_BT_2100; probe.ct = UHDR_CT_LINEAR; probe.range = UHDR_CR_FULL_RANGE;
        probe.w = probe.h = 16; probe.planes[0] = pixels.data(); probe.stride[0] = 16;
        const bool supported = uhdr_enc_set_output_format(encoder, UHDR_CODEC_HEIF).error_code == UHDR_CODEC_OK &&
            uhdr_enc_set_raw_image(encoder, &probe, UHDR_HDR_IMG).error_code == UHDR_CODEC_OK &&
            uhdr_encode(encoder).error_code == UHDR_CODEC_OK;
        uhdr_release_encoder(encoder);
        return supported;
    }();
    return available;
#else
    return false;
#endif
}
bool saveHdrHeic(const QImage& image, const QString& path, QString* error, const QImage& sdrBase) {
    const auto fail = [&](const QString& message) { if (error) *error = message; return false; };
#ifdef HYPRCAPTURE_HAVE_ULTRAHDR
    if (!hdrHeicAvailable()) return fail(QStringLiteral("No gain-map HEIC encoder is available"));
    if (!isHdrImage(image)) return fail(QStringLiteral("HDR capture data is unavailable"));
    if (QFile::exists(path)) return fail(QStringLiteral("Output already exists"));
    auto linear = image.convertToFormat(QImage::Format_RGBA16FPx4);
    auto sdr = sdrBase.isNull() ? hdrToSdr(image) : sdrBase.convertToFormat(QImage::Format_RGBA8888);
    if (sdr.size() != image.size()) return fail(QStringLiteral("SDR and HDR dimensions differ"));
    if (linear.isNull() || sdr.isNull()) return fail(QStringLiteral("Could not prepare HDR pixels"));
    // libuhdr's linear units are 203 nits, while our scRGB transport is 80.
    // Use BT.2020 for gain computation so negative scRGB (P3 colors) survives.
    float peakNits = 203;
    for (int y = 0; y < linear.height(); ++y) {
        auto* row = reinterpret_cast<qfloat16*>(linear.scanLine(y));
        for (int x = 0; x < linear.width(); ++x, row += 4) {
            const auto rgb = color::scRgbToBt2020({float(row[0]), float(row[1]), float(row[2])});
            for (int c = 0; c < 3; ++c) {
                const float nits = std::isfinite(rgb[c]) ? std::clamp(rgb[c] * color::scRgbNits, 0.0F, 10000.0F) : 0;
                peakNits = std::max(peakNits, nits);
                row[c] = qfloat16(nits / 203.0F);
            }
        }
    }
    using Encoder = std::unique_ptr<uhdr_codec_private_t, decltype(&uhdr_release_encoder)>;
    Encoder encoder(uhdr_create_encoder(), uhdr_release_encoder);
    if (!encoder) return fail(QStringLiteral("Could not allocate HEIC encoder"));
    const auto check = [&](uhdr_error_info_t status) {
        if (status.error_code == UHDR_CODEC_OK) return true;
        return fail(status.has_detail ? QString::fromUtf8(status.detail) : QStringLiteral("Gain-map HEIC encoding failed"));
    };
    uhdr_raw_image_t hdrInput{};
    hdrInput.fmt = UHDR_IMG_FMT_64bppRGBAHalfFloat;
    hdrInput.cg = UHDR_CG_BT_2100; hdrInput.ct = UHDR_CT_LINEAR; hdrInput.range = UHDR_CR_FULL_RANGE;
    hdrInput.w = linear.width(); hdrInput.h = linear.height();
    hdrInput.planes[0] = linear.bits(); hdrInput.stride[0] = linear.bytesPerLine() / 8;
    uhdr_raw_image_t sdrInput{};
    sdrInput.fmt = UHDR_IMG_FMT_32bppRGBA8888;
    sdrInput.cg = UHDR_CG_BT_709; sdrInput.ct = UHDR_CT_SRGB; sdrInput.range = UHDR_CR_FULL_RANGE;
    sdrInput.w = sdr.width(); sdrInput.h = sdr.height();
    sdrInput.planes[0] = sdr.bits(); sdrInput.stride[0] = sdr.bytesPerLine() / 4;
    if (!check(uhdr_enc_set_output_format(encoder.get(), UHDR_CODEC_HEIF)) ||
        !check(uhdr_enc_set_raw_image(encoder.get(), &hdrInput, UHDR_HDR_IMG)) ||
        !check(uhdr_enc_set_raw_image(encoder.get(), &sdrInput, UHDR_SDR_IMG)) ||
        !check(uhdr_enc_set_gainmap_scale_factor(encoder.get(), 1)) ||
        !check(uhdr_enc_set_using_multi_channel_gainmap(encoder.get(), 1)) ||
        !check(uhdr_enc_set_target_display_peak_brightness(encoder.get(), peakNits)) ||
        !check(uhdr_enc_set_quality(encoder.get(), 95, UHDR_BASE_IMG)) ||
        !check(uhdr_enc_set_quality(encoder.get(), 100, UHDR_GAIN_MAP_IMG)) ||
        !check(uhdr_encode(encoder.get()))) return false;
    const auto* stream = uhdr_get_encoded_stream(encoder.get());
    if (!stream || !stream->data || !stream->data_sz) return fail(QStringLiteral("HEIC encoder returned no data"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return fail(file.errorString());
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) { file.remove(); return fail(file.errorString()); }
    const auto* bytes = static_cast<const char*>(stream->data);
    size_t remaining = stream->data_sz;
    while (remaining) {
        const auto written = file.write(bytes, qint64(remaining));
        if (written <= 0) { const auto message = file.errorString(); file.remove(); return fail(message); }
        bytes += written; remaining -= size_t(written);
    }
    if (!file.flush()) { const auto message = file.errorString(); file.remove(); return fail(message); }
    return true;
#else
    Q_UNUSED(image); Q_UNUSED(path); Q_UNUSED(sdrBase);
    return fail(QStringLiteral("HEIC support was not built"));
#endif
}
} // namespace hyprcapture::ui
