#pragma once

#include "gameplay/Damage.hpp"
#include "gameplay/Difficulty.hpp"
#include "gameplay/DyeColor.hpp"
#include "gameplay/EntitySection.hpp"
#include "gameplay/EnvironmentSnapshot.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/StatusEffect.hpp"
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
    // The 48-bit LegacyRandomSource state (stored in a u64) this creature's
    // wander/AI/loot draws advance in place. Widened from 32-bit with RNG-0.
    std::uint64_t rngState = 0U;

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
    // LivingEntity's active MobEffects, as EM-2's small fixed inline store rather
    // than a heap map. Empty for almost every creature (count == 0), so an
    // unaffected mob pays one integer to skip the whole effect tick.
    ActiveEffects effects{};
    // AgeableMob#age (EM-3): negative = a baby with that many ticks left to grow,
    // 0 = an adult ready to breed, positive = an adult's breed cooldown counting
    // down. Distinct from `ageTicks` above, which is a monotonic counter for
    // despawn/sound cadence and never goes negative. Zero (adult, no cooldown) is
    // the default, so a non-ageable mob simply never moves it.
    int age = 0;
    // Animal#loveTicks: ticks of the in-love state a feed grants. Two in-love
    // adults of one species breed on contact; ticks down to zero otherwise.
    int loveTicks = 0;
    // AR-A2: SheepEntity#sheared. Only meaningful for the sheep species (any
    // other creature simply never has this flipped), so it lives here rather
    // than on a sheep-only subtype — the same "shared struct, per-species field
    // idles at its default" shape `age`/`loveTicks` already use for non-ageable
    // mobs. True once shorn; EatGrassGoal clears it after the sheep eats a
    // grass block. Wool colour (dye, 26.1) is deferred — every sheep this build
    // spawns/regrows is white, see rollSheepLoot's note.
    bool sheared = false;

    // DYE-0: SheepEntity#getColor. The creature's dye colour, a dense DyeColor
    // id (white=0..black=15). Only meaningful for a coloured species (the sheep
    // this build spawns) — every other creature simply leaves it at the default
    // white, the same "shared struct, per-species field idles at its default"
    // shape `sheared`/`eggLayTimer` use. One byte, so it does not break the
    // SoA/snapshot POD-ness. Unlike `sheared`, colour IS persistent state (a
    // dyed sheep reopens the colour it was dyed), so it travels in the save
    // record — see PersistentEntity::color.
    DyeColor color = kDefaultDyeColor;

    // AR-A4: ChickenEntity#eggTime. Only meaningful for a laysEggs() species
    // (the same "shared struct, per-species field idles at its default" shape
    // `sheared` uses) — counts down to zero, at which point the landing tick's
    // egg scheduler drops one of the species' egg item and rerolls the next
    // interval. Zero is never a resting value for a laying species (spawn/
    // restore always give it a fresh positive roll before tick can observe it).
    int eggLayTimer = 0;

    // Stateful Goal instances and the current navigation path are per entity.
    entities::MobBrain brain;

    [[nodiscard]] bool dead() const { return damage.dead(); }
    // Angerable#hasAngerTime: whether a neutral mob is currently provoked.
    [[nodiscard]] bool angry() const { return angerTicks > 0; }
    // AgeableMob#isBaby: a negative age. Only ageable species ever go negative.
    [[nodiscard]] bool baby() const { return age < 0; }
    // Animal#canBreed / #isInLove, and the combined "ready to make a baby": an
    // adult (age 0, so not a baby and off cooldown) that is currently in love.
    [[nodiscard]] bool inLove() const { return loveTicks > 0; }
    [[nodiscard]] bool canBreed() const { return age == 0 && type->breedable(); }
    [[nodiscard]] bool readyToMate() const { return canBreed() && inLove(); }
    // The per-instance render/collision scale: a baby is drawn and collides at
    // its species' baby scale, an adult at 1.0. Derived from age and the type, so
    // it needs no stored field and stays correct the instant a baby grows up.
    [[nodiscard]] float bodyScale() const {
        return baby() ? type->breeding().babyScale : 1.0F;
    }
    // The species control object. Safe to dereference for any spawned entity.
    [[nodiscard]] const entities::EntityType& kind() const { return *type; }
    // Entity#getDimensions: the species box, shrunk by the per-instance scale so a
    // baby's hitbox matches its smaller body.
    [[nodiscard]] entities::EntityDimensions dimensions() const {
        const auto box = type->dimensions();
        const float scale = bodyScale();
        return {box.width * scale, box.height * scale};
    }
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

// AgeableMob age constants (26.1). A newborn starts this many ticks below zero
// (twenty real minutes to grow up) and climbs to adulthood; a fresh breed sets
// both parents to the cooldown; a feed grants the love window.
inline constexpr int kBabyAgeTicks = -24000;
inline constexpr int kBreedCooldownTicks = 6000;
inline constexpr int kLoveTicks = 600;
// AnimalMateGoal's contact distance: parents within this settle and breed.
inline constexpr float kBreedingRange = 3.0F;

// Sheep#ate (26.1): eating a grass block ages a lamb up by a flat 60 seconds
// (SheepEntity.ate -> this.ageUp(60)), on top of clearing `sheared`. Adults are
// unaffected — canAgeUp() gates it, mirrored by ageUp's own `age < 0` guard.
inline constexpr int kSheepEatAgeUpSeconds = 60;

// AgeableMob#getSpeedUpSecondsWhenFeeding (26.1): a feed that cannot start love
// (a baby) instead speeds its growth. Vanilla:
//   (int)(ticksUntilAdult / 20 * 0.1F)
// where ticksUntilAdult is the positive distance to adulthood (== -age for a
// baby). The result is a whole number of *seconds*; ageUp() multiplies it back
// by 20 to reach ticks. Integer/float arithmetic order is preserved verbatim so
// the growth grant matches JE to the tick.
[[nodiscard]] constexpr int getSpeedUpSecondsWhenFeeding(int ticksUntilAdult) {
    return static_cast<int>(static_cast<float>(ticksUntilAdult / 20) * 0.1F);
}

// AR-A4: ChickenEntity#eggTime (26.1): `random.nextInt(6000) + 6000`, so the
// next lay is 6000-12000 ticks (5-10 minutes) after the last, redrawn every
// time the timer elapses. Shared by every laysEggs() species, not chicken-only
// — matching kBreedCooldownTicks/kLoveTicks being mechanism constants above.
inline constexpr int kEggLayBaseTicks = 6000;
inline constexpr int kEggLayRandomTicks = 6000;

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

    // AR-A2: a sheared sheep's EatGrassGoal finished its animation and wants
    // this cell eaten. `entityId` is who to clear `sheared` on once the write
    // actually lands (GameSession, which alone can reach WorldMutationService);
    // the goal recorded `cell` when it last saw the grass, which is why the
    // caller must re-check the block itself before writing (it may have
    // changed since — a broken block, another sheep, or the sheep having
    // wandered off between the request and this drain).
    struct GrassEatRequest final {
        std::uint64_t entityId = 0U;
        glm::ivec3 cell{0};
    };

    std::size_t liveCount = 0U;
    // Entity#pushAwayFrom moves both parties; this is the player's share.
    glm::vec3 playerPush{0.0F};
    // AI emits attacks as simulation events; GameSession owns applying player
    // damage, difficulty scaling, hurt audio and death handling.
    std::vector<MobAttack> mobAttacks;
    std::vector<GrassEatRequest> grassEats;
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
    void spawn(glm::vec3 position, const entities::EntityType& type, std::uint64_t seed = 0U);

    // Restores a creature from a save record: spawns it and overwrites the pose
    // and fields a fresh spawn would not reproduce (velocity, health, yaw, the
    // wander rng and the timers), so a loaded world reopens with its herd where
    // it left off. Returns the stable id the restored creature holds.
    std::uint64_t restore(glm::vec3 position, const entities::EntityType& type, float yaw,
                          glm::vec3 velocity, float health, int angerTicks,
                          unsigned int ageTicks, std::uint64_t rngState, int fireTicks = 0,
                          const ActiveEffects& effects = {}, int age = 0, int loveTicks = 0,
                          DyeColor color = kDefaultDyeColor);

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
        bool raining = false,
        // The player's held stack, so TemptGoal can see a species' tempt item on
        // offer. Empty (default) means an empty hand — no animal is tempted.
        ItemStack heldItem = {},
        // AR-M2: the same tick-resolved snapshot NaturalSpawner reads
        // (GameSession::environment_). `ambientDarkness < 4` is Level#isDay —
        // the daylight-ignition rule below reads it alongside per-cell
        // directSkyLight, so "is it day" and "is this cell dark enough for a
        // monster to spawn" can never disagree about the same tick. The default
        // (ambientDarkness == 0, full daylight) matches every existing headless
        // caller that never lit a test world's sky — such a world reads as
        // "daytime but no sky exposure anywhere", so nothing ignites there.
        const EnvironmentSnapshot& environment = EnvironmentSnapshot{});

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
    // landed rather than being swallowed by the invulnerability window. `type`
    // picks which DamageType the shared Damage.hpp pipeline scores this hit
    // as — melee callers leave it at the default EntityAttack; RW-0's
    // projectile hit passes DamageType::Projectile so the same pipeline stage
    // order (armor/effects/absorption/shield, once they exist) applies to an
    // arrow exactly the way it already does to a sword swing.
    // `extraKnockbackStrength` is ENCH-1's Knockback enchant contribution
    // (EnchantmentHelper.getKnockback's `level * 0.5F`, see
    // EnchantmentCombat.hpp's meleeKnockbackEnchantBonus) — zero for every
    // caller that has no weapon enchant to add, which is every caller but the
    // player's own melee attack today.
    bool hurt(std::uint64_t entityId, float amount, glm::vec3 knockbackOrigin,
              ActorReference attacker = ActorReference::player(),
              DamageType type = DamageType::EntityAttack,
              float extraKnockbackStrength = 0.0F);

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

    // LivingEntity#addEffect: applies a status effect to a creature by id. The
    // merge (stronger/longer wins) lives in the shared StatusEffect API; this
    // just resolves the id. Returns true when the store changed. A miss (dead or
    // unknown id) is false, not an abort — content applies effects speculatively.
    bool applyEffect(std::uint64_t entityId, core::StatusEffectId effect,
                     std::int32_t durationTicks, std::uint8_t amplifier);
    // LivingEntity#removeEffect / #removeAllEffects. `hasEffect` is a query.
    bool removeEffect(std::uint64_t entityId, core::StatusEffectId effect);
    std::size_t clearEffects(std::uint64_t entityId);
    [[nodiscard]] bool hasEffect(std::uint64_t entityId, core::StatusEffectId effect) const;

    // ENCH-1 / DamageEnchantment#onTargetDamaged (typeIndex==2): applies Bane of
    // Arthropods' bonus Slowness IV to the target for `20 + nextInt(10*level)`
    // ticks. The duration's random draw comes from the target creature's own
    // reproducible RNG stream (entity.rngState, the same LCG the mob's AI and
    // XP drop use) rather than the wall clock, so a replayed hit lands the same
    // duration every time. Returns true when the effect store changed; a no-op
    // (unknown id, level 0) is false. The caller (PlayerInteraction) has already
    // confirmed the target is an arthropod.
    bool applyBaneOfArthropodsSlowness(std::uint64_t entityId, std::uint8_t level);

    // Animal#setInLove: puts a breedable adult into love for kLoveTicks. This is
    // the entry a feeding interaction (AR-A) calls after it has decided the held
    // item matches the species' tempt item and consumed one. Returns true when
    // the creature entered love (a baby or a mob on cooldown cannot). The item
    // check and consumption stay with the caller — EM-3 owns only the state.
    bool setInLove(std::uint64_t entityId);
    // AgeableMob#setBaby / #setBreedingAge: force a creature's age, so a spawn
    // egg can make a baby and a test can age one up. Ageing a baby to 0 turns it
    // into an adult with its full-size hitbox on the same call.
    bool setAge(std::uint64_t entityId, int age);
    // AgeableMob#ageUp (26.1): grows a baby toward adulthood by `seconds` real
    // seconds (clamped so it never overshoots past 0 into cooldown). A no-op on
    // an adult (age >= 0) or a non-breedable species, matching vanilla's own
    // clamp — the returned bool is true only when the age actually moved. This
    // is the "feed a baby to speed its growth" half of Animal#mobInteract and
    // the ageUp(60) inside Sheep#ate. (forcedAge/age-lock is not modelled here —
    // this build has no age-lock feature; noted as a known simplification.)
    bool ageUp(std::uint64_t entityId, int seconds);
    // AR-A5: SheepEntity#ate (26.1) — the whole "just ate a grass block" event:
    // clears `sheared` (regrows wool) and, if the sheep is a lamb, ages it up 60
    // seconds. GameSession calls this once the grass write actually lands,
    // replacing the older clearSheared-only relay so the lamb-growth half of
    // vanilla's ate() is no longer dropped. Returns true if anything changed.
    bool ate(std::uint64_t entityId);
    // AR-A2: SheepEntity#shear — sets `sheared` true. Returns false (a no-op)
    // if the creature is unknown or already sheared, which is sabotage anchor
    // ③'s contract: shearing an already-bald sheep must yield nothing, and the
    // caller (PlayerInteraction) gates the wool drop on this return value.
    bool shear(std::uint64_t entityId);
    // AR-A2: the other half of EatGrassGoal's relay — GameSession calls this
    // once its WorldMutationService write actually lands, so a sheep whose
    // grass turned out to be already gone (races against another sheep, a
    // player break) never regrows wool for nothing.
    bool clearSheared(std::uint64_t entityId);
    [[nodiscard]] const SimpleEntity* byIdConst(std::uint64_t id) const { return byId(id); }

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

    // XP-2: experience a kill (lastHurtByPlayer-gated, see die()) or a
    // successful breed (processBreeding) earned this tick, as (position,
    // points) pairs. Drained the same way as pendingDrops — the caller turns
    // each into an orb via GameSession::spawnExperienceOrbs, which is where
    // the denomination split and the deterministic scatter RNG live (XP-1).
    // This queue only ever grows during tick(); nothing here bypasses it.
    [[nodiscard]] std::span<const std::pair<glm::vec3, std::int32_t>> pendingExperience() const {
        return pendingExperience_;
    }
    void clearPendingExperience() { pendingExperience_.clear(); }

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

    // DIM-5: silently removes one creature by id and returns its full state, or
    // nullopt if the id is unknown. Unlike kill(), no death, loot or sound fires —
    // this is the extraction half of a cross-dimension transfer (the creature is
    // about to be re-created in another Level with the same state), the
    // single-entity analogue of removeInChunk.
    [[nodiscard]] std::optional<SimpleEntity> detach(std::uint64_t entityId);

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
    // Installs EM-3's Tempt/AnimalMate/FollowParent goals on a freshly built
    // brain when the species is breedable, reading the numbers off its breeding
    // profile. A non-breedable species gets none, so it pays nothing. Kept in the
    // spawn path (not AnimalAi::configureBrain) because the goals are data-driven
    // off the type, which configureBrain does not receive.
    static void installBreedingGoals(SimpleEntity& entity);
    // AnimalMateGoal#breed: for each (parentA, parentB) the AI pass filed, if both
    // are still adults in love, spawns one baby at their midpoint, puts both on
    // the breed cooldown and clears their love. Runs after the tick's main loop so
    // the spawn cannot invalidate the iteration.
    void processBreeding(const std::vector<std::pair<std::uint64_t, std::uint64_t>>& requests);
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
    // XP-2: (position, points) pairs from a gated kill or a successful breed.
    std::vector<std::pair<glm::vec3, std::int32_t>> pendingExperience_;
    // The 48-bit mc::rng state (Java LegacyRandomSource core) mob-death loot
    // rolls advance. Session-derived, so a fixed raw internal state.
    std::uint64_t lootRandomState_ = 0x00001F123BB5ULL;
    std::uint64_t nextEntityId_ = 1U;
    std::uint64_t gameTick_ = 0U;
};

} // namespace mc::gameplay
