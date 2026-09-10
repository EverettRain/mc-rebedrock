#include "gameplay/RecipeBookCategory.hpp"

#include "compat/ContentNamespace.hpp"

#include <algorithm>
#include <string>
#include <span>
#include <string_view>

namespace mc::gameplay {
namespace data {

struct BakedRecipeCategory final {
    std::string_view identifier;
    RecipeBookCategory category;
};

// vanilla 的 `category` 字段，一行一条配方。
#include "gameplay/RecipeBookCategoryData.inc"

} // namespace data

namespace {

// 两张表都按标识符升序（生成器排的），所以查表是二分而不是线性扫。
[[nodiscard]] constexpr bool sortedByIdentifier(
    std::span<const data::BakedRecipeCategory> rows) {
    for (std::size_t index = 1U; index < rows.size(); ++index) {
        if (!(rows[index - 1U].identifier < rows[index].identifier)) return false;
    }
    return true;
}

static_assert(sortedByIdentifier(data::kCraftingRecipeCategories),
              "RecipeBookCategoryData.inc 的合成表必须按标识符升序——查表是二分");
static_assert(sortedByIdentifier(data::kFurnaceRecipeCategories),
              "RecipeBookCategoryData.inc 的熔炉表必须按标识符升序——查表是二分");

// ADV-0b 归一化边界④：分类查表。两张表的 key 已经统一成 `rebedrock:`（且仍然
// 升序——前缀是统一换的，相对顺序不变，上面的 static_assert 继续成立），所以
// vanilla 拼法要先换掉才二分得到。★ 这一条是最容易漏的：查不到不会报错，只会
// 静默落到 Misc 分类。
[[nodiscard]] RecipeBookCategory lookup(std::span<const data::BakedRecipeCategory> rows,
                                        std::string_view identifier,
                                        RecipeBookCategory fallback) {
    const std::string canonical = compat::canonicalContentId(identifier);
    const std::string_view key{canonical};
    const auto row = std::ranges::lower_bound(
        rows, key, {}, &data::BakedRecipeCategory::identifier);
    if (row == rows.end() || row->identifier != key) {
        return fallback;
    }
    return row->category;
}

} // namespace

RecipeBookCategory craftingRecipeBookCategory(std::string_view identifier) {
    return lookup(data::kCraftingRecipeCategories, identifier,
                  RecipeBookCategory::CraftingMisc);
}

RecipeBookCategory furnaceRecipeBookCategory(std::string_view identifier) {
    return lookup(data::kFurnaceRecipeCategories, identifier,
                  RecipeBookCategory::FurnaceMisc);
}

} // namespace mc::gameplay
