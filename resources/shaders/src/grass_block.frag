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
// The 1.16.1 biome colour lookup textures, sampled with linear filtering so the
// grass/foliage colour blends smoothly across biome boundaries. The texture is
// 512 texels at 4 blocks each, covering [-1024, 1024] blocks around the world
// origin; the UV mapping must match kBiomeTextureSize/BlockSpan in the renderer.
layout(binding = 6) uniform sampler2D biomeGrassColors;
layout(binding = 7) uniform sampler2D biomeFoliageColors;

float lightBrightness(float normalizedLevel) {
    float darkness = 1.0 - clamp(normalizedLevel, 0.0, 1.0);
    return normalizedLevel / (darkness * 3.0 + 1.0);
}

void main() {
    vec2 animatedUv = fragmentUv;
    float animatedLayer = fragmentTextureLayer;
    float animationFrame = floor(mod(camera.horizonFog.w * 10.0, 32.0));
    if (abs(fragmentTextureLayer - 20.0) < 0.1) {
        animatedLayer += animationFrame;
    } else if (abs(fragmentTextureLayer - 52.0) < 0.1) {
        animatedLayer += animationFrame;
    } else if (abs(fragmentTextureLayer - 344.0) < 0.1) {
        // Lava still: 20 frames starting at layer 344.
        animatedLayer += floor(mod(camera.horizonFog.w * 10.0, 20.0));
    } else if (abs(fragmentTextureLayer - 364.0) < 0.1) {
        // Lava flow: 16 frames starting at layer 364.
        animatedLayer += floor(mod(camera.horizonFog.w * 10.0, 16.0));
    }
    vec4 texel = texture(blockTextures, vec3(animatedUv, animatedLayer));
    vec3 sunDirection = normalize(camera.sunDirection.xyz);
    vec3 normal = normalize(fragmentNormal);
    float diffuse = max(dot(normal, sunDirection), 0.0);
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
        float radius = camera.pointLights[lightIndex].w;
        float attenuation = pow(max(1.0 - length(delta) / radius, 0.0), 2.0);
        illumination += camera.lightColors[lightIndex].rgb *
            attenuation * camera.lightColors[lightIndex].a;
    }
    illumination = clamp(illumination, vec3(0.02), vec3(1.25));
    bool cameraUnderwater = camera.renderSettings.y > 0.5;
    bool waterSurface = abs(fragmentTextureLayer - 20.0) < 0.1 ||
        abs(fragmentTextureLayer - 52.0) < 0.1;
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
    vec3 biomeTint = vec3(1.0);
    if (fragmentBiomeMask == 1u) {
        biomeTint = texture(biomeGrassColors, (fragmentWorldPosition.xz + 1024.0) / 2048.0).rgb;
    } else if (fragmentBiomeMask == 2u) {
        biomeTint = texture(biomeFoliageColors, (fragmentWorldPosition.xz + 1024.0) / 2048.0).rgb;
    }
    vec3 litColor = texel.rgb * biomeTint * illumination * ambientOcclusion;
    float outputAlpha = texel.a;
    // The depth-based surface tint approximates looking down through water from
    // above. When the camera is submerged the volumetric EXP2 fog below governs
    // visibility instead, so skip the tint to avoid double-darkening.
    if (waterSurface && !cameraUnderwater) {
        // The vanilla water_still texture is grayscale — the blue comes from a
        // biome water-colour tint applied here. Without it the surface reads as
        // a murky grey sheet, whitest at night.
        const vec3 kWaterColor = vec3(0.25, 0.45, 0.85);
        litColor *= kWaterColor;
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
        fogColor = camera.horizonFog.rgb;
    }
    outColor = vec4(mix(litColor, fogColor, clamp(fog, 0.0, 1.0)), outputAlpha);
}
