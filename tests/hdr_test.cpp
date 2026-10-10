#include "shared/hdr_color.hpp"
#include "shared/protocol.hpp"
#include "ui/hdr_image.hpp"
#include "ui/annotation_editor.hpp"
#include <QApplication>
#include <QColorSpace>
#include <QTemporaryDir>
#include <QtCore/qfloat16.h>
#include <cassert>
#include <cstdio>
#include <chrono>
#ifdef HYPRCAPTURE_HAVE_HEIF
#include <libheif/heif.h>
#endif
using namespace hyprcapture;
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    for (unsigned i = 0; i < 65536; ++i) {
        const auto h = uint16_t(i);
        if ((h & 0x7c00) != 0x7c00) assert(color::floatToHalf(color::halfToFloat(h)) == h);
    }
    assert(std::abs(color::nitsToPq(100) - 0.5080784F) < 0.000001F);
    assert(std::abs(color::nitsToPq(203) - 0.5806889F) < 0.00001F);
    assert(color::nitsToPq(10000) == 1.0F);
    QImage sdr(32, 32, QImage::Format_RGBA8888);
    sdr.fill(QColor(128, 64, 32, 255));
    const auto linear = ui::sdrToHdr(sdr);
    const auto preview = ui::hdrToSdr(linear);
    assert(preview.pixelColor(0, 0) == sdr.pixelColor(0, 0));
    assert(preview.colorSpace() == QColorSpace(QColorSpace::SRgb));
    // Diffuse whites and saturated SDR primaries must not be darkened just
    // because their display runs HDR. Exercise the current 173-nit output.
    for (const auto c : {QColor(64, 64, 64), QColor(128, 128, 128), QColor(192, 192, 192),
                         QColor(255, 255, 255), QColor(255, 0, 0)}) {
        sdr.fill(c);
        const auto converted = ui::hdrToSdr(ui::sdrToHdr(sdr, 173), 173).pixelColor(0, 0);
        assert(std::abs(converted.red() - c.red()) <= 2);
        assert(std::abs(converted.green() - c.green()) <= 2);
        assert(std::abs(converted.blue() - c.blue()) <= 2);
    }
    QImage channels(256, 256, QImage::Format_RGBA16FPx4);
    for (unsigned i = 0; i < 65536; ++i) {
        auto* pixel = reinterpret_cast<uint16_t*>(channels.scanLine(i / 256)) + (i % 256) * 4;
        pixel[0] = i; pixel[1] = color::floatToHalf(0.2F); pixel[2] = color::floatToHalf(0.5F); pixel[3] = color::floatToHalf(1.0F);
    }
    const auto optimized = ui::hdrToSdr(channels, 173);
    for (unsigned i = 0; i < 65536; ++i) {
        const auto expected = color::sdrLinear({color::halfToFloat(i), 0.2F, 0.5F}, 173);
        const auto actual = optimized.pixelColor(i % 256, i / 256);
        for (int c = 0; c < 3; ++c) assert(std::abs(optimized.constScanLine(i / 256)[(i % 256) * 4 + c] - color::byte(color::linearToSrgb(expected[c]))) <= 1);
        assert(actual.alpha() == 255);
    }
    if (argc > 1 && std::string(argv[1]) == "--benchmark") {
        QImage big(6144, 3456, QImage::Format_RGBA16FPx4);
        auto* pixels = reinterpret_cast<qfloat16*>(big.bits());
        for (int i = 0; i < big.width() * big.height(); ++i) {
            const auto value = qfloat16((i % 2 ? 0.216F : 1.0F) * 173.0F / 80.0F);
            pixels[i * 4] = pixels[i * 4 + 1] = pixels[i * 4 + 2] = value;
            pixels[i * 4 + 3] = qfloat16(1.0F);
        }
        const auto started = std::chrono::steady_clock::now();
        const auto converted = ui::hdrToSdr(big, 173);
        assert(!converted.isNull());
        printf("6K HDR preview conversion: %lld ms\n", static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count()));
    }
    // HDR highlights retain ordering in SDR and never modify the float source.
    QImage hdr(64, 64, QImage::Format_RGBA16FPx4);
    const float nits[]{100, 203, 1000, 4000};
    for (int y = 0; y < hdr.height(); ++y) {
        auto* row = reinterpret_cast<qfloat16*>(hdr.scanLine(y));
        for (int x = 0; x < hdr.width(); ++x) {
            const float v = nits[x / 16] / color::scRgbNits;
            for (int c = 0; c < 3; ++c) row[x * 4 + c] = qfloat16(v);
            row[x * 4 + 3] = qfloat16(1.0F);
        }
    }
    hdr.setColorSpace(QColorSpace::SRgbLinear);
    const auto before = hdr.copy();
    const auto mapped = ui::hdrToSdr(hdr);
    assert(hdr == before);
    assert(mapped.pixelColor(8, 8).red() < mapped.pixelColor(24, 8).red());
    assert(mapped.pixelColor(24, 8).red() < mapped.pixelColor(40, 8).red());
    assert(mapped.pixelColor(40, 8).red() <= mapped.pixelColor(56, 8).red());
    AnnotationEditor editor;
    editor.setImage(mapped);
    const auto annotated = editor.resultImageOn(hdr).convertToFormat(QImage::Format_RGBA16FPx4);
    assert(!annotated.isNull());
    assert(float(reinterpret_cast<const qfloat16*>(annotated.constScanLine(8))[56 * 4]) == 50.0F);
    // Backward compatible, explicit transport metadata; malformed format rejected.
    CaptureSession session;
    session.id = "hdr-test";
    session.defaults.screenshotFormat = "heic";
    MonitorInfo monitor;
    monitor.name = "HDR"; monitor.logicalGeometry = {0, 0, 64, 64};
    monitor.artifactPath = "/tmp/test.rgba"; monitor.artifactWidth = monitor.artifactHeight = 64;
    monitor.artifactFormat = "rgba16f-linear-srgb"; monitor.artifactReferenceNits = 203;
    session.monitors.push_back(monitor);
    auto encoded = encodeSessionJson(session);
    auto decoded = decodeSessionJson(encoded);
    assert(decoded && decoded->monitors[0].artifactFormat == "rgba16f-linear-srgb");
    assert(decoded->monitors[0].artifactReferenceNits == 203 && decoded->defaults.screenshotFormat == "heic");
    auto at = encoded.find("rgba16f-linear-srgb"); encoded.replace(at, 18, "unknown-format");
    assert(!decodeSessionJson(encoded));
    QTemporaryDir directory;
    assert(directory.isValid());
    QString error;
    const auto path = directory.filePath("test.heic");
    assert(!ui::saveHdrHeic(sdr, path, &error)); // never promote an 8-bit input to HDR
#ifdef HYPRCAPTURE_HAVE_HEIF
    if (ui::hdrHeicAvailable()) {
        if (!ui::saveHdrHeic(hdr, path, &error)) { fprintf(stderr, "HEIC encode: %s\n", qPrintable(error)); return 1; }
        assert(!ui::saveHdrHeic(hdr, path, &error)); // never overwrite
        auto* context = heif_context_alloc();
        assert(heif_context_read_from_file(context, path.toLocal8Bit().constData(), nullptr).code == heif_error_Ok);
        heif_image_handle* handle = nullptr;
        assert(heif_context_get_primary_image_handle(context, &handle).code == heif_error_Ok);
        assert(heif_image_handle_get_luma_bits_per_pixel(handle) == 10);
        assert(heif_image_handle_has_alpha_channel(handle));
        heif_color_profile_nclx* profile = nullptr;
        assert(heif_image_handle_get_nclx_color_profile(handle, &profile).code == heif_error_Ok);
        assert(profile->color_primaries == 9 && profile->transfer_characteristics == 16 && profile->matrix_coefficients == 9);
        heif_nclx_color_profile_free(profile);
        heif_image* image = nullptr;
        assert(heif_decode_image(handle, &image, heif_colorspace_YCbCr, heif_chroma_444, nullptr).code == heif_error_Ok);
        heif_content_light_level light{};
        assert(heif_image_has_content_light_level(image));
        heif_image_get_content_light_level(image, &light);
        assert(light.max_content_light_level >= 3999 && light.max_content_light_level <= 4001);
        int stride = 0;
        const auto* y = reinterpret_cast<const uint16_t*>(heif_image_get_plane_readonly(image, heif_channel_Y, &stride));
        for (int block = 0; block < 4; ++block) {
            const int value = y[8 * (stride / 2) + block * 16 + 8];
            const int expected = int(std::lround(color::nitsToPq(nits[block]) * 1023));
            if (std::abs(value - expected) > 6) {
                fprintf(stderr, "HEIC PQ roundtrip block %d: %d expected %d\n", block, value, expected); return 1;
            }
        }
        heif_image_release(image); heif_image_handle_release(handle); heif_context_free(context);
        puts("HEIC: real 10-bit encode/decode, PQ/BT.2020 NCLX, alpha and MaxCLL verified");
    } else puts("HEIC encoder unavailable: capability rejection verified");
#endif
    puts("HDR half-float, SDR mapping, highlight-preserving annotation and protocol tests passed");
}
