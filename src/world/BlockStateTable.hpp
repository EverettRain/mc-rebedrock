#pragma once

#include "world/Block.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace mc::world {

// The interned block-state table: every state every block can be in, numbered
// once, so a cell is a single id rather than three parallel bytes.
//
// Vanilla builds this in StateDefinition as the cartesian product of a block's
// properties and interns the results; the same shape is built here at compile
// time, one contiguous id range per block.
//
// What this fixes, in order of how much it matters:
//
//   1. The state ceiling. A cell's state used to be a 3-bit slot beside the
//      block, which holds eight values. Stairs need facing(4) x half(2) x
//      shape(5) x waterlogged(2) = 80, doors need 32, fences 16 — none of them
//      fit. The escape hatch the project had already reached for is to spend a
//      Block enum member per state instead, which is what the four wall torches
//      and the lit furnace are; that trades a 3-bit ceiling for an 8-bit one
//      (86 of 256 members spent so far) and breaks block identity on the way,
//      see (2). A per-block id range has neither ceiling.
//   2. Block identity separate from state. `Block::LitFurnace` is a burning
//      furnace's *state*, not a different block, but nothing in the type system
//      says so — so a WorldMutationService comparing old and new blocks would
//      destroy and rebuild the furnace's block entity on every burn swap,
//      losing the smelt. Once `lit` is a value in the furnace's own id range,
//      BlockState::isSameBlock answers correctly with no special case.
//   3. Storage. Three arrays per section (blocks, orientations, fluid levels =
//      12 KB) collapse into one u16 array (8 KB), and the mesher reads one
//      array in order instead of three in step.
//
// Only (1) and (2) are delivered by this header; the section collapse is a
// separate step, because it moves the save format with it.
//
// `slot` below is the existing per-cell state byte, unchanged in meaning: a
// facing for furnaces and chests, an axis for logs, an age 0-7 for crops, a
// moisture 0-7 for farmland, a persistence flag for leaves. Giving each block
// only the range it uses is what makes the table small; naming the properties
// inside that range is a readability step that belongs with BlockBehavior.

// How many values of the state slot a block can be in.
[[nodiscard]] constexpr std::uint16_t blockStateSlotCount(const BlockDefinition& definition) {
    if (isCrop(definition.block) || definition.block == Block::Farmland) {
        return 8U;  // CropBlock.AGE / FarmlandBlock.MOISTURE
    }
    if (definition.leaves) {
        return 2U;  // LeavesBlock.PERSISTENT
    }
    if (definition.pillar) {
        return 6U;  // RotatedPillarBlock.AXIS, stored as a facing
    }
    if (definition.horizontalFacing) {
        return 4U;  // HorizontalDirectionalBlock.FACING
    }
    return 1U;
}

// How many fluid levels a block can carry. Only the fluids themselves do;
// waterlogging would widen this to every block that admits it.
[[nodiscard]] constexpr std::uint16_t blockFluidLevelCount(const BlockDefinition& definition) {
    return isFluid(definition.block) ? 9U : 1U;
}

// AbstractFurnaceBlock.LIT: two states, or one for everything that never burns.
[[nodiscard]] constexpr std::uint16_t blockLitCount(const BlockDefinition& definition) {
    return definition.lit ? 2U : 1U;
}

// A state is one point in the product of the axes a block declares. Three axes
// is where this stops being worth hand-rolling: a general property list (the
// shape vanilla's StateDefinition has) is the natural home for the fourth, and
// belongs with the block behaviour work.
[[nodiscard]] constexpr std::uint16_t blockStateCount(const BlockDefinition& definition) {
    return static_cast<std::uint16_t>(blockStateSlotCount(definition) *
                                      blockFluidLevelCount(definition) *
                                      blockLitCount(definition));
}

inline constexpr std::size_t kBlockKindCount = static_cast<std::size_t>(Block::Count);

// The first id of each block's range, plus a trailing total.
[[nodiscard]] constexpr std::array<std::uint16_t, kBlockKindCount + 1U> buildStateRangeStarts() {
    std::array<std::uint16_t, kBlockKindCount + 1U> starts{};
    std::uint32_t next = 0U;
    for (std::size_t index = 0; index < kBlockKindCount; ++index) {
        starts[index] = static_cast<std::uint16_t>(next);
        next += blockStateCount(kBlockRegistry[index]);
    }
    starts[kBlockKindCount] = static_cast<std::uint16_t>(next);
    return starts;
}

inline constexpr auto kBlockStateRangeStarts = buildStateRangeStarts();
inline constexpr std::uint32_t kBlockStateCount = kBlockStateRangeStarts[kBlockKindCount];

// The whole point of interning is that ids stay inside a u16. If a future
// content drop ever pushes past this, the id type widens — it does not silently
// wrap.
static_assert(kBlockStateCount <= 65536U,
              "the interned block-state table must fit in a std::uint16_t id");

// Resolves a block plus its slot and fluid level to the interned id. Values
// outside the block's declared range fall back to that block's default state,
// the same way ChunkSection::setBlock already resets the slot when a cell's
// block changes — an invalid combination has never been representable, and now
// it is not numberable either.
[[nodiscard]] constexpr std::uint16_t blockStateId(Block block, BlockOrientation orientation,
                                                   std::uint8_t fluidLevel, bool lit = false) {
    const auto kind = isValidBlock(block) ? static_cast<std::size_t>(block) : 0U;
    const auto& definition = kBlockRegistry[kind];
    const std::uint16_t slotCount = blockStateSlotCount(definition);
    const std::uint16_t fluidCount = blockFluidLevelCount(definition);
    const std::uint16_t litCount = blockLitCount(definition);
    std::uint16_t slot = static_cast<std::uint16_t>(orientation);
    if (slot >= slotCount) slot = 0U;
    std::uint16_t fluid = fluidLevel;
    if (fluid >= fluidCount) fluid = 0U;
    const std::uint16_t litIndex = (lit && litCount > 1U) ? 1U : 0U;
    return static_cast<std::uint16_t>(kBlockStateRangeStarts[kind] +
                                      (slot * fluidCount + fluid) * litCount + litIndex);
}

// The inverse: which block owns an id. A binary search over the range starts
// would also work; the linear walk is constexpr-friendly and this is only
// reached from the accessors below, which the compiler folds at every constant
// call site.
[[nodiscard]] constexpr std::size_t blockKindOfState(std::uint16_t id) {
    std::size_t low = 0U;
    std::size_t high = kBlockKindCount - 1U;
    while (low < high) {
        const std::size_t middle = (low + high + 1U) / 2U;
        if (kBlockStateRangeStarts[middle] <= id) {
            low = middle;
        } else {
            high = middle - 1U;
        }
    }
    return low;
}

[[nodiscard]] constexpr Block blockOfState(std::uint16_t id) {
    return static_cast<Block>(blockKindOfState(id));
}

[[nodiscard]] constexpr BlockOrientation orientationOfState(std::uint16_t id) {
    const auto kind = blockKindOfState(id);
    const auto& definition = kBlockRegistry[kind];
    const std::uint16_t offset = static_cast<std::uint16_t>(id - kBlockStateRangeStarts[kind]);
    return static_cast<BlockOrientation>(
        offset / (blockFluidLevelCount(definition) * blockLitCount(definition)));
}

[[nodiscard]] constexpr std::uint8_t fluidLevelOfState(std::uint16_t id) {
    const auto kind = blockKindOfState(id);
    const auto& definition = kBlockRegistry[kind];
    const std::uint16_t offset = static_cast<std::uint16_t>(id - kBlockStateRangeStarts[kind]);
    return static_cast<std::uint8_t>((offset / blockLitCount(definition)) %
                                     blockFluidLevelCount(definition));
}

[[nodiscard]] constexpr bool litOfState(std::uint16_t id) {
    const auto kind = blockKindOfState(id);
    const std::uint16_t offset = static_cast<std::uint16_t>(id - kBlockStateRangeStarts[kind]);
    return offset % blockLitCount(kBlockRegistry[kind]) != 0U;
}

// The light a state emits. This is why `lit` had to become a state rather than
// a second block: the light engine reads it per cell, and a furnace's 13 only
// applies while it is burning.
[[nodiscard]] constexpr std::uint8_t emittedLightOfState(std::uint16_t id) {
    const auto kind = blockKindOfState(id);
    const auto& definition = kBlockRegistry[kind];
    return litOfState(id) ? definition.litLight : definition.light;
}

} // namespace mc::world
