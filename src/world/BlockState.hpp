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
    // LeverBlock/ButtonBlock.POWERED: whether the switch is on / the button held.
    [[nodiscard]] constexpr bool powered() const {
        return value(StateProperty::Powered) != 0U;
    }
    // RepeaterBlock.DELAY, exposed as the 1-4 tick delay (stored 0-3).
    [[nodiscard]] constexpr int repeaterDelay() const {
        return static_cast<int>(value(StateProperty::Delay)) + 1;
    }
    // A comparator's output signal, 0-15 (its ComparatorBlockEntity value here).
    [[nodiscard]] constexpr int analogSignal() const {
        return static_cast<int>(value(StateProperty::AnalogSignal));
    }
    // ComparatorBlock.MODE: true = SUBTRACT, false = COMPARE.
    [[nodiscard]] constexpr bool comparatorSubtract() const {
        return value(StateProperty::ComparatorMode) != 0U;
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
    // StateProperty::SubmergedFluid (F2). None for anything that has not
    // declared the axis (canBeSubmerged), so a caller reads it without
    // checking the block first — the same "absent property reads back as its
    // zero" contract every other axis here already gives.
    [[nodiscard]] constexpr SubmergedFluid submergedFluid() const {
        return static_cast<SubmergedFluid>(value(StateProperty::SubmergedFluid));
    }
    [[nodiscard]] constexpr BlockState withSubmergedFluid(SubmergedFluid fluid) const {
        return with(StateProperty::SubmergedFluid, static_cast<std::uint8_t>(fluid));
    }

    // AR-B2: StateProperty::Half read as a stair's Half (bottom/top). Bottom
    // (0) for anything that has not declared the axis.
    [[nodiscard]] constexpr SlabPortion stairHalf() const {
        // Reuses SlabPortion's Bottom/Top enumerators (both are plain 0/1 axes
        // with the same "which half of the cell" meaning) rather than minting a
        // third identical two-value enum; Double is never produced here since
        // the schema only ever gives Half two values.
        return static_cast<SlabPortion>(value(StateProperty::Half));
    }
    [[nodiscard]] constexpr BlockState withStairHalf(SlabPortion half) const {
        return with(StateProperty::Half, static_cast<std::uint8_t>(half));
    }
    // AR-B2: StateProperty::Half read as a door's Half (DoubleBlockHalf's
    // lower/upper). Same axis as stairHalf, different accessor name for the
    // reader — a door has no "which half of the cell" question, only "which of
    // the two cells".
    [[nodiscard]] constexpr bool isDoorUpperHalf() const {
        return value(StateProperty::Half) != 0U;
    }
    [[nodiscard]] constexpr BlockState withDoorUpperHalf(bool upper) const {
        return with(StateProperty::Half, upper ? 1U : 0U);
    }
    // AR-B3: StateProperty::Half read as a trapdoor's Half (TrapDoorBlock.HALF,
    // vanilla's Half enum — bottom/top face of the single cell it occupies,
    // not a door's "which of two cells" meaning). Bottom (0) for anything that
    // has not declared the axis, the same "absent reads as zero" contract
    // every other accessor here gives.
    [[nodiscard]] constexpr SlabPortion trapdoorHalf() const {
        return static_cast<SlabPortion>(value(StateProperty::Half));
    }
    [[nodiscard]] constexpr BlockState withTrapdoorHalf(SlabPortion half) const {
        return with(StateProperty::Half, static_cast<std::uint8_t>(half));
    }
    // StairBlock.SHAPE. Straight for anything that has not declared the axis.
    [[nodiscard]] constexpr StairShape stairShape() const {
        return static_cast<StairShape>(value(StateProperty::StairShape));
    }
    [[nodiscard]] constexpr BlockState withStairShape(StairShape shape) const {
        return with(StateProperty::StairShape, static_cast<std::uint8_t>(shape));
    }
    // DoorBlock.HINGE. Left for anything that has not declared the axis.
    [[nodiscard]] constexpr DoorHinge hinge() const {
        return static_cast<DoorHinge>(value(StateProperty::Hinge));
    }
    [[nodiscard]] constexpr BlockState withHinge(DoorHinge side) const {
        return with(StateProperty::Hinge, static_cast<std::uint8_t>(side));
    }
    // DoorBlock.OPEN / FenceGateBlock.OPEN. False for anything that has not
    // declared the axis.
    [[nodiscard]] constexpr bool open() const { return value(StateProperty::Open) != 0U; }
    [[nodiscard]] constexpr BlockState withOpen(bool value) const {
        return with(StateProperty::Open, value ? 1U : 0U);
    }
    // AR-B3: WallBlock's four per-side connection booleans. False (not
    // connected) for anything that has not declared the axis. `wallConnected`
    // takes a horizontal BlockOrientation rather than four named accessors, so
    // a neighbour-derivation loop over the four directions (updateShape, the
    // shape table) reads and writes through one call instead of a per-side
    // switch at the call site.
    [[nodiscard]] constexpr StateProperty wallAxis(BlockOrientation side) const {
        switch (side) {
        case BlockOrientation::North:
            return StateProperty::WallNorth;
        case BlockOrientation::East:
            return StateProperty::WallEast;
        case BlockOrientation::South:
            return StateProperty::WallSouth;
        case BlockOrientation::West:
            return StateProperty::WallWest;
        case BlockOrientation::Up:
        case BlockOrientation::Down:
            return StateProperty::WallNorth; // never asked; horizontal-only axis
        }
        return StateProperty::WallNorth;
    }
    [[nodiscard]] constexpr bool wallConnected(BlockOrientation side) const {
        return value(wallAxis(side)) != 0U;
    }
    [[nodiscard]] constexpr BlockState withWallConnected(BlockOrientation side, bool connected) const {
        return with(wallAxis(side), connected ? 1U : 0U);
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
    [[nodiscard]] constexpr BlockState withPowered(bool value) const {
        return with(StateProperty::Powered, value ? 1U : 0U);
    }
    // `delay` is the 1-4 tick delay a repeater shows; stored 0-3.
    [[nodiscard]] constexpr BlockState withRepeaterDelay(int delay) const {
        const int clamped = delay < 1 ? 1 : (delay > 4 ? 4 : delay);
        return with(StateProperty::Delay, static_cast<std::uint8_t>(clamped - 1));
    }
    [[nodiscard]] constexpr BlockState withAnalogSignal(int signal) const {
        const int clamped = signal < 0 ? 0 : (signal > 15 ? 15 : signal);
        return with(StateProperty::AnalogSignal, static_cast<std::uint8_t>(clamped));
    }
    [[nodiscard]] constexpr BlockState withComparatorSubtract(bool subtract) const {
        return with(StateProperty::ComparatorMode, subtract ? 1U : 0U);
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
    [[nodiscard]] constexpr std::uint32_t rawId() const { return id_; }
    // Rebuilds a state from a raw id — a chunk section's palette entry, or the
    // save layer round-tripping a cell. An id at or above the built-in table
    // (isUnknownStateId) names an UnknownBlock placeholder: content this build's
    // registry does not know, kept verbatim so the save can write its original
    // name and properties back. Such an id is preserved rather than clamped here;
    // every metadata accessor already clamps it to block 0's air-like defaults
    // (see validatedBlockStateId), so the cell stays inert without a special case.
    [[nodiscard]] static constexpr BlockState fromRawId(std::uint32_t value) {
        BlockState state;
        state.id_ = value;
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

    std::uint32_t id_ = 0U;
};

// The collision/selection shape a state carries lives in BlockShape.hpp
// (`blockShape`, `collisionSpan`), a layer above this one so the shape source
// can name a BlockState without this header depending on it.

// The light-engine's per-cell opacity, state-aware. skyLightOpacity(Block) in
// Block.hpp answers by *identity* alone, which is right for every block that
// does not carry a per-state shape — but a dry slab and a submerged one are
// the same Block with different light behaviour (F2's "含水格按水衰减"
// requirement), and Block.hpp cannot see BlockState (this header is the
// layer above it). Anything that is not a slab defers to the identity
// answer unchanged; a submerged slab reads Water's own filter instead of its
// own (a slab's un-submerged `lightFilter` is 0 — a dry slab does not dim the
// column at all, matching a dry stair/fence's vanilla behaviour), so the
// light engine dims a submerged cell exactly as if the cell held water.
[[nodiscard]] constexpr std::uint8_t skyLightOpacity(BlockState state) {
    if (state.submergedFluid() == SubmergedFluid::Water) {
        return skyLightOpacity(Block::Water);
    }
    return skyLightOpacity(state.block());
}

// BlockState is exactly its raw id: a uint32 now (widened from uint16 so the
// interned block-state space can grow past 65536 — see BlockStateTable.hpp). A
// chunk cell never stores this directly; the section palette does, and each cell
// holds only a small local index (ChunkSection), so the width change does not
// grow world memory. The id is never serialised raw — saves and the wire carry
// the block name + property names/values.
static_assert(sizeof(BlockState) == sizeof(std::uint32_t));
static_assert(BlockState{}.block() == Block::Air);
// The schema's arithmetic, pinned at compile time: a crop's age is its own
// property, and it no longer travels in the direction enum.
static_assert(BlockState{Block::WheatCrops}.withAge(7).age() == 7);
static_assert(BlockState{Block::WheatCrops}.withAge(7).orientation() == BlockOrientation::North);
static_assert(BlockState{Block::Furnace, BlockOrientation::West}.withLit(true).lit());
static_assert(BlockState{Block::Furnace, BlockOrientation::West}.withLit(true).orientation() ==
              BlockOrientation::West);

} // namespace mc::world
