#pragma once

#include "gameplay/EntitySection.hpp"
#include "gameplay/Inventory.hpp"

#include <glm/vec3.hpp>

#include <cstddef>
#include <unordered_map>
#include <vector>

namespace mc::world {
class World;
}

namespace mc::gameplay {

struct ItemEntity final {
    glm::vec3 position{0.0F};
    glm::vec3 previousPosition{0.0F};
    glm::vec3 velocity{0.0F};
    ItemStack stack;
    unsigned int ageTicks = 0;
    float visualPhase = 0.0F;
    [[nodiscard]] friend bool operator==(const ItemEntity&, const ItemEntity&) = default;
};

class ItemEntitySystem final {
  public:
    void spawn(glm::vec3 position, ItemStack stack, glm::vec3 initialVelocity = {});
    // Reinstates a drop from a save, keeping the age it had — spawn() would
    // reset it, and the age drives both the despawn timer and the pickup delay.
    void restore(glm::vec3 position, ItemStack stack, glm::vec3 velocity,
                 unsigned int ageTicks);
    [[nodiscard]] std::size_t tick(
        const world::World& world,
        glm::vec3 playerPosition,
        Inventory& inventory);

    [[nodiscard]] const std::vector<ItemEntity>& entities() const { return entities_; }

  private:
    std::vector<ItemEntity> entities_;
    float nextVisualPhase_ = 0.0F;
    // Chunk-section spatial hash over the drops, rebuilt at the end of each
    // tick, so the player magnet queries only the sections around the player
    // instead of sweeping every item. Holds indices into entities_.
    std::unordered_map<EntitySection, std::vector<std::size_t>, EntitySectionHash> sections_;
};

} // namespace mc::gameplay
