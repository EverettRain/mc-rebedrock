#include "gameplay/FurnaceSystem.hpp"
#include "gameplay/Item.hpp"

#include <cassert>
#include <cstddef>

// FurnaceSystem replaced a single global furnace that lived on CraftingSystem:
// every furnace in the world shared one inventory, and none of it was saved. The
// promises pinned here are exactly the two that fixes — a furnace is its own
// block entity (independent contents, keyed by position) and it smelts on its
// own — plus the resume-after-load behaviour the save path depends on.
int main() {
    using namespace mc::gameplay;
    using mc::world::Block;

    const auto ironOre = [] {
        return ItemStack{Block::IronOre, 1U, blockItemFor(Block::IronOre)};
    };
    const auto coal = [] { return ItemStack{Block::Air, 1U, &items::Coal}; };

    // --- Two furnaces do not share an inventory. ---
    // The global furnace's defining bug: fuel dropped into one furnace appeared
    // in every furnace. Keyed block entities cannot do that.
    {
        FurnaceSystem furnaces;
        assert(furnaces.place({0, 64, 0}));
        assert(furnaces.place({5, 64, 0}));
        assert(!furnaces.place({0, 64, 0})); // already there

        furnaces.find({0, 64, 0})->fuel = coal();
        assert(furnaces.find({0, 64, 0})->fuel.count == 1U);
        assert(furnaces.find({5, 64, 0})->fuel.empty()); // the other furnace is untouched
    }

    // --- A furnace smelts its input, consuming fuel and time. ---
    {
        FurnaceSystem furnaces;
        assert(furnaces.place({0, 64, 0}));
        auto& furnace = *furnaces.find({0, 64, 0});
        furnace.input = ironOre();
        furnace.fuel = coal();

        // The first tick lights the furnace: fuel is spent to start a burn.
        furnaces.tick();
        assert(furnace.burning());
        assert(furnace.fuel.empty()); // one coal consumed to begin
        assert(furnaces.fuelProgress({0, 64, 0}) > 0.0F);

        // Iron takes 200 ticks; after the first it is partway there.
        assert(furnaces.cookProgress({0, 64, 0}) > 0.0F);
        assert(furnaces.cookProgress({0, 64, 0}) < 1.0F);
        assert(furnace.output.empty());

        // Drive the remaining ticks; one iron ingot comes out and the ore is
        // spent. (One tick already ran, so 199 more complete the 200.)
        for (int i = 0; i < 199; ++i) {
            furnaces.tick();
        }
        assert(furnace.input.empty());
        assert(furnace.output.item == &items::IronIngot);
        assert(furnace.output.count == 1U);
        assert(furnaces.cookProgress({0, 64, 0}) == 0.0F); // reset for the next item
    }

    // --- An idle furnace never lights and never consumes fuel. ---
    {
        FurnaceSystem furnaces;
        assert(furnaces.place({0, 64, 0}));
        auto& furnace = *furnaces.find({0, 64, 0});
        furnace.fuel = coal(); // fuel but nothing to smelt
        furnaces.tick();
        assert(!furnace.burning());
        assert(furnace.fuel.count == 1U); // fuel is only spent for a real smelt
    }

    // --- remove() hands back the contents a broken furnace scatters. ---
    {
        FurnaceSystem furnaces;
        assert(furnaces.place({2, 64, 2}));
        furnaces.find({2, 64, 2})->input = ironOre();
        furnaces.find({2, 64, 2})->fuel = coal();
        const auto removed = furnaces.remove({2, 64, 2});
        assert(removed.has_value());
        assert(removed->input.block == Block::IronOre);
        assert(removed->fuel.item == &items::Coal);
        assert(furnaces.find({2, 64, 2}) == nullptr); // and it is gone
        assert(!furnaces.remove({2, 64, 2}).has_value());
    }

    // --- A restored furnace resumes its smelt rather than restarting it. ---
    // The recipe cache is a string_view into static data and is not saved, so
    // restore() re-points it from the input. Without that, the first tick after
    // a load would see "the recipe changed" and reset cook progress to zero,
    // silently undoing minutes of smelting on every save/load.
    {
        FurnaceSystem live;
        assert(live.place({0, 64, 0}));
        auto& furnace = *live.find({0, 64, 0});
        furnace.input = ironOre();
        furnace.fuel = coal();
        for (int i = 0; i < 50; ++i) {
            live.tick();
        }
        const int savedCookTicks = live.find({0, 64, 0})->cookTicks;
        assert(savedCookTicks > 0);

        // Round-trip through a value copy, as the save format does (it stores the
        // counters but not the string_view cache).
        std::vector<FurnaceBlockEntity> persisted(live.entities().begin(), live.entities().end());
        for (auto& entity : persisted) {
            entity.activeRecipe = {}; // what a fresh load hands restore()
        }
        FurnaceSystem loaded;
        loaded.restore(std::move(persisted));
        loaded.tick();
        // Progress continued from where it was, not from zero.
        assert(loaded.find({0, 64, 0})->cookTicks == savedCookTicks + 1);
    }

    return 0;
}
