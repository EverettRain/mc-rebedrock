#pragma once

#include "world/Block.hpp"
#include "world/BlockStateTable.hpp"
#include "world/StateSchema.hpp"

#include <cstdint>

namespace mc::world {

// One cell's whole block state as a single opaque value.
//
// Why an opaque id rather than the fields it packs: every signature that takes
// a block — WorldMutationService::setBlock, the block behaviour callbacks, the
// tick scheduler — wants to name "the state of this cell", and there are dozens
// of call sites that pass one. Naming the individual fields in those signatures
// would mean rewriting all of them again when the representation changes. So
// the representation is sealed behind this type.
//
// The representation is deliberately not part of the contract: the id is an
// index into the interned table in BlockStateTable.hpp, it is not stable across
// builds, and nothing outside these headers may assume anything about its value
// or its width. The save layer maps it through a palette of block identifiers
// and property *names*, never through the raw number.
//
// Which properties a state carries is the block's own declaration
// (`StateSchema`, see StateSchema.hpp). Asking a block for a property it does
// not have is not an error: it reads back as that property's zero and writing
// it changes nothing, so a caller never has to check first.
class BlockState final {
  public:
    constexpr BlockState() = default;

    constexpr explicit BlockState(Block block, BlockOrientation orientation = BlockOrientation::North,
                                  std::uint8_t fluidLevel = 0U, bool lit = false)
        : id_(blockStateId(block, orientation, fluidLevel, lit)) {}

    [[nodiscard]] constexpr Block block() const { return blockOfState(id_); }

    // The generic pair. Everything below is a named shorthand for these two, and
    // they are what a caller that iterates properties — the save layer, a future
    // `/setblock oak_stairs[half=top]` — uses instead of a fixed field list.
    [[nodiscard]] constexpr std::uint8_t value(StateProperty property) const {
        return stateValueOf(id_, property);
    }
    [[nodiscard]] constexpr BlockState with(StateProperty property, std::uint8_t value) const {
        return fromRawId(withStateValue(id_, property, value));
    }
    // Whether this state's block declares the property at all. Only diagnostics
    // and serialisation need to ask; ordinary code can read any property.
    [[nodiscard]] constexpr bool has(StateProperty property) const {
        return kBlockRegistry[static_cast<std::size_t>(block())].states.has(property);
    }

    [[nodiscard]] constexpr BlockOrientation orientation() const {
        return orientationOfState(id_);
    }
    [[nodiscard]] constexpr std::uint8_t fluidLevel() const { return fluidLevelOfState(id_); }
    // AbstractFurnaceBlock.LIT. False for everything that cannot burn, so a
    // caller never has to ask whether the property exists first.
    [[nodiscard]] constexpr bool lit() const { return litOfState(id_); }
    // CropBlock.AGE, 0-7. Zero for anything that is not a crop.
    [[nodiscard]] constexpr int age() const {
        return static_cast<int>(value(StateProperty::Age));
    }
    // FarmlandBlock.MOISTURE, 0-7.
    [[nodiscard]] constexpr int moisture() const {
        return static_cast<int>(value(StateProperty::Moisture));
    }
    // LeavesBlock.PERSISTENT: leaves a player placed, which never decay.
    [[nodiscard]] constexpr bool persistent() const {
        return value(StateProperty::Persistent) != 0U;
    }
    // The light this state emits: a furnace's 13 only while it burns.
    [[nodiscard]] constexpr std::uint8_t emittedLight() const {
        return emittedLightOfState(id_);
    }
    // SlabBlock.TYPE. Bottom for anything that is not a slab, so a caller reads
    // it without checking the block first.
    [[nodiscard]] constexpr SlabPortion slabPortion() const {
        return static_cast<SlabPortion>(value(StateProperty::SlabType));
    }
    // Whether this state fills its whole cell: a full cube, or a double slab.
    // The mesher, collision and support checks read this rather than
    // isFullCube(block), which cannot see a slab's per-state shape.
    [[nodiscard]] constexpr bool isFullCubeState() const {
        if (isSlab(block())) {
            return slabPortion() == SlabPortion::Double;
        }
        return isFullCube(block());
    }

    [[nodiscard]] constexpr BlockState with(BlockOrientation orientation) const {
        return with(StateProperty::Facing, static_cast<std::uint8_t>(orientation));
    }
    [[nodiscard]] constexpr BlockState withFluidLevel(std::uint8_t level) const {
        return with(StateProperty::FluidLevel, level);
    }
    [[nodiscard]] constexpr BlockState withLit(bool value) const {
        return with(StateProperty::Lit, value ? 1U : 0U);
    }
    // Out-of-range ages and moistures clamp rather than wrapping to zero: these
    // two are counters that code increments, and a grower that runs one past the
    // maximum must saturate, not reset the crop to a seedling.
    [[nodiscard]] constexpr BlockState withAge(int age) const {
        return with(StateProperty::Age, clampProperty(age, StateProperty::Age));
    }
    [[nodiscard]] constexpr BlockState withMoisture(int moisture) const {
        return with(StateProperty::Moisture, clampProperty(moisture, StateProperty::Moisture));
    }
    [[nodiscard]] constexpr BlockState withPersistent(bool value) const {
        return with(StateProperty::Persistent, value ? 1U : 0U);
    }
    [[nodiscard]] constexpr BlockState withSlabPortion(SlabPortion portion) const {
        return with(StateProperty::SlabType, static_cast<std::uint8_t>(portion));
    }

    // Whether two states belong to the same block, ignoring everything else.
    //
    // This is the predicate LevelChunk.setBlockState uses to decide a block
    // entity's fate: same block keeps it, different block destroys and
    // recreates it. It is a named method rather than `a.block() == b.block()`
    // at the call site because the lit furnace and the four wall torches are
    // properties of one block rather than separate Block values, and the answer
    // here has to travel with them. A furnace that keeps burning must not lose
    // its smelting progress to a lit-state swap.
    [[nodiscard]] constexpr bool isSameBlock(BlockState other) const {
        return block() == other.block();
    }

    [[nodiscard]] constexpr bool operator==(const BlockState&) const = default;

    // The raw id, for the section palette and for hashing. Not stable across
    // builds — the save layer must map it through identifiers and property
    // names, exactly as it already does for Block.
    [[nodiscard]] constexpr std::uint16_t rawId() const { return id_; }
    [[nodiscard]] static constexpr BlockState fromRawId(std::uint16_t value) {
        BlockState state;
        state.id_ = validatedBlockStateId(value);
        return state;
    }

  private:
    [[nodiscard]] constexpr std::uint8_t clampProperty(int requested,
                                                       StateProperty property) const {
        const auto count =
            kBlockRegistry[static_cast<std::size_t>(block())].states.valueCount(property);
        if (requested < 0) {
            return 0U;
        }
        const int maximum = static_cast<int>(count) - 1;
        return static_cast<std::uint8_t>(requested > maximum ? maximum : requested);
    }

    std::uint16_t id_ = 0U;
};

// The vertical span [bottom, top] of a state's collision box within its cell,
// in 0..1 cell-local units. A full cube is {0, 1}, a non-colliding block {0, 0},
// farmland the vanilla 15/16 box, and a slab its half box (bottom {0, 0.5}, top
// {0.5, 1}, double {0, 1}). An empty span (top <= bottom) means no collision.
//
// This is the one place the "how tall is this block, and where does its box sit"
// question is answered, so the player walk, the creature walk and the placement
// occupancy check all read the same shape instead of each assuming a full cube.
// It is a vertical span rather than a box set because every shape this project
// has so far (farmland, slabs) fills its whole 1x1 footprint; stairs and fences
// will need a box set and should extend this rather than fork it.
struct BlockCollisionSpan final {
    float bottom = 0.0F;
    float top = 0.0F;
};

[[nodiscard]] constexpr BlockCollisionSpan collisionSpan(BlockState state) {
    const Block block = state.block();
    if (!hasCollision(block)) {
        return {};
    }
    if (isSlab(block)) {
        switch (state.slabPortion()) {
        case SlabPortion::Bottom:
            return {0.0F, 0.5F};
        case SlabPortion::Top:
            return {0.5F, 1.0F};
        case SlabPortion::Double:
            return {0.0F, 1.0F};
        }
    }
    if (isFarmland(block)) {
        return {0.0F, kFarmlandModelHeight};
    }
    return {0.0F, 1.0F};
}

static_assert(sizeof(BlockState) == sizeof(std::uint16_t));
static_assert(BlockState{}.block() == Block::Air);
// The schema's arithmetic, pinned at compile time: a crop's age is its own
// property, and it no longer travels in the direction enum.
static_assert(BlockState{Block::WheatCrops}.withAge(7).age() == 7);
static_assert(BlockState{Block::WheatCrops}.withAge(7).orientation() == BlockOrientation::North);
static_assert(BlockState{Block::Furnace, BlockOrientation::West}.withLit(true).lit());
static_assert(BlockState{Block::Furnace, BlockOrientation::West}.withLit(true).orientation() ==
              BlockOrientation::West);

} // namespace mc::world
