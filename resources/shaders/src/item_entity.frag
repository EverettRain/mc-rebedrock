#version 450

#include "include/lightmap.glsl"
#include "include/sun_shadow.glsl"

layout(location = 0) in vec2 fragmentUv;
layout(location = 1) flat in float fragmentTextureLayer;
layout(location = 2) in vec3 fragmentNormal;
layout(location = 3) flat in float fragmentIsCube;
layout(location = 13) flat in float fragmentClosedBox;
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
    // RN-35：级联的两个光源矩阵（0 = 近段 16 格框，1 = 远段 128 格框）。
    // 数组而不是两个具名字段：std140 下 mat4 数组的元素间距就是 64 字节，
    // 与两个相邻的 mat4 逐字节相同，而数组让「加一级」是改一个数字
    mat4 lightViewProj[2];
} camera;

layout(binding = 1) uniform sampler2DArray blockTextures;
// Dedicated entity/creature skins, box-UV mapped (one layer per species).
layout(binding = 4) uniform sampler2DArray entityTextures;
layout(binding = 8) uniform sampler2DArrayShadow shadowDepth;
// RN-34：同一张阴影图的**非比较**采样器（NEAREST）。接触硬化要读回深度值本身来估
// 遮挡距离，而比较采样器返回的是「通过比较」的比例，做不到这件事——所以它必须是
// 第二个绑定点，而不是换掉 binding 8。两个绑定点指向同一个 imageView。
layout(binding = 10) uniform sampler2DArray shadowDepthRaw;

// Vanilla's light curve, identical to grass_block.frag: level 15 is full
// brightness and the falloff steepens toward darkness.
vec3 weatherFogColor(vec3 color) {
    color.rg *= 1.0 - camera.weatherSettings.x * 0.50;
    color.b *= 1.0 - camera.weatherSettings.x * 0.40;
    return color * (1.0 - camera.weatherSettings.y * 0.50);
}

void main() {
    if (fragmentOpacity < 0.01) {
        discard;
    }
    // 方块图标的背面。盒子是闭合的，所以背面永远被正面挡着——除非这个方块是
    // **半透明**的，那时正面会把内壁一起透出来：手持一块玻璃能看见盒子的内侧。
    // 不能靠管线的背面剔除，那条管线还画着有 X 镜像的生物模型（镜像翻绕序）。
    if (fragmentClosedBox > 0.5 && !gl_FrontFacing) {
        discard;
    }
    if (fragmentIsCube > 1.5) {
        // RN-23: the shadow disc, standing in for vanilla's misc/shadow.png. That
        // texture is NOT a radial gradient -- measured off 26.1's own 64x64 file,
        // its alpha is a flat 255 out to r = 0.92 and only the outermost texel or
        // two ramp to 0 (the mid row is 0, 104, 255 x 60, 128, 0). It is a disc
        // with a soft rim, and bilinear filtering is what softens even that.
        //
        // This used to be smoothstep(0.30, 1.0), which starts fading at not quite
        // a third of the radius, so every pixel outside the very middle was
        // weaker than vanilla and the decal read as a faint smudge instead of a
        // shadow. The numbers below are the texture's own ramp, so the disc is
        // as solid here as it is there -- without shipping the file, which the
        // no-bundled-assets rule forbids.
        float radius = length(fragmentUv - vec2(0.5)) * 2.0;
        float softness = 1.0 - smoothstep(0.92, 1.0, radius);
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
            faceShade = cardinalShade(normal);
            // Same receiver rule as terrain: back faces return visibility 1
            // without PCF. Preserve this material's diffuse weighting; the final
            // skyFactor still scales combined ambient/direct skylight.
            float shadowFactor = 1.0;
            if (camera.lightingSettings.w > 0.5) {
                shadowFactor = sunShadowFactor(shadowDepth, shadowDepthRaw,
                                               camera.lightViewProj[0], camera.lightViewProj[1],
                                               fragmentWorldPosition, normal,
                                               camera.sunDirection.xyz,
                                               camera.lightingSettings.z,
                                               // 下落的方块是实心盒，没有薄片
                                               camera.weatherSettings.xy, 0.0);
            }
            // RN-38 把这里的 0.72/0.28 换成了共用的 kSkyAmbientFraction，但仍是
            // 手抄的一份「环境 + 直射」。RN-42 直接调用**同一个函数**：入射角权重、
            // 太阳落山后的份额转移、云量转移，三件事从此只有一份实现。
            //
            // 前两个参数是 1：时段与天气的总量在下面的 tintWeight 里，这里只要分配比例
            terrainSunFactor = sunSkyFactor(1.0, 1.0, shadowFactor, camera.weatherSettings.x,
                                            camera.weatherSettings.y,
                                            dot(normal, normalize(camera.sunDirection.xyz)),
                                            normalize(camera.sunDirection.xyz).y,
                                            // 下落的方块不带水柱这一位
                                            0.0,
                                            // RN-20f-0：光影包这一位
                                            camera.lightingSettings.w);
        } else {
            vec3 fixedLightDirection = normalize(vec3(-0.45, 0.85, 0.30));
            float diffuse = max(dot(normal, fixedLightDirection), 0.0);
            faceShade = 0.42 + diffuse * 0.58;
        }
    }
    if (fragmentSceneLight.x < 0.0) {
        texel.rgb *= faceShade;
    } else {
        // The same lightmap the terrain samples, so a creature reads as part of
        // the scene it stands in. Sharing the include is the point: this used to
        // be a fourth hand-copy of the terrain lighting and drifted from it.
        vec3 skyTint = mix(vec3(0.50, 0.62, 0.95), vec3(1.0, 0.97, 0.90),
                           camera.sunDirection.w);
        // 色调的权重不含阴影（见 grass_block.frag 同一处）
        float tintWeight = camera.sunDirection.w * camera.weatherSettings.z;
        float skyFactor = tintWeight;
        if (fragmentFallingBlock > 0.5) {
            skyFactor *= terrainSunFactor;
        }
        vec3 illumination = sampleLightmap(fragmentSceneLight.x, fragmentSceneLight.y, skyFactor) *
            mix(vec3(1.0), skyTint, tintWeight);
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
    outColor = texel;
}
