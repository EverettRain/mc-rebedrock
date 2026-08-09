#include "gameplay/entities/PigEntity.hpp"

#include "gameplay/EntitySystem.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "gameplay/entities/MobAi.hpp"

#include <cstdint>

namespace mc::gameplay::entities {
namespace {

// PigAi installs AnimalAi's per-entity Swim/EscapeDanger/Wander/Look goals with
// the vanilla 1.25 escape multiplier. Breeding, temptation and following parents
// remain future goals. The shared profile carries no mutable runtime state.
class PigAi final : public AnimalAi {
  public:
    // PigEntity uses priorities 6/7/8 for wander/look/look-around in 1.16.1.
    PigAi() : AnimalAi(1.25F, 1.0F, 1) {}
};

const PigAi kPigAi;

// PigEntity's loot table: one to three raw porkchops. (Vanilla drops the cooked
// cut when the pig dies on fire; that path is not modelled here.)
EntityDrops rollPigLoot(std::uint32_t& rng) {
    EntityDrops drops;
    const auto count = static_cast<std::uint8_t>(1U + (nextRandom(rng) >> 8) % 3U);
    drops.add({world::Block::Air, count, &items::Porkchop});
    return drops;
}

// The pig's box-UV model, its skin, and the bedrock identifiers the JSON
// declares. Literals have static storage, so the descriptor's string_views stay
// valid for the life of the program.
constexpr EntityRenderDescriptor kPigRender{
    /*geometryPath=*/"animation/pig.geo.json",
    /*animationPath=*/"animation/pig.animation.json",
    /*texturePath=*/"entity/pig/pig.png",
    /*geometryId=*/"geometry.pig",
    /*walkAnimation=*/"animation.pig.walk",
    /*idleAnimation=*/"animation.pig.idle",
    /*scale=*/1.0F,
};

// PigEntity's sound hooks (1.16.1): ambient mob/pig/say1-3, and hurt reuses the
// same three say clips (PigEntity has no distinct hurt sound), death is the
// single mob/pig/death.ogg, steps mob/pig/step1-5. Default getSoundVolume 1.0.
constexpr audio::MobSoundProfile kPigSounds{
    /*root=*/"mob/pig",
    /*ambientBase=*/"say",   /*ambientVariations=*/3,
    /*hurtBase=*/"say",      /*hurtVariations=*/3,
    /*deathBase=*/"death",   /*deathVariations=*/1,
    /*stepBase=*/"step",     /*stepVariations=*/5,
    /*volume=*/1.0F,
    /*stepVolume=*/0.15F,
};

} // namespace

const EntityType& PigEntity::type() {
    // PigEntity: 0.9 x 0.9 box, ten health, GENERIC_MOVEMENT_SPEED 0.25 folded
    // into the walking tier's blocks-per-tick, vanilla egg colours 0xF0A5A5 /
    // 0xDB635E. Built once; its address is stable for the run.
    static EntityType type = EntityType::Builder::create(MobCategory::Creature, kPigAi)
                                 .sized(0.9F, 0.9F)
                                 .health(10.0F)
                                 .movementSpeed(0.05F)
                                 .followRange(16.0F)
                                 .spawnEgg(0xF0A5A5U, 0xDB635EU)
                                 .loot(&rollPigLoot)
                                 .renderer(kPigRender)
                                 .sounds(kPigSounds)
                                 .vanillaName("pig")
                                 .build("pig");
    // File it in the registry exactly once, passing the static's stable address.
    static const bool registered = [] {
        entityTypeRegistry().add(type);
        return true;
    }();
    static_cast<void>(registered);
    return type;
}

} // namespace mc::gameplay::entities
