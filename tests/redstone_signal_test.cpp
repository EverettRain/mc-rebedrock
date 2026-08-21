#include "gameplay/RedstoneSignal.hpp"

#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <cstdio>
#include <cstdlib>

// W-4 slice 1: the redstone signal model, pinned against Java 26.1's exact
// semantics (SignalGetter / RedstoneTorchBlock / PoweredBlock). The subtle part
// this guards is weak vs strong power: a redstone torch strongly powers the block
// above it (so a conductor re-emits its signal a cell further), while a block of
// redstone is weak-only and does NOT power an adjacent conductor.

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "redstone_signal_test line %d failed: %s\n", line, expression);
        std::abort();
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

using namespace mc::gameplay;
using mc::world::Block;
using mc::world::BlockOrientation;
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

// --- isSignalSource pre-filter. ---
void testIsSignalSource() {
    REQUIRE(redstone::isSignalSource(Block::RedstoneBlock));
    REQUIRE(redstone::isSignalSource(Block::RedstoneTorch));
    REQUIRE(redstone::isSignalSource(Block::RedstoneWallTorch));
    REQUIRE(!redstone::isSignalSource(Block::Stone));
    REQUIRE(!redstone::isSignalSource(Block::Air));
    REQUIRE(!redstone::isSignalSource(Block::Torch)); // the light torch is not a source
}

// --- Per-block weak/strong emission. ---
void testPerBlockSignal() {
    using redstone::Direction;

    // Block of redstone: weak 15 every side, strong 0 every side (weak-only).
    const BlockState redstone{Block::RedstoneBlock};
    for (const Direction dir : redstone::kAllDirections) {
        REQUIRE(redstone::getSignal(redstone, dir) == 15);
        REQUIRE(redstone::getDirectSignal(redstone, dir) == 0);
    }

    // Lit ground torch: weak 15 on all sides except UP; strong 15 only DOWN.
    const BlockState litTorch = BlockState{Block::RedstoneTorch}.withLit(true);
    REQUIRE(redstone::getSignal(litTorch, Direction::Up) == 0);
    REQUIRE(redstone::getSignal(litTorch, Direction::Down) == 15);
    REQUIRE(redstone::getSignal(litTorch, Direction::North) == 15);
    REQUIRE(redstone::getSignal(litTorch, Direction::East) == 15);
    REQUIRE(redstone::getDirectSignal(litTorch, Direction::Down) == 15);
    REQUIRE(redstone::getDirectSignal(litTorch, Direction::Up) == 0);
    REQUIRE(redstone::getDirectSignal(litTorch, Direction::North) == 0);

    // Unlit torch: nothing at all.
    const BlockState unlitTorch = BlockState{Block::RedstoneTorch}.withLit(false);
    for (const Direction dir : redstone::kAllDirections) {
        REQUIRE(redstone::getSignal(unlitTorch, dir) == 0);
        REQUIRE(redstone::getDirectSignal(unlitTorch, dir) == 0);
    }

    // Lit wall torch facing North: weak 15 every side except the way it faces.
    const BlockState wall = BlockState{Block::RedstoneWallTorch, BlockOrientation::North}.withLit(true);
    REQUIRE(redstone::getSignal(wall, Direction::North) == 0); // toward FACING
    REQUIRE(redstone::getSignal(wall, Direction::South) == 15);
    REQUIRE(redstone::getSignal(wall, Direction::Up) == 15);
    REQUIRE(redstone::getSignal(wall, Direction::Down) == 15);
}

// --- The strong-power path: a lit torch below a conductor powers the conductor,
//     which re-emits — and a torch reads that on the block it stands on. ---
void testConductorReemission() {
    using redstone::Direction;
    auto world = loadedWorld();
    // base (Stone) with a lit redstone torch directly below it, and the torch we
    // are querying standing on top of base.
    world.setState(0, 0, 0, BlockState{Block::Stone});                                  // base
    world.setState(0, -1, 0, BlockState{Block::RedstoneTorch}.withLit(true));           // source below
    world.setState(0, 1, 0, BlockState{Block::RedstoneTorch}.withLit(true));            // torch on base

    // base receives strong 15 from the torch below and, being a conductor,
    // presents it on every face.
    REQUIRE(redstone::getDirectSignalTo(world, {0, 0, 0}) == 15);
    REQUIRE(redstone::getSignal(world, {0, 0, 0}, Direction::Down) == 15);
    REQUIRE(redstone::hasSignal(world, {0, 0, 0}, Direction::Down));
    // ...so the torch standing on base reads its input as powered.
    REQUIRE(redstone::torchHasNeighborSignal(world, {0, 1, 0}));

    // Extinguish the source below: base loses power, the torch's input clears.
    world.setState(0, -1, 0, BlockState{Block::RedstoneTorch}.withLit(false));
    REQUIRE(redstone::getDirectSignalTo(world, {0, 0, 0}) == 0);
    REQUIRE(!redstone::hasSignal(world, {0, 0, 0}, Direction::Down));
    REQUIRE(!redstone::torchHasNeighborSignal(world, {0, 1, 0}));
}

// --- The weak-only contrast: a block of redstone next to a conductor does NOT
//     power it (getDirectSignal is 0), so nothing standing on it reads a signal.
//     This is the exact distinction weak vs strong exists for. ---
void testWeakOnlyDoesNotPowerConductor() {
    using redstone::Direction;
    auto world = loadedWorld();
    world.setState(0, 0, 0, BlockState{Block::Stone});          // conductor base
    world.setState(1, 0, 0, BlockState{Block::RedstoneBlock});  // weak-only source beside it

    REQUIRE(redstone::getDirectSignalTo(world, {0, 0, 0}) == 0);
    REQUIRE(!redstone::hasSignal(world, {0, 0, 0}, Direction::Down));
    REQUIRE(!redstone::torchHasNeighborSignal(world, {0, 1, 0}));
    // But the redstone block does present weak power on the shared face itself.
    REQUIRE(redstone::getBestNeighborSignal(world, {2, 0, 0}) == 15); // read from the block's east side
}

// --- getBestNeighborSignal: what a wire would read. ---
void testBestNeighborSignal() {
    auto world = loadedWorld();
    world.setState(5, 5, 5, BlockState{Block::RedstoneTorch}.withLit(true));
    // A cell beside the lit torch sees 15; a cell in open air sees 0.
    REQUIRE(redstone::getBestNeighborSignal(world, {6, 5, 5}) == 15);
    REQUIRE(redstone::getBestNeighborSignal(world, {50, 5, 50}) == 0);
    // The cell below the torch reads the torch's UP face, which is dead, so it
    // sees 0 (JE queries getSignal(neighbor, dirFromPosToNeighbor): below -> torch
    // is the UP direction).
    REQUIRE(redstone::getBestNeighborSignal(world, {5, 4, 5}) == 0);
}

} // namespace

int main() {
    testIsSignalSource();
    testPerBlockSignal();
    testConductorReemission();
    testWeakOnlyDoesNotPowerConductor();
    testBestNeighborSignal();
    return 0;
}
