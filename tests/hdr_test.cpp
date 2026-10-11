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
#ifdef HYPRCAPTURE_HAVE_ULTRAHDR
#include <ultrahdr_api.h>
#include <QFile>
#include <QImageReader>
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
#ifdef HYPRCAPTURE_HAVE_ULTRAHDR
    if (ui::hdrHeicAvailable()) {
        // Include HDR saturated primaries, P3 green and gray highlights. A
        // gray-only test cannot detect matrix or gain-map color-space errors.
        const std::array<float, 3> colors[]{
            {100,100,100}, {203,203,203}, {630,630,630}, {4000,4000,4000},
            {400,0,0}, {0,400,0}, {0,0,400},
            {-38.91F,180.28F,-13.61F}, {400,0,400}};
        QImage colored(576, 128, QImage::Format_RGBA16FPx4);
        colored.setColorSpace(QColorSpace::SRgbLinear);
        for (int y = 0; y < colored.height(); ++y) {
            auto* row = reinterpret_cast<qfloat16*>(colored.scanLine(y));
            for (int x = 0; x < colored.width(); ++x) {
                for (int c = 0; c < 3; ++c) row[x*4+c] = qfloat16(colors[x/64][c]/80);
                row[x*4+3] = qfloat16(y < 16 ? 0.5F : 1.0F);
            }
        }
        const auto base = ui::hdrToSdr(colored, 173);
        const auto original = colored.copy();
        if (!ui::saveHdrHeic(colored, path, &error, base)) {
            fprintf(stderr,"HEIC encode: %s\n",qPrintable(error)); return 1;
        }
        assert(colored == original);
        assert(!ui::saveHdrHeic(colored, path, &error, base));
        // Independent system decoder / Qt image plugin must see a normal SDR
        // primary, without knowing anything about the gain-map extension.
        QImageReader reader(path);
        auto fallback = reader.read();
        assert(!fallback.isNull() && fallback.size() == base.size());
        assert(std::abs(fallback.pixelColor(8,8).alpha()-128)<=2);
        if (fallback.colorSpace().isValid()) fallback = fallback.convertedToColorSpace(QColorSpace::SRgb);
        for (int block = 0; block < 9; ++block) {
            const auto expected = base.pixelColor(block*64+32,64);
            const auto actual = fallback.pixelColor(block*64+32,64);
            for (int c = 0; c < 3; ++c) {
                const int e[]{expected.red(),expected.green(),expected.blue()};
                const int v[]{actual.red(),actual.green(),actual.blue()};
                if (std::abs(e[c]-v[c]) > 5) {
                    fprintf(stderr,"SDR fallback block %d channel %d: %d expected %d\n",block,c,v[c],e[c]); return 1;
                }
            }
        }
        QFile file(path); assert(file.open(QIODevice::ReadOnly)); auto bytes=file.readAll();
        uhdr_compressed_image_t input{}; input.data=bytes.data(); input.data_sz=input.capacity=bytes.size();
        input.cg=UHDR_CG_UNSPECIFIED; input.ct=UHDR_CT_UNSPECIFIED; input.range=UHDR_CR_UNSPECIFIED;
        auto* decoder=uhdr_create_decoder(); assert(decoder);
        const auto check=[](uhdr_error_info_t status){if(status.error_code!=UHDR_CODEC_OK){fprintf(stderr,"HDR decode: %s\n",status.detail); return false;}return true;};
        assert(check(uhdr_dec_set_image(decoder,&input)));
        assert(check(uhdr_dec_set_out_img_format(decoder,UHDR_IMG_FMT_64bppRGBAHalfFloat)));
        assert(check(uhdr_dec_set_out_color_transfer(decoder,UHDR_CT_LINEAR)));
        assert(check(uhdr_dec_set_out_max_display_boost(decoder,10000.0F/203)));
        assert(check(uhdr_dec_probe(decoder)));
        const auto* metadata=uhdr_dec_get_gainmap_metadata(decoder);
        assert(metadata && metadata->hdr_capacity_max > 3);
        assert(uhdr_dec_get_gainmap_width(decoder)==colored.width());
        assert(uhdr_dec_get_gainmap_height(decoder)==colored.height());
        assert(check(uhdr_decode(decoder)));
        const auto* decodedImage=uhdr_get_decoded_image(decoder);
        assert(decodedImage && decodedImage->cg==UHDR_CG_BT_2100);
        assert(std::abs(float(static_cast<const qfloat16*>(decodedImage->planes[0])[3])-0.5F)<0.01F);
        for (int block=0;block<9;++block) {
            const auto* row=static_cast<const qfloat16*>(decodedImage->planes[0])+64*decodedImage->stride[0]*4+(block*64+32)*4;
            const auto expected=color::scRgbToBt2020({colors[block][0]/80,colors[block][1]/80,colors[block][2]/80});
            for(int c=0;c<3;++c){
                const float nits=float(row[c])*203, expectedNits=expected[c]*80;
                if(std::abs(nits-expectedNits)>std::max(3.0F,expectedNits*0.04F)) {
                    fprintf(stderr,"HDR gain-map block %d channel %d: %.2f nit expected %.2f\n",block,c,nits,expectedNits);return 1;
                }
            }
        }
        uhdr_release_decoder(decoder);
        if (argc == 3 && std::string(argv[1]) == "--export-fixture") assert(QFile::copy(path, QString::fromLocal8Bit(argv[2])));
        const auto odd = colored.copy(0,0,17,19);
        const auto oddPath = directory.filePath("odd.heic");
        assert(ui::saveHdrHeic(odd, oddPath, &error, base.copy(0,0,17,19)));
        assert(QImageReader(oddPath).read().size()==QSize(17,19));
        puts("HEIC: independent SDR fallback and colored HDR/P3 gain-map reconstruction verified");
    } else puts("HEIC encoder unavailable: capability rejection verified");
#endif
    puts("HDR half-float, SDR mapping, highlight-preserving annotation and protocol tests passed");
}
