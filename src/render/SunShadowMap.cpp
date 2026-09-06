#include "render/SunShadowMap.hpp"

#include "render/Frustum.hpp"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace mc::render {
namespace {

// 光源朝向的 up。太阳永远不会竖直：DayNightCycle 的轨道带 0.28 的 z 倾角，
// |sunDirection.y| 的上界是 1/sqrt(1 + 0.28^2) = 0.963，lookAt 因此不会退化。
// sun_shadow_map_test 逐 tick 钉住这条上界——谁把轨道改成过天顶，那里先炸，
// 而不是在 mac 上炸成一屏 NaN。
constexpr glm::vec3 kLightUp{0.0F, 1.0F, 0.0F};

[[nodiscard]] float snapToTexelGrid(float value) {
    return std::round(value / kSunShadowTexelSize) * kSunShadowTexelSize;
}

} // namespace

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

} // namespace mc::render
