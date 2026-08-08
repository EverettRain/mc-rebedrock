#include "gameplay/ContentRegistry.hpp"

#include <algorithm>
#include <cassert>

int main() {
    using namespace mc;
    using namespace mc::gameplay;

    const auto& registry = contentRegistry();
    // Blocks and items are filed under this project's namespace, and reachable
    // through the vanilla alias or the bare name as well.
    assert(registry.block("rebedrock:chest") != nullptr);
    assert(registry.block("minecraft:chest") == registry.block("rebedrock:chest"));
    assert(registry.block("chest") == registry.block("rebedrock:chest"));
    assert(registry.block("rebedrock:lapis_ore") != nullptr);
    assert(registry.item("rebedrock:book") != nullptr);
    assert(registry.item("minecraft:book") == registry.item("rebedrock:book"));
    assert(registry.item("rebedrock:diamond_pickaxe") != nullptr);
    assert(registry.item("minecraft:golden_pickaxe") == registry.item("rebedrock:golden_pickaxe"));
    assert(registry.block("rebedrock:not_registered") == nullptr);
    assert(registry.item("rebedrock:not_registered") == nullptr);
    // A block that exists but was never given a creative category stays unlisted.
    assert(registry.block("rebedrock:water") == nullptr);

    const auto functional = registry.catalog(CreativeCategory::Functional);
    assert(std::ranges::any_of(functional, [](const ItemStack& stack) {
        return stack.block == world::Block::Chest;
    }));
    const auto food = registry.catalog(CreativeCategory::Food);
    assert(std::ranges::any_of(food, [](const ItemStack& stack) {
        return stack.item == &items::Apple;
    }));

    ContentRegistry isolated;
    assert(isolated.registerBlock(world::Block::Stone,
                                  CreativeCategory::BuildingBlocks));
    assert(!isolated.registerBlock(world::Block::Stone,
                                   CreativeCategory::Decoration));
    assert(!isolated.registerBlock(world::Block::Air,
                                   CreativeCategory::BuildingBlocks));
    assert(isolated.registerItem(&items::Book, CreativeCategory::Materials));
    assert(!isolated.registerItem(&items::Book, CreativeCategory::Materials));
    assert(!isolated.registerItem(nullptr, CreativeCategory::Materials));
}
