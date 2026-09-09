// AR-M4: the composter — the farmer villager's job site, and the first of the
// thirteen workstation blocks this roster was missing.
//
// The three things worth pinning, because each is a place a plausible-looking
// implementation goes wrong:
//
//   1. the bowl's CAVITY FLOOR rises with the fill (`1 + level * 2`), so a
//      composter is not one shape with a decoration on top — standing in a full
//      one really is standing higher;
//   2. an EMPTY composter accepts its first compostable unconditionally, and
//      every later one rolls. Vanilla writes that as a double negative
//      (`(level != 0 || !(chance > 0)) && !(random < chance)`) and it is the
//      single easiest line in ComposterBlock to transcribe backwards;
//   3. level 8 (READY) is reached only by the twenty-tick scheduled tick, never
//      by an item going in, and it reuses level 7's geometry with a DIFFERENT
//      sprite.

#include "gameplay/Composter.hpp"
#include "gameplay/GameSession.hpp"
#include "gameplay/GameCommand.hpp"
#include "gameplay/WorldSimulation.hpp"
#include "world/Block.hpp"
#include "world/BlockShape.hpp"
#include "world/BlockState.hpp"
#include "world/Chunk.hpp"
#include "world/ElementModelBaker.hpp"
#include "world/World.hpp"

#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

using namespace mc;

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"composter_test line " + std::to_string(line) +
                                 " failed: " + expression};
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

[[nodiscard]] bool near(float a, float b) { return std::fabs(a - b) < 1.0e-5F; }

struct TestHost final : gameplay::SimulationHost {
    void submitWorldEdit(int, int, int, world::Block, std::uint8_t,
                         std::optional<world::BlockOrientation>) override {}
    void submitWorldStateEdit(int, int, int, world::BlockState) override {}
    void previewBlockEdit(int, int, int) override {}
    void playBlockBreak(world::Block, glm::vec3) override {}
    void playBlockHit(world::Block, glm::vec3) override {}
    void playBlockPlace(world::Block, glm::vec3) override {}
    void playItemBreak(glm::vec3) override {}
    void playItemPickup(glm::vec3) override {}
    void playEat(glm::vec3) override {}
    void playPlayerHurt(glm::vec3) override {}
    void playPlayerFall(glm::vec3, bool) override {}
    void playBurp(glm::vec3) override {}
    void playExplode(glm::vec3) override {}
    void playCreatureHurt(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureDeath(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureAmbient(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureStep(const gameplay::entities::EntityType&, glm::vec3) override {}
    void playFootstep(world::Block, glm::vec3, float) override {}
    void playSplash(glm::vec3, float) override {}
    void spawnBlockBreakParticles(glm::ivec3, world::Block) override {}
    void spawnWaterSplash(glm::vec3) override {}
    void onPlayerDied() override {}
    void onFurnaceStateChanged() override {}
    void onEatingStarted() override {}
    void onEatingCancelled() override {}
};

[[nodiscard]] world::World makeFlatWorld() {
    world::World world;
    world::Chunk chunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            chunk.setBlock(x, 0, z, world::Block::Stone);
        }
    }
    world.setChunk({0, 0}, std::move(chunk));
    return world;
}

// --- 1) the bowl -----------------------------------------------------------

// The cavity floor is `clamp(1 + level * 2, 2, 16) / 16`, and the five boxes are
// a floor plus four walls. Exact values, per level: a "the shape grows with the
// level" comparison would pass a floor that rose at the wrong rate, which is the
// difference between standing on the rim and standing inside.
void testBowlFloorRisesWithTheFill() {
    for (int level = 0; level <= world::kComposterReadyLevel; ++level) {
        const auto state =
            world::BlockState{world::Block::Composter}.withComposterLevel(level);
        const auto shape = world::blockShape(state);
        REQUIRE(shape.kind == world::ShapeKind::Boxes);
        REQUIRE(shape.boxes.size() == 5U);
        // Vanilla's `shapes[8] = shapes[7]`: READY does not raise the floor
        // past level 7's.
        const int effective = level >= world::kComposterReadyLevel ? 7 : level;
        const int raw = 1 + effective * 2;
        const float expected = static_cast<float>(raw < 2 ? 2 : raw) / 16.0F;
        REQUIRE(near(shape.boxes[0].maxY, expected));
        // The floor spans the whole footprint; the four walls are 2/16 thick and
        // full height, which is what leaves a 12x12 hole down the middle.
        REQUIRE(near(shape.boxes[0].minX, 0.0F) && near(shape.boxes[0].maxX, 1.0F));
        for (std::size_t wall = 1; wall < 5U; ++wall) {
            REQUIRE(near(shape.boxes[wall].minY, 0.0F));
            REQUIRE(near(shape.boxes[wall].maxY, 1.0F));
        }
    }
    // The two ends of the range, spelled out rather than derived, so a change to
    // the formula above cannot also "fix" the expectation.
    REQUIRE(near(world::blockShape(world::BlockState{world::Block::Composter}).boxes[0].maxY,
                 2.0F / 16.0F));
    REQUIRE(near(world::blockShape(world::BlockState{world::Block::Composter}
                                       .withComposterLevel(7))
                     .boxes[0]
                     .maxY,
                 15.0F / 16.0F));
}

// --- 2) the model ----------------------------------------------------------

// An empty composter is five boxes; a filled one is six, and the sixth is the
// compost surface at `1 + level * 2`. READY reuses level 7's height and swaps
// the sprite slot — asserted as two separate facts, so a change that made READY
// look like level 7 in every way (the easy mistake) fails.
void testCompostSurfaceHeightAndSprite() {
    using world::bake::elementsFor;
    const auto elementsAt = [](int level) {
        return elementsFor(world::Block::Composter,
                           world::BlockState{world::Block::Composter}.withComposterLevel(level));
    };
    REQUIRE(elementsAt(0).size() == 5U);
    for (int level = 1; level <= world::kComposterReadyLevel; ++level) {
        const auto elements = elementsAt(level);
        REQUIRE(elements.size() == 6U);
        const auto& compost = elements.back();
        const int effective = level >= world::kComposterReadyLevel ? 7 : level;
        REQUIRE(near(compost.to16.y, static_cast<float>(1 + effective * 2)));
        REQUIRE(near(compost.from16.x, 2.0F) && near(compost.to16.x, 14.0F));
        // The compost is a SURFACE: only its up face is present, so the sides of
        // the pile are never drawn (they would poke through the walls).
        for (std::uint8_t f = 0; f < world::bake::kFacingCount; ++f) {
            const bool isUp = static_cast<world::bake::Facing>(f) == world::bake::Facing::Up;
            REQUIRE(compost.faces[f].present == isUp);
        }
        // Slot 3 is composter_compost, slot 4 composter_ready.
        const std::uint8_t expectedSlot = level >= world::kComposterReadyLevel ? 4U : 3U;
        REQUIRE(compost.faces[static_cast<std::size_t>(world::bake::Facing::Up)].slot ==
                expectedSlot);
    }
    // The same height, a different sprite — the pair of facts READY exists for.
    const auto seven = elementsAt(7).back();
    const auto ready = elementsAt(world::kComposterReadyLevel).back();
    REQUIRE(near(seven.to16.y, ready.to16.y));
    REQUIRE(seven.faces[static_cast<std::size_t>(world::bake::Facing::Up)].slot !=
            ready.faces[static_cast<std::size_t>(world::bake::Facing::Up)].slot);
}

// --- 3) the fill rule ------------------------------------------------------

// ComposterBlock#addItem, both branches of it.
void testFirstItemIsFreeAndLaterOnesRoll() {
    // An EMPTY composter accepts even on a roll that would certainly fail
    // (0.99 against a 0.3 chance). This is the double negative in vanilla's
    // condition, and a "roll every time" transcription fails right here.
    REQUIRE(gameplay::composterAddItem(0, 0.3F, 0.99F) == 1);
    // A non-empty one rolls: below the chance accepts, at or above it does not.
    REQUIRE(gameplay::composterAddItem(1, 0.3F, 0.29F) == 2);
    REQUIRE(gameplay::composterAddItem(1, 0.3F, 0.30F) == 1);
    REQUIRE(gameplay::composterAddItem(1, 0.3F, 0.99F) == 1);
    // A non-compostable never fills, not even an empty composter.
    REQUIRE(gameplay::composterAddItem(0, 0.0F, 0.0F) == 0);
    // MAX_LEVEL is 7: an item can never produce READY, only the scheduled tick
    // can.
    REQUIRE(gameplay::composterAddItem(gameplay::kComposterMaxFillLevel, 1.0F, 0.0F) ==
            gameplay::kComposterMaxFillLevel);
    REQUIRE(gameplay::composterAddItem(6, 1.0F, 0.0F) == gameplay::kComposterMaxFillLevel);
}

// The chance table, at each of the tiers this roster actually has, plus a
// non-compostable. Asserted as exact tiers because "> 0" would pass a table that
// gave bread a sapling's odds.
void testCompostChances() {
    REQUIRE(near(gameplay::compostChance(
                     gameplay::ItemStack{world::Block::Air, 1U, &gameplay::items::WheatSeeds}),
                 0.3F));
    REQUIRE(near(gameplay::compostChance(
                     gameplay::ItemStack{world::Block::Air, 1U, &gameplay::items::Wheat}),
                 0.65F));
    REQUIRE(near(gameplay::compostChance(
                     gameplay::ItemStack{world::Block::Air, 1U, &gameplay::items::Bread}),
                 0.85F));
    // A block wielded as its BlockItem: leaves 0.3, hay 0.85.
    REQUIRE(near(gameplay::compostChance(gameplay::ItemStack{world::Block::OakLeaves, 1U}), 0.3F));
    REQUIRE(near(gameplay::compostChance(gameplay::ItemStack{world::Block::HayBlock, 1U}), 0.85F));
    // Not compostable.
    REQUIRE(near(gameplay::compostChance(gameplay::ItemStack{world::Block::Stone, 1U}), 0.0F));
    REQUIRE(!gameplay::isCompostable(gameplay::ItemStack{world::Block::Stone, 1U}));
    REQUIRE(gameplay::isCompostable(gameplay::ItemStack{world::Block::HayBlock, 1U}));
}

// --- 4) the whole cycle through a session ----------------------------------

// One right-click on `cell`, driven the way the renderer drives it: enqueue the
// command and let the session's own tick resolve it. Deliberately end to end —
// calling the composter handler directly would still pass if it had never been
// wired into PlayerInteraction's use ladder, which is precisely the kind of
// "written but not reachable" gap this project keeps finding.
//
// The shape matters. A right-click is a PRESS that stays held until its release,
// and PlayerInteraction repeats a held use every four ticks; a naive
// "enqueue and tick once" therefore leaves the button DOWN, and the composter
// keeps being clicked forever — which, once it ripens, empties it again on the
// very next repeat. So: release, spend the four-tick cooldown, press, tick
// exactly once (that tick is where the use lands), and queue the release. The
// caller is then free to tick as many times as it likes with nothing else
// happening.
void clickBlock(gameplay::GameSession& session, world::World& world,
                gameplay::SimulationHost& host, glm::ivec3 cell) {
    session.enqueueCommand(gameplay::UseItemStop{});
    for (int cooldown = 0; cooldown < 4; ++cooldown) {
        session.tick(world, host);
    }
    gameplay::UseItemOn use;
    use.block = cell;
    use.adjacent = {cell.x, cell.y + 1, cell.z};
    use.face = world::BlockOrientation::Up;
    use.hitPosition = glm::vec3{cell} + glm::vec3{0.5F, 1.0F, 0.5F};
    use.lookDirection = {0.0F, 0.0F, -1.0F};
    session.enqueueCommand(use);
    session.tick(world, host);
    session.enqueueCommand(gameplay::UseItemStop{});
}

// Fill to 7 with a guaranteed-accept item, wait the twenty ticks, then take the
// bone meal out. Asserted end to end because the three halves live in three
// different layers (interaction, tick scheduler, item spawn) and each has been a
// place a mechanic silently stopped short before.
void testFillRipenAndExtract() {
    TestHost host;
    world::World world = makeFlatWorld();
    gameplay::GameSession session;
    session.setGameMode(gameplay::GameMode::Survival);
    session.player().setPosition({8.5F, 1.0F, 10.5F});
    const glm::ivec3 cell{8, 1, 8};
    world.setBlock(cell.x, cell.y, cell.z, world::Block::Composter);

    // Hay blocks compost at 0.85; the first is free and the rest are driven by
    // the session's own stream, so the loop keeps going until it reaches 7
    // rather than assuming a fixed number of clicks.
    int clicks = 0;
    while (world.state(cell.x, cell.y, cell.z).composterLevel() <
               gameplay::kComposterMaxFillLevel &&
           clicks < 400) {
        session.inventory().replaceSelected(gameplay::ItemStack{world::Block::HayBlock, 64U});
        clickBlock(session, world, host, cell);
        ++clicks;
    }
    REQUIRE(world.state(cell.x, cell.y, cell.z).composterLevel() ==
            gameplay::kComposterMaxFillLevel);
    // The first click into an EMPTY composter is free, so filling can never take
    // fewer than seven clicks — and with a 0.85 item it should not take many
    // more.
    REQUIRE(clicks >= gameplay::kComposterMaxFillLevel);

    // Reaching 7 armed the twenty-tick ripen, and nothing else.
    REQUIRE(session.worldSimulation().pendingComposterReadyCount() == 1U);

    // Counted rather than assumed: tick until it turns READY and assert the
    // count is EXACTLY vanilla's twenty. A "tick a while, then check" shape
    // would pass a one-tick ripen and a fifty-tick one alike, and the delay is
    // the whole difference between compost that finishes and compost that
    // finishes *later*.
    int waited = 0;
    while (world.state(cell.x, cell.y, cell.z).composterLevel() !=
               world::kComposterReadyLevel &&
           waited < 200) {
        session.tick(world, host);
        ++waited;
    }
    // The literal 20, NOT kComposterReadyDelayTicks: an expectation taken from
    // the constant under test proves only that the constant equals itself —
    // a sabotage that set the delay to one tick passed this line unchanged
    // until it was written out. 20 is ComposterBlock#addItem's own
    // `level.scheduleTick(pos, block, 20)`.
    REQUIRE(waited == 20);
    REQUIRE(gameplay::kComposterReadyDelayTicks == 20);
    REQUIRE(session.worldSimulation().pendingComposterReadyCount() == 0U);

    // A click on a READY composter empties it and drops exactly one bone meal,
    // whatever is in hand.
    const std::size_t itemsBefore = session.itemEntities().entities().size();
    session.inventory().replaceSelected(gameplay::ItemStack{});
    clickBlock(session, world, host, cell);
    REQUIRE(world.state(cell.x, cell.y, cell.z).composterLevel() == 0);
    REQUIRE(session.itemEntities().entities().size() == itemsBefore + 1U);
    const auto& dropped = session.itemEntities().entities().back();
    REQUIRE(dropped.stack.item == &gameplay::items::BoneMeal);
    REQUIRE(dropped.stack.count == 1U);
}

// A full-but-not-ready composter (level 7) consumes the click and keeps the
// item: vanilla returns SUCCESS from useItemOn without calling addItem. Its own
// test because "does nothing" is exactly what an accidental fall-through to
// block placement also looks like from the outside.
void testFullComposterKeepsTheItem() {
    TestHost host;
    world::World world = makeFlatWorld();
    gameplay::GameSession session;
    session.setGameMode(gameplay::GameMode::Survival);
    session.player().setPosition({8.5F, 1.0F, 10.5F});
    const glm::ivec3 cell{8, 1, 8};
    world.setState(cell.x, cell.y, cell.z,
                   world::BlockState{world::Block::Composter}.withComposterLevel(
                       gameplay::kComposterMaxFillLevel));
    session.inventory().replaceSelected(gameplay::ItemStack{world::Block::HayBlock, 3U});
    clickBlock(session, world, host, cell);
    REQUIRE(world.state(cell.x, cell.y, cell.z).composterLevel() ==
            gameplay::kComposterMaxFillLevel);
    REQUIRE(session.inventory().selectedStack().count == 3U);
}

} // namespace

int main() {
    testBowlFloorRisesWithTheFill();
    testCompostSurfaceHeightAndSprite();
    testFirstItemIsFreeAndLaterOnesRoll();
    testCompostChances();
    testFillRipenAndExtract();
    testFullComposterKeepsTheItem();
    return 0;
}
