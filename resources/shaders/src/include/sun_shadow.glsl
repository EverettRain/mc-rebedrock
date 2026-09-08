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
    // 三个接收者统一：没有太阳直射的面不受此方向的遮挡影响，也无需 PCF。
    // 受光面的光照权重保持原样；合并 sky 通道仍包含环境天光，这是待拆分的近似。
    float incidence = dot(normal, normalize(sunDirection));
    if (incidence <= 0.0) {
        return 1.0;
    }
    // RN-33：偏置沿**法线**把采样点抬离表面，而不是朝太阳压深度。压深度会把影子从
    // 投射者脚下推开（那里的真实深度差趋近于 0，一压就没了）；沿法线抬不会。
    // 抬高必须在投影**之前**加进世界坐标——加在投影之后就又变成了沿光线方向的位移。
    vec3 offsetPosition = worldPosition + normal * sunShadowNormalOffsetBlocks(incidence);
    vec4 lightPosition = lightViewProj * vec4(offsetPosition, 1.0);
    vec3 projected = lightPosition.xyz / lightPosition.w;
    // xy 从 [-1,1] 重映射到 [0,1]；z **不**重映射——投影是 orthoRH_ZO，
    // 深度已经在 [0,1] 里了，再 * 0.5 + 0.5 会把它压进 [0.5,1]
    vec3 shadowUv = vec3(projected.xy * 0.5 + 0.5, projected.z);
    if (shadowUv.x < 0.0 || shadowUv.x > 1.0 || shadowUv.y < 0.0 || shadowUv.y > 1.0 ||
        shadowUv.z < 0.0 || shadowUv.z > 1.0) {
        // 128 格的正交框之外没有阴影图可查，一律按全亮
        return 1.0;
    }

    float reference = shadowUv.z - kSunShadowDepthBiasBlocks / kSunShadowDepthRangeBlocks;

    // 2x2 的 tap 网格，位置 ±0.5 纹素。加上每个 tap 自带的 2x2 硬件双线性比较，
    // 有效覆盖 2x2 纹素 = **0.125 格**的半影。
    //
    // 从前是 3x3、步长 1 纹素，覆盖 4x4 纹素 = 0.25 格。那个半径是**固定**的：不管接收
    // 点离挡光的方块是 0 格还是 20 格都糊同样宽。而方块**脚下**的遮挡距离就是 0，物理上
    // 那里应当是硬边，却照样吃满 0.25 格 —— 紧贴方块的约 1/8 格因此读成了亮边，实测
    // 值 35/255（该夹具满对比度 55），是这条边的主因。
    //
    // 缩到 2x2 把它减半，同时把每像素的采样次数从 9 降到 4（省约 55%，逐屏幕像素、
    // 三个着色器）。边缘不会退化成台阶：硬件那层 2x2 加权比较仍在，一次查表就是四次
    // 比较，价钱只算一次。
    //
    // 真正的答案是接触硬化（按遮挡距离缩放半径），但它要多一轮遮挡搜索采样加一个
    // 运行时决定的循环长度，成本压在每一个朝阳的屏幕像素上；等有了真机帧时间再评估。
    // PCF 的 tap 位于接收面上不同的位置，比较深度必须随平面移动。
    // 从现有光源矩阵取横向正交轴，不引入第二套太阳几何或屏幕导数。
    vec3 lightRight = normalize(vec3(lightViewProj[0][0], lightViewProj[1][0], lightViewProj[2][0]));
    vec3 lightUp = normalize(vec3(lightViewProj[0][1], lightViewProj[1][1], lightViewProj[2][1]));
    float normalRight = dot(normal, lightRight);
    float normalUp = dot(normal, lightUp);
    float normalSun = incidence;
    float texel = 1.0 / kSunShadowMapResolution;
    float lit = 0.0;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            float tapX = float(x) - 0.5;
            float tapY = float(y) - 0.5;
            float tapReference = reference + sunShadowTapOffsetBlocks(
                normalRight, normalUp, normalSun, tapX, tapY) / kSunShadowDepthRangeBlocks;
            lit += texture(shadowMap, vec3(shadowUv.xy + vec2(tapX, tapY) * texel, tapReference));
        }
    }
    return mix(kSunShadowFactor, 1.0, lit * 0.25);
}
