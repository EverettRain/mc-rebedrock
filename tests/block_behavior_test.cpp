// B1-1/B1-2 — the block behaviour table, its pre-filter, and the shape/placement
// slots B1-2 fills.
//
// This is the infrastructure R1 (B-block) retires switches on: a table indexed
// by BlockId with fn-ptr slots and a pre-filter bitset. B1-1 built it and wired
// getDrops; B1-2 fills getShape and getStateForPlacement, pointing them at the
// single-source free functions in place of the model/support switches those
// functions used to hold, and this harness shows the table dispatches exactly
// what the free functions do.
//
// What this pins:
//   1. the pre-filter is consistent — the constexpr baked table, the runtime
//      table, and each bit's source (collision/interaction/random-tick/support/
//      drops) all agree, for every built-in block;
//   2. HasDrops is locked to the real drops behaviour: the bit is set iff
//      minedDrops can actually yield loot (probed across seeds/ages/tools);
//   3. dispatchBlockDrops is behaviour-identical to calling minedDrops — the
//      table + pre-filter path produces the same MinedDrops as the switch, for
//      every block and tool;
//   4. dispatchBlockShape is behaviour-identical to world::blockShape across
//      every built-in and a spread of states (slab halves, wall-torch facings,
//      crop ages), proving the getShape slot is the single shape source;
//   5. dispatchStateForPlacement is behaviour-identical to world::placementBlock,
//      proving the getStateForPlacement slot routes to the single placement
//      source (whose canSurvive switch B1-2 replaced with a table);
//   6. the dispatch mechanism itself — a null slot skips, the generic slot fetch
//      returns the wired pointer, and the slots later tasks own are still null;
//   7. the table is sized to the registry, not a constant, so it grows with
//      external blocks (R0-5) instead of dropping or overflowing them.

#include "gameplay/BlockBehavior.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/MiningSystem.hpp"
#include "gameplay/WorldSimulation.hpp"
#include "world/Block.hpp"
#include "world/BlockPlacement.hpp"
#include "world/BlockRegistry.hpp"
#include "world/BlockShape.hpp"
#include "world/BlockState.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace {

using mc::core::BlockId;
using mc::gameplay::BlockBehavior;
using mc::gameplay::BlockBehaviorBit;
using mc::gameplay::behaviorFor;
using mc::gameplay::behaviorSlot;
using mc::gameplay::blockBehaviorPrefilterFor;
using mc::gameplay::blockBehaviorTable;
using mc::gameplay::blockYieldsLoot;
using mc::gameplay::dispatchBlockDrops;
using mc::gameplay::dispatchBlockShape;
using mc::gameplay::dispatchStateForPlacement;
using mc::gameplay::ItemStack;
using mc::gameplay::PlacementBehaviorContext;
using mc::gameplay::kBuiltinBlockBehaviorPrefilter;
using mc::gameplay::minedDrops;
using mc::gameplay::MinedDrops;
using mc::gameplay::WorldSimulation;
using mc::world::Block;

// A datapack block this build does not define, standing in for one a loader
// parsed. It borrows Stone's shape and only swaps the identity — enough to grow
// the registry past the built-ins. Mirrors block_registry_lifecycle_test.
[[nodiscard]] mc::world::ExternalBlockDef makeExternalBlock(std::string_view path) {
    mc::world::ExternalBlockDef def{mc::world::blockDefinition(Block::Stone)};
    def.definition.identifier.space = "testmod";
    def.definition.identifier.path = path;
    def.definition.vanilla = {};
    return def;
}

// Two MinedDrops are equal iff they carry the same items in the same order.
[[nodiscard]] bool sameDrops(const MinedDrops& a, const MinedDrops& b) {
    if (a.count != b.count) return false;
    for (std::size_t i = 0; i < a.count; ++i) {
        if (a.entries[i].block != b.entries[i].block) return false;
        if (a.entries[i].count != b.entries[i].count) return false;
        if (a.entries[i].item != b.entries[i].item) return false;
    }
    return true;
}

// Two shapes are equal iff their kind, column span and box set match.
[[nodiscard]] bool sameShape(const mc::world::BlockShape& a, const mc::world::BlockShape& b) {
    if (a.kind != b.kind) return false;
    if (a.bottom != b.bottom || a.top != b.top) return false;
    if (a.boxes.size() != b.boxes.size()) return false;
    for (std::size_t i = 0; i < a.boxes.size(); ++i) {
        const auto& x = a.boxes[i];
        const auto& y = b.boxes[i];
        if (x.minX != y.minX || x.minY != y.minY || x.minZ != y.minZ) return false;
        if (x.maxX != y.maxX || x.maxY != y.maxY || x.maxZ != y.maxZ) return false;
    }
    return true;
}

// A stone floor so ground/soil/wall-supported blocks have something to survive
// on when the placement dispatch is probed, mirroring player_interaction_test.
void buildFloor(mc::world::World& world) {
    mc::world::Chunk chunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            chunk.setBlock(x, 0, z, Block::Stone);
        }
    }
    world.setChunk({0, 0}, std::move(chunk));
}

// The pre-filter agrees with itself (baked vs runtime) and with every bit's
// independent source, for every built-in block.
void testPrefilterParity() {
    for (std::size_t i = 0; i < mc::world::kBuiltinBlockCount; ++i) {
        const auto block = static_cast<Block>(i);
        const auto id = mc::world::blockId(block);
        const auto& definition = mc::world::blockDefinition(block);

        const auto baked = kBuiltinBlockBehaviorPrefilter[i];
        const auto runtime = behaviorFor(id).prefilter;
        assert(baked == runtime);
        assert(baked == blockBehaviorPrefilterFor(block));

        assert(runtime.has(BlockBehaviorBit::HasCollision) == definition.collision);
        assert(runtime.has(BlockBehaviorBit::HasInteraction) ==
               (definition.container != mc::world::ContainerType::None));
        assert(runtime.has(BlockBehaviorBit::HasNeighborReaction) ==
               (definition.support != mc::world::BlockSupport::None));
        assert(runtime.has(BlockBehaviorBit::HasRandomTick) ==
               WorldSimulation::isRandomlyTicking(block));
        assert(runtime.has(BlockBehaviorBit::HasDrops) == blockYieldsLoot(block));
        // Redstone lands in W-4; nothing is a signal source yet.
        assert(!runtime.has(BlockBehaviorBit::IsSignalSource));
    }
}

// The HasDrops bit is locked to the actual drops behaviour, not merely derived:
// probe the real minedDrops with a tool that harvests everything, across seeds,
// ages and the double-slab flag, and confirm the bit is set iff loot can appear.
void testHasDropsLockedToBehaviour() {
    const ItemStack diamondPickaxe{Block::Air, 1U, &mc::gameplay::items::DiamondPickaxe};
    for (std::size_t i = 0; i < mc::world::kBuiltinBlockCount; ++i) {
        const auto block = static_cast<Block>(i);
        const auto id = mc::world::blockId(block);

        bool canDrop = false;
        for (std::uint32_t seed = 1U; seed <= 4096U && !canDrop; ++seed) {
            for (const int age : {0, 7}) {
                for (const bool doubledSlab : {false, true}) {
                    std::uint32_t state = seed;
                    if (!minedDrops(block, diamondPickaxe, state, age, doubledSlab).empty()) {
                        canDrop = true;
                    }
                }
            }
        }
        assert(behaviorFor(id).prefilter.has(BlockBehaviorBit::HasDrops) == canDrop);
    }
}

// Dispatching drops through the table + pre-filter yields exactly what calling
// minedDrops directly does, for every built-in block and a spread of tools,
// ages and slab states — the "table == switch" equivalence, in parallel with
// the still-present switch.
void testDropsDispatchEqualsSwitch() {
    const std::array<ItemStack, 3> tools{
        ItemStack{},                                                   // bare hand
        ItemStack{Block::Air, 1U, &mc::gameplay::items::WoodenPickaxe}, // weak tool
        ItemStack{Block::Air, 1U, &mc::gameplay::items::DiamondPickaxe} // harvests all
    };
    for (std::size_t i = 0; i < mc::world::kBuiltinBlockCount; ++i) {
        const auto block = static_cast<Block>(i);
        const auto id = mc::world::blockId(block);
        for (const auto& tool : tools) {
            for (const int age : {0, 3, 7}) {
                for (const bool doubledSlab : {false, true}) {
                    for (std::uint32_t seed = 1U; seed <= 64U; ++seed) {
                        std::uint32_t viaSwitch = seed;
                        std::uint32_t viaTable = seed;
                        const auto expected =
                            minedDrops(block, tool, viaSwitch, age, doubledSlab);
                        const auto actual =
                            dispatchBlockDrops(id, tool, viaTable, age, doubledSlab);
                        assert(sameDrops(expected, actual));
                        // A chance-based table must advance the RNG identically,
                        // or the next block's roll would diverge.
                        assert(viaSwitch == viaTable);
                    }
                }
            }
        }
    }
}

// The dispatch mechanism: a clear pre-filter means a null slot and no call; a
// set one exposes the wired pointer through the generic fetch; and the slots the
// migration tasks own are still null everywhere (B1-1 fills only getDrops).
void testDispatchMechanism() {
    const ItemStack diamondPickaxe{Block::Air, 1U, &mc::gameplay::items::DiamondPickaxe};

    // Glass is harvestable but drops nothing (silk only): bit clear, slot null.
    const auto glass = mc::world::blockId(Block::Glass);
    assert(!behaviorFor(glass).prefilter.has(BlockBehaviorBit::HasDrops));
    assert(behaviorFor(glass).getDrops == nullptr);
    std::uint32_t glassSeed = 1U;
    assert(dispatchBlockDrops(glass, diamondPickaxe, glassSeed).empty());

    // Stone drops: bit set, slot wired to the shared minedDrops.
    const auto stone = mc::world::blockId(Block::Stone);
    assert(behaviorFor(stone).prefilter.has(BlockBehaviorBit::HasDrops));
    assert(behaviorFor(stone).getDrops == &minedDrops);
    assert(behaviorSlot(stone, &BlockBehavior::getDrops) == &minedDrops);

    // B1-2 wired getShape and getStateForPlacement for every built-in: getShape
    // to the single-source world::blockShape, getStateForPlacement to a forwarder
    // (probed for behaviour below, since the forwarder is not itself public).
    for (std::size_t i = 0; i < mc::world::kBuiltinBlockCount; ++i) {
        const auto& behavior = behaviorFor(mc::world::blockId(static_cast<Block>(i)));
        assert(behavior.getShape == &mc::world::blockShape);
        assert(behavior.getStateForPlacement != nullptr);
    }

    // The slots later tasks own are null for every block.
    for (std::size_t i = 0; i < mc::world::kBuiltinBlockCount; ++i) {
        const auto& behavior = behaviorFor(mc::world::blockId(static_cast<Block>(i)));
        assert(behavior.useItemOn == nullptr);
        assert(behavior.updateShape == nullptr);
        assert(behavior.onPlace == nullptr);
        assert(behavior.onRemove == nullptr);
    }
}

// Dispatching a block's shape through the getShape slot yields exactly what
// world::blockShape does, for every built-in and a spread of states: the slab
// halves, the wall-torch facings and the crop ages that make the shape depend on
// state, not just identity. This is the "table == single source" equivalence for
// the shape the mesher, pick ray and collision all derive from.
void testShapeDispatchEqualsSource() {
    using mc::world::BlockOrientation;
    using mc::world::BlockState;
    using mc::world::blockShape;
    using mc::world::SlabPortion;

    for (std::size_t i = 0; i < mc::world::kBuiltinBlockCount; ++i) {
        const auto block = static_cast<Block>(i);
        const auto id = mc::world::blockId(block);

        const BlockState base{block};
        assert(sameShape(dispatchBlockShape(id, base), blockShape(base)));

        if (mc::world::isSlab(block)) {
            for (const auto portion : {SlabPortion::Bottom, SlabPortion::Top, SlabPortion::Double}) {
                const auto state = base.withSlabPortion(portion);
                assert(sameShape(dispatchBlockShape(id, state), blockShape(state)));
            }
        }
        if (block == Block::WallTorch) {
            for (const auto facing : {BlockOrientation::North, BlockOrientation::East,
                                      BlockOrientation::South, BlockOrientation::West}) {
                const BlockState state{block, facing};
                assert(sameShape(dispatchBlockShape(id, state), blockShape(state)));
            }
        }
        if (mc::world::blockDefinition(block).model == mc::world::BlockModel::Crop) {
            for (int age = 0; age <= 7; ++age) {
                const auto state = base.withAge(age);
                assert(sameShape(dispatchBlockShape(id, state), blockShape(state)));
            }
        }
    }
}

// Dispatching placement through the getStateForPlacement slot yields exactly what
// world::placementBlock does, on a real world with a stone floor. Probed across
// every built-in and a spread of clicked faces so blocks that survive only in
// some orientations (a torch, a wall torch, a crop over farmland) exercise both
// the survives and the nullopt branch — the block's support switch B1-2 turned
// into a table lives on the direct side of this equivalence.
void testPlacementDispatchEqualsSource() {
    using mc::world::BlockOrientation;
    using mc::world::PlacementContext;

    mc::world::World world;
    buildFloor(world);
    world.setBlock(5, 1, 5, Block::Farmland); // a tilled cell so crops can survive

    const std::array<BlockOrientation, 3> faces{BlockOrientation::Up, BlockOrientation::North,
                                                BlockOrientation::Down};
    for (std::size_t i = 0; i < mc::world::kBuiltinBlockCount; ++i) {
        const auto block = static_cast<Block>(i);
        const auto id = mc::world::blockId(block);
        for (const auto face : faces) {
            PlacementContext context;
            context.clickedBlock = glm::ivec3{5, 1, 5};
            context.placePosition = glm::ivec3{5, 2, 5};
            context.clickedFace = face;
            context.hitPosition = glm::vec3{5.5F, 2.7F, 5.5F};
            context.lookDirection = glm::vec3{0.0F, 0.0F, -1.0F};

            const PlacementBehaviorContext behaviorContext{world, block, context};
            const auto viaTable = dispatchStateForPlacement(id, behaviorContext);
            const auto viaDirect = mc::world::placementBlock(world, block, context);
            assert(viaTable.has_value() == viaDirect.has_value());
            if (viaTable.has_value()) {
                assert(*viaTable == *viaDirect);
            }
        }
    }
}

// The table is sized to the registry, so external blocks (R0-5) get an entry
// instead of being dropped or overflowing a constant-sized table. Their entries
// are empty until data-driven behaviour attaches (D).
void testTableSizedToRegistry() {
    assert(blockBehaviorTable().size() == mc::world::blockCount());
    assert(mc::world::blockCount() > mc::world::kBuiltinBlockCount);  // externals were added

    for (std::size_t i = mc::world::kBuiltinBlockCount; i < mc::world::blockCount(); ++i) {
        const auto id = BlockId::of(static_cast<BlockId::Value>(i));
        const auto& behavior = behaviorFor(id);  // in bounds, no overflow
        assert(behavior.getDrops == nullptr);
        assert(behavior.getShape == nullptr);
        assert(behavior.getStateForPlacement == nullptr);
        assert(behavior.prefilter.bits == 0U);
        // A block with no getShape slot dispatches to an empty shape rather than
        // reading past the table.
        assert(dispatchBlockShape(id, mc::world::BlockState{}).kind ==
               mc::world::ShapeKind::Empty);
    }
}

}  // namespace

int main() {
    // Register external content *before* the registry or behaviour table is
    // first built, so both include it — the way a datapack loader would.
    mc::world::externalBlockDefs().push_back(makeExternalBlock("widget"));
    mc::world::externalBlockDefs().push_back(makeExternalBlock("gadget"));

    testPrefilterParity();
    testHasDropsLockedToBehaviour();
    testDropsDispatchEqualsSwitch();
    testShapeDispatchEqualsSource();
    testPlacementDispatchEqualsSource();
    testDispatchMechanism();
    testTableSizedToRegistry();
    return 0;
}
