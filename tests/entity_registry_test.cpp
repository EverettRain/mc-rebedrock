#include "gameplay/Difficulty.hpp"
#include "gameplay/EntitySystem.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/entities/CowEntity.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "gameplay/entities/PigEntity.hpp"
#include "gameplay/entities/ZombieEntity.hpp"
#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <glm/vec3.hpp>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <utility>

int main() {
    mc::gameplay::entities::registerBuiltinEntities();
    const auto& registry = mc::gameplay::entities::entityTypeRegistry();

    // Registration + name resolution through both the rebedrock: id and the
    // minecraft: alias, exactly like a Registry#get("minecraft:cow") lookup.
    const auto* cow = registry.byId("cow");
    assert(cow != nullptr);
    assert(registry.byId("minecraft:cow") == cow);
    assert(registry.byId("rebedrock:cow") == cow);
    assert(cow->vanillaId().path == "cow");

    // 26.1 AbstractCow.createAttributes(): CREATURE category, 0.9 x 1.4 box,
    // 10 health and the unscaled MOVEMENT_SPEED attribute 0.2.
    assert(cow->category() == mc::gameplay::entities::MobCategory::Creature);
    const auto dimensions = cow->dimensions();
    assert(dimensions.width == 0.9F);
    assert(dimensions.height == 1.4F);
    assert(cow->attributes().maxHealth() == 10.0F);
    assert(cow->attributes().movementSpeed() == 0.20F);
    const auto egg = cow->spawnEgg();
    assert(egg.primary == 0xF3C9A3U);
    assert(egg.secondary == 0xFFFFFFU);

    // The loot table mirrors vanilla cow.json: 0-2 leather and 1-3 raw beef.
    // Roll enough times that both pools must have come up.
    std::uint64_t rng = 0x1234ABCDULL;
    bool sawBeef = false;
    bool sawLeather = false;
    for (int roll = 0; roll < 200; ++roll) {
        for (const auto& stack : cow->rollLoot(rng).view()) {
            sawBeef = sawBeef || stack.item == &mc::gameplay::items::Beef;
            sawLeather = sawLeather || stack.item == &mc::gameplay::items::Leather;
        }
    }
    assert(sawBeef);
    assert(sawLeather);

    // --- Each species registers 26.1 event IDs, never physical clip paths. ---
    // Candidate files and variation counts belong exclusively to sounds.json.
    const auto cowSounds = cow->soundProfile();
    assert(cowSounds.volume == 0.4F);
    assert(cowSounds.stepVolume == 0.15F);
    assert(cowSounds.ambientEvent == "entity.cow.ambient");
    assert(cowSounds.hurtEvent == "entity.cow.hurt");
    assert(cowSounds.deathEvent == "entity.cow.death");
    assert(cowSounds.stepEvent == "entity.cow.step");
    const auto* pig = registry.byId("minecraft:pig");
    const auto* zombie = registry.byId("minecraft:zombie");
    assert(pig != nullptr && zombie != nullptr);
    assert(cow->render().texturePath == "entity/cow/cow_temperate.png");
    assert(pig->render().texturePath == "entity/pig/pig_temperate.png");
    assert(zombie->render().texturePath == "entity/zombie/zombie.png");
    assert(pig->soundProfile().hurtEvent == "entity.pig.hurt");
    assert(pig->soundProfile().deathEvent == "entity.pig.death");
    assert(zombie->soundProfile().ambientEvent == "entity.zombie.ambient");
    assert(zombie->soundProfile().hurtEvent == "entity.zombie.hurt");
    assert(zombie->soundProfile().stepEvent == "entity.zombie.step");

    // GameRenderer's crosshair pick uses the exact living-entity box in 1.16.1.
    // The projectile-only 0.3 expansion must not make a ray 0.25 blocks outside
    // a pig's side select it and hide a block behind it.
    {
        mc::gameplay::EntitySystem targets;
        targets.spawn({0.0F, 0.0F, 3.0F}, mc::gameplay::entities::PigEntity::type(), 91U);
        const auto direct = targets.raycast({0.0F, 0.45F, 0.0F}, {0.0F, 0.0F, 1.0F}, 5.0F);
        assert(direct.has_value());
        assert(std::abs(direct->distance - 2.55F) < 0.0001F);
        const auto outside = targets.raycast({0.70F, 0.45F, 0.0F}, {0.0F, 0.0F, 1.0F}, 5.0F);
        assert(!outside.has_value());

        // Spatial buckets use the feet position. A cow standing just below a
        // 16-block section boundary extends its upper body into the section
        // above; aiming there must still find the entity bucketed below.
        targets.clear();
        targets.spawn({0.0F, 15.0F, 3.0F}, mc::gameplay::entities::CowEntity::type(), 92U);
        const auto upperBody = targets.raycast({0.0F, 16.20F, 0.0F}, {0.0F, 0.0F, 1.0F}, 5.0F);
        assert(upperBody.has_value());
        assert(std::abs(upperBody->distance - 2.55F) < 0.0001F);

        // The same rule applies at horizontal section faces: the ray may enter
        // the portion of the AABB in X+1 while the entity centre remains in X.
        targets.clear();
        targets.spawn({15.8F, 0.0F, 3.0F}, mc::gameplay::entities::PigEntity::type(), 93U);
        const auto acrossHorizontalBoundary =
            targets.raycast({16.10F, 0.45F, 0.0F}, {0.0F, 0.0F, 1.0F}, 5.0F);
        assert(acrossHorizontalBoundary.has_value());
    }

    // Peaceful pass (MobEntity#checkDespawn): a flat stone floor, one pig, one
    // cow and one zombie. Ticking once on Peaceful keeps the two CREATURE
    // species and silently removes the MONSTER — the category decides, so the
    // second passive species rides the same rule the pig does.
    mc::world::Chunk chunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            chunk.setBlock(x, 0, z, mc::world::Block::Stone);
        }
    }
    mc::world::World world;
    world.setChunk({0, 0}, std::move(chunk));

    // Full-AABB occupancy and block-placement overlap checks reject a wall cell,
    // and a legacy/restored creature already inside one is moved to the nearest
    // free face on its next collision tick instead of remaining trapped forever.
    {
        mc::world::Chunk collisionChunk;
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                collisionChunk.setBlock(x, 0, z, mc::world::Block::Stone);
            }
        }
        collisionChunk.setBlock(8, 1, 8, mc::world::Block::Stone);
        collisionChunk.setBlock(4, 2, 4, mc::world::Block::Stone);
        mc::world::World collisionWorld;
        collisionWorld.setChunk({0, 0}, std::move(collisionChunk));

        const auto& pigType = mc::gameplay::entities::PigEntity::type();
        const auto& cowType = mc::gameplay::entities::CowEntity::type();
        const glm::vec3 lowCeilingPosition{4.5F, 1.001F, 4.5F};
        assert(mc::gameplay::EntitySystem::canOccupy(collisionWorld, lowCeilingPosition,
                                                     pigType.dimensions()));
        assert(!mc::gameplay::EntitySystem::canOccupy(collisionWorld, lowCeilingPosition,
                                                      cowType.dimensions()));
        const glm::vec3 trappedPosition{8.5F, 1.001F, 8.5F};
        assert(!mc::gameplay::EntitySystem::canOccupy(collisionWorld, trappedPosition,
                                                      pigType.dimensions()));

        mc::gameplay::EntitySystem trapped;
        trapped.spawn(trappedPosition, pigType, 92U);
        assert(trapped.intersectsBlock(8, 1, 8));
        assert(!trapped.intersectsBlock(10, 1, 8));
        static_cast<void>(trapped.tick(collisionWorld, glm::vec3{0.0F, -1000.0F, 0.0F}, 0.6F, 1.8F,
                                       mc::gameplay::Difficulty::Normal));
        const auto& recovered = trapped.entities().front();
        assert(mc::gameplay::EntitySystem::canOccupy(collisionWorld, recovered.position,
                                                     recovered.dimensions()));
        assert(!trapped.intersectsBlock(8, 1, 8));
    }

    // --- MobEntity#baseTick's ambient scheduler fires the idle sound roughly
    // every four seconds: the counter climbs one per tick and the roll gate
    // almost surely opens within a couple of hundred ticks. ---
    {
        mc::gameplay::EntitySystem barker;
        barker.spawn({8.0F, 1.0F, 8.0F}, mc::gameplay::entities::PigEntity::type(), 99U);
        bool heardAmbient = false;
        for (int tick = 0; tick < 200 && !heardAmbient; ++tick) {
            static_cast<void>(barker.tick(world, glm::vec3{0.0F, -1000.0F, 0.0F}, 0.6F, 1.8F,
                                          mc::gameplay::Difficulty::Normal));
            for (const auto& sound : barker.pendingSounds()) {
                heardAmbient = heardAmbient || sound.event == mc::gameplay::MobSoundEvent::Ambient;
            }
            barker.clearPendingSounds();
        }
        assert(heardAmbient);
    }

    mc::gameplay::EntitySystem entities;
    entities.spawn({8.0F, 1.0F, 8.0F}, mc::gameplay::entities::PigEntity::type(), 1U);
    entities.spawn({9.0F, 1.0F, 8.0F}, mc::gameplay::entities::CowEntity::type(), 2U);
    entities.spawn({10.0F, 1.0F, 8.0F}, mc::gameplay::entities::ZombieEntity::type(), 3U);
    static_cast<void>(entities.tick(world, glm::vec3{0.0F, -1000.0F, 0.0F}, 0.6F, 1.8F,
                                    mc::gameplay::Difficulty::Peaceful));

    std::size_t cowCount = 0U;
    std::size_t pigCount = 0U;
    std::size_t zombieCount = 0U;
    for (const auto& entity : entities.entities()) {
        if (entity.type == &mc::gameplay::entities::CowEntity::type()) {
            ++cowCount;
        } else if (entity.type == &mc::gameplay::entities::PigEntity::type()) {
            ++pigCount;
        } else if (entity.type == &mc::gameplay::entities::ZombieEntity::type()) {
            ++zombieCount;
        }
    }
    assert(cowCount == 1U);
    assert(pigCount == 1U);
    assert(zombieCount == 0U);

    // --- LivingEntity knockback arc: a grounded hit gets the vanilla 0.4
    // lift, 0.08 gravity and 0.98 vertical drag. It peaks quickly and lands in
    // roughly half a second; a follow-up hit while airborne must not restart
    // the vertical arc. ---
    {
        mc::gameplay::EntitySystem knockback;
        knockback.spawn({8.0F, 1.001F, 8.0F}, mc::gameplay::entities::PigEntity::type(), 31U);
        const std::uint64_t pigId = knockback.entities().front().id;
        // Settle once so takeKnockback sees the same grounded state as a mob
        // standing in a running world.
        for (int tick = 0; tick < 2; ++tick) {
            static_cast<void>(knockback.tick(world, glm::vec3{0.0F, -1000.0F, 0.0F}, 0.6F, 1.8F,
                                             mc::gameplay::Difficulty::Normal));
        }
        assert(knockback.byId(pigId)->onGround);
        assert(knockback.hurt(pigId, 1.0F, {7.0F, 1.001F, 8.0F}));

        static_cast<void>(knockback.tick(world, glm::vec3{0.0F, -1000.0F, 0.0F}, 0.6F, 1.8F,
                                         mc::gameplay::Difficulty::Normal));
        const float airborneVelocity = knockback.byId(pigId)->velocity.y;
        assert(!knockback.byId(pigId)->onGround);
        // Stronger damage passes the invulnerability-window guard, but because
        // the target is airborne it must not receive another +0.4 lift.
        assert(knockback.hurt(pigId, 2.0F, {7.0F, 1.001F, 8.0F}));
        assert(std::abs(knockback.byId(pigId)->velocity.y - airborneVelocity) < 0.0001F);

        float peakY = knockback.byId(pigId)->position.y;
        int landingTick = 0;
        for (int tick = 2; tick <= 20; ++tick) {
            static_cast<void>(knockback.tick(world, glm::vec3{0.0F, -1000.0F, 0.0F}, 0.6F, 1.8F,
                                             mc::gameplay::Difficulty::Normal));
            const auto* pig = knockback.byId(pigId);
            peakY = std::max(peakY, pig->position.y);
            if (pig->onGround) {
                landingTick = tick;
                break;
            }
        }
        assert(peakY < 2.30F); // under 1.3 blocks above its feet
        assert(landingTick > 0);
        assert(landingTick <= 12); // at most 0.6 seconds at 20 TPS
    }

    // --- A killed creature drops its loot on the death tick, not after the
    // twenty-tick corpse animation (LivingEntity#onDeath drops immediately). ---
    {
        mc::gameplay::EntitySystem killers;
        killers.spawn({7.0F, 1.0F, 7.0F}, mc::gameplay::entities::PigEntity::type(), 4U);
        assert(killers.pendingDrops().empty());
        // A fatal hit rolls the pig's porkchops straight into pendingDrops_
        // inside hurt(), with no tick() advancing the animation. hurt() takes
        // the stable id, not the vector index.
        const std::uint64_t pigId = killers.entities().front().id;
        assert(killers.hurt(pigId, 100.0F, {7.0F, 1.0F, 7.0F}));
        assert(!killers.pendingDrops().empty());
        killers.clearPendingDrops();
        // Running the corpse animation to completion removes the body without
        // rolling the loot a second time.
        for (int tick = 0; tick < 25; ++tick) {
            static_cast<void>(killers.tick(world, glm::vec3{0.0F, -1000.0F, 0.0F}, 0.6F, 1.8F,
                                           mc::gameplay::Difficulty::Normal));
        }
        assert(killers.entities().empty());
        assert(killers.pendingDrops().empty());
    }

    // --- Simulation distance: a creature past the radius is frozen but a
    // within-radius one keeps simulating, and a far MONSTER fades via
    // MobEntity#checkDespawn's despawn-range pass. ---
    {
        mc::gameplay::EntitySystem simulation;
        // Within the 16-block radius: gravity pulls it from y=10 to the floor.
        simulation.spawn({8.0F, 10.0F, 8.0F}, mc::gameplay::entities::PigEntity::type(), 51U);
        // 32 blocks out (dx = 32 > 16): frozen — stays exactly where it spawned.
        simulation.spawn({40.0F, 10.0F, 8.0F}, mc::gameplay::entities::PigEntity::type(), 52U);
        // 192 blocks out and a MONSTER: despawns when distant, independently of
        // the simulation radius.
        simulation.spawn({200.0F, 1.0F, 200.0F}, mc::gameplay::entities::ZombieEntity::type(), 53U);
        const std::uint64_t nearId = simulation.entities()[0].id;
        const std::uint64_t farId = simulation.entities()[1].id;
        const std::uint64_t monsterId = simulation.entities()[2].id;
        const glm::vec3 player{8.0F, 1.0F, 8.0F};
        for (int tick = 0; tick < 200; ++tick) {
            static_cast<void>(simulation.tick(
                world, player, 0.6F, 1.8F, mc::gameplay::Difficulty::Normal, true, false, 16.0F));
        }
        // The near pig fell and landed; the far pig never moved a pixel.
        assert(simulation.byId(nearId) != nullptr);
        assert(simulation.byId(nearId)->position.y < 5.0F);
        assert(simulation.byId(farId) != nullptr);
        assert(std::abs(simulation.byId(farId)->position.y - 10.0F) < 0.01F);
        // The far zombie was removed by the despawn-range pass.
        assert(simulation.byId(monsterId) == nullptr);
    }

    return 0;
}
