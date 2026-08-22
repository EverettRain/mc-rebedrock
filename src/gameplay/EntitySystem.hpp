#pragma once

#include "gameplay/Damage.hpp"
#include "gameplay/Difficulty.hpp"
#include "gameplay/EntitySection.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/entities/EntityType.hpp"
#include "gameplay/entities/MobBrain.hpp"

#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mc::world {
class World;
}

namespace mc::gameplay {
class EntityRenderSnapshot;



// A minimal free-roaming creature. Its per-species constants — hitbox, health
// cap, wander speed, AI, loot — live on the entities::EntityType it points at,
// not in this struct: the simulation reads them through `type` so one code path
// drives every registered creature with no species switch. What stays here is
// the mutable per-instance state a LivingEntity needs: pose, the hurt/death
// bookkeeping, per-entity MobBrain, and the previous/current pairs the renderer
// interpolates between. Breeding remains deliberately absent; basic land
// pathfinding and prioritized goals live in entities/MobBrain.hpp.
struct SimpleEntity final {
    // The registered species this creature is an instance of; never null once
    // spawned. Behaviour is dispatched through it (attributes, AI, loot, render).
    const entities::EntityType* type = nullptr;
    // Stable for this run even when EntitySystem compacts its vector.
    std::uint64_t id = 0U;

    glm::vec3 position{0.0F};
    glm::vec3 previousPosition{0.0F};
    glm::vec3 velocity{0.0F};
    float yaw = 0.0F;                  // facing in radians (0 faces +Z)
    float previousYaw = 0.0F;          // last tick's yaw, for render interpolation
    float lookYaw = 0.0F;              // independent LOOK-control intent
    float walkDistance = 0.0F;         // accumulated horizontal travel; drives the walk cycle
    float previousWalkDistance = 0.0F; // last tick's walkDistance, for interpolation
    // Entity#fallDistance: how far the body has fallen since it last touched
    // ground. The landing tick converts it into damage (see EntitySystem::tick).
    float fallDistance = 0.0F;
    bool onGround = false;
    bool moving = false;
    float movementSpeedMultiplier = 1.0F;
    unsigned int ageTicks = 0U;
    unsigned int wanderTimer = 0U;
    std::uint32_t rngState = 0U;

    // LivingEntity#getAttacker plus a monotonically increasing hit sequence.
    // EscapeDangerGoal consumes every landed hit once while the stable actor
    // reference lets later revenge/target goals keep following the attacker.
    ActorReference lastAttacker{};
    glm::vec3 lastAttackerPosition{0.0F};
    int recentAttackerTicks = 0;
    std::uint64_t lastHurtSequence = 0U;

    // MobEntity#ambientSoundChance: the idle-sound scheduler's counter. Each
    // tick baseTick rolls nextInt(1000) against it while it climbs; a roll that
    // lands plays the ambient clip and snaps the counter back to
    // -getMinAmbientSoundDelay() (80), so a species barks roughly every four
    // seconds. Hurt resets it the way playHurtSound's resetSoundDelay does.
    int ambientSoundChance = 0;
    // Horizontal travel accumulated toward the next playStepSound. Footsteps
    // fire once per block of real walking and reset on the ground.
    float stepAccumulator = 0.0F;

    // The shared damage state: health, the hurt/invulnerability/death timers and
    // the last hit. PlayerVitals drives the same fields through Damage.hpp, so
    // the player and every mob resolve damage through one pipeline.
    DamageState damage;
    // Angerable#angerTime: ticks a neutral mob stays hostile after being hit.
    // Zero for passive and plain hostile mobs; driven by NeutralMob.hpp.
    int angerTicks = 0;
    // MobEntity#checkDespawn's despawn-range counter: ticks spent past 128
    // blocks for a distant-despawning category (MONSTER/AMBIENT/WATER_CREATURE).
    // Once it crosses the threshold the creature is silently removed.
    int despawnTicks = 0;
    // Entity#fireTicks: how many ticks the creature stays ablaze. Set by
    // setSecondsOnFire (a lit block, lava, a future daylight source); ticks down
    // every simulation tick and deals OnFire damage each second while it burns.
    // Water and rain extinguish it. Zero means not on fire, which is the usual
    // case, so it costs nothing until something lights the creature.
    int fireTicks = 0;

    // Stateful Goal instances and the current navigation path are per entity.
    entities::MobBrain brain;

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

// What a ray found: which creature it hit and how far along the ray. The id is
// stable across ticks — unlike a vector index, which is only valid until the
// next tick removes or reorders a creature — so the caller can hand it straight
// to hurt()/kill().
struct EntityRayHit final {
    std::uint64_t entityId = 0U;
    float distance = 0.0F;
};

// Entity#raycast against the published render snapshot instead of the live
// entity vector: the attack-targeting ray tests the same positions the draw
// pass shows (the snapshot is what the render thread reads), with no reference
// into a vector the tick may be mid-compaction on. Skips entities whose death
// animation has finished, exactly like EntitySystem::raycast. `reach` bounds
// the result, so the caller's block/entity distance comparison still works.
[[nodiscard]] std::optional<EntityRayHit> raycastSnapshotEntities(
    const EntityRenderSnapshot& snapshot,
    glm::vec3 origin,
    glm::vec3 direction,
    float reach);

// The four sound hooks a 1.16.1 LivingEntity raises. The caller plays the
// matching clip for the creature's species (see entities::EntityType::soundProfile).
enum class MobSoundEvent : std::uint8_t {
    Hurt,
    Death,
    Ambient,
    Step,
};

// One creature sound queued for the caller: where it happened, which event, and
// the species that owns the clip. `type` points at the registered EntityType
// (static storage), so it outlives the queue and never needs ref-counting.
struct PendingMobSound final {
    glm::vec3 position{0.0F};
    MobSoundEvent event = MobSoundEvent::Hurt;
    const entities::EntityType* type = nullptr;
};

// EntityDrops (a creature's death loot) now lives on the entity type; see
// entities/EntityType.hpp. Re-exported into this namespace so existing callers
// keep using the unqualified name.
using entities::EntityDrops;

// What one tick left behind for the caller to act on: creatures that finished
// dying (with their loot), and the push the herd applied to the player.
struct EntityTickResult final {
    struct MobAttack final {
        std::uint64_t attackerId = 0U;
        ActorReference target{};
        float amount = 0.0F;
    };

    std::size_t liveCount = 0U;
    // Entity#pushAwayFrom moves both parties; this is the player's share.
    glm::vec3 playerPush{0.0F};
    // AI emits attacks as simulation events; GameSession owns applying player
    // damage, difficulty scaling, hurt audio and death handling.
    std::vector<MobAttack> mobAttacks;
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

    // Restores a creature from a save record: spawns it and overwrites the pose
    // and fields a fresh spawn would not reproduce (velocity, health, yaw, the
    // wander rng and the timers), so a loaded world reopens with its herd where
    // it left off. Returns the stable id the restored creature holds.
    std::uint64_t restore(glm::vec3 position, const entities::EntityType& type, float yaw,
                          glm::vec3 velocity, float health, int angerTicks,
                          unsigned int ageTicks, std::uint32_t rngState, int fireTicks = 0);

    // Advances every creature one 20 TPS tick against the world: target/action
    // selectors, land navigation, gravity, collision, pushing and damage timers.
    // `pusher`/`pusherWidth`/`pusherHeight` describe the player, so the herd and
    // the player shove each other apart the way vanilla entities do. `difficulty`
    // drives the peaceful pass: on Peaceful, MONSTER-category mobs are removed
    // the same tick, matching MobEntity#checkDespawn's isDisallowedInPeaceful.
    // `simulationRadius` (blocks, horizontal) freezes creatures farther than that
    // from the pusher — no AI, movement, gravity or timers — while keeping them
    // rendered and targetable, the way ServerWorld's tick distance keeps far mobs
    // alive but dormant. 0 disables the gate (headless tests).
    EntityTickResult tick(
        const world::World& world,
        glm::vec3 pusher = glm::vec3{0.0F, -1000.0F, 0.0F},
        float pusherWidth = 0.6F,
        float pusherHeight = 1.8F,
        Difficulty difficulty = Difficulty::Normal,
        bool playerAlive = true,
        bool playerCreative = false,
        float simulationRadius = 0.0F,
        // Whether it is raining this tick (WeatherSystem#isRaining). A burning
        // creature standing under open sky is put out by the rain, the way
        // Entity#baseTick's isBeingRainedOn extinguishes it. False leaves fire to
        // be quenched only by water.
        bool raining = false);

    // GameRenderer's crosshair pick: the nearest creature whose targeting box
    // the ray enters within `reach`. Ordinary 1.16.1 living entities have a
    // zero targeting margin, so this is their exact physical bounding box.
    [[nodiscard]] std::optional<EntityRayHit> raycast(
        glm::vec3 origin,
        glm::vec3 direction,
        float reach = kAttackReach) const;

    // Whether a creature with these dimensions can stand at `position` without
    // intersecting a solid block. Spawn systems use the complete AABB rather
    // than checking only the feet cell, so a mob cannot be born in a wall or
    // under a ceiling.
    [[nodiscard]] static bool canOccupy(
        const world::World& world,
        glm::vec3 position,
        entities::EntityDimensions dimensions);

    // Whether a still-present creature overlaps this full block cell. Block
    // placement uses it alongside the player's overlap check.
    // Whether any live creature overlaps a block placed at (x, y, z) whose
    // collision box spans [y+boxBottom, y+boxTop]. The default is a full cube;
    // slab placement passes the half box so a slab can go in beside a creature.
    [[nodiscard]] bool intersectsBlock(int x, int y, int z, float boxBottom = 0.0F,
                                       float boxTop = 1.0F) const;

    // LivingEntity#hurt plus the attacker's knockback. Returns true when the hit
    // landed rather than being swallowed by the invulnerability window.
    bool hurt(std::uint64_t entityId, float amount, glm::vec3 knockbackOrigin,
              ActorReference attacker = ActorReference::player());

    // Entity#kill / LivingEntity#kill: OutOfWorld damage at infinite magnitude,
    // the same path /kill routes a player through. Returns true when the
    // creature was killed by this call.
    bool kill(std::uint64_t entityId);

    // Entity#setSecondsOnFire: lights the creature for `seconds` of burning, the
    // single entry point every ignition source (lava, a fire block, the daylight
    // burn) routes through. Vanilla only ever lengthens a burn, so this takes the
    // max of the current and requested duration. A fireImmune species is never
    // lit. Returns true when the call left the creature ablaze.
    bool setOnFire(std::uint64_t entityId, int seconds);

    // The loot a creature that just finished dying leaves behind. Drained by
    // the caller after tick(); the same creature never reports twice.
    [[nodiscard]] std::span<const std::pair<glm::vec3, EntityDrops>> pendingDrops() const {
        return pendingDrops_;
    }
    void clearPendingDrops() { pendingDrops_.clear(); }

    // Sound events creatures raised this tick, so the caller can play the right
    // clip for the right species. Drained the same way as the drops.
    [[nodiscard]] std::span<const PendingMobSound> pendingSounds() const {
        return pendingSounds_;
    }
    void clearPendingSounds() { pendingSounds_.clear(); }

    // Wipes every creature and the id/spatial indexes, and restarts the id
    // allocator — the world-reset path (a new save or /reload). Any id held by
    // the caller before this is invalid afterwards, exactly as indices were.
    void clear();

    // M-3 (C5): removes every creature whose position is inside the given chunk
    // and returns them, so the chunk-unload path can write them to the chunk's
    // region file. A chunk leaving the simulation radius takes its herd with it
    // instead of leaving it ticking in a chunk that no longer exists; the load
    // path restores them when the chunk streams back in. The caller must hold
    // the world write section (no tick is running).
    [[nodiscard]] std::vector<SimpleEntity> removeInChunk(int chunkX, int chunkZ);
    [[nodiscard]] const std::vector<SimpleEntity>& entities() const { return entities_; }
    // The creature with the given stable id, or null once it has despawned.
    // Stable across vector compactions, so commands and brains can hold an id
    // past the tick boundary.
    [[nodiscard]] SimpleEntity* byId(std::uint64_t id);
    [[nodiscard]] const SimpleEntity* byId(std::uint64_t id) const;

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
    // Rebuilds indexes after the entity vector was compacted. Ordinary movement
    // updates only entities that crossed a section boundary.
    void rebuildSpatialIndex();
    void updateSectionMembership(std::size_t index);

    std::vector<SimpleEntity> entities_;
    // Stable id → current index. Kept in sync by rebuildSpatialIndex so a
    // survived id always resolves to the live element after a compaction.
    std::unordered_map<std::uint64_t, std::size_t> idToIndex_;
    // Chunk-section spatial hash: section → indices into entities_ at their
    // current positions. Turns the O(n²) pushing sweep and O(n) raycast into
    // O(neighbours) queries.
    std::unordered_map<EntitySection, std::vector<std::size_t>, EntitySectionHash> sections_;
    // The bucket currently containing each vector slot. Parallel to entities_
    // so crossing checks are a pair of integer comparisons.
    std::vector<EntitySection> entitySections_;
    std::vector<std::pair<glm::vec3, EntityDrops>> pendingDrops_;
    std::vector<PendingMobSound> pendingSounds_;
    std::uint32_t lootRandomState_ = 0x1F123BB5U;
    std::uint64_t nextEntityId_ = 1U;
    std::uint64_t gameTick_ = 0U;
};

} // namespace mc::gameplay
