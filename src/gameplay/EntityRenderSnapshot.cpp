#include "gameplay/EntityRenderSnapshot.hpp"

#include "gameplay/EntitySystem.hpp"

namespace mc::gameplay {

void EntityRenderSnapshot::capture(const std::vector<SimpleEntity>& creatures,
                                   const std::vector<ItemEntity>& items,
                                   const std::vector<FallingBlockEntity>& fallingBlocks) {
    items_ = items;
    fallingBlocks_ = fallingBlocks;
    // Reused rather than reallocated: this runs every tick, and the population
    // is stable from one tick to the next.
    entities_.clear();
    entities_.reserve(creatures.size());
    for (const auto& entity : creatures) {
        entities_.push_back(EntityRenderState{
            entity.type,
            entity.id,
            entity.position,
            entity.previousPosition,
            entity.yaw,
            entity.previousYaw,
            entity.walkDistance,
            entity.previousWalkDistance,
            entity.damage.hurtTicks,
            entity.damage.deathTicks,
        });
    }
}

} // namespace mc::gameplay
