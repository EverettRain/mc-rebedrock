#version 450

#include "include/lightmap.glsl"
#include "include/sun_shadow.glsl"

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
// RN-13: the model json's per-element `"shade"` (see grass_block.vert).
layout(location = 12) flat in float fragmentShade;
// RN-41：1 = 竖直薄片（十字植物、作物），fragmentNormal 是着色法线不是几何法线
layout(location = 13) flat in float fragmentThinPlane;

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
    // x = 点光源数量, y = 平滑光照开关, z = 保留位（恒 0，见 RN-19b）, w = 阴影图有效
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
    // RN-4b: appended after lightViewProj so earlier offsets are unchanged.
    vec4 blockAnimationSettings;      // x = active animation count
    vec4 blockAnimations[16];         // x=base layer, y=frame count, z=frame time
} camera;

layout(binding = 1) uniform sampler2DArray blockTextures;
// The sun shadow depth map written by the pre-pass (binding 8). lightingSettings.w
// is 1.0 only when the pre-pass ran this frame, so the sample is skipped when
// the shadow feature is off. sampler2DShadow, not sampler2D: binding 8 carries a
// compare sampler (see include/sun_shadow.glsl).
layout(binding = 8) uniform sampler2DArrayShadow shadowDepth;
// RN-34：同一张阴影图的**非比较**采样器（NEAREST）。接触硬化要读回深度值本身来估
// 遮挡距离，而比较采样器返回的是「通过比较」的比例，做不到这件事——所以它必须是
// 第二个绑定点，而不是换掉 binding 8。两个绑定点指向同一个 imageView。
layout(binding = 10) uniform sampler2DArray shadowDepthRaw;

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
    // Shared receiver rule: back-facing surfaces get visibility 1, sun-facing
    // surfaces keep the existing PCF visibility and lighting weights.
    // The projection, the slope-scaled bias and the 3x3 PCF all live in the shared
    // include; this used to be three hand-copies of a single nearest tap.
    float shadowFactor = 1.0;
    if (camera.lightingSettings.w > 0.5) {
        shadowFactor = sunShadowFactor(shadowDepth, shadowDepthRaw, camera.lightViewProj[0],
                                       camera.lightViewProj[1], fragmentWorldPosition, normal,
                                       camera.sunDirection.xyz, camera.lightingSettings.z,
                                       camera.weatherSettings.xy, fragmentThinPlane);
    }
    // CardinalLighting.DEFAULT, from the shared lightmap include — skipped for a
    // face whose model element declares `"shade": false` (RN-13). Vanilla's
    // BlockModelPart carries that flag per quad and the block renderer feeds
    // shade=1 for those, which is why a lit repeater's six glow billboards read
    // as one even brightness rather than four dim sides and two bright caps.
    float faceShade = fragmentShade < 0.5 ? 1.0 : cardinalShade(normal);
    bool smoothLighting = camera.lightingSettings.y > 0.5;
    float skyLevel = smoothLighting ? fragmentSkyLight : fragmentFlatSkyLight;
    float blockLevel = smoothLighting ? fragmentBlockLight : fragmentFlatBlockLight;
    // SKY_LIGHT_FACTOR for this tick (sunDirection.w), times the weather dimming,
    // times sun-facing shadow visibility. This still dims the combined sky channel,
    // including ambient skylight; a separate direct-sun term is not available yet.
    // Weather and shadow scale only the sky half; the
    // levels themselves stay the mesh/world values, so gameplay light checks are
    // untouched and block light still adds at full strength inside a shadow.
    // RN-38：天光是直射 + 环境两项，阴影只挡直射。`shadowFactor` 现在是**可见度**
    // （1 = 太阳完全照到），份额的分配在 sunSkyFactor 里，三个采样者共用那一份
    float skyFactor = sunSkyFactor(camera.sunDirection.w, camera.weatherSettings.z, shadowFactor,
                                   camera.weatherSettings.x, camera.weatherSettings.y,
                                   // RN-42：直射项的两个几何量。正午的竖直面因此掉到
                                   // 散射那一份，而清晨朝阳的那一面仍旧吃满
                                   dot(normal, normalize(camera.sunDirection.xyz)),
                                   normalize(camera.sunDirection.xyz).y,
                                   // RN-46a：头顶的水柱（格）。水把直射散成漫射，
                                   // 水下的影子因此是淡的，不是糊的
                                   float((fragmentBiomeMask >> 4u) & 15u));
    vec3 lightmap = sampleLightmap(skyLevel, blockLevel, skyFactor);
    // The sky half carries the time-of-day tint: cool blue moonlight, warm
    // sunlight. Block light brings its own tint inside the lightmap.
    vec3 skyTint = mix(vec3(0.50, 0.62, 0.95), vec3(1.0, 0.97, 0.90),
                       camera.sunDirection.w);
    // 色调的权重是**时段与天气**，不含阴影：影子里的光来自天空，它该是天空的颜色，
    // 只是更暗。用含阴影的 skyFactor 会把影子里的色调冲淡成白，那正是影子看起来
    // 「偏灰」的原因
    float tintWeight = camera.sunDirection.w * camera.weatherSettings.z;
    vec3 illumination = lightmap * mix(vec3(1.0), skyTint, tintWeight) * faceShade;
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
    // 26.1 的 AO 只有开/关，开就是 clamp(ao, 0.2, 1.0)
    // 从前这里还有一条自造的 Standard 曲线 mix(0.72, 1.0, smoothstep(ao))，
    // 它把最暗的角抬到 0.86 以上（烘焙侧的下限本身就是 0.5125，不是 0.35），
    // 于是「平滑光照开着却看不出来」——那条曲线连同它的档位已在 RN-19b 删除
    float ambientOcclusion = waterSurface
        ? 1.0
        : (smoothLighting ? clamp(fragmentAmbientOcclusion, 0.2, 1.0) : 1.0);
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
    // RN-46a：低两位才是着色位——高四位现在装着头顶的水柱。写成整字节比较的症状是
    // 水下的草地整片失去生物群系着色
    vec3 biomeTint = (fragmentBiomeMask & 3u) == 3u ? fragmentTint : vec3(1.0);
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
