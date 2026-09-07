#include "render/SunShadowMap.hpp"

#include "render/Frustum.hpp"
#include "render/vulkan/HudTypes.hpp"
#include "world/DayNightCycle.hpp"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace mc::render {
namespace {

// 光源朝向的 up：太阳轨道平面的法线，和轨道倾角同源（RN-24）。
//
// 从前是世界的 (0,1,0)。它不会退化——DayNightCycle 的轨道带 0.28 的 z 倾角，
// |sunDirection.y| 的上界是 1/sqrt(1 + 0.28^2) = 0.963——但 |cross(-sun, up)| 在正午只剩
// 0.2696，归一化在那里放大误差；更要紧的是它让光源基绕光轴多出一个自旋，正午处整体
// 转速 0.05357°/tick 是太阳自身 0.01444°/tick 的 3.71 倍，多出来的全是自旋。自旋是唯一
// 在阴影图平面内转动纹素网格的分量，物理上不改变任何一片阴影。
//
// 太阳整天严格落在轨道平面里（dot(sunDirection, kSunOrbitNormal) 全天 < 1e-16），所以拿
// 法线当 up 时：cross(-sun, up) 的模恒为 1（不会退化），基的 y 轴恒等于法线本身，绕光轴
// 的自旋恒为 0。sun_shadow_map_test 同时钉住这两条——谁把轨道改成过天顶或改成非平面，
// 那里先炸，而不是在 mac 上炸成一屏 NaN 或者悄悄把 RN-24 的稳定性还回去。
constexpr glm::vec3 kLightUp = world::DayNightCycle::kSunOrbitNormal;

[[nodiscard]] float snapToTexelGrid(float value) {
    return std::round(value / kSunShadowTexelSize) * kSunShadowTexelSize;
}

} // namespace

double sunShadowSunTick(double dayTimeTicks) {
    // std::floor 而不是截断：dayTimeTicks 今天恒为非负整数，但截断会在将来某个负值上
    // 把步长边界折向零，让同一个步长在原点两侧长度不同。
    return std::floor(dayTimeTicks / kSunShadowAngleStepTicks) * kSunShadowAngleStepTicks;
}

glm::mat4 sunShadowLightViewProj(const glm::vec3& sunDirection, const glm::vec3& eye) {
    const glm::vec3 sun = glm::normalize(sunDirection);
    // 只取旋转：lookAt 的朝向是 normalize(target - position) = -sun，与视点无关。
    // 把旋转与平移拆开，量化才有地方落——平移分量正是要被钉到纹素网格上的那个量。
    const glm::mat4 lightRotation = glm::lookAt(glm::vec3{0.0F}, -sun, kLightUp);
    const glm::vec3 center = eye + sun * kSunShadowEyeDistance;
    glm::vec3 centerInLight{lightRotation * glm::vec4{center, 1.0F}};
    // 只量化横向两轴。深度轴不必量化：写入端与采样端用的是同一个矩阵，深度原点的
    // 连续漂移在比较里两边抵消，只有横向的采样相位漂移会表现成阴影边爬行。
    centerInLight.x = snapToTexelGrid(centerInLight.x);
    centerInLight.y = snapToTexelGrid(centerInLight.y);
    // lightView: P -> lightRotation * P - centerInLight。未量化时它与
    // glm::lookAt(center, center - sun * 2 * kSunShadowEyeDistance, up) 逐字相同。
    const glm::mat4 lightView =
        glm::translate(glm::mat4{1.0F}, -centerInLight) * lightRotation;
    const glm::mat4 lightProj =
        glm::orthoRH_ZO(-kSunShadowOrthoHalfExtent, kSunShadowOrthoHalfExtent,
                        -kSunShadowOrthoHalfExtent, kSunShadowOrthoHalfExtent,
                        kSunShadowNearPlane, kSunShadowFarPlane);
    return lightProj * lightView;
}

float sunShadowCasterDepth(const glm::vec3& sunDirection, const Aabb& bounds) {
    // 光行进的方向，深度沿它增大
    const glm::vec3 forward = -glm::normalize(sunDirection);
    const glm::vec3 center = (bounds.minimum + bounds.maximum) * 0.5F;
    const glm::vec3 extent = (bounds.maximum - bounds.minimum) * 0.5F;
    // 线性函数在轴对齐盒上的最小值有闭式：中心的投影减去半长在各轴上的绝对贡献。
    // 这就是盒上沿光方向最靠前那个角，不必枚举八个角。
    return glm::dot(center, forward) -
           (std::abs(forward.x) * extent.x + std::abs(forward.y) * extent.y +
            std::abs(forward.z) * extent.z);
}

void selectSunShadowCasters(const glm::mat4& lightViewProj, const glm::vec3& sunDirection,
                            std::span<const Aabb> bounds, std::vector<std::size_t>& selected) {
    selected.clear();
    const Frustum lightFrustum(lightViewProj);
    for (std::size_t index = 0; index < bounds.size(); ++index) {
        if (lightFrustum.intersects(bounds[index])) {
            selected.push_back(index);
        }
    }
    if (selected.size() <= kMaxSunShadowCasters) {
        return;
    }
    std::ranges::sort(selected, [&](std::size_t first, std::size_t second) {
        return sunShadowCasterDepth(sunDirection, bounds[first]) <
               sunShadowCasterDepth(sunDirection, bounds[second]);
    });
    selected.resize(kMaxSunShadowCasters);
}

Aabb sunShadowItemDrawBounds(const ItemPush& push) {
    glm::mat4 transform = push.viewModelTransform;
    glm::vec3 size{push.dimensions};
    if (push.data.x == kItemModeGeneratedItem) {
        // item_entity.vert 的 local z 是 ±0.03125，模型矩阵携带掉落物的 0.3 倍缩放。
        size = {1.0F, 1.0F, 0.0625F};
    } else if (push.data.x == kItemModeBlockCube || push.data.x == kItemModeBlockItemDropped) {
        transform = glm::rotate(glm::translate(glm::mat4{1.0F}, glm::vec3{push.positionSize}),
            push.textureLayersRotation.w, glm::vec3{0, 1, 0});
        if (glm::length(size) <= 0.0001F) size = glm::vec3{push.positionSize.w};
    }
    return sunShadowTransformedBounds(transform, size);
}

Aabb sunShadowTransformedBounds(const glm::mat4& transform, glm::vec3 size) {
    const glm::vec3 center{transform[3]};
    const glm::vec3 half = glm::abs(size) * 0.5F;
    const glm::vec3 extent = glm::abs(glm::vec3{transform[0]}) * half.x +
        glm::abs(glm::vec3{transform[1]}) * half.y + glm::abs(glm::vec3{transform[2]}) * half.z;
    return {center - extent, center + extent};
}

void selectSunShadowEntityCasters(const glm::mat4& lightViewProj, const glm::vec3& sunDirection,
    std::span<const SunShadowEntityCaster> casters, std::vector<std::size_t>& selected) {
    selected.clear();
    const Frustum frustum(lightViewProj);
    for (std::size_t i = 0; i < casters.size(); ++i) {
        const auto& caster = casters[i];
        if (castsSunShadow(caster.kind) && caster.drawCount != 0 && frustum.intersects(caster.bounds))
            selected.push_back(i);
    }
    if (selected.size() <= kMaxSunShadowEntityCasters) return;
    std::ranges::sort(selected, [&](std::size_t a, std::size_t b) {
        const bool playerA = casters[a].kind == ShadowEntityKind::Player;
        const bool playerB = casters[b].kind == ShadowEntityKind::Player;
        if (playerA != playerB) return playerA;
        const float depthA = sunShadowCasterDepth(sunDirection, casters[a].bounds);
        const float depthB = sunShadowCasterDepth(sunDirection, casters[b].bounds);
        return depthA == depthB ? a < b : depthA < depthB;
    });
    selected.resize(kMaxSunShadowEntityCasters);
}

void selectSunShadowSceneCasters(const glm::mat4& lightViewProj, const glm::vec3& sunDirection,
    std::span<const Aabb> terrain, std::span<const SunShadowEntityCaster> entities,
    std::vector<std::size_t>& terrainSelected, std::vector<std::size_t>& entitySelected) {
    selectSunShadowCasters(lightViewProj, sunDirection, terrain, terrainSelected);
    selectSunShadowEntityCasters(lightViewProj, sunDirection, entities, entitySelected);
}

} // namespace mc::render
