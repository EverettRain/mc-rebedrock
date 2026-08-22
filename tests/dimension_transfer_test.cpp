// DIM-5: dimension transfer mechanism (headless).
//
// Proves the four testable invariants of the transfer mechanism:
//   1. Coordinate scaling reads DimensionType.coordinateScale (Overworld->Nether
//      divides by 8, Nether->Overworld multiplies by 8, same-scale pairs are
//      unchanged) — never a hardcoded 8.
//   2. Entity transfer moves a creature between Levels preserving its state and
//      RNG stream, into the target Level's entity system.
//   3. Transfer to an unloaded destination queues + records an async request and
//      does NOT synchronously generate the chunk (lowframe long-tail rule); a
//      later drain lands it once the chunk is resident.
//   4. Player transfer repoints the primary dimension and hands the hasPlayer flag.
#include "gameplay/DimensionTransfer.hpp"
#include "gameplay/GameSession.hpp"  // SimulationHost lives here
#include "gameplay/Level.hpp"
#include "gameplay/entities/CowEntity.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/Dimension.hpp"
#include "world/World.hpp"

#include <cassert>
#include <cmath>
#include <cstddef>

using mc::gameplay::scaleCoordinatesBetweenDimensions;
using mc::world::DimensionId;

namespace {

void loadFlatChunk(mc::world::World& world, int cx, int cz) {
    mc::world::Chunk chunk;
    for (int x = 0; x < 16; ++x) {
        for (int z = 0; z < 16; ++z) {
            chunk.setBlock(x, 0, z, mc::world::Block::Stone);
        }
    }
    world.setChunk({cx, cz}, std::move(chunk));
}

bool nearlyEqual(float a, float b) { return std::fabs(a - b) < 1e-3F; }

}  // namespace

int main() {
    mc::gameplay::entities::registerBuiltinEntities();

    // --- Coordinate scaling reads coordinateScale -----------------------------
    // Overworld -> Nether: X/Z / 8 (scale 1 / 8), Y unchanged. Sabotage ①'s guard.
    {
        const glm::vec3 in{80.0F, 64.0F, -160.0F};
        const glm::vec3 toNether = scaleCoordinatesBetweenDimensions(
            in, DimensionId::Overworld, DimensionId::Nether);
        assert(nearlyEqual(toNether.x, 10.0F));
        assert(nearlyEqual(toNether.z, -20.0F));
        assert(nearlyEqual(toNether.y, 64.0F));  // vertical never scaled

        // Nether -> Overworld: X/Z * 8.
        const glm::vec3 backToOw = scaleCoordinatesBetweenDimensions(
            toNether, DimensionId::Nether, DimensionId::Overworld);
        assert(nearlyEqual(backToOw.x, 80.0F));
        assert(nearlyEqual(backToOw.z, -160.0F));

        // Overworld <-> End: both scale 1, coordinate unchanged.
        const glm::vec3 toEnd = scaleCoordinatesBetweenDimensions(
            in, DimensionId::Overworld, DimensionId::End);
        assert(nearlyEqual(toEnd.x, 80.0F) && nearlyEqual(toEnd.z, -160.0F));
    }

    mc::world::World overworld;
    loadFlatChunk(overworld, 0, 0);
    mc::gameplay::GameSession session;
    session.bindPrimaryWorld(overworld);

    // --- Entity transfer preserves state/RNG, moves Levels --------------------
    // Bind a Nether world with a loaded chunk at the scaled destination, spawn a
    // cow in the Overworld, then transfer it. It leaves the Overworld level and
    // appears in the Nether level with its state intact.
    {
        mc::world::World nether;
        // The cow will be at Overworld (8,1,8) -> Nether (1,1,1) -> chunk (0,0).
        loadFlatChunk(nether, 0, 0);
        session.bindWorld(DimensionId::Nether, nether);

        session.level(DimensionId::Overworld)
            .entities.spawn({8.0F, 1.001F, 8.0F}, mc::gameplay::entities::CowEntity::type(), 42U);
        const auto id = session.level(DimensionId::Overworld).entities.entities().front().id;
        // Capture pre-transfer scalar state for the RNG/state-preservation check
        // (SimpleEntity is non-copyable — it owns a MobBrain — so read fields, not
        // the whole value).
        const auto* before = session.level(DimensionId::Overworld).entities.byIdConst(id);
        const auto beforeRng = before->rngState;
        const auto beforeHealth = before->damage.health;

        const auto result = session.transferEntity(id, DimensionId::Overworld, DimensionId::Nether);
        assert(result == mc::gameplay::GameSession::TransferResult::Moved);
        // Gone from the Overworld level.
        assert(session.level(DimensionId::Overworld).entities.entities().empty());
        // Present in the Nether level, at the scaled position.
        assert(session.level(DimensionId::Nether).entities.entities().size() == 1U);
        const auto& moved = session.level(DimensionId::Nether).entities.entities().front();
        assert(nearlyEqual(moved.position.x, 1.0F));  // 8 / 8
        assert(nearlyEqual(moved.position.z, 1.0F));
        // Sabotage ③'s guard: state and RNG stream preserved across the move.
        assert(moved.rngState == beforeRng);
        assert(nearlyEqual(moved.damage.health, beforeHealth));
    }

    // --- Transfer to an unloaded destination queues, never generates ----------
    // Rebind the Nether to a world with NO chunk at the destination. A transfer is
    // queued + an async request recorded; the Nether world is NOT generated.
    // Sabotage ②'s guard.
    {
        mc::world::World emptyNether;  // no chunks
        session.bindWorld(DimensionId::Nether, emptyNether);
        // Start from a clean Nether level (the previous block left a cow there).
        session.level(DimensionId::Nether).entities.clear();
        session.clearPendingCrossDimLoads();
        session.level(DimensionId::Overworld)
            .entities.spawn({80.0F, 1.001F, 80.0F}, mc::gameplay::entities::CowEntity::type(), 7U);
        const auto id = session.level(DimensionId::Overworld).entities.entities().front().id;

        const auto result = session.transferEntity(id, DimensionId::Overworld, DimensionId::Nether);
        assert(result == mc::gameplay::GameSession::TransferResult::QueuedAwaitingChunk);
        // The creature left the Overworld (it is held in the transfer queue).
        assert(session.level(DimensionId::Overworld).entities.entities().empty());
        assert(session.level(DimensionId::Nether).entities.entities().empty());
        assert(session.queuedTransfers().size() == 1U);
        // NOT generated: the Nether world still has zero chunks.
        assert(emptyNether.chunkCount() == 0U);
        // An async load request was recorded for the destination chunk.
        assert(!session.pendingCrossDimLoads().empty());
        // Destination is Overworld (80,80) -> Nether (10,10) -> chunk (0,0).
        assert(session.queuedTransfers().front().destinationChunk.x == 0);
        assert(session.queuedTransfers().front().destinationChunk.z == 0);

        // Draining before the chunk loads lands nothing (still no force-load).
        assert(session.drainQueuedTransfers() == 0U);
        assert(session.queuedTransfers().size() == 1U);
        assert(emptyNether.chunkCount() == 0U);

        // Once the destination chunk streams in, the drain lands the creature.
        loadFlatChunk(emptyNether, 0, 0);
        assert(session.drainQueuedTransfers() == 1U);
        assert(session.queuedTransfers().empty());
        assert(session.level(DimensionId::Nether).entities.entities().size() == 1U);
        const auto& landed = session.level(DimensionId::Nether).entities.entities().front();
        assert(nearlyEqual(landed.position.x, 10.0F));  // 80 / 8
    }

    // --- Transfer to a dimension with no world bound is a no-op, not a loss ----
    {
        session.level(DimensionId::Overworld)
            .entities.spawn({8.0F, 1.001F, 8.0F}, mc::gameplay::entities::CowEntity::type(), 3U);
        const auto id = session.level(DimensionId::Overworld).entities.entities().back().id;
        const auto before = session.level(DimensionId::Overworld).entities.entities().size();
        // The End has no world bound in this test.
        const auto result = session.transferEntity(id, DimensionId::Overworld, DimensionId::End);
        assert(result == mc::gameplay::GameSession::TransferResult::NoTargetWorld);
        // The creature stayed in the Overworld (put back), count unchanged.
        assert(session.level(DimensionId::Overworld).entities.entities().size() == before);
    }

    // --- Player transfer repoints the primary dimension -----------------------
    {
        assert(session.primaryDimension() == DimensionId::Overworld);
        assert(session.level(DimensionId::Overworld).hasPlayer);
        assert(!session.level(DimensionId::Nether).hasPlayer);

        const glm::vec3 landing = session.transferPlayer(DimensionId::Nether);
        assert(session.primaryDimension() == DimensionId::Nether);
        // The hasPlayer flag moved with the player.
        assert(!session.level(DimensionId::Overworld).hasPlayer);
        assert(session.level(DimensionId::Nether).hasPlayer);
        // The landing coordinate is the scaled player position (player starts at
        // x=24 -> 24/8 = 3).
        assert(nearlyEqual(landing.x, 3.0F));
    }

    return 0;
}
