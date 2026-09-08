#pragma once

// 太阳阴影图的几何单一源（RN-11）。
//
// 这里放的是「阴影图长什么样」的全部答案：正交框多大、深度范围多少、多少纹素、
// 每帧最多画几个投射者、光源矩阵怎么构造、投射者按什么排序。它们从前散在
// WorldRenderer::updateShadowMatrix、WorldRenderer::recordShadow 与
// VulkanRenderer::createShadowResources 三处，其中 2048 这个分辨率在
// createShadowResources 里是个手写字面量，而 updateShadowMatrix 里算不出它——
// 于是「一个纹素是世界里的多长」这个 texel snapping 必需的量根本无处可算。
//
// 不依赖 Vulkan：核心几何使用 glm 与 Aabb，实体入口读取既有的纯数据 ItemPush。
// headless 测试直接调用生产选择与包围盒计算。

#include "render/MeshData.hpp"  // Aabb

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mc::render {

// RN-35：级联。两级，各自一张正交框、各自一层深度图、各自独立吸附。
//
// 走级联而不是射影畸变（RN-34 §5 登记的那条）的理由是两条几何事实：只有射影变换能被
// 光栅化正确插值（分段线性映射会让跨接缝的三角形画成直线，实算是几十个纹素的错位），
// 而射影函数做不出奇对称的加密——单张图上因此做不出「以玩家为中心的径向加密」。
// 级联绕开了这两条，还额外保住了 RN-24：**每一级仍是正交框，可以各自吸附**，
// 稳定性一点都不用让，也不依赖 TAA。
inline constexpr std::size_t kSunShadowCascadeCount = 2;

// 每级正交框的半边长（格）。近段 8 ⇒ 全宽 16 格，只覆盖玩家周围——那正是「玩家附近」；
// 远段 64 ⇒ 全宽 128 格，与 RN-11 以来一致。
// 框外一律无阴影，最远那一级的框边是一条随视点与太阳移动的硬边界（RN-11 C(c) 的账）。
inline constexpr std::array<float, kSunShadowCascadeCount> kSunShadowOrthoHalfExtents{8.0F, 64.0F};

// 最远那一级的半边长。光锥深度、实体剔除这些「与级别无关」的量取它。
inline constexpr float kSunShadowOrthoHalfExtent =
    kSunShadowOrthoHalfExtents[kSunShadowCascadeCount - 1];

// 光源「相机」放在视点沿太阳方向 96 格处，正交深度范围 0.1..320 格。
inline constexpr float kSunShadowEyeDistance = 96.0F;
inline constexpr float kSunShadowNearPlane = 0.1F;
inline constexpr float kSunShadowFarPlane = 320.0F;

// 深度图里 1.0 个 NDC 单位等于多少格。着色器的偏置以「格」表达再除以它，
// 偏置因此是一个能和方块尺寸对照的量，而不是一个无量纲的魔数。
inline constexpr float kSunShadowDepthRangeBlocks = kSunShadowFarPlane - kSunShadowNearPlane;

// 阴影图边长（纹素）。OffscreenTarget 的创建参数从这里取，不再手写。
inline constexpr std::uint32_t kSunShadowMapResolution = 2048U;

// 阴影矩阵采用的太阳角度步长（tick）。RN-24。
//
// 太阳方向本来就只在 tick 边界上变（dayTimeTicks 是整数，20 Hz），可那仍然是每秒 20 次
// 光源基旋转，而**每一次旋转都会重新随机化阴影图的采样相位**：
//
//   固定世界点 P 的纹素坐标 = (R·P - snap(R·eye)) / texel，而 snap() 的输出恒为 texel 的
//   整数倍，所以采样相位 = frac(R·P / texel)。整纹素的差异是**逐位不可见**的——写入端与
//   采样端是同一个矩阵，整纹素平移把 9 个 PCF tap 平移到同样的深度上（实测：给
//   centerInLight.x 加 1/2/3/4 个纹素，三个机位、阈值 >0，变化像素为 0）。可见的只有那个
//   分数相位，它每 tick 变化 |ΔR·P| / texel，**正比于 P 的世界绝对坐标**：离原点 70 格时
//   约 0.3 纹素/tick，离原点几千格时每 tick 完全随机。于是阴影边的锯齿逐 tick 重排——
//   玩家看到的「边缘随时间变化并出现锯齿滑动」。时间一暂停 ΔR = 0，相位恒定，画面逐位
//   稳定，这正是用户观察到的那半句。
//
// 把角度量化到 kSunShadowAngleStepTicks 之后，步内 R 逐位不变：静止视点下整个矩阵逐位
// 相同，移动视点下 snap 只给整纹素的变化——两者都已实测不可见。代价是每步边界上一次
// 重相位，实测等效相位约 0.15 纹素。
//
// 8 tick = 0.4 秒，且 24000 % 8 == 0，一天正好 3000 步。选 8 而不是更大：跨步时**物理**
// 影长也一起跳，8 tick 对 2 格高的生物只有 0.14 纹素、对 10 格高、太阳仰角 30° 的建筑
// 是 1.3 纹素，都看不出来；16/20 tick 在高塔配低日角下会有可见的影尖跳步。若真机上仍
// 看得到跨步跳变，备选是 4（RN-24 落地记录里有整张步长对照表，不必重做那轮测量）。
inline constexpr double kSunShadowAngleStepTicks = 8.0;
static_assert(24'000.0 / kSunShadowAngleStepTicks ==
              static_cast<double>(static_cast<int>(24'000.0 / kSunShadowAngleStepTicks)),
              "角度步长必须整除一天的 tick 数，否则跨日边界上会多出一个短步");

// 阴影矩阵该用哪个 tick 的太阳。着色用的太阳**不**走这里：uniform.sunDirection 仍取真实
// tick，量化它会让面亮度每 0.4 秒跳一档。两者最大错配是 8 * 0.0156° = 0.12°，对着色器里
// 那道 N·L 门只影响掠射方向上 0.12° 宽的一条带，不构成可见差异。这是**有意**的不一致。
[[nodiscard]] double sunShadowSunTick(double dayTimeTicks);

// 一个纹素在世界里的边长（格），逐级不同。texel snapping 量化到它，PCF 的步长是它的倒数。
//
// ★ 着色器里的偏置、法线抬升、逐 tap 平面修正、半影半径全都以「纹素」表达，而纹素的
// 世界尺寸现在是级别的函数——那三个函数因此从读全局常量改成收一个参数（RN-35 §1.4）。
// 漏改的症状是近段的偏置按远段的纹素抬，也就是抬高 8 倍：影子整片从投射者脚下浮起来。
[[nodiscard]] constexpr float sunShadowTexelSize(std::size_t cascade) {
    return 2.0F * kSunShadowOrthoHalfExtents[cascade] /
           static_cast<float>(kSunShadowMapResolution);
}
static_assert(sunShadowTexelSize(0) == 0.0078125F);
static_assert(sunShadowTexelSize(1) == 0.0625F);
// 近段的纹素正好是远段的 1/8。这个比值在着色器与测试里都被当成常量读，写成断言
// 而不是注释——改了框宽却忘了改那一侧的人，在这里先炸
static_assert(sunShadowTexelSize(1) == sunShadowTexelSize(0) * 8.0F);

// 最远那一级的纹素。与级别无关的旧调用点（阴影调试叠加层等）取它。
inline constexpr float kSunShadowTexelSize = sunShadowTexelSize(kSunShadowCascadeCount - 1);

// 预通道每帧最多画多少个 section。视点飞高或光锥覆盖密集区域时候选能涨到数千，
// 每帧全部重画正是那种可能把设备推向丢失的重负载帧。
inline constexpr std::size_t kMaxSunShadowCasters = 512;

// RN-11b：实体按整只计数，独立于地形的 512 个 section。本地玩家在光锥内时预留一个名额。
inline constexpr std::size_t kMaxSunShadowEntityCasters = 512;
enum class ShadowEntityKind : std::uint8_t { Player, Creature, Item, FallingBlock, Decal, Orb };
struct SunShadowEntityCaster {
    ShadowEntityKind kind;
    Aabb bounds;
    std::size_t firstDraw;
    std::size_t drawCount = 0;
};
[[nodiscard]] constexpr bool castsSunShadow(ShadowEntityKind kind) {
    return kind == ShadowEntityKind::Player || kind == ShadowEntityKind::Creature ||
           kind == ShadowEntityKind::Item || kind == ShadowEntityKind::FallingBlock;
}
[[nodiscard]] constexpr bool drawEntityShadowDecal(bool sunShadows) { return !sunShadows; }
struct ItemPush;
// 与实体顶点程序的世界几何模式对应，投影剔除也必须保留图标薄片的真实厚度。
[[nodiscard]] Aabb sunShadowItemDrawBounds(const ItemPush& push);
// 仿射变换后的盒子，包含旋转、非等比缩放、镜像与 inflate，不拿物理碰撞盒代替渲染几何。
[[nodiscard]] Aabb sunShadowTransformedBounds(const glm::mat4& transform, glm::vec3 size);
void selectSunShadowEntityCasters(const glm::mat4& lightViewProj, const glm::vec3& sunDirection,
    std::span<const SunShadowEntityCaster> casters, std::vector<std::size_t>& selected);

// 光源的视图投影矩阵。
//
// 两件事和从前不同：
//
// 1. 投影用 glm::orthoRH_ZO，不是 glm::ortho。全仓没有定义
//    GLM_FORCE_DEPTH_ZERO_TO_ONE，所以 glm::ortho 派发到 orthoRH_NO，深度落在
//    [-1, 1]；而 Vulkan 的裁剪是 0 <= z_clip <= w，depthClampEnable 又是 VK_FALSE。
//    结果是 z_ndc < 0 的几何被硬件裁掉，也就是光源空间深度 d < (near+far)/2 = 160.05
//    格的一切——光锥里绝大部分几何从来没写进过阴影图，只有离光源 160 格以外的那一片
//    能通过。主相机一直是显式的 perspectiveRH_ZO（PerspectiveCamera.cpp），这里是
//    唯一漏掉的一处。局部改这一个调用而不是全局定义宏：全局宏会改变所有矩阵，而主
//    相机已经自己表达了意图。
//
//    接收端不受影响：着色器从前对 z 做的 `* 0.5 + 0.5` 恰好把 NO 的 [-1,1] 还原成
//    和 ZO 相同的 [0,1]，(2d-f-n)/(f-n) * 0.5 + 0.5 == (d-n)/(f-n) 是恒等式。所以
//    换约定之后着色器必须**去掉 z 上的那次重映射**（xy 仍然要），去掉之后
//    shadowUv.z 与从前逐位相同。漏改就是把一个缺陷换成另一个。
//
// 2. 光源正交框的平移分量量化到纹素网格（texel snapping）。从前它逐帧跟着视点这个
//    连续浮点量平移，每一帧的纹素因此落在不同的世界位置上，被量化的阴影边界逐帧改变
//    采样相位，玩家一平移阴影边就沿地面爬行。量化之后，对任何固定的世界点，它在阴影
//    图里的纹素坐标在视点平移下只会整纹素跳变——而整纹素跳变是**逐位不可见**的，写入端
//    与采样端是同一个矩阵。
//
//    注意这条只在光源基**不转**时买到稳定：snap() 只清掉相位的整数部分，太阳一转，
//    frac(R·P / texel) 就重新随机化。那是 kSunShadowAngleStepTicks 的账（RN-24）。
//
// 3. 光源的 up 是太阳轨道平面的法线（world::DayNightCycle::kSunOrbitNormal），不是世界的
//    (0,1,0)。用 (0,1,0) 时光源基除了跟着太阳转，还绕光轴多出一个自旋，正午附近达到太阳
//    自身转速的 3.7 倍；那个自旋物理上什么也不做，却是唯一在阴影图**平面内**转动纹素
//    网格的分量，于是角度一量化，跨步跳变就整整大出 2.6 到 4 倍。改用轨道法线后自旋恒为
//    0，且 |cross(-sun, up)| 从最小 0.2696 变成恒等 1.0，lookAt 的 right 轴不再在正午附近
//    放大归一化误差。
//
// `eye` 必须是**渲染视点**（RenderEye::position），不是相机对象的位置：第三人称把
// 渲染眼点沿视线拉后 4 格，用相机位置会让光锥中心停在玩家身上而不是画面中心。
// RN-35：`cascade` 选哪一级的正交框与哪一个吸附步长。两级用的是**同一个**旋转与
// 同一个深度范围，只有横向半边长与吸附步长不同——于是「近段的影子和远段的影子是同一个
// 太阳投的」这件事是结构性的，不是靠两处常量碰巧相等。
[[nodiscard]] glm::mat4 sunShadowLightViewProj(const glm::vec3& sunDirection, const glm::vec3& eye,
                                               std::size_t cascade = kSunShadowCascadeCount - 1);

// 投射者的排序键：包围盒沿光行进方向最靠前那个角的光源空间深度，越小越靠近光源。
//
// 关键性质是**它不含视点**。从前的键是 section 中心到相机的距离平方，相机一动整张表
// 重排，512 的截断线在表上滑动，整块 16x16 的 section 成批进出投射者集合——某个
// section 掉出去，它在地面上投的那片阴影就整片消失，于是地面上出现以区块为粒度、随
// 移动扫过的亮斑。换成光源空间深度之后，排序在相机平移下逐字不变；再叠加上面的 texel
// snapping 让光锥本身在亚纹素平移下逐字不变，亚纹素平移下入选集合因此逐位相同。
//
// 取「近点」而不是包围盒中心：本作的 section 包围盒是整段 16^3（ChunkMesher.cpp 的
// buildSectionImpl），尺寸一致，两者只差一个常量、排序等价。写近点是因为它才是物理上
// 正确的陈述——谁先挡住光——包围盒将来一旦收紧就自动仍然正确。
[[nodiscard]] float sunShadowCasterDepth(const glm::vec3& sunDirection, const Aabb& bounds);

// 光锥剔除 + 按光源空间深度截断到 kMaxSunShadowCasters，结果是 `bounds` 里的下标。
// `selected` 先被清空再填充，调用方复用同一个 vector 就不会逐帧分配。
void selectSunShadowCasters(const glm::mat4& lightViewProj, const glm::vec3& sunDirection,
                            std::span<const Aabb> bounds, std::vector<std::size_t>& selected);

// 同时产出两个互不挤占的集合，生产绘制与 headless 场景测试共用这一入口。
void selectSunShadowSceneCasters(const glm::mat4& lightViewProj, const glm::vec3& sunDirection,
    std::span<const Aabb> terrain, std::span<const SunShadowEntityCaster> entities,
    std::vector<std::size_t>& terrainSelected, std::vector<std::size_t>& entitySelected);

} // namespace mc::render
