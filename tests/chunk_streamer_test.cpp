#include "world/ChunkStreamer.hpp"
#include "world/SurfaceGenerator.hpp"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <ranges>
#include <thread>

namespace {

[[nodiscard]] std::optional<mc::world::ChunkStreamBatch>
waitForBatch(mc::world::ChunkStreamer& streamer,
             std::chrono::seconds timeout = std::chrono::seconds{5}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto batch = streamer.poll()) {
            return batch;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return std::nullopt;
}

} // namespace

int main() {
    assert(mc::world::chunkPositionFromWorld(0.0F, 0.0F) == (mc::world::ChunkPosition{0, 0}));
    assert(mc::world::chunkPositionFromWorld(15.99F, 15.99F) == (mc::world::ChunkPosition{0, 0}));
    assert(mc::world::chunkPositionFromWorld(16.0F, -0.01F) == (mc::world::ChunkPosition{1, -1}));
    assert(mc::world::chunkPositionFromWorld(-16.01F, -16.0F) ==
           (mc::world::ChunkPosition{-2, -1}));

    const auto radiusTwo = mc::world::chunkPositionsInRadius({3, -2}, 2);
    assert(radiusTwo.size() == 25U);
    assert(radiusTwo.front() == (mc::world::ChunkPosition{1, -4}));
    assert(radiusTwo.back() == (mc::world::ChunkPosition{5, 0}));
    mc::world::ChunkStreamer streamer{0x5EEDULL, 0, 0};
    streamer.request({0, 0});
    auto first = waitForBatch(streamer);
    assert(first.has_value());
    assert(first->loadedChunkCount == 1U);
    assert(first->chunkUpdates.size() == 1U);
    assert(!first->sectionUpdates.empty());
    assert(first->sectionUpdates.size() < 16U);

    const auto editSubmitStarted = std::chrono::steady_clock::now();
    streamer.setBlock(2, 10, 3, mc::world::Block::Air);
    const auto editSubmitElapsed = std::chrono::steady_clock::now() - editSubmitStarted;
    assert(editSubmitElapsed < std::chrono::milliseconds{50});
    auto edited = waitForBatch(streamer);
    const auto editCompletedElapsed = std::chrono::steady_clock::now() - editSubmitStarted;
    assert(edited.has_value());
    assert(edited->chunkUpdates.empty());
    assert(edited->highPriority);
    assert(edited->sectionUpdates.size() == 1U);
    assert(editCompletedElapsed < std::chrono::milliseconds{250});

    // Transparent blocks used to trigger a full 3x3x3 lighting-tile rebuild.
    // The persistent engine should keep this edit local as well.
    const auto leavesStarted = std::chrono::steady_clock::now();
    streamer.setBlock(2, 200, 3, mc::world::Block::OakLeaves);
    auto leavesEdited = waitForBatch(streamer);
    const auto leavesElapsed = std::chrono::steady_clock::now() - leavesStarted;
    assert(leavesEdited.has_value());
    assert(leavesEdited->highPriority);
    assert(!leavesEdited->sectionUpdates.empty());
    assert(leavesEdited->sectionUpdates.size() <= 8U);
    assert(leavesElapsed < std::chrono::milliseconds{250});

    streamer.request({1, 0});
    auto shifted = waitForBatch(streamer);
    assert(shifted.has_value());
    assert(shifted->loadedChunkCount == 1U);
    // Sixteen removal records plus only non-empty sections for the newly
    // generated chunk; empty sections are not useful GPU uploads.
    assert(shifted->sectionUpdates.size() > 16U);
    assert(shifted->sectionUpdates.size() < 32U);

    const auto epoch = streamer.resetWorld(0xBEEFULL, {{2, 200, 3, mc::world::BlockState{mc::world::Block::Bricks}}});
    streamer.request({0, 0});
    auto restored = waitForBatch(streamer);
    assert(restored.has_value());
    assert(restored->worldEpoch == epoch);
    assert(restored->chunkUpdates.size() == 1U);
    assert(restored->chunkUpdates.front().chunk.block(2, 200, 3) == mc::world::Block::Bricks);

    // Runtime edits remain authoritative after their chunk is unloaded and
    // regenerated during the same world session.
    streamer.setBlock(2, 200, 3, mc::world::Block::Glowstone);
    auto persistedEdit = waitForBatch(streamer);
    assert(persistedEdit.has_value());
    assert(persistedEdit->appliedBlockEditCount == 1U);
    streamer.request({1, 0});
    assert(waitForBatch(streamer).has_value());
    streamer.request({0, 0});
    auto reloaded = waitForBatch(streamer);
    assert(reloaded.has_value());
    const auto reloadedChunk =
        std::ranges::find_if(reloaded->chunkUpdates, [](const mc::world::ChunkDataUpdate& update) {
            return !update.remove && update.position == mc::world::ChunkPosition{0, 0};
        });
    assert(reloadedChunk != reloaded->chunkUpdates.end());
    assert(reloadedChunk->chunk.block(2, 200, 3) == mc::world::Block::Glowstone);

    streamer.setRadii(1, 2);
    assert(streamer.loadRadius() == 1);
    assert(streamer.unloadRadius() == 2);
    auto expanded = waitForBatch(streamer);
    assert(expanded.has_value());
    assert(expanded->loadedChunkCount == 9U);

    // Vanilla spawn chunks: a protected region survives a move of the load
    // centre. Protect {0,0} (already loaded at radius 1) and move far away.
    streamer.protectChunks({0, 0}, 0);
    assert(streamer.protectedRadius() == 0);
    streamer.protectChunks({0, 0}, 1);
    assert(streamer.protectedRadius() == 1);
    streamer.request({6, 6});
    auto movedAway = waitForBatch(streamer);
    assert(movedAway.has_value());
    // The protected chunk is still served after the move: the batch contains a
    // non-removal chunk update for {0,0} (or a remesh), never a removal of it.
    const auto centreUpdate = std::ranges::find_if(
        movedAway->chunkUpdates, [](const mc::world::ChunkDataUpdate& update) {
            return update.position == mc::world::ChunkPosition{0, 0};
        });
    assert(centreUpdate == movedAway->chunkUpdates.end() ||
           !centreUpdate->remove);

    // requestSync force-loads a specific chunk on demand: the vanilla-style
    // anti-void fallback (ServerChunkManager#getChunk with create=true) used
    // when the player reaches the streaming boundary. The chunk lies far
    // outside the 9x9 request radius, so only the sync pass can deliver it.
    {
        mc::world::ChunkStreamer syncStreamer{0x5EEDULL, 4, 4};
        syncStreamer.request({0, 0});
        const auto syncStarted = std::chrono::steady_clock::now();
        auto syncBatch = syncStreamer.requestSync(
            {7, 7}, std::chrono::milliseconds{8000});
        const auto syncElapsed =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - syncStarted).count();
        assert(syncBatch.has_value());
        const auto syncChunk = std::ranges::find_if(
            syncBatch->chunkUpdates, [](const mc::world::ChunkDataUpdate& update) {
                return !update.remove && update.position == (mc::world::ChunkPosition{7, 7});
            });
        assert(syncChunk != syncBatch->chunkUpdates.end());
        // Generous bound: on the debug build the initial 9x9 load runs first,
        // then the single sync chunk is produced.
        assert(syncElapsed < 7000.0);
        // The chunk now exists in the worker's world; a normal request centred
        // on it streams the surrounding area.
        syncStreamer.request({7, 7});
        assert(waitForBatch(syncStreamer).has_value());
    }

    // An orientation-only edit — a crop growing to a new stage, farmland gaining
    // moisture — must reach the worker and remesh the section, or the growth
    // would advance silently in the simulation while the rendered crop never
    // changed. The crop is the only block in its (high, terrain-free) section,
    // so the cutout mesh holds just its own stage texture.
    {
        // The headless worker meshes with the registry-built atlas layers, which
        // the renderer fills at startup; pin a stage base here so the crop's
        // stage0 + age resolves deterministically in the test.
        mc::world::setBlockTextureLayers(mc::world::Block::WheatCrops,
                                         {274.0F, 274.0F, 274.0F});
        mc::world::ChunkStreamer cropStreamer{0x5EEDULL, 0, 0};
        cropStreamer.request({0, 0});
        assert(waitForBatch(cropStreamer).has_value());
        cropStreamer.setState(8, 120, 8,
                              mc::world::BlockState{mc::world::Block::WheatCrops}.withAge(0));
        auto planted = waitForBatch(cropStreamer);
        assert(planted.has_value());
        const auto stage0Layer = static_cast<std::uint16_t>(
            mc::world::cropTextureLayer(mc::world::Block::WheatCrops, 0));
        bool sawStage0 = false;
        for (const auto& update : planted->sectionUpdates) {
            for (const auto& vertex : update.mesh.cutoutMesh.vertices) {
                if (vertex.textureLayer == stage0Layer) {
                    sawStage0 = true;
                }
            }
        }
        assert(sawStage0);
        // Same block, different age: the worker must still apply the new state
        // and rebuild the section rather than skipping an "unchanged" block.
        cropStreamer.setState(8, 120, 8,
                              mc::world::BlockState{mc::world::Block::WheatCrops}.withAge(5));
        auto grown = waitForBatch(cropStreamer);
        assert(grown.has_value());
        const auto stage5Layer = static_cast<std::uint16_t>(
            mc::world::cropTextureLayer(mc::world::Block::WheatCrops, 5));
        bool sawStage5 = false;
        for (const auto& update : grown->sectionUpdates) {
            for (const auto& vertex : update.mesh.cutoutMesh.vertices) {
                if (vertex.textureLayer == stage5Layer) {
                    sawStage5 = true;
                }
            }
        }
        assert(sawStage5);
    }

    // A tree generated by one chunk may extend into an already loaded
    // neighbour. That write used to exist only in the worker's World and GPU
    // mesh; gameplay retained the neighbour's old state, producing visible
    // leaves with missing collision/raycast blocks. Find one deterministic
    // cross-border crown and prove the worker publishes its state delta.
    {
        constexpr std::uint64_t borderSeed = 0xC0FFEEULL;
        struct BorderCandidate final {
            mc::world::ChunkPosition source;
            mc::world::ChunkPosition target;
            mc::world::gen::TreeBorderBlock block;
            mc::world::BlockState expected;
        };
        std::optional<BorderCandidate> candidate;
        const mc::world::SurfaceGenerator generator{borderSeed};
        // This seed's origin is mostly plains; the 12x12 generation survey
        // reaches its wooded biomes while keeping the probe bounded.
        for (int chunkZ = 0; chunkZ < 12 && !candidate.has_value(); ++chunkZ) {
            for (int chunkX = 0; chunkX < 12 && !candidate.has_value(); ++chunkX) {
                std::vector<mc::world::gen::TreeBorderBlock> borderBlocks;
                static_cast<void>(generator.generate(chunkX, chunkZ, borderBlocks));
                for (const auto& block : borderBlocks) {
                    const auto target = mc::world::chunkPositionFromWorld(
                        static_cast<float>(block.worldX), static_cast<float>(block.worldZ));
                    auto targetChunk = generator.generate(target.x, target.z);
                    const int localX = block.worldX - target.x * mc::world::kChunkWidth;
                    const int localZ = block.worldZ - target.z * mc::world::kChunkDepth;
                    const auto expected = targetChunk.state(localX, block.y, localZ);
                    if (target != mc::world::ChunkPosition{chunkX, chunkZ} &&
                        mc::world::gen::treeReplaceable(expected.block())) {
                        candidate = BorderCandidate{{chunkX, chunkZ}, target, block, expected};
                        break;
                    }
                }
            }
        }
        assert(candidate.has_value());

        mc::world::ChunkStreamer borderStreamer{borderSeed, 0, 2};
        borderStreamer.request(candidate->target);
        assert(waitForBatch(borderStreamer).has_value());
        borderStreamer.request(candidate->source);
        const auto sourceBatch = waitForBatch(borderStreamer);
        assert(sourceBatch.has_value());
        const auto delivered = std::ranges::find_if(
            sourceBatch->stateUpdates, [&candidate](const mc::world::BlockStateDelta& update) {
                return update.worldX == candidate->block.worldX &&
                       update.y == candidate->block.y &&
                       update.worldZ == candidate->block.worldZ;
            });
        assert(delivered != sourceBatch->stateUpdates.end());
        assert(delivered->expected == candidate->expected);
        assert(delivered->state == candidate->block.state);

        // An explicit Air edit at the same position is authoritative. This is
        // the real broken-leaf case: a late/replayed crown must not resurrect
        // a leaf whose generated predecessor was already Air as well.
        mc::world::ChunkStreamer editedBorderStreamer{borderSeed, 0, 2};
        const auto editedEpoch = editedBorderStreamer.resetWorld(
            borderSeed,
            {{candidate->block.worldX, candidate->block.y, candidate->block.worldZ,
              mc::world::BlockState{mc::world::Block::Air}}});
        editedBorderStreamer.request(candidate->target);
        const auto targetBatch = waitForBatch(editedBorderStreamer);
        assert(targetBatch.has_value());
        assert(targetBatch->worldEpoch == editedEpoch);
        editedBorderStreamer.request(candidate->source);
        const auto editedSourceBatch = waitForBatch(editedBorderStreamer);
        assert(editedSourceBatch.has_value());
        const auto resurrected = std::ranges::find_if(
            editedSourceBatch->stateUpdates,
            [&candidate](const mc::world::BlockStateDelta& update) {
                return update.worldX == candidate->block.worldX &&
                       update.y == candidate->block.y &&
                       update.worldZ == candidate->block.worldZ;
            });
        assert(resurrected == editedSourceBatch->stateUpdates.end());
    }

    // Opt-in local benchmark for profiling the default 9x9 streaming window
    // without slowing down the normal regression suite. The streamer now
    // delivers nearest-first batches rather than one monolithic batch, so the
    // benchmark drains them all and times when the full 9x9 is present.
    if (std::getenv("MC_REBEDROCK_STREAM_BENCH") != nullptr) {
        const auto benchmarkStarted = std::chrono::steady_clock::now();
        streamer.setRadii(4, 4);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
        std::size_t loaded = 0U;
        while (loaded != 81U && std::chrono::steady_clock::now() < deadline) {
            while (auto batch = streamer.poll()) {
                loaded = std::max(loaded, batch->loadedChunkCount);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        assert(loaded == 81U);
        const auto elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - benchmarkStarted);
        std::cout << "9x9 chunk stream: " << elapsed.count() << "s\n";
    }

    // Destruction must not wait for a complete lighting pass. This protects
    // application shutdown while a large view-distance request is in flight.
    const auto stopStarted = std::chrono::steady_clock::now();
    {
        mc::world::ChunkStreamer cancellable{0xC0FFEEULL, 8, 8};
        cancellable.request({0, 0});
        cancellable.stop();
    }
    const auto stopElapsed = std::chrono::steady_clock::now() - stopStarted;
    assert(stopElapsed < std::chrono::seconds{1});
    return 0;
}
