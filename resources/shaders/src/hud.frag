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

// 这个着色器全程在 sRGB 编码值上工作，输出即最终写进帧缓冲的字节。
//
// 整帧都画在未经伽马转换的目标上，被采样的颜色纹理也都是 UNORM，因此纹素采到什么
// 就是什么——与 vanilla 完全一致：纹素乘顶点色、乘亮度，混合也在这些值上做。
// hud.color 里的常量本来就是 vanilla 的编码值（GRAY 0xAA 写成 0.667）。
void main() {
    vec4 color = hud.color;
    if (hud.data.x > 4.5) {
        // 准星：反色混合的那张 GUI 精灵，边缘靠 discard 而不是 alpha 混合
        vec4 texel = texture(guiTextures, vec3(fragmentUv, fragmentTextureLayer));
        color.rgb *= texel.rgb;
        color.a *= texel.a;
        if (color.a < 0.1) {
            discard;
        }
    } else if (hud.data.x > 2.5) {
        if (hud.data.x > 3.5) {
            // 界面里的 3D 方块图标：vanilla 同样是在编码值上乘逐面亮度
            vec4 texel = texture(blockTextures, vec3(fragmentUv, fragmentTextureLayer));
            color.rgb *= texel.rgb * fragmentLight;
            color.a *= texel.a;
        } else {
            vec4 texel = texture(guiTextures, vec3(fragmentUv, fragmentTextureLayer));
            color.rgb *= texel.rgb;
            color.a *= texel.a;
        }
    } else if (hud.data.x > 1.5) {
        // Layer 0 is ascii.png; the rest are legacy unicode font pages.
        color.a *= texture(fontTexture, vec3(fragmentUv, fragmentTextureLayer)).r;
    } else if (hud.data.x > 0.5) {
        vec4 texel = texture(blockTextures, vec3(fragmentUv, fragmentTextureLayer));
        color.rgb *= texel.rgb;
        color.a *= texel.a;
    }
    outColor = color;
}
