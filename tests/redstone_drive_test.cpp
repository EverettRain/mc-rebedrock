#include "gameplay/RedstoneSignal.hpp"
#include "gameplay/WorldSimulation.hpp"

#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <cstdio>
#include <cstdlib>

// W-4 slice 3a: the redstone drive wired into the real runtime — the scheduler's
// RedstoneComponent task and WorldSimulation's per-gametick drain. A torch that
// learns its input changed schedules a toggle, and the scheduler delivers that
// toggle exactly TOGGLE_DELAY (2gt) later, applied through the mutation service.
// The end-to-end circuit path (lever propagation + the W-4a harness) is 3b; this
// pins the driving loop itself: reaction -> schedule -> drain -> apply.

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "redstone_drive_test line %d failed: %s\n", line, expression);
        std::abort();
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

using mc::world::Block;
using mc::world::BlockState;

[[nodiscard]] mc::world::World loadedWorld() {
    mc::world::World world;
    for (int cx = -1; cx <= 1; ++cx) {
        for (int cz = -1; cz <= 1; ++cz) {
            world.setChunk({cx, cz}, mc::world::Chunk{});
        }
    }
    return world;
}

[[nodiscard]] bool lit(const mc::world::World& world, int x, int y, int z) {
    return world.state(x, y, z).lit();
}

} // namespace

int main() {
    using mc::gameplay::WorldSimulation;

    // A vertical torch inverter, built by hand (setState, since this test drives
    // the component reaction directly rather than through a lever's propagation):
    //   (0,62,0) Stone     — supports the input torch
    //   (0,63,0) RedstoneTorch (lit) — the input source, strongly powers base
    //   (0,64,0) Stone     — base: the block the output torch stands on
    //   (0,65,0) RedstoneTorch (lit) — the output torch under test
    auto world = loadedWorld();
    world.setState(0, 62, 0, BlockState{Block::Stone});
    world.setState(0, 63, 0, BlockState{Block::RedstoneTorch}.withLit(true));
    world.setState(0, 64, 0, BlockState{Block::Stone});
    world.setState(0, 65, 0, BlockState{Block::RedstoneTorch}.withLit(true));

    WorldSimulation simulation;

    // Sanity: the output torch reads its input as powered (base is strongly
    // powered by the torch below it and re-emits as a conductor).
    REQUIRE(mc::gameplay::redstone::torchHasNeighborSignal(world, {0, 65, 0}));

    // --- Input already on: the output torch should invert to off, 2gt after it
    //     is told. It is told at t=0; the toggle lands at t=2. ---
    simulation.notifyRedstoneComponent(world, {0, 65, 0});
    // One gametick: the tick is due at t=2, so nothing has flipped yet.
    static_cast<void>(simulation.tick(world, true));
    REQUIRE(lit(world, 0, 65, 0)); // still lit at t=1
    // Second gametick: t=2, the toggle fires and the torch goes out.
    static_cast<void>(simulation.tick(world, true));
    REQUIRE(!lit(world, 0, 65, 0)); // inverted off at t=2

    // Stable: an off inverter with a powered input has nothing more to do.
    static_cast<void>(simulation.tick(world, true));
    static_cast<void>(simulation.tick(world, true));
    REQUIRE(!lit(world, 0, 65, 0));

    // --- Input falls: cut the source, tell the torch, and it relights 2gt on. ---
    world.setState(0, 63, 0, BlockState{Block::RedstoneTorch}.withLit(false));
    REQUIRE(!mc::gameplay::redstone::torchHasNeighborSignal(world, {0, 65, 0}));
    simulation.notifyRedstoneComponent(world, {0, 65, 0});
    static_cast<void>(simulation.tick(world, true));
    REQUIRE(!lit(world, 0, 65, 0)); // not yet
    static_cast<void>(simulation.tick(world, true));
    REQUIRE(lit(world, 0, 65, 0)); // relit

    // --- Dedup: hammering the input before the tick fires still yields one flip.
    //     Re-power the source and notify repeatedly; only one toggle is queued. ---
    world.setState(0, 63, 0, BlockState{Block::RedstoneTorch}.withLit(true));
    simulation.notifyRedstoneComponent(world, {0, 65, 0});
    simulation.notifyRedstoneComponent(world, {0, 65, 0});
    simulation.notifyRedstoneComponent(world, {0, 65, 0});
    static_cast<void>(simulation.tick(world, true));
    static_cast<void>(simulation.tick(world, true));
    REQUIRE(!lit(world, 0, 65, 0)); // off, once
    // And no stray second toggle lingered to flip it back.
    static_cast<void>(simulation.tick(world, true));
    static_cast<void>(simulation.tick(world, true));
    REQUIRE(!lit(world, 0, 65, 0));

    return 0;
}
