#pragma once

#include "world/Block.hpp"
#include "world/StateSchema.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace mc::world {

// The interned block-state table: every state every block can be in, numbered
// once, so a cell is a single id rather than several parallel bytes.
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
//      and the lit furnace were; that trades a 3-bit ceiling for an 8-bit one
//      and breaks block identity on the way, see (2). A per-block id range has
//      neither ceiling.
//   2. Block identity separate from state. `Block::LitFurnace` was a burning
//      furnace's *state*, not a different block, but nothing in the type system
//      said so — so a WorldMutationService comparing old and new blocks would
//      destroy and rebuild the furnace's block entity on every burn swap,
//      losing the smelt. Now that `lit` is a value in the furnace's own id
//      range, BlockState::isSameBlock answers correctly with no special case.
//   3. Storage. Three arrays per section (blocks, orientations, fluid levels =
//      12 KB) collapse into one u16 array, and the mesher reads one array in
//      order instead of three in step.
//
// The axes themselves come from each block's `StateSchema` (see
// StateSchema.hpp). This file only does arithmetic over that list: it does not
// know what a crop or a furnace is, which is what lets a fourth property be
// added without touching it.

// How many states a block has: the product of its declared properties.
[[nodiscard]] constexpr std::uint16_t blockStateCount(const BlockDefinition& definition) {
    return definition.states.stateCount();
}

// One id range per built-in block, keyed by BlockId (== the enum ordinal for a
// built-in). The mixed-radix state arithmetic below is derived straight from the
// BlockId's declared StateSchema, so numbering a block's states never consults
// the enum — only the id and its schema.
inline constexpr std::size_t kBlockKindCount = kBuiltinBlockCount;

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
// wrap. Stairs and doors are what will move this number; the assertion is the
// tripwire, not a claim that the budget is comfortable.
static_assert(kBlockStateCount <= 65536U,
              "the interned block-state table must fit in a std::uint16_t id");

// State ids at or above the built-in table are not interned states at all: they
// name an UnknownBlock placeholder — a block a removed datapack/mod once placed —
// whose identity this build's registry no longer knows. They carry no baked
// metadata (every accessor below clamps them to block 0's air-like defaults) and
// exist only so the persistence layer can round-trip the block's original
// identifier and property blob (see persistence/UnknownBlockTable.hpp).
// BlockState::fromRawId keeps such an id verbatim rather than clamping it, which
// is what lets the sentinel survive a chunk-section palette and reach the save
// writer unchanged.
static_assert(kBlockStateCount < 65536U,
              "no id space is left above the built-in table for UnknownBlock placeholders");
inline constexpr std::uint16_t kFirstUnknownStateId = static_cast<std::uint16_t>(kBlockStateCount);
[[nodiscard]] constexpr bool isUnknownStateId(std::uint16_t id) {
    return id >= kFirstUnknownStateId;
}

// Hot-path metadata, indexed directly by the raw state id. Java stores these
// values on every BlockStateBase instance and precomputes its cache; C++ keeps
// the cell itself at two bytes and moves the immutable metadata to one compact
// constexpr structure-of-arrays table. Reading a property is a bounds check
// plus two indexed loads, never a division and never a search.
//
// `values` is one row per property, so an absent property reads back as 0 for
// every state of a block that does not declare it — which is why every
// property's zero has to be its sensible default (north, age 0, unlit, dry).
struct BlockStateMetadataTable final {
    // The identity of each interned state, held as a BlockId (uint16) rather than
    // the u8 enum so a state's block can range past 256 once external content
    // exists. `blockIdOfState` reads it directly; `blockOfState` narrows it back
    // to the enum handle for the callers that still speak `Block`.
    std::array<BlockId, kBlockStateCount> blocks{};
    std::array<std::array<std::uint8_t, kBlockStateCount>, kStatePropertyCount> values{};
    std::array<std::uint8_t, kBlockStateCount> emittedLights{};
};

[[nodiscard]] constexpr BlockStateMetadataTable buildBlockStateMetadata() {
    BlockStateMetadataTable result{};
    for (std::size_t kind = 0; kind < kBlockKindCount; ++kind) {
        const auto& definition = kBlockRegistry[kind];
        const auto& schema = definition.states;
        for (std::uint16_t id = kBlockStateRangeStarts[kind];
             id < kBlockStateRangeStarts[kind + 1U]; ++id) {
            const auto offset =
                static_cast<std::uint16_t>(id - kBlockStateRangeStarts[kind]);
            result.blocks[id] = BlockId::of(static_cast<BlockId::Value>(kind));
            for (std::size_t axisIndex = 0; axisIndex < schema.size(); ++axisIndex) {
                const auto axis = schema.axis(axisIndex);
                const auto digit = static_cast<std::uint8_t>(
                    (offset / schema.stride(axisIndex)) % axis.valueCount);
                result.values[static_cast<std::size_t>(axis.property)][id] = digit;
            }
            const bool isLit =
                result.values[static_cast<std::size_t>(StateProperty::Lit)][id] != 0U;
            result.emittedLights[id] = isLit ? definition.litLight : definition.light;
        }
    }
    return result;
}

inline constexpr auto kBlockStateMetadata = buildBlockStateMetadata();

[[nodiscard]] constexpr std::uint16_t validatedBlockStateId(std::uint16_t id) {
    return id < kBlockStateCount ? id : 0U;
}

// A block's default state: every property at value 0. This is what placing a
// block with nothing further to say produces.
[[nodiscard]] constexpr std::uint16_t defaultBlockStateId(Block block) {
    const auto kind = isValidBlock(block) ? static_cast<std::size_t>(block) : 0U;
    return kBlockStateRangeStarts[kind];
}

// The BlockId-keyed form of the above, for identity-first callers. An id past
// the built-in table (a future external block with no baked state range) falls
// back to block 0's default, the same way an out-of-enum Block does.
[[nodiscard]] constexpr std::uint16_t defaultBlockStateId(BlockId id) {
    const auto kind = id.index() < kBlockKindCount ? id.index() : 0U;
    return kBlockStateRangeStarts[kind];
}

// One property's value, straight out of the cache.
[[nodiscard]] constexpr std::uint8_t stateValueOf(std::uint16_t id, StateProperty property) {
    return kBlockStateMetadata
        .values[static_cast<std::size_t>(property)][validatedBlockStateId(id)];
}

// The same state with one property changed.
//
// A property the block does not declare is not an error and not representable:
// the id comes back unchanged, the way an invalid combination has never been
// numberable. A value outside the property's range falls back to 0 for the same
// reason — this mirrors what ChunkSection::setBlock already did when a cell's
// block changed under its state.
[[nodiscard]] constexpr std::uint16_t withStateValue(std::uint16_t id, StateProperty property,
                                                     std::uint8_t value) {
    const auto validId = validatedBlockStateId(id);
    const auto kind = kBlockStateMetadata.blocks[validId].index();
    const auto& schema = kBlockRegistry[kind].states;
    const auto stride = schema.strideOf(property);
    if (stride == 0U) {
        return validId; // the block has no such property
    }
    const auto count = schema.valueCount(property);
    const std::uint8_t next = value < count ? value : 0U;
    const auto current = stateValueOf(validId, property);
    return static_cast<std::uint16_t>(validId + (next - current) * stride);
}

// The block identity of an interned state, as the runtime BlockId.
[[nodiscard]] constexpr BlockId blockIdOfState(std::uint16_t id) {
    return kBlockStateMetadata.blocks[validatedBlockStateId(id)];
}

[[nodiscard]] constexpr Block blockOfState(std::uint16_t id) {
    return blockFromId(blockIdOfState(id));
}

[[nodiscard]] constexpr BlockOrientation orientationOfState(std::uint16_t id) {
    return static_cast<BlockOrientation>(stateValueOf(id, StateProperty::Facing));
}

[[nodiscard]] constexpr std::uint8_t fluidLevelOfState(std::uint16_t id) {
    return stateValueOf(id, StateProperty::FluidLevel);
}

[[nodiscard]] constexpr bool litOfState(std::uint16_t id) {
    return stateValueOf(id, StateProperty::Lit) != 0U;
}

// The light a state emits. This is why `lit` had to become a property rather
// than a second block: the light engine reads it per cell, and a furnace's 13
// only applies while it is burning.
[[nodiscard]] constexpr std::uint8_t emittedLightOfState(std::uint16_t id) {
    return kBlockStateMetadata.emittedLights[validatedBlockStateId(id)];
}

// Resolves a block plus the three properties the old three-axis table had to
// the interned id. Kept because facing, fluid level and lit are the properties
// most placement paths actually name; anything else goes through
// `BlockState::with(StateProperty, value)`.
[[nodiscard]] constexpr std::uint16_t blockStateId(Block block,
                                                   BlockOrientation orientation,
                                                   std::uint8_t fluidLevel, bool lit = false) {
    auto id = defaultBlockStateId(block);
    id = withStateValue(id, StateProperty::Facing, static_cast<std::uint8_t>(orientation));
    id = withStateValue(id, StateProperty::FluidLevel, fluidLevel);
    id = withStateValue(id, StateProperty::Lit, lit ? 1U : 0U);
    return id;
}

} // namespace mc::world
