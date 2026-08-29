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

vec3 srgbToLinear(vec3 value) {
    return mix(value / 12.92, pow((value + 0.055) / 1.055, vec3(2.4)), step(0.04045, value));
}

vec3 linearToSrgb(vec3 value) {
    return mix(value * 12.92, 1.055 * pow(value, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, value));
}

void main() {
    vec4 color = hud.color;
    if (hud.data.x > 4.5) {
        color *= texture(guiTextures, vec3(fragmentUv, fragmentTextureLayer));
        if (color.a < 0.1) {
            discard;
        }
    } else if (hud.data.x > 2.5) {
        if (hud.data.x > 3.5) {
            color *= texture(blockTextures, vec3(fragmentUv, fragmentTextureLayer));
            color.rgb *= fragmentLight;
        } else {
            // vanilla multiplies the raw sRGB texel by the vertex colour and lets
            // the (sRGB) framebuffer pass it through. Our sampler already decoded
            // to linear, so convert back, tint in sRGB space, then decode again;
            // tinting in linear space would leave every dark tint visibly
            // brighter than vanilla — most noticeably the menu dirt backdrop.
            vec4 texel = texture(guiTextures, vec3(fragmentUv, fragmentTextureLayer));
            color.rgb = srgbToLinear(linearToSrgb(texel.rgb) * hud.color.rgb);
            color.a = texel.a * hud.color.a;
        }
    } else if (hud.data.x > 1.5) {
        // Layer 0 is ascii.png; the rest are legacy unicode font pages.
        color.a *= texture(fontTexture, vec3(fragmentUv, fragmentTextureLayer)).r;
    } else if (hud.data.x > 0.5) {
        color *= texture(blockTextures, vec3(fragmentUv, fragmentTextureLayer));
    }
    outColor = color;
}
