#pragma once

#include "world/Block.hpp"
#include "world/BlockStateTable.hpp"

#include <cstdint>

namespace mc::world {

// One cell's whole block state as a single opaque value.
//
// Why an opaque id rather than the three fields it currently packs: every
// signature that takes a block — WorldMutationService::setBlock, the block
// behaviour callbacks, the tick scheduler — wants to name "the state of this
// cell", and there are 42 call sites that pass one. Naming the three fields in
// those signatures would mean rewriting all 42 again when the representation
// changes. So the representation is sealed behind this type now, and T0.4
// swaps the packing below for an interned state table without touching a
// single caller.
//
// The representation is deliberately not part of the contract: the id is an
// index into the interned table in BlockStateTable.hpp, and nothing outside
// these two headers may assume anything about its value or its width.
//
// `orientation` is the per-cell state slot the project already overloads:
// a facing for torches and furnaces, an age 0-7 for crops, a moisture 0-7 for
// farmland, a persistence flag for leaves (see cropAge/farmlandMoisture in
// Block.hpp). That overloading is exactly what a real property table replaces;
// until T0.4 lands, the slot is passed through unchanged so behaviour does not
// move.
class BlockState final {
  public:
    constexpr BlockState() = default;

    constexpr explicit BlockState(Block block, BlockOrientation orientation = BlockOrientation::North,
                                  std::uint8_t fluidLevel = 0U, bool lit = false)
        : id_(blockStateId(block, orientation, fluidLevel, lit)) {}

    [[nodiscard]] constexpr Block block() const { return blockOfState(id_); }
    [[nodiscard]] constexpr BlockOrientation orientation() const {
        return orientationOfState(id_);
    }
    [[nodiscard]] constexpr std::uint8_t fluidLevel() const { return fluidLevelOfState(id_); }
    // AbstractFurnaceBlock.LIT. False for everything that cannot burn, so a
    // caller never has to ask whether the property exists first.
    [[nodiscard]] constexpr bool lit() const { return litOfState(id_); }
    // The light this state emits: a furnace's 13 only while it burns.
    [[nodiscard]] constexpr std::uint8_t emittedLight() const {
        return emittedLightOfState(id_);
    }

    [[nodiscard]] constexpr BlockState with(BlockOrientation value) const {
        return BlockState{block(), value, fluidLevel(), lit()};
    }
    [[nodiscard]] constexpr BlockState withFluidLevel(std::uint8_t value) const {
        return BlockState{block(), orientation(), value, lit()};
    }
    [[nodiscard]] constexpr BlockState withLit(bool value) const {
        return BlockState{block(), orientation(), fluidLevel(), value};
    }

    // Whether two states belong to the same block, ignoring everything else.
    //
    // This is the predicate LevelChunk.setBlockState uses to decide a block
    // entity's fate: same block keeps it, different block destroys and
    // recreates it. It is a named method rather than `a.block() == b.block()`
    // at the call site because T0.4 turns the lit furnace and the four wall
    // torches from separate Block values into properties of one block, and the
    // answer here has to change with them. A furnace that keeps burning must
    // not lose its smelting progress to a lit-state swap.
    [[nodiscard]] constexpr bool isSameBlock(BlockState other) const {
        return block() == other.block();
    }

    [[nodiscard]] constexpr bool operator==(const BlockState&) const = default;

    // The raw id, for the save palette and for hashing. Not stable across
    // builds — the save layer must map it through a palette of identifiers,
    // exactly as it already does for Block.
    [[nodiscard]] constexpr std::uint16_t rawId() const { return id_; }
    [[nodiscard]] static constexpr BlockState fromRawId(std::uint16_t value) {
        BlockState state;
        state.id_ = value;
        return state;
    }

  private:
    std::uint16_t id_ = 0U;
};

static_assert(sizeof(BlockState) == sizeof(std::uint16_t));
static_assert(BlockState{}.block() == Block::Air);

} // namespace mc::world
