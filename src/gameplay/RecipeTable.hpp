#pragma once

// The process-level recipe table: the built-in floor plus a datapack overlay,
// the same two-layer shape BlockTags uses. D-3 moved the recipe list out of
// CraftingSystem — it was a static vector of hand-transcribed recipes — into the
// baked constexpr floor (RecipeBakedData.inc, resolved here once) and a codec
// overlay. The matcher still reads a `span<const CraftingRecipe>`; it neither
// knows nor cares that the span now comes from data.
//
// Resolution — turning a recipe's identifier strings into the Item*/Block the
// matcher compares — lives here because this is the one place that may reach the
// item and block registries. An overlay recipe naming an item or block this
// build has no id for is skipped whole, never resolved to the wrong ingredient.

#include "assets/ResourceProvider.hpp"
#include "gameplay/CraftingSystem.hpp"

#include <deque>
#include <span>
#include <string>
#include <vector>

namespace mc::gameplay {

class RecipeTable final {
  public:
    // Resolves the baked constexpr floor. No parsing; this is the whole table for
    // a headless caller or an installation whose pack carries only `assets/`.
    void loadBuiltinDefaults();

    // The floor, then a datapack overlay merged on top by identifier: a file
    // whose id matches a built-in replaces that recipe, a new id is appended.
    void load(const assets::ResourceProvider& resources);

    [[nodiscard]] std::span<const CraftingRecipe> crafting() const { return crafting_; }
    [[nodiscard]] std::span<const FurnaceRecipe> furnace() const { return furnace_; }

  private:
    void applyOverlay(const assets::ResourceProvider& resources);

    std::vector<CraftingRecipe> crafting_;
    std::vector<FurnaceRecipe> furnace_;
    // Stable backing for identifiers an overlay supplied (a CraftingRecipe holds
    // its identifier as a string_view; the built-in ones view static baked data,
    // these view here). A deque never relocates its elements, so the views stay
    // valid as more overlay recipes are appended.
    std::deque<std::string> ownedNames_;
};

// The table CraftingSystem's matcher reads. Built-in defaults on first use, so
// crafting works with no wiring at all; Application loads the overlay once the
// pack stack is up, exactly as it does for block tags.
[[nodiscard]] RecipeTable& recipeTable();

} // namespace mc::gameplay
