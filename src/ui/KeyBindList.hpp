#pragma once

// UI-6c：按键绑定列表**展开后的行表**——绑定行之间夹着分类标题行。
//
// 26.1 的 `KeyBindsList` 构造时按 `KeyMapping.Category` 排序，每当分类变化就先插一条
// `CategoryEntry`（`controls/KeyBindsList.java:29-46`）。排序归 input 层（它知道每个动作
// 属于哪一类）；**插标题行是界面的事**，所以在这里，而不是让 `input::keyBindRows()`
// 返回一个混着标题的序列——那会让 input 层的花名册被显示需求污染。
//
// 展开之后"一行"不再等于"一个动作"：24 个动作加 6 个分类标题 = 30 行。滚动窗口因此
// 数的是**行**，不是动作。这一点很容易写错成后者，症状是滚到底时最后几行进不来。
//
// 纯 constexpr，不碰 Vulkan、不碰语言表（标题的翻译键由 input 层给，翻译由绘制侧做），
// 因此整张表都能被无头断言。

#include "input/InputAction.hpp"
#include "input/InputNaming.hpp"

#include <array>
#include <cstddef>

namespace mc::ui {

// 列表里的一行：要么是一条分类标题，要么是一个可重绑的动作。
struct KeyBindListEntry final {
    bool isCategory = false;
    input::InputCategory category = input::InputCategory::Movement;
    input::InputAction action = input::InputAction::Count;

    [[nodiscard]] constexpr bool operator==(const KeyBindListEntry&) const = default;
};

// 展开后的行数：动作数 + 分类数（每个分类恰好一条标题）。
//
// 分类数从**实际用到的**分类算，不是 `kInputCategoryCount`：26.1 有八个分类，
// 本作的花名册只落在其中六个（没有 CREATIVE、SPECTATOR 的可绑定动作），
// 而一条没有任何行的分类标题是一行孤零零的字。
[[nodiscard]] constexpr std::size_t keyBindUsedCategoryCount() {
    std::size_t used = 0;
    auto previous = input::InputCategory::Count;
    for (const auto action : input::keyBindRows()) {
        const auto category = input::actionCategory(action);
        if (category != previous) {
            ++used;
            previous = category;
        }
    }
    return used;
}

inline constexpr std::size_t kKeyBindListRowCount =
    input::keyBindRows().size() + keyBindUsedCategoryCount();

// 整张表。分类边界处插一条标题，其余照 `input::keyBindRows()` 的顺序。
//
// ★ 这一条依赖 `keyBindRows()` **已按分类分组排序**（input 层保证）。没排序的话
//   同一个分类会被切成好几段、每段前面都顶一条标题——画面上是"分类标题重复出现"。
[[nodiscard]] constexpr std::array<KeyBindListEntry, kKeyBindListRowCount> keyBindListRows() {
    std::array<KeyBindListEntry, kKeyBindListRowCount> rows{};
    std::size_t out = 0;
    auto previous = input::InputCategory::Count;
    for (const auto action : input::keyBindRows()) {
        const auto category = input::actionCategory(action);
        if (category != previous) {
            rows[out] = KeyBindListEntry{true, category, input::InputAction::Count};
            ++out;
            previous = category;
        }
        rows[out] = KeyBindListEntry{false, category, action};
        ++out;
    }
    return rows;
}

// 第 `row` 行是什么。越界返回一条空条目而不是抛：调用方是绘制循环，
// 让它在一个滚动位置上崩溃远比少画一行糟。
[[nodiscard]] constexpr KeyBindListEntry keyBindListRow(std::size_t row) {
    constexpr auto rows = keyBindListRows();
    return row < rows.size() ? rows[row] : KeyBindListEntry{};
}

// 从第 `firstRow` 行起、第 `widgetIndex` 个控件落在**第几个可见行**（相对 firstRow）。
//
// 标题行不产生任何控件，所以控件序号与行号之间**不是**倍数关系：
// 一个绑定行贡献 `widgetsPerRow` 个序号，一条标题行贡献 0。
// 写成纯函数是因为绘制侧与输入侧都要它，而这正是上一轮那次闪退的形状
// （同一个映射两份抄本，改了一份）。
[[nodiscard]] constexpr std::size_t keyBindWidgetVisibleRow(std::size_t firstRow,
                                                            std::size_t widgetIndex,
                                                            std::size_t widgetsPerRow) {
    if (widgetsPerRow == 0U) {
        return 0U;
    }
    std::size_t seen = 0;
    for (std::size_t offset = 0; firstRow + offset < kKeyBindListRowCount; ++offset) {
        if (keyBindListRow(firstRow + offset).isCategory) {
            continue;
        }
        if (widgetIndex < seen + widgetsPerRow) {
            return offset;
        }
        seen += widgetsPerRow;
    }
    return 0U;
}

// 从第 `firstRow` 起的 `visibleRows` 行里，有几行是绑定行（也就是会产生控件的行）。
[[nodiscard]] constexpr std::size_t keyBindBindingRowsIn(std::size_t firstRow,
                                                         std::size_t visibleRows) {
    std::size_t bindings = 0;
    for (std::size_t offset = 0; offset < visibleRows; ++offset) {
        const std::size_t row = firstRow + offset;
        if (row >= kKeyBindListRowCount) {
            break;
        }
        if (!keyBindListRow(row).isCategory) {
            ++bindings;
        }
    }
    return bindings;
}

} // namespace mc::ui
