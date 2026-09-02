#version 450

layout(location = 0) in vec2 fragmentUv;
layout(location = 1) flat in float fragmentTextureLayer;
layout(location = 2) flat in vec3 fragmentLight;
layout(location = 0) out vec4 outColor;

layout(binding = 1) uniform sampler2DArray blockTextures;
layout(binding = 2) uniform sampler2DArray fontTexture;
layout(binding = 3) uniform sampler2DArray guiTextures;

layout(push_constant) uniform HudPush {
    vec4 rect;
    vec4 color;
    vec4 uvRect;
    vec4 data;
} hud;

// 这个着色器**全程在 sRGB 编码值上工作**，输出即最终写进帧缓冲的字节。
//
// vanilla 的界面画在一块未经伽马转换的帧缓冲上：纹素怎么采到就怎么乘顶点色，
// alpha 混合也在这些编码值上做。GUI 那趟因此绑的是场景图的 UNORM 视图（世界那趟
// 绑的是同一张图的 SRGB 视图），固定功能混合于是与 vanilla 同一个空间。
//
// 两个后果，都是修复：
//   · hud.color 里的常量本来就是 vanilla 的编码值（GRAY 0xAA 写成 0.667），
//     此前被当线性值输出，屏幕上是 0xD5——所有非白色的界面文字都亮一档；
//   · 半透明深色叠加层（提示框底衬、界面暗化）此前在线性空间混合，放走的背景
//     多得多，提示框压在浅色面板上亮到原版的两倍。
//
// 采样器仍是 sRGB 格式的（世界那趟需要线性值），所以采到的纹素要先编码回去。
vec3 linearToSrgb(vec3 value) {
    return mix(value * 12.92, 1.055 * pow(value, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, value));
}

void main() {
    vec4 color = hud.color;
    if (hud.data.x > 4.5) {
        // 准星：反色混合的那张 GUI 精灵，边缘靠 discard 而不是 alpha 混合
        vec4 texel = texture(guiTextures, vec3(fragmentUv, fragmentTextureLayer));
        color.rgb *= linearToSrgb(texel.rgb);
        color.a *= texel.a;
        if (color.a < 0.1) {
            discard;
        }
    } else if (hud.data.x > 2.5) {
        if (hud.data.x > 3.5) {
            // 界面里的 3D 方块图标：vanilla 同样是在编码值上乘逐面亮度
            vec4 texel = texture(blockTextures, vec3(fragmentUv, fragmentTextureLayer));
            color.rgb *= linearToSrgb(texel.rgb) * fragmentLight;
            color.a *= texel.a;
        } else {
            vec4 texel = texture(guiTextures, vec3(fragmentUv, fragmentTextureLayer));
            color.rgb *= linearToSrgb(texel.rgb);
            color.a *= texel.a;
        }
    } else if (hud.data.x > 1.5) {
        // Layer 0 is ascii.png; the rest are legacy unicode font pages.
        color.a *= texture(fontTexture, vec3(fragmentUv, fragmentTextureLayer)).r;
    } else if (hud.data.x > 0.5) {
        vec4 texel = texture(blockTextures, vec3(fragmentUv, fragmentTextureLayer));
        color.rgb *= linearToSrgb(texel.rgb);
        color.a *= texel.a;
    }
    outColor = color;
}
