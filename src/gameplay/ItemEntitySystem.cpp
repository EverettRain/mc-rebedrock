#include "gameplay/ItemEntitySystem.hpp"

#include "world/Block.hpp"
#include "world/World.hpp"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace mc::gameplay {

void ItemEntitySystem::spawn(
    glm::vec3 position,
    ItemStack stack,
    glm::vec3 initialVelocity) {
    if (!stack.empty()) {
        constexpr float goldenAngle = 2.39996323F;
        constexpr float fullTurn = 6.28318531F;
        entities_.push_back({
            position, position, initialVelocity, stack, 0, nextVisualPhase_});
        nextVisualPhase_ = std::fmod(nextVisualPhase_ + goldenAngle, fullTurn);
    }
}

std::size_t ItemEntitySystem::tick(
    const world::World& world,
    glm::vec3 playerPosition,
    Inventory& inventory) {
    std::size_t pickedUpStacks = 0U;
    for (auto& entity : entities_) {
        entity.previousPosition = entity.position;
        ++entity.ageTicks;
        entity.velocity.y -= 0.04F;
        entity.velocity *= 0.98F;
        glm::vec3 next = entity.position + entity.velocity;
        const int blockX = static_cast<int>(std::floor(next.x));
        const int blockZ = static_cast<int>(std::floor(next.z));
        const int belowY = static_cast<int>(std::floor(next.y - 0.13F));
        if (world::hasCollision(world.block(blockX, belowY, blockZ)) &&
            entity.velocity.y <= 0.0F) {
            next.y = static_cast<float>(belowY + 1) + 0.13F;
            entity.velocity.y = 0.0F;
            entity.velocity.x *= 0.70F;
            entity.velocity.z *= 0.70F;
        }
        if (world::isFluid(world.block(blockX, belowY, blockZ))) {
            entity.velocity *= 0.80F;
            entity.velocity.y += 0.03F;
        }
        entity.position = next;

        if (entity.ageTicks >= 10U &&
            glm::distance(entity.position, playerPosition) <= 1.5F) {
            const auto previousCount = entity.stack.count;
            inventory.add(entity.stack);
            if (entity.stack.count != previousCount) {
                ++pickedUpStacks;
            }
        }
    }
    std::erase_if(entities_, [](const ItemEntity& entity) {
        return entity.stack.empty() || entity.ageTicks > 6'000U || entity.position.y < -8.0F;
    });
    return pickedUpStacks;
}

} // namespace mc::gameplay
