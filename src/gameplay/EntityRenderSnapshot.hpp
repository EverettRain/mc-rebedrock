#pragma once

// What the renderer needs to draw a creature, copied out once per tick.
//
// P3 Step 5. Drawing straight from `EntitySystem::entities()` works only while
// the simulation runs on the render thread: once the tick moves to its own
// thread, that vector is being mutated — reordered, compacted, resized — while
// the draw pass walks it. The failure mode is not a subtle one (a dangling
// `SimpleEntity&` mid-frame), and it is exactly the kind of thing that survives
// a long soak run without showing itself.
//
// So the tick publishes this instead: a flat vector of plain values, rebuilt at
// the end of every tick. It carries only the eight fields the draw pass reads —
// deliberately not a copy of SimpleEntity, whose brain, navigation path and
// goal state are simulation-owned and have no business crossing to the render
// side.
//
// The species pointer is safe to carry: EntityType instances are static
// singletons, the same reason SoundEvent may hold one.

#include "gameplay/ItemEntitySystem.hpp"
#include "gameplay/WorldSimulation.hpp"
#include "gameplay/entities/EntityType.hpp"

#include <cstdint>
#include <vector>

#include <glm/vec3.hpp>

namespace mc::gameplay {

struct SimpleEntity;

struct EntityRenderState final {
    const entities::EntityType* type = nullptr;
    // Stable across ticks, so a renderer can correlate frames (and, later,
    // interpolate against the right creature after the vector is compacted).
    std::uint64_t id = 0U;

    // The two endpoints the render frame interpolates between.
    glm::vec3 position{0.0F};
    glm::vec3 previousPosition{0.0F};
    float yaw = 0.0F;
    float previousYaw = 0.0F;
    float walkDistance = 0.0F;
    float previousWalkDistance = 0.0F;

    // The only parts of DamageState the draw pass reads: the death animation's
    // progress and whether to flash the hurt overlay.
    int hurtTicks = 0;
    int deathTicks = 0;

    // EntityType instances are static singletons, so the pointer compares by
    // identity and round-trips by id — the codec can rebuild the same pointer.
    [[nodiscard]] friend bool operator==(const EntityRenderState&, const EntityRenderState&) =
        default;
};

class EntityRenderSnapshot final {
  public:
    // Value equality over the three vectors, needed so NetMessage (a variant that
    // includes this) is equality-comparable. libc++ requires every variant
    // alternative to be comparable even when a held value never is at runtime;
    // libstdc++ is lenient, so this is what makes the mac build match Linux.
    [[nodiscard]] friend bool operator==(const EntityRenderSnapshot&,
                                         const EntityRenderSnapshot&) = default;

    // Rebuilds from the live lists. Called at the end of a tick, on whichever
    // thread owns the simulation.
    //
    // Items and falling blocks are copied whole rather than projected the way
    // creatures are: both are already flat value types with nothing
    // simulation-private in them, so a narrower struct would be duplication
    // without a reason. `SimpleEntity` is the odd one out precisely because it
    // is not — it carries a MobBrain and is not even copyable.
    void capture(const std::vector<SimpleEntity>& creatures,
                 const std::vector<ItemEntity>& items,
                 const std::vector<FallingBlockEntity>& fallingBlocks);

    [[nodiscard]] const std::vector<EntityRenderState>& entities() const { return entities_; }
    [[nodiscard]] const std::vector<ItemEntity>& items() const { return items_; }
    [[nodiscard]] const std::vector<FallingBlockEntity>& fallingBlocks() const {
        return fallingBlocks_;
    }
    // Populates the snapshot directly, for the client side of the transport
    // (C-1b-4): the codec decodes the three vectors off the channel and hands
    // them here, the client analogue of capture() on the server.
    void assign(std::vector<EntityRenderState> creatures, std::vector<ItemEntity> items,
                std::vector<FallingBlockEntity> fallingBlocks) {
        entities_ = std::move(creatures);
        items_ = std::move(items);
        fallingBlocks_ = std::move(fallingBlocks);
    }
    [[nodiscard]] bool empty() const {
        return entities_.empty() && items_.empty() && fallingBlocks_.empty();
    }
    // The resident bytes the three buffers hold, counting capacity not size:
    // capture() reuses the capacity across ticks, so this is the steady-state
    // cost once a population stabilises. The N-Mem budget gate pins a
    // per-entity ceiling on it.
    [[nodiscard]] std::size_t residentBytes() const {
        return sizeof(*this) + entities_.capacity() * sizeof(EntityRenderState) +
               items_.capacity() * sizeof(ItemEntity) +
               fallingBlocks_.capacity() * sizeof(FallingBlockEntity);
    }
    void clear() {
        entities_.clear();
        items_.clear();
        fallingBlocks_.clear();
    }

  private:
    std::vector<EntityRenderState> entities_;
    std::vector<ItemEntity> items_;
    std::vector<FallingBlockEntity> fallingBlocks_;
};

} // namespace mc::gameplay
