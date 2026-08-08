#pragma once

#include "gameplay/Inventory.hpp"

#include <glm/vec3.hpp>

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
};

class ItemEntitySystem final {
  public:
    void spawn(glm::vec3 position, ItemStack stack, glm::vec3 initialVelocity = {});
    [[nodiscard]] std::size_t tick(
        const world::World& world,
        glm::vec3 playerPosition,
        Inventory& inventory);

    [[nodiscard]] const std::vector<ItemEntity>& entities() const { return entities_; }

  private:
    std::vector<ItemEntity> entities_;
    float nextVisualPhase_ = 0.0F;
};

} // namespace mc::gameplay
