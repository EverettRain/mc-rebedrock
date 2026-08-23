#include "gameplay/entities/ZombieEntity.hpp"

#include "gameplay/EntitySystem.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "gameplay/entities/MobAi.hpp"
#include "gameplay/entities/MobBrain.hpp"

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
    // Spawn-egg tint 0x00AFAF / 0x799C65. No loot roll: its rotten-flesh drop
    // needs an item this build does not have yet, so the table is left empty.
    // xpReward 5 (Mob's DEFAULT_XP_REWARD, which Zombie inherits unchanged).
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
                                 .build("zombie");
    static const bool registered = [] {
        entityTypeRegistry().registerBuiltin(type);
        return true;
    }();
    static_cast<void>(registered);
    return type;
}

} // namespace mc::gameplay::entities
