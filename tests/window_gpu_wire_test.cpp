#include "plugin/window_gpu_wire.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>

using hyprcapture::CornerRadii;
using namespace hyprcapture::gpuwire;

namespace {

static Frame frame() {
    Frame f{}; f.sequence=9; f.captureMonotonicNs=1234; f.geometryEpoch=7; f.logicalX=-2.5; f.logicalY=3.25; f.logicalWidth=800; f.logicalHeight=600;
    f.imageWidth=1600; f.imageHeight=1200; f.stride=6400; f.modifier=0x3000; f.offset=64; f.cropX=4; f.cropY=8; f.cropWidth=1592; f.cropHeight=1184; f.flipY=true;
    f.shadowEnabled=true; f.shadow={1,2,800,600,8,9,784,584,24,{12,0,5,9},{10,6,3,1},2,3,{1,2,3,4},true}; return f;
}

template<std::size_t Bytes>
constexpr std::array<std::uint8_t, Bytes> fromHex(std::string_view hex) {
    assert(hex.size() == Bytes * 2);
    const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    std::array<std::uint8_t, Bytes> bytes{};
    for (std::size_t i = 0; i < Bytes; ++i)
        bytes[i] = static_cast<std::uint8_t>((digit(hex[i * 2]) << 4) | digit(hex[i * 2 + 1]));
    return bytes;
}

constexpr auto v1 = fromHex<HCGF_V1_BYTES>(
    "48434746000100e8000000000000000900000000000004d20000000000000007"
    "c004000000000000400a00000000000040890000000000004082c00000000000"
    "00000640000004b0343242410000190000000000000030000000000000000040"
    "000000040000000800000638000004a000000007000000003ff0000000000000"
    "400000000000000040890000000000004082c000000000004020000000000000"
    "4022000000000000408880000000000040824000000000004038000000000000"
    "4028000000000000402400000000000040000000000000000000000301020304"
    "0000000100000000");

constexpr auto v2 = fromHex<HCGF_BYTES>(
    "4843474600020118000000000000000900000000000004d20000000000000007"
    "c004000000000000400a00000000000040890000000000004082c00000000000"
    "00000640000004b0343242410000190000000000000030000000000000000040"
    "000000040000000800000638000004a000000007000000003ff0000000000000"
    "400000000000000040890000000000004082c000000000004020000000000000"
    "4022000000000000408880000000000040824000000000004038000000000000"
    "4028000000000000000000000000000040140000000000004022000000000000"
    "4024000000000000401800000000000040080000000000003ff0000000000000"
    "400000000000000000000003010203040000000100000000");

void testInputAndRelease() {
    InputGeometry input{0x1234,0x5678,9012,-2.5,3.25,800,600,400,300}, inputRound{};
    std::array<std::uint8_t,HCGI_BYTES> inputWire{};
    assert(encode(input,inputWire));
    assert(decode(inputWire.data(),inputWire.size(),inputRound));
    assert(inputRound.window == input.window && inputRound.surface == input.surface && inputRound.pid == input.pid);
    assert(inputRound.contentX == -2.5 && inputRound.surfaceWidth == 400);
    for (std::size_t size=0;size<HCGI_BYTES;++size) assert(!decode(inputWire.data(),size,inputRound));
    auto badInput=inputWire; badInput[80]=1; assert(!decode(badInput.data(),badInput.size(),inputRound));
    auto invalidInput=input; invalidInput.surface=0; assert(!encode(invalidInput,badInput));
    invalidInput=input; invalidInput.contentWidth=__builtin_nan("x"); assert(!encode(invalidInput,badInput));
    std::array<char,HCGI_BYTES*2+1> hex{};
    for(std::size_t i=0;i<inputWire.size();++i) std::snprintf(hex.data()+i*2,3,"%02x",inputWire[i]);
    assert(std::strcmp(hex.data(),"4843474900010058000000000000123400000000000056780000000000002334c004000000000000400a00000000000040890000000000004082c0000000000040790000000000004072c000000000000000000000000000")==0);
    std::array<std::uint8_t,HCGR_BYTES> release{}; Release r{9,7},r2{}; assert(encode(r,release)); assert(std::memcmp(release.data(),"HCGR",4)==0&&release[7]==HCGR_BYTES); assert(decode(release.data(),release.size(),r2)&&r2.sequence==9&&r2.geometryEpoch==7); release[24]=1; assert(!decode(release.data(),release.size(),r2));
}

void testV2() {
    auto f = frame();
    std::array<std::uint8_t, HCGF_BYTES> wire{};
    assert(encode(f, wire));
    assert(wire == v2);

    Frame round{};
    assert(decode(wire.data(), wire.size(), round));
    assert(round.sequence == f.sequence && round.geometryEpoch == f.geometryEpoch);
    assert(round.shadowEnabled && round.flipY);
    assert(round.shadow.rounding == f.shadow.rounding);
    assert(round.shadow.windowRounding == f.shadow.windowRounding);
    assert(round.shadow.roundingPower == 2 && round.shadow.power == 3);
    assert(round.shadow.rgba == f.shadow.rgba && round.shadow.sharp);

    // Use populated shadow metadata to check that the encoder drops the whole block.
    f.shadowEnabled = false;
    assert(encode(f, wire));
    assert(std::all_of(wire.begin() + 120, wire.end(), [](auto byte) { return byte == 0; }));
    assert(decode(wire.data(), wire.size(), round));
    assert(!round.shadowEnabled && round.shadow.rounding == CornerRadii{});

    f = frame();
    f.logicalX = std::numeric_limits<double>::quiet_NaN();
    assert(!encode(f, wire));
    f = frame();
    f.cropX = 0xffffffffU;
    assert(!encode(f, wire));
    f = frame();
    f.imageHeight = 0x80000000U;
    assert(!encode(f, wire));
}

void testV1() {
    Frame round{};
    assert(decode(v1.data(), v1.size(), round));
    assert(round.sequence == 9 && round.captureMonotonicNs == 1234 && round.geometryEpoch == 7);
    assert(round.shadowEnabled && round.flipY);
    assert(round.shadow.rounding == CornerRadii::uniform(12));
    assert(round.shadow.windowRounding == CornerRadii::uniform(10));
    assert(round.shadow.roundingPower == 2 && round.shadow.power == 3 && round.shadow.sharp);
    assert((round.shadow.rgba == std::array<std::uint8_t, 4>{1, 2, 3, 4}));
}

void expectRejection(const std::uint8_t* bytes, std::size_t size, Error expected) {
    Frame out{};
    Error error{};
    assert(!decode(bytes, size, out, &error));
    assert(error == expected);
}

template<std::size_t Bytes>
void testStrictFields(std::array<std::uint8_t, Bytes> wire) {
    const auto reject = [](const auto& bytes, Error error) {
        expectRejection(bytes.data(), bytes.size(), error);
    };
    auto bad = wire;
    bad[5] = 3;
    reject(bad, Error::Version);
    bad = wire;
    // A valid version paired with the other version's datagram length.
    bad[5] = Bytes == HCGF_V1_BYTES ? 2 : 1;
    reject(bad, Error::Length);
    bad = wire;
    // The datagram size matches the version, but the declared size does not.
    constexpr auto other = Bytes == HCGF_V1_BYTES ? HCGF_BYTES : HCGF_V1_BYTES;
    bad[6] = other >> 8;
    bad[7] = other & 0xff;
    reject(bad, Error::HeaderLength);
    bad = wire;
    bad[115] |= 8;
    reject(bad, Error::Flags);
    bad = wire;
    bad[115] &= ~1;
    reject(bad, Error::Flags);
    bad = wire;
    bad[116] = 1;
    reject(bad, Error::Reserved);
    bad = wire;
    bad[Bytes - 1] = 1;
    reject(bad, Error::Reserved);
    bad = wire;
    bad[Bytes - 5] = 2;
    reject(bad, Error::Shadow);

    constexpr std::size_t roundingPower = Bytes == HCGF_V1_BYTES ? 208 : 256;
    for (std::size_t offset = 192; offset < roundingPower; offset += 8) {
        bad = wire;
        bad[offset] = 0xbf;
        bad[offset + 1] = 0xf0;
        reject(bad, Error::Shadow);
        bad = wire;
        bad[offset] = 0x7f;
        bad[offset + 1] = 0xf0;
        reject(bad, Error::Shadow);
    }

    wire[115] &= ~4;
    std::fill(wire.begin() + 120, wire.end(), 0);
    Frame out{};
    assert(decode(wire.data(), wire.size(), out) && !out.shadowEnabled);
    for (std::size_t offset = 120; offset < Bytes; ++offset) {
        bad = wire;
        bad[offset] = 1;
        reject(bad, offset < Bytes - 4 ? Error::Shadow : Error::Reserved);
    }
}

void testStrictFrames() {
    testStrictFields(v1);
    testStrictFields(v2);
    for (std::size_t size = 0; size < v2.size(); ++size)
        expectRejection(v2.data(), size, Error::Length);
    std::array<std::uint8_t, HCGF_BYTES + 1> extra{};
    std::copy(v2.begin(), v2.end(), extra.begin());
    expectRejection(extra.data(), extra.size(), Error::Length);

    auto f = frame();
    std::array<std::uint8_t, HCGF_BYTES> wire{};
    for (auto* radii : {&f.shadow.rounding, &f.shadow.windowRounding}) {
        for (auto* corner : {&radii->topLeft, &radii->topRight, &radii->bottomRight, &radii->bottomLeft}) {
            const auto original = *corner;
            *corner = -1;
            assert(!encode(f, wire));
            *corner = std::numeric_limits<double>::infinity();
            assert(!encode(f, wire));
            *corner = original;
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 1) {
        testInputAndRelease();
        testV2();
    } else if (argc == 2 && std::string_view(argv[1]) == "v1") {
        testV1();
    } else if (argc == 2 && std::string_view(argv[1]) == "strict") {
        testStrictFrames();
    } else {
        return 1;
    }
}
