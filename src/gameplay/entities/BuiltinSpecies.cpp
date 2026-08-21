#include "gameplay/entities/BuiltinSpecies.hpp"

#include "gameplay/Item.hpp"
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
const AnimalAi kSheepAi{1.25F, 1.0F, 0};  // sheep PanicGoal runs at 1.25

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

// Chicken.json: 0-2 feathers (and raw chicken, an item this build lacks, so the
// meat pool is dropped like the zombie's rotten flesh).
EntityDrops rollChickenLoot(std::uint32_t& rng) {
    EntityDrops drops;
    const auto feathers = static_cast<std::uint8_t>((nextRandom(rng) >> 8) % 3U);
    drops.add({world::Block::Air, feathers, &items::Feather});
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
    /*texturePath=*/"entity/chicken/chicken.png",
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
};

constexpr EntityRenderDescriptor kHuskRender{
    /*geometryPath=*/"animation/husk.geo.json",
    /*animationPath=*/"animation/husk.animation.json",
    /*texturePath=*/"entity/zombie/husk.png",
    /*geometryId=*/"geometry.husk",
    /*walkAnimation=*/"animation.husk.walk",
    /*idleAnimation=*/"animation.husk.idle",
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
    // 0xA1A1A1 / 0xFF0000.
    SpeciesDef{
        /*path=*/"chicken", /*vanillaName=*/"chicken", MobCategory::Creature,
        SpawnPlacement::OnGround, EntityDimensions{0.4F, 0.7F},
        attributesOf(4.0F, 0.25F, 0.0F, 16.0F), /*hasSpawnEgg=*/true,
        SpawnEggColors{0xA1A1A1U, 0xFF0000U}, kChickenRender, kChickenSounds, &kChickenAi,
        &rollChickenLoot},
    // Sheep (26.1): 8 health, MOVEMENT_SPEED 0.23, box 0.9 x 1.3, egg tint
    // 0xE7E7E7 / 0xFFB5B5. Wool/mutton need items this build lacks: no loot yet.
    SpeciesDef{
        /*path=*/"sheep", /*vanillaName=*/"sheep", MobCategory::Creature,
        SpawnPlacement::OnGround, EntityDimensions{0.9F, 1.3F},
        attributesOf(8.0F, 0.23F, 0.0F, 16.0F), /*hasSpawnEgg=*/true,
        SpawnEggColors{0xE7E7E7U, 0xFFB5B5U}, kSheepRender, kSheepSounds, &kSheepAi, nullptr},
    // Husk (26.1): a desert zombie — 20 health, follow range 35, MOVEMENT_SPEED
    // 0.23, attack 3, box 0.6 x 1.95, egg tint 0x797061 / 0x66907B. Melee like
    // the zombie; rotten flesh needs an item this build lacks, so no loot.
    SpeciesDef{
        /*path=*/"husk", /*vanillaName=*/"husk", MobCategory::Monster,
        SpawnPlacement::OnGround, EntityDimensions{0.6F, 1.95F},
        attributesOf(20.0F, 0.23F, 3.0F, 35.0F), /*hasSpawnEgg=*/true,
        SpawnEggColors{0x797061U, 0x66907BU}, kHuskRender, kHuskSounds, &kMeleeMonsterAi, nullptr},
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
