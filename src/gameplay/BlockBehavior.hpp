#pragma once

// B1-1 — the block behaviour table and its pre-filter, the infrastructure R1
// (B-block) builds its switch retirement on. Vanilla dispatches block behaviour
// through BlockBehaviour's virtual overrides; this is the C++ DOD equivalent —
// a table indexed by BlockId whose entries are plain function pointers plus a
// pre-filter bitset, so the hot loop tests one bit before it ever touches a
// slot. It mirrors the shape already proven by kRandomTickTable (fn-ptr + an
// `isRandomlyTicking` pre-filter) and BlockShape (an interned table the shape is
// derived from), rather than inventing a new one.
//
// This file is B1-1: it *builds the table and proves the dispatch mechanism*.
// It does not retire any switch — B1-2 fills the placement/shape slots and
// deletes those switches, B1-3 fills the drops/interaction slots and deletes
// theirs, W-3/W-4 fill updateShape and the redstone signal slots. The one slot
// wired here is getDrops, pointed at the existing MiningSystem::minedDrops in
// parallel with its switch (zero behaviour change), so the parity harness can
// show the table produces exactly the drops the switch does.
//
// The table lives in gameplay/ rather than world/ on purpose: two of the six
// pre-filter bits (HasRandomTick, HasDrops) are gameplay-layer facts, so keeping
// the whole entry here avoids fracturing the bitset across the layer boundary
// (B-DESIGN §2 blesses either world/ or gameplay/).

#include "gameplay/Inventory.hpp"       // ItemStack
#include "gameplay/MiningSystem.hpp"    // MinedDrops, minedDrops
#include "gameplay/RedstoneEmission.hpp" // redstone::PowerFn, weakPowerFn, isSignalSource
#include "gameplay/RedstoneSignal.hpp"  // AR-B4-4: redstone::repeaterIsLocked
#include "gameplay/WorldSimulation.hpp" // WorldSimulation::isRandomlyTicking
#include "world/Block.hpp"
#include "world/BlockPlacement.hpp" // PlacementContext, placementBlock (World fwd)
#include "world/BlockPos.hpp"       // BlockPos
#include "world/BlockRegistry.hpp"  // blockCount
#include "world/BlockShape.hpp"     // BlockShape, blockShape
#include "world/BlockState.hpp"
#include "world/StairShapeDerivation.hpp" // AR-B2: stairUpdateShape, doorUpdateShape
#include "world/WallShapeDerivation.hpp"  // AR-B3: wallUpdateShape

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace mc::gameplay {

// The pre-filter bits, one per behaviour class the hot loops want to reject in
// bulk. The bit answers "could this block possibly do X" so the caller can skip
// the 99% of blocks that cannot before it pays for a table lookup or a call.
enum class BlockBehaviorBit : std::uint8_t {
    // Reacts to a neighbour or support change (grass/flowers/crops that pop off
    // bad ground, torches that fall). Sourced from the block's support rule now;
    // W-3 generalises it to updateShape (fence connections, redstone wire).
    HasNeighborReaction = 0,
    // Emits a redstone signal (redstone::isSignalSource): the pre-filter the
    // signal aggregation and the wire evaluator use to skip the overwhelming
    // majority of blocks before asking for any power. Sourced from the redstone
    // emission model now that redstone (W-4/5/6) has landed.
    IsSignalSource = 1,
    // Breaking it can yield loot (the non-silk drop). Distinct from the block's
    // dropsItem flag: glass drops nothing despite dropsItem, and tall grass and
    // the crops drop despite noDrops().
    HasDrops = 2,
    // Right-clicking it does something block-side (opens a container). Item-side
    // uses (flint&steel, hoe, bonemeal) are item behaviour, not this bit.
    HasInteraction = 3,
    // Has a random tick (grass spread, crop growth). Mirrors kRandomTickTable's
    // own pre-filter so the two cannot disagree.
    HasRandomTick = 4,
    // Has a collision box. The draw/collision loops' cheapest reject.
    HasCollision = 5,
};

// A packed set of the bits above. Six bits fit one byte, so the pre-filter costs
// nothing to store next to the slot pointers and one masked load to test.
struct BlockBehaviorPrefilter final {
    std::uint8_t bits = 0U;

    [[nodiscard]] constexpr bool has(BlockBehaviorBit bit) const {
        return (bits & maskOf(bit)) != 0U;
    }
    constexpr void set(BlockBehaviorBit bit, bool on) {
        const auto mask = maskOf(bit);
        bits = on ? static_cast<std::uint8_t>(bits | mask)
                  : static_cast<std::uint8_t>(bits & static_cast<std::uint8_t>(~mask));
    }
    [[nodiscard]] constexpr bool operator==(const BlockBehaviorPrefilter&) const = default;

  private:
    [[nodiscard]] static constexpr std::uint8_t maskOf(BlockBehaviorBit bit) {
        return static_cast<std::uint8_t>(1U << static_cast<unsigned>(bit));
    }
};

// Whether breaking `block` can produce loot at all (the HasDrops pre-filter's
// source of truth). Derived, not a second copy of the drops switch: a block
// yields loot unless it is glass (silk-touch only) or an item-less block whose
// only "drop" is being air/fluid. The four noDrops() plants that still roll a
// loot table — tall grass and the three crops — are named explicitly. The parity
// harness pins this against the real MiningSystem::minedDrops for every block.
[[nodiscard]] constexpr bool blockYieldsLoot(world::Block block) {
    using world::Block;
    if (block == Block::Glass) return false; // harvestable, but silk-touch only
    if (world::blockDefinition(block).dropsItem) return true;
    return block == Block::GrassPlant || block == Block::WheatCrops ||
           block == Block::Carrots || block == Block::Potatoes;
}

// The pre-filter for one built-in block, computed from its definition and the
// existing behaviour tables. Fully constexpr so the built-in table below bakes
// into rodata, the way kRandomTickTable does.
[[nodiscard]] constexpr BlockBehaviorPrefilter blockBehaviorPrefilterFor(world::Block block) {
    const auto& definition = world::blockDefinition(block);
    BlockBehaviorPrefilter prefilter;
    prefilter.set(BlockBehaviorBit::HasCollision, definition.collision);
    prefilter.set(BlockBehaviorBit::HasInteraction,
                  definition.container != world::ContainerType::None);
    // AR-B2: a stair recomputes its join shape from a changed horizontal
    // neighbour (StairBlock#updateShape) and a door's upper half tracks its
    // lower half's removal (DoorBlock#updateShape) — both are updateShape
    // reactions, model-driven like the interaction bit above so a species never
    // needs to declare this by hand.
    // AR-B3: a wall recomputes its four connection bits from a changed
    // horizontal neighbour (WallBlock#updateShape), the identical
    // model-driven updateShape reaction stairs/doors already declare here.
    // AR-B4-4: a fence gate re-derives IN_WALL from the two neighbours across
    // its axis (FenceGateBlock#updateShape), and a repeater re-derives LOCKED
    // from the diodes at its sides (RepeaterBlock#updateShape). The gate is
    // model-driven like the three above; the repeater is *declaration*-driven —
    // it shares BlockModel::ElementModel with the comparator, the lever, the
    // anvil and the enchanting table, so keying on the model would drag all of
    // them onto this path. "Does this block declare LOCKED" is the honest
    // question, and it wires the next diode automatically.
    prefilter.set(BlockBehaviorBit::HasNeighborReaction,
                  definition.support != world::BlockSupport::None ||
                      definition.model == world::BlockModel::Stairs ||
                      definition.model == world::BlockModel::Door ||
                      definition.model == world::BlockModel::Wall ||
                      definition.model == world::BlockModel::FenceGate ||
                      definition.states.has(world::StateProperty::Locked));
    prefilter.set(BlockBehaviorBit::HasRandomTick, WorldSimulation::isRandomlyTicking(block));
    prefilter.set(BlockBehaviorBit::HasDrops, blockYieldsLoot(block));
    prefilter.set(BlockBehaviorBit::IsSignalSource, redstone::isSignalSource(block));
    return prefilter;
}

// The constexpr built-in pre-filter, baked into rodata and indexed by BlockId
// ordinal — the hot loops' fast path, and the reference the runtime table below
// must agree with. Sized to the built-in count for the same reason
// kRandomTickTable is: only built-in blocks have compile-time behaviour to bake.
inline constexpr std::array<BlockBehaviorPrefilter, world::kBuiltinBlockCount>
    kBuiltinBlockBehaviorPrefilter = [] {
        std::array<BlockBehaviorPrefilter, world::kBuiltinBlockCount> entries{};
        for (std::size_t i = 0; i < world::kBuiltinBlockCount; ++i) {
            entries[i] = blockBehaviorPrefilterFor(static_cast<world::Block>(i));
        }
        return entries;
    }();

// The behaviour slots the migration tasks fill. B1-1 declared the slot types and
// left every one but getDrops null; B1-2 fills getShape and getStateForPlacement.
// Dispatch skips a null slot. A reserved slot's context is forward-declared and
// defined by the task that fills it, so reserving it pulls no extra include and
// pre-commits no context layout.
struct InteractionBehaviorContext; // B1-3 (useItemOn)
struct NeighborUpdateContext;      // W-3 (updateShape)
// W-8: what the onPlace slot reads. `state` is the block that just arrived;
// `previous` is what it replaced, for a future onRemove user. The simulation is
// here because the one thing a placement behaviour does so far is schedule a
// tick, and only the simulation owns the scheduler.
struct BlockLifecycleContext final {
    world::World& world;
    world::BlockPos pos;
    world::BlockState state;
    world::BlockState previous;
    WorldSimulation& simulation;
};

// Everything the getStateForPlacement slot reads (B1-2). It carries the world,
// the block being placed and the use-on interaction, and forwards to
// world::placementBlock — the single placement source — so placement dispatches
// through the behaviour table rather than a switch on the block.
struct PlacementBehaviorContext final {
    const world::World& world;
    world::Block block = world::Block::Air;
    const world::PlacementContext& placement;
};

// Everything the updateShape slot reads (W-3), the C++ shape of Java's
// updateShape(state, level, pos, direction, neighborPos, neighborState). The
// derivation looks at the changed neighbour (and, through `world`, any others it
// needs — a fence checks all four sides) and returns the block's *new state*, or
// nullopt for "no change". Its contract is A3b: a pure property rewrite of the
// same block. `neighborPos` is `pos + fromOffset`.
struct NeighborUpdateContext final {
    const world::World& world;
    world::BlockPos pos;              // the cell recomputing its shape
    world::BlockState state;          // its current state — the derivation's input
    world::BlockPos fromOffset;       // pos -> the neighbour that changed
    world::BlockState neighborState;  // that neighbour's new state
};

using GetDropsFn =
    MinedDrops (*)(world::Block, const ItemStack&, std::uint64_t&, int, bool);
using GetShapeFn = world::BlockShape (*)(world::BlockState);
using GetStateForPlacementFn = std::optional<world::BlockState> (*)(const PlacementBehaviorContext&);
using UseItemOnFn = void (*)(const InteractionBehaviorContext&);
using UpdateShapeFn = std::optional<world::BlockState> (*)(const NeighborUpdateContext&);
using BlockLifecycleFn = void (*)(const BlockLifecycleContext&);

// One block's behaviour: the slot pointers plus the pre-filter. An entry with a
// null slot and a clear bit is a block that does nothing on that path, which the
// dispatch helpers below cost nothing for.
struct BlockBehavior final {
    // Wired in B1-1 to the existing MiningSystem::minedDrops, in parallel with
    // its switch. B1-3 splits it per block and deletes the switch.
    GetDropsFn getDrops = nullptr;
    // Wired in B1-2 to the single-source shape/placement free functions, in place
    // of the model/support switches those functions used to hold.
    GetShapeFn getShape = nullptr;                          // B1-2
    GetStateForPlacementFn getStateForPlacement = nullptr;  // B1-2
    // Reserved, null until their owning task fills them.
    UseItemOnFn useItemOn = nullptr;                        // B1-3
    UpdateShapeFn updateShape = nullptr;                    // W-3
    // W-8/W-9: the block newly *arrived* here — Java's Block#onPlace under the
    // `!oldState.is(state.getBlock())` guard that most of its overrides carry
    // (RedStoneWireBlock, TntBlock, ObserverBlock, LightningRodBlock all open
    // with it). That guard is the slot's trigger, exactly: MutationSink::
    // onBlockPlaced fires when the block *kind* at the cell changed, and not on a
    // state-only write.
    //
    // W-8 named this setPlacedBy, which W-9 corrects. The distinction W-8 was
    // drawing is real and load-bearing — a hook on *every* setBlockState would
    // see a diode's own POWERED flip — but Java's name for the kind-changed
    // variant is onPlace, and setPlacedBy is something narrower still: BlockItem
    // #place calls it, so it fires only for an entity placing an item and never
    // for a command fill, a piston move or worldgen. This slot deliberately
    // covers all of those, so it is a strict superset of setPlacedBy and an
    // exact match for the guarded onPlace. Nothing about the gate changed; only
    // the name it goes by. A diode created by a command self-starting is a
    // consequence of the superset, and the right answer anyway.
    //
    // Wired for the diodes (self-start), redstone dust (solve its own POWER) and
    // the pistons (extend if they landed in a powered region); null otherwise.
    BlockLifecycleFn onPlace = nullptr;                     // W-8, W-9
    // Still unwired: it has no user yet, and a slot dispatched to nobody is
    // worse than an empty one. Its semantics will be Java's
    // affectNeighborsAfterRemoval, the mirror of onPlace above.
    BlockLifecycleFn onRemove = nullptr;                    // lifecycle
    // Redstone signal emission (RedstoneEmission.hpp): weak power (getSignal) and
    // strong/direct power (getDirectSignal) a block state pushes out of a face.
    // Wired for every signal source, gated by the IsSignalSource pre-filter. The
    // hot redstone query reads the constexpr emission tables directly (see
    // RedstoneEmission.hpp), the way the mesher reads world::blockShape rather
    // than dispatchBlockShape; these slots put the same answer on the one
    // behaviour face so a non-redstone consumer reaches it uniformly.
    redstone::PowerFn getWeakPower = nullptr;              // == redstone::getSignal
    redstone::PowerFn getStrongPower = nullptr;            // == redstone::getDirectSignal

    BlockBehaviorPrefilter prefilter{};
};

namespace detail {

// The getStateForPlacement slot: forwards to world::placementBlock, the single
// placement source (it resolves orientation/slab-half/wall-facing from the
// block's model and support fields, and routes the torch to standingAndWall
// itself). Every built-in shares this one slot; B1-3/I-item may later split it.
[[nodiscard]] inline std::optional<world::BlockState>
placementSlot(const PlacementBehaviorContext& context) {
    return world::placementBlock(context.world, context.block, context.placement);
}

// AR-B2's updateShape slots: adapt the context struct into the plain
// (world, pos, state, ...) parameters StairShapeUpdate.hpp's derivations take
// (kept independent of this header — see that file's comment), always
// returning a value so applyUpdateShapeContract's own fixed-point check (an
// unchanged result reports "no change") is the single place that convergence
// is decided, not each derivation re-deciding it.
[[nodiscard]] inline std::optional<world::BlockState>
stairUpdateShapeSlot(const NeighborUpdateContext& context) {
    return world::stairUpdateShape(context.world, context.pos, context.state, context.fromOffset);
}
[[nodiscard]] inline std::optional<world::BlockState>
doorUpdateShapeSlot(const NeighborUpdateContext& context) {
    return world::doorUpdateShape(context.state, context.fromOffset, context.neighborState);
}
// AR-B3's updateShape slot: wallUpdateShape re-derives all four connection
// bits from the current world (same "re-derive fresh, not incremental" shape
// stairUpdateShapeSlot already takes above).
[[nodiscard]] inline std::optional<world::BlockState>
wallUpdateShapeSlot(const NeighborUpdateContext& context) {
    return world::wallUpdateShape(context.world, context.pos, context.state, context.fromOffset);
}
// AR-B4-4's slots. The gate's derivation is world-layer (it only asks whether a
// neighbour is a wall); the repeater's is not, because "locked" is a redstone
// question, so it lives here and calls the one existing predicate rather than
// restating it.
// W-8's slot: DiodeBlock#setPlacedBy (DiodeBlock.java:159-163) —
//
//     if (this.shouldTurnOn(level, pos, state)) level.scheduleTick(pos, this, 1);
//
// A diode dropped into a line that is already live has to start itself. Nothing
// else will: the ordinary placement fan-out tells the diode's *neighbours* that
// something appeared, never the new cell about itself, so a repeater placed on a
// powered wire sat dark until an unrelated edit happened to wake it.
inline void diodePlacedSlot(const BlockLifecycleContext& context) {
    context.simulation.scheduleDiodeSelfStart(context.world, context.pos, context.state);
}

// W-9's slot: RedStoneWireBlock#onPlace (RedStoneWireBlock.java:296-304) —
//
//     if (!oldState.is(state.getBlock()) && !level.isClientSide()) {
//         this.updatePowerStrength(level, pos, state, null, true);
//         for (Direction d : Direction.Plane.VERTICAL) level.updateNeighborsAt(pos.relative(d), this);
//         this.updateNeighborsOfNeighboringWires(level, pos);
//     }
//
// Dust dropped into a cell that is already being powered has to solve its own
// POWER. Nothing else will — the placement's fan-out tells the *neighbours*
// something appeared, and a block of redstone or a lever told about a new
// neighbour has nothing to recompute — so dust laid against a live source sat at
// 0 until something else nearby happened to be edited. The second dust cell of a
// run is quietly saved by the first one's island re-solve, which is why only the
// cell touching the source ever shows the defect.
//
// Only the `updatePowerStrength` line is ported. The two fan-out lines below it
// exist in Java to service *corner* (step-up/step-down) wire connections, which
// this build's wire model does not have: WireNetworkEvaluator::floodNetwork
// connects wires through the six orthogonal neighbours only (RedstoneWire.hpp
// says as much — the conductor step rules are a later refinement), and a wire
// tick re-solves the entire connected island in one pass and then wakes every
// changed cell's non-wire neighbours. That is strictly more than Java's
// incremental propagation reaches, so copying the two fan-outs would add a
// notification storm with nothing to notify. When the step rules do land, the
// island flood gains those edges and this stays unnecessary — but that is the
// change that must revisit this comment.
//
// W-9 also hangs the piston here, from the other Java hook —
// PistonBaseBlock#setPlacedBy (PistonBaseBlock.java:75-78) calls the same
// `checkIfExtend` its neighborChanged does, so a piston placed into a powered
// region extends on the spot instead of waiting for an edit that may never
// come. The body is the same one line for both: "this cell has arrived, read
// what is reaching it" is exactly what notifyRedstoneComponent means, and
// writing it twice would only let the two copies drift.
inline void redstoneRecheckPlacedSlot(const BlockLifecycleContext& context) {
    context.simulation.notifyRedstoneComponent(
        context.world, {context.pos.x, context.pos.y, context.pos.z});
}

// The blocks on that slot: dust and the two pistons. Not a model or a tag test —
// the wire is one block, and the pistons share BlockModel::Cube with most of the
// roster — so it is spelled out, the way the LOCKED-declaring repeater test above
// is, and the block_behavior slot-ownership assertion holds it to this list.
[[nodiscard]] inline constexpr bool rechecksRedstoneOnPlacement(world::Block block) {
    return block == world::Block::RedstoneWire || block == world::Block::Piston ||
           block == world::Block::StickyPiston;
}

[[nodiscard]] inline std::optional<world::BlockState>
fenceGateUpdateShapeSlot(const NeighborUpdateContext& context) {
    return world::fenceGateUpdateShape(context.world, context.pos, context.state,
                                       context.fromOffset);
}
[[nodiscard]] inline std::optional<world::BlockState>
repeaterLockedUpdateShapeSlot(const NeighborUpdateContext& context) {
    // RepeaterBlock#updateShape: recompute LOCKED unless the neighbour that
    // changed lies on the repeater's own FACING axis. FACING is horizontal, so
    // the vertical neighbours (axis Y) are *not* excluded — a diode placed above
    // or below still triggers the recompute, and reading this as "only the two
    // horizontal sides" is the easy way to get it wrong.
    const world::BlockOrientation from = world::orientationFromOffset(
        glm::ivec3{context.fromOffset.x, context.fromOffset.y, context.fromOffset.z});
    if (world::sameHorizontalAxis(from, context.state.orientation()) &&
        world::isHorizontal(from)) {
        return context.state;
    }
    return context.state.withRepeaterLocked(
        redstone::repeaterIsLocked(context.world, context.pos, context.state));
}

// Builds the runtime table. Sized to the *registry* (blockCount()), not to the
// built-in constant, so it grows with external blocks (R0-5) instead of topping
// out — a hardcoded capacity here would drop or overflow external ids. Built-in
// entries take the baked pre-filter and the wired slots (getDrops, getShape,
// getStateForPlacement); external entries stay empty until data-driven
// definitions (D) attach their behaviour.
[[nodiscard]] inline std::vector<BlockBehavior> buildBlockBehaviorTable() {
    std::vector<BlockBehavior> table(world::blockCount());
    const std::size_t builtins = std::min<std::size_t>(world::kBuiltinBlockCount, table.size());
    for (std::size_t i = 0; i < builtins; ++i) {
        auto& entry = table[i];
        entry.prefilter = kBuiltinBlockBehaviorPrefilter[i];
        if (entry.prefilter.has(BlockBehaviorBit::HasDrops)) {
            // B1-3 wires the block's own drop handler (blockDropFn), so the slot
            // is per-block rather than one shared minedDrops with a switch inside.
            entry.getDrops = blockDropFn(static_cast<world::Block>(i));
        }
        // getShape/getStateForPlacement point at the single-source free functions
        // (world::blockShape and placementSlot). Both gate internally — blockShape
        // maps air/fluid to an empty shape, placementBlock returns nullopt for a
        // block that cannot be placed — so wiring every built-in stays behaviour-
        // identical to calling them directly.
        entry.getShape = &world::blockShape;
        entry.getStateForPlacement = &placementSlot;
        // AR-B2: the model-driven updateShape reaction the prefilter above
        // just turned on for Stairs/Door — a per-model slot, not per-block,
        // since every stair species shares the same derivation.
        if (entry.prefilter.has(BlockBehaviorBit::HasNeighborReaction)) {
            const auto model = world::blockDefinition(static_cast<world::Block>(i)).model;
            if (model == world::BlockModel::Stairs) {
                entry.updateShape = &stairUpdateShapeSlot;
            } else if (model == world::BlockModel::Door) {
                entry.updateShape = &doorUpdateShapeSlot;
            } else if (model == world::BlockModel::Wall) {
                entry.updateShape = &wallUpdateShapeSlot;
            } else if (model == world::BlockModel::FenceGate) {
                entry.updateShape = &fenceGateUpdateShapeSlot;
            } else if (world::blockDefinition(static_cast<world::Block>(i))
                           .states.has(world::StateProperty::Locked)) {
                entry.updateShape = &repeaterLockedUpdateShapeSlot;
            }
        }
        // W-8/W-9: the onPlace slot's users. The diodes are keyed on
        // redstone::isDiode rather than the model, which they share with the
        // lever and the anvil; dust and the pistons are named outright.
        if (redstone::isDiode(static_cast<world::Block>(i))) {
            entry.onPlace = &diodePlacedSlot;
        } else if (rechecksRedstoneOnPlacement(static_cast<world::Block>(i))) {
            entry.onPlace = &redstoneRecheckPlacedSlot;
        }
        if (entry.prefilter.has(BlockBehaviorBit::IsSignalSource)) {
            // The block's own weak/strong emission handler (redstone::weakPowerFn),
            // so the slot is per-block rather than one shared function with a
            // switch inside — the same shape getDrops takes.
            const auto block = static_cast<world::Block>(i);
            entry.getWeakPower = redstone::weakPowerFn(block);
            entry.getStrongPower = redstone::strongPowerFn(block);
        }
    }
    return table;
}

} // namespace detail

// The process-wide behaviour table, built once on first use from the frozen
// registry. Header-only lazy singleton, like blockRegistry().
[[nodiscard]] inline const std::vector<BlockBehavior>& blockBehaviorTable() {
    static const std::vector<BlockBehavior> table = detail::buildBlockBehaviorTable();
    return table;
}

// The behaviour of one block. An id past the table (never happens for a valid
// registry id, but a defensive guard against a stale or invalid id) reads as the
// empty behaviour rather than out of bounds.
[[nodiscard]] inline const BlockBehavior& behaviorFor(core::BlockId id) {
    static const BlockBehavior empty{};
    const auto& table = blockBehaviorTable();
    const auto index = id.index();
    return index < table.size() ? table[index] : empty;
}

// The pre-filter query the hot loops use: one masked load, no slot touched.
[[nodiscard]] inline bool blockHasBehavior(core::BlockId id, BlockBehaviorBit bit) {
    return behaviorFor(id).prefilter.has(bit);
}

// Fetches a behaviour slot, or nullptr if the block has none — the generic
// half of dispatch. The caller gates on the pre-filter bit, then invokes the
// returned pointer (see dispatchBlockDrops for the pattern).
template <class Fn>
[[nodiscard]] Fn behaviorSlot(core::BlockId id, Fn BlockBehavior::* slot) {
    return behaviorFor(id).*slot;
}

// Block -> drops, through the table and the pre-filter instead of a switch. A
// block whose HasDrops bit is clear has no getDrops slot and yields nothing
// without a call; otherwise the tool-adequacy gate (the same canHarvestBlock
// minedDrops applies) runs, then the block's own drop handler produces the loot.
// This is behaviour-identical to calling minedDrops directly, which is what the
// parity harness asserts across every block and tool.
[[nodiscard]] inline MinedDrops dispatchBlockDrops(core::BlockId id, const ItemStack& tool,
                                                   std::uint64_t& randomState, int age = 0,
                                                   bool doubledSlab = false) {
    const auto& behavior = behaviorFor(id);
    if (!behavior.prefilter.has(BlockBehaviorBit::HasDrops) || behavior.getDrops == nullptr) {
        return {};
    }
    const auto block = world::blockFromId(id);
    if (!canHarvestBlock(block, tool)) {
        return {};
    }
    return behavior.getDrops(block, tool, randomState, age, doubledSlab);
}

// A block's base shape, through the table's getShape slot. This is the uniform
// behaviour surface (and the parity harness' subject): it produces exactly what
// world::blockShape does for a built-in, and an empty shape for a block with no
// slot. The hot mesher/raycast/collision paths keep calling the constexpr
// world::blockShape directly — routing them through this runtime table would cost
// the allocation-backed lookup and lose constexpr, and blockShape is already the
// single source this slot points at (B-DESIGN §2: getShape "已 BlockShape").
[[nodiscard]] inline world::BlockShape dispatchBlockShape(core::BlockId id,
                                                          world::BlockState state) {
    const auto slot = behaviorFor(id).getShape;
    return slot != nullptr ? slot(state) : world::BlockShape{};
}

// The state a placement resolves to, through the table's getStateForPlacement
// slot, or nullopt for a block with no slot. Behaviour-identical to calling
// world::placementBlock directly (the slot forwards to it); the switch this
// replaced was the block's support switch inside canBlockSurvive, now a table.
[[nodiscard]] inline std::optional<world::BlockState>
dispatchStateForPlacement(core::BlockId id, const PlacementBehaviorContext& context) {
    const auto slot = behaviorFor(id).getStateForPlacement;
    return slot != nullptr ? slot(context) : std::nullopt;
}

// Runs one updateShape derivation under the A3b contract, the single place that
// contract is enforced so every fence/wire/stair/door derivation obeys it by
// construction:
//
//   * **Pure property rewrite** — the result must be the same block. A
//     derivation that changed the block kind would be a placement or a break
//     wearing updateShape's clothes; it is refused (treated as no-change) rather
//     than allowed to corrupt the cell — updateShape may not create or destroy.
//   * **Idempotent, converging to a fixed point** — a derivation that returns
//     the state unchanged means "settled", reported as nullopt so a re-
//     notification writes nothing and the fan-out cannot churn. This is what
//     makes repeatedly notifying a stable connection free.
//
// Returns the new state only when it genuinely differs and is the same block;
// nullopt otherwise.
[[nodiscard]] inline std::optional<world::BlockState>
applyUpdateShapeContract(UpdateShapeFn slot, const NeighborUpdateContext& context) {
    if (slot == nullptr) {
        return std::nullopt;
    }
    const std::optional<world::BlockState> result = slot(context);
    if (!result.has_value()) {
        return std::nullopt;
    }
    // A3b: updateShape only rewrites properties, never the block itself. A
    // kind-changing result is a bug in the derivation; refuse it.
    if (result->block() != context.state.block()) {
        return std::nullopt;
    }
    // Fixed point: an unchanged result is "no change", not a write.
    if (*result == context.state) {
        return std::nullopt;
    }
    return result;
}

// The neighbour's new shape state, through the behaviour table's updateShape
// slot — or nullopt when the block cannot react to a neighbour at all. The
// HasNeighborReaction pre-filter is the whole point: stone, dirt, ore and the
// other overwhelming majority test one bit and are rejected before the slot is
// ever fetched or a context examined, the cost Java pays a virtual call for on
// every neighbour of every block even though almost none override updateShape.
[[nodiscard]] inline std::optional<world::BlockState>
dispatchUpdateShape(const BlockBehavior& behavior, const NeighborUpdateContext& context) {
    if (!behavior.prefilter.has(BlockBehaviorBit::HasNeighborReaction)) {
        return std::nullopt;
    }
    return applyUpdateShapeContract(behavior.updateShape, context);
}

[[nodiscard]] inline std::optional<world::BlockState>
dispatchUpdateShape(core::BlockId id, const NeighborUpdateContext& context) {
    return dispatchUpdateShape(behaviorFor(id), context);
}

// W-8: run a block's placement behaviour, if it has one. A null slot is the
// overwhelming majority and costs one predictable branch.
inline void dispatchOnPlace(core::BlockId id, const BlockLifecycleContext& context) {
    const BlockBehavior& behavior = behaviorFor(id);
    if (behavior.onPlace != nullptr) {
        behavior.onPlace(context);
    }
}

} // namespace mc::gameplay
