#include "gameplay/entities/BuiltinSpecies.hpp"

#include "gameplay/Item.hpp"
#include "gameplay/Random.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "gameplay/entities/MobAi.hpp"
#include "gameplay/entities/MobBrain.hpp"
#include "world/Block.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <memory>

namespace mc::gameplay::entities {
namespace {

// --- shared behaviour ----------------------------------------------------

// Passive land animals reuse AnimalAi wholesale (Swim/EscapeDanger/Wander/Look),
// exactly as Pig and Cow do; only the panic-speed multiplier differs per species.
const AnimalAi kChickenAi{1.4F, 1.0F, 0}; // chicken PanicGoal runs at 1.4

// AR-A2: the one thing sheep do that no other AnimalAi species does yet — a
// sheared sheep regrows its wool by eating grass. This is exactly the
// "a creature that does something new gets a fresh EntityAi" case the
// MeleeMonsterAi comment below describes: AnimalAi's shared Swim/Escape/
// Wander/Look stays wholesale (installed via the base configureBrain call),
// only EatGrassGoal is new. Priority 5 matches vanilla's own EatBlockGoal,
// tied with WanderAroundFarGoal (also 5, both Move-controlled): GoalSelector's
// `<=` preemption rule means neither can interrupt the other once running, so
// whichever canStart wins the tick keeps running until it naturally finishes
// — the same "one passive action at a time" shape vanilla's own tie produces.
class SheepAi final : public AnimalAi {
  public:
    SheepAi() : AnimalAi(1.25F, 1.0F, 0) {} // sheep PanicGoal runs at 1.25

    void configureBrain(MobBrain& brain) const override {
        AnimalAi::configureBrain(brain);
        brain.goals().add(5, std::make_unique<EatGrassGoal>());
    }
};

const SheepAi kSheepAi;

// A melee hostile: MonsterAi's idle fallback plus the same player-acquire + melee
// goals the zombie installs. A husk fights hand-to-hand like a zombie, so it is
// pure reuse — no new EntityAi. A creature that did something new (a creeper's
// detonation, a skeleton's bow) is where a fresh EntityAi would go instead.
class MeleeMonsterAi final : public MonsterAi {
  public:
    void configureBrain(MobBrain& brain) const override {
        MonsterAi::configureBrain(brain);
        brain.goals().add(2, std::make_unique<MeleeAttackGoal>(1.0F));
        brain.targets().add(2, std::make_unique<ActiveTargetPlayerGoal>());
    }
};

const MeleeMonsterAi kMeleeMonsterAi;

// --- loot ----------------------------------------------------------------

// Chicken.json (26.1): one raw chicken plus 0-2 feathers. (Vanilla swaps the
// raw chicken for cooked when the chicken dies on fire; that path is not
// modelled here, same simplification CowEntity's loot notes.)
EntityDrops rollChickenLoot(std::uint64_t& rng) {
    EntityDrops drops;
    const auto feathers = static_cast<std::uint8_t>(mc::rng::nextInt(rng, 3U));
    drops.add({world::Block::Air, 1U, &items::RawChicken});
    drops.add({world::Block::Air, feathers, &items::Feather});
    return drops;
}

// Sheep.json (26.1): 1-2 mutton plus exactly one wool block, tinted by the
// sheep's dye colour. Colour variants (dye/AR-A2 shearing) are out of scope
// here — every sheep this manifest spawns drops white wool, the default a
// freshly-spawned sheep carries before any dye interaction exists.
EntityDrops rollSheepLoot(std::uint64_t& rng) {
    EntityDrops drops;
    const auto mutton = static_cast<std::uint8_t>(1U + mc::rng::nextInt(rng, 2U));
    drops.add({world::Block::Air, mutton, &items::Mutton});
    drops.add({world::Block::WhiteWool, 1U, blockItemFor(world::Block::WhiteWool)});
    return drops;
}

// Husk.json (26.1): 0-2 rotten flesh, the same pool zombie.json rolls — a
// husk drops nothing else (no armour/equipment table exists yet, AR-M2).
// Shared with the zombie's own loot fn below since both tables are identical
// in 26.1 (LootTables.ZOMBIE reused verbatim by Husk).
EntityDrops rollRottenFleshLoot(std::uint64_t& rng) {
    EntityDrops drops;
    const auto count = static_cast<std::uint8_t>(mc::rng::nextInt(rng, 3U));
    drops.add({world::Block::Air, count, &items::RottenFlesh});
    return drops;
}

// --- render descriptors --------------------------------------------------
//
// The bedrock model/skin/animation ids a renderer would bind. Like the zombie,
// the assets are not shipped yet, so these creatures do not appear in-game; the
// descriptor is data proving the render interface carries them. String literals
// have static storage, so the descriptor's views outlive the type.

constexpr EntityRenderDescriptor kChickenRender{
    /*geometryPath=*/"animation/chicken.geo.json",
    /*animationPath=*/"animation/chicken.animation.json",
    /*texturePath=*/"entity/chicken/chicken_temperate.png",
    /*geometryId=*/"geometry.chicken",
    /*walkAnimation=*/"animation.chicken.walk",
    /*idleAnimation=*/"animation.chicken.idle",
    /*scale=*/1.0F,
};

constexpr EntityRenderDescriptor kSheepRender{
    /*geometryPath=*/"animation/sheep.geo.json",
    /*animationPath=*/"animation/sheep.animation.json",
    /*texturePath=*/"entity/sheep/sheep.png",
    /*geometryId=*/"geometry.sheep",
    /*walkAnimation=*/"animation.sheep.walk",
    /*idleAnimation=*/"animation.sheep.idle",
    /*scale=*/1.0F,
    // The fleece: "wool"-prefixed bones sample this second skin, so the wool
    // reads as wool instead of an inflated copy of the body texture. White for
    // now (dye tinting is a later step); loaded from the pack, never bundled.
    /*secondaryTexturePath=*/"entity/sheep/sheep_wool.png",
};

// AR-M1 Tier B: husk reuses the zombie's own geometry/animation wholesale
// (26.1's `humanoid` model, same box-UV body a husk shares with a zombie in
// vanilla too — HuskModel extends ZombieModel with no new geometry) rather
// than pointing at a husk.geo/animation.json this build never shipped. Only
// the texture path differs, and even that is a filename the pack stack does
// not carry yet: buildSpeciesSkin degrades to the same procedural box-UV
// placeholder the zombie itself renders with when no pack skin exists. A real
// sandy husk skin, and the walk-cycle geometry actually looking right in the
// desert at night, are both 待 mac — see task report.
constexpr EntityRenderDescriptor kHuskRender{
    /*geometryPath=*/"animation/zombie.geo.json",
    /*animationPath=*/"animation/zombie.animation.json",
    /*texturePath=*/"entity/zombie/husk.png",
    /*geometryId=*/"geometry.zombie",
    /*walkAnimation=*/"animation.zombie.walk",
    /*idleAnimation=*/"animation.zombie.idle",
    /*scale=*/1.0F,
};

// --- sounds --------------------------------------------------------------

constexpr audio::MobSoundProfile kChickenSounds{
    "entity.chicken.ambient", "entity.chicken.hurt", "entity.chicken.death",
    "entity.chicken.step",    1.0F,                  0.15F,
};
constexpr audio::MobSoundProfile kSheepSounds{
    "entity.sheep.ambient", "entity.sheep.hurt", "entity.sheep.death",
    "entity.sheep.step",    1.0F,                0.15F,
};
constexpr audio::MobSoundProfile kHuskSounds{
    "entity.husk.ambient", "entity.husk.hurt", "entity.husk.death",
    "entity.husk.step",    1.0F,               0.15F,
};

// --- attribute helper ----------------------------------------------------

// A manifest row states its numbers as a built EntityAttributes; this spells one
// out by attribute so the table stays readable.
[[nodiscard]] constexpr EntityAttributes attributesOf(float maxHealth, float movementSpeed,
                                                      float attackDamage, float followRange) {
    EntityAttributes attributes;
    attributes.set(Attribute::MaxHealth, maxHealth);
    attributes.set(Attribute::MovementSpeed, movementSpeed);
    attributes.set(Attribute::AttackDamage, attackDamage);
    attributes.set(Attribute::FollowRange, followRange);
    return attributes;
}

// --- the manifest --------------------------------------------------------

const std::array<SpeciesDef, 3> kManifest{{
    // Chicken (26.1): 4 health, MOVEMENT_SPEED 0.25, box 0.4 x 0.7, egg tint
    // 0xA1A1A1 / 0xFF0000. AR-A4: breedable, tempted by (and bred with) wheat
    // seeds, chick baby scale 0.5 (EM-3's default); fall-immune
    // (EntityBehavior::FallImmune — ChickenEntity#causeFallDamage is a no-op in
    // vanilla); lays an Egg on the shared 6000-12000 tick timer
    // (EggLayProfile, EntitySystem's egg scheduler).
    SpeciesDef{
        /*path=*/"chicken", /*vanillaName=*/"chicken", MobCategory::Creature,
        SpawnPlacement::OnGround, EntityDimensions{0.4F, 0.7F},
        attributesOf(4.0F, 0.25F, 0.0F, 16.0F), /*hasSpawnEgg=*/true,
        SpawnEggColors{0xA1A1A1U, 0xFF0000U}, kChickenRender, kChickenSounds, &kChickenAi,
        &rollChickenLoot,
        /*breeding=*/BreedingProfile{/*breedable=*/true,
                                     /*temptItem=*/ItemStack{world::Block::Air, 1U,
                                                             &items::WheatSeeds},
                                     /*babyScale=*/0.5F},
        /*behaviorFlags=*/static_cast<std::uint16_t>(EntityBehavior::FallImmune),
        /*eggLay=*/EggLayProfile{/*laysEggs=*/true,
                                 ItemStack{world::Block::Air, 1U, &items::Egg}}},
    // Sheep (26.1): 8 health, MOVEMENT_SPEED 0.23, box 0.9 x 1.3, egg tint
    // 0xE7E7E7 / 0xFFB5B5. Drops mutton + white wool (rollSheepLoot). AR-A2:
    // breedable, tempted by wheat, lamb baby scale 0.5 (EM-3's default).
    SpeciesDef{
        /*path=*/"sheep", /*vanillaName=*/"sheep", MobCategory::Creature,
        SpawnPlacement::OnGround, EntityDimensions{0.9F, 1.3F},
        attributesOf(8.0F, 0.23F, 0.0F, 16.0F), /*hasSpawnEgg=*/true,
        SpawnEggColors{0xE7E7E7U, 0xFFB5B5U}, kSheepRender, kSheepSounds, &kSheepAi,
        &rollSheepLoot,
        /*breeding=*/BreedingProfile{/*breedable=*/true,
                                     /*temptItem=*/ItemStack{world::Block::Air, 1U, &items::Wheat},
                                     /*babyScale=*/0.5F}},
    // Husk (26.1): a desert zombie — 20 health, follow range 35, MOVEMENT_SPEED
    // 0.23, attack 3, box 0.6 x 1.95, egg tint 0x797061 / 0x66907B. Melee like
    // the zombie; AR-M1 wires its loot to the same 0-2 rotten flesh pool.
    // AR-M2: undead() so the daylight-ignition rule considers it at all, plus
    // sunImmune() so that same rule skips it — the whole reason the SunImmune
    // bit exists (EM1). hungerOnHit(): Husk#doHurtTarget applies EM2's Hunger
    // effect to whatever it lands a melee hit on; the zombie beside it in this
    // manifest carries no such bit, so only husk's hit does.
    SpeciesDef{
        /*path=*/"husk", /*vanillaName=*/"husk", MobCategory::Monster,
        SpawnPlacement::OnGround, EntityDimensions{0.6F, 1.95F},
        attributesOf(20.0F, 0.23F, 3.0F, 35.0F), /*hasSpawnEgg=*/true,
        SpawnEggColors{0x797061U, 0x66907BU}, kHuskRender, kHuskSounds, &kMeleeMonsterAi,
        &rollRottenFleshLoot, /*breeding=*/BreedingProfile{},
        /*behaviorFlags=*/static_cast<std::uint16_t>(EntityBehavior::Undead |
                                                       EntityBehavior::SunImmune |
                                                       EntityBehavior::HungerOnHit)},
}};

// Builds one manifest row into an immutable EntityType. The mechanical
// translation the manifest exists to make trivial: no per-species code path.
[[nodiscard]] EntityType buildFromDef(const SpeciesDef& def) {
    EntityType::Builder builder = EntityType::Builder::create(def.category, *def.ai)
                                      .sized(def.dimensions.width, def.dimensions.height)
                                      .attributes(def.attributes)
                                      .renderer(def.render)
                                      .sounds(def.sounds)
                                      .vanillaName(def.vanillaName);
    if (def.placement != SpawnPlacement::OnGround) {
        builder.spawnPlacement(def.placement);
    }
    if (def.hasSpawnEgg) {
        builder.spawnEgg(def.spawnEgg.primary, def.spawnEgg.secondary);
    }
    if (def.loot != nullptr) {
        builder.loot(def.loot);
    }
    if (def.breeding.breedable) {
        builder.breeding(def.breeding);
    }
    if (def.behaviorFlags != 0U) {
        builder.behavior(static_cast<EntityBehavior>(def.behaviorFlags));
    }
    if (def.eggLay.laysEggs) {
        builder.eggLay(def.eggLay);
    }
    return builder.build(def.path);
}

} // namespace

std::span<const SpeciesDef> builtinSpeciesManifest() { return kManifest; }

void registerBuiltinSpeciesManifest() {
    // The built EntityTypes need a stable address for the run (the registry files
    // pointers, SimpleEntity holds one). A std::deque never moves an existing
    // element on push, so every type stays put. Filled exactly once.
    static std::deque<EntityType> storage;
    static const bool registered = [] {
        // Iterate the manifest accessor, not the table directly, so the list is
        // the single source of truth: a row absent from it is absent from the
        // build (byId stops resolving it).
        for (const SpeciesDef& def : builtinSpeciesManifest()) {
            storage.push_back(buildFromDef(def));
            entityTypeRegistry().registerBuiltin(storage.back());
        }
        return true;
    }();
    static_cast<void>(registered);
}

} // namespace mc::gameplay::entities
