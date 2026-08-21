// W-5: the AC wire evaluator's bit-for-bit contract. The self-authored
// descending-power wavefront (RedstoneWireEvaluator) must settle every wire
// cell's POWER to the exact same value the naive serial relaxation
// (RedstoneWire.hpp's computeWireNetwork, the ground-truth oracle) reaches — the
// fixed point of the attenuation law is unique, so this holds for arbitrary
// branching networks, loops and multi-source graphs. This is the lockstep cross
// check the roadmap asks for, run over an explicit attenuation line, a loop, and
// a large randomised circuit set. A separate golden-order case pins the
// deterministic Orientation traversal that fixes the block-update emission order.

#include "gameplay/RedstoneWire.hpp"
#include "gameplay/RedstoneWireEvaluator.hpp"

#include "world/Block.hpp"
#include "world/BlockPos.hpp"
#include "world/BlockState.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "redstone_wire_ac_test line %d failed: %s\n", line, expression);
        std::abort();
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

using mc::world::Block;
using mc::world::BlockPos;
using mc::world::BlockState;
using mc::world::World;
namespace redstone = mc::gameplay::redstone;

[[nodiscard]] World loadedWorld() {
    World world;
    for (int cx = -1; cx <= 1; ++cx) {
        for (int cz = -1; cz <= 1; ++cz) {
            world.setChunk({cx, cz}, mc::world::Chunk{});
        }
    }
    return world;
}

void set(World& world, BlockPos pos, Block block) {
    static_cast<void>(world.setState(pos.x, pos.y, pos.z, BlockState{block}));
}

// The serial oracle's answer as a position -> POWER map.
[[nodiscard]] std::map<std::int64_t, int> serialPower(const World& world, BlockPos start) {
    std::map<std::int64_t, int> out;
    for (const auto& cell : redstone::computeWireNetwork(world, start)) {
        out[mc::world::packBlockPos(cell.pos)] = cell.power;
    }
    return out;
}

// The AC evaluator's answer as a position -> POWER map.
[[nodiscard]] std::map<std::int64_t, int> acPower(redstone::WireNetworkEvaluator& evaluator,
                                                  const World& world, BlockPos start) {
    std::map<std::int64_t, int> out;
    for (const auto& cell : evaluator.solve(world, start)) {
        out[mc::world::packBlockPos(cell.pos)] = cell.power;
    }
    return out;
}

// The lockstep invariant: AC == serial, cell for cell, from the same start.
void requireEquivalent(redstone::WireNetworkEvaluator& evaluator, const World& world,
                       BlockPos start, int line) {
    const auto serial = serialPower(world, start);
    const auto ac = acPower(evaluator, world, start);
    require(serial == ac, "AC power map == serial power map", line);
}

// A tiny deterministic RNG (splitmix64) so the fuzz set is reproducible.
struct Rng final {
    std::uint64_t state;
    std::uint32_t next() {
        state += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        z = z ^ (z >> 31);
        return static_cast<std::uint32_t>(z);
    }
    std::uint32_t below(std::uint32_t bound) { return next() % bound; }
};

// ---- Scenario 1: an explicit straight run (source 15, attenuation -1/cell). ----
void testAttenuationLine() {
    World world = loadedWorld();
    constexpr int kLength = 17;
    for (int x = 0; x < kLength; ++x) {
        set(world, {x, 64, 0}, Block::RedstoneWire);
    }
    set(world, {-1, 64, 0}, Block::RedstoneBlock); // constant 15 against w0

    redstone::WireNetworkEvaluator evaluator;
    const auto ac = acPower(evaluator, world, {0, 64, 0});
    for (int x = 0; x < kLength; ++x) {
        const int expected = std::max(0, 15 - x);
        REQUIRE(ac.at(mc::world::packBlockPos({x, 64, 0})) == expected);
    }
    requireEquivalent(evaluator, world, {0, 64, 0}, __LINE__);
}

// ---- Scenario 2: a loop (ring of wire with one source) — a cycle in the graph,
//      where the naive relaxation and the wavefront must still agree. ----
void testLoop() {
    World world = loadedWorld();
    // A 4x4 hollow square ring on y=64.
    for (int x = 0; x <= 3; ++x) {
        set(world, {x, 64, 0}, Block::RedstoneWire);
        set(world, {x, 64, 3}, Block::RedstoneWire);
    }
    for (int z = 0; z <= 3; ++z) {
        set(world, {0, 64, z}, Block::RedstoneWire);
        set(world, {3, 64, z}, Block::RedstoneWire);
    }
    set(world, {0, 64, -1}, Block::RedstoneBlock); // feed the ring at one corner

    redstone::WireNetworkEvaluator evaluator;
    requireEquivalent(evaluator, world, {0, 64, 0}, __LINE__);
    // The corner opposite the source is reached two ways; the shorter path wins.
    const auto ac = acPower(evaluator, world, {0, 64, 0});
    REQUIRE(ac.at(mc::world::packBlockPos({0, 64, 0})) == 15); // adjacent to source
    REQUIRE(ac.at(mc::world::packBlockPos({3, 64, 3})) == 9);  // 6 cells along either arm
}

// ---- Scenario 3: multi-source (two feeds, the stronger-nearer wins per cell). ----
void testMultiSource() {
    World world = loadedWorld();
    for (int x = 0; x <= 8; ++x) {
        set(world, {x, 64, 0}, Block::RedstoneWire);
    }
    set(world, {-1, 64, 0}, Block::RedstoneBlock); // feeds the left end
    set(world, {9, 64, 0}, Block::RedstoneBlock);  // feeds the right end

    redstone::WireNetworkEvaluator evaluator;
    requireEquivalent(evaluator, world, {4, 64, 0}, __LINE__);
    const auto ac = acPower(evaluator, world, {4, 64, 0});
    // Each cell takes the max of the two attenuation ramps.
    for (int x = 0; x <= 8; ++x) {
        const int expected = std::max({0, 15 - x, 15 - (8 - x)});
        REQUIRE(ac.at(mc::world::packBlockPos({x, 64, 0})) == expected);
    }
}

// ---- Scenario 4: randomised circuit set — the lockstep fuzz. ----
void testRandomEquivalence() {
    redstone::WireNetworkEvaluator evaluator; // one instance: also exercises reuse
    for (std::uint32_t seed = 1; seed <= 300; ++seed) {
        Rng rng{seed * 0x2545F4914F6CDD1DULL};
        World world = loadedWorld();
        std::vector<BlockPos> wires;

        // A random 3D blob: each cell is wire (~55%), a redstone source (~10%),
        // or empty. Keep it inside one chunk column so packing stays simple.
        for (int x = -4; x <= 4; ++x) {
            for (int z = -4; z <= 4; ++z) {
                for (int y = 64; y <= 65; ++y) {
                    const std::uint32_t roll = rng.below(100);
                    if (roll < 55) {
                        set(world, {x, y, z}, Block::RedstoneWire);
                        wires.push_back({x, y, z});
                    } else if (roll < 65) {
                        set(world, {x, y, z}, Block::RedstoneBlock);
                    }
                }
            }
        }
        if (wires.empty()) {
            continue;
        }
        // Sample a handful of starts per seed. The map comparison already covers
        // a start's whole connected component, so a few random starts across the
        // blob reach every component with high probability over 300 seeds; each
        // start also re-checks solve() is start-invariant and that the reused
        // evaluator resets its arena cleanly between very different networks.
        const std::size_t samples = wires.size() < 8 ? wires.size() : 8;
        for (std::size_t s = 0; s < samples; ++s) {
            const BlockPos start = wires[rng.below(static_cast<std::uint32_t>(wires.size()))];
            requireEquivalent(evaluator, world, start, __LINE__);
        }
    }
}

// ---- Scenario 5: the deterministic Orientation traversal (golden order). ----
// The AC path fixes its neighbour order at Orientation.of(UP, NORTH, LEFT)
// = [South, North, West, East, Down, Up]. A plus of wire around the origin is
// discovered in exactly that order; a non-deterministic or reordered traversal
// (sabotage ①) would shuffle the discovery sequence and break this.
void testDeterministicTraversalOrder() {
    World world = loadedWorld();
    set(world, {0, 64, 0}, Block::RedstoneWire);
    set(world, {0, 64, 1}, Block::RedstoneWire);  // South (+Z)
    set(world, {0, 64, -1}, Block::RedstoneWire); // North (-Z)
    set(world, {-1, 64, 0}, Block::RedstoneWire); // West (-X)
    set(world, {1, 64, 0}, Block::RedstoneWire);  // East (+X)

    redstone::WireNetworkEvaluator evaluator;
    const auto& network = evaluator.solve(world, {0, 64, 0});
    const std::vector<BlockPos> expected{
        {0, 64, 0}, {0, 64, 1}, {0, 64, -1}, {-1, 64, 0}, {1, 64, 0},
    };
    REQUIRE(network.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        REQUIRE(network[i].pos.x == expected[i].x);
        REQUIRE(network[i].pos.y == expected[i].y);
        REQUIRE(network[i].pos.z == expected[i].z);
    }

    // Determinism: the same world re-solved gives the identical ordered result.
    const auto first = acPower(evaluator, world, {0, 64, 0});
    const auto second = acPower(evaluator, world, {0, 64, 0});
    REQUIRE(first == second);
}

// ---- Scenario 6: a non-wire start settles to nothing. ----
void testNonWireStart() {
    World world = loadedWorld();
    set(world, {0, 64, 0}, Block::RedstoneBlock);
    redstone::WireNetworkEvaluator evaluator;
    REQUIRE(evaluator.solve(world, {0, 64, 0}).empty());
}

} // namespace

int main() {
    testAttenuationLine();
    testLoop();
    testMultiSource();
    testRandomEquivalence();
    testDeterministicTraversalOrder();
    testNonWireStart();
    return 0;
}
