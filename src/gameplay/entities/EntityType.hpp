#pragma once

#include "audio/MobSoundProfile.hpp"
#include "core/ContentId.hpp"
#include "core/Identifier.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/Random.hpp"
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
    // Optional second skin sampled by "wool"-prefixed bones (the sheep fleece
    // layer). Empty for the common single-texture creature; when set, the
    // species gets a second entity-texture-array layer loaded from this path and
    // its wool bones sample that layer instead of the body skin.
    std::string_view secondaryTexturePath{}; // "entity/sheep/sheep_wool.png"
};

// AgeableMob's breeding parameters, as data on the type rather than a species
// class override (EM-3). A species that does not set one is not breedable; a
// species that does states only the numbers — which held item tempts it into
// love, and (optionally) a non-default baby scale. This is the "content states
// parameters, the mechanism is shared" rule: EM-3 owns the state machine and
// the goals, AR-A hands each animal its wheat/seeds through this struct.
struct BreedingProfile final {
    // Whether this species breeds at all. False leaves the tempt/mate/follow
    // goals uninstalled, so a non-ageable mob pays nothing.
    bool breedable = false;
    // The item that tempts the animal and, when fed to two adults, starts love.
    // Empty means "nothing tempts it" (a breedable species with no attractant is
    // a misconfiguration, but harmless — it simply never enters love).
    ItemStack temptItem{};
    // AgeableMob baby render scale. Vanilla babies are half size; a species may
    // override (a chicken chick is smaller still).
    float babyScale = 0.5F;
};

// AR-A4: ChickenEntity#eggTime, as data on the type rather than a species-only
// code path — the same "content states parameters, the mechanism is shared"
// split BreedingProfile uses. A species that does not set one never lays; a
// species that does states only which item drops. The interval itself
// (6000-12000 ticks, EntitySystem::kEggLayBaseTicks/kEggLayRandomTicks) is a
// mechanism constant, not per-species, matching vanilla where every
// egg-laying mob shares Chicken's own timer shape.
struct EggLayProfile final {
    // Whether this species lays eggs at all. False costs a non-laying creature
    // nothing — its eggLayTimer field simply never moves off zero.
    bool laysEggs = false;
    // The item dropped when the timer elapses (a chicken's Egg).
    ItemStack item{};
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

// A value in [0, 1) drawn from the entity's own reproducible stream, the same
// generator the simulation advances — no separate global RNG, no <random>. This
// forwards to the shared mc::rng (Java's LegacyRandomSource core); the old inline
// 32-bit Numerical-Recipes LCG that lived here is gone.
[[nodiscard]] inline float randomUnit(std::uint64_t& state) { return rng::nextFloat(state); }

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
    virtual void onSpawn(SimpleEntity& self, std::uint64_t& rng) const {
        static_cast<void>(self);
        static_cast<void>(rng);
    }

    // A species-level tick hook for behavior outside the Goal selectors. It is
    // called before the per-entity MobBrain every simulation tick.
    virtual void tick(SimpleEntity& self, std::uint64_t& rng) const {
        static_cast<void>(self);
        static_cast<void>(rng);
    }

    // LivingEntity#damage → Angerable#setTarget: fired when the creature takes a
    // hit, the hook a neutral species overrides to turn on its attacker. Default
    // does nothing, so passive and hostile mobs ignore it; see NeutralMob.hpp.
    virtual void onAttacked(SimpleEntity& self, std::uint64_t& rng) const {
        static_cast<void>(self);
        static_cast<void>(rng);
    }
};

// A species' loot table, reduced to a roll against the entity's RNG stream.
using LootRoll = EntityDrops (*)(std::uint64_t& rng);

// The per-species boolean behaviours that decide whether a mechanic touches a
// creature at all: "does this type burn?", "does this type burn in daylight?".
// Vanilla scatters these across boolean fields and overridable methods
// (Entity#isFireImmune, the mob's own daylight-burn check); here they are one
// bit set on the type, so a mechanic tests one bit instead of a species switch.
//
// This is the single home for every "some class is exempt from some mechanic"
// question. When fall-immunity or water-breathing acquires a consumer it is one
// more bit here, never an `if (species == …)` at the mechanic's call site.
enum class EntityBehavior : std::uint16_t {
    // Entity#isFireImmune: fire, lava and the burn tick do nothing. Set by
    // nether natives (a future strider/blaze); the mechanic (EM1) reads it.
    FireImmune = 1U << 0U,
    // The daylight-burn exemption undead read: a husk does not catch fire in the
    // sun. The daylight ignition source itself is AR-M2; this is the bit it will
    // consult so the ignition never needs a species switch.
    SunImmune = 1U << 1U,
    // AR-A4: Entity#causeFallDamage's exemption a chicken (and, in vanilla, a
    // bat/parrot) reads — the landing tick in EntitySystem::tick skips the
    // fallDistance-to-damage conversion for any type with this bit rather than
    // naming the species at the call site.
    FallImmune = 1U << 2U,
    // AR-M2: the family the daylight-burn source (below) gates ignition on,
    // alongside MobCategory::Monster and !sunImmune(). Vanilla's own daylight
    // check is written against Zombie/AbstractSkeleton specifically, not every
    // hostile (a creeper or spider never burns) — this bit reproduces that
    // narrower family without a species switch at the ignition call site.
    // Zombie and husk both carry it; husk additionally carries SunImmune so
    // the ignition rule's `!sunImmune()` term skips it. Doubles as
    // LivingEntity#getGroup() == EntityGroup.UNDEAD (1.16.1): the same undead
    // family ENCH-1's Smite target-category gate reads
    // (DamageEnchantment#getAttackDamage's typeIndex==1 branch).
    Undead = 1U << 3U,
    // AR-M2: Husk#doHurtTarget's Hunger-on-hit. The mob-melee call site in
    // GameSession reads this bit off the attacker's type after a landed hit
    // and applies EM2's hunger effect — no `if (species == husk)` there either.
    HungerOnHit = 1U << 4U,
    // LivingEntity#getGroup() == EntityGroup.ARTHROPOD: the gate Bane of
    // Arthropods reads (DamageEnchantment#getAttackDamage's typeIndex==2 branch,
    // plus its Slowness-on-hit). No arthropod mob exists in this build yet
    // (spider/silverfish are AR content gaps), so nothing sets this today — the
    // bit and its accessor exist so the category check is honest (answers false
    // because no arthropod has been registered, not because the mechanic is
    // unimplemented) and so a future spider/silverfish needs only this one flag.
    Arthropod = 1U << 5U,
    // DYE-1: the "a dye right-clicked on this creature recolours it" family.
    // Vanilla's dye-on-mob branch lives on SheepEntity#mobInteract specifically
    // (it is the only DyeableItem-recolourable mob in 1.16.1) — this bit
    // reproduces that species-narrow gate without a `species == sheep` check at
    // the interaction call site, exactly as Undead/Arthropod do for their own
    // mechanics. A creature without this bit ignores a dye click entirely.
    Dyeable = 1U << 6U,
};

[[nodiscard]] constexpr std::uint16_t operator|(EntityBehavior a, EntityBehavior b) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(a) |
                                      static_cast<std::uint16_t>(b));
}

// AR-M2: lets a third (and later) bit chain onto the pair-wise overload above
// (`a | b | c`), which would otherwise fail to resolve once the left operand
// has already collapsed to the plain uint16_t the first `operator|` returns.
[[nodiscard]] constexpr std::uint16_t operator|(std::uint16_t a, EntityBehavior b) {
    return static_cast<std::uint16_t>(a | static_cast<std::uint16_t>(b));
}

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

    // The behaviour bit set, and the one-bit tests mechanics read. `fireImmune`
    // is consumed by EM1's fire state machine; `sunImmune` is reserved for the
    // daylight-burn source (AR-M2). Adding a mechanic exemption is a bit here.
    [[nodiscard]] std::uint16_t behaviorFlags() const { return behaviorFlags_; }
    [[nodiscard]] bool hasBehavior(EntityBehavior flag) const {
        return (behaviorFlags_ & static_cast<std::uint16_t>(flag)) != 0U;
    }
    [[nodiscard]] bool fireImmune() const { return hasBehavior(EntityBehavior::FireImmune); }
    [[nodiscard]] bool sunImmune() const { return hasBehavior(EntityBehavior::SunImmune); }
    // AR-A4: Entity#fall's landing-tick damage conversion skips a type with this
    // bit — see EntityBehavior::FallImmune.
    [[nodiscard]] bool fallImmune() const { return hasBehavior(EntityBehavior::FallImmune); }
    // AR-M2: the daylight-burn source's family gate — see EntityBehavior::Undead.
    [[nodiscard]] bool undead() const { return hasBehavior(EntityBehavior::Undead); }
    // AR-M2: whether a landed melee hit from this type applies EM2's hunger
    // effect to the player — see EntityBehavior::HungerOnHit.
    [[nodiscard]] bool hungerOnHit() const { return hasBehavior(EntityBehavior::HungerOnHit); }
    // The Smite / Bane of Arthropods target-category gates (ENCH-1). `isUndead()`
    // reads the same UNDEAD bit AR-M2's daylight burn does — one bit, two
    // consumers — so tagging a mob undead once serves both.
    [[nodiscard]] bool isUndead() const { return hasBehavior(EntityBehavior::Undead); }
    [[nodiscard]] bool isArthropod() const { return hasBehavior(EntityBehavior::Arthropod); }
    // DYE-1: SheepEntity#mobInteract's dye-recolour gate — see
    // EntityBehavior::Dyeable. The interaction call site reads this one bit off
    // the target's type instead of naming the sheep species.
    [[nodiscard]] bool dyeable() const { return hasBehavior(EntityBehavior::Dyeable); }
    // AgeableMob breeding parameters (EM-3). `breedable()` is the one-flag test
    // the AI/tick reads before installing or running any breeding logic.
    [[nodiscard]] const BreedingProfile& breeding() const { return breeding_; }
    [[nodiscard]] bool breedable() const { return breeding_.breedable; }
    // AR-A4: egg-laying parameters. `laysEggs()` is the one-flag test the
    // landing-tick egg scheduler reads before touching a creature's
    // eggLayTimer at all.
    [[nodiscard]] const EggLayProfile& eggLay() const { return eggLay_; }
    [[nodiscard]] bool laysEggs() const { return eggLay_.laysEggs; }
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
    [[nodiscard]] EntityDrops rollLoot(std::uint64_t& rng) const {
        return loot_ != nullptr ? loot_(rng) : EntityDrops{};
    }

    // Mob#getBaseExperienceReward (26.1): the experience a killed creature
    // awards, before XP-2's lastHurtByPlayer gate is even checked. Modelled as a
    // [min, max] range: an ordinary Mob has a flat reward (min == max, e.g. the
    // zombie's 5), but AnimalEntity#getBaseExperienceReward rolls `1 + random(3)`
    // — a 1..3 range — so cow/pig/sheep/chicken carry min=1, max=3. Zero (the
    // default, min == max == 0) is a creature that never drops experience. Only a
    // kill grants any, never a breed (that path is a flat 1-7 roll, not this
    // field). A data field on the type rather than a per-species switch at the
    // death call site, matching the fireImmune precedent.
    [[nodiscard]] std::int32_t xpReward() const { return xpReward_; }
    [[nodiscard]] std::int32_t xpRewardMax() const { return xpRewardMax_; }
    // Rolls the kill reward in [xpReward_, xpRewardMax_]; a flat reward (the
    // common case) never touches `rng`, so a deterministic stream is only
    // advanced for the animals that actually randomise (1..3).
    [[nodiscard]] std::int32_t rollExperienceReward(std::uint64_t& rng) const {
        if (xpRewardMax_ <= xpReward_) {
            return xpReward_;
        }
        return xpReward_ +
               static_cast<std::int32_t>(mc::rng::nextInt(
                   rng, static_cast<std::uint32_t>(xpRewardMax_ - xpReward_ + 1)));
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
    // The behaviour bit set (EntityBehavior). Zero means the creature is subject
    // to every mechanic, which is the default for ordinary land animals.
    std::uint16_t behaviorFlags_ = 0U;
    // AgeableMob breeding parameters; default is non-breedable.
    BreedingProfile breeding_{};
    // AR-A4: egg-laying parameters; default is a species that never lays.
    EggLayProfile eggLay_{};
    EntityRenderDescriptor render_{};
    audio::MobSoundProfile soundProfile_{};
    const EntityAi* ai_ = nullptr;
    LootRoll loot_ = nullptr;
    // Mob#getBaseExperienceReward; 0 (never drops experience) unless the species
    // states one. xpReward_ is the minimum (and the whole value for a flat
    // reward); xpRewardMax_ is the inclusive upper bound of the roll.
    std::int32_t xpReward_ = 0;
    std::int32_t xpRewardMax_ = 0;
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
    // Sets the whole attribute array at once — the form the species manifest
    // (batch import) uses, where a row already carries a built EntityAttributes.
    // The per-attribute setters below stay for the hand-written species classes.
    Builder& attributes(const EntityAttributes& attributes);
    Builder& health(float maxHealth);
    Builder& movementSpeed(float speed);
    Builder& attackDamage(float damage);
    Builder& followRange(float range);
    Builder& knockbackResistance(float resistance);
    Builder& spawnEgg(std::uint32_t primary, std::uint32_t secondary);
    // Sets a behaviour bit. Chainable, so a nether native reads
    // `.fireImmune()` and a husk `.sunImmune()`; a species that states nothing
    // is subject to every mechanic. The generic `behavior()` takes the enum for
    // the manifest/import path.
    Builder& behavior(EntityBehavior flag);
    Builder& fireImmune();
    Builder& sunImmune();
    // AR-A4: EntityBehavior::FallImmune — a chicken (and, in vanilla, a
    // bat/parrot) reads this to skip the landing-tick fall-damage conversion.
    Builder& fallImmune();
    // AR-M2: EntityBehavior::Undead — the family the daylight-ignition rule
    // gates on (zombie, husk); doubles as ENCH-1's Smite target-category marker.
    Builder& undead();
    // AR-M2: EntityBehavior::HungerOnHit — husk's melee applies EM2's hunger
    // effect to whatever it lands a hit on.
    Builder& hungerOnHit();
    // ENCH-1: EntityBehavior::Arthropod — the Bane of Arthropods target-category
    // marker (no arthropod mob exists yet, so nothing sets it today).
    Builder& arthropod();
    // Marks the species breedable and states its whole breeding profile (tempt
    // item + baby scale). AR-A hands each animal its wheat/seeds through this;
    // EM-3 owns everything the profile drives.
    Builder& breeding(const BreedingProfile& profile);
    // Shorthand: breedable with `temptItem` and the default 0.5 baby scale.
    Builder& breedableWith(const ItemStack& temptItem);
    // AR-A4: states the whole egg-laying profile (the dropped item). The
    // landing-tick scheduler (EntitySystem::tick) owns the timer/interval.
    Builder& eggLay(const EggLayProfile& profile);
    // Shorthand: lays `item` on the shared 6000-12000 tick interval.
    Builder& laysEggs(const ItemStack& item);
    Builder& loot(LootRoll roll);
    // Mob#getBaseExperienceReward: the flat experience a kill of this species
    // awards (XP-2's lastHurtByPlayer gate decides *whether* it is paid out, not
    // how much). A species that states nothing keeps the zero default.
    Builder& xpReward(std::int32_t amount);
    // AnimalEntity#getBaseExperienceReward's `1 + random(3)`: a kill awards a
    // value drawn uniformly from [min, max]. Passive animals use xpReward(1, 3).
    Builder& xpReward(std::int32_t min, std::int32_t max);
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
