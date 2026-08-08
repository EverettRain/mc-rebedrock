#include "gameplay/entities/CowEntity.hpp"

#include "gameplay/EntitySystem.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "gameplay/entities/MobAi.hpp"

#include <cstdint>

namespace mc::gameplay::entities {
namespace {

// CowAi inherits the animal grazing cadence from AnimalAi (WanderAroundFarGoal);
// nothing species-specific to add yet — 1.16.1's EscapeDangerGoal (flee on
// damage), AnimalMateGoal and TemptGoal land here when breeding/fleeing ship.
// The single shared instance carries no state; everything mutable lives on the
// SimpleEntity it steers.
class CowAi final : public AnimalAi {
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
// valid for the life of the program. The skin path resolves through the same
// vanilla-jar fallback the pig's does (…/textures/minecraft/entity/cow/cow.png).
constexpr EntityRenderDescriptor kCowRender{
    /*geometryPath=*/"animation/cow.geo.json",
    /*animationPath=*/"animation/cow.animation.json",
    /*texturePath=*/"entity/cow/cow.png",
    /*geometryId=*/"geometry.cow",
    /*walkAnimation=*/"animation.cow.walk",
    /*idleAnimation=*/"animation.cow.idle",
    /*scale=*/1.0F,
};

} // namespace

const EntityType& CowEntity::type() {
    // CowEntity.createCowAttributes() (1.16.1): 10 health, GENERIC_MOVEMENT_SPEED
    // 0.2 (folded to this engine's blocks-per-tick wander the same way the pig's
    // 0.25 becomes 0.05). Box 0.9 x 1.4. Spawn-egg tint 0xF3C9A3 / 0xFFFFFF.
    static EntityType type = EntityType::Builder::create(MobCategory::Creature, kCowAi)
                                 .sized(0.9F, 1.4F)
                                 .health(10.0F)
                                 .movementSpeed(0.04F)
                                 .followRange(16.0F)
                                 .spawnEgg(0xF3C9A3U, 0xFFFFFFU)
                                 .loot(&rollCowLoot)
                                 .renderer(kCowRender)
                                 .vanillaName("cow")
                                 .build("cow");
    // File it in the registry exactly once, passing the static's stable address.
    static const bool registered = [] {
        entityTypeRegistry().add(type);
        return true;
    }();
    static_cast<void>(registered);
    return type;
}

} // namespace mc::gameplay::entities
