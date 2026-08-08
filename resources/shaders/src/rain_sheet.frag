#version 450

// Scrolled water-layer quad for 贴图雨. The atlas V scrolls with wall-clock
// time so the bands fall; the water layer's alpha (multiplied by the sheet
// opacity) keeps the sheets translucent like rain.

layout(binding = 0) uniform CameraUniform {
    mat4 model;
    mat4 view;
    mat4 projection;
    vec4 cameraPosition;
    vec4 sunDirection;
    vec4 horizonFog;
    vec4 renderSettings;
} camera;

layout(binding = 1) uniform sampler2DArray blockTextures;

layout(location = 0) in vec2 fragmentUv;
layout(location = 1) flat in float fragmentOpacity;
layout(location = 2) flat in float fragmentLayer;
layout(location = 3) flat in float fragmentTime;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 scrolled = vec2(fragmentUv.x, fragmentUv.y + fragmentTime);
    vec4 texel = texture(blockTextures, vec3(scrolled, fragmentLayer));
    texel.a *= fragmentOpacity;
    outColor = texel;
}
