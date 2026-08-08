#pragma once

#include "gameplay/Damage.hpp"
#include "gameplay/Difficulty.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/entities/EntityType.hpp"

#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace mc::world {
class World;
}

namespace mc::gameplay {

// A minimal free-roaming creature. Its per-species constants — hitbox, health
// cap, wander speed, AI, loot — live on the entities::EntityType it points at,
// not in this struct: the simulation reads them through `type` so one code path
// drives every registered creature with no species switch. What stays here is
// the mutable per-instance state a LivingEntity needs: pose, the hurt/death
// bookkeeping, and the previous/current pairs the renderer interpolates between.
// Everything heavier — pathfinding, breeding — is still deliberately absent.
struct SimpleEntity final {
    // The registered species this creature is an instance of; never null once
    // spawned. Behaviour is dispatched through it (attributes, AI, loot, render).
    const entities::EntityType* type = nullptr;

    glm::vec3 position{0.0F};
    glm::vec3 previousPosition{0.0F};
    glm::vec3 velocity{0.0F};
    float yaw = 0.0F;                  // facing in radians (0 faces +Z)
    float previousYaw = 0.0F;          // last tick's yaw, for render interpolation
    float walkDistance = 0.0F;         // accumulated horizontal travel; drives the walk cycle
    float previousWalkDistance = 0.0F; // last tick's walkDistance, for interpolation
    // Entity#fallDistance: how far the body has fallen since it last touched
    // ground. The landing tick converts it into damage (see EntitySystem::tick).
    float fallDistance = 0.0F;
    bool onGround = false;
    bool moving = false;
    unsigned int ageTicks = 0U;
    unsigned int wanderTimer = 0U;
    std::uint32_t rngState = 0U;

    // The shared damage state: health, the hurt/invulnerability/death timers and
    // the last hit. PlayerVitals drives the same fields through Damage.hpp, so
    // the player and every mob resolve damage through one pipeline.
    DamageState damage;
    // Angerable#angerTime: ticks a neutral mob stays hostile after being hit.
    // Zero for passive and plain hostile mobs; driven by NeutralMob.hpp.
    int angerTicks = 0;

    [[nodiscard]] bool dead() const { return damage.dead(); }
    // Angerable#hasAngerTime: whether a neutral mob is currently provoked.
    [[nodiscard]] bool angry() const { return angerTicks > 0; }
    // The species control object. Safe to dereference for any spawned entity.
    [[nodiscard]] const entities::EntityType& kind() const { return *type; }
    [[nodiscard]] entities::EntityDimensions dimensions() const { return type->dimensions(); }
    // Entity#getBoundingBox: the box is centred on the feet position in X/Z and
    // rises from it.
    [[nodiscard]] glm::vec3 boundingBoxMinimum() const {
        const float half = dimensions().width * 0.5F;
        return {position.x - half, position.y, position.z - half};
    }
    [[nodiscard]] glm::vec3 boundingBoxMaximum() const {
        const auto box = dimensions();
        const float half = box.width * 0.5F;
        return {position.x + half, position.y + box.height, position.z + half};
    }
};

// What a ray found: which creature it hit and how far along the ray. The index
// is only valid until the next tick, which may remove dead creatures.
struct EntityRayHit final {
    std::size_t index = 0U;
    float distance = 0.0F;
};

// EntityDrops (a creature's death loot) now lives on the entity type; see
// entities/EntityType.hpp. Re-exported into this namespace so existing callers
// keep using the unqualified name.
using entities::EntityDrops;

// What one tick left behind for the caller to act on: creatures that finished
// dying (with their loot), and the push the herd applied to the player.
struct EntityTickResult final {
    std::size_t liveCount = 0U;
    // Entity#pushAwayFrom moves both parties; this is the player's share.
    glm::vec3 playerPush{0.0F};
};

class EntitySystem final {
  public:
    // The reach a survival player attacks at, matching vanilla's 3.0 block
    // entity interaction range.
    static constexpr float kAttackReach = 3.0F;

    // Spawns a creature of the given registered type. `seed` makes its wander
    // deterministic so tests and replays are reproducible; 0 derives a seed from
    // the spawn order. The type must outlive every creature spawned from it,
    // which registered types (static storage) always do.
    void spawn(glm::vec3 position, const entities::EntityType& type, std::uint32_t seed = 0U);

    // Advances every creature one 20 TPS tick against the world: gravity, box
    // collision, a simple wander, mutual pushing and the hurt/death timers.
    // `pusher`/`pusherWidth`/`pusherHeight` describe the player, so the herd and
    // the player shove each other apart the way vanilla entities do. `difficulty`
    // drives the peaceful pass: on Peaceful, MONSTER-category mobs are removed
    // the same tick, matching MobEntity#checkDespawn's isDisallowedInPeaceful.
    EntityTickResult tick(
        const world::World& world,
        glm::vec3 pusher = glm::vec3{0.0F, -1000.0F, 0.0F},
        float pusherWidth = 0.6F,
        float pusherHeight = 1.8F,
        Difficulty difficulty = Difficulty::Normal);

    // ProjectileUtil#getEntityCollision: the nearest creature whose box the ray
    // enters within `reach`. Dead creatures are not targetable.
    [[nodiscard]] std::optional<EntityRayHit> raycast(
        glm::vec3 origin,
        glm::vec3 direction,
        float reach = kAttackReach) const;

    // LivingEntity#hurt plus the attacker's knockback. Returns true when the hit
    // landed rather than being swallowed by the invulnerability window.
    bool hurt(std::size_t index, float amount, glm::vec3 knockbackOrigin);

    // Entity#kill / LivingEntity#kill: OutOfWorld damage at infinite magnitude,
    // the same path /kill routes a player through. Returns true when the
    // creature was killed by this call.
    bool kill(std::size_t index);

    // The loot a creature that just finished dying leaves behind. Drained by
    // the caller after tick(); the same creature never reports twice.
    [[nodiscard]] std::span<const std::pair<glm::vec3, EntityDrops>> pendingDrops() const {
        return pendingDrops_;
    }
    void clearPendingDrops() { pendingDrops_.clear(); }

    // Creatures that were hit or died this tick, so the caller can play the
    // vanilla sound for them. Drained the same way as the drops.
    [[nodiscard]] std::span<const std::pair<glm::vec3, bool>> pendingSounds() const {
        return pendingSounds_;
    }
    void clearPendingSounds() { pendingSounds_.clear(); }

    void clear() { entities_.clear(); }
    [[nodiscard]] const std::vector<SimpleEntity>& entities() const { return entities_; }

  private:
    // LivingEntity#onDeath: the one-time death event. Claims the death through
    // the shared beginDeath guard and rolls the creature's loot into
    // pendingDrops_ on the same tick health crossed zero — vanilla drops at
    // death, not when the corpse is removed twenty ticks later. Returns false
    // if death was already claimed.
    bool die(SimpleEntity& entity);
    void moveWithCollisions(const world::World& world, SimpleEntity& entity, glm::vec3 distance);
    [[nodiscard]] static bool boxIntersectsWorld(
        const world::World& world,
        glm::vec3 minimum,
        glm::vec3 maximum);

    std::vector<SimpleEntity> entities_;
    std::vector<std::pair<glm::vec3, EntityDrops>> pendingDrops_;
    // (position, died) for each sound the caller still has to play.
    std::vector<std::pair<glm::vec3, bool>> pendingSounds_;
    std::uint32_t lootRandomState_ = 0x1F123BB5U;
};

} // namespace mc::gameplay
