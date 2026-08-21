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
#include "gameplay/WorldSimulation.hpp" // WorldSimulation::isRandomlyTicking
#include "world/Block.hpp"
#include "world/BlockPlacement.hpp" // PlacementContext, placementBlock (World fwd)
#include "world/BlockPos.hpp"       // BlockPos
#include "world/BlockRegistry.hpp"  // blockCount
#include "world/BlockShape.hpp"     // BlockShape, blockShape
#include "world/BlockState.hpp"

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
    prefilter.set(BlockBehaviorBit::HasNeighborReaction,
                  definition.support != world::BlockSupport::None);
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
struct BlockLifecycleContext;      // onPlace / onRemove owner

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
    MinedDrops (*)(world::Block, const ItemStack&, std::uint32_t&, int, bool);
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
    BlockLifecycleFn onPlace = nullptr;                     // lifecycle
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
                                                   std::uint32_t& randomState, int age = 0,
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

} // namespace mc::gameplay
