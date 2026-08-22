#include "gameplay/BlockEntityTicker.hpp"
#include "gameplay/ChestSystem.hpp"
#include "gameplay/FurnaceSystem.hpp"
#include "gameplay/Item.hpp"
#include "world/BlockEntityType.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <vector>

// BE2's ticker behaviour table: block entities advance through a fn-ptr table
// keyed by BlockEntityTypeId, with a null slot as the hot-path pre-filter that
// skips a whole type's container. What is pinned here is that the drive is
// behaviour-equivalent to the old hand-list of tick() calls (chest lids ease,
// furnaces burn), that the pre-filter really skips a slotless type, and that the
// drive order is a fixed array index — never a map's — so a multi-entity tick is
// reproducible.
int main() {
    using namespace mc::gameplay;
    using mc::world::Block;
    using mc::world::BlockEntityKind;

    const auto ironOre = [] {
        return ItemStack{Block::IronOre, 1U, blockItemFor(Block::IronOre)};
    };
    const auto coal = [] { return ItemStack{Block::Air, 1U, &items::Coal}; };

    // --- The pre-filter agrees with the table: every built-in kind ticks. ---
    {
        assert(hasTicker(mc::world::blockEntityTypeId(BlockEntityKind::Chest)));
        assert(hasTicker(mc::world::blockEntityTypeId(BlockEntityKind::TrappedChest)));
        assert(hasTicker(mc::world::blockEntityTypeId(BlockEntityKind::Furnace)));
        // Invalid id, and any id past the table, is tickless rather than a crash.
        assert(!hasTicker(mc::core::BlockEntityTypeId::invalid()));
        assert(!hasTicker(mc::core::BlockEntityTypeId::of(
            static_cast<mc::core::BlockEntityTypeId::Value>(kBlockEntityTickerTable.size()))));
    }

    // --- Driving the table steps a chest's lid exactly as ChestSystem::tick()
    //     did: the moved drive changed who calls tick(), not the lid maths. ---
    {
        ChestSystem chests;
        ChestSystem trapped;
        FurnaceSystem furnaces;
        assert(chests.place({0, 64, 0}));
        assert(chests.open({0, 64, 0}));

        const BlockEntityTickContext context{chests, trapped, furnaces};
        // One drive: the lid eases 0.1 toward the open target, the same step
        // ChestSystem::tick() applies.
        const auto stats = tickBlockEntities(context);
        // Every built-in kind has a ticker, so nothing was skipped this drive.
        assert(stats.ticked == 3);
        assert(stats.skipped == 0);
        const auto* chest = chests.find({0, 64, 0});
        assert(chest != nullptr);
        assert(chest->lidAngle > 0.09F && chest->lidAngle < 0.11F);

        // Nine more drives take the fully-open lid to 1.0 and pin it there.
        for (int i = 0; i < 9; ++i) {
            static_cast<void>(tickBlockEntities(context));
        }
        assert(chests.find({0, 64, 0})->lidAngle == 1.0F);
    }

    // --- The trapped chest ticks on its own container: its lid eases like a
    //     chest's, and the (empty) chest container beside it is untouched. This
    //     is BE3's reuse — the same ChestSystem tick, a distinct container. ---
    {
        ChestSystem chests;
        ChestSystem trapped;
        FurnaceSystem furnaces;
        assert(trapped.place({2, 64, 2}));
        assert(trapped.open({2, 64, 2}));

        const BlockEntityTickContext context{chests, trapped, furnaces};
        static_cast<void>(tickBlockEntities(context));
        assert(trapped.find({2, 64, 2})->lidAngle > 0.09F);
        // Nothing leaked into the chest container.
        assert(chests.entities().empty());
    }

    // --- Driving the table burns a furnace exactly as FurnaceSystem::tick()
    //     did, and does so on the same drive as the chest above — the two
    //     containers step in one call. ---
    {
        ChestSystem chests;
        ChestSystem trapped;
        FurnaceSystem furnaces;
        assert(furnaces.place({0, 64, 0}));
        auto& furnace = *furnaces.find({0, 64, 0});
        furnace.input = ironOre();
        furnace.fuel = coal();

        const BlockEntityTickContext context{chests, trapped, furnaces};
        static_cast<void>(tickBlockEntities(context));
        // The first drive lit the furnace (fuel spent, burn started) and made a
        // dent in the 200-tick smelt — precisely FurnaceSystem::tick()'s first
        // step.
        assert(furnace.burning());
        assert(furnace.fuel.empty());
        assert(furnaces.cookProgress({0, 64, 0}) > 0.0F);
        assert(furnaces.cookProgress({0, 64, 0}) < 1.0F);
    }

    // --- The drive is behaviour-equivalent to the old hand-list: the same
    //     initial state, stepped the same number of times, once through the
    //     table and once through the systems directly, lands identically. ---
    {
        const auto build = [&](FurnaceBlockEntity& out) {
            out.input = ironOre();
            out.fuel = coal();
        };
        ChestSystem chestsTable;
        ChestSystem trappedTable;
        FurnaceSystem furnacesTable;
        ChestSystem chestsDirect;
        FurnaceSystem furnacesDirect;
        for (ChestSystem* c : {&chestsTable, &chestsDirect}) {
            assert(c->place({1, 64, 1}));
            static_cast<void>(c->open({1, 64, 1}));
        }
        for (FurnaceSystem* f : {&furnacesTable, &furnacesDirect}) {
            assert(f->place({1, 64, 1}));
            build(*f->find({1, 64, 1}));
        }

        const BlockEntityTickContext tableContext{chestsTable, trappedTable, furnacesTable};
        for (int i = 0; i < 120; ++i) {
            static_cast<void>(tickBlockEntities(tableContext));
            // The hand-list this replaced: furnace then chest, called directly.
            furnacesDirect.tick();
            chestsDirect.tick();
        }
        const auto& tableFurnace = *furnacesTable.find({1, 64, 1});
        const auto& directFurnace = *furnacesDirect.find({1, 64, 1});
        assert(tableFurnace.cookTicks == directFurnace.cookTicks);
        assert(tableFurnace.burnTicks == directFurnace.burnTicks);
        assert(chestsTable.find({1, 64, 1})->lidAngle == chestsDirect.find({1, 64, 1})->lidAngle);
    }

    // --- The pre-filter is real: a table with the furnace slot punched out
    //     leaves the furnace container untouched — no burn, no smelt — and the
    //     stats report it stepped over. This is what makes a slotless block
    //     entity cost nothing per tick. ---
    {
        ChestSystem chests;
        ChestSystem trapped;
        FurnaceSystem furnaces;
        assert(furnaces.place({0, 64, 0}));
        auto& furnace = *furnaces.find({0, 64, 0});
        furnace.input = ironOre();
        furnace.fuel = coal();

        // A copy of the built-in table with the furnace ticker removed.
        std::array<BlockEntityTickerFn, mc::world::kBuiltinBlockEntityTypeCount> punched =
            kBlockEntityTickerTable;
        punched[mc::world::blockEntityTypeId(BlockEntityKind::Furnace).index()] = nullptr;

        const BlockEntityTickContext context{chests, trapped, furnaces};
        const auto stats = tickBlockEntities(context, punched);
        assert(stats.ticked == 2);  // the chest and trapped-chest slots ran
        assert(stats.skipped == 1); // the furnace slot was stepped over
        // The furnace never lit: its fuel is intact and no smelt began.
        assert(!furnace.burning());
        assert(furnace.fuel.count == 1U);
        assert(furnaces.cookProgress({0, 64, 0}) == 0.0F);
    }

    // --- Determinism: two furnaces stepped the same number of times from the
    //     same initial state end in the same state. The drive order is a fixed
    //     array index, so this holds regardless of allocation identity — the
    //     guard against ever iterating containers in a map's order. ---
    {
        const auto run = [&]() {
            ChestSystem chests;
            ChestSystem trapped;
            FurnaceSystem furnaces;
            static_cast<void>(furnaces.place({3, 64, 3}));
            auto& furnace = *furnaces.find({3, 64, 3});
            furnace.input = ironOre();
            furnace.fuel = coal();
            const BlockEntityTickContext context{chests, trapped, furnaces};
            for (int i = 0; i < 205; ++i) {
                static_cast<void>(tickBlockEntities(context));
            }
            return *furnaces.find({3, 64, 3});
        };
        const FurnaceBlockEntity first = run();
        const FurnaceBlockEntity second = run();
        assert(first.cookTicks == second.cookTicks);
        assert(first.burnTicks == second.burnTicks);
        assert(first.output.count == second.output.count);
        assert(first.output.item == second.output.item);
    }

    return 0;
}
