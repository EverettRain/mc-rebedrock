#include "gameplay/GameSession.hpp"
#include "gameplay/GameplayMutationSink.hpp"
#include "gameplay/Item.hpp"

#include "world/Block.hpp"
#include "world/BlockState.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"
#include "world/WorldMutationService.hpp"

#include <cassert>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// A3b's acceptance: every block change now flows through WorldMutationService,
// so the consequences a cell edit has — section/save invalidation, neighbour
// reactions, block-entity lifecycle, drops — are dispatched from one place
// instead of being re-assembled at each call site. What is asserted here is
// that the *whole chain* stays consistent, and specifically that it no longer
// depends on which caller made the edit.

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"mutation_flow_test line " + std::to_string(line) +
                                 " failed: " + expression};
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

using mc::gameplay::GameplayMutationSink;
using mc::world::Block;
using mc::world::BlockState;
using mc::world::MutationCause;
using mc::world::MutationFlags;

struct RecordedEdit final {
    int x = 0;
    int y = 0;
    int z = 0;
    BlockState state{};
};

// Records the render-side half of the pipeline: the streamer/persistence
// submission and the light preview. Both must fire for an edit to be visible
// and saved, which is exactly the pair a hand-written call site used to forget.
struct RecordingHost final : mc::gameplay::SimulationHost {
    std::vector<RecordedEdit> stateEdits;
    std::vector<RecordedEdit> previews;

    void submitWorldEdit(int x, int y, int z, Block block, std::uint8_t fluidLevel,
                         std::optional<mc::world::BlockOrientation> orientation) override {
        stateEdits.push_back(
            {x, y, z,
             BlockState{block, orientation.value_or(mc::world::defaultOrientation(block)),
                        fluidLevel}});
    }
    void submitWorldStateEdit(int x, int y, int z, BlockState state) override {
        stateEdits.push_back({x, y, z, state});
    }
    void previewBlockEdit(int x, int y, int z) override {
        previews.push_back({x, y, z, BlockState{}});
    }
    void playBlockBreak(Block, glm::vec3) override {}
    void playItemPickup(glm::vec3) override {}
    void playEat(glm::vec3) override {}
    void playPlayerHurt(glm::vec3) override {}
    void playPlayerFall(glm::vec3, bool) override {}
    void playBurp(glm::vec3) override {}
    void playCreatureHurt(const mc::gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureDeath(const mc::gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureAmbient(const mc::gameplay::entities::EntityType&, glm::vec3) override {}
    void playCreatureStep(const mc::gameplay::entities::EntityType&, glm::vec3) override {}
    void playFootstep(Block, glm::vec3, float) override {}
    void playSplash(glm::vec3, float) override {}
    void spawnBlockBreakParticles(glm::ivec3, Block) override {}
    void onPlayerDied() override {}
    void onFurnaceStateChanged() override {}
    void onEatingStarted() override {}
    void onEatingCancelled() override {}
};

[[nodiscard]] mc::world::World loadedWorld() {
    mc::world::World world;
    for (int chunkX = -1; chunkX <= 1; ++chunkX) {
        for (int chunkZ = -1; chunkZ <= 1; ++chunkZ) {
            world.setChunk({chunkX, chunkZ}, mc::world::Chunk{});
        }
    }
    return world;
}

} // namespace

int main() {
    // --- Breaking a block drives the whole chain from one call: the cell is
    // cleared, the section/save submission and the light preview both fire, and
    // the simulation is woken so a sand block above falls. Before A3b each of
    // those was a separate statement the caller had to remember. ---
    {
        auto world = loadedWorld();
        mc::gameplay::GameSession session;
        RecordingHost host;
        session.setEventHost(host);
        world.setState(4, 20, 4, BlockState{Block::Stone});
        world.setState(4, 21, 4, BlockState{Block::Sand});

        GameplayMutationSink sink{world, session};
        const auto result =
            session.worldMutations().setBlock(world, {4, 20, 4}, BlockState{}, MutationFlags::All,
                                              MutationCause::PlayerBreak, sink);

        REQUIRE(result.changed);
        REQUIRE(result.previous.block() == Block::Stone);
        REQUIRE(world.block(4, 20, 4) == Block::Air);
        // Section/save invalidation and the light preview, exactly once each.
        session.drainEvents();
        REQUIRE(host.stateEdits.size() == 1U);
        session.drainEvents();
        REQUIRE(host.stateEdits[0].x == 4 && host.stateEdits[0].y == 20);
        session.drainEvents();
        REQUIRE(host.stateEdits[0].state.block() == Block::Air);
        session.drainEvents();
        REQUIRE(host.previews.size() == 1U);
        // The neighbour pass reached the simulation: the sand above is queued
        // to fall. Nothing else in this test could have queued it.
        const auto changes = session.worldSimulation().tick(world);
        bool sandFell = false;
        for (const auto& change : changes) {
            if (change.position.y == 21 && change.position.x == 4 && change.position.z == 4) {
                sandFell = true;
            }
        }
        REQUIRE(sandFell);
    }

    // --- The same cell, the same resulting state, reached by two different
    // causes must produce the same world and the same render submission. This
    // is the property the scattered call sites did not have: a break notified
    // neighbours, a placement did not, a bucket did neither. ---
    {
        const auto run = [](MutationCause cause) {
            auto world = loadedWorld();
            mc::gameplay::GameSession session;
            RecordingHost host;
            session.setEventHost(host);
            world.setState(2, 30, 2, BlockState{Block::Stone});
            world.setState(2, 31, 2, BlockState{Block::Sand});
            GameplayMutationSink sink{world, session};
            // Drops are suppressed so the two causes are compared on the
            // *dispatch*, not on PlayerBreak's extra loot roll.
            static_cast<void>(session.worldMutations().setBlock(
                world, {2, 30, 2}, BlockState{}, MutationFlags::All | MutationFlags::SuppressDrops,
                cause, sink));
            return std::tuple{host.stateEdits.size(), host.previews.size(),
                              session.worldSimulation().tick(world).size(),
                              world.block(2, 30, 2)};
        };
        REQUIRE(run(MutationCause::PlayerBreak) == run(MutationCause::Fluid));
    }

    // --- Lighting a furnace changes the state but not the block, so its block
    // entity — and the smelt inside it — must survive. This is the regression
    // the SkipBlockEntity/same-block rule exists to prevent. ---
    {
        auto world = loadedWorld();
        mc::gameplay::GameSession session;
        RecordingHost host;
        session.setEventHost(host);
        world.setState(6, 40, 6, BlockState{Block::Furnace});
        REQUIRE(session.furnaceSystem().place({6, 40, 6}));
        auto* furnace = session.furnaceSystem().find({6, 40, 6});
        REQUIRE(furnace != nullptr);
        furnace->burnTicks = 77;

        GameplayMutationSink sink{world, session};
        const auto lit = world.state(6, 40, 6).withLit(true);
        const auto result = session.worldMutations().setBlock(
            world, {6, 40, 6}, lit, MutationFlags::All, MutationCause::ScheduledTick, sink);

        REQUIRE(result.changed);
        REQUIRE(world.state(6, 40, 6).lit());
        // Same block: the entity is still there, still holding its burn.
        auto* afterLighting = session.furnaceSystem().find({6, 40, 6});
        REQUIRE(afterLighting != nullptr);
        REQUIRE(afterLighting->burnTicks == 77);
        // And the render submission carried LIT, which the block/fluid/
        // orientation triple cannot express — an unlit furnace would be drawn.
        session.drainEvents();
        REQUIRE(host.stateEdits.size() == 1U);
        session.drainEvents();
        REQUIRE(host.stateEdits[0].state.lit());
    }

    // --- Breaking that furnace *is* a block change, so the entity is destroyed
    // and its contents spill. The call site no longer special-cases it. ---
    {
        auto world = loadedWorld();
        mc::gameplay::GameSession session;
        RecordingHost host;
        session.setEventHost(host);
        world.setState(6, 40, 6, BlockState{Block::Furnace});
        REQUIRE(session.furnaceSystem().place({6, 40, 6}));
        session.furnaceSystem().find({6, 40, 6})->input = {Block::Stone, 3U};
        const std::size_t itemsBefore = session.itemEntities().entities().size();

        GameplayMutationSink sink{world, session};
        static_cast<void>(session.worldMutations().setBlock(world, {6, 40, 6}, BlockState{},
                                                            MutationFlags::All,
                                                            MutationCause::PlayerBreak, sink));
        session.drainEvents();

        REQUIRE(session.furnaceSystem().find({6, 40, 6}) == nullptr);
        REQUIRE(session.itemEntities().entities().size() > itemsBefore);
    }

    // --- Placing a chest creates its block entity through the same hook, so a
    // container placed by any caller is usable. ---
    {
        auto world = loadedWorld();
        mc::gameplay::GameSession session;
        RecordingHost host;
        session.setEventHost(host);
        GameplayMutationSink sink{world, session};
        static_cast<void>(session.worldMutations().setBlock(
            world, {8, 40, 8}, BlockState{Block::Chest}, MutationFlags::All,
            MutationCause::PlayerPlace, sink));
        REQUIRE(session.chestSystem().find({8, 40, 8}) != nullptr);
    }

    // --- BE2 lifecycle contract: the unified entry gates create/destroy on the
    // block's own hasBlockEntity pre-filter, so the three failures the contract
    // exists to forbid are each pinned here. ---

    // (1) Break a full chest: every one of its 27 slots spills and the entity is
    //     gone. This is the "forgot to spill" guard — a removal that only deletes
    //     the entity would scatter nothing.
    {
        auto world = loadedWorld();
        mc::gameplay::GameSession session;
        RecordingHost host;
        session.setEventHost(host);
        GameplayMutationSink placeSink{world, session};
        static_cast<void>(session.worldMutations().setBlock(
            world, {8, 40, 8}, BlockState{Block::Chest}, MutationFlags::All,
            MutationCause::PlayerPlace, placeSink));
        auto* chest = session.chestSystem().find({8, 40, 8});
        REQUIRE(chest != nullptr);
        // Fill all 27 slots so the spill count is exact rather than "some".
        for (auto& slot : chest->items) {
            slot = mc::gameplay::ItemStack{Block::Stone, 1U};
        }
        const std::size_t itemsBefore = session.itemEntities().entities().size();

        // Drops suppressed so the count is the container spill alone, not the
        // chest block's own loot on top of it.
        GameplayMutationSink breakSink{world, session};
        static_cast<void>(session.worldMutations().setBlock(
            world, {8, 40, 8}, BlockState{}, MutationFlags::All | MutationFlags::SuppressDrops,
            MutationCause::PlayerBreak, breakSink));
        session.drainEvents();
        REQUIRE(session.chestSystem().find({8, 40, 8}) == nullptr);
        REQUIRE(session.itemEntities().entities().size() - itemsBefore ==
                mc::gameplay::ChestBlockEntity::kSlotCount);
    }

    // (2) Break a furnace with all three slots and an item mid-smelt loaded: the
    //     input, the fuel and the output all spill. The old special-case only
    //     ever spilled what a test happened to set.
    {
        auto world = loadedWorld();
        mc::gameplay::GameSession session;
        RecordingHost host;
        session.setEventHost(host);
        GameplayMutationSink placeSink{world, session};
        static_cast<void>(session.worldMutations().setBlock(
            world, {6, 40, 6}, BlockState{Block::Furnace}, MutationFlags::All,
            MutationCause::PlayerPlace, placeSink));
        auto* furnace = session.furnaceSystem().find({6, 40, 6});
        REQUIRE(furnace != nullptr);
        furnace->input = mc::gameplay::ItemStack{Block::IronOre, 2U};
        furnace->fuel = mc::gameplay::ItemStack{Block::Air, 1U, &mc::gameplay::items::Coal};
        furnace->output = mc::gameplay::ItemStack{Block::Air, 1U, &mc::gameplay::items::IronIngot};
        const std::size_t itemsBefore = session.itemEntities().entities().size();

        // Drops suppressed so the count is the three furnace slots alone.
        GameplayMutationSink breakSink{world, session};
        static_cast<void>(session.worldMutations().setBlock(
            world, {6, 40, 6}, BlockState{}, MutationFlags::All | MutationFlags::SuppressDrops,
            MutationCause::PlayerBreak, breakSink));
        session.drainEvents();
        REQUIRE(session.furnaceSystem().find({6, 40, 6}) == nullptr);
        REQUIRE(session.itemEntities().entities().size() - itemsBefore == 3U);
    }

    // (3) The pre-filter is a real gate, not a chest/furnace enumeration: placing
    //     a block with no block entity (stone) creates none, and breaking it
    //     tries to destroy none. A hasBlockEntity that leaked true for stone
    //     would try to mint a chest here.
    {
        auto world = loadedWorld();
        mc::gameplay::GameSession session;
        RecordingHost host;
        session.setEventHost(host);
        GameplayMutationSink sink{world, session};
        static_cast<void>(session.worldMutations().setBlock(
            world, {9, 40, 9}, BlockState{Block::Stone}, MutationFlags::All,
            MutationCause::PlayerPlace, sink));
        REQUIRE(session.chestSystem().find({9, 40, 9}) == nullptr);
        REQUIRE(session.furnaceSystem().find({9, 40, 9}) == nullptr);
    }

    // --- A write that resolves to the state already stored is free: no
    // submission, no preview, no neighbour wake-up. Re-placing the same block
    // must not churn the mesh pipeline. ---
    {
        auto world = loadedWorld();
        mc::gameplay::GameSession session;
        RecordingHost host;
        session.setEventHost(host);
        world.setState(1, 10, 1, BlockState{Block::Stone});
        GameplayMutationSink sink{world, session};
        const auto result = session.worldMutations().setBlock(
            world, {1, 10, 1}, BlockState{Block::Stone}, MutationFlags::All,
            MutationCause::PlayerPlace, sink);
        REQUIRE(!result.changed);
        session.drainEvents();
        REQUIRE(host.stateEdits.empty());
        session.drainEvents();
        REQUIRE(host.previews.empty());
    }

    // --- An edit outside the loaded world writes nothing and dispatches
    // nothing, rather than half-applying. ---
    {
        auto world = loadedWorld();
        mc::gameplay::GameSession session;
        RecordingHost host;
        session.setEventHost(host);
        GameplayMutationSink sink{world, session};
        const auto result = session.worldMutations().setBlock(
            world, {5, mc::world::kMinY - 1, 5}, BlockState{Block::Stone}, MutationFlags::All,
            MutationCause::Command, sink);
        REQUIRE(!result.changed);
        session.drainEvents();
        REQUIRE(host.stateEdits.empty());
        session.drainEvents();
        REQUIRE(host.previews.empty());
    }

    // --- updateLimit is the guard against a self-feeding update chain. Zero
    // must stop the recursion rather than crash or spin. ---
    {
        auto world = loadedWorld();
        mc::gameplay::GameSession session;
        RecordingHost host;
        session.setEventHost(host);
        world.setState(3, 12, 3, BlockState{Block::Stone});
        GameplayMutationSink sink{world, session};
        const auto result =
            session.worldMutations().setBlock(world, {3, 12, 3}, BlockState{}, MutationFlags::All,
                                              MutationCause::PlayerBreak, sink, 0);
        REQUIRE(result.changed);
        session.drainEvents();
        REQUIRE(host.stateEdits.size() == 1U);
    }

    return 0;
}
