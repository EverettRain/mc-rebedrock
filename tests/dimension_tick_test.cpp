// DIM-2: the cross-dimension tick loop (headless).
//
// Proves the four load-bearing invariants of the user-focus node:
//   1. Per-dimension tick: an active secondary dimension (a hand-loaded Nether)
//      advances its own creatures alongside the primary.
//   2. Empty dimension is free: a dimension with no loaded chunks is skipped for
//      the cost of a branch — its tick touches zero chunks and zero creatures.
//   3. Cross-dimension query never force-loads: reading an unloaded coordinate in
//      another dimension returns the default and records an async request; the
//      dimension's chunk count does not change (no synchronous generation).
//   4. Fixed-time clocks: the Nether/End clocks do not advance the day; the
//      Overworld's does. And the loop order is fixed by DimensionId (determinism).
#include "gameplay/Difficulty.hpp"
#include "gameplay/GameSession.hpp"  // SimulationHost lives here
#include "gameplay/Level.hpp"
#include "gameplay/entities/BuiltinSpecies.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/Dimension.hpp"
#include "world/World.hpp"

#include <cassert>
#include <cstddef>

using mc::world::DimensionId;

namespace {

struct SilentHost final : public mc::gameplay::SimulationHost {
    void submitWorldEdit(int, int, int, mc::world::Block, std::uint8_t,
                         std::optional<mc::world::BlockOrientation>) override {}
    void submitWorldStateEdit(int, int, int, mc::world::BlockState) override {}
    void previewBlockEdit(int, int, int) override {}
    void playBlockBreak(mc::world::Block, glm::vec3) override {}
    void playItemPickup(glm::vec3) override {}
    void playEat(glm::vec3) override {}
    void playPlayerHurt(glm::vec3) override {}
    void playPlayerFall(glm::vec3, bool) override {}
    void playBurp(glm::vec3) override {}
    void playCreatureHurt(const mc::gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureDeath(const mc::gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureAmbient(const mc::gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureStep(const mc::gameplay::entities::EntityType&, glm::vec3) override {}
    void playFootstep(mc::world::Block, glm::vec3, float) override {}
    void playSplash(glm::vec3, float) override {}
    void spawnBlockBreakParticles(glm::ivec3, mc::world::Block) override {}
    void onPlayerDied() override {}
    void onFurnaceStateChanged() override {}
    void onEatingStarted() override {}
    void onEatingCancelled() override {}
};

// A single stone-floored chunk at chunk coordinate (cx, cz).
void loadFlatChunk(mc::world::World& world, int cx, int cz) {
    mc::world::Chunk chunk;
    for (int x = 0; x < 16; ++x) {
        for (int z = 0; z < 16; ++z) {
            chunk.setBlock(x, 0, z, mc::world::Block::Stone);
        }
    }
    world.setChunk({cx, cz}, std::move(chunk));
}

}  // namespace

int main() {
    mc::gameplay::entities::registerBuiltinEntities();

    mc::world::World overworld;
    loadFlatChunk(overworld, 0, 0);
    loadFlatChunk(overworld, 1, 1);

    SilentHost host;
    mc::gameplay::GameSession session;
    session.bindPrimaryWorld(overworld);

    // --- Fixed-time clocks: pinned at bind, and stay pinned across ticks ------
    // DIM-0 puts the Nether at 18000 and the End at 6000; bindPrimaryWorld pins
    // and pauses them.
    const auto netherTime0 = session.clocks().totalTicks(mc::world::ClockId::Nether);
    const auto endTime0 = session.clocks().totalTicks(mc::world::ClockId::End);
    assert(netherTime0 == 18000U);
    assert(endTime0 == 6000U);
    const auto overworldTime0 = session.clocks().totalTicks(mc::world::ClockId::Overworld);

    // --- Empty dimensions are skipped -----------------------------------------
    // With only the Overworld loaded, one tick leaves the Nether and End dormant.
    session.tick(overworld, host);
    {
        const auto& reports = session.secondaryLevelReports();
        const auto nether = static_cast<std::size_t>(DimensionId::Nether);
        const auto end = static_cast<std::size_t>(DimensionId::End);
        // Sabotage ②'s guard: a dormant dimension's tick touches nothing.
        assert(reports[nether].skippedEmpty);
        assert(reports[nether].chunksResident == 0U);
        assert(reports[nether].creaturesTicked == 0U);
        assert(reports[end].skippedEmpty);
        assert(reports[end].chunksResident == 0U);
    }

    // The Overworld clock advanced a tick; the fixed-time clocks did not.
    assert(session.clocks().totalTicks(mc::world::ClockId::Overworld) == overworldTime0 + 1U);
    assert(session.clocks().totalTicks(mc::world::ClockId::Nether) == netherTime0);
    assert(session.clocks().totalTicks(mc::world::ClockId::End) == endTime0);

    // --- A bound-but-empty dimension is still free ----------------------------
    // Bind the Nether to a world with NO loaded chunks and even put a creature in
    // it: because no chunk is resident, the tick skips the whole level — the
    // creature is not ticked. This is the sharpest form of Sabotage ②'s guard:
    // "empty" is decided by loaded chunks, not by whether a world object exists.
    {
        mc::world::World emptyNether;  // bound below; must outlive the tick calls
        session.bindWorld(DimensionId::Nether, emptyNether);
        session.level(DimensionId::Nether)
            .entities.spawn({8.0F, 1.001F, 8.0F}, mc::gameplay::entities::builtinSpecies("cow"), 3U);
        assert(emptyNether.chunkCount() == 0U);
        session.tick(overworld, host);
        const auto nid = static_cast<std::size_t>(DimensionId::Nether);
        assert(session.secondaryLevelReports()[nid].skippedEmpty);       // still skipped
        assert(session.secondaryLevelReports()[nid].creaturesTicked == 0U);  // cow dormant
        // Clear the creature so the later blocks start from an empty Nether level.
        session.level(DimensionId::Nether).entities.clear();
    }

    // --- Cross-dimension query never force-loads ------------------------------
    // The Nether has no chunk at (100, 0, 100). A query returns Air, records an
    // async request, and does NOT create the chunk. Sabotage ①'s guard.
    {
        mc::world::World nether;
        session.bindWorld(DimensionId::Nether, nether);
        assert(nether.chunkCount() == 0U);
        session.clearPendingCrossDimLoads();
        const auto block = session.blockAcrossDimensions(DimensionId::Nether, 100, 0, 100);
        assert(block == mc::world::Block::Air);              // default, not generated
        assert(nether.chunkCount() == 0U);                  // ★ no synchronous load
        assert(session.pendingCrossDimLoads().size() == 1U); // request recorded instead
        assert(session.pendingCrossDimLoads()[0].dimension == DimensionId::Nether);
        assert(session.pendingCrossDimLoads()[0].chunk.x == 6);  // floorDiv(100,16)
        assert(session.pendingCrossDimLoads()[0].chunk.z == 6);

        // A loaded chunk reads through without touching the request queue.
        loadFlatChunk(nether, 6, 6);
        session.clearPendingCrossDimLoads();
        const auto stone = session.blockAcrossDimensions(DimensionId::Nether, 100, 0, 100);
        assert(stone == mc::world::Block::Stone);
        assert(session.pendingCrossDimLoads().empty());  // loaded: no request
        // Negative coordinates land on the correct (floor-divided) chunk.
        session.clearPendingCrossDimLoads();
        static_cast<void>(session.blockAcrossDimensions(DimensionId::Nether, -1, 0, -1));
        assert(session.pendingCrossDimLoads().size() == 1U);
        assert(session.pendingCrossDimLoads()[0].chunk.x == -1);
        assert(session.pendingCrossDimLoads()[0].chunk.z == -1);
    }

    // --- An active secondary dimension ticks its own creatures ----------------
    // Give the Nether a loaded chunk and a cow, then tick: the Nether level is no
    // longer dormant and its creature advances.
    {
        mc::world::World nether;
        loadFlatChunk(nether, 0, 0);
        session.bindWorld(DimensionId::Nether, nether);
        session.level(DimensionId::Nether)
            .entities.spawn({8.0F, 1.001F, 8.0F}, mc::gameplay::entities::builtinSpecies("cow"), 7U);

        session.tick(overworld, host);
        const auto& reports = session.secondaryLevelReports();
        const auto nid = static_cast<std::size_t>(DimensionId::Nether);
        assert(!reports[nid].skippedEmpty);          // active now
        assert(reports[nid].chunksResident == 1U);   // the one loaded chunk
        assert(reports[nid].creaturesTicked == 1U);  // the cow ticked
        // The End is still dormant — one active secondary dimension does not wake
        // the others.
        assert(reports[static_cast<std::size_t>(DimensionId::End)].skippedEmpty);
    }

    // --- Determinism: the loop is fixed DimensionId order, same seed same herd -
    // Two Nether levels, same seed, same tick count, produce the same creature
    // pose. Sabotage ③'s guard against hash-order / RNG drift.
    {
        auto runNether = [&]() {
            mc::world::World nether;
            loadFlatChunk(nether, 0, 0);
            mc::gameplay::Level lvl;
            lvl.id = DimensionId::Nether;
            lvl.bindWorld(nether);
            lvl.entities.spawn({8.0F, 1.001F, 8.0F},
                               mc::gameplay::entities::builtinSpecies("cow"), 21U);
            for (int t = 0; t < 20; ++t) {
                lvl.tickPassive(/*doWeatherCycle=*/true, mc::gameplay::Difficulty::Normal);
            }
            // Return a coarse pose signature.
            const auto& cow = lvl.entities.entities().front();
            return cow.position.x + cow.position.z * 31.0F + cow.yaw * 7.0F;
        };
        const float a = runNether();
        const float b = runNether();
        assert(a == b);  // reproducible across runs (self-contained RNG stream)
    }

    return 0;
}
