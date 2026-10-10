#pragma once
namespace hyprcapture::color {
inline constexpr const char* sdrResolveVertex = R"(#version 300 es
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
inline constexpr const char* sdrResolveFragment = R"(#version 300 es
precision highp float;
uniform sampler2D sourceImage;
uniform int sourceTF;
uniform float whiteScale;
uniform float blackLevel;
uniform float shoulderKnee;
uniform mat3 primaries;
out vec4 result;
vec3 decode(vec3 v) {
    if (sourceTF == 0 || sourceTF == 5) return v;
    if (sourceTF == 2) return pow(max(v, vec3(0.0)), vec3(2.2));
    if (sourceTF == 3) return pow(max(v, vec3(0.0)), vec3(2.8));
    if (sourceTF == 11) {
        vec3 p = pow(max(v, vec3(0.0)), vec3(32.0 / 2523.0));
        return pow(max(p - 3424.0 / 4096.0, vec3(0.0)) / (2413.0 / 128.0 - 2392.0 / 128.0 * p), vec3(16384.0 / 2610.0));
    }
    vec3 a = abs(v);
    vec3 linear = mix(pow((a + 0.055) / 1.055, vec3(2.4)), a / 12.92, lessThanEqual(a, vec3(0.04045)));
    return sign(v) * linear;
}
void main() {
    vec4 source = texelFetch(sourceImage, ivec2(gl_FragCoord.xy), 0);
    float alpha = clamp(source.a, 0.0, 1.0);
    vec3 rgb = primaries * decode(source.rgb / max(alpha, 0.000001));
    rgb = (rgb - blackLevel) * whiteScale;
    float peak = max(max(rgb.r, rgb.g), rgb.b);
    if ((sourceTF == 11 || whiteScale < 1.0) && peak > shoulderKnee) {
        float headroom = 1.0 - shoulderKnee;
        float mapped = shoulderKnee + headroom * (peak - shoulderKnee) / (peak - shoulderKnee + headroom);
        rgb *= mapped / peak;
    }
    float y = clamp(dot(rgb, vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0);
    float saturation = 1.0;
    for (int c = 0; c < 3; ++c) if (rgb[c] < 0.0) saturation = min(saturation, y / (y - rgb[c]));
    rgb = clamp(vec3(y) + saturation * (rgb - y), 0.0, 1.0);
    vec3 encoded = mix(1.055 * pow(rgb, vec3(1.0 / 2.4)) - 0.055, rgb * 12.92, lessThanEqual(rgb, vec3(0.0031308)));
    result = vec4(encoded * alpha, alpha);
}
)";
}
