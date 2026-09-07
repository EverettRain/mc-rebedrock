#version 450
// 深度附件专用；透明像素口径与 item_entity.frag 一致，不采样正在写入的阴影图。
layout(location = 0) in vec2 fragmentUv;
layout(location = 1) flat in float fragmentTextureLayer;
layout(location = 5) flat in float fragmentOpacity;
layout(location = 7) flat in float fragmentEntityTexture;
layout(binding = 1) uniform sampler2DArray blockTextures;
layout(binding = 4) uniform sampler2DArray entityTextures;
void main() {
    if (fragmentOpacity < 0.01) discard;
    float alpha = fragmentEntityTexture > 0.5
        ? texture(entityTextures, vec3(fragmentUv, fragmentTextureLayer)).a
        : texture(blockTextures, vec3(fragmentUv, fragmentTextureLayer)).a;
    if (alpha < 0.1) discard;
}
