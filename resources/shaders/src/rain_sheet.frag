#version 450

#include "include/lightmap.glsl"

// Dedicated native-aspect vanilla rain texture. Scene light is sampled once
// per precipitation column, matching the lightmap treatment of the vanilla
// renderer without changing any world light value.

layout(binding = 0) uniform CameraUniform {
    mat4 model;
    mat4 view;
    mat4 projection;
    vec4 cameraPosition;
    vec4 sunDirection;
    vec4 horizonFog;
    vec4 renderSettings;
    vec4 pointLights[8];
    vec4 lightColors[8];
    vec4 lightingSettings;
    vec4 celestialLayers;
    vec4 weatherSettings;
} camera;

layout(binding = 9) uniform sampler2D rainTexture;

layout(location = 0) in vec2 fragmentUv;
layout(location = 1) flat in float fragmentOpacity;
layout(location = 2) flat in vec2 fragmentSceneLight;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 texel = texture(rainTexture, fragmentUv);
    texel.a *= fragmentOpacity;
    if (texel.a < 0.01) {
        discard;
    }

    // The same lightmap the terrain samples, so the sheet sits in the scene's
    // light rather than in its own.
    vec3 skyTint = mix(vec3(0.50, 0.62, 0.95), vec3(1.0, 0.97, 0.90),
                       camera.sunDirection.w);
    float skyFactor = camera.sunDirection.w * camera.weatherSettings.z;
    vec3 illumination = sampleLightmap(fragmentSceneLight.x, fragmentSceneLight.y, skyFactor) *
        mix(vec3(1.0), skyTint, skyFactor);
    texel.rgb *= clamp(illumination, vec3(0.035), vec3(1.25));
    outColor = texel;
}
