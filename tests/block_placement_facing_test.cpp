// AR-CX8: which way a HorizontalDirectionalBlock turns when it is placed.
//
// The field report: every stair came out 180 degrees from vanilla. Look north,
// place a stair, and its FACING read `south`. The cause was not the stair —
// `placementOrientation` had ONE rule for every block that declares a horizontal
// FACING,
//
//     return oppositeOrientation(horizontalFacing(context.lookDirection));
//
// justified by a comment about `HorizontalDirectionalBlock`. But 26.1's
// `HorizontalDirectionalBlock` declares only the PROPERTY: it has no
// `getStateForPlacement` at all, so every subclass writes its own and they do
// not agree. Three families come out of that file:
//
//   `.getOpposite()`  AbstractFurnaceBlock:51, ChestBlock:218, DiodeBlock:155
//                     (repeater + comparator), TrapDoorBlock:161's vertical arm
//   bare direction    StairBlock:103, DoorBlock:151, FenceGateBlock:135
//   `.getClockWise()` AnvilBlock:54
//
// So this file does two different things, and both are needed. The first is the
// four-orientation assertion for each family, driven through the real
// `placementBlock` so it covers the path a player's click takes. The second is
// the one that stops the next block from being wrong: EVERY block in the roster
// that declares a horizontal FACING is checked against the rule its vanilla
// source takes, from a list written here. A new such block joins the list or
// this test fails — which is the difference between fixing the stair and fixing
// the class of defect the stair belonged to.

#include "gameplay/ItemPlacement.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/ItemUse.hpp"
#include "world/Block.hpp"
#include "world/BlockPlacement.hpp"
#include "world/BlockState.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using mc::world::Block;
using mc::world::BlockOrientation;
using mc::world::BlockState;
using mc::world::Chunk;
using mc::world::HorizontalPlacement;
using mc::world::PlacementContext;
using mc::world::World;

[[nodiscard]] World floored() {
    World world;
    Chunk chunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            chunk.setBlock(x, 0, z, Block::Stone);
        }
    }
    world.setChunk({0, 0}, std::move(chunk));
    return world;
}

// The four cardinal view vectors, paired with the direction they name. `+Z` is
// south and `-Z` north, matching BlockOrientation and `orientationOffset`.
struct Look final {
    const char* name;
    glm::vec3 direction;
    BlockOrientation looking;
};

inline constexpr std::array<Look, 4> kLooks{{
    {"north", {0.0F, 0.0F, -1.0F}, BlockOrientation::North},
    {"east", {1.0F, 0.0F, 0.0F}, BlockOrientation::East},
    {"south", {0.0F, 0.0F, 1.0F}, BlockOrientation::South},
    {"west", {-1.0F, 0.0F, 0.0F}, BlockOrientation::West},
}};

// A click on the top face of the floor block below (8,1,8), looking `direction`.
[[nodiscard]] PlacementContext clickOnFloor(glm::vec3 direction) {
    PlacementContext context;
    context.clickedBlock = {8, 0, 8};
    context.placePosition = {8, 1, 8};
    context.clickedFace = BlockOrientation::Up;
    context.hitPosition = {8.5F, 1.0F, 8.5F};
    context.lookDirection = direction;
    return context;
}

[[nodiscard]] const char* orientationName(BlockOrientation orientation) {
    switch (orientation) {
    case BlockOrientation::North: return "north";
    case BlockOrientation::East: return "east";
    case BlockOrientation::South: return "south";
    case BlockOrientation::West: return "west";
    case BlockOrientation::Up: return "up";
    case BlockOrientation::Down: return "down";
    }
    return "?";
}

[[nodiscard]] BlockOrientation expectedFacing(HorizontalPlacement rule,
                                              BlockOrientation looking) {
    switch (rule) {
    case HorizontalPlacement::TowardPlayer:
        return mc::world::oppositeOrientation(looking);
    case HorizontalPlacement::AwayFromPlayer:
        return looking;
    case HorizontalPlacement::Clockwise:
        return mc::world::clockwiseOrientation(looking);
    }
    return looking;
}

// Places `block` on the floor from each of the four cardinal views and checks
// the stored FACING against `rule`. Goes through `placementBlock`, not through
// `placementOrientation` directly: the stair, the fence gate and the plain
// fall-through take three different arms of that function, and the defect was
// only visible from the arm the stair takes.
void assertPlacementRule(Block block, HorizontalPlacement rule) {
    for (const Look& look : kLooks) {
        World world = floored();
        const auto placed = mc::world::placementBlock(world, block, clickOnFloor(look.direction));
        assert(placed.has_value() && "the block must be placeable on stone");
        const BlockOrientation got = placed->orientation();
        const BlockOrientation want = expectedFacing(rule, look.looking);
        if (got != want) {
            std::cerr << mc::world::blockDefinition(block).identifier.toString()
                      << ": looking " << look.name << " placed facing=" << orientationName(got)
                      << ", vanilla places facing=" << orientationName(want) << "\n";
        }
        assert(got == want);
    }
}

} // namespace

int main() {
    // --- The report, verbatim: look north, place a stair, read its FACING. ---
    //
    // Before the fix this block alone is the whole reproduction: it asserted
    // `south`. All four are listed rather than looped, because "all four are
    // wrong by 180" and "one is wrong" are different defects and the file should
    // say which one this was.
    {
        World world = floored();
        const auto north =
            mc::world::placementBlock(world, Block::OakStairs, clickOnFloor({0.0F, 0.0F, -1.0F}));
        assert(north.has_value());
        assert(north->orientation() == BlockOrientation::North);
        const auto south =
            mc::world::placementBlock(world, Block::OakStairs, clickOnFloor({0.0F, 0.0F, 1.0F}));
        assert(south.has_value());
        assert(south->orientation() == BlockOrientation::South);
        const auto east =
            mc::world::placementBlock(world, Block::OakStairs, clickOnFloor({1.0F, 0.0F, 0.0F}));
        assert(east.has_value());
        assert(east->orientation() == BlockOrientation::East);
        const auto west =
            mc::world::placementBlock(world, Block::OakStairs, clickOnFloor({-1.0F, 0.0F, 0.0F}));
        assert(west.has_value());
        assert(west->orientation() == BlockOrientation::West);
    }

    // --- One representative of each family, end to end. ---
    assertPlacementRule(Block::OakStairs, HorizontalPlacement::AwayFromPlayer);
    assertPlacementRule(Block::OakFenceGate, HorizontalPlacement::AwayFromPlayer);
    assertPlacementRule(Block::Furnace, HorizontalPlacement::TowardPlayer);
    assertPlacementRule(Block::Chest, HorizontalPlacement::TowardPlayer);
    assertPlacementRule(Block::TrappedChest, HorizontalPlacement::TowardPlayer);
    assertPlacementRule(Block::Repeater, HorizontalPlacement::TowardPlayer);
    assertPlacementRule(Block::Comparator, HorizontalPlacement::TowardPlayer);
    assertPlacementRule(Block::Anvil, HorizontalPlacement::Clockwise);
    assertPlacementRule(Block::ChippedAnvil, HorizontalPlacement::Clockwise);
    assertPlacementRule(Block::DamagedAnvil, HorizontalPlacement::Clockwise);

    // A trapdoor placed on a floor takes TrapDoorBlock:161's vertical-face arm,
    // which is the one that DOES take `.getOpposite()`. It reaches that arm
    // inside `placementBlock` rather than through the shared rule, so this is
    // the assertion that keeps the two answers equal — the arm and the
    // declaration must not drift apart.
    assertPlacementRule(Block::OakTrapdoor, HorizontalPlacement::TowardPlayer);

    // The door does not go through `placementBlock` at all — it is a two-cell
    // write, so `gameplay::doorPlaceResult` owns its state. That path used to
    // spell the answer out for itself (`horizontalFacing(...)`, which happened
    // to be right); it now asks the shared rule. Asserted through the real
    // item-use entry, because asking `placementOrientation` here instead would
    // test the rule twice and the door's actual placement not at all — which is
    // exactly what the first version of this file did, and a sabotage that put
    // an `oppositeOrientation` back into doorPlaceResult walked straight past it.
    for (const Look& look : kLooks) {
        World world = floored();
        const mc::gameplay::ItemStack stack{Block::OakDoor, 1U, nullptr};
        const auto result =
            mc::gameplay::legacyBlockStackUseOn(stack, world, clickOnFloor(look.direction));
        assert(result.action == mc::gameplay::ItemUseAction::PlaceDoor);
        if (result.state.orientation() != look.looking) {
            std::cerr << "oak_door: looking " << look.name << " placed facing="
                      << orientationName(result.state.orientation())
                      << ", DoorBlock.java:151 places facing=" << look.name << "\n";
        }
        assert(result.state.orientation() == look.looking);
    }

    // --- The whole table. Every horizontal-FACING block in the roster. ---
    //
    // This is the half that stops the next one from being wrong. Membership is
    // a list someone wrote, keyed on the vanilla class each block maps to:
    // the stair/door/gate family by model (they all come from the same three
    // files), the three anvils by name, and everything else defaulting to the
    // majority `.getOpposite()`. A block that declares a horizontal FACING and
    // is not covered by one of those makes this fail.
    {
        static constexpr std::array kTowardPlayerBlocks{
            // AbstractFurnaceBlock.java:51
            Block::Furnace,
            // ChestBlock.java:218 (TrappedChestBlock inherits it)
            Block::Chest,
            Block::TrappedChest,
            // DiodeBlock.java:155 (RepeaterBlock/ComparatorBlock inherit it)
            Block::Repeater,
            Block::Comparator,
            // WallTorchBlock has no getStateForPlacement: the FACING is the
            // clicked wall, chosen by StandingAndWallBlockItem, so these two
            // never reach the rule at all. Listed because they DO declare the
            // property, and an unlisted declarer is what this assertion hunts.
            Block::WallTorch,
            Block::RedstoneWallTorch,
        };
        static constexpr std::array kClockwiseBlocks{
            // AnvilBlock.java:54
            Block::Anvil,
            Block::ChippedAnvil,
            Block::DamagedAnvil,
        };

        std::size_t declarers = 0;
        std::size_t away = 0;
        std::vector<std::string> unexplained;
        for (std::size_t index = 0; index < static_cast<std::size_t>(Block::Count); ++index) {
            const auto block = static_cast<Block>(index);
            if (!mc::world::hasHorizontalFacing(block)) {
                continue;
            }
            ++declarers;
            const auto rule = mc::world::horizontalPlacementOf(block);
            const auto model = mc::world::blockDefinition(block).model;
            // StairBlock:103 / DoorBlock:151 / FenceGateBlock:135 — one rule for
            // the whole family, so it is keyed on the model rather than on
            // eighty-odd wood variants.
            const bool walkThrough = model == mc::world::BlockModel::Stairs ||
                                     model == mc::world::BlockModel::Door ||
                                     model == mc::world::BlockModel::FenceGate;
            if (walkThrough) {
                ++away;
                assert(rule == HorizontalPlacement::AwayFromPlayer);
                continue;
            }
            if (std::find(kClockwiseBlocks.begin(), kClockwiseBlocks.end(), block) !=
                kClockwiseBlocks.end()) {
                assert(rule == HorizontalPlacement::Clockwise);
                continue;
            }
            // Everything left must be either a trapdoor (TrapDoorBlock:161) or
            // one of the named `.getOpposite()` blocks. Anything else is a block
            // nobody has checked against vanilla.
            const bool named = std::find(kTowardPlayerBlocks.begin(), kTowardPlayerBlocks.end(),
                                         block) != kTowardPlayerBlocks.end();
            if (!named && model != mc::world::BlockModel::TrapDoor) {
                unexplained.emplace_back(
                    mc::world::blockDefinition(block).identifier.toString());
                continue;
            }
            assert(rule == HorizontalPlacement::TowardPlayer);
        }
        if (!unexplained.empty()) {
            std::cerr << "blocks declaring a horizontal FACING with no checked placement rule:\n";
            for (const std::string& name : unexplained) {
                std::cerr << "  " << name
                          << " — read its vanilla getStateForPlacement and list it here\n";
            }
        }
        assert(unexplained.empty());
        // Guards on the guard: an emptied roster, or a family that quietly lost
        // its members, would let the loop above pass by doing nothing.
        assert(declarers > 40);
        assert(away > 30);
        std::cout << "horizontal-facing blocks checked: " << declarers << " (" << away
                  << " walk-through, " << kClockwiseBlocks.size() << " clockwise)\n";
    }

    // --- The rule table is a bijection on the four horizontals. ---
    //
    // Each of the three turns must permute the compass, never collapse two views
    // onto one facing: a rule that answered `north` for two different views would
    // pass every assertion above that happens to look north.
    for (const HorizontalPlacement rule :
         {HorizontalPlacement::TowardPlayer, HorizontalPlacement::AwayFromPlayer,
          HorizontalPlacement::Clockwise}) {
        std::vector<BlockOrientation> produced;
        for (const Look& look : kLooks) {
            produced.push_back(expectedFacing(rule, look.looking));
        }
        std::sort(produced.begin(), produced.end());
        assert(std::adjacent_find(produced.begin(), produced.end()) == produced.end());
    }
    // And they are three DIFFERENT turns. Clockwise applied twice is opposite,
    // so a `Clockwise` that had been written as `counterClockwise` would still
    // permute — it would simply put every anvil the wrong way round.
    assert(expectedFacing(HorizontalPlacement::Clockwise, BlockOrientation::North) ==
           BlockOrientation::East);
    assert(expectedFacing(HorizontalPlacement::TowardPlayer, BlockOrientation::North) ==
           BlockOrientation::South);
    assert(expectedFacing(HorizontalPlacement::AwayFromPlayer, BlockOrientation::North) ==
           BlockOrientation::North);

    std::cout << "block placement facing: ok\n";
    return 0;
}
