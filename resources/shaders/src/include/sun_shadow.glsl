// 太阳阴影的采样，三个片元着色器共用：grass_block.frag、block_cutout.frag、
// item_entity.frag（下落方块那条分支）。
//
// 从前这段是三份手抄的「一次 nearest 采样 + 一次硬阈值」，抄得还不完全一样
// （item_entity 少了一次中间变量）。lightmap.glsl 的抬头已经写过这个教训一次：
// 两份副本漂移就曾经交付过一次 MoltenVK 的管线创建崩溃。
//
// binding 8 的采样器现在是 VulkanRenderer 的 shadowCompareSampler：
// VK_FILTER_LINEAR + compareEnable + VK_COMPARE_OP_LESS_OR_EQUAL。所以：
//   * 采样器必须声明成 sampler2DShadow。用非 shadow 的 sampler2D 采一个开了 compare
//     的采样器是未定义用法，MoltenVK 上是 SPIR-V→MSL 转换失败＝黑窗。
//   * texture() 的第三个分量是**比较参考值**，返回值是「通过比较」的比例（1.0 = 亮），
//     不再是深度值。
//   * 每一次 tap 因为 LINEAR 本身就是硬件在 2x2 邻域上的加权比较。
//
// 采样器本身留在各自的 .frag 里声明（`layout(binding = 8) uniform sampler2DShadow`），
// 由函数参数传进来：shader_descriptor_bindings_test 扫的是 .vert/.frag 里的
// `layout(binding = N)`，不递归进 include 目录，把声明搬进来会让那条护栏瞎掉。

// 全影时的天光系数。1.0 是全亮
const float kSunShadowFactor = 0.35;

// 阴影图边长（纹素），必须与 SunShadowMap.hpp 的 kSunShadowMapResolution 一致
const float kSunShadowMapResolution = 2048.0;

// 1.0 个 NDC 深度单位等于多少格 = 正交的 far - near。偏置以「格」表达再除以它，
// 于是偏置是一个能和方块尺寸对照的量。注意这个换算在换成 orthoRH_ZO 前后**相同**：
// 旧的 NO 约定下着色器对 z 做的 `* 0.5 + 0.5` 恰好把 [-1,1] 还原成同一个 [0,1]，
// (2d-f-n)/(f-n) * 0.5 + 0.5 == (d-n)/(f-n) 是恒等式
const float kSunShadowDepthRangeBlocks = 319.9;

#include "sun_shadow_bias.glsl"

float sunShadowFactor(sampler2DShadow shadowMap, mat4 lightViewProj, vec3 worldPosition,
                      vec3 normal, vec3 sunDirection) {
    vec4 lightPosition = lightViewProj * vec4(worldPosition, 1.0);
    vec3 projected = lightPosition.xyz / lightPosition.w;
    // xy 从 [-1,1] 重映射到 [0,1]；z **不**重映射——投影是 orthoRH_ZO，
    // 深度已经在 [0,1] 里了，再 * 0.5 + 0.5 会把它压进 [0.5,1]
    vec3 shadowUv = vec3(projected.xy * 0.5 + 0.5, projected.z);
    if (shadowUv.x < 0.0 || shadowUv.x > 1.0 || shadowUv.y < 0.0 || shadowUv.y > 1.0 ||
        shadowUv.z < 0.0 || shadowUv.z > 1.0) {
        // 128 格的正交框之外没有阴影图可查，一律按全亮
        return 1.0;
    }

    float biasBlocks = sunShadowBiasBlocks(dot(normal, normalize(sunDirection)));
    float reference = shadowUv.z - biasBlocks / kSunShadowDepthRangeBlocks;

    // 3x3 的 tap 网格，步长 1 纹素。加上每个 tap 自带的 2x2 双线性，有效覆盖 4x4 纹素
    // = 0.25 x 0.25 格的半影：方块是 1 格，四分之一格读起来是「软了但没糊」。
    // 2x2（±0.5 纹素）只有 0.125 格，和单个硬件 tap 差不多，治不了锯齿；
    // 5x5 是 0.375 格半影但 25 tap x 3 个着色器的纯填充率成本，没实测不上
    // PCF 的 tap 位于接收面上不同的位置，比较深度必须随平面移动。
    // 从现有光源矩阵取横向正交轴，不引入第二套太阳几何或屏幕导数。
    vec3 lightRight = normalize(vec3(lightViewProj[0][0], lightViewProj[1][0], lightViewProj[2][0]));
    vec3 lightUp = normalize(vec3(lightViewProj[0][1], lightViewProj[1][1], lightViewProj[2][1]));
    float normalRight = dot(normal, lightRight);
    float normalUp = dot(normal, lightUp);
    float normalSun = dot(normal, normalize(sunDirection));
    float texel = 1.0 / kSunShadowMapResolution;
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float tapReference = reference + sunShadowTapOffsetBlocks(
                normalRight, normalUp, normalSun, float(x), float(y)) / kSunShadowDepthRangeBlocks;
            lit += texture(shadowMap, vec3(shadowUv.xy + vec2(float(x), float(y)) * texel,
                                           tapReference));
        }
    }
    return mix(kSunShadowFactor, 1.0, lit * (1.0 / 9.0));
}
