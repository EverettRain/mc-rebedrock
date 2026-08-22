// DIM-3: per-dimension streaming + generation hook (headless).
//
// DIM-3 only builds the *seam* — the per-dimension seed derivation, the
// generator-config hook (bounds from the DimensionType, generator-present flag),
// the idle-skip gate, and the consumer of DIM-2's recorded cross-dimension load
// requests. Real Nether/End terrain is the worldgen subtree's delivery and is
// NOT generated here; the hook reports hasTerrainGenerator == false for them.
#include "gameplay/GameSession.hpp"  // SimulationHost lives here
#include "gameplay/Level.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/Dimension.hpp"
#include "world/DimensionGenerator.hpp"
#include "world/World.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>

using mc::world::DimensionId;
using mc::world::dimensionGeneratorConfig;
using mc::world::dimensionSeed;
using mc::world::dimensionType;

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
    // Register species first: constructing a Level (each holds a NaturalSpawner)
    // before the entity registry is populated prints a harmless "no mob will ever
    // spawn" diagnostic, and this test builds several standalone Levels.
    mc::gameplay::entities::registerBuiltinEntities();

    // --- Seed derivation: deterministic, per-dimension distinct ---------------
    const std::uint64_t worldSeed = 0x1234'5678'9ABC'DEF0ULL;
    // Deterministic: same world + dimension -> same seed, every call.
    assert(dimensionSeed(worldSeed, DimensionId::Nether)
           == dimensionSeed(worldSeed, DimensionId::Nether));
    // The Overworld keeps the world seed unchanged, so an existing single-
    // dimension world regenerates byte-for-byte (Sabotage ①'s inverse: no drift
    // for the Overworld).
    assert(dimensionSeed(worldSeed, DimensionId::Overworld) == worldSeed);
    // Every dimension derives a *different* seed — the Nether's noise is never the
    // Overworld's (Sabotage ①'s guard).
    assert(dimensionSeed(worldSeed, DimensionId::Nether) != worldSeed);
    assert(dimensionSeed(worldSeed, DimensionId::End) != worldSeed);
    assert(dimensionSeed(worldSeed, DimensionId::Nether)
           != dimensionSeed(worldSeed, DimensionId::End));
    // Different worlds -> different derived seeds for the same dimension.
    assert(dimensionSeed(worldSeed, DimensionId::Nether)
           != dimensionSeed(worldSeed + 1U, DimensionId::Nether));

    // --- Generator config: bounds from DimensionType, generator seam ----------
    {
        const auto ow = dimensionGeneratorConfig(DimensionId::Overworld);
        const auto nether = dimensionGeneratorConfig(DimensionId::Nether);
        const auto end = dimensionGeneratorConfig(DimensionId::End);

        // Height/ceiling come from the DimensionType, never a hardcoded 256
        // (Sabotage ②'s guard).
        assert(nether.height == dimensionType(DimensionId::Nether).height);
        assert(nether.minY == dimensionType(DimensionId::Nether).minY);
        assert(nether.hasCeiling == dimensionType(DimensionId::Nether).hasCeiling);
        assert(nether.hasCeiling);  // the Nether has its bedrock roof
        assert(ow.height == dimensionType(DimensionId::Overworld).height);
        assert(!ow.hasCeiling);

        // The generator seam: only the Overworld has a real terrain generator on
        // disk today; the Nether/End stay false until worldgen delivers theirs.
        assert(ow.hasTerrainGenerator);
        assert(!nether.hasTerrainGenerator);
        assert(!end.hasTerrainGenerator);
    }

    // --- Idle-skip: a dimension with no player / no generator does not stream --
    {
        mc::gameplay::Level ow;
        ow.id = DimensionId::Overworld;
        mc::world::World owWorld;
        ow.bindWorld(owWorld);
        // No player yet -> idle -> no streaming.
        assert(!ow.shouldStream());
        ow.hasPlayer = true;
        assert(ow.shouldStream());  // player present + generator present -> stream

        // The Nether has a player but no generator (seam) -> still does not
        // stream (Sabotage ③'s guard: an idle/generator-less dimension requests
        // nothing).
        mc::gameplay::Level nether;
        nether.id = DimensionId::Nether;
        mc::world::World netherWorld;
        nether.bindWorld(netherWorld);
        nether.hasPlayer = true;
        assert(!nether.shouldStream());  // no terrain generator -> no streaming
        // And with no world bound at all, nothing streams.
        mc::gameplay::Level unbound;
        unbound.id = DimensionId::Overworld;
        unbound.hasPlayer = true;
        assert(!unbound.shouldStream());
    }

    // --- Consumer of DIM-2's cross-dimension load requests --------------------
    // A cross-dimension query into an unloaded Nether coordinate records a
    // request (DIM-2). DIM-3's resolver routes it: the Nether has no generator, so
    // the request is deferred (held for worldgen), never generated in the tick.
    {
        mc::world::World overworld;
        loadFlatChunk(overworld, 0, 0);
        SilentHost host;
        mc::gameplay::GameSession session;
        session.setWorldGenerationSeed(worldSeed);
        session.bindPrimaryWorld(overworld);

        // The primary (Overworld) level derives the world seed unchanged.
        assert(session.primaryLevel().generationSeed == worldSeed);
        assert(session.level(DimensionId::Nether).generationSeed
               == dimensionSeed(worldSeed, DimensionId::Nether));

        // Query an unloaded Nether coordinate -> one recorded request.
        mc::world::World nether;
        session.bindWorld(DimensionId::Nether, nether);
        session.clearPendingCrossDimLoads();
        static_cast<void>(session.blockAcrossDimensions(DimensionId::Nether, 40, 0, 40));
        assert(session.pendingCrossDimLoads().size() == 1U);

        // Resolve: the Nether has no generator, so it is deferred, not routed to a
        // streamer, and the request stays queued for a future worldgen delivery.
        const auto routing = session.resolvePendingCrossDimLoads();
        assert(routing.routableToStreamer == 0U);
        assert(routing.deferredNoGenerator == 1U);
        assert(session.pendingCrossDimLoads().size() == 1U);  // still held

        // An Overworld cross-dimension request (a chunk not resident in the loaded
        // set) DOES route to a streamer, because the Overworld has a generator.
        session.clearPendingCrossDimLoads();
        static_cast<void>(session.blockAcrossDimensions(DimensionId::Overworld, 5000, 0, 5000));
        assert(session.pendingCrossDimLoads().size() == 1U);
        const auto owRouting = session.resolvePendingCrossDimLoads();
        assert(owRouting.routableToStreamer == 1U);
        assert(owRouting.deferredNoGenerator == 0U);
        assert(session.pendingCrossDimLoads().empty());  // routed, cleared
        // The Nether world was never generated by any of this.
        assert(nether.chunkCount() == 0U);
    }

    return 0;
}
