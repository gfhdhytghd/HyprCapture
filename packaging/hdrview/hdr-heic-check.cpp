#include <ultrahdr_api.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

void check(uhdr_error_info_t status) {
    if (status.error_code != UHDR_CODEC_OK) throw std::runtime_error(status.detail);
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
    if (bytes.empty() || !is_uhdr_image(bytes.data(), bytes.size())) return 3;
    auto* decoder = uhdr_create_decoder();
    if (!decoder) return 4;
    uhdr_compressed_image_t compressed{bytes.data(), bytes.size(), bytes.size(), UHDR_CG_UNSPECIFIED, UHDR_CT_UNSPECIFIED, UHDR_CR_UNSPECIFIED};
    check(uhdr_dec_set_image(decoder, &compressed));
    check(uhdr_dec_set_out_color_transfer(decoder, UHDR_CT_LINEAR));
    check(uhdr_dec_set_out_img_format(decoder, UHDR_IMG_FMT_64bppRGBAHalfFloat));
    check(uhdr_decode(decoder));
    const auto* image = uhdr_get_decoded_image(decoder);
    if (!image || image->w != 576 || image->h != 128 || image->cg != UHDR_CG_BT_2100) return 5;
    auto value = [&](int x, int y, int channel) {
        const auto* p = static_cast<const char*>(image->planes[0]) + (y * image->stride[0] + x) * 8 + channel * 2;
        _Float16 half;
        std::memcpy(&half, p, 2);
        return static_cast<float>(half);
    };
    const float levels[]{100, 203, 630, 4000};
    for (int i = 0; i < 4; ++i) {
        for (int c = 0; c < 3; ++c) {
            const float actual = value(i * 64 + 32, 64, c) * 203;
            if (std::abs(actual - levels[i]) > std::max(3.f, levels[i] * .04f)) return 6;
        }
    }
    if (std::abs(value(32, 8, 3) - .5f) > .02f) return 7;
    uhdr_release_decoder(decoder);
    std::puts("ISO HEIC HDR decode: BT.2020, 100/203/630/4000 nit samples and alpha passed");
}
