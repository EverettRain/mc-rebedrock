#include "gameplay/entities/BuiltinSpecies.hpp"

#include "gameplay/Item.hpp"
#include "gameplay/Random.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "gameplay/entities/MobAi.hpp"
#include "gameplay/entities/MobBrain.hpp"
#include "world/Block.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <memory>

namespace mc::gameplay::entities {
namespace {

// --- shared behaviour ----------------------------------------------------

// Passive land animals reuse AnimalAi wholesale (Swim/EscapeDanger/Wander/Look),
// exactly as Pig and Cow do; only the panic-speed multiplier differs per species.
const AnimalAi kPigAi{1.25F, 1.0F, 1};     // pig PanicGoal runs at 1.25
const AnimalAi kCowAi{2.0F, 1.0F, 0};      // cow PanicGoal runs at 2.0
const AnimalAi kChickenAi{1.4F, 1.0F, 0};  // chicken PanicGoal runs at 1.4

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

// Pig.json (26.1): one to three raw porkchops. (Vanilla drops the cooked cut
// when the pig dies on fire; that path is not modelled here — the same
// simplification every other meat drop in this table makes.)
EntityDrops rollPigLoot(std::uint64_t& rng) {
    EntityDrops drops;
    const auto count = static_cast<std::uint8_t>(1U + mc::rng::nextInt(rng, 3U));
    drops.add({world::Block::Air, count, &items::Porkchop});
    return drops;
}

// Cow.json (26.1): one pool rolls 0-2 leather, the other 1-3 raw beef.
EntityDrops rollCowLoot(std::uint64_t& rng) {
    EntityDrops drops;
    const auto beefCount = static_cast<std::uint8_t>(1U + mc::rng::nextInt(rng, 3U));
    const auto leatherCount = static_cast<std::uint8_t>(mc::rng::nextInt(rng, 3U));
    drops.add({world::Block::Air, beefCount, &items::Beef});
    drops.add({world::Block::Air, leatherCount, &items::Leather});
    return drops;
}

// Chicken.json (26.1): one raw chicken plus 0-2 feathers. (Vanilla swaps the
// raw chicken for cooked when the chicken dies on fire; that path is not
// modelled here, the same simplification the pig/cow rows above note.)
EntityDrops rollChickenLoot(std::uint64_t& rng) {
    EntityDrops drops;
    const auto feathers = static_cast<std::uint8_t>(mc::rng::nextInt(rng, 3U));
    drops.add({world::Block::Air, 1U, &items::RawChicken});
    drops.add({world::Block::Air, feathers, &items::Feather});
    return drops;
}

// Sheep.json (26.1): 1-2 mutton plus exactly one wool block, tinted by the
// sheep's dye colour. DYE-2: the loot roll has no access to the dying entity, so
// it emits a white-wool placeholder that EntitySystem::die retints to the mob's
// authoritative DyeColor (woolBlockFor) before the drop spawns — the kill-path
// mirror of the shear path in PlayerInteraction. A white sheep keeps white wool.
EntityDrops rollSheepLoot(std::uint64_t& rng) {
    EntityDrops drops;
    const auto mutton = static_cast<std::uint8_t>(1U + mc::rng::nextInt(rng, 2U));
    drops.add({world::Block::Air, mutton, &items::Mutton});
    drops.add({world::Block::WhiteWool, 1U, blockItemFor(world::Block::WhiteWool)});
    return drops;
}

// Zombie.json / Husk.json (26.1): 0-2 rotten flesh and nothing else (no
// armour/equipment table exists yet, AR-M2). One function serves both rows —
// 26.1's Husk reuses LootTables.ZOMBIE verbatim.
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

constexpr EntityRenderDescriptor kPigRender{
    /*geometryPath=*/"animation/pig.geo.json",
    /*animationPath=*/"animation/pig.animation.json",
    /*texturePath=*/"entity/pig/pig_temperate.png",
    /*geometryId=*/"geometry.pig",
    /*walkAnimation=*/"animation.pig.walk",
    /*idleAnimation=*/"animation.pig.idle",
    /*scale=*/1.0F,
};

// Java 26.1 splits the old cow skin into biome variants; the plains/default
// variant is the temperate texture.
constexpr EntityRenderDescriptor kCowRender{
    /*geometryPath=*/"animation/cow.geo.json",
    /*animationPath=*/"animation/cow.animation.json",
    /*texturePath=*/"entity/cow/cow_temperate.png",
    /*geometryId=*/"geometry.cow",
    /*walkAnimation=*/"animation.cow.walk",
    /*idleAnimation=*/"animation.cow.idle",
    /*scale=*/1.0F,
};

constexpr EntityRenderDescriptor kZombieRender{
    /*geometryPath=*/"animation/zombie.geo.json",
    /*animationPath=*/"animation/zombie.animation.json",
    /*texturePath=*/"entity/zombie/zombie.png",
    /*geometryId=*/"geometry.zombie",
    /*walkAnimation=*/"animation.zombie.walk",
    /*idleAnimation=*/"animation.zombie.idle",
    /*scale=*/1.0F,
};

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

constexpr audio::MobSoundProfile kPigSounds{
    "entity.pig.ambient", "entity.pig.hurt", "entity.pig.death",
    "entity.pig.step",    1.0F,              0.15F,
};
constexpr audio::MobSoundProfile kCowSounds{
    "entity.cow.ambient", "entity.cow.hurt", "entity.cow.death",
    "entity.cow.step",    0.4F,              0.15F,
};
constexpr audio::MobSoundProfile kZombieSounds{
    "entity.zombie.ambient", "entity.zombie.hurt", "entity.zombie.death",
    "entity.zombie.step",    1.0F,                 0.15F,
};
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

const std::array<SpeciesDef, 6> kManifest{{
    // Pig (26.1): 10 health, MOVEMENT_SPEED 0.25, box 0.9 x 0.9, egg tint
    // 0xF0A5A5 / 0xDB635E. Drops 1-3 raw porkchops. Not breedable yet (26.1
    // tempts a pig with a carrot, an item this build does not have).
    SpeciesDef{
        /*path=*/"pig", /*vanillaName=*/"pig", MobCategory::Creature,
        SpawnPlacement::OnGround, EntityDimensions{0.9F, 0.9F},
        attributesOf(10.0F, 0.25F, 0.0F, 16.0F), /*hasSpawnEgg=*/true,
        SpawnEggColors{0xF0A5A5U, 0xDB635EU}, kPigRender, kPigSounds, &kPigAi, &rollPigLoot,
        /*breeding=*/BreedingProfile{}, /*behaviorFlags=*/0U, /*eggLay=*/EggLayProfile{},
        // AnimalEntity#getBaseExperienceReward: 1..3 on a player kill.
        /*xpRewardMin=*/1, /*xpRewardMax=*/3},
    // Cow (26.1): AbstractCow.createAttributes() gives 10 health and
    // MOVEMENT_SPEED 0.2; box 0.9 x 1.4, egg tint 0xF3C9A3 / 0xFFFFFF. Drops
    // beef + leather. AR-A3: breedable, tempted by wheat (Animal.TEMPT_INGREDIENT
    // for cows), calf baby scale 0.5 (EM-3's default). EM-3's
    // installBreedingGoals reads this generically off the type, so no
    // cow-specific goal wiring exists anywhere.
    SpeciesDef{
        /*path=*/"cow", /*vanillaName=*/"cow", MobCategory::Creature,
        SpawnPlacement::OnGround, EntityDimensions{0.9F, 1.4F},
        attributesOf(10.0F, 0.20F, 0.0F, 16.0F), /*hasSpawnEgg=*/true,
        SpawnEggColors{0xF3C9A3U, 0xFFFFFFU}, kCowRender, kCowSounds, &kCowAi, &rollCowLoot,
        /*breeding=*/BreedingProfile{/*breedable=*/true,
                                     /*temptItem=*/ItemStack{world::Block::Air, 1U, &items::Wheat},
                                     /*babyScale=*/0.5F},
        /*behaviorFlags=*/0U, /*eggLay=*/EggLayProfile{},
        /*xpRewardMin=*/1, /*xpRewardMax=*/3},
    // Zombie (26.1): Zombie.createAttributes() gives 20 health, follow range 35,
    // MOVEMENT_SPEED 0.23 and attack damage 3; box 0.6 x 1.95, egg tint
    // 0x00AFAF / 0x799C65, xpReward 5 (Mob's DEFAULT_XP_REWARD, which Zombie
    // inherits unchanged). Melee like the husk below, so it shares
    // kMeleeMonsterAi. AR-M2: Undead — the family the daylight-ignition rule
    // (EntitySystem::tick) gates on, and ENCH-1's Smite target category
    // (getGroup() == UNDEAD). It carries no SunImmune bit, so unlike the husk
    // it burns.
    SpeciesDef{
        /*path=*/"zombie", /*vanillaName=*/"zombie", MobCategory::Monster,
        SpawnPlacement::OnGround, EntityDimensions{0.6F, 1.95F},
        attributesOf(20.0F, 0.23F, 3.0F, 35.0F), /*hasSpawnEgg=*/true,
        SpawnEggColors{0x00AFAFU, 0x799C65U}, kZombieRender, kZombieSounds, &kMeleeMonsterAi,
        &rollRottenFleshLoot, /*breeding=*/BreedingProfile{},
        /*behaviorFlags=*/static_cast<std::uint16_t>(EntityBehavior::Undead),
        /*eggLay=*/EggLayProfile{}, /*xpRewardMin=*/5, /*xpRewardMax=*/5},
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
                                 ItemStack{world::Block::Air, 1U, &items::Egg}},
        /*xpRewardMin=*/1, /*xpRewardMax=*/3},
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
                                     /*babyScale=*/0.5F},
        // DYE-1: EntityBehavior::Dyeable — the sheep is the one recolourable mob
        // (SheepEntity#mobInteract's DyeItem branch). The dye interaction reads
        // this bit off the type, so this row is the whole "sheep can be dyed"
        // wiring — no species check anywhere else.
        /*behaviorFlags=*/static_cast<std::uint16_t>(EntityBehavior::Dyeable),
        /*eggLay=*/EggLayProfile{}, /*xpRewardMin=*/1, /*xpRewardMax=*/3},
    // Husk (26.1): a desert zombie — 20 health, follow range 35, MOVEMENT_SPEED
    // 0.23, attack 3, box 0.6 x 1.95, egg tint 0x797061 / 0x66907B. Melee like
    // the zombie; AR-M1 wires its loot to the same 0-2 rotten flesh pool.
    // AR-M2: undead() so the daylight-ignition rule considers it at all, plus
    // sunImmune() so that same rule skips it. The SunImmune EntityBehavior bit
    // was introduced in AR-M2 (656d040) together with the ignition source that
    // reads it — it was NOT reserved by an earlier EM1/AR-M1 pass; this is the
    // first and only species that sets it. hungerOnHit(): Husk#doHurtTarget
    // applies EM2's Hunger
    // effect to whatever it lands a melee hit on; the zombie beside it in this
    // manifest carries no such bit, so only husk's hit does. The same Undead bit
    // is ENCH-1's Smite target-category gate (getGroup() == UNDEAD, HuskEntity
    // extends the zombie and never overrides getGroup).
    SpeciesDef{
        /*path=*/"husk", /*vanillaName=*/"husk", MobCategory::Monster,
        SpawnPlacement::OnGround, EntityDimensions{0.6F, 1.95F},
        attributesOf(20.0F, 0.23F, 3.0F, 35.0F), /*hasSpawnEgg=*/true,
        SpawnEggColors{0x797061U, 0x66907BU}, kHuskRender, kHuskSounds, &kMeleeMonsterAi,
        &rollRottenFleshLoot, /*breeding=*/BreedingProfile{},
        /*behaviorFlags=*/static_cast<std::uint16_t>(EntityBehavior::Undead |
                                                       EntityBehavior::SunImmune |
                                                       EntityBehavior::HungerOnHit),
        /*eggLay=*/EggLayProfile{}, /*xpRewardMin=*/5, /*xpRewardMax=*/5},
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
    if (def.xpRewardMax > 0) {
        builder.xpReward(def.xpRewardMin, def.xpRewardMax);
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

const EntityType& builtinSpecies(std::string_view path) {
    registerBuiltinSpeciesManifest();
    const EntityType* type = entityTypeRegistry().byId(path);
    if (type == nullptr) {
        std::cerr << "No built-in species named '" << path << "' in the manifest\n";
        std::abort();
    }
    return *type;
}

} // namespace mc::gameplay::entities
