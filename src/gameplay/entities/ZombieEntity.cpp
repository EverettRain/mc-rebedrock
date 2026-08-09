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

// ZombieEntity's sound hooks (1.16.1): ambient mob/zombie/say1-3, hurt
// mob/zombie/hurt1-2, the single mob/zombie/death.ogg, steps mob/zombie/step1-5.
// Default getSoundVolume 1.0.
constexpr audio::MobSoundProfile kZombieSounds{
    /*root=*/"mob/zombie",
    /*ambientBase=*/"say",   /*ambientVariations=*/3,
    /*hurtBase=*/"hurt",     /*hurtVariations=*/2,
    /*deathBase=*/"death",   /*deathVariations=*/1,
    /*stepBase=*/"step",     /*stepVariations=*/5,
    /*volume=*/1.0F,
    /*stepVolume=*/0.15F,
};

} // namespace

const EntityType& ZombieEntity::type() {
    // ZombieEntity.createZombieAttributes() (1.16.1): 20 health, follow range 35,
    // GENERIC_MOVEMENT_SPEED 0.23 (folded to this engine's blocks-per-tick wander
    // the same way the pig's 0.25 becomes 0.05), attack damage 3. Box 0.6 x 1.95.
    // Spawn-egg tint 0x00AFAF / 0x799C65. No loot roll: its rotten-flesh drop
    // needs an item this build does not have yet, so the table is left empty.
    static EntityType type = EntityType::Builder::create(MobCategory::Monster, kZombieAi)
                                 .sized(0.6F, 1.95F)
                                 .health(20.0F)
                                 .movementSpeed(0.046F)
                                 .attackDamage(3.0F)
                                 .followRange(35.0F)
                                 .knockbackResistance(0.0F)
                                 .spawnEgg(0x00AFAFU, 0x799C65U)
                                 .renderer(kZombieRender)
                                 .sounds(kZombieSounds)
                                 .vanillaName("zombie")
                                 .build("zombie");
    static const bool registered = [] {
        entityTypeRegistry().add(type);
        return true;
    }();
    static_cast<void>(registered);
    return type;
}

} // namespace mc::gameplay::entities
