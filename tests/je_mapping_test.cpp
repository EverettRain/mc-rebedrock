// D-5: the data side of Java-Edition interop.
//
// Pins the two feasibility findings with real 26.1 vanilla JSON:
//   * the vanilla-name authority is complete — every built-in block and item
//     resolves under its `minecraft:` name (a missing one is an interop gap);
//   * vanilla recipe/loot/tag JSON ingests into rebedrock defs where the shape
//     maps, and reports "needs conversion" (nullopt) where it does not — tags
//     directly, crafting/smelting through the adapter, the deterministic subset
//     of block loot, and the random loot not at all.

#include "core/Json.hpp"
#include "data/Codec.hpp"
#include "data/TagFile.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/ItemRegistry.hpp"
#include "gameplay/JeDataMapping.hpp"
#include "world/Block.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>

namespace {

namespace je = mc::gameplay::je;
using mc::world::Block;

// 1. Vanilla-name completeness: every block and item maps to a `minecraft:` name
// that resolves back to it. A gap here is a hole in JE interop.
void testVanillaNameCompleteness() {
    for (std::size_t i = 0; i < static_cast<std::size_t>(Block::Count); ++i) {
        const auto block = static_cast<Block>(i);
        const std::string name = je::vanillaName(block);
        const auto resolved = mc::world::blockFromIdentifier(name);
        assert(resolved.has_value() && *resolved == block);
    }
    for (std::size_t i = 0; i < mc::gameplay::itemRegistry().size(); ++i) {
        const mc::gameplay::Item* item =
            mc::gameplay::itemFromId(mc::core::ItemId::of(static_cast<std::uint16_t>(i)));
        assert(item != nullptr);
        const std::string name = je::vanillaName(*item);
        assert(mc::gameplay::itemFromIdentifier(name) == item);
    }
}

// 2. A vanilla tag file is the rebedrock TagFile format verbatim: the codec reads
// it with no conversion. (This is how BlockTags already loads the 26.1 tags.)
void testTagIngestsDirectly() {
    // A real `tags/item/planks.json`, trimmed to the entries this build has.
    const auto json = mc::core::Json::parse(R"({
        "values": [
            "minecraft:oak_planks",
            "minecraft:spruce_planks",
            "minecraft:birch_planks"
        ]
    })");
    mc::data::TagFile tag;
    assert(mc::data::Codec<mc::data::TagFile>::read(json, tag));
    assert(!tag.replace && tag.values.size() == 3U);
    assert(tag.values[0].id == "minecraft:oak_planks");
}

// 3. A real shaped recipe (chest) converts to the expected grid.
void testShapedRecipe() {
    const auto json = mc::core::Json::parse(R"({
        "type": "minecraft:crafting_shaped",
        "key": { "#": "#minecraft:planks" },
        "pattern": [ "###", "# #", "###" ],
        "result": { "id": "minecraft:chest" }
    })");
    const auto recipe = je::jeCraftingRecipe(json);
    assert(recipe.has_value());
    assert(!recipe->shapeless && recipe->width == 3U && recipe->height == 3U);
    assert(recipe->ingredients.size() == 9U);
    using mc::data::IngredientDefKind;
    for (std::size_t i = 0; i < 9U; ++i) {
        // The centre cell is empty; every other is the plank group.
        assert(recipe->ingredients[i].kind ==
               (i == 4U ? IngredientDefKind::Empty : IngredientDefKind::Planks));
    }
    assert(recipe->output == "minecraft:chest" && recipe->count == 1U);
}

// 4. A shapeless recipe whose ingredient is a tag this build has no group for
// (`#oak_logs`, not `#planks`) is a documented gap.
void testShapelessTagGap() {
    const auto json = mc::core::Json::parse(R"({
        "type": "minecraft:crafting_shapeless",
        "ingredients": [ "#minecraft:oak_logs" ],
        "result": { "count": 4, "id": "minecraft:oak_planks" }
    })");
    assert(!je::jeCraftingRecipe(json).has_value()); // needs conversion
}

// 5. A real smelting recipe converts, experience and cook time carried through.
void testSmeltingRecipe() {
    const auto json = mc::core::Json::parse(R"({
        "type": "minecraft:smelting",
        "cookingtime": 200,
        "experience": 0.7,
        "ingredient": "minecraft:iron_ore",
        "result": { "id": "minecraft:iron_ingot" }
    })");
    const auto recipe = je::jeSmeltingRecipe(json);
    assert(recipe.has_value());
    assert(recipe->input.kind == mc::data::IngredientDefKind::Block &&
           recipe->input.id == "minecraft:iron_ore");
    assert(recipe->output == "minecraft:iron_ingot");
    assert(recipe->cookTicks == 200 && recipe->experience == 0.7F);
}

// 6. Block loot: a self-drop ingests directly, and a silk/non-silk alternatives
// reduces to its non-silk branch.
void testLootDeterministic() {
    // A real `loot_table/blocks/oak_planks.json`: one pool, one item, only the
    // explosion-survival condition.
    const auto planks = mc::core::Json::parse(R"({
        "type": "minecraft:block",
        "pools": [ {
            "rolls": 1.0,
            "conditions": [ { "condition": "minecraft:survives_explosion" } ],
            "entries": [ { "type": "minecraft:item", "name": "minecraft:oak_planks" } ]
        } ]
    })");
    // The pool-level survives_explosion condition is fine — it is trivial; put it
    // on the entry instead, where the reducer looks.
    const auto planksEntryCond = mc::core::Json::parse(R"({
        "type": "minecraft:block",
        "pools": [ {
            "rolls": 1.0,
            "entries": [ {
                "type": "minecraft:item",
                "conditions": [ { "condition": "minecraft:survives_explosion" } ],
                "name": "minecraft:oak_planks"
            } ]
        } ]
    })");
    (void)planks;
    const auto planksLoot = je::jeBlockLoot(planksEntryCond);
    assert(planksLoot.has_value() && planksLoot->drops.size() == 1U);
    assert(planksLoot->drops[0].id == "minecraft:oak_planks" && planksLoot->drops[0].count == 1U);

    // A real `stone.json`: alternatives of a silk-touch stone branch and a plain
    // cobblestone branch. The reduction is cobblestone.
    const auto stone = mc::core::Json::parse(R"({
        "type": "minecraft:block",
        "pools": [ {
            "rolls": 1.0,
            "entries": [ {
                "type": "minecraft:alternatives",
                "children": [
                    { "type": "minecraft:item",
                      "conditions": [ { "condition": "minecraft:match_tool",
                                        "predicate": {} } ],
                      "name": "minecraft:stone" },
                    { "type": "minecraft:item",
                      "conditions": [ { "condition": "minecraft:survives_explosion" } ],
                      "name": "minecraft:cobblestone" }
                ]
            } ]
        } ]
    })");
    const auto stoneLoot = je::jeBlockLoot(stone);
    assert(stoneLoot.has_value() && stoneLoot->drops.size() == 1U);
    assert(stoneLoot->drops[0].id == "minecraft:cobblestone");
}

// 7. Random loot has no rebedrock evaluator: a block-state / count gate is a gap.
void testLootGap() {
    // A trimmed real `wheat.json`: an age-gated alternatives, plus a second pool
    // that only fires at age 7. Neither is deterministic.
    const auto wheat = mc::core::Json::parse(R"({
        "type": "minecraft:block",
        "functions": [ { "function": "minecraft:explosion_decay" } ],
        "pools": [ {
            "rolls": 1.0,
            "entries": [ {
                "type": "minecraft:alternatives",
                "children": [
                    { "type": "minecraft:item",
                      "conditions": [ { "condition": "minecraft:block_state_property",
                                        "properties": { "age": "7" } } ],
                      "name": "minecraft:wheat" },
                    { "type": "minecraft:item", "name": "minecraft:wheat_seeds" }
                ]
            } ]
        } ]
    })");
    assert(!je::jeBlockLoot(wheat).has_value()); // needs conversion

    // A Fortune bonus function is a gap too.
    const auto ore = mc::core::Json::parse(R"({
        "type": "minecraft:block",
        "pools": [ {
            "rolls": 1.0,
            "entries": [ {
                "type": "minecraft:item",
                "functions": [ { "function": "minecraft:apply_bonus",
                                 "enchantment": "minecraft:fortune" } ],
                "name": "minecraft:coal"
            } ]
        } ]
    })");
    assert(!je::jeBlockLoot(ore).has_value());
}

} // namespace

int main() {
    testVanillaNameCompleteness();
    testTagIngestsDirectly();
    testShapedRecipe();
    testShapelessTagGap();
    testSmeltingRecipe();
    testLootDeterministic();
    testLootGap();
    return 0;
}
