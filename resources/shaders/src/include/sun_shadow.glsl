// 太阳阴影的采样，三个片元着色器共用：grass_block.frag、block_cutout.frag、
// item_entity.frag（下落方块那条分支）。
//
// 从前这段是三份手抄的「一次 nearest 采样 + 一次硬阈值」，抄得还不完全一样
// （item_entity 少了一次中间变量）。lightmap.glsl 的抬头已经写过这个教训一次：
// 两份副本漂移就曾经交付过一次 MoltenVK 的管线创建崩溃。
//
// RN-35：级联。阴影图是一张**两层**的数组图像，采样器因此是 sampler2DArrayShadow /
// sampler2DArray，第三个坐标分量是层号。近段（层 0）覆盖玩家周围 16 格、一个纹素
// 1/128 格；远段（层 1）覆盖 128 格、一个纹素 1/16 格，与从前完全一样。
// 接收点先试近段，落在它的框里就用它，否则退到远段——**只试一次，不做两级混合**。
//
// binding 8 的采样器现在是 VulkanRenderer 的 shadowCompareSampler：
// VK_FILTER_LINEAR + compareEnable + VK_COMPARE_OP_LESS_OR_EQUAL。所以：
//   * 采样器必须声明成 sampler2DArrayShadow。用非 shadow 的采样器采一个开了 compare
//     的采样器是未定义用法，MoltenVK 上是 SPIR-V→MSL 转换失败＝黑窗。
//   * texture() 的第三个分量是**比较参考值**，返回值是「通过比较」的比例（1.0 = 亮），
//     不再是深度值。
//   * 每一次 tap 因为 LINEAR 本身就是硬件在 2x2 邻域上的加权比较。
//
// 采样器本身留在各自的 .frag 里声明（`layout(binding = 8) uniform sampler2DArrayShadow`），
// 由函数参数传进来：shader_descriptor_bindings_test 扫的是 .vert/.frag 里的
// `layout(binding = N)`，不递归进 include 目录，把声明搬进来会让那条护栏瞎掉。

// RN-38：这个函数回答的是**可见度**——1.0 = 太阳完全照到，0.0 = 完全挡住。
// 从前它返回的是「天光该乘多少」，把 0.35 那个全影系数烘在里面；那让「挡住了多少」
// 与「影子里该有多亮」变成同一个数，而后者其实是天空散射的份额（见 sunSkyFactor）。

#include "shadow_map.glsl"

// 1.0 个 NDC 深度单位等于多少格 = 正交的 far - near。偏置以「格」表达再除以它，
// 于是偏置是一个能和方块尺寸对照的量。注意这个换算在换成 orthoRH_ZO 前后**相同**：
// 旧的 NO 约定下着色器对 z 做的 `* 0.5 + 0.5` 恰好把 [-1,1] 还原成同一个 [0,1]，
// (2d-f-n)/(f-n) * 0.5 + 0.5 == (d-n)/(f-n) 是恒等式
const float kSunShadowDepthRangeBlocks = 319.9;

#include "sun_shadow_bias.glsl"

// 纹素怎么从矩阵推出来，见 shadow_map.glsl。

// 一级的投影结果落在它的框里吗。三个轴都要判：横向出框是「没有阴影图可查」，
// 深度出框是「这个点比光源还近或比远平面还远」，两者都只能按全亮处理。
bool sunShadowInsideCascade(vec3 shadowUv) {
    return shadowUv.x >= 0.0 && shadowUv.x <= 1.0 && shadowUv.y >= 0.0 && shadowUv.y <= 1.0 &&
           shadowUv.z >= 0.0 && shadowUv.z <= 1.0;
}

// RN-43：**一级之内**的可见度。从 sunShadowFactor 里整段搬出来，一个字没改——
// 过渡带要对同一个接收点跑两级，而把这段抄成两份正是这一整条链子上反复出问题的形状。
float sunShadowVisibilityInCascade(sampler2DArrayShadow shadowMap, sampler2DArray shadowDepth,
                                   mat4 lightViewProjFar, vec3 shadowUv, float layer,
                                   float texelBlocks, vec3 normal, float incidence,
                                   float thinPlane) {
    float texel = 1.0 / kSunShadowMapResolution;
    // RN-41：薄片植物再加一层，见 sunShadowThinPlaneBiasBlocks。普通面上 thinPlane 是 0，
    // 这一项整个消失
    float biasBlocks = kSunShadowDepthBiasBlocks +
        sunShadowThinPlaneBiasBlocks(thinPlane, incidence);
    float reference = shadowUv.z - biasBlocks / kSunShadowDepthRangeBlocks;

    // RN-34：先找遮挡物，再决定糊多宽（接触硬化）。
    //
    // binding 10 是同一张深度图的**非比较**采样器（NEAREST），读回的是深度值本身，
    // 不是比较结果——比较采样器做不到这件事，这就是它要单独一个绑定点的原因。
    //
    // 四个点摆在 ±1.5 纹素的方框上，比下面 PCF 的足迹（最多 ±1 纹素）大一圈。这不是
    // 随便取的：**搜索半径必须严格覆盖 PCF 的足迹**，否则「没找到遮挡物」这个提前返回
    // 就会漏掉本该投影的边缘像素，影子边上出现一圈缺口。
    float searchTexel = 1.5 * texel;
    float blockerDepthSum = 0.0;
    float blockerCount = 0.0;
    for (int i = 0; i < 4; ++i) {
        vec2 corner = vec2(i == 0 || i == 3 ? -1.0 : 1.0, i < 2 ? -1.0 : 1.0);
        float sampled = texture(shadowDepth, vec3(shadowUv.xy + corner * searchTexel, layer)).r;
        if (sampled < reference) {
            blockerDepthSum += sampled;
            blockerCount += 1.0;
        }
    }
    // 一个遮挡物都没有 ⇒ PCF 的每一个 tap 也必然通过 ⇒ 全亮。
    // 这条提前返回顺带**省掉**了绝大多数屏幕像素的四次 PCF：受光的地面是画面的大头。
    if (blockerCount == 0.0) {
        return 1.0;
    }
    float blockerDistance =
        (shadowUv.z - blockerDepthSum / blockerCount) * kSunShadowDepthRangeBlocks;
    float penumbraTexels = sunShadowPenumbraTexels(blockerDistance, texelBlocks);

    // 2x2 的 tap 网格，位置 ±penumbraTexels（上限 ±0.5 纹素，即从前的固定值）。加上每个
    // tap 自带的 2x2 硬件双线性比较，足迹半宽在 [0.5, 1.0] 纹素之间随遮挡距离变化。
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
    // 两级共用同一个旋转（SunShadowMap.cpp 只让横向半边长与吸附步长逐级不同），
    // 所以横向正交轴取哪一级的矩阵都一样。取远段那份，它与级别选择无关
    vec3 lightRight = normalize(vec3(lightViewProjFar[0][0], lightViewProjFar[1][0], lightViewProjFar[2][0]));
    vec3 lightUp = normalize(vec3(lightViewProjFar[0][1], lightViewProjFar[1][1], lightViewProjFar[2][1]));
    float normalRight = dot(normal, lightRight);
    float normalUp = dot(normal, lightUp);
    float normalSun = incidence;
    float lit = 0.0;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            float tapX = (float(x) - 0.5) * penumbraTexels * 2.0;
            float tapY = (float(y) - 0.5) * penumbraTexels * 2.0;
            float tapReference = reference + sunShadowTapOffsetBlocks(
                normalRight, normalUp, normalSun, tapX, tapY, texelBlocks) / kSunShadowDepthRangeBlocks;
            lit += texture(shadowMap,
                           vec4(shadowUv.xy + vec2(tapX, tapY) * texel, layer, tapReference));
        }
    }
    return lit * 0.25;
}

// `nearCascadeEnabled` 是 lightingSettings.z：近段那一层**这一帧有没有内容**。
// 玩家把级联关掉时层 0 那一步在编译期被剪，图里留着的是上一次的内容——不跳过它，
// 脚下会盖着一片陈旧的影子，而它随玩家走动而不动。
float sunShadowFactor(sampler2DArrayShadow shadowMap, sampler2DArray shadowDepth,
                      mat4 lightViewProjNear, mat4 lightViewProjFar,
                      vec3 worldPosition, vec3 normal, vec3 sunDirection,
                      float nearCascadeEnabled, vec2 weather, float thinPlane) {
    // 三个接收者统一：没有太阳直射的面不受此方向的遮挡影响，也无需 PCF。
    // 受光面的光照权重保持原样；合并 sky 通道仍包含环境天光，这是待拆分的近似。
    float incidence = dot(normal, normalize(sunDirection));
    if (incidence <= 0.0) {
        return 1.0;
    }
    // RN-36：云厚到直射不剩什么时，整套遮挡搜索加 PCF 都不必跑——暴雨里那是逐屏幕
    // 像素省下来的一整轮采样。算在最前面，投影之前
    if (sunShadowOvercast(weather.x, weather.y) >= kSunShadowInvisibleOvercast) {
        return 1.0;
    }
    // RN-35：选级。先试近段——它的纹素是远段的 1/8，能表达的边细八倍。
    //
    // ★ 法线抬升要用**被选中那一级**的纹素，而抬升又发生在投影之前，所以两级各投影
    // 一次是不可避的：拿远段的抬升去投近段，抬的量是 8 倍，影子会整片从脚下浮起来。
    // 代价是一次多余的 mat4 乘（近段没命中时才发生），换来的是两级各自自洽。
    bool tryNear = nearCascadeEnabled > 0.5;
    int cascade = tryNear ? 0 : 1;
    float texelBlocks = sunShadowTexelBlocksOf(tryNear ? lightViewProjNear : lightViewProjFar);
    vec3 offsetPosition = worldPosition + normal * sunShadowNormalOffsetBlocks(incidence, texelBlocks);
    vec4 lightPosition = (tryNear ? lightViewProjNear : lightViewProjFar) * vec4(offsetPosition, 1.0);
    vec3 projected = lightPosition.xyz / lightPosition.w;
    // xy 从 [-1,1] 重映射到 [0,1]；z **不**重映射——投影是 orthoRH_ZO，
    // 深度已经在 [0,1] 里了，再 * 0.5 + 0.5 会把它压进 [0.5,1]
    vec3 shadowUv = vec3(projected.xy * 0.5 + 0.5, projected.z);
    if (!sunShadowInsideCascade(shadowUv)) {
        if (!tryNear) {
            // 已经在远段了：最远那一级的框之外没有阴影图可查，一律按全亮
            return 1.0;
        }
        cascade = 1;
        texelBlocks = sunShadowTexelBlocksOf(lightViewProjFar);
        offsetPosition = worldPosition + normal * sunShadowNormalOffsetBlocks(incidence, texelBlocks);
        lightPosition = lightViewProjFar * vec4(offsetPosition, 1.0);
        projected = lightPosition.xyz / lightPosition.w;
        shadowUv = vec3(projected.xy * 0.5 + 0.5, projected.z);
        if (!sunShadowInsideCascade(shadowUv)) {
            return 1.0;
        }
    }
    float layer = float(cascade);
    // RN-49：**不做两级混合**。RN-43 在这里加过一条过渡带（近段框最外两成，两级各采
    // 一次再 mix），本节点把它整个删了——理由见 docs 的 RN-49，一句话是：
    //
    //   两级的分歧不只是「半影多宽」，还有「看没看见这个投射者」。玻璃边框宽 1/16 格，
    //   正好是远段一个纹素，远段常常整条漏掉它；mix 于是把远段的漏采混进近段的实影，
    //   实机上就是「阴影线条上出现光斑」。实测被抬亮 31%。
    //
    // 而按「影子是两级的并集」（谁看见算谁）改成 min 之后，带在实心投射者上**一个像素
    // 都不动**——RN-43 当初量到的「边从 2 像素展成 6~10 像素」全部来自那次变亮。
    // 于是这条带买不到任何东西，却要在一圈里付第二遍遮挡搜索加 PCF。删掉。
    //
    // 接缝本身改由 RN-47 的「近段距离」那一档处理：把它推到 16 或 24 格。
    return sunShadowVisibilityInCascade(shadowMap, shadowDepth, lightViewProjFar, shadowUv, layer,
                                        texelBlocks, normal, incidence, thinPlane);
}
