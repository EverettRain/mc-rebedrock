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

    // 1.16.1 CowEntity.createCowAttributes(): CREATURE category, 0.9 x 1.4 box,
    // 10 health, GENERIC_MOVEMENT_SPEED 0.2 folded to the engine's 0.04
    // blocks-per-tick wander (the same fold that turns the pig's 0.25 into 0.05).
    assert(cow->category() == mc::gameplay::entities::MobCategory::Creature);
    const auto dimensions = cow->dimensions();
    assert(dimensions.width == 0.9F);
    assert(dimensions.height == 1.4F);
    assert(cow->attributes().maxHealth == 10.0F);
    assert(cow->attributes().movementSpeed == 0.04F);
    const auto egg = cow->spawnEgg();
    assert(egg.primary == 0xF3C9A3U);
    assert(egg.secondary == 0xFFFFFFU);

    // The loot table mirrors vanilla cow.json: 0-2 leather and 1-3 raw beef.
    // Roll enough times that both pools must have come up.
    std::uint32_t rng = 0x1234ABCDU;
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

    // --- Each species registers its own sound set (1.16.1 MobEntity hooks). ---
    // Cow: getSoundVolume 0.4 (CowEntity overrides it), ambient mob/cow/say1-4,
    // and death reuses the same three hurt clips (sounds.json maps
    // entity.cow.death to the hurt clips). Pig has no distinct hurt sound —
    // entity.pig.hurt points back at say1-3 — and a single death.ogg. Zombie
    // hurts are hurt1-2 with five step clips.
    const auto cowSounds = cow->soundProfile();
    assert(cowSounds.root == "mob/cow");
    assert(cowSounds.volume == 0.4F);
    assert(cowSounds.stepVolume == 0.15F);
    assert(cowSounds.ambientVariations == 4);
    assert(cowSounds.hurtVariations == 3 && cowSounds.deathVariations == 3);
    assert(cowSounds.deathBase == cowSounds.hurtBase);
    const auto* pig = registry.byId("minecraft:pig");
    const auto* zombie = registry.byId("minecraft:zombie");
    assert(pig != nullptr && zombie != nullptr);
    assert(pig->soundProfile().hurtBase == "say");
    assert(pig->soundProfile().deathVariations == 1);
    assert(zombie->soundProfile().ambientVariations == 3);
    assert(zombie->soundProfile().hurtVariations == 2);
    assert(zombie->soundProfile().stepVariations == 5);

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

    // --- MobEntity#baseTick's ambient scheduler fires the idle sound roughly
    // every four seconds: the counter climbs one per tick and the roll gate
    // almost surely opens within a couple of hundred ticks. ---
    {
        mc::gameplay::EntitySystem barker;
        barker.spawn({8.0F, 1.0F, 8.0F}, mc::gameplay::entities::PigEntity::type(), 99U);
        bool heardAmbient = false;
        for (int tick = 0; tick < 200 && !heardAmbient; ++tick) {
            static_cast<void>(barker.tick(
                world, glm::vec3{0.0F, -1000.0F, 0.0F}, 0.6F, 1.8F,
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
            static_cast<void>(killers.tick(
                world, glm::vec3{0.0F, -1000.0F, 0.0F}, 0.6F, 1.8F,
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
        simulation.spawn({200.0F, 1.0F, 200.0F}, mc::gameplay::entities::ZombieEntity::type(),
                         53U);
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
