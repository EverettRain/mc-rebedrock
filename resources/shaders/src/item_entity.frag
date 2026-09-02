#version 450

layout(location = 0) in vec2 fragmentUv;
layout(location = 1) flat in float fragmentTextureLayer;
layout(location = 2) in vec3 fragmentNormal;
layout(location = 3) flat in float fragmentIsCube;
layout(location = 4) flat in float fragmentShadowOpacity;
layout(location = 5) flat in float fragmentOpacity;
layout(location = 6) in float fragmentCameraDistance;
layout(location = 7) flat in float fragmentEntityTexture;
// Per-entity (sky, block) light levels, normalised to [0, 1]. Negative means the
// draw supplied no scene light and keeps the legacy fixed-light shading.
layout(location = 8) flat in vec2 fragmentSceneLight;
layout(location = 9) in vec3 fragmentWorldPosition;
// OverlayTexture's hurt row, 1.0 while a creature is inside its hurtTime.
layout(location = 10) flat in float fragmentHurtFlash;
layout(location = 11) flat in float fragmentFallingBlock;
// DYE-3: wool dye tint, white (1,1,1) for every non-wool cube so the multiply
// below is an unconditional no-op there.
layout(location = 12) flat in vec3 fragmentWoolTint;
layout(location = 0) out vec4 outColor;

// The full camera block: the tail (point lights, lighting settings) is the same
// buffer grass_block.frag reads, so lit entities can use the terrain's lighting
// terms verbatim.
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
} camera;

layout(binding = 1) uniform sampler2DArray blockTextures;
// Dedicated entity/creature skins, box-UV mapped (one layer per species).
layout(binding = 4) uniform sampler2DArray entityTextures;
layout(binding = 8) uniform sampler2D shadowDepth;

// Vanilla's light curve, identical to grass_block.frag: level 15 is full
// brightness and the falloff steepens toward darkness.
float lightBrightness(float normalizedLevel) {
    float darkness = 1.0 - clamp(normalizedLevel, 0.0, 1.0);
    return normalizedLevel / (darkness * 3.0 + 1.0);
}

vec3 weatherFogColor(vec3 color) {
    color.rg *= 1.0 - camera.weatherSettings.x * 0.50;
    color.b *= 1.0 - camera.weatherSettings.x * 0.40;
    return color * (1.0 - camera.weatherSettings.y * 0.50);
}

void main() {
    if (fragmentOpacity < 0.01) {
        discard;
    }
    if (fragmentIsCube > 1.5) {
        float radius = length(fragmentUv - vec2(0.5)) * 2.0;
        float softness = 1.0 - smoothstep(0.30, 1.0, radius);
        outColor = vec4(0.0, 0.0, 0.0, fragmentShadowOpacity * softness);
        return;
    }
    vec4 texel = fragmentEntityTexture > 0.5
        ? texture(entityTextures, vec3(fragmentUv, fragmentTextureLayer))
        : texture(blockTextures, vec3(fragmentUv, fragmentTextureLayer));
    // DYE-3: wool dye tint. White for non-wool cubes, so this never touches the
    // body skin, blocks or dropped items.
    texel.rgb *= fragmentWoolTint;
    if (texel.a < 0.1) {
        discard;
    }
    // Vanilla shades an entity in two independent stages: a fixed DiffuseLighting
    // term that depends only on the face normal, times the lightmap sample taken
    // once for the whole entity. Billboards keep the face term at 1.0.
    float faceShade = 1.0;
    float terrainSunFactor = 1.0;
    if (fragmentIsCube > 0.5) {
        vec3 normal = normalize(fragmentNormal);
        if (fragmentFallingBlock > 0.5) {
            float upward = max(normal.y, 0.0);
            float downward = max(-normal.y, 0.0);
            float side = 1.0 - abs(normal.y);
            faceShade = upward + downward * 0.50 + side * mix(0.68, 0.80, abs(normal.z));
            float shadowFactor = 1.0;
            if (camera.lightingSettings.w > 0.5) {
                vec4 lightPosition = camera.lightViewProj * vec4(fragmentWorldPosition, 1.0);
                vec3 shadowUv = lightPosition.xyz / lightPosition.w * 0.5 + 0.5;
                if (shadowUv.x >= 0.0 && shadowUv.x <= 1.0 &&
                    shadowUv.y >= 0.0 && shadowUv.y <= 1.0 && shadowUv.z <= 1.0) {
                    float closestDepth = texture(shadowDepth, shadowUv.xy).r;
                    if (shadowUv.z - 0.002 > closestDepth) {
                        shadowFactor = 0.35;
                    }
                }
            }
            float diffuse = max(dot(normal, normalize(camera.sunDirection.xyz)), 0.0);
            terrainSunFactor = 0.72 + diffuse * shadowFactor * 0.28;
        } else {
            vec3 fixedLightDirection = normalize(vec3(-0.45, 0.85, 0.30));
            float diffuse = max(dot(normal, fixedLightDirection), 0.0);
            faceShade = 0.42 + diffuse * 0.58;
        }
    }
    if (fragmentSceneLight.x < 0.0) {
        texel.rgb *= faceShade;
    } else {
        // Same curve, colours and moving point lights as grass_block.frag, so a
        // creature reads as part of the scene it stands in: sky light follows the
        // day/night cycle, block light keeps the warm torch tint, and the two
        // combine with max() rather than summing.
        // Cool blue moonlight by night, warm sunlight by day — matches the
        // terrain shader so a creature or held block reads as part of the scene.
        vec3 skyTint = mix(vec3(0.50, 0.62, 0.95), vec3(1.0, 0.97, 0.90),
                           camera.sunDirection.w);
        float skyBrightness = lightBrightness(fragmentSceneLight.x) * camera.sunDirection.w *
            camera.weatherSettings.z;
        float blockBrightness = lightBrightness(fragmentSceneLight.y);
        vec3 skyIllumination = skyTint * vec3(skyBrightness);
        if (fragmentFallingBlock > 0.5) {
            skyIllumination *= faceShade * terrainSunFactor;
        }
        vec3 illumination = max(skyIllumination,
                                vec3(1.0, 0.72, 0.38) * blockBrightness * 0.92);
        for (int lightIndex = 0; lightIndex < int(camera.lightingSettings.x); ++lightIndex) {
            vec3 delta = camera.pointLights[lightIndex].xyz - fragmentWorldPosition;
            float radius = camera.pointLights[lightIndex].w;
            float attenuation = pow(max(1.0 - length(delta) / radius, 0.0), 2.0);
            illumination += camera.lightColors[lightIndex].rgb * attenuation *
                camera.lightColors[lightIndex].a;
        }
        illumination = clamp(illumination, vec3(0.035), vec3(1.25));
        texel.rgb *= fragmentFallingBlock > 0.5 ? illumination : faceShade * illumination;
    }
    // OverlayTexture's hurt row is opaque red at alpha 178/255, applied over
    // the lit colour exactly like vanilla's overlay combiner.
    if (fragmentHurtFlash > 0.0) {
        texel.rgb = mix(texel.rgb, vec3(1.0, 0.0, 0.0), 0.698 * fragmentHurtFlash);
    }
    texel.a *= fragmentOpacity;
    if (camera.renderSettings.y > 0.5) {
        float densityDistance = fragmentCameraDistance * camera.renderSettings.z;
        float fog = max(
            1.0 - exp(-(densityDistance * densityDistance)),
            smoothstep(
                camera.renderSettings.w * 0.25,
                camera.renderSettings.w,
                fragmentCameraDistance));
        texel.rgb = mix(texel.rgb, vec3(0.0196, 0.0196, 0.20), clamp(fog, 0.0, 1.0));
    } else if (fragmentSceneLight.x >= 0.0) {
        // Scene-lit draws also take the terrain's horizon fog, so a creature at
        // the far edge of the render distance fades into the sky the same way
        // the chunks around it do instead of standing out as a hard silhouette.
        float fogEnd = max(camera.renderSettings.x, 16.0);
        float fog = smoothstep(fogEnd * 0.75, fogEnd, fragmentCameraDistance);
        texel.rgb = mix(texel.rgb, weatherFogColor(camera.horizonFog.rgb),
                        clamp(fog, 0.0, 1.0));
    }
#ifdef ENCODE_SRGB_OUTPUT
    // 同一份着色器编两次：世界那趟写进 sRGB 附件（硬件编码，混合在线性空间），
    // GUI 那趟写进 UNORM 附件，得自己编码。第一人称手持物与背包里的玩家预览走后者。
    texel.rgb = mix(texel.rgb * 12.92, 1.055 * pow(texel.rgb, vec3(1.0 / 2.4)) - 0.055,
                    step(vec3(0.0031308), texel.rgb));
#endif
    outColor = texel;
}
