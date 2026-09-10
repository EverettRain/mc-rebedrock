#pragma once

// The crafting/smelting recipe file format, as D-1 codec definitions.
//
// D-3 moves the recipe list out of CraftingSystem's evaluator — where it was a
// static vector of hand-transcribed recipes (`bread.json` copied into C++) — and
// into data: a baked constexpr floor (below, resolved once at load) plus a
// datapack overlay parsed through the codec, with the built-in floor as the
// no-`data/` fallback. The matcher (shaped/shapeless/mirror) is untouched; only
// where a recipe *comes from* changes.
//
// These defs are data only — identifiers as strings, no Item*/Block — so this
// header has no gameplay dependency. Resolving an identifier to the runtime
// ingredient (an ItemId/BlockId, then the Item*/Block the matcher compares) is
// gameplay's job, in RecipeTable, the one place that knows the item and block
// registries.

#include "data/Codec.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mc::data {

// What one grid cell (or a furnace input) requires. Mirrors CraftingSystem's
// IngredientKind so resolution is one-to-one: an empty cell, a specific item, a
// specific block, or the "any plank" group vanilla spells as the `#planks` tag.
enum class IngredientDefKind : std::uint8_t {
    Empty,
    Item,
    Block,
    Planks,
};

struct IngredientDef final {
    IngredientDefKind kind = IngredientDefKind::Empty;
    std::string id; // the item/block identifier for Item/Block; empty otherwise

    [[nodiscard]] bool operator==(const IngredientDef&) const = default;
};

struct CraftingRecipeDef final {
    std::string identifier;
    std::uint8_t width = 0U;
    std::uint8_t height = 0U;
    bool shapeless = false;
    bool allowMirror = false;
    std::vector<IngredientDef> ingredients;
    std::string output; // the produced item/block identifier
    std::uint8_t count = 1U;
    // ADV-1: which material unlocks this recipe (see BakedCraftingRecipe below).
    // A datapack recipe that names none stays Empty, and the advancement floor
    // simply generates no unlock advancement for it — an overlay recipe is
    // reachable through /recipe give and through crafting it, which is what
    // vanilla does for a recipe whose provider never called unlockedBy().
    IngredientDef unlockedBy;

    [[nodiscard]] bool operator==(const CraftingRecipeDef&) const = default;
};

struct FurnaceRecipeDef final {
    std::string identifier;
    IngredientDef input;
    std::string output;
    std::uint8_t count = 1U;
    std::int32_t cookTicks = 200;
    float experience = 0.0F;
    IngredientDef unlockedBy; // ADV-1, see CraftingRecipeDef::unlockedBy

    [[nodiscard]] bool operator==(const FurnaceRecipeDef&) const = default;
};

// An ingredient encodes as the object form that names its kind: `{}` empty,
// `{"item": "..."}`, `{"block": "..."}`, or `{"tag": "planks"}`. A `tag` other
// than `planks` is the one group this build understands, so anything else is a
// clean read failure and the overlay skips that recipe rather than resolving it
// wrong.
template <>
struct Codec<IngredientDef> {
    static core::Json write(const IngredientDef& ingredient) {
        ObjectWriter writer;
        switch (ingredient.kind) {
            case IngredientDefKind::Empty:
                break;
            case IngredientDefKind::Item:
                writer.field("item", ingredient.id);
                break;
            case IngredientDefKind::Block:
                writer.field("block", ingredient.id);
                break;
            case IngredientDefKind::Planks:
                writer.field("tag", std::string{"planks"});
                break;
        }
        return writer.take();
    }
    static bool read(const core::Json& json, IngredientDef& out) {
        if (!json.isObject()) {
            return false;
        }
        if (json.contains("item")) {
            out.kind = IngredientDefKind::Item;
            return Codec<std::string>::read(json["item"], out.id);
        }
        if (json.contains("block")) {
            out.kind = IngredientDefKind::Block;
            return Codec<std::string>::read(json["block"], out.id);
        }
        if (json.contains("tag")) {
            std::string tag;
            if (!Codec<std::string>::read(json["tag"], tag) || tag != "planks") {
                return false;
            }
            out.kind = IngredientDefKind::Planks;
            out.id.clear();
            return true;
        }
        // An object naming none of the keys is an empty cell.
        out.kind = IngredientDefKind::Empty;
        out.id.clear();
        return true;
    }
};

template <>
struct Codec<CraftingRecipeDef> {
    static core::Json write(const CraftingRecipeDef& recipe) {
        return ObjectWriter{}
            .field("identifier", recipe.identifier)
            .field("width", recipe.width)
            .field("height", recipe.height)
            .field("shapeless", recipe.shapeless)
            .field("allowMirror", recipe.allowMirror)
            .field("ingredients", recipe.ingredients)
            .field("output", recipe.output)
            .field("count", recipe.count)
            .field("unlockedBy", recipe.unlockedBy)
            .take();
    }
    static bool read(const core::Json& json, CraftingRecipeDef& out) {
        ObjectReader reader{json};
        reader.optionalField("identifier", out.identifier)
            .field("width", out.width)
            .field("height", out.height)
            .optionalField("shapeless", out.shapeless)
            .optionalField("allowMirror", out.allowMirror)
            .field("ingredients", out.ingredients)
            .field("output", out.output)
            .optionalField("count", out.count)
            // ADV-1: optional — a datapack recipe that names no unlock material
            // simply gets no unlock advancement (see CraftingRecipeDef).
            .optionalField("unlockedBy", out.unlockedBy);
        return reader.ok();
    }
};

template <>
struct Codec<FurnaceRecipeDef> {
    static core::Json write(const FurnaceRecipeDef& recipe) {
        return ObjectWriter{}
            .field("identifier", recipe.identifier)
            .field("input", recipe.input)
            .field("output", recipe.output)
            .field("count", recipe.count)
            .field("cookTicks", recipe.cookTicks)
            .field("experience", recipe.experience)
            .field("unlockedBy", recipe.unlockedBy)
            .take();
    }
    static bool read(const core::Json& json, FurnaceRecipeDef& out) {
        ObjectReader reader{json};
        reader.optionalField("identifier", out.identifier)
            .field("input", out.input)
            .field("output", out.output)
            .optionalField("count", out.count)
            .optionalField("cookTicks", out.cookTicks)
            .optionalField("experience", out.experience)
            .optionalField("unlockedBy", out.unlockedBy);
        return reader.ok();
    }
};

} // namespace mc::data

namespace mc::data::recipe {

// The baked, constexpr-friendly forms: string_view and span instead of string
// and vector, so the built-in floor is a compile-time table in `.rodata` and
// bringing it up parses nothing. RecipeTable converts these to the owning defs
// (below), then resolves them against the item/block registries.
struct BakedIngredient final {
    IngredientDefKind kind;
    std::string_view id;
};

// ADV-1：`unlockedBy` —— 触发这条配方解锁的那个材料。
//
// 这不是本作发明的字段，是把 vanilla 已有的东西搬到它本来的位置：vanilla 的
// `advancement/recipes/**.json`（1492 个）是**派生产物**，源头是配方构建器上的
// `unlockedBy(...)`，`RecipeProvider` 在数据生成期把它生成成 JSON。所以解锁材料
// 本来就属于「配方那一行」，写成第二份 77 条独立成就表才是复制。
//
// 类型是 BakedIngredient 而不是裸 id 字符串：vanilla 那个 `items` 谓词写的经常
// 是**标签**（`#minecraft:planks`、`#minecraft:stone_tool_materials`），而
// BakedIngredient 的 Planks 分支正是本作表达「任意木板」的那一个；用它还能让
// 「这堆物品算不算这个材料」复用 `ingredientMatches`——配方匹配的单一真相源，
// 不必再抄一份谓词。
//
// ★ 必填。`IngredientDefKind::Empty` 在这里被保留成「没填」的哨兵（一格空材料
// 对「解锁条件」没有意义），RecipeTable.cpp 里有一条 constexpr 全表检查的
// static_assert：新增一条配方而漏填 unlockedBy，**编译期**就停下。这条规矩是
// 为了让「配方表与解锁表两份表述会不同步」这个风险死在类型层面。
struct BakedCraftingRecipe final {
    std::string_view identifier;
    std::uint8_t width;
    std::uint8_t height;
    bool shapeless;
    bool allowMirror;
    std::span<const BakedIngredient> ingredients;
    std::string_view output;
    std::uint8_t count;
    BakedIngredient unlockedBy;
};

struct BakedFurnaceRecipe final {
    std::string_view identifier;
    BakedIngredient input;
    std::string_view output;
    std::uint8_t count;
    std::int32_t cookTicks;
    float experience;
    BakedIngredient unlockedBy;
};

[[nodiscard]] inline IngredientDef toDef(const BakedIngredient& baked) {
    return IngredientDef{baked.kind, std::string{baked.id}};
}

[[nodiscard]] inline CraftingRecipeDef toDef(const BakedCraftingRecipe& baked) {
    CraftingRecipeDef def;
    def.identifier = std::string{baked.identifier};
    def.width = baked.width;
    def.height = baked.height;
    def.shapeless = baked.shapeless;
    def.allowMirror = baked.allowMirror;
    def.ingredients.reserve(baked.ingredients.size());
    for (const auto& ingredient : baked.ingredients) {
        def.ingredients.push_back(toDef(ingredient));
    }
    def.output = std::string{baked.output};
    def.count = baked.count;
    def.unlockedBy = toDef(baked.unlockedBy);
    return def;
}

[[nodiscard]] inline FurnaceRecipeDef toDef(const BakedFurnaceRecipe& baked) {
    FurnaceRecipeDef def;
    def.identifier = std::string{baked.identifier};
    def.input = toDef(baked.input);
    def.output = std::string{baked.output};
    def.count = baked.count;
    def.cookTicks = baked.cookTicks;
    def.experience = baked.experience;
    def.unlockedBy = toDef(baked.unlockedBy);
    return def;
}

} // namespace mc::data::recipe
