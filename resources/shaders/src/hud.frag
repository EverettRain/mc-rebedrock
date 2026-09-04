#version 450

layout(location = 0) in vec2 fragmentUv;
layout(location = 1) flat in float fragmentTextureLayer;
// NOT flat. RN-14 made this per-vertex so the icon's corner AO shades the face
// with a gradient (see hud.vert); this side kept saying `flat`, which both drops
// the gradient and is an interface mismatch — Vulkan requires the interpolation
// decoration to match for the two stages to interface at all.
layout(location = 2) in vec3 fragmentLight;
layout(location = 0) out vec4 outColor;

layout(binding = 1) uniform sampler2DArray blockTextures;
layout(binding = 2) uniform sampler2DArray fontTexture;
layout(binding = 3) uniform sampler2DArray guiTextures;

// Declared identically in hud.vert and in mc::render::HudPush. This block used to
// stop at `data`, four fields against the vertex stage's five, and read `color`
// as a tint while RN-14 was using it to carry the icon's box — which is what made
// every block icon a black diamond. hud_push_constant_test holds the three
// declarations together now; the fields this stage does not read are still
// declared, because the block is a memory layout, not a list of what one stage
// happens to want.
layout(push_constant) uniform HudPush {
    vec4 rect;       // clip-space origin xy, size zw
    vec4 color;      // tint, multiplied into the texel
    vec4 uvRect;     // sprite source origin xy, size zw (sprite modes)
    vec4 data;       // x = draw mode, y = atlas layer
    vec4 iconBoxMin; // block icon: the box, xyz
    vec4 iconBoxMax;
    vec4 iconUv01;   // block icon: the face's four corner UVs
    vec4 iconUv23;
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
