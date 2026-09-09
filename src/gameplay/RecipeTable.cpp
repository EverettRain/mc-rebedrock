#include "gameplay/RecipeTable.hpp"

#include "compat/ContentNamespace.hpp"
#include "core/Json.hpp"
#include "data/DataPackPaths.hpp"
#include "data/RecipeFile.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/ItemRegistry.hpp"
#include "world/Block.hpp"

// The baked constexpr floor: identifiers + shape, generated from the recipes
// that used to be hardcoded in CraftingSystem. Included once, here.
#include "gameplay/RecipeBakedData.inc"

namespace mc::data::recipe {
namespace {

// ADV-1 ★★ `unlockedBy` 是必填，漏填在**编译期**停下。
//
// vanilla 的 1492 个 `advancement/recipes/**.json` 是数据生成期从配方构建器上的
// `unlockedBy(...)` 派生出来的；本作把那一列写回配方自己那一行，成就底座在加载
// 期生成。这条 static_assert 就是「两份表述会不同步」这个风险的类型层面死因：
// 新增一条配方而不写解锁材料，聚合初始化会把这一格值初始化成
// `IngredientDefKind::Empty`（一格空材料对「解锁条件」没有意义，所以这里把它当
// 「没填」的哨兵），于是整个构建停下——不会等到运行期才发现那条配方永远解锁不了。
[[nodiscard]] constexpr bool everyRecipeNamesAnUnlockMaterial() {
    for (const auto& recipe : kBakedCraftingRecipes) {
        if (recipe.unlockedBy.kind == IngredientDefKind::Empty) return false;
    }
    for (const auto& recipe : kBakedFurnaceRecipes) {
        if (recipe.unlockedBy.kind == IngredientDefKind::Empty) return false;
    }
    return true;
}

static_assert(everyRecipeNamesAnUnlockMaterial(),
              "RecipeBakedData.inc: 每条配方都必须填 unlockedBy（触发它解锁的那个"
              "材料，逐条抄 vanilla `advancement/recipes/**.json` 里那个 "
              "inventory_changed 的 items）。漏填的那条在这里停下。");

} // namespace
} // namespace mc::data::recipe

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

// ADV-1：把 def 里的解锁材料填进 RecipeUnlock 的两个形态。`identifier` 由调用方
// 给出一段**稳定**的存储（内置的是静态烘焙数据，数据包来的是 ownedNames_ 里的
// 那一份），这里只负责搬。解析不出来（本作没有这个 id）时留成 Empty：配方本身
// 照常可做，只是没有解锁成就——为了一条成就把整条配方丢掉是本末倒置。
void resolveUnlock(const data::IngredientDef& def, std::string_view identifier,
                   RecipeUnlock& out) {
    out = RecipeUnlock{};
    RecipeIngredient resolved;
    if (def.kind == data::IngredientDefKind::Empty || !resolveIngredient(def, resolved)) {
        return;
    }
    out.kind = resolved.kind;
    out.resolved = resolved;
    out.identifier = def.kind == data::IngredientDefKind::Planks ? std::string_view{} : identifier;
}

[[nodiscard]] bool resolveCrafting(const data::CraftingRecipeDef& def, std::string_view identifier,
                                   std::string_view unlockedById, CraftingRecipe& out) {
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
    resolveUnlock(def.unlockedBy, unlockedById, recipe.unlockedBy);
    out = std::move(recipe);
    return true;
}

[[nodiscard]] bool resolveFurnace(const data::FurnaceRecipeDef& def, std::string_view identifier,
                                  std::string_view unlockedById, FurnaceRecipe& out) {
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
    resolveUnlock(def.unlockedBy, unlockedById, recipe.unlockedBy);
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
        if (resolveCrafting(data::recipe::toDef(baked), baked.identifier, baked.unlockedBy.id,
                            recipe)) {
            crafting_.push_back(std::move(recipe));
        }
    }
    for (const auto& baked : data::recipe::kBakedFurnaceRecipes) {
        FurnaceRecipe recipe;
        if (resolveFurnace(data::recipe::toDef(baked), baked.identifier, baked.unlockedBy.id,
                           recipe)) {
            furnace_.push_back(std::move(recipe));
        }
    }
}

void RecipeTable::load(const assets::ResourceProvider& resources) {
    loadBuiltinDefaults();
    applyOverlay(resources);
}

void RecipeTable::applyOverlay(const assets::ResourceProvider& resources) {
    // Recipes live under a pack's `data/` half (JE layout: data/<ns>/recipe/ —
    // singular, see data/DataPackPaths.hpp; this call site read "recipes" until
    // ADV-0 and therefore matched nothing in a real 26.1 pack),
    // never `assets/` — PACK-1's on-disk per-save datapacks are the first real
    // caller to scan a directory for these, which is what surfaced list()'s
    // default-to-assets root as a bug fixed alongside this card.
    for (const auto& location :
        resources.list("minecraft", data::pack::kRecipeDir, assets::PackType::ServerData)) {
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
        // ADV-0b 归一化边界①：数据包文件名给出的 id。vanilla 包里的
        // `minecraft:oak_planks` 必须**覆盖**我们的 `rebedrock:oak_planks`，
        // 而不是被当成第二条重名配方追加进来。
        const std::string name = compat::canonicalContentId(keyFor(location, data::pack::kRecipeDir));

        // A `type` of "smelting" selects the furnace shape; anything else (and the
        // default) is a crafting recipe.
        const bool smelting = root["type"].isString() && root["type"].asString() == "smelting";
        if (smelting) {
            data::FurnaceRecipeDef def;
            FurnaceRecipe resolved;
            if (!data::Codec<data::FurnaceRecipeDef>::read(root, def)) {
                continue;
            }
            // ADV-1：解锁材料的 id 也要一段稳定存储（RecipeUnlock::identifier
            // 是 view）。ownedNames_ 是 deque，元素不搬家，与配方 id 同一条路。
            ownedNames_.push_back(def.unlockedBy.id);
            const std::string_view unlockId = ownedNames_.back();
            for (auto& existing : furnace_) {
                if (existing.identifier == name) {
                    if (resolveFurnace(def, existing.identifier, unlockId, resolved)) {
                        existing = std::move(resolved);
                    }
                    goto nextFile;
                }
            }
            ownedNames_.push_back(name);
            if (resolveFurnace(def, ownedNames_.back(), unlockId, resolved)) {
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
            ownedNames_.push_back(def.unlockedBy.id); // 见上：解锁材料 id 的稳定存储
            const std::string_view unlockId = ownedNames_.back();
            for (auto& existing : crafting_) {
                if (existing.identifier == name) {
                    if (resolveCrafting(def, existing.identifier, unlockId, resolved)) {
                        existing = std::move(resolved);
                    }
                    goto nextFile;
                }
            }
            ownedNames_.push_back(name);
            if (resolveCrafting(def, ownedNames_.back(), unlockId, resolved)) {
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
