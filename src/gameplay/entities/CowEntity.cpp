#include "gameplay/entities/CowEntity.hpp"

#include "gameplay/EntitySystem.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "gameplay/entities/MobAi.hpp"

#include <cstdint>

namespace mc::gameplay::entities {
namespace {

// CowAi installs AnimalAi's per-entity Swim/EscapeDanger/Wander/Look goals with
// the vanilla 2.0 escape multiplier. AnimalMateGoal and TemptGoal remain future
// additions. The shared profile carries no mutable runtime state.
class CowAi final : public AnimalAi {
  public:
    // AbstractCow uses priorities 5/6/7 for wander/look/look-around in 26.1.
    CowAi() : AnimalAi(2.0F, 1.0F, 0) {}
};

const CowAi kCowAi;

// CowEntity's loot table, straight from vanilla data/minecraft/loot_tables/
// entities/cow.json: one pool rolls 0-2 leather, the other 1-3 raw beef. (Vanilla
// swaps the beef for cooked steak when the cow dies on fire; that path is not
// modelled here.)
EntityDrops rollCowLoot(std::uint32_t& rng) {
    EntityDrops drops;
    const auto beefCount = static_cast<std::uint8_t>(1U + (nextRandom(rng) >> 8) % 3U);
    const auto leatherCount = static_cast<std::uint8_t>((nextRandom(rng) >> 8) % 3U);
    drops.add({world::Block::Air, beefCount, &items::Beef});
    drops.add({world::Block::Air, leatherCount, &items::Leather});
    return drops;
}

// The cow's box-UV model, its skin, and the bedrock identifiers the JSON
// declares. Literals have static storage, so the descriptor's string_views stay
// valid for the life of the program. Java 26.1 splits the old cow skin into
// biome variants; the plains/default variant is the temperate texture.
constexpr EntityRenderDescriptor kCowRender{
    /*geometryPath=*/"animation/cow.geo.json",
    /*animationPath=*/"animation/cow.animation.json",
    /*texturePath=*/"entity/cow/cow_temperate.png",
    /*geometryId=*/"geometry.cow",
    /*walkAnimation=*/"animation.cow.walk",
    /*idleAnimation=*/"animation.cow.idle",
    /*scale=*/1.0F,
};

// Physical clips and variations are selected by the active packs' sounds.json.
constexpr audio::MobSoundProfile kCowSounds{
    /*ambientEvent=*/"entity.cow.ambient",
    /*hurtEvent=*/"entity.cow.hurt",
    /*deathEvent=*/"entity.cow.death",
    /*stepEvent=*/"entity.cow.step",
    /*volume=*/0.4F,
    /*stepVolume=*/0.15F,
};

} // namespace

const EntityType& CowEntity::type() {
    // AbstractCow.createAttributes() (26.1): 10 health, MOVEMENT_SPEED 0.2.
    // Box 0.9 x 1.4. Spawn-egg tint 0xF3C9A3 / 0xFFFFFF.
    // AR-A3: breedable, tempted by wheat (Animal.TEMPT_INGREDIENT for cows),
    // calf baby scale 0.5 (EM-3's default) — the same shorthand AR-A2 used for
    // sheep. EM-3's installBreedingGoals reads this generically off the type,
    // so no cow-specific goal wiring is needed here.
    static EntityType type = EntityType::Builder::create(MobCategory::Creature, kCowAi)
                                 .sized(0.9F, 1.4F)
                                 .health(10.0F)
                                 .movementSpeed(0.20F)
                                 .followRange(16.0F)
                                 .spawnEgg(0xF3C9A3U, 0xFFFFFFU)
                                 .loot(&rollCowLoot)
                                 .renderer(kCowRender)
                                 .sounds(kCowSounds)
                                 .breedableWith(ItemStack{world::Block::Air, 1U, &items::Wheat})
                                 .vanillaName("cow")
                                 .build("cow");
    // File it in the registry exactly once, passing the static's stable address.
    static const bool registered = [] {
        entityTypeRegistry().registerBuiltin(type);
        return true;
    }();
    static_cast<void>(registered);
    return type;
}

} // namespace mc::gameplay::entities
