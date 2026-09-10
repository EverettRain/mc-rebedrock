// AR-B4-6: the two things a diode still got wrong.
//
//   * Repeaters and comparators were declared noCollision, so you fell straight
//     through one. DiodeBlock's SHAPE is `Block.column(16, 0, 2)` and DiodeBlock
//     never overrides getCollisionShape — vanilla's 2px plate is a real box you
//     stand on. The "no collision" was ours.
//   * A comparator ignored containers entirely. ComparatorBlock#getInputSignal
//     asks the block behind it for an analog output first, which is how a chest
//     drives one.

#include "gameplay/GameSession.hpp"
#include "gameplay/PlayerController.hpp"
#include "gameplay/RedstoneSignal.hpp"
#include "world/Block.hpp"
#include "world/BlockShape.hpp"
#include "world/BlockState.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <utility>

namespace {

using mc::world::Block;
using mc::world::BlockState;
using mc::world::Chunk;
using mc::world::World;

[[nodiscard]] World floored() {
    World world;
    for (int cz = -1; cz <= 1; ++cz) {
        for (int cx = -1; cx <= 1; ++cx) {
            Chunk chunk;
            for (int z = 0; z < 16; ++z) {
                for (int x = 0; x < 16; ++x) {
                    chunk.setBlock(x, 0, z, Block::Stone);
                }
            }
            world.setChunk({cx, cz}, std::move(chunk));
        }
    }
    return world;
}

} // namespace

int main() {
    // --- The 2px plate is a real collision box. ---
    {
        for (const Block diode : {Block::Repeater, Block::Comparator}) {
            assert(mc::world::hasCollision(diode));
            const auto span = mc::world::collisionSpan(BlockState{diode});
            assert(std::fabs(span.bottom - 0.0F) < 1.0e-6F);
            assert(std::fabs(span.top - 2.0F / 16.0F) < 1.0e-6F);
        }
    }

    // --- Standing on one does not bounce. This is the pressure plate's lesson
    // being checked rather than assumed: turning the plate's collision on made
    // it oscillate, because its height moves with POWERED (1/16 <-> 0.5/16) and
    // fought the feet-cell probe. A diode's 2px never moves, so it should be
    // still — and "should be" is why there is a test. Powered halfway through,
    // since a state change is exactly what made the plate jump. ---
    {
        World world = floored();
        world.setState(8, 1, 8, BlockState{Block::Repeater});
        mc::gameplay::PlayerController player({8.5F, 1.0F + 2.0F / 16.0F, 8.5F});
        mc::gameplay::PlayerInput idle;
        float lowest = player.position().y;
        float highest = player.position().y;
        for (int tick = 0; tick < 200; ++tick) {
            if (tick == 100) {
                world.setState(8, 1, 8, BlockState{Block::Repeater}.withPowered(true));
            }
            player.tick(world, idle);
            lowest = std::min(lowest, player.position().y);
            highest = std::max(highest, player.position().y);
        }
        assert(player.onGround());
        // Resting on the plate's 2/16 top, not sunk into the floor below it.
        assert(std::fabs(player.position().y - (1.0F + 2.0F / 16.0F)) < 0.05F);
        // And it never moved: a bounce would show as a spread between the
        // extremes, which is precisely what the plate did.
        assert(highest - lowest < 0.01F);
    }

    // --- AbstractContainerMenu.getRedstoneSignalFromContainer, walked through
    // every step. 27 slots, 64-stacking, i.e. a chest. ---
    {
        using mc::gameplay::redstone::redstoneSignalFromContainer;
        constexpr int kSlots = 27;
        const auto forStacks = [&](float stacks) {
            return redstoneSignalFromContainer(stacks, kSlots);
        };
        assert(forStacks(0.0F) == 0);                  // empty
        assert(forStacks(1.0F / 64.0F) == 1);          // a single item is still 1
        assert(forStacks(1.0F) == 1);                  // one full stack
        assert(forStacks(static_cast<float>(kSlots)) == 15);        // completely full
        assert(forStacks(static_cast<float>(kSlots) / 2.0F) == 8);  // half full
        // Monotone, and never out of range, across the whole span.
        int previous = 0;
        for (int filled = 0; filled <= kSlots * 64; ++filled) {
            const int signal = forStacks(static_cast<float>(filled) / 64.0F);
            assert(signal >= 0 && signal <= 15);
            assert(signal >= previous);
            previous = signal;
        }
        // A degenerate container answers 0 rather than dividing by zero.
        assert(redstoneSignalFromContainer(0.0F, 0) == 0);
    }

    // --- comparatorInputSignal: a container *replaces* the ordinary input, and
    // "no analog output" is not the same as "an analog output of 0". ---
    {
        using mc::gameplay::redstone::comparatorInputSignal;
        World world = floored();
        const mc::world::BlockPos pos{8, 1, 8};
        const BlockState comparator{Block::Comparator, mc::world::BlockOrientation::North};
        world.setState(pos.x, pos.y, pos.z, comparator);
        // A redstone block behind it (north, its FACING/input side) drives 15.
        world.setState(pos.x, pos.y, pos.z - 1, BlockState{Block::RedstoneBlock});
        assert(comparatorInputSignal(world, pos, comparator, -1) == 15);
        // With a container there, the container wins — even an empty one, which
        // is the case a plain `max` would get wrong.
        assert(comparatorInputSignal(world, pos, comparator, 0) == 0);
        assert(comparatorInputSignal(world, pos, comparator, 7) == 7);
    }

    // --- AR-M4: the composter answers from its own STATE, with no block entity
    // and no analog-output callback behind it. `ComposterBlock#
    // getAnalogOutputSignal` returns LEVEL directly, so the signal is the fill
    // itself (0..8) and not the container formula's 0..15 rescale. ---
    {
        using mc::gameplay::redstone::comparatorInputSignal;
        using mc::gameplay::redstone::stateAnalogOutput;
        World world = floored();
        const mc::world::BlockPos pos{8, 1, 8};
        const BlockState comparator{Block::Comparator, mc::world::BlockOrientation::North};
        world.setState(pos.x, pos.y, pos.z, comparator);

        // Every level reads back as itself — exact values, because a "rises with
        // the fill" check would pass the container rescale too, and that is the
        // wrong answer for a composter (it would read 15 when full, not 8).
        for (int level = 0; level <= mc::world::kComposterReadyLevel; ++level) {
            const auto composter =
                BlockState{Block::Composter}.withComposterLevel(level);
            world.setState(pos.x, pos.y, pos.z - 1, composter);
            assert(stateAnalogOutput(composter) == level);
            assert(comparatorInputSignal(world, pos, comparator, -1) == level);
        }

        // An EMPTY composter still overrides an ordinary input, exactly as an
        // empty chest does — that is the difference between "an analog source
        // reading 0" and "not an analog source", and it is what a plain `max`
        // gets wrong.
        world.setState(pos.x, pos.y, pos.z - 1, BlockState{Block::Composter});
        assert(comparatorInputSignal(world, pos, comparator, -1) == 0);

        // A block that is not an analog source at all leaves the ordinary diode
        // input alone.
        world.setState(pos.x, pos.y, pos.z - 1, BlockState{Block::RedstoneBlock});
        assert(stateAnalogOutput(BlockState{Block::RedstoneBlock}) < 0);
        assert(comparatorInputSignal(world, pos, comparator, -1) == 15);

        // A container behind it still wins over the state-derived path: the
        // callback is checked first, so a block that is BOTH would not go
        // silently to the wrong answer.
        world.setState(pos.x, pos.y, pos.z - 1,
                       BlockState{Block::Composter}.withComposterLevel(3));
        assert(comparatorInputSignal(world, pos, comparator, 11) == 11);
    }

    return 0;
}
