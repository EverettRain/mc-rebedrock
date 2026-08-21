#pragma once

#include "audio/MobSoundProfile.hpp"
#include "core/ContentId.hpp"
#include "core/Identifier.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/entities/EntityAttributes.hpp"
#include "gameplay/entities/EntityAttributeOverlay.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace mc::gameplay {
// The live creature the AI and loot hooks act on. Declared in EntitySystem.hpp;
// only referenced here through pointers/references so the two headers do not
// depend on each other's full definitions.
struct SimpleEntity;
} // namespace mc::gameplay

namespace mc::gameplay::entities {

class MobBrain;

// SpawnGroup / MobCategory (1.16.1): what a creature counts as for spawn caps,
// despawn behaviour and which creative tab its egg lands in. Replaces the old
// implicit "everything is a pig" assumption with an explicit classification.
enum class MobCategory : std::uint8_t {
    Creature,      // passive land animals: pig, cow, sheep, chicken
    Monster,       // hostile mobs: zombie, skeleton, creeper
    Ambient,       // bats
    WaterCreature, // squid, dolphins, fish
    Misc,          // projectiles, item frames — never gets a spawn egg
};

// SpawnGroup's per-category constants, read by NaturalSpawner and the
// despawn/peaceful passes. Registration picks a category
// (CREATURE, MONSTER, …) and everything below is derived from it, so a species
// never restates its own spawn law.
struct MobCategoryTraits final {
    // SpawnGroup#getCapacity: the per-player soft cap the natural spawner stops at.
    int spawnCap = 0;
    // HostileEntity#isDisallowedInPeaceful: removed the instant the world is set
    // to Peaceful. True for MONSTER only, matching vanilla.
    bool disallowedInPeaceful = false;
    // MobEntity#checkDespawn: eligible for random far-from-player despawn.
    // Passive animals (CREATURE) persist; hostiles and ambients do not.
    bool despawnsWhenDistant = false;
    // The spawner's light rule: MONSTER spawns in darkness, the rest in daylight.
    bool spawnsInDarkness = false;
    // Whether the category participates in natural spawning at all (MISC never
    // does).
    bool naturalSpawn = false;
};

// The spawn law for a category. A small closed mapping over a fixed enum (like
// difficultyName), not a per-creature switch — adding a creature never touches
// it; adding a whole new category (rare) does.
[[nodiscard]] constexpr MobCategoryTraits mobCategoryTraits(MobCategory category) {
    switch (category) {
    case MobCategory::Creature:
        return {/*spawnCap=*/10, /*peaceful=*/false, /*despawn=*/false,
                /*dark=*/false, /*natural=*/true};
    case MobCategory::Monster:
        return {/*spawnCap=*/70, /*peaceful=*/true, /*despawn=*/true,
                /*dark=*/true, /*natural=*/true};
    case MobCategory::Ambient:
        return {/*spawnCap=*/15, /*peaceful=*/false, /*despawn=*/true,
                /*dark=*/false, /*natural=*/true};
    case MobCategory::WaterCreature:
        return {/*spawnCap=*/5, /*peaceful=*/false, /*despawn=*/true,
                /*dark=*/false, /*natural=*/true};
    case MobCategory::Misc:
        return {/*spawnCap=*/0, /*peaceful=*/false, /*despawn=*/false,
                /*dark=*/false, /*natural=*/false};
    }
    return {};
}

// SpawnPlacementTypes (26.1 `world/entity/SpawnPlacementTypes.java`): the kind
// of cell a species may be born in. Vanilla makes each one a lambda behind a
// SpawnPlacementType interface; here it is a plain enum that a free function
// switches on (`gameplay/SpawnPlacements.hpp`) — a virtual dispatch that exists
// to produce one bool is a cost with nothing to show for it, and the set is
// closed in vanilla too.
enum class SpawnPlacement : std::uint8_t {
    OnGround,       // ON_GROUND: a valid floor below, two clear cells to stand in
    InWater,        // IN_WATER: the cell is water — squid, fish, dolphins
    InLava,         // IN_LAVA: striders
    NoRestrictions, // NO_RESTRICTIONS: wherever the rest of the chain allows
};

// EntityType.EntityDimensions: the axis-aligned collision box. Square in X/Z
// like every vanilla mob, so a single width describes both horizontal axes.
struct EntityDimensions final {
    float width = 0.9F;
    float height = 0.9F;
};

// SpawnEggItem's two tint colours, packed 0xRRGGBB. `primary` paints the shell,
// `secondary` the spots.
struct SpawnEggColors final {
    std::uint32_t primary = 0xFFFFFFU;
    std::uint32_t secondary = 0xFFFFFFU;
};

// EntityRendererFactory's payload, reduced to the assets a renderer binds for a
// species: the bedrock geometry + animation JSON, the skin, and the identifiers
// the JSON declares so a renderer maps its bones without guessing (the "geo
// mapping"). Paths are relative to the resource root, keeping the renderer
// asset-driven rather than hard-coding a model per creature. All views point at
// string literals owned by the registering creature class, so they outlive the
// type.
struct EntityRenderDescriptor final {
    std::string_view geometryPath{};  // "animation/pig.geo.json"
    std::string_view animationPath{}; // "animation/pig.animation.json"
    std::string_view texturePath{};   // "entity/pig/pig_temperate.png"
    std::string_view geometryId{};    // "geometry.pig"
    std::string_view walkAnimation{}; // "animation.pig.walk"
    std::string_view idleAnimation{}; // "animation.pig.idle"
    float scale = 1.0F;               // uniform model scale, 1.0 for most mobs
};

// One creature's death drops. Small and inline so a loot roll never allocates;
// vanilla pigs roll one to three raw porkchops, which is the widest case here.
struct EntityDrops final {
    static constexpr std::size_t kMaximumEntries = 2;

    std::array<ItemStack, kMaximumEntries> entries{};
    std::size_t count = 0U;

    void add(const ItemStack& stack);
    [[nodiscard]] std::span<const ItemStack> view() const { return {entries.data(), count}; }
};

// The shared deterministic LCG (Numerical Recipes constants). Exposed here so a
// species' AI and loot draw from the entity's own reproducible stream, the same
// generator the simulation advances — no separate global RNG, no <random>.
[[nodiscard]] inline std::uint32_t nextRandom(std::uint32_t& state) {
    state = state * 1664525U + 1013904223U;
    return state;
}

// A value in [0, 1) from the top 24 bits (an LCG's low bits are weak).
[[nodiscard]] inline float randomUnit(std::uint32_t& state) {
    return static_cast<float>(nextRandom(state) >> 8) / static_cast<float>(1U << 24);
}

// The immutable AI profile for a species. The profile itself is shared, but it
// configures a distinct MobBrain (and therefore distinct stateful Goal objects)
// for every spawned creature. This mirrors MobEntity#initGoals without sharing
// EscapeDangerGoal/MeleeAttackGoal runtime state between mobs.
class EntityAi {
  public:
    virtual ~EntityAi() = default;

    // MobEntity#initGoals: install this species' prioritized target and action
    // goals into a newly created per-entity brain.
    virtual void configureBrain(MobBrain& brain) const = 0;

    // Fires once when a creature spawns, the moment MobEntity#initGoals runs.
    virtual void onSpawn(SimpleEntity& self, std::uint32_t& rng) const {
        static_cast<void>(self);
        static_cast<void>(rng);
    }

    // A species-level tick hook for behavior outside the Goal selectors. It is
    // called before the per-entity MobBrain every simulation tick.
    virtual void tick(SimpleEntity& self, std::uint32_t& rng) const {
        static_cast<void>(self);
        static_cast<void>(rng);
    }

    // LivingEntity#damage → Angerable#setTarget: fired when the creature takes a
    // hit, the hook a neutral species overrides to turn on its attacker. Default
    // does nothing, so passive and hostile mobs ignore it; see NeutralMob.hpp.
    virtual void onAttacked(SimpleEntity& self, std::uint32_t& rng) const {
        static_cast<void>(self);
        static_cast<void>(rng);
    }
};

// A species' loot table, reduced to a roll against the entity's RNG stream.
using LootRoll = EntityDrops (*)(std::uint32_t& rng);

// EntityType<T> (1.16.1): the immutable, per-species control object. It owns the
// creature's hitbox, classification, attribute caps, spawn-egg tint, renderer
// descriptor, AI and loot. Every consumer — simulation, renderer, spawn egg,
// commands — reads behaviour from this object, so there is no global species
// dictionary, no reflection and no giant switch. Instances are built once
// through the Builder and stored in static storage by the registering creature
// class, giving each a stable address that can be compared by pointer.
class EntityType final {
  public:
    class Builder;

    [[nodiscard]] const core::Identifier& id() const { return id_; }
    [[nodiscard]] const core::Identifier& vanillaId() const { return vanillaId_; }
    [[nodiscard]] MobCategory category() const { return category_; }
    // SpawnPlacements.getPlacementType: which cell the natural spawner may put
    // this species in. Land animals and hostiles are ON_GROUND, which is why
    // that is the default a species need not restate.
    [[nodiscard]] SpawnPlacement spawnPlacement() const { return spawnPlacement_; }
    [[nodiscard]] EntityDimensions dimensions() const { return dimensions_; }
    // The effective attributes: the species' compiled-in floor, with any
    // datapack override applied on top (per attribute). Resolves through the
    // process-wide overlay by this type's id — one subscript, no map, and the
    // floor itself when no pack overrode this species (the usual case).
    [[nodiscard]] const EntityAttributes& attributes() const {
        return entityAttributeTable().effectiveOr(typeId(), attributes_);
    }
    // The compiled-in floor alone, ignoring any overlay. The overlay loader reads
    // a datapack file onto a copy of this so unlisted attributes keep their
    // default; tests use it to prove the floor is untouched.
    [[nodiscard]] const EntityAttributes& attributesFloor() const { return attributes_; }
    [[nodiscard]] SpawnEggColors spawnEgg() const { return spawnEgg_; }
    [[nodiscard]] bool hasSpawnEgg() const { return hasSpawnEgg_; }
    [[nodiscard]] const EntityRenderDescriptor& render() const { return render_; }
    [[nodiscard]] const EntityAi& ai() const { return *ai_; }

    // The species' sound set (1.16.1 MobEntity getAmbientSound/getHurtSound/
    // getDeathSound/playStepSound). Empty for a species that registered none.
    [[nodiscard]] const audio::MobSoundProfile& soundProfile() const { return soundProfile_; }

    // The runtime index the registry hands out, stable for one run. Stands in
    // for Registry#getRawId, letting the simulation and renderer key per-type
    // caches by a small integer.
    [[nodiscard]] std::uint16_t networkId() const { return networkId_; }

    // The same value as a strongly-typed dense id, the handle every other kind
    // of content uses (BlockId / ItemId). deref = entityTypeRegistry().all()[id]
    // or byNetworkId(id.value()); the two forms carry the identical integer.
    [[nodiscard]] core::EntityTypeId typeId() const {
        return core::EntityTypeId::of(networkId_);
    }

    // Loot-table roll for a creature of this type; empty when none is defined.
    [[nodiscard]] EntityDrops rollLoot(std::uint32_t& rng) const {
        return loot_ != nullptr ? loot_(rng) : EntityDrops{};
    }

  private:
    friend class Builder;
    friend class EntityTypeRegistry;
    friend class UnknownEntityTable;

    core::Identifier id_{};
    core::Identifier vanillaId_{};
    MobCategory category_ = MobCategory::Creature;
    SpawnPlacement spawnPlacement_ = SpawnPlacement::OnGround;
    EntityDimensions dimensions_{};
    EntityAttributes attributes_{};
    SpawnEggColors spawnEgg_{};
    bool hasSpawnEgg_ = false;
    EntityRenderDescriptor render_{};
    audio::MobSoundProfile soundProfile_{};
    const EntityAi* ai_ = nullptr;
    LootRoll loot_ = nullptr;
    std::uint16_t networkId_ = 0U;
};

// EntityType.Builder: the factory function chain a creature class uses to define
// its type declaratively, e.g.
//
//   EntityType::Builder::create(MobCategory::Creature, kPigAi)
//       .sized(0.9F, 0.9F).health(10.0F).movementSpeed(0.25F)
//       .spawnEgg(0xF0A5A5U, 0xDB635EU).loot(&rollPigLoot)
//       .renderer(kPigRender).vanillaName("pig").build("pig");
//
// The AI reference is required at create() time, exactly as a MobEntity's
// EntityFactory is required to register an EntityType — a mob cannot exist
// without behaviour.
class EntityType::Builder final {
  public:
    [[nodiscard]] static Builder create(MobCategory category, const EntityAi& ai);

    // SpawnPlacements.register: only a species that is *not* born standing on
    // the ground states this, exactly as vanilla only registers the exceptions.
    Builder& spawnPlacement(SpawnPlacement placement);
    Builder& sized(float width, float height);
    Builder& health(float maxHealth);
    Builder& movementSpeed(float speed);
    Builder& attackDamage(float damage);
    Builder& followRange(float range);
    Builder& knockbackResistance(float resistance);
    Builder& spawnEgg(std::uint32_t primary, std::uint32_t secondary);
    Builder& loot(LootRoll roll);
    Builder& renderer(const EntityRenderDescriptor& descriptor);
    // The species' sound set; without it the creature is silent. Each species
    // states its own clips the way its Java class overrides the sound hooks.
    Builder& sounds(const audio::MobSoundProfile& profile);
    // The `minecraft:` alias so 1.16.1 assets and translation keys still resolve.
    Builder& vanillaName(std::string_view path);

    // EntityType.Builder#build(id): finalises the immutable type under
    // `rebedrock:<path>`. The result is meant to be stored in static storage by
    // the caller (one per species) and then handed to the registry.
    [[nodiscard]] EntityType build(std::string_view path) const;

  private:
    EntityType draft_{};
};

} // namespace mc::gameplay::entities
