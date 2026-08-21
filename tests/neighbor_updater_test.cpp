#include "world/BlockPos.hpp"
#include "world/MutationFlags.hpp"
#include "world/NeighborUpdater.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <vector>

// W-1 acceptance: MutationFlags line up bit-for-bit with Java's
// Block.UpdateFlags, the neighbour updater fans out in Java's fixed order,
// drains iteratively (never by deep recursion), caps a self-feeding chain at
// the depth limit, and keys its records on the JE BlockPos.asLong() encoding.

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "neighbor_updater_test line %d failed: %s\n", line, expression);
        std::abort();
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

using namespace mc::world;

[[nodiscard]] std::uint16_t bits(MutationFlags flag) {
    return static_cast<std::uint16_t>(flag);
}

// --- flags line up bit-for-bit with Block.UpdateFlags. ---
void testFlagBits() {
    REQUIRE(bits(MutationFlags::NotifyNeighbors) == 1U);
    REQUIRE(bits(MutationFlags::NotifyClients) == 2U);
    REQUIRE(bits(MutationFlags::Invisible) == 4U);
    REQUIRE(bits(MutationFlags::Immediate) == 8U);
    REQUIRE(bits(MutationFlags::KnownShape) == 16U);
    REQUIRE(bits(MutationFlags::SuppressDrops) == 32U);
    REQUIRE(bits(MutationFlags::MovedByPiston) == 64U);
    REQUIRE(bits(MutationFlags::SkipShapeUpdateOnWire) == 128U);
    REQUIRE(bits(MutationFlags::SkipBlockEntity) == 256U);
    REQUIRE(bits(MutationFlags::SkipOnPlace) == 512U);

    // Composite presets, checked against Java's own constants.
    REQUIRE(bits(MutationFlags::All) == 3U);              // UPDATE_ALL
    REQUIRE(bits(MutationFlags::SkipAllSideEffects) == 816U); // UPDATE_SKIP_ALL_SIDEEFFECTS
    // Generation is rebedrock's own worldgen preset: known shape, no placement
    // callback, no block entity.
    REQUIRE(bits(MutationFlags::Generation) ==
            (16U | 512U | 256U)); // KnownShape | SkipOnPlace | SkipBlockEntity

    REQUIRE(kDefaultUpdateLimit == 512); // UPDATE_LIMIT
}

// --- packed coordinate is the JE BlockPos.asLong() encoding. ---
void testPacking() {
    // The field layout: X in bits [38,63], Z in bits [12,37], Y in bits [0,11].
    REQUIRE(kPackedXOffset == 38);
    REQUIRE(kPackedZOffset == 12);
    REQUIRE(kPackedYOffset == 0);

    // A hand-computed sample: x=1 lands at bit 38, y=2 at bit 0, z=3 at bit 12.
    const std::int64_t packed = packBlockPos(1, 2, 3);
    REQUIRE(packed == ((std::int64_t{1} << 38) | (std::int64_t{3} << 12) | std::int64_t{2}));

    // Round-trips, including the sign extension negatives rely on.
    const BlockPos samples[] = {
        {0, 0, 0},       {1, 2, 3},      {-1, -1, -1},   {30000000, 2000, 30000000},
        {-30000000, -2048, -30000000}, {123456, -5, -987654},
    };
    for (const BlockPos& pos : samples) {
        const std::int64_t p = packBlockPos(pos);
        REQUIRE(unpackBlockPosX(p) == pos.x);
        REQUIRE(unpackBlockPosY(p) == pos.y);
        REQUIRE(unpackBlockPosZ(p) == pos.z);
        REQUIRE(unpackBlockPos(p) == pos);
    }
}

// --- the six neighbours are notified in Java's UPDATE_ORDER, and the source is
// carried through unchanged. ---
void testFixedOrder() {
    NeighborUpdater updater;
    const BlockPos source{10, 20, 30};
    std::vector<BlockPos> seen;
    updater.updateNeighborsAt(source, kDefaultUpdateLimit, [&](BlockPos neighbor, BlockPos from) {
        REQUIRE(from == source);
        seen.push_back(neighbor);
    });

    // Exactly WEST, EAST, DOWN, UP, NORTH, SOUTH of the source, in that order.
    const std::vector<BlockPos> expected{
        {9, 20, 30},  {11, 20, 30}, {10, 19, 30},
        {10, 21, 30}, {10, 20, 29}, {10, 20, 31},
    };
    REQUIRE(seen == expected);
    REQUIRE(!updater.draining()); // fully settled once the top-level call returns
}

// --- a re-entrant reaction has its own fan-out collected onto the running
// drain and processed depth-first, in source order, not by recursion. ---
//
// The updater binds one notify for the whole drain (as Java binds the Level):
// a reaction that re-enters updateNeighborsAt contributes its fan-out but the
// running drain's notify is what delivers it. A single std::function models
// that faithfully — the same object drives every notification.
void testReentrantOrder() {
    NeighborUpdater updater;
    std::vector<BlockPos> seen;
    const BlockPos root{0, 0, 0};
    // The WEST neighbour of the root, when visited, itself changes and notifies
    // its own neighbours. Java runs that whole sub-fan-out before the root's
    // remaining five directions.
    const BlockPos westChild{-1, 0, 0};
    bool spawned = false;
    std::function<void(BlockPos, BlockPos)> notify = [&](BlockPos neighbor, BlockPos) {
        seen.push_back(neighbor);
        if (neighbor == westChild && !spawned) {
            spawned = true;
            updater.updateNeighborsAt(westChild, kDefaultUpdateLimit, notify);
        }
    };
    updater.updateNeighborsAt(root, kDefaultUpdateLimit, notify);

    // root WEST is visited, then all six of westChild's neighbours, then the
    // root's EAST/DOWN/UP/NORTH/SOUTH.
    const std::vector<BlockPos> expected{
        {-1, 0, 0},                                             // root WEST (spawns)
        {-2, 0, 0}, {0, 0, 0}, {-1, -1, 0},                     // westChild WEST/EAST/DOWN
        {-1, 1, 0}, {-1, 0, -1}, {-1, 0, 1},                    // westChild UP/NORTH/SOUTH
        {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, // rest of root
    };
    REQUIRE(seen == expected);
}

// --- a long self-feeding chain drains without growing the C++ call stack: the
// updater must be iterative. A recursive drain would overflow here. ---
void testIterativeDeepChain() {
    NeighborUpdater updater;
    // Every notification, while there is budget, changes a fresh cell and
    // notifies again — a chain far deeper than the call stack could hold. With
    // updateLimit < 0 the depth cap is off, so only the budget stops it: this
    // isolates stack-safety from the limit test below.
    long budget = 200000;
    long notifications = 0;
    int child = 1;
    std::function<void(BlockPos, BlockPos)> notify = [&](BlockPos, BlockPos) {
        ++notifications;
        if (budget > 0) {
            --budget;
            updater.updateNeighborsAt({child++, 0, 0}, -1, notify);
        }
    };
    updater.updateNeighborsAt({0, 0, 0}, -1, notify);

    REQUIRE(notifications > 0);
    REQUIRE(budget == 0);         // the chain ran to the full depth
    REQUIRE(!updater.draining()); // and left the updater settled
}

// --- a genuinely unbounded topology (every reaction spawns another, forever)
// terminates because the depth limit caps the total chained updates. Without
// the cap this spins; the safety counter turns that into a clean abort. ---
void testDepthLimitConverges() {
    NeighborUpdater updater;
    constexpr int kLimit = 512;
    long notifications = 0;
    int child = 1;
    // Hard ceiling well above the bound a correct cap produces (~6 * limit), so
    // a correct run never trips it but a missing cap does.
    constexpr long kSafety = 50000;
    std::function<void(BlockPos, BlockPos)> notify = [&](BlockPos, BlockPos) {
        ++notifications;
        REQUIRE(notifications < kSafety); // catches a removed depth limit
        updater.updateNeighborsAt({child++, 0, 0}, kLimit, notify);
    };
    updater.updateNeighborsAt({0, 0, 0}, kLimit, notify);

    REQUIRE(notifications > 0);
    // At most six notifications per enqueued fan-out, and no more than `kLimit`
    // fan-outs are ever enqueued.
    REQUIRE(notifications <= 6L * kLimit);
    REQUIRE(!updater.draining());
}

} // namespace

int main() {
    testFlagBits();
    testPacking();
    testFixedOrder();
    testReentrantOrder();
    testIterativeDeepChain();
    testDepthLimitConverges();
    return 0;
}
