#include "gameplay/entities/ZombieEntity.hpp"

#include "gameplay/EntitySystem.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/Random.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "gameplay/entities/MobAi.hpp"
#include "gameplay/entities/MobBrain.hpp"
#include "world/Block.hpp"

#include <cstdint>
#include <memory>

namespace mc::gameplay::entities {
namespace {

// ZombieEntity#initCustomGoals (1.16.1): melee at action priority 2 and living
// non-creative players at target priority 2. MonsterAi contributes the lower
// priority wander/look fallback; every mutable cooldown/path lives in MobBrain.
class ZombieAi final : public MonsterAi {
  public:
    void configureBrain(MobBrain& brain) const override {
        MonsterAi::configureBrain(brain);
        brain.goals().add(2, std::make_unique<MeleeAttackGoal>(1.0F));
        brain.targets().add(2, std::make_unique<ActiveTargetPlayerGoal>());
    }
};

const ZombieAi kZombieAi;

// Zombie.json (26.1): 0-2 rotten flesh, no other pool (no armour/equipment
// table yet — AR-M2). Husk's own manifest-row loot fn in BuiltinSpecies.cpp
// rolls the identical range; the two are kept as separate functions rather
// than a shared header entry point because every other species' loot fn in
// this codebase is likewise local to its own translation unit (Cow/Pig).
EntityDrops rollZombieLoot(std::uint64_t& rng) {
    EntityDrops drops;
    const auto count = static_cast<std::uint8_t>(mc::rng::nextInt(rng, 3U));
    drops.add({world::Block::Air, count, &items::RottenFlesh});
    return drops;
}

// The bedrock model, skin and animation identifiers a renderer would bind for a
// zombie. The assets are not shipped yet, so nothing spawns a zombie in-game;
// the descriptor is data that proves the render interface carries a hostile mob
// just as it does the pig. (Per-species model loading is the renderer's next
// step — see README.)
constexpr EntityRenderDescriptor kZombieRender{
    /*geometryPath=*/"animation/zombie.geo.json",
    /*animationPath=*/"animation/zombie.animation.json",
    /*texturePath=*/"entity/zombie/zombie.png",
    /*geometryId=*/"geometry.zombie",
    /*walkAnimation=*/"animation.zombie.walk",
    /*idleAnimation=*/"animation.zombie.idle",
    /*scale=*/1.0F,
};

// Physical clips and variations are selected by the active packs' sounds.json.
constexpr audio::MobSoundProfile kZombieSounds{
    /*ambientEvent=*/"entity.zombie.ambient",
    /*hurtEvent=*/"entity.zombie.hurt",
    /*deathEvent=*/"entity.zombie.death",
    /*stepEvent=*/"entity.zombie.step",
    /*volume=*/1.0F,
    /*stepVolume=*/0.15F,
};

} // namespace

const EntityType& ZombieEntity::type() {
    // Zombie.createAttributes() (26.1): 20 health, follow range 35,
    // MOVEMENT_SPEED 0.23, attack damage 3. Box 0.6 x 1.95.
    // Spawn-egg tint 0x00AFAF / 0x799C65. AR-M1: loot wired to 0-2 rotten flesh
    // (rollZombieLoot), the item this build previously lacked.
    // xpReward 5 (Mob's DEFAULT_XP_REWARD, which Zombie inherits unchanged).
    // AR-M2: undead() — the family the daylight-ignition rule (EntitySystem::
    // tick) gates on; a zombie carries no SunImmune bit, so it burns.
    static EntityType type = EntityType::Builder::create(MobCategory::Monster, kZombieAi)
                                 .sized(0.6F, 1.95F)
                                 .health(20.0F)
                                 .movementSpeed(0.23F)
                                 .attackDamage(3.0F)
                                 .followRange(35.0F)
                                 .knockbackResistance(0.0F)
                                 .spawnEgg(0x00AFAFU, 0x799C65U)
                                 .xpReward(5)
                                 .renderer(kZombieRender)
                                 .sounds(kZombieSounds)
                                 .vanillaName("zombie")
                                 .loot(&rollZombieLoot)
                                 .undead()
                                 .build("zombie");
    static const bool registered = [] {
        entityTypeRegistry().registerBuiltin(type);
        return true;
    }();
    static_cast<void>(registered);
    return type;
}

} // namespace mc::gameplay::entities
