#version 450

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

float lightBrightness(float normalizedLevel) {
    float darkness = 1.0 - clamp(normalizedLevel, 0.0, 1.0);
    return normalizedLevel / (darkness * 3.0 + 1.0);
}

void main() {
    vec4 texel = texture(rainTexture, fragmentUv);
    texel.a *= fragmentOpacity;
    if (texel.a < 0.01) {
        discard;
    }

    vec3 skyTint = mix(vec3(0.50, 0.62, 0.95), vec3(1.0, 0.97, 0.90),
                       camera.sunDirection.w);
    float skyBrightness = lightBrightness(fragmentSceneLight.x) * camera.sunDirection.w *
        camera.weatherSettings.z;
    float blockBrightness = lightBrightness(fragmentSceneLight.y);
    vec3 illumination = max(skyTint * skyBrightness,
                            vec3(1.0, 0.72, 0.38) * blockBrightness * 0.92);
    texel.rgb *= clamp(illumination, vec3(0.035), vec3(1.25));
    outColor = texel;
}
