#include "world/ChunkStreamer.hpp"

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

    const auto epoch = streamer.resetWorld(0xBEEFULL, {{2, 200, 3, mc::world::Block::Bricks, 0U}});
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
        mc::world::ChunkStreamer cropStreamer{0x5EEDULL, 0, 0};
        cropStreamer.request({0, 0});
        assert(waitForBatch(cropStreamer).has_value());
        cropStreamer.setBlock(8, 120, 8, mc::world::Block::WheatCrops, 0U,
                              mc::world::cropOrientation(0));
        auto planted = waitForBatch(cropStreamer);
        assert(planted.has_value());
        bool sawStage0 = false;
        for (const auto& update : planted->sectionUpdates) {
            for (const auto& vertex : update.mesh.cutoutMesh.vertices) {
                if (vertex.textureLayer ==
                    static_cast<std::uint16_t>(mc::world::kWheatStage0Layer)) {
                    sawStage0 = true;
                }
            }
        }
        assert(sawStage0);
        // Same block, different age: the worker must still apply the orientation
        // and rebuild the section rather than skipping an "unchanged" block.
        cropStreamer.setBlock(8, 120, 8, mc::world::Block::WheatCrops, 0U,
                              mc::world::cropOrientation(5));
        auto grown = waitForBatch(cropStreamer);
        assert(grown.has_value());
        bool sawStage5 = false;
        for (const auto& update : grown->sectionUpdates) {
            for (const auto& vertex : update.mesh.cutoutMesh.vertices) {
                if (vertex.textureLayer ==
                    static_cast<std::uint16_t>(mc::world::kWheatStage5Layer)) {
                    sawStage5 = true;
                }
            }
        }
        assert(sawStage5);
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
