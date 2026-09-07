#pragma once

#include "render/SunShadowMap.hpp"
#include "render/vulkan/HudTypes.hpp"
#include <glm/common.hpp>
#include <vector>

namespace mc::render {
// 同一帧只生成一次，颜色和太阳深度都读这些 ItemPush。所有矩阵在这里保持世界空间；
// 只有颜色提交时才给 GeneratedItem 乘相机 view（该模式还被第一人称手持物使用）。
struct EntityRenderDraw {
    ItemPush push;
    std::uint32_t vertexCount;
    std::uint32_t firstVertex;
};
class EntityRenderDraws {
public:
    std::vector<EntityRenderDraw> draws;
    std::vector<SunShadowEntityCaster> casters;
    void clear() { draws.clear(); casters.clear(); }
    void begin(ShadowEntityKind kind) { casters.push_back({kind, {}, draws.size(), 0}); }
    void append(const ItemPush& push, std::uint32_t count, std::uint32_t first = 0) {
        auto& caster = casters.back();
        if (castsSunShadow(caster.kind)) {
            const Aabb bounds = sunShadowItemDrawBounds(push);
            if (caster.drawCount == 0) caster.bounds = bounds;
            else {
                caster.bounds.minimum = glm::min(caster.bounds.minimum, bounds.minimum);
                caster.bounds.maximum = glm::max(caster.bounds.maximum, bounds.maximum);
            }
        }
        draws.push_back({push, count, first});
        ++caster.drawCount;
    }
};
} // namespace mc::render
