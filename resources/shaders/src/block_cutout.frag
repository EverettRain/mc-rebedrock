#version 450

layout(location = 0) in vec2 fragmentUv;
layout(location = 1) in vec3 fragmentNormal;
layout(location = 2) flat in float fragmentTextureLayer;
layout(location = 3) in float fragmentCameraDistance;
layout(location = 4) in float fragmentAmbientOcclusion;
layout(location = 5) in float fragmentSkyLight;
layout(location = 6) in vec3 fragmentWorldPosition;
layout(location = 7) in float fragmentBlockLight;
layout(location = 8) flat in float fragmentFlatSkyLight;
layout(location = 9) flat in float fragmentFlatBlockLight;
layout(location = 10) flat in uint fragmentBiomeMask;
layout(location = 0) out vec4 outColor;

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
} camera;

layout(binding = 1) uniform sampler2DArray blockTextures;
// The 1.16.1 biome colour lookup textures (see grass_block.frag).
layout(binding = 6) uniform sampler2D biomeGrassColors;
layout(binding = 7) uniform sampler2D biomeFoliageColors;

float lightBrightness(float normalizedLevel) {
    float darkness = 1.0 - clamp(normalizedLevel, 0.0, 1.0);
    return normalizedLevel / (darkness * 3.0 + 1.0);
}

void main() {
    vec4 texel = texture(blockTextures, vec3(fragmentUv, fragmentTextureLayer));
    if (texel.a < 0.5) {
        discard;
    }
    vec3 normal = normalize(fragmentNormal);
    float diffuse = max(dot(normal, normalize(camera.sunDirection.xyz)), 0.0);
    float upward = max(normal.y, 0.0);
    float downward = max(-normal.y, 0.0);
    float side = 1.0 - abs(normal.y);
    float horizontalShade = mix(0.68, 0.80, abs(normal.z));
    float faceShade = upward + downward * 0.50 + side * horizontalShade;
    bool smoothLighting = camera.lightingSettings.y > 0.5;
    bool highLighting = camera.lightingSettings.z > 0.5;
    float skyLevel = smoothLighting ? fragmentSkyLight : fragmentFlatSkyLight;
    float blockLevel = smoothLighting ? fragmentBlockLight : fragmentFlatBlockLight;
    float skyBrightness = lightBrightness(skyLevel) * camera.sunDirection.w;
    float blockBrightness = lightBrightness(blockLevel);
    // Vanilla tints the sky light by the time of day — cool blue moonlight at
    // night, warm sunlight by day. That is why desert sand reads pale at night
    // instead of keeping its yellow cast under achromatic light.
    vec3 skyTint = mix(vec3(0.50, 0.62, 0.95), vec3(1.0, 0.97, 0.90),
                       camera.sunDirection.w);
    vec3 skyIllumination = skyTint *
        (faceShade * (0.72 + diffuse * 0.28) * skyBrightness);
    vec3 blockIllumination = vec3(1.0, 0.72, 0.38) * blockBrightness * 0.92;
    vec3 illumination = max(skyIllumination, blockIllumination);
    for (int lightIndex = 0; lightIndex < int(camera.lightingSettings.x); ++lightIndex) {
        vec3 delta = camera.pointLights[lightIndex].xyz - fragmentWorldPosition;
        float attenuation = pow(max(
            1.0 - length(delta) / camera.pointLights[lightIndex].w, 0.0), 2.0);
        illumination += camera.lightColors[lightIndex].rgb *
            attenuation * camera.lightColors[lightIndex].a;
    }
    illumination = clamp(illumination, vec3(0.02), vec3(1.25));
    float ao = smoothLighting
        ? (highLighting
            ? clamp(fragmentAmbientOcclusion, 0.2, 1.0)
            : mix(0.72, 1.0, smoothstep(0.0, 1.0, fragmentAmbientOcclusion)))
        : 1.0;
    vec3 biomeTint = vec3(1.0);
    if (fragmentBiomeMask == 1u) {
        biomeTint = texture(biomeGrassColors, (fragmentWorldPosition.xz + 1024.0) / 2048.0).rgb;
    } else if (fragmentBiomeMask == 2u) {
        biomeTint = texture(biomeFoliageColors, (fragmentWorldPosition.xz + 1024.0) / 2048.0).rgb;
    }
    vec3 litColor = texel.rgb * biomeTint * illumination * ao;
    bool cameraUnderwater = camera.renderSettings.y > 0.5;
    float fog;
    vec3 fogColor;
    if (cameraUnderwater) {
        // Underwater fog (BackgroundRenderer.applyFog style): pure EXP2 fading to
        // the biome water fog colour (0x050533). Density is renderSettings.z
        // (0.08, denser than vanilla's 0.05 for a murkier look); no distance wall.
        float densityDistance = fragmentCameraDistance * camera.renderSettings.z;
        fog = 1.0 - exp(-(densityDistance * densityDistance));
        fogColor = vec3(0.0196, 0.0196, 0.20);
    } else {
        float fogEnd = max(camera.renderSettings.x, 16.0);
        fog = smoothstep(fogEnd * 0.75, fogEnd, fragmentCameraDistance);
        fogColor = camera.horizonFog.rgb;
    }
    outColor = vec4(mix(litColor, fogColor, clamp(fog, 0.0, 1.0)), 1.0);
}
