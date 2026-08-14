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
        sections_[entitySectionOf(position)].push_back(entities_.size() - 1U);
        nextVisualPhase_ = std::fmod(nextVisualPhase_ + goldenAngle, fullTurn);
    }
}

void ItemEntitySystem::restore(glm::vec3 position, ItemStack stack, glm::vec3 velocity,
                               unsigned int ageTicks) {
    if (stack.empty()) {
        return;
    }
    constexpr float goldenAngle = 2.39996323F;
    constexpr float fullTurn = 6.28318531F;
    entities_.push_back({position, position, velocity, std::move(stack), ageTicks,
                         nextVisualPhase_});
    sections_[entitySectionOf(position)].push_back(entities_.size() - 1U);
    nextVisualPhase_ = std::fmod(nextVisualPhase_ + goldenAngle, fullTurn);
}

std::size_t ItemEntitySystem::tick(
    const world::World& world,
    glm::vec3 playerPosition,
    Inventory& inventory) {
    std::size_t pickedUpStacks = 0U;
    // A dropped item occupies a small 0.25³ box, the same as vanilla
    // ItemEntity's hitbox. Collision is resolved per axis against the block
    // grid, so an item lands on floors, leans on walls and never passes
    // through one.
    constexpr float kHalfExtent = 0.125F;
    const auto collidesAt = [&](glm::vec3 centre) {
        const glm::vec3 minimum = centre - glm::vec3{kHalfExtent};
        const glm::vec3 maximum = centre + glm::vec3{kHalfExtent};
        const int minX = static_cast<int>(std::floor(minimum.x + 1e-4F));
        const int maxX = static_cast<int>(std::floor(maximum.x - 1e-4F));
        const int minY = static_cast<int>(std::floor(minimum.y + 1e-4F));
        const int maxY = static_cast<int>(std::floor(maximum.y - 1e-4F));
        const int minZ = static_cast<int>(std::floor(minimum.z + 1e-4F));
        const int maxZ = static_cast<int>(std::floor(maximum.z - 1e-4F));
        for (int y = minY; y <= maxY; ++y) {
            if (y < 0 || y >= world::kWorldHeight) continue;
            for (int z = minZ; z <= maxZ; ++z) {
                for (int x = minX; x <= maxX; ++x) {
                    if (world::hasCollision(world.block(x, y, z))) {
                        return true;
                    }
                }
            }
        }
        return false;
    };
    // Whether the inventory has any slot that could take the stack, so a full
    // backpack does not magnet an item forever (vanilla's empty-slot gate).
    const auto hasRoomFor = [&](const ItemStack& stack) {
        if (stack.empty()) {
            return false;
        }
        const auto maximum = itemMaximumStackSize(stack);
        for (std::size_t index = 0; index < Inventory::kSlotCount; ++index) {
            const auto& slot = inventory.slot(index);
            if (slot.empty() || (sameItem(slot, stack) && slot.count < maximum)) {
                return true;
            }
        }
        return false;
    };

    // Player magnet, resolved through the spatial hash: only drops in the
    // player's section neighbourhood can sit within the 1.5-block reach, so
    // query those buckets instead of sweeping every drop. It runs before the
    // movement loop against the previous tick's positions; the loop adds one
    // to ageTicks, so the pre-increment gate of 9 matches the inline check's
    // age >= 10.
    {
        const EntitySection playerSection = entitySectionOf(playerPosition);
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dz = -1; dz <= 1; ++dz) {
                    const auto neighbor = sections_.find(EntitySection{
                        playerSection.chunkX + dx, playerSection.sectionY + dy,
                        playerSection.chunkZ + dz});
                    if (neighbor == sections_.end()) {
                        continue;
                    }
                    for (const std::size_t index : neighbor->second) {
                        auto& entity = entities_[index];
                        if (entity.stack.empty() || entity.ageTicks < 9U) {
                            continue;
                        }
                        const glm::vec3 toPlayer = playerPosition - entity.position;
                        const float distance = glm::length(toPlayer);
                        if (distance <= 1.5F && distance > 1e-3F && hasRoomFor(entity.stack)) {
                            entity.velocity += glm::normalize(toPlayer) * 0.05F;
                            const float speed = glm::length(entity.velocity);
                            constexpr float kMagnetMaxSpeed = 0.18F;
                            if (speed > kMagnetMaxSpeed) {
                                entity.velocity *= kMagnetMaxSpeed / speed;
                            }
                        }
                    }
                }
            }
        }
    }

    for (auto& entity : entities_) {
        entity.previousPosition = entity.position;
        ++entity.ageTicks;
        entity.velocity.y -= 0.04F;
        entity.velocity *= 0.98F;
        // ItemEntity#tick: a drop resting on the floor decelerates at the
        // surface's slipperiness (0.6 for most blocks) rather than the 0.98
        // air drag, so it does not keep sliding far after it lands.
        if (collidesAt(entity.position - glm::vec3{0.0F, 0.02F, 0.0F})) {
            entity.velocity.x *= 0.6F * 0.98F;
            entity.velocity.z *= 0.6F * 0.98F;
        }

        // The player magnet ran in the bucket pass above; only the physics that
        // every drop must perform stays here.

        // ItemEntity#applyBuoyancy: water slows and lifts the drop.
        const auto foot = entity.position - glm::vec3{0.0F, 0.2F, 0.0F};
        if (world::isFluid(world.block(
                static_cast<int>(std::floor(entity.position.x)),
                static_cast<int>(std::floor(foot.y)),
                static_cast<int>(std::floor(entity.position.z))))) {
            entity.velocity *= 0.80F;
            entity.velocity.y += 0.03F;
        }

        // Entity#move: resolve Y first, then the horizontal axes, so an item
        // lands flush on a floor and slides along a wall instead of piling in.
        // When a move would collide, bisect toward the contact so even a fast
        // drop ends resting exactly on the surface (the mobs' moveWithCollisions
        // walks the same way).
        const auto moveAxis = [&](int axis, float amount) {
            if (std::abs(amount) < 1e-4F) {
                return;
            }
            glm::vec3 candidate = entity.position;
            candidate[axis] += amount;
            if (!collidesAt(candidate)) {
                entity.position = candidate;
                return;
            }
            float safe = 0.0F;
            float blocked = 1.0F;
            for (int iteration = 0; iteration < 12; ++iteration) {
                const float middle = (safe + blocked) * 0.5F;
                candidate = entity.position;
                candidate[axis] += amount * middle;
                if (collidesAt(candidate)) {
                    blocked = middle;
                } else {
                    safe = middle;
                }
            }
            entity.position[axis] += amount * safe;
            entity.velocity[axis] = 0.0F;
        };
        moveAxis(1, entity.velocity.y);
        moveAxis(0, entity.velocity.x);
        moveAxis(2, entity.velocity.z);

        // The item is collected once it actually reaches the player.
        if (entity.ageTicks >= 10U &&
            glm::distance(entity.position, playerPosition) <= 0.4F) {
            const auto previousCount = entity.stack.count;
            inventory.add(entity.stack);
            if (entity.stack.count != previousCount) {
                ++pickedUpStacks;
            }
        }
    }

    // Merge nearby stacks of the same item into a single group (vanilla
    // ItemEntity#tryMerge, capped at the stack size): a ground full of the same
    // drop renders as one icon instead of one entity per item.
    for (std::size_t index = 0; index < entities_.size(); ++index) {
        auto& group = entities_[index];
        if (group.stack.empty()) {
            continue;
        }
        const auto maximum = itemMaximumStackSize(group.stack);
        for (std::size_t other = index + 1; other < entities_.size(); ++other) {
            auto& target = entities_[other];
            if (target.stack.empty() || !sameItem(group.stack, target.stack) ||
                group.stack.count >= maximum) {
                continue;
            }
            if (std::abs(group.position.x - target.position.x) > 0.5F ||
                std::abs(group.position.y - target.position.y) > 0.5F ||
                std::abs(group.position.z - target.position.z) > 0.5F) {
                continue;
            }
            const auto moved = std::min(
                target.stack.count, static_cast<std::uint8_t>(maximum - group.stack.count));
            group.stack.count = static_cast<std::uint8_t>(group.stack.count + moved);
            target.stack.count = static_cast<std::uint8_t>(target.stack.count - moved);
            if (target.stack.count == 0U) {
                target.stack = {};
            }
        }
    }

    std::erase_if(entities_, [](const ItemEntity& entity) {
        return entity.stack.empty() || entity.ageTicks > 6'000U || entity.position.y < -8.0F;
    });
    // Rebuild the hash so the next tick's magnet and any cross-tick query see
    // only the surviving drops at their current positions and indices.
    sections_.clear();
    for (std::size_t index = 0; index < entities_.size(); ++index) {
        sections_[entitySectionOf(entities_[index].position)].push_back(index);
    }
    return pickedUpStacks;
}

} // namespace mc::gameplay
