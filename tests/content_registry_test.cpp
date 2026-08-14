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
    assert(registry.item("rebedrock:lava_bucket") != nullptr);
    assert(registry.item("minecraft:lava_bucket") == registry.item("rebedrock:lava_bucket"));
    assert(registry.item("minecraft:golden_pickaxe") == registry.item("rebedrock:golden_pickaxe"));
    // Description ids are derived from registry identities, not baked English
    // or Chinese names. Vanilla aliases use minecraft keys; original content
    // stays in the project's language namespace.
    assert(encodeDescriptionId(items::Apple.descriptionId()) == "item.minecraft.apple");
    const auto customItem = Item::of("test_widget").custom();
    assert(encodeDescriptionId(customItem.descriptionId()) == "item.rebedrock.test_widget");
    const Item* stoneItem = blockItemFor(world::Block::Stone);
    assert(stoneItem != nullptr);
    assert(encodeDescriptionId(stoneItem->descriptionId()) == "block.minecraft.stone");
    assert(registry.block("rebedrock:not_registered") == nullptr);
    assert(registry.item("rebedrock:not_registered") == nullptr);
    // A block that exists but was never given a creative category stays unlisted.
    assert(registry.block("rebedrock:water") == nullptr);

    // The decorative stone variants are registered and reachable by the vanilla
    // alias and the bare name as well.
    assert(registry.block("rebedrock:polished_granite") != nullptr);
    assert(registry.block("rebedrock:polished_diorite") != nullptr);
    assert(registry.block("rebedrock:polished_andesite") != nullptr);
    assert(registry.block("rebedrock:smooth_stone") != nullptr);
    assert(registry.block("minecraft:polished_diorite") ==
           registry.block("rebedrock:polished_diorite"));
    assert(registry.block("polished_diorite") == registry.block("rebedrock:polished_diorite"));

    const auto functional = registry.catalog(CreativeCategory::Functional);
    assert(std::ranges::any_of(functional, [](const ItemStack& stack) {
        return stack.block == world::Block::Chest;
    }));
    const auto building = registry.catalog(CreativeCategory::BuildingBlocks);
    assert(std::ranges::any_of(building, [](const ItemStack& stack) {
        return stack.block == world::Block::PolishedDiorite;
    }));
    assert(std::ranges::any_of(building, [](const ItemStack& stack) {
        return stack.block == world::Block::SmoothStone;
    }));
    const auto food = registry.catalog(CreativeCategory::Food);
    assert(std::ranges::any_of(food, [](const ItemStack& stack) {
        return stack.item == &items::Apple;
    }));
    const auto materials = registry.catalog(CreativeCategory::Materials);
    assert(std::ranges::any_of(materials, [](const ItemStack& stack) {
        return stack.item == &items::LavaBucket;
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
