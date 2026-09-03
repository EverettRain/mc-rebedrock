#version 450

#include "include/lightmap.glsl"

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
layout(location = 11) in vec3 fragmentTint;

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
    vec4 celestialLayers;
    vec4 weatherSettings;
    vec4 fluidAnimationLayers;
    vec4 fluidAnimationFrameCounts;
    vec4 fluidAnimationFrameTimes;
    vec4 fluidAnimationSettings;
    mat4 lightViewProj;
    // RN-4b: appended after lightViewProj so earlier offsets are unchanged.
    vec4 blockAnimationSettings;      // x = active animation count
    vec4 blockAnimations[16];         // x=base layer, y=frame count, z=frame time
} camera;

layout(binding = 1) uniform sampler2DArray blockTextures;
// The sun shadow depth map written by the pre-pass (binding 8). lightingSettings.w
// is 1.0 only when the pre-pass ran this frame, so the sample is skipped when
// the shadow feature is off.
layout(binding = 8) uniform sampler2D shadowDepth;

vec3 weatherFogColor(vec3 color) {
    color.rg *= 1.0 - camera.weatherSettings.x * 0.50;
    color.b *= 1.0 - camera.weatherSettings.x * 0.40;
    return color * (1.0 - camera.weatherSettings.y * 0.50);
}

void main() {
    vec2 animatedUv = fragmentUv;
    float animatedLayer = fragmentTextureLayer;
    for (int animation = 0; animation < 4; ++animation) {
        float baseLayer = camera.fluidAnimationLayers[animation];
        if (abs(fragmentTextureLayer - baseLayer) < 0.1) {
            float frameCount = max(camera.fluidAnimationFrameCounts[animation], 1.0);
            float frameTime = max(camera.fluidAnimationFrameTimes[animation], 1.0);
            animatedLayer += floor(mod(camera.fluidAnimationSettings.x / frameTime, frameCount));
            break;
        }
    }
    // RN-4b: cycle animated non-fluid block textures the same way. A fluid base
    // never equals a block-animation base, so running both loops is safe.
    int blockAnimationCount = int(camera.blockAnimationSettings.x);
    for (int animation = 0; animation < blockAnimationCount; ++animation) {
        vec4 blockAnimation = camera.blockAnimations[animation];
        if (abs(fragmentTextureLayer - blockAnimation.x) < 0.1) {
            float frameCount = max(blockAnimation.y, 1.0);
            float frameTime = max(blockAnimation.z, 1.0);
            animatedLayer = blockAnimation.x +
                floor(mod(camera.fluidAnimationSettings.x / frameTime, frameCount));
            break;
        }
    }
    vec4 texel = texture(blockTextures, vec3(animatedUv, animatedLayer));
    vec3 normal = normalize(fragmentNormal);
    // Sun shadow: project the fragment into the light space the pre-pass wrote
    // the depth map with, and darken the sun term where a closer surface blocks
    // it. The comparison uses a small depth bias to avoid self-shadow acne, and
    // falls back to fully lit outside the map's bounds.
    float shadowFactor = 1.0;
    if (camera.lightingSettings.w > 0.5) {
        vec4 lightPosition = camera.lightViewProj * vec4(fragmentWorldPosition, 1.0);
        vec3 projected = lightPosition.xyz / lightPosition.w;
        vec3 shadowUv = projected * 0.5 + 0.5;
        if (shadowUv.x >= 0.0 && shadowUv.x <= 1.0 && shadowUv.y >= 0.0 && shadowUv.y <= 1.0 &&
            shadowUv.z <= 1.0) {
            float closestDepth = texture(shadowDepth, shadowUv.xy).r;
            if (shadowUv.z - 0.002 > closestDepth) {
                shadowFactor = 0.35;
            }
        }
    }
    // CardinalLighting.DEFAULT, from the shared lightmap include.
    float faceShade = cardinalShade(normal);
    bool smoothLighting = camera.lightingSettings.y > 0.5;
    bool highLighting = camera.lightingSettings.z > 0.5;
    float skyLevel = smoothLighting ? fragmentSkyLight : fragmentFlatSkyLight;
    float blockLevel = smoothLighting ? fragmentBlockLight : fragmentFlatBlockLight;
    // SKY_LIGHT_FACTOR for this tick (sunDirection.w), times the weather dimming,
    // times the sun-shadow term. Weather and shadow scale only the sky half; the
    // levels themselves stay the mesh/world values, so gameplay light checks are
    // untouched and block light still adds at full strength inside a shadow.
    float skyFactor = camera.sunDirection.w * camera.weatherSettings.z * shadowFactor;
    vec3 lightmap = sampleLightmap(skyLevel, blockLevel, skyFactor);
    // The sky half carries the time-of-day tint: cool blue moonlight, warm
    // sunlight. Block light brings its own tint inside the lightmap.
    vec3 skyTint = mix(vec3(0.50, 0.62, 0.95), vec3(1.0, 0.97, 0.90),
                       camera.sunDirection.w);
    vec3 illumination = lightmap * mix(vec3(1.0), skyTint, skyFactor) * faceShade;
    for (int lightIndex = 0; lightIndex < int(camera.lightingSettings.x); ++lightIndex) {
        vec3 delta = camera.pointLights[lightIndex].xyz - fragmentWorldPosition;
        float radius = camera.pointLights[lightIndex].w;
        float attenuation = pow(max(1.0 - length(delta) / radius, 0.0), 2.0);
        illumination += camera.lightColors[lightIndex].rgb *
            attenuation * camera.lightColors[lightIndex].a;
    }
    illumination = clamp(illumination, vec3(0.02), vec3(1.25));
    bool cameraUnderwater = camera.renderSettings.y > 0.5;
    bool waterSurface = abs(fragmentTextureLayer - camera.fluidAnimationLayers.x) < 0.1 ||
        abs(fragmentTextureLayer - camera.fluidAnimationLayers.y) < 0.1;
    float ambientOcclusion = waterSurface
        ? 1.0
        : (smoothLighting
            ? (highLighting
                ? clamp(fragmentAmbientOcclusion, 0.2, 1.0)
                : mix(0.72, 1.0, smoothstep(0.0, 1.0, fragmentAmbientOcclusion)))
            : 1.0);
    // The per-vertex biome colour tint (grass tops/plants and foliage) is
    // white for everything else, so ordinary blocks are unchanged.
    // The per-fragment biome colour: grass tops/plants sample the grass map,
    // oak-family leaves the foliage map, everything else is white. The lookup
    // texture is linear-filtered, so the colour gradients across biome
    // boundaries instead of switching per block.
    // The biome colour is a per-vertex tint now: the mesher resolves each column
    // as the average of the biome colours in the 5x5 block window around it —
    // vanilla's biomeBlendRadius — so a biome border interpolates across the
    // face. It used to sample a lookup texture baked from the *overworld* biome
    // map by world position, which meant the nether and end read overworld
    // colours, and it could not tint water at all.
    vec3 biomeTint = fragmentBiomeMask == 3u ? fragmentTint : vec3(1.0);
    vec3 litColor = texel.rgb * biomeTint * illumination * ambientOcclusion;
    float outputAlpha = texel.a;
    // The depth-based surface tint approximates looking down through water from
    // above. When the camera is submerged the volumetric EXP2 fog below governs
    // visibility instead, so skip the tint to avoid double-darkening.
    if (waterSurface && !cameraUnderwater) {
        // Top-down water transparency driven by the water-column depth the mesher
        // stores in the water AO channel (>= 1 block, interpolated per corner).
        // Shallow water stays clear so the seabed reads through; deeper water
        // absorbs toward an opaque murky blue with a strong gradient. The opacity
        // is set explicitly rather than derived from the near-opaque water texture
        // alpha, so the depth variation is actually visible from above.
        float columnDepth = max(fragmentAmbientOcclusion - 1.0, 0.0);
        float opacityFactor = 1.0 - exp(-columnDepth * 0.5);
        outputAlpha = mix(0.50, 0.95, opacityFactor);
        float darkness = 1.0 - exp(-columnDepth * 0.40);
        // Deeper water absorbs light toward a darker, bluer shade. The darkening
        // is relative to the already-lit colour, so it follows the day/night
        // cycle — a fixed deep colour held its brightness at night and read as
        // self-illuminated.
        vec3 deepTint = vec3(0.30, 0.45, 0.65);
        litColor = mix(litColor, litColor * deepTint, darkness * 0.85);
    }

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
        fogColor = weatherFogColor(camera.horizonFog.rgb);
    }
    outColor = vec4(mix(litColor, fogColor, clamp(fog, 0.0, 1.0)), outputAlpha);
}
