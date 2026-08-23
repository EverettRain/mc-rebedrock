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
    assert(crafting.size() == 43U); // the exact count that used to be hardcoded
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
    assert(table.crafting().size() == 44U); // 43 built-ins + demo_combo (oak_planks replaced)
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
    assert(table.crafting().size() == 43U);
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
    assert(table.crafting().size() == 43U); // neither bad recipe was added
    assert(findCrafting(table.crafting(), "minecraft:bad_item") == nullptr);
    assert(findCrafting(table.crafting(), "minecraft:bad_output") == nullptr);
}

} // namespace

int main() {
    testCodecRoundTrip();
    testBuiltinFloorResolves();
    testOverlayMerges();
    testNoDataFallback();
    testUnknownIdentifierSkipped();
    return 0;
}
