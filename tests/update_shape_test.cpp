#include "gameplay/BlockBehavior.hpp"

#include "world/World.hpp"

#include <cstdio>
#include <cstdlib>
#include <optional>

// W-3 acceptance for the generalised updateShape derivation: a neighbour → new
// state function dispatched through the R1 behaviour table under the A3b
// contract. There is no fence/wire content yet, so these drive the mechanism
// with fixture derivations (the shape the real ones will take): pure property
// rewrite, idempotent convergence to a fixed point, and a HasNeighborReaction
// pre-filter that keeps the 99% of blocks that cannot react out of dispatch.

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "update_shape_test line %d failed: %s\n", line, expression);
        std::abort();
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

using namespace mc::gameplay;
using mc::world::Block;
using mc::world::BlockState;

// A converging pure-property rewrite: step Age down by one until it reaches 0,
// then leave it unchanged. Stands in for a fence dropping to its settled set of
// connections — same block throughout, reaches a fixed point.
std::optional<BlockState> shrinkAge(const NeighborUpdateContext& context) {
    if (context.state.age() > 0) {
        return context.state.withAge(context.state.age() - 1);
    }
    return context.state; // settled: unchanged -> fixed point
}

// Always reports a real change (Age 7). It never settles on its own, so a loop
// only terminates because of the fixed-point guard — and an off-bit block only
// stays out of dispatch because of the pre-filter.
std::optional<BlockState> bumpAgeToSeven(const NeighborUpdateContext& context) {
    return context.state.withAge(7);
}

// Breaks A3b: returns a different block. Must be refused.
std::optional<BlockState> changeKind(const NeighborUpdateContext&) {
    return BlockState{Block::Stone};
}

[[nodiscard]] BlockBehavior reacting(UpdateShapeFn slot) {
    BlockBehavior behavior;
    behavior.updateShape = slot;
    behavior.prefilter.set(BlockBehaviorBit::HasNeighborReaction, true);
    return behavior;
}

[[nodiscard]] NeighborUpdateContext contextFor(const mc::world::World& world, BlockState state) {
    return {world, {0, 0, 0}, state, {1, 0, 0}, BlockState{Block::Air}};
}

} // namespace

int main() {
    const mc::world::World world;

    // --- Pure property rewrite: the derivation runs and keeps the block. ---
    {
        const auto behavior = reacting(&shrinkAge);
        const auto result =
            dispatchUpdateShape(behavior, contextFor(world, BlockState{Block::WheatCrops}.withAge(3)));
        REQUIRE(result.has_value());
        REQUIRE(result->block() == Block::WheatCrops); // same block
        REQUIRE(result->age() == 2);                   // one step down
    }

    // --- A3b: a derivation that changes the block kind is refused, not applied. ---
    {
        const auto behavior = reacting(&changeKind);
        const auto result =
            dispatchUpdateShape(behavior, contextFor(world, BlockState{Block::WheatCrops}.withAge(3)));
        REQUIRE(!result.has_value());
    }

    // --- Idempotent convergence: notifying to the fixed point terminates and
    //     does not churn. The last step (Age already 0) reports no change. ---
    {
        const auto behavior = reacting(&shrinkAge);
        BlockState state = BlockState{Block::WheatCrops}.withAge(5);
        int steps = 0;
        constexpr int kSafety = 1000;
        while (true) {
            const auto result = dispatchUpdateShape(behavior, contextFor(world, state));
            if (!result.has_value()) {
                break; // reached the fixed point
            }
            state = *result;
            REQUIRE(++steps < kSafety); // catches a lost fixed-point guard
        }
        REQUIRE(steps == 5); // 5 -> 0 in exactly five writes
        REQUIRE(state.age() == 0);
    }

    // --- The fixed point directly: an already-settled cell writes nothing. ---
    {
        const auto behavior = reacting(&shrinkAge);
        REQUIRE(!dispatchUpdateShape(behavior,
                                     contextFor(world, BlockState{Block::WheatCrops}.withAge(0)))
                     .has_value());
    }

    // --- Pre-filter gate: HasNeighborReaction clear means the slot is never
    //     reached, even one that would otherwise change the state. ---
    {
        BlockBehavior offBit;
        offBit.updateShape = &bumpAgeToSeven;
        offBit.prefilter.set(BlockBehaviorBit::HasNeighborReaction, false);
        const auto skipped =
            dispatchUpdateShape(offBit, contextFor(world, BlockState{Block::WheatCrops}.withAge(0)));
        REQUIRE(!skipped.has_value()); // catches a dead pre-filter

        // The same slot with the bit set does dispatch, proving the gate — not a
        // null slot — is what suppressed it above.
        const auto onBit = reacting(&bumpAgeToSeven);
        const auto dispatched =
            dispatchUpdateShape(onBit, contextFor(world, BlockState{Block::WheatCrops}.withAge(0)));
        REQUIRE(dispatched.has_value());
        REQUIRE(dispatched->age() == 7);
    }

    // --- A block that reacts to neighbours but has no shape derivation (grass,
    //     flowers: HasNeighborReaction set from support, updateShape null) yields
    //     nothing without a crash. ---
    {
        BlockBehavior supportOnly;
        supportOnly.prefilter.set(BlockBehaviorBit::HasNeighborReaction, true);
        REQUIRE(!dispatchUpdateShape(supportOnly, contextFor(world, BlockState{Block::WheatCrops}))
                     .has_value());
    }

    return 0;
}
