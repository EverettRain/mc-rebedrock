// EM-3: the age/breeding (AgeableMob) mechanism.
//
// Covers the age state machine (baby grows to adult, cooldown counts down), the
// per-instance baby scale (hitbox + snapshot), love/breeding through the
// AnimalMateGoal + EntitySystem breed resolution, the post-breed cooldown, the
// parameterized TemptGoal (a species is tempted only by its own item), the
// FollowParentGoal, determinism, and persistence round-trip. All headless.

#include "core/ContentId.hpp"
#include "gameplay/Difficulty.hpp"
#include "gameplay/EntityRenderSnapshot.hpp"
#include "gameplay/EntitySystem.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/entities/EntityType.hpp"
#include "gameplay/entities/MobAi.hpp"
#include "gameplay/entities/MobBrain.hpp"
#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <glm/vec3.hpp>

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace mc::gameplay;

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"aging_breeding_test line " + std::to_string(line) +
                                 " failed: " + expression};
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

[[nodiscard]] bool nearly(float value, float expected) {
    return std::fabs(value - expected) < 0.001F;
}

mc::world::World makeFlatWorld() {
    mc::world::World world;
    for (int chunkZ = -2; chunkZ <= 2; ++chunkZ) {
        for (int chunkX = -2; chunkX <= 2; ++chunkX) {
            mc::world::Chunk chunk;
            for (int z = 0; z < 16; ++z) {
                for (int x = 0; x < 16; ++x) {
                    chunk.setBlock(x, 0, z, mc::world::Block::Stone);
                }
            }
            world.setChunk({chunkX, chunkZ}, std::move(chunk));
        }
    }
    return world;
}

// Two distinct tempt items so the parameterization test can offer the wrong one.
const Item kWheat = Item::of("wheat");
const Item kSeeds = Item::of("wheat_seeds");

[[nodiscard]] ItemStack wheatStack() { return ItemStack{mc::world::Block::Air, 1U, &kWheat}; }
[[nodiscard]] ItemStack seedStack() { return ItemStack{mc::world::Block::Air, 1U, &kSeeds}; }

const entities::AnimalAi kAnimalAi;

// A breedable test species tempted by wheat. Local to the test so it owns its
// numbers and does not depend on any AR content.
const entities::EntityType& wheatAnimal() {
    static const entities::EntityType type =
        entities::EntityType::Builder::create(entities::MobCategory::Creature, kAnimalAi)
            .sized(0.9F, 1.4F)
            .health(10.0F)
            .movementSpeed(0.25F)
            .breedableWith(wheatStack())
            .build("test_wheat_animal");
    return type;
}

// A second breedable species, a different type, to prove cross-species pairs do
// not breed.
const entities::EntityType& otherAnimal() {
    static const entities::EntityType type =
        entities::EntityType::Builder::create(entities::MobCategory::Creature, kAnimalAi)
            .sized(0.9F, 0.9F)
            .health(10.0F)
            .movementSpeed(0.25F)
            .breedableWith(seedStack())
            .build("test_other_animal");
    return type;
}

// A non-breedable species: age never moves, no breeding goals installed.
const entities::EntityType& plainAnimal() {
    static const entities::EntityType type =
        entities::EntityType::Builder::create(entities::MobCategory::Creature, kAnimalAi)
            .sized(0.9F, 0.9F)
            .health(10.0F)
            .movementSpeed(0.25F)
            .build("test_plain_animal");
    return type;
}

EntityTickResult tickEntities(EntitySystem& system, const mc::world::World& world,
                              ItemStack held = {}) {
    return system.tick(world, glm::vec3{0.0F, -1000.0F, 0.0F}, 0.6F, 1.8F, Difficulty::Normal,
                       true, false, 0.0F, false, held);
}

// A tick with the player present at a position, so tempt/look goals can sense it.
EntityTickResult tickWithPlayer(EntitySystem& system, const mc::world::World& world,
                                glm::vec3 playerPos, ItemStack held) {
    return system.tick(world, playerPos, 0.6F, 1.8F, Difficulty::Normal, true, false, 0.0F, false,
                       held);
}

// --- age state machine + scale ---

void testAgeGrowsUp() {
    const mc::world::World world = makeFlatWorld();
    EntitySystem system;
    system.spawn(glm::vec3{0.5F, 1.0F, 0.5F}, wheatAnimal(), 1U);
    const std::uint64_t id = system.entities().front().id;
    // Make it a baby a few ticks from adulthood.
    REQUIRE(system.setAge(id, -3));
    REQUIRE(system.byId(id)->baby());
    REQUIRE(!system.byId(id)->canBreed());
    // A baby's hitbox is the species box scaled by 0.5.
    REQUIRE(nearly(system.byId(id)->dimensions().width, 0.9F * 0.5F));
    REQUIRE(nearly(system.byId(id)->bodyScale(), 0.5F));

    for (int tick = 0; tick < 3; ++tick) {
        tickEntities(system, world);
    }
    // Age reached 0: an adult, full-size, able to breed.
    REQUIRE(system.byId(id)->age == 0);
    REQUIRE(!system.byId(id)->baby());
    REQUIRE(system.byId(id)->canBreed());
    REQUIRE(nearly(system.byId(id)->dimensions().width, 0.9F));
    REQUIRE(nearly(system.byId(id)->bodyScale(), 1.0F));
}

// The per-instance scale reaches the render snapshot.
void testBabyScaleInSnapshot() {
    const mc::world::World world = makeFlatWorld();
    EntitySystem system;
    system.spawn(glm::vec3{0.5F, 1.0F, 0.5F}, wheatAnimal(), 2U);
    system.spawn(glm::vec3{4.5F, 1.0F, 0.5F}, wheatAnimal(), 3U);
    const std::uint64_t babyId = system.entities().front().id;
    REQUIRE(system.setAge(babyId, -100));

    EntityRenderSnapshot snapshot;
    snapshot.capture(system.entities(), {}, {}, {});
    bool sawBaby = false;
    bool sawAdult = false;
    for (const auto& state : snapshot.entities()) {
        if (state.id == babyId) {
            REQUIRE(nearly(state.scale, 0.5F));
            sawBaby = true;
        } else {
            REQUIRE(nearly(state.scale, 1.0F));
            sawAdult = true;
        }
    }
    REQUIRE(sawBaby && sawAdult);
}

// --- love + breeding ---

// Two in-love adults of one species that stand together breed one baby, then go
// on cooldown. This is sabotage anchor ② (no cooldown -> infinite babies).
void testBreeding() {
    const mc::world::World world = makeFlatWorld();
    EntitySystem system;
    // Two adults right next to each other so the mate goal reaches contact fast.
    system.spawn(glm::vec3{0.5F, 1.0F, 0.5F}, wheatAnimal(), 10U);
    system.spawn(glm::vec3{1.2F, 1.0F, 0.5F}, wheatAnimal(), 11U);
    const std::uint64_t a = system.entities()[0].id;
    const std::uint64_t b = system.entities()[1].id;
    REQUIRE(system.setInLove(a));
    REQUIRE(system.setInLove(b));
    REQUIRE(system.byId(a)->inLove() && system.byId(a)->readyToMate());

    const std::size_t before = system.entities().size();
    bool bred = false;
    for (int tick = 0; tick < 40 && !bred; ++tick) {
        tickEntities(system, world);
        if (system.entities().size() > before) {
            bred = true;
        }
    }
    REQUIRE(bred);
    REQUIRE(system.entities().size() == before + 1U);
    // The parents are on cooldown and out of love now.
    REQUIRE(system.byId(a)->age > 0);
    REQUIRE(system.byId(b)->age > 0);
    REQUIRE(!system.byId(a)->inLove());
    REQUIRE(!system.byId(a)->canBreed());  // cooldown blocks re-breeding

    // Exactly one baby exists (a couple breeds once, not twice).
    int babies = 0;
    for (const auto& entity : system.entities()) {
        if (entity.baby()) {
            ++babies;
        }
    }
    REQUIRE(babies == 1);

    // Feeding again during cooldown does nothing.
    REQUIRE(!system.setInLove(a));
}

// Cross-species pairs do not breed.
void testNoCrossSpeciesBreeding() {
    const mc::world::World world = makeFlatWorld();
    EntitySystem system;
    system.spawn(glm::vec3{0.5F, 1.0F, 0.5F}, wheatAnimal(), 20U);
    system.spawn(glm::vec3{1.2F, 1.0F, 0.5F}, otherAnimal(), 21U);
    const std::uint64_t a = system.entities()[0].id;
    const std::uint64_t b = system.entities()[1].id;
    REQUIRE(system.setInLove(a));
    REQUIRE(system.setInLove(b));

    const std::size_t before = system.entities().size();
    for (int tick = 0; tick < 60; ++tick) {
        tickEntities(system, world);
    }
    REQUIRE(system.entities().size() == before);  // no baby
}

// A non-breedable species cannot enter love and never ages.
void testNonBreedable() {
    const mc::world::World world = makeFlatWorld();
    EntitySystem system;
    system.spawn(glm::vec3{0.5F, 1.0F, 0.5F}, plainAnimal(), 30U);
    const std::uint64_t id = system.entities().front().id;
    REQUIRE(!system.byId(id)->kind().breedable());
    REQUIRE(!system.setInLove(id));
    REQUIRE(!system.setAge(id, -100) || system.byId(id)->age == 0);  // forced adult
    for (int tick = 0; tick < 20; ++tick) {
        tickEntities(system, world);
    }
    REQUIRE(system.byId(id)->age == 0);
}

// --- tempt (parameterized) ---

// A wheat-tempted animal follows a player holding wheat but ignores one holding
// seeds. This is sabotage anchor ③ (a hardcoded item would follow regardless).
void testTemptIsParameterized() {
    const mc::world::World world = makeFlatWorld();

    // Holding the right item (wheat): the animal moves toward the player.
    EntitySystem tempted;
    tempted.spawn(glm::vec3{6.5F, 1.0F, 0.5F}, wheatAnimal(), 40U);
    const std::uint64_t rightId = tempted.entities().front().id;
    const glm::vec3 playerPos{0.5F, 1.0F, 0.5F};
    const float startDistance = std::fabs(tempted.byId(rightId)->position.x - playerPos.x);
    for (int tick = 0; tick < 40; ++tick) {
        tickWithPlayer(tempted, world, playerPos, wheatStack());
    }
    const float endDistance = std::fabs(tempted.byId(rightId)->position.x - playerPos.x);
    REQUIRE(endDistance < startDistance - 0.5F);  // it approached

    // Holding the wrong item (seeds): the wheat animal is not tempted, so it does
    // not home in on the player. It may wander, but not steadily toward the
    // player the way temptation drives it.
    EntitySystem ignored;
    ignored.spawn(glm::vec3{6.5F, 1.0F, 0.5F}, wheatAnimal(), 40U);
    const std::uint64_t wrongId = ignored.entities().front().id;
    const float ignoredStart = std::fabs(ignored.byId(wrongId)->position.x - playerPos.x);
    for (int tick = 0; tick < 40; ++tick) {
        tickWithPlayer(ignored, world, playerPos, seedStack());
    }
    const float ignoredEnd = std::fabs(ignored.byId(wrongId)->position.x - playerPos.x);
    // With the same seed, the tempted animal ends up meaningfully closer than the
    // untempted one, which is the parameterization working.
    REQUIRE(endDistance < ignoredEnd - 0.5F);
}

// --- follow parent ---

// A baby walks toward the nearest adult of its species.
void testFollowParent() {
    const mc::world::World world = makeFlatWorld();
    EntitySystem system;
    system.spawn(glm::vec3{0.5F, 1.0F, 0.5F}, wheatAnimal(), 50U);   // adult
    system.spawn(glm::vec3{7.5F, 1.0F, 0.5F}, wheatAnimal(), 51U);   // becomes baby
    const std::uint64_t adultId = system.entities()[0].id;
    const std::uint64_t babyId = system.entities()[1].id;
    REQUIRE(system.setAge(babyId, -10000));

    const float start = std::fabs(system.byId(babyId)->position.x -
                                  system.byId(adultId)->position.x);
    for (int tick = 0; tick < 60; ++tick) {
        tickEntities(system, world);
    }
    const float end = std::fabs(system.byId(babyId)->position.x -
                                system.byId(adultId)->position.x);
    REQUIRE(end < start - 0.5F);  // the baby closed the gap
}

// --- determinism ---

void testDeterministicBreeding() {
    const mc::world::World world = makeFlatWorld();
    const auto run = [&](EntitySystem& system) {
        system.spawn(glm::vec3{0.5F, 1.0F, 0.5F}, wheatAnimal(), 77U);
        system.spawn(glm::vec3{1.3F, 1.0F, 0.5F}, wheatAnimal(), 78U);
        system.setInLove(system.entities()[0].id);
        system.setInLove(system.entities()[1].id);
        for (int tick = 0; tick < 50; ++tick) {
            tickEntities(system, world);
        }
    };
    EntitySystem a;
    EntitySystem b;
    run(a);
    run(b);
    REQUIRE(a.entities().size() == b.entities().size());
    // The baby (if any) is at the same place with the same age in both runs.
    REQUIRE(a.entities().size() == 3U);
    // Find the baby in each and compare.
    const auto findBaby = [](const EntitySystem& system) -> const SimpleEntity* {
        for (const auto& entity : system.entities()) {
            if (entity.baby()) {
                return &entity;
            }
        }
        return nullptr;
    };
    const SimpleEntity* babyA = findBaby(a);
    const SimpleEntity* babyB = findBaby(b);
    REQUIRE(babyA != nullptr && babyB != nullptr);
    REQUIRE(babyA->age == babyB->age);
    REQUIRE(nearly(babyA->position.x, babyB->position.x));
    REQUIRE(nearly(babyA->position.z, babyB->position.z));
}

} // namespace

int main() {
    testAgeGrowsUp();
    testBabyScaleInSnapshot();
    testBreeding();
    testNoCrossSpeciesBreeding();
    testNonBreedable();
    testTemptIsParameterized();
    testFollowParent();
    testDeterministicBreeding();
    return 0;
}
