// D-3: recipes through the data path.
//
// The recipe list is no longer a static vector inside the matcher; it is a baked
// constexpr floor resolved by RecipeTable plus a datapack overlay. What is pinned
// here: the codec round-trips a recipe through JSON text, the baked floor
// resolves to exactly the recipes that used to be hardcoded (count and the
// representative shapes across every ingredient kind), an overlay adds/replaces
// recipes, a build with no `data/` still crafts on the floor alone, and an
// overlay recipe naming an item this build lacks is skipped rather than resolved
// into a hole.

#include "assets/ResourceProvider.hpp"
#include "core/Json.hpp"
#include "data/Codec.hpp"
#include "data/RecipeFile.hpp"
#include "gameplay/CraftingSystem.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/RecipeTable.hpp"
#include "world/Block.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using mc::data::Codec;
using mc::gameplay::CraftingRecipe;
using mc::gameplay::FurnaceRecipe;
using mc::gameplay::IngredientKind;
using mc::gameplay::RecipeTable;
using mc::world::Block;

// A datapack of recipe files, served from memory.
class MemoryProvider final : public mc::assets::ResourceProvider {
  public:
    void add(std::string path, std::string body) {
        const mc::assets::ResourceLocation location{"minecraft", std::move(path),
                                                    mc::assets::PackType::ServerData};
        files_[location.toString()] = std::move(body);
    }
    [[nodiscard]] std::filesystem::path locate(const mc::assets::ResourceLocation&) const override {
        return {};
    }
    [[nodiscard]] bool exists(const mc::assets::ResourceLocation& location) const override {
        return files_.find(location.toString()) != files_.end();
    }
    [[nodiscard]] std::filesystem::path resourceRoot() const override { return {}; }
    [[nodiscard]] std::vector<std::byte>
    readBytes(const mc::assets::ResourceLocation& location) const override {
        const auto slot = files_.find(location.toString());
        if (slot == files_.end()) return {};
        std::vector<std::byte> bytes(slot->second.size());
        std::memcpy(bytes.data(), slot->second.data(), slot->second.size());
        return bytes;
    }
    [[nodiscard]] std::vector<mc::assets::ResourceLocation>
    list(std::string_view space, std::string_view pathPrefix,
        mc::assets::PackType = mc::assets::PackType::ClientResources) const override {
        std::vector<mc::assets::ResourceLocation> found;
        for (const auto& [key, body] : files_) {
            (void)body;
            auto location =
                mc::assets::ResourceLocation::parse(key, mc::assets::PackType::ServerData);
            if (location.space == space && location.path.size() >= pathPrefix.size() &&
                std::string_view{location.path}.substr(0, pathPrefix.size()) == pathPrefix) {
                found.push_back(std::move(location));
            }
        }
        std::sort(found.begin(), found.end(),
                  [](const auto& a, const auto& b) { return a.path < b.path; });
        return found;
    }

  private:
    std::unordered_map<std::string, std::string> files_;
};

const CraftingRecipe* findCrafting(std::span<const CraftingRecipe> recipes, std::string_view id) {
    for (const auto& recipe : recipes) {
        if (recipe.identifier == id) return &recipe;
    }
    return nullptr;
}
const FurnaceRecipe* findFurnace(std::span<const FurnaceRecipe> recipes, std::string_view id) {
    for (const auto& recipe : recipes) {
        if (recipe.identifier == id) return &recipe;
    }
    return nullptr;
}

// 1. Every recipe def round-trips through JSON text, across each ingredient kind.
void testCodecRoundTrip() {
    using namespace mc::data;
    const CraftingRecipeDef shaped{
        "minecraft:demo_axe", 2, 3, false, true,
        {{IngredientDefKind::Planks, ""},
         {IngredientDefKind::Item, "minecraft:stick"},
         {IngredientDefKind::Empty, ""},
         {IngredientDefKind::Block, "minecraft:cobblestone"}},
        "minecraft:stone_axe", 1};
    assert(roundTripsThroughText(shaped));

    const FurnaceRecipeDef smelt{"minecraft:demo_smelt",
                                 {IngredientDefKind::Block, "minecraft:iron_ore"},
                                 "minecraft:iron_ingot", 1, 200, 0.7F};
    assert(roundTripsThroughText(smelt));

    // A tag other than planks is a clean failure; a non-object ingredient too.
    IngredientDef sink;
    assert(!Codec<IngredientDef>::read(mc::core::Json::parse(R"({"tag":"logs"})"), sink));
    assert(!Codec<IngredientDef>::read(mc::core::Json::parse("42"), sink));
    // The empty-cell object reads back as Empty.
    assert(Codec<IngredientDef>::read(mc::core::Json::parse("{}"), sink));
    assert(sink.kind == IngredientDefKind::Empty);
}

// 2. The baked floor resolves to exactly the former hardcoded recipes.
void testBuiltinFloorResolves() {
    RecipeTable table;
    table.loadBuiltinDefaults();
    const auto crafting = table.crafting();
    const auto furnace = table.furnace();
    // EQ-0 added 16 armor recipes (4 materials craftable x 4 slots; chainmail
    // has no recipe) on top of the 43 that used to be hardcoded. AR-CX1 appended
    // 6 utility recipes (bow/arrow/shears/bucket/paper/book); AR-CX2 landed the
    // sugar_cane block (paper now resolves) and added yellow_dye; AR-CX4-b added
    // flint_and_steel — so all 8 AR-CX crafting recipes (bow/arrow/shears/bucket/
    // paper/book/yellow_dye/flint_and_steel) resolve.
    assert(crafting.size() == 43U + 16U + 8U);
    assert(furnace.size() == 7U);

    // 1x1 log -> 4 planks, a block ingredient and a block output.
    const CraftingRecipe* planks = findCrafting(crafting, "minecraft:oak_planks");
    assert(planks != nullptr && planks->width == 1U && planks->height == 1U && !planks->shapeless);
    assert(planks->ingredients.size() == 1U);
    assert(planks->ingredients[0].kind == IngredientKind::Block);
    assert(planks->ingredients[0].block == Block::OakLog);
    assert((planks->output ==
            mc::gameplay::ItemStack{Block::OakPlanks, 4U,
                                    mc::gameplay::blockItemFor(Block::OakPlanks)}));

    // The plank group (AnyPlanks) and an item output.
    const CraftingRecipe* sticks = findCrafting(crafting, "minecraft:sticks");
    assert(sticks != nullptr && sticks->ingredients.size() == 2U);
    assert(sticks->ingredients[0].kind == IngredientKind::AnyPlanks);
    assert(sticks->output.item == &mc::gameplay::items::Stick && sticks->output.count == 4U);

    // An item ingredient (wheat) and a shaped 3x1.
    const CraftingRecipe* bread = findCrafting(crafting, "minecraft:bread");
    assert(bread != nullptr && bread->width == 3U && bread->height == 1U);
    assert(bread->ingredients[0].kind == IngredientKind::Item &&
           bread->ingredients[0].item == &mc::gameplay::items::Wheat);
    assert(bread->output.item == &mc::gameplay::items::Bread);

    // A shapeless recipe and an empty cell in a shaped one.
    assert(findCrafting(crafting, "minecraft:coarse_dirt")->shapeless);
    const CraftingRecipe* axe = findCrafting(crafting, "minecraft:wooden_axe");
    assert(axe != nullptr && axe->allowMirror);
    assert(axe->ingredients[4].kind == IngredientKind::Empty); // the gap in the 2x3

    // A furnace recipe: input, output, and the experience float survived the bake.
    const FurnaceRecipe* iron = findFurnace(furnace, "minecraft:iron_ingot_from_smelting");
    assert(iron != nullptr && iron->input.kind == IngredientKind::Block &&
           iron->input.block == Block::IronOre);
    assert(iron->output.item == &mc::gameplay::items::IronIngot);
    assert(iron->cookTicks == 200 && iron->experience == 0.7F);
}

// 2b. AR-CX1: the six utility recipes resolve to their exact 26.1 shapes and
// output counts, and paper stays inert until its sugar_cane block lands.
void testArcx1UtilityRecipes() {
    RecipeTable table;
    table.loadBuiltinDefaults();
    const auto crafting = table.crafting();

    // bow: 3x3 shaped, one bow (string + stick, no mirror).
    const CraftingRecipe* bow = findCrafting(crafting, "minecraft:bow");
    assert(bow != nullptr && bow->width == 3U && bow->height == 3U && !bow->shapeless);
    assert(bow->ingredients.size() == 9U);
    assert(bow->ingredients[1].kind == IngredientKind::Item &&
           bow->ingredients[1].item == &mc::gameplay::items::String);
    assert(bow->ingredients[2].kind == IngredientKind::Item &&
           bow->ingredients[2].item == &mc::gameplay::items::Stick);
    assert(bow->ingredients[0].kind == IngredientKind::Empty);
    assert(bow->output.item == &mc::gameplay::items::Bow && bow->output.count == 1U);

    // arrow: 1x3, flint/stick/feather, yields 4 (pin the count — sabotage guard).
    const CraftingRecipe* arrow = findCrafting(crafting, "minecraft:arrow");
    assert(arrow != nullptr && arrow->width == 1U && arrow->height == 3U && !arrow->shapeless);
    assert(arrow->ingredients.size() == 3U);
    assert(arrow->ingredients[0].item == &mc::gameplay::items::Flint);
    assert(arrow->ingredients[1].item == &mc::gameplay::items::Stick);
    assert(arrow->ingredients[2].item == &mc::gameplay::items::Feather);
    assert(arrow->output.item == &mc::gameplay::items::Arrow && arrow->output.count == 4U);

    // shears: 2x2, two iron_ingot diagonally, one shears.
    const CraftingRecipe* shears = findCrafting(crafting, "minecraft:shears");
    assert(shears != nullptr && shears->width == 2U && shears->height == 2U && !shears->shapeless);
    assert(shears->ingredients.size() == 4U);
    assert(shears->ingredients[1].item == &mc::gameplay::items::IronIngot);
    assert(shears->ingredients[2].item == &mc::gameplay::items::IronIngot);
    assert(shears->ingredients[0].kind == IngredientKind::Empty);
    assert(shears->output.item == &mc::gameplay::items::Shears && shears->output.count == 1U);

    // bucket: 3x2, three iron_ingot, one bucket.
    const CraftingRecipe* bucket = findCrafting(crafting, "minecraft:bucket");
    assert(bucket != nullptr && bucket->width == 3U && bucket->height == 2U && !bucket->shapeless);
    assert(bucket->ingredients.size() == 6U);
    assert(bucket->ingredients[0].item == &mc::gameplay::items::IronIngot);
    assert(bucket->ingredients[2].item == &mc::gameplay::items::IronIngot);
    assert(bucket->ingredients[4].item == &mc::gameplay::items::IronIngot);
    assert(bucket->output.item == &mc::gameplay::items::Bucket && bucket->output.count == 1U);

    // book: 2x2 SHAPELESS (3 paper + 1 leather), one book. The shapeless flag and
    // width/height <= 3 both matter (sabotage guard: a 1x4 book never matches).
    const CraftingRecipe* book = findCrafting(crafting, "minecraft:book");
    assert(book != nullptr && book->width == 2U && book->height == 2U && book->shapeless);
    assert(book->ingredients.size() == 4U);
    assert(book->output.item == &mc::gameplay::items::Book && book->output.count == 1U);

    // paper: AR-CX2 landed the sugar_cane block, so the recipe now resolves —
    // 3x1 shaped of three sugar_cane blocks, yielding 3 paper.
    const CraftingRecipe* paper = findCrafting(crafting, "minecraft:paper");
    assert(paper != nullptr && paper->width == 3U && paper->height == 1U && !paper->shapeless);
    assert(paper->ingredients.size() == 3U);
    assert(paper->ingredients[0].kind == IngredientKind::Block &&
           paper->ingredients[0].block == Block::SugarCane);
    assert(paper->output.item == &mc::gameplay::items::Paper && paper->output.count == 3U);

    // yellow_dye (AR-CX2): 1x1 shapeless dandelion block -> 1 yellow_dye.
    const CraftingRecipe* yellowDye = findCrafting(crafting, "minecraft:yellow_dye");
    assert(yellowDye != nullptr && yellowDye->width == 1U && yellowDye->height == 1U &&
           yellowDye->shapeless);
    assert(yellowDye->ingredients.size() == 1U);
    assert(yellowDye->ingredients[0].kind == IngredientKind::Block &&
           yellowDye->ingredients[0].block == Block::Dandelion);
    assert(yellowDye->output.item == &mc::gameplay::items::YellowDye &&
           yellowDye->output.count == 1U);

    // flint_and_steel (AR-CX4-b): shapeless iron_ingot + flint -> 1
    // flint_and_steel. Both ingredients are items (iron from smelting, flint from
    // gravel), so the recipe resolves the moment flint_and_steel registers.
    const CraftingRecipe* flintAndSteel = findCrafting(crafting, "minecraft:flint_and_steel");
    assert(flintAndSteel != nullptr && flintAndSteel->shapeless);
    assert(flintAndSteel->ingredients.size() == 2U);
    assert(flintAndSteel->ingredients[0].kind == IngredientKind::Item &&
           flintAndSteel->ingredients[0].item == &mc::gameplay::items::IronIngot);
    assert(flintAndSteel->ingredients[1].kind == IngredientKind::Item &&
           flintAndSteel->ingredients[1].item == &mc::gameplay::items::Flint);
    assert(flintAndSteel->output.item == &mc::gameplay::items::FlintAndSteel &&
           flintAndSteel->output.count == 1U);
}

// 2c. The utility recipes actually match a live grid and yield the pinned counts.
void testArcx1CraftMatches() {
    using mc::gameplay::CraftingSystem;
    using mc::gameplay::ItemStack;
    namespace items = mc::gameplay::items;

    // arrow: 1x3 column in the 3x3 table grid (col 0), yields 4.
    {
        CraftingSystem crafting;
        crafting.tableGridSlot(0) = {Block::Air, 1U, &items::Flint};
        crafting.tableGridSlot(3) = {Block::Air, 1U, &items::Stick};
        crafting.tableGridSlot(6) = {Block::Air, 1U, &items::Feather};
        const ItemStack out = crafting.tableOutput();
        assert(out.item == &items::Arrow && out.count == 4U);
    }
    // shears: 2x2 in the top-left of the table grid, yields 1.
    {
        CraftingSystem crafting;
        crafting.tableGridSlot(1) = {Block::Air, 1U, &items::IronIngot};
        crafting.tableGridSlot(3) = {Block::Air, 1U, &items::IronIngot};
        const ItemStack out = crafting.tableOutput();
        assert(out.item == &items::Shears && out.count == 1U);
    }
    // bucket: 3x2 "# #" / " # " in the table grid, yields 1.
    {
        CraftingSystem crafting;
        crafting.tableGridSlot(0) = {Block::Air, 1U, &items::IronIngot};
        crafting.tableGridSlot(2) = {Block::Air, 1U, &items::IronIngot};
        crafting.tableGridSlot(4) = {Block::Air, 1U, &items::IronIngot};
        const ItemStack out = crafting.tableOutput();
        assert(out.item == &items::Bucket && out.count == 1U);
    }
    // bow: 3x3 in the table grid, yields 1.
    {
        CraftingSystem crafting;
        crafting.tableGridSlot(1) = {Block::Air, 1U, &items::String};
        crafting.tableGridSlot(2) = {Block::Air, 1U, &items::Stick};
        crafting.tableGridSlot(3) = {Block::Air, 1U, &items::String};
        crafting.tableGridSlot(5) = {Block::Air, 1U, &items::Stick};
        crafting.tableGridSlot(7) = {Block::Air, 1U, &items::String};
        crafting.tableGridSlot(8) = {Block::Air, 1U, &items::Stick};
        const ItemStack out = crafting.tableOutput();
        assert(out.item == &items::Bow && out.count == 1U);
    }
    // book: 2x2 shapeless (position-independent) 3 paper + 1 leather, yields 1.
    // Match against the 2x2 player grid — shapeless with width/height<=2 fits.
    {
        CraftingSystem crafting;
        crafting.playerGridSlot(0) = {Block::Air, 1U, &items::Paper};
        crafting.playerGridSlot(1) = {Block::Air, 1U, &items::Paper};
        crafting.playerGridSlot(2) = {Block::Air, 1U, &items::Leather};
        crafting.playerGridSlot(3) = {Block::Air, 1U, &items::Paper};
        const ItemStack out = crafting.playerOutput();
        assert(out.item == &items::Book && out.count == 1U);
    }
}

// 3. A datapack overlay adds a recipe, replaces a built-in by id, and adds a
// smelting recipe.
void testOverlayMerges() {
    RecipeTable table;
    MemoryProvider pack;
    // A brand-new shapeless recipe.
    pack.add("recipes/demo_combo.json",
             R"({"width":1,"height":2,"shapeless":true,
                 "ingredients":[{"item":"minecraft:coal"},{"item":"minecraft:coal"}],
                 "output":"minecraft:diamond","count":1})");
    // Replace the built-in oak_planks to yield 8 rather than 4.
    pack.add("recipes/oak_planks.json",
             R"({"width":1,"height":1,"ingredients":[{"block":"minecraft:oak_log"}],
                 "output":"minecraft:oak_planks","count":8})");
    // A smelting override/addition.
    pack.add("recipes/demo_smelt.json",
             R"({"type":"smelting","input":{"block":"minecraft:cobblestone"},
                 "output":"minecraft:stone","count":1,"cookTicks":123,"experience":0.5})");

    table.load(pack);
    // 43+16 built-ins (EQ-0 armor) + 8 AR-CX utility (paper resolves + yellow_dye
    // (AR-CX2) + flint_and_steel (AR-CX4-b)) + demo_combo (oak_planks replaced in
    // place, not added).
    assert(table.crafting().size() == 43U + 16U + 8U + 1U);
    assert(findCrafting(table.crafting(), "minecraft:demo_combo") != nullptr);
    assert(findCrafting(table.crafting(), "minecraft:oak_planks")->output.count == 8U);
    const FurnaceRecipe* smelt = findFurnace(table.furnace(), "minecraft:demo_smelt");
    assert(smelt != nullptr && smelt->cookTicks == 123 && smelt->output.block == Block::Stone);
}

// 4. No `data/` at all: the floor is the whole table and crafting still works.
void testNoDataFallback() {
    RecipeTable table;
    MemoryProvider empty;
    table.load(empty);
    assert(table.crafting().size() == 43U + 16U + 8U);
    assert(table.furnace().size() == 7U);
    assert(findCrafting(table.crafting(), "minecraft:oak_planks")->output.count == 4U);
}

// 5. An overlay recipe naming content this build lacks is skipped, not resolved
// into a broken recipe.
void testUnknownIdentifierSkipped() {
    RecipeTable table;
    MemoryProvider pack;
    pack.add("recipes/bad_item.json",
             R"({"width":1,"height":1,"ingredients":[{"item":"minecraft:no_such_item"}],
                 "output":"minecraft:oak_planks","count":1})");
    pack.add("recipes/bad_output.json",
             R"({"width":1,"height":1,"ingredients":[{"item":"minecraft:coal"}],
                 "output":"minecraft:no_such_block","count":1})");
    table.load(pack);
    assert(table.crafting().size() == 43U + 16U + 8U); // neither bad recipe was added
    assert(findCrafting(table.crafting(), "minecraft:bad_item") == nullptr);
    assert(findCrafting(table.crafting(), "minecraft:bad_output") == nullptr);
}

} // namespace

int main() {
    testCodecRoundTrip();
    testBuiltinFloorResolves();
    testArcx1UtilityRecipes();
    testArcx1CraftMatches();
    testOverlayMerges();
    testNoDataFallback();
    testUnknownIdentifierSkipped();
    return 0;
}
