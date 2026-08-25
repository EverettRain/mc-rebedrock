#include "gameplay/ContentRegistry.hpp"
#include "gameplay/SpawnEggItems.hpp"
#include "gameplay/entities/EntityRegistry.hpp"

#include <algorithm>
#include <cassert>

int main() {
    using namespace mc;
    using namespace mc::gameplay;

    // Spawn eggs register into the creative catalog off the entity registry
    // (kSpawnEggItemsRegistered, SpawnEggItems.hpp); the species must exist
    // first, mirroring the app's own startup order.
    entities::registerBuiltinEntities();

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
    const auto food = registry.catalog(CreativeCategory::FoodAndDrink);
    assert(std::ranges::any_of(food, [](const ItemStack& stack) {
        return stack.item == &items::Apple;
    }));
    const auto materials = registry.catalog(CreativeCategory::Ingredients);
    assert(std::ranges::any_of(materials, [](const ItemStack& stack) {
        return stack.item == &items::LavaBucket;
    }));

    // --- AR-A1: the animal drop items land in Food (auto-catalogued off their
    // own .category(CreativeCategory::Food) declaration — no catalog-list edit
    // needed, per AR-CI). Egg used to be mis-tabbed under Materials; it now
    // joins the other drops here. ---
    assert(std::ranges::any_of(food, [](const ItemStack& stack) {
        return stack.item == &items::Mutton;
    }));
    assert(std::ranges::any_of(food, [](const ItemStack& stack) {
        return stack.item == &items::RawChicken;
    }));
    assert(std::ranges::any_of(food, [](const ItemStack& stack) {
        return stack.item == &items::Beef;
    }));
    assert(std::ranges::any_of(food, [](const ItemStack& stack) {
        return stack.item == &items::Egg;
    }));
    assert(!std::ranges::any_of(materials, [](const ItemStack& stack) {
        return stack.item == &items::Egg;
    }));

    // --- AR-M1: rotten flesh (zombie/husk drop) lands in the Food tab the
    // same auto-catalogued way, off its own .category(CreativeCategory::Food)
    // declaration. ---
    assert(std::ranges::any_of(food, [](const ItemStack& stack) {
        return stack.item == &items::RottenFlesh;
    }));
    assert(registry.item("rebedrock:rotten_flesh") != nullptr);
    assert(registry.item("minecraft:rotten_flesh") == registry.item("rebedrock:rotten_flesh"));

    // --- AR-A1: sheep/chicken spawn eggs reach the SpawnEggs tab, the same way
    // the pre-existing pig/cow/zombie eggs do — reachable via /give and via the
    // creative inventory even before natural spawning is mac-verified. ---
    const auto spawnEggs = registry.catalog(CreativeCategory::SpawnEggs);
    assert(std::ranges::any_of(spawnEggs, [](const ItemStack& stack) {
        return stack.item == &items::SheepSpawnEgg;
    }));
    assert(std::ranges::any_of(spawnEggs, [](const ItemStack& stack) {
        return stack.item == &items::ChickenSpawnEgg;
    }));
    assert(registry.item("rebedrock:sheep_spawn_egg") != nullptr);
    assert(registry.item("rebedrock:chicken_spawn_egg") != nullptr);
    assert(registry.item("minecraft:sheep_spawn_egg") == registry.item("rebedrock:sheep_spawn_egg"));

    // --- AR-M1: husk spawn egg reaches the SpawnEggs tab the same way. ---
    assert(std::ranges::any_of(spawnEggs, [](const ItemStack& stack) {
        return stack.item == &items::HuskSpawnEgg;
    }));
    assert(registry.item("rebedrock:husk_spawn_egg") != nullptr);
    assert(registry.item("minecraft:husk_spawn_egg") == registry.item("rebedrock:husk_spawn_egg"));

    ContentRegistry isolated;
    assert(isolated.registerBlock(world::Block::Stone,
                                  CreativeCategory::BuildingBlocks));
    assert(!isolated.registerBlock(world::Block::Stone,
                                   CreativeCategory::NaturalBlocks));
    assert(!isolated.registerBlock(world::Block::Air,
                                   CreativeCategory::BuildingBlocks));
    assert(isolated.registerItem(&items::Book, CreativeCategory::Ingredients));
    assert(!isolated.registerItem(&items::Book, CreativeCategory::Ingredients));
    assert(!isolated.registerItem(nullptr, CreativeCategory::Ingredients));
}
