#version 450

layout(binding = 0) uniform sampler2D shadowDepth;

layout(location = 0) in vec2 fragmentUv;
layout(location = 0) out vec4 outColor;

// GUI 那趟画在场景图的 UNORM 视图上（世界那趟画在同一张图的 SRGB 视图上），
// 因此这里要自己把线性值编码成 sRGB——vanilla 的界面也正是在编码值上合成的。
vec3 guiEncode(vec3 value) {
    return mix(value * 12.92, 1.055 * pow(value, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, value));
}

void main() {
    // Raw depth is [0, 1] from the light's near plane; invert so near = bright.
    float depth = texture(shadowDepth, fragmentUv).r;
    outColor = vec4(guiEncode(vec3(1.0 - depth)), 1.0);
}
