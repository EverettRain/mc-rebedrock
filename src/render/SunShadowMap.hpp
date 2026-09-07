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

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mc::render {

// 正交框的半边长（格）。横截面因此是 128×128 格。
// 框外一律无阴影，框边是一条随视点与太阳移动的硬边界——那是 CSM（RN-11 C(c)）的账。
inline constexpr float kSunShadowOrthoHalfExtent = 64.0F;

// 光源「相机」放在视点沿太阳方向 96 格处，正交深度范围 0.1..320 格。
inline constexpr float kSunShadowEyeDistance = 96.0F;
inline constexpr float kSunShadowNearPlane = 0.1F;
inline constexpr float kSunShadowFarPlane = 320.0F;

// 深度图里 1.0 个 NDC 单位等于多少格。着色器的偏置以「格」表达再除以它，
// 偏置因此是一个能和方块尺寸对照的量，而不是一个无量纲的魔数。
inline constexpr float kSunShadowDepthRangeBlocks = kSunShadowFarPlane - kSunShadowNearPlane;

// 阴影图边长（纹素）。OffscreenTarget 的创建参数从这里取，不再手写。
inline constexpr std::uint32_t kSunShadowMapResolution = 2048U;

// 一个纹素在世界里的边长（格）。texel snapping 量化到它，PCF 的步长是它的倒数。
inline constexpr float kSunShadowTexelSize =
    2.0F * kSunShadowOrthoHalfExtent / static_cast<float>(kSunShadowMapResolution);
static_assert(kSunShadowTexelSize == 0.0625F);

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
//    图里的纹素坐标在视点平移下只会整纹素跳变。
//
// `eye` 必须是**渲染视点**（RenderEye::position），不是相机对象的位置：第三人称把
// 渲染眼点沿视线拉后 4 格，用相机位置会让光锥中心停在玩家身上而不是画面中心。
[[nodiscard]] glm::mat4 sunShadowLightViewProj(const glm::vec3& sunDirection,
                                               const glm::vec3& eye);

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
