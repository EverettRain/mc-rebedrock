// E3 — the species manifest (batch import) and the creatures added through it.
//
// Proves the cost of a new creature collapsed to a data row: chicken, sheep and
// husk exist with no class of their own, resolve by name, carry the right
// category and attributes, spawn/tick/loot, ride the peaceful-removal rule by
// category, and round-trip through a save by name. Dropping a manifest row, or
// mis-categorising one, is what the sabotage cases break.

#include "gameplay/Difficulty.hpp"
#include "gameplay/EntitySystem.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/entities/BuiltinSpecies.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "persistence/SaveRepository.hpp"
#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <glm/vec3.hpp>

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace {

using mc::gameplay::Difficulty;
using mc::gameplay::EntitySystem;
using mc::gameplay::entities::MobCategory;
using mc::gameplay::entities::entityTypeRegistry;

namespace fs = std::filesystem;

mc::world::World makeFlatWorld() {
    mc::world::Chunk chunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            chunk.setBlock(x, 0, z, mc::world::Block::Stone);
        }
    }
    mc::world::World world;
    world.setChunk({0, 0}, std::move(chunk));
    return world;
}

// Every manifest species resolves by both its rebedrock: id and its minecraft:
// alias, and carries the attributes/category the row states — proving a row is
// a first-class species with no per-species code (sabotage①: a dropped row
// makes byId miss).
void testManifestSpeciesResolve() {
    const auto& registry = entityTypeRegistry();
    for (const auto& def : mc::gameplay::entities::builtinSpeciesManifest()) {
        const auto* type = registry.byId(def.path);
        assert(type != nullptr);
        assert(registry.byId("minecraft:" + std::string{def.vanillaName}) == type);
        assert(type->category() == def.category);
    }

    const auto* chicken = registry.byId("chicken");
    const auto* sheep = registry.byId("sheep");
    const auto* husk = registry.byId("husk");
    assert(chicken != nullptr && sheep != nullptr && husk != nullptr);

    // Attributes came off the manifest row (sabotage③: buildFromDef dropping
    // .attributes() leaves these at the struct defaults, e.g. health 10).
    assert(chicken->attributes().maxHealth() == 4.0F);
    assert(sheep->attributes().maxHealth() == 8.0F);
    assert(husk->attributes().maxHealth() == 20.0F);
    assert(husk->attributes().attackDamage() == 3.0F);
    assert(husk->attributes().followRange() == 35.0F);

    // Passive vs hostile classification.
    assert(chicken->category() == MobCategory::Creature);
    assert(sheep->category() == MobCategory::Creature);
    assert(husk->category() == MobCategory::Monster);
    assert(husk->networkId() != chicken->networkId()); // distinct dense ids
}

// A new species drops loot through the same roll path as the originals.
// AR-A1: chicken drops one raw_chicken every kill plus 0-2 feathers.
void testChickenLoot() {
    const auto* chicken = entityTypeRegistry().byId("chicken");
    assert(chicken != nullptr);
    std::uint32_t rng = 0xC0FFEEU;
    bool sawFeather = false;
    for (int roll = 0; roll < 200; ++roll) {
        bool sawRawChicken = false;
        std::uint8_t rawChickenCount = 0U;
        for (const auto& stack : chicken->rollLoot(rng).view()) {
            sawFeather = sawFeather || stack.item == &mc::gameplay::items::Feather;
            if (stack.item == &mc::gameplay::items::RawChicken) {
                sawRawChicken = true;
                rawChickenCount = stack.count;
            }
        }
        // Sabotage③-adjacent guard: every kill drops raw chicken, never an
        // empty meat pool (the pre-AR-A1 state this loot fn started in).
        assert(sawRawChicken);
        assert(rawChickenCount == 1U);
    }
    assert(sawFeather);
}

// AR-A1 sabotage②: "kill sheep drops wool+mutton" — a sheep must drop both
// 1-2 mutton and exactly one white wool block on every kill, never an empty
// pool (the placeholder rollSheepLoot returned nullptr/no loot fn before this
// task wired it).
void testSheepLoot() {
    const auto* sheep = entityTypeRegistry().byId("sheep");
    assert(sheep != nullptr);
    std::uint32_t rng = 0xFEEDFACEU;
    bool sawMutton1 = false;
    bool sawMutton2 = false;
    for (int roll = 0; roll < 200; ++roll) {
        bool sawMutton = false;
        bool sawWool = false;
        for (const auto& stack : sheep->rollLoot(rng).view()) {
            if (stack.item == &mc::gameplay::items::Mutton) {
                sawMutton = true;
                assert(stack.count == 1U || stack.count == 2U);
                sawMutton1 = sawMutton1 || stack.count == 1U;
                sawMutton2 = sawMutton2 || stack.count == 2U;
            }
            if (stack.block == mc::world::Block::WhiteWool) {
                sawWool = true;
                assert(stack.count == 1U);
            }
        }
        assert(sawMutton);
        assert(sawWool);
    }
    // Both ends of the 1-2 mutton range are reachable over enough rolls.
    assert(sawMutton1 && sawMutton2);
}

// AR-A1 acceptance: "Hit/kill: raycast hits all three; hurt/kill works." The
// raycast/hurt/kill mechanism is entity-generic (EntitySystem dispatches off
// each SimpleEntity's dimensions()/health, never a species switch — see
// entity_registry_test.cpp's pig/cow coverage of the same mechanism), so this
// closes the loop for the two species that gained real loot in this task
// rather than re-deriving the mechanism from scratch.
void testChickenAndSheepHitAndKill() {
    const auto* chickenType = entityTypeRegistry().byId("chicken");
    const auto* sheepType = entityTypeRegistry().byId("sheep");
    assert(chickenType != nullptr && sheepType != nullptr);

    EntitySystem targets;
    // The third argument is the deterministic RNG seed, not the entity id — ids
    // are assigned internally starting at 1 (EntitySystem::nextEntityId_), so
    // read the real ids back rather than assuming they equal the seed.
    targets.spawn({0.0F, 0.0F, 3.0F}, *chickenType, 201U);
    targets.spawn({4.0F, 0.0F, 3.0F}, *sheepType, 202U);
    assert(targets.entities().size() == 2U);
    const std::uint64_t chickenId = targets.entities()[0].id;
    const std::uint64_t sheepId = targets.entities()[1].id;

    // A ray straight at each creature's centre hits it.
    const auto chickenHit = targets.raycast({0.0F, 0.35F, 0.0F}, {0.0F, 0.0F, 1.0F}, 5.0F);
    assert(chickenHit.has_value());
    const auto sheepHit = targets.raycast({4.0F, 0.65F, 0.0F}, {0.0F, 0.0F, 1.0F}, 5.0F);
    assert(sheepHit.has_value());

    // A lethal hurt() rolls loot straight into pendingDrops_ on the same call
    // (LivingEntity#onDeath), no tick() needed — mirrors entity_registry_test's
    // pig coverage of the same mechanism.
    assert(targets.pendingDrops().empty());
    assert(targets.hurt(chickenId, 100.0F, {0.0F, 1.0F, 3.0F})); // lethal
    assert(targets.hurt(sheepId, 100.0F, {4.0F, 1.0F, 3.0F}));   // lethal
    assert(!targets.pendingDrops().empty());
    bool chickenDroppedMeat = false;
    bool sheepDroppedWoolOrMutton = false;
    for (const auto& drop : targets.pendingDrops()) {
        for (const auto& stack : drop.second.view()) {
            if (stack.item == &mc::gameplay::items::RawChicken) chickenDroppedMeat = true;
            if (stack.item == &mc::gameplay::items::Mutton ||
                stack.block == mc::world::Block::WhiteWool) {
                sheepDroppedWoolOrMutton = true;
            }
        }
    }
    assert(chickenDroppedMeat);
    assert(sheepDroppedWoolOrMutton);
}

// A manifest species spawns and ticks through the generic simulation with no
// special case — the whole point of the batch path.
void testSpawnAndTick() {
    auto world = makeFlatWorld();
    EntitySystem entities;
    entities.spawn({8.0F, 1.0F, 8.0F}, *entityTypeRegistry().byId("chicken"), 1U);
    entities.spawn({9.0F, 1.0F, 8.0F}, *entityTypeRegistry().byId("husk"), 2U);
    // Health was seeded from the manifest attributes on spawn.
    assert(entities.byId(1U) != nullptr);
    assert(entities.byId(1U)->damage.maxHealth == 4.0F);
    assert(entities.byId(2U)->damage.maxHealth == 20.0F);
    for (int tick = 0; tick < 20; ++tick) {
        static_cast<void>(entities.tick(world, glm::vec3{0.0F, -1000.0F, 0.0F}, 0.6F, 1.8F,
                                        Difficulty::Normal));
    }
    assert(entities.byId(1U) != nullptr); // survived twenty ticks
}

// Category decides the peaceful pass: the hostile husk is removed the instant
// the world is Peaceful, the passive chicken and sheep persist (sabotage②:
// mis-tagging husk Creature makes it survive Peaceful, which this catches).
void testPeacefulRemovalByCategory() {
    auto world = makeFlatWorld();
    EntitySystem entities;
    entities.spawn({8.0F, 1.0F, 8.0F}, *entityTypeRegistry().byId("chicken"), 1U);
    entities.spawn({9.0F, 1.0F, 8.0F}, *entityTypeRegistry().byId("sheep"), 2U);
    entities.spawn({10.0F, 1.0F, 8.0F}, *entityTypeRegistry().byId("husk"), 3U);
    static_cast<void>(entities.tick(world, glm::vec3{0.0F, -1000.0F, 0.0F}, 0.6F, 1.8F,
                                    Difficulty::Peaceful));
    assert(entities.byId(1U) != nullptr); // chicken persists
    assert(entities.byId(2U) != nullptr); // sheep persists
    assert(entities.byId(3U) == nullptr); // husk (MONSTER) removed
}

// A manifest species persists and restores by name like any other, carrying its
// per-instance fields (sabotage③ in the task's sense: a dropped field on the
// round-trip).
void testSaveRoundTrip(const fs::path& scratch) {
    const auto root = scratch / "species_save";
    fs::remove_all(root);
    mc::persistence::SaveRepository repository{root};
    auto game = repository.create("SpeciesHerd", 5ULL);
    repository.save(game);

    std::vector<mc::persistence::PersistentEntity> stored;
    stored.push_back({"chicken", 1.5F, 64.0F, 1.5F, 0.0F, 0, 0, 0, 4.0F, 0, 7U, 0U});
    stored.push_back({"husk", 2.5F, 64.0F, 2.5F, 0.0F, 0, 0, 0, 17.0F, 33, 11U, 0U});
    repository.saveChunk(game.summary.identifier, 0, 0, {}, stored);

    const auto loaded = repository.loadChunkEntities(game.summary.identifier, 0, 0);
    assert(loaded.size() == 2U);
    for (const auto& record : loaded) {
        const auto& type = mc::gameplay::entities::resolveEntityTypeForRestore(record.species);
        // A known manifest species resolves to the real type, not a placeholder.
        assert(type.id().path == record.species);
        if (record.species == "husk") {
            assert(record.health == 17.0F);
            assert(record.angerTicks == 33);
            assert(record.ageTicks == 11U);
        }
    }
    fs::remove_all(root);
}

} // namespace

int main() {
    mc::gameplay::entities::registerBuiltinEntities();
    const auto scratch = fs::temp_directory_path() / "mc_rebedrock_e3_species_test";
    fs::remove_all(scratch);
    fs::create_directories(scratch);

    testManifestSpeciesResolve();
    testChickenLoot();
    testSheepLoot();
    testChickenAndSheepHitAndKill();
    testSpawnAndTick();
    testPeacefulRemovalByCategory();
    testSaveRoundTrip(scratch);

    fs::remove_all(scratch);
    return 0;
}
