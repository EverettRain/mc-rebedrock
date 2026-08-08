#include "gameplay/entities/PigEntity.hpp"

#include "gameplay/EntitySystem.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "gameplay/entities/MobAi.hpp"

#include <cstdint>

namespace mc::gameplay::entities {
namespace {

// PigAi inherits the animal grazing cadence from AnimalAi (WanderAroundFarGoal);
// nothing species-specific to add yet — breeding and fleeing land here when they
// ship. The single shared instance carries no state; everything mutable lives on
// the SimpleEntity it steers.
class PigAi final : public AnimalAi {
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
