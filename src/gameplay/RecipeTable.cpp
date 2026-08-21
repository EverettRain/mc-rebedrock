#include "gameplay/RecipeTable.hpp"

#include "core/Json.hpp"
#include "data/RecipeFile.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/ItemRegistry.hpp"
#include "world/Block.hpp"

// The baked constexpr floor: identifiers + shape, generated from the recipes
// that used to be hardcoded in CraftingSystem. Included once, here.
#include "gameplay/RecipeBakedData.inc"

#include <string>
#include <string_view>
#include <utility>

namespace mc::gameplay {
namespace {

// Resolves one ingredient definition to the runtime form the matcher compares.
// Returns false when a named item or block does not exist in this build, so the
// caller can drop the whole recipe rather than resolve a hole into it.
[[nodiscard]] bool resolveIngredient(const data::IngredientDef& def, RecipeIngredient& out) {
    switch (def.kind) {
        case data::IngredientDefKind::Empty:
            out = RecipeIngredient{};
            return true;
        case data::IngredientDefKind::Planks:
            out = RecipeIngredient{IngredientKind::AnyPlanks, world::Block::Air, nullptr};
            return true;
        case data::IngredientDefKind::Block:
            if (const auto block = world::blockFromIdentifier(def.id); block.has_value()) {
                out = RecipeIngredient{IngredientKind::Block, *block, nullptr};
                return true;
            }
            return false;
        case data::IngredientDefKind::Item:
            if (const Item* item = itemFromIdentifier(def.id); item != nullptr) {
                out = RecipeIngredient{IngredientKind::Item, world::Block::Air, item};
                return true;
            }
            return false;
    }
    return false;
}

// Resolves an output identifier + count to its stack: a block name yields the
// block stack (its BlockItem, the way the old outputs were built), an item name
// the item stack.
[[nodiscard]] bool resolveOutput(const std::string& id, std::uint8_t count, ItemStack& out) {
    if (const auto block = world::blockFromIdentifier(id); block.has_value()) {
        out = ItemStack{*block, count, blockItemFor(*block)};
        return true;
    }
    if (const Item* item = itemFromIdentifier(id); item != nullptr) {
        if (const BlockItem* blockItem = asBlockItem(item); blockItem != nullptr) {
            out = ItemStack{blockItem->block(), count, item};
        } else {
            out = ItemStack{world::Block::Air, count, item};
        }
        return true;
    }
    return false;
}

[[nodiscard]] bool resolveCrafting(const data::CraftingRecipeDef& def, std::string_view identifier,
                                   CraftingRecipe& out) {
    CraftingRecipe recipe;
    recipe.identifier = identifier;
    recipe.width = def.width;
    recipe.height = def.height;
    recipe.shapeless = def.shapeless;
    recipe.allowMirror = def.allowMirror;
    recipe.ingredients.reserve(def.ingredients.size());
    for (const auto& ingredient : def.ingredients) {
        RecipeIngredient resolved;
        if (!resolveIngredient(ingredient, resolved)) {
            return false;
        }
        recipe.ingredients.push_back(resolved);
    }
    if (!resolveOutput(def.output, def.count, recipe.output)) {
        return false;
    }
    out = std::move(recipe);
    return true;
}

[[nodiscard]] bool resolveFurnace(const data::FurnaceRecipeDef& def, std::string_view identifier,
                                  FurnaceRecipe& out) {
    FurnaceRecipe recipe;
    recipe.identifier = identifier;
    if (!resolveIngredient(def.input, recipe.input)) {
        return false;
    }
    if (!resolveOutput(def.output, def.count, recipe.output)) {
        return false;
    }
    recipe.cookTicks = def.cookTicks;
    recipe.experience = def.experience;
    out = std::move(recipe);
    return true;
}

// The store key an overlay file lands under: `recipes/oak_planks.json` in the
// `minecraft` namespace -> `minecraft:oak_planks`, so it matches the built-in
// identifier a replacement should overwrite.
[[nodiscard]] std::string keyFor(const assets::ResourceLocation& location,
                                 std::string_view prefix) {
    std::string_view path = location.path;
    if (path.size() >= prefix.size() && path.substr(0, prefix.size()) == prefix) {
        path.remove_prefix(prefix.size());
        if (!path.empty() && path.front() == '/') {
            path.remove_prefix(1U);
        }
    }
    if (path.size() >= 5U && path.substr(path.size() - 5U) == ".json") {
        path.remove_suffix(5U);
    }
    return location.space + ":" + std::string{path};
}

} // namespace

void RecipeTable::loadBuiltinDefaults() {
    crafting_.clear();
    furnace_.clear();
    ownedNames_.clear();
    for (const auto& baked : data::recipe::kBakedCraftingRecipes) {
        CraftingRecipe recipe;
        // The baked identifier is a static string_view; view it directly.
        if (resolveCrafting(data::recipe::toDef(baked), baked.identifier, recipe)) {
            crafting_.push_back(std::move(recipe));
        }
    }
    for (const auto& baked : data::recipe::kBakedFurnaceRecipes) {
        FurnaceRecipe recipe;
        if (resolveFurnace(data::recipe::toDef(baked), baked.identifier, recipe)) {
            furnace_.push_back(std::move(recipe));
        }
    }
}

void RecipeTable::load(const assets::ResourceProvider& resources) {
    loadBuiltinDefaults();
    applyOverlay(resources);
}

void RecipeTable::applyOverlay(const assets::ResourceProvider& resources) {
    for (const auto& location : resources.list("minecraft", "recipes")) {
        const auto bytes = resources.readBytes(location);
        if (bytes.empty()) {
            continue;
        }
        core::Json root;
        try {
            root = core::Json::parse(std::string_view{
                reinterpret_cast<const char*>(bytes.data()), bytes.size()});
        } catch (const std::exception&) {
            continue; // a malformed recipe must not take the rest of the pack down
        }
        const std::string name = keyFor(location, "recipes");

        // A `type` of "smelting" selects the furnace shape; anything else (and the
        // default) is a crafting recipe.
        const bool smelting = root["type"].isString() && root["type"].asString() == "smelting";
        if (smelting) {
            data::FurnaceRecipeDef def;
            FurnaceRecipe resolved;
            if (!data::Codec<data::FurnaceRecipeDef>::read(root, def)) {
                continue;
            }
            for (auto& existing : furnace_) {
                if (existing.identifier == name) {
                    if (resolveFurnace(def, existing.identifier, resolved)) {
                        existing = std::move(resolved);
                    }
                    goto nextFile;
                }
            }
            ownedNames_.push_back(name);
            if (resolveFurnace(def, ownedNames_.back(), resolved)) {
                furnace_.push_back(std::move(resolved));
            } else {
                ownedNames_.pop_back();
            }
        } else {
            data::CraftingRecipeDef def;
            CraftingRecipe resolved;
            if (!data::Codec<data::CraftingRecipeDef>::read(root, def)) {
                continue;
            }
            for (auto& existing : crafting_) {
                if (existing.identifier == name) {
                    if (resolveCrafting(def, existing.identifier, resolved)) {
                        existing = std::move(resolved);
                    }
                    goto nextFile;
                }
            }
            ownedNames_.push_back(name);
            if (resolveCrafting(def, ownedNames_.back(), resolved)) {
                crafting_.push_back(std::move(resolved));
            } else {
                ownedNames_.pop_back();
            }
        }
    nextFile:;
    }
}

RecipeTable& recipeTable() {
    static RecipeTable table = [] {
        RecipeTable defaults;
        defaults.loadBuiltinDefaults();
        return defaults;
    }();
    return table;
}

} // namespace mc::gameplay
