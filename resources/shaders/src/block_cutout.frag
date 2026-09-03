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
    // These four were missing here while present in the C++ CameraUniform, so
    // lightViewProj sat 64 bytes early — a latent std140 mismatch masked only
    // because the shadow pass defaults off. Declaring them fixes the offset and
    // gives the cutout path the animation clock (RN-7 makes fire animate).
    vec4 fluidAnimationLayers;
    vec4 fluidAnimationFrameCounts;
    vec4 fluidAnimationFrameTimes;
    vec4 fluidAnimationSettings;
    mat4 lightViewProj;
    // RN-4b/RN-7: appended after lightViewProj, matching grass_block.frag.
    vec4 blockAnimationSettings;      // x = active animation count
    vec4 blockAnimations[16];         // x=base layer, y=frame count, z=frame time
} camera;

layout(binding = 1) uniform sampler2DArray blockTextures;
// The sun shadow depth map (see grass_block.frag).
layout(binding = 8) uniform sampler2D shadowDepth;

vec3 weatherFogColor(vec3 color) {
    color.rg *= 1.0 - camera.weatherSettings.x * 0.50;
    color.b *= 1.0 - camera.weatherSettings.x * 0.40;
    return color * (1.0 - camera.weatherSettings.y * 0.50);
}

void main() {
    // RN-7: cycle animated block textures (fire) the same way grass_block.frag
    // does for the opaque path — advance the atlas layer from the animation's
    // base by floor(tick/frameTime mod frameCount).
    float animatedLayer = fragmentTextureLayer;
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
    vec4 texel = texture(blockTextures, vec3(fragmentUv, animatedLayer));
    if (texel.a < 0.5) {
        discard;
    }
    vec3 normal = normalize(fragmentNormal);
    // Sun shadow (see grass_block.frag).
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
    // Mask 3 is the per-vertex tint: the biome colour the mesher resolved per
    // column (grass, foliage, the grass block's side overlay), and redstone
    // dust's power-derived red on its grey sprite
    // (RedStoneWireBlock.getColorForPower). Masks 1 and 2 used to sample a
    // biome lookup texture by world position; that texture is gone (BM-1), and
    // so are its descriptor bindings.
    vec3 biomeTint = fragmentBiomeMask == 3u ? fragmentTint : vec3(1.0);
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
        fogColor = weatherFogColor(camera.horizonFog.rgb);
    }
    outColor = vec4(mix(litColor, fogColor, clamp(fog, 0.0, 1.0)), 1.0);
}
