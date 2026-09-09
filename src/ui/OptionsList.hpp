#pragma once

// UI-6a：设置项的**双列**滚动列表（GUI spec §5 的范式 L2，26.1 的
// `net.minecraft.client.gui.components.OptionsList`）。
//
// 它是 ScrollList 之上的一层：视口与滚动条几何仍归 [[ui/ScrollList.hpp]]，这里只加
// "一行里横着摆两个设置项"这件事。26.1 的每一个设置子屏都用它排版
// （`OptionsSubScreen.addOptions()` → `list.addSmall(...)` / `list.addBig(...)`）。
//
// 本作在这之前一个消费者都没有：所有设置页都是"一条按钮带"，每行一个按钮。
// 于是 Video Settings 的十几项要么挤成很长一列、要么被删到放得下为止——后者正是
// 本作 12 项对 26.1 二十八项的由来之一。
//
// ★ 数值全部取自 26.1 源码，逐条带出处：
//   BIG_BUTTON_WIDTH = 310         OptionsList.java:17
//   DEFAULT_ITEM_HEIGHT = 25       OptionsList.java:18
//   小按钮宽 150                    OptionInstance.java:117（createButton 的默认宽）
//   第二列偏移 X_OFFSET = 160       OptionsList.java:107
//   起点 x = screen.width/2 - 155  OptionsList.java:148
//   格子内缩 2                      AbstractSelectionList.Entry.getContentX/Y():471,475
//
// 310 = 150 + 10 + 150：两列加中间 10 的缝，正好等于行宽。这不是巧合，是**闭合关系**，
// 下面有一条 static_assert 钉住它——改任一个数而不改其余，两列就不再对齐行的两端。

#include "ui/PageStack.hpp"
#include "ui/ScrollList.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace mc::ui {

// 一行的高度（`DEFAULT_ITEM_HEIGHT`）。注意它比控件本身高：控件高 20，格子高 25，
// 差出来的 5 是行距。
inline constexpr int kOptionsRowHeight = 25;
// 独占一行的控件宽度（`addBig`），与行宽相同。
inline constexpr int kOptionsBigWidth = kOptionsRowWidth;
// 双列时每一列的控件宽度（`OptionInstance.createButton(options)` 的默认 150）。
inline constexpr int kOptionsSmallWidth = 150;
// 第二列相对第一列的偏移（`X_OFFSET`）。
inline constexpr int kOptionsColumnOffset = 160;
// 控件在格子里的内缩（`Entry.getContentX() = getX() + 2`）。
inline constexpr int kListEntryPadding = 2;
// 设置项控件本身的高度。26.1 的按钮与滑条都是 20 高。
inline constexpr int kOptionsWidgetHeight = 20;

// ★ 闭合关系：两列加中缝正好铺满行宽。
static_assert(kOptionsSmallWidth * 2 + 10 == kOptionsBigWidth,
              "the two small columns plus their gap must fill the row width");
static_assert(kOptionsSmallWidth + 10 == kOptionsColumnOffset,
              "the second column starts one small width plus the gap to the right");

// 设置列表的视口：铺满三段式版面的内容区，行宽 310、行高 25。
[[nodiscard]] constexpr ScrollList optionsScrollList(const UiRect& contentBox) {
    return ScrollList{static_cast<int>(contentBox.x),     static_cast<int>(contentBox.y),
                      static_cast<int>(contentBox.width), static_cast<int>(contentBox.height),
                      kOptionsRowWidth,                   kOptionsRowHeight};
}

// `addSmall(a, b)` 两个一行：n 个设置项占 ceil(n/2) 行。
[[nodiscard]] constexpr std::size_t optionsSmallRowCount(std::size_t optionCount) {
    return (optionCount + 1U) / 2U;
}

// 第 `visibleIndex` 个可见行里、第 `column`（0 或 1）列那个控件的矩形。
//
// 26.1 的 `Entry.extractContent`：`x = screen.width/2 - 155`，然后 `xOffset += 160`；
// y 是 `entry.getContentY()`，也就是行顶 + 2。
//
// 起点用 `rowLeft()` 而不是照抄 `width/2 - 155`：当视口铺满画布时两者相等
// （rowLeft = 0 + W/2 - 310/2 = W/2 - 155），而列表若被摆在别处，跟着行走才是对的。
[[nodiscard]] constexpr UiRect optionsSmallCell(const ScrollList& list, std::size_t visibleIndex,
                                                int column) {
    const auto row = scrollListRow(list, visibleIndex);
    return {row.x + static_cast<float>(column * kOptionsColumnOffset),
            row.y + static_cast<float>(kListEntryPadding),
            static_cast<float>(kOptionsSmallWidth),
            static_cast<float>(kOptionsWidgetHeight)};
}

// 变高版的格位：按**像素偏移**定位，而不是"第几个可见行 × 25"。
//
// ★ 有了分节行（13 / 31）之后，`scrollListRow(list, visibleIndex)` 那条
//   `y = list.y + index * rowHeight` 就不成立了——分节行之后的每一个控件都会偏。
//   偏移由 `optionsRowTop` 累加得出，这两个函数是同一条几何的两端。
[[nodiscard]] constexpr UiRect optionsSmallCellAt(const ScrollList& list, int rowTop,
                                                  int column) {
    return {static_cast<float>(list.rowLeft() + column * kOptionsColumnOffset),
            static_cast<float>(list.y + rowTop + kListEntryPadding),
            static_cast<float>(kOptionsSmallWidth),
            static_cast<float>(kOptionsWidgetHeight)};
}

[[nodiscard]] constexpr UiRect optionsBigCellAt(const ScrollList& list, int rowTop) {
    return {static_cast<float>(list.rowLeft()),
            static_cast<float>(list.y + rowTop + kListEntryPadding),
            static_cast<float>(kOptionsBigWidth),
            static_cast<float>(kOptionsWidgetHeight)};
}

// 一个设置项落在第几行、第几列，以及它是不是**独占整行**的那种。
struct OptionsSlot final {
    std::size_t row = 0;
    int column = 0;
    // 26.1 的 `addBig`：宽 310、独占一行，而不是双列里的一格。
    bool big = false;

    [[nodiscard]] constexpr bool operator==(const OptionsSlot&) const = default;
};

// 一次 `addSmall(...)` / `addBig(...)` / `addHeader(...)` 调用。
//
// ★ 从前这张表只是一串项数（`std::size_t`），于是"这一组是 addBig 还是 addSmall"
//   这个事实**根本没地方存**。视频设置的 preset 因此被画成 150 宽的左列一格——
//   版面上看是"第一行右边空着"，而 26.1 那是一个铺满行宽的大按钮。
//
// UI-6e：又多了第三种。26.1 的设置屏用 `addHeader` 分节
// （`VideoSettingsScreen.addOptions()`：DISPLAY / QUALITY / PREFERENCES 三节），
// 而分节行**占一行却不产生控件**——与绑定列表的分类标题行同构。
enum class OptionsGroupKind : std::uint8_t {
    Small,   // addSmall：两两配对，每格 150 宽
    Big,     // addBig：每项独占一行，310 宽
    Header,  // addHeader：一行标题文字，不产生控件
};

struct OptionsGroup final {
    // 这一组有几个设置项。Header 恒为 0——它不产生控件。
    std::size_t count = 0;
    OptionsGroupKind kind = OptionsGroupKind::Small;
    // 仅 Header 用。
    std::string_view headerKey{};
    std::string_view headerFallback{};

    [[nodiscard]] constexpr bool operator==(const OptionsGroup&) const = default;
};

// 26.1 的 `addSmall(...)` 的分组行为：**每次调用从新行起**，组内两两配对。
//
//   list.addSmall(a, b);        // 行 0：a b
//   list.addSmall(c, d, e);     // 行 1：c d   行 2：e
//   list.addSmall(f);           // 行 3：f
//
// 这不是"把所有项摊平了两两配对"——那样上例的 e 会和 f 挤在同一行，而 26.1 里它们
// 分属两次调用、必须分行。ControlsScreen 就是这个形状：一次 addSmall 放两个跳转按钮，
// 再一次 addSmall 放七个设置项，中间那道行边界是**语义分组**，不是排版巧合。
//
// `groupSizes` 是各次 addSmall 的项数，按调用顺序。越界返回最后一行之后的位置而不抛：
// 调用方通常已经用控件数夹过，这里再抛一次只会把一个排版问题变成崩溃。
[[nodiscard]] constexpr std::size_t optionsGroupRowCount(const OptionsGroup& group) {
    switch (group.kind) {
    case OptionsGroupKind::Header:
        return 1U;   // 一行标题，零个控件
    case OptionsGroupKind::Big:
        return group.count;   // 每项独占一行
    case OptionsGroupKind::Small:
        break;
    }
    return (group.count + 1U) / 2U;   // 两两配对，落单的一项也占一整行
}

// ★ 分节行的高度**不是** 25，而且**首个与其后不同**。
//   26.1 `OptionsList.addHeader`（`OptionsList.java:52-56`）：
//       int paddingTop = children().isEmpty() ? 0 : lineHeight * 2;   // lineHeight = 9
//       addEntry(new HeaderEntry(...), paddingTop + lineHeight + 4);
//   于是首个标题高 0 + 9 + 4 = 13，其后每个高 18 + 9 + 4 = 31。
//   那 18 是**与上一节之间的留白**——所以它属于标题行本身，不是上一行的下边距。
inline constexpr int kOptionsHeaderLineHeight = 9;
inline constexpr int kOptionsHeaderPadding = 4;
inline constexpr int kOptionsHeaderFirstHeight =
    kOptionsHeaderLineHeight + kOptionsHeaderPadding;                 // 13
inline constexpr int kOptionsHeaderLaterHeight =
    kOptionsHeaderLineHeight * 2 + kOptionsHeaderLineHeight + kOptionsHeaderPadding;  // 31

static_assert(kOptionsHeaderFirstHeight == 13, "first header entry is 13 tall");
static_assert(kOptionsHeaderLaterHeight == 31, "a later header entry is 31 tall");

// 第 `row` 行是什么：标题还是设置行，多高。
//
// ★ 与绑定列表的 `keyBindListRow` 同构，理由也一样：**行号与控件序号不是倍数关系**，
//   因为标题行占一行却不产生控件。一旦有了变高，"行号 → 像素位置"也不再是乘法，
//   所以这两件事都必须从一张表里问出来，不能各自算。
struct OptionsRowInfo final {
    bool isHeader = false;
    int height = kOptionsRowHeight;
    std::string_view headerKey{};
    std::string_view headerFallback{};

    [[nodiscard]] constexpr bool operator==(const OptionsRowInfo&) const = default;
};

[[nodiscard]] constexpr OptionsRowInfo optionsRowAt(std::span<const OptionsGroup> groups,
                                                    std::size_t row) {
    std::size_t seen = 0;
    bool sawAnyEntry = false;
    for (const OptionsGroup& group : groups) {
        const std::size_t rows = optionsGroupRowCount(group);
        if (row < seen + rows) {
            if (group.kind == OptionsGroupKind::Header) {
                return OptionsRowInfo{true,
                                      sawAnyEntry ? kOptionsHeaderLaterHeight
                                                  : kOptionsHeaderFirstHeight,
                                      group.headerKey, group.headerFallback};
            }
            return OptionsRowInfo{false, kOptionsRowHeight, {}, {}};
        }
        seen += rows;
        // 「首个」的判据是**列表里已经有条目**，不是「已经有过标题」——
        // 一个前面摆了控件的标题，即使它是第一个标题，也要那 18 的留白。
        if (group.count > 0U || group.kind == OptionsGroupKind::Header) {
            sawAnyEntry = true;
        }
    }
    return OptionsRowInfo{false, kOptionsRowHeight, {}, {}};
}

// 第 `row` 行的顶边相对列表视口顶部的**像素**偏移。
//
// ★ 等高时它等于 `row * 25`；有了标题行就不再是乘法了。凡是从行号求 y 的地方
//   都必须走它——照 `row * kOptionsRowHeight` 算，标题行之后的每一行都会偏。
[[nodiscard]] constexpr int optionsRowTop(std::span<const OptionsGroup> groups, std::size_t row) {
    int top = 0;
    for (std::size_t index = 0; index < row; ++index) {
        top += optionsRowAt(groups, index).height;
    }
    return top;
}

// 从第 `firstRow` 行起，高 `viewportHeight` 的视口里**完整**装得下几行。
//
// 等高时它等于 `viewportHeight / 25`，与 `ScrollList::visibleRows()` 一致；
// 变高时必须逐行累加——一屏能装几行取决于你从哪一行开始看。
[[nodiscard]] constexpr std::size_t optionsVisibleRows(std::span<const OptionsGroup> groups,
                                                       std::size_t firstRow, int viewportHeight,
                                                       std::size_t totalRows) {
    if (viewportHeight <= 0) {
        return 0U;
    }
    int used = 0;
    std::size_t rows = 0;
    for (std::size_t row = firstRow; row < totalRows; ++row) {
        const int height = optionsRowAt(groups, row).height;
        if (used + height > viewportHeight) {
            break;
        }
        used += height;
        ++rows;
    }
    return rows;
}

// 最多能滚到第几行——**变高版**。
//
// ★ 等高时它是 `总行数 - 视口/行高`，一句除法；有了分节行（13 / 31）就不行了：
//   最后一屏能装几行取决于**末尾那几行各自多高**。从末尾往回累加到装不下为止，
//   剩下的就是起点。用等高公式的症状是"滚到底还差一行"或者"滚过头露出空白"。
[[nodiscard]] constexpr std::size_t optionsMaxFirstRow(std::span<const OptionsGroup> groups,
                                                       int viewportHeight,
                                                       std::size_t totalRows) {
    if (viewportHeight <= 0) {
        return totalRows;
    }
    int used = 0;
    std::size_t rows = 0;
    for (std::size_t index = totalRows; index > 0U; --index) {
        const int height = optionsRowAt(groups, index - 1U).height;
        if (used + height > viewportHeight) {
            break;
        }
        used += height;
        ++rows;
    }
    return totalRows > rows ? totalRows - rows : 0U;
}

[[nodiscard]] constexpr OptionsSlot optionsGroupedSlot(std::span<const OptionsGroup> groups,
                                                       std::size_t index) {
    std::size_t row = 0;
    std::size_t seen = 0;
    for (const OptionsGroup& group : groups) {
        // ★ 标题组 count == 0，所以它**永远不会**吞掉一个设置项序号——但它
        //   `optionsGroupRowCount` 是 1，行号照样往前走。这正是"行 ≠ 控件"。
        if (group.count > 0U && index < seen + group.count) {
            const std::size_t withinGroup = index - seen;
            if (group.kind == OptionsGroupKind::Big) {
                return OptionsSlot{row + withinGroup, 0, true};
            }
            return OptionsSlot{row + withinGroup / 2U, static_cast<int>(withinGroup % 2U), false};
        }
        seen += group.count;
        row += optionsGroupRowCount(group);
    }
    return OptionsSlot{row, 0, false};
}

// 各组一共占多少行。
[[nodiscard]] constexpr std::size_t optionsGroupedRowCount(std::span<const OptionsGroup> groups) {
    std::size_t rows = 0;
    for (const OptionsGroup& group : groups) {
        rows += optionsGroupRowCount(group);
    }
    return rows;
}

// `addBig`：独占一行，宽度等于行宽。
[[nodiscard]] constexpr UiRect optionsBigCell(const ScrollList& list, std::size_t visibleIndex) {
    const auto row = scrollListRow(list, visibleIndex);
    return {row.x, row.y + static_cast<float>(kListEntryPadding),
            static_cast<float>(kOptionsBigWidth), static_cast<float>(kOptionsWidgetHeight)};
}

// UI-6c：Controls 枢纽上 `addSmall(...)` 的分组，按 26.1 的调用顺序。
//
//   addSmall(mouse_settings, keybinds)                                     → 2 项
//   addSmall(toggleCrouch, toggleSprint, toggleAttack, toggleUse,
//            autoJump, sprintWindow, operatorItemsTab)                 → 7 项
//
// 两组之间那道行边界是**语义分组**（`ControlsScreen.addOptions()` 的两次调用），
// 不是排版巧合：把它们摊平成一组，keybinds 会和 toggleCrouch 挤在同一行。
inline constexpr std::array<OptionsGroup, 2> kControlsHubGroups{
    {{2U, OptionsGroupKind::Small}, {7U, OptionsGroupKind::Small}}};

// UI-6f（D15）：视频设置，照 26.1 `VideoSettingsScreen.addOptions()` 的**真实**结构：
//   addHeader(DISPLAY)     → addBig(fullscreenResolution) → addSmall(7 项)
//   addHeader(QUALITY)     → addBig(graphicsPreset)       → addSmall(17 项)
//   addHeader(PREFERENCES) → addSmall(4 项)
//
// ★ 三件事在这一轮之前都是错的（偏差 D15）：**缺三个分节行**、**分节顺序反了**
//   （26.1 是 Display 在前）、以及**第一个大按钮是全屏分辨率而不是预设**。
//   UI-6d 立项时我给出的分组表还没查到分节行，那是事后更正的事实。
//
// 本作只补有后端的项，所以每组的项数比 vanilla 少，**但分组结构与顺序照抄**：
//   Display    : addBig(Resolution) + addSmall(FrameRateLimit, Vsync, GuiScale)
//   Quality    : addBig(GraphicsPreset 置灰) + addSmall(11 项)
//   Preferences: 26.1 那 4 项（自动保存指示器 / 暗角 / 攻击指示器 / 区块淡入）
//                本作**一个都没有后端**，整组不放——不做点不动的空壳（已登记 D16）。
inline constexpr std::array<OptionsGroup, 6> kVideoSettingsGroups{{
    {0U, OptionsGroupKind::Header, "options.video.display.header", "Display"},
    {1U, OptionsGroupKind::Big},
    {3U, OptionsGroupKind::Small},
    {0U, OptionsGroupKind::Header, "options.video.quality.header", "Quality"},
    {1U, OptionsGroupKind::Big},
    {11U, OptionsGroupKind::Small},
}};

// UI-6e ②：音乐与声音，照 26.1 `SoundOptionsScreen.addOptions()` 的五次调用：
//   addBig(MASTER)                          → 1 项，独占一行
//   addSmall(其余 9 类)                      → 9 项，5 行（最后一项落单）
//   addBig(soundDevice)                     → 1 项（本作置灰）
//   addSmall(showSubtitles, directionalAudio) → 2 项
//   addSmall(musicFrequency, musicToast)      → 2 项（本作两个都置灰）
inline constexpr std::array<OptionsGroup, 5> kSoundSettingsGroups{
    {{1U, OptionsGroupKind::Big},
     {9U, OptionsGroupKind::Small},
     {1U, OptionsGroupKind::Big},
     {2U, OptionsGroupKind::Small},
     {2U, OptionsGroupKind::Small}}};

// UI-6e ④：Options 主页。26.1 `OptionsScreen.init()`：
//   副页眉一行两项：fov 滑块 + （世界内 Difficulty / 世界外 Online）
//   内容区一张 2 列 GridLayout，十个跳转按钮
// ★ 本作把副页眉那两项放进内容区第一行（26.1 在页眉里，偏差 D23）。
// 项数恒定 12（不随"在不在世界里"变），行数恒定 6。
// ★ UI-6f（D23）：只剩十个跳转。fov 与 Difficulty/Online 那两项**搬进了副页眉**，
//   与 26.1 一致（`OptionsScreen.init()` 的 header 是 vertical layout：标题 + 一行控件）。
inline constexpr std::array<OptionsGroup, 1> kOptionsHubGroups{{{10U, OptionsGroupKind::Small}}};

// 这一页有几个控件摆在**副页眉**里（页眉那一行），其余才进内容区。
//
// ★ 这样版式仍是 HeaderFooterList，不必为 Options 单开一种——副页眉是"页眉里有几个
//   控件"这一个数，不是另一种版面。大多数页面是 0。
[[nodiscard]] constexpr std::size_t optionsSubHeaderCount(PageId page) {
    return page == PageId::Options ? 2U : 0U;
}

// 高级图形：本项目自有页，两项一组。RN-47 之后是四项。
inline constexpr std::array<OptionsGroup, 1> kAdvancedGraphicsGroups{
    {{4U, OptionsGroupKind::Small}}};

// UI-11 / A2：26.1 `FontOptionsScreen.addOptions` 就一句
// `list.addSmall({forceUnicodeFont, japaneseGlyphVariants})`——两项、双列、无分节行。
inline constexpr std::array<OptionsGroup, 1> kFontSettingsGroups{{
    {2U, OptionsGroupKind::Small, {}, {}},
}};

// ★ 分节行**不产生控件**，所以它的 `count` 必须是 0。写成非 0 会让它吞掉一个设置项
//   序号，而那一项之后的每一个控件都会错位——症状是"少了一个控件，其余全部串行"。
//   `optionsGroupedSlot` 里那个 `group.count > 0U` 只是防御，真正的护栏是这条编译期检查。
[[nodiscard]] constexpr bool optionsHeadersProduceNoWidgets(std::span<const OptionsGroup> groups) {
    for (const OptionsGroup& group : groups) {
        if (group.kind == OptionsGroupKind::Header && group.count != 0U) {
            return false;
        }
        // 反过来也要：非标题组必须有名字为空的 header 字段，否则说明写错了 kind。
        if (group.kind != OptionsGroupKind::Header && !group.headerKey.empty()) {
            return false;
        }
    }
    return true;
}

static_assert(optionsHeadersProduceNoWidgets(kControlsHubGroups),
              "a header row must not consume an option index");
static_assert(optionsHeadersProduceNoWidgets(kVideoSettingsGroups),
              "a header row must not consume an option index");
static_assert(optionsHeadersProduceNoWidgets(kAdvancedGraphicsGroups),
              "a header row must not consume an option index");
static_assert(optionsHeadersProduceNoWidgets(kSoundSettingsGroups),
              "a header row must not consume an option index");
static_assert(optionsHeadersProduceNoWidgets(kOptionsHubGroups),
              "a header row must not consume an option index");
static_assert(optionsHeadersProduceNoWidgets(kFontSettingsGroups),
              "a header row must not consume an option index");

// 这一屏的 addSmall 分组。三段式版面的页脚按钮不在其中（它由 buttonCount 单独认出来）。
[[nodiscard]] constexpr std::span<const OptionsGroup> optionsGroupsOf(PageId page) {
    switch (page) {
    case PageId::VideoSettings:
        return kVideoSettingsGroups;
    case PageId::AdvancedGraphics:
        return kAdvancedGraphicsGroups;
    case PageId::SoundSettings:
        return kSoundSettingsGroups;
    case PageId::Options:
        return kOptionsHubGroups;
    case PageId::FontSettings:
        return kFontSettingsGroups;
    // ★ UI-11 顺手修掉的一处漏洞：这里原本带着 `default: break;`。
    //   本线的规矩是「按 PageId 分派的 switch 一律不带 default」（README 护栏 19），
    //   而这一处漏了——加一页时它**不会**被 -Wswitch 点名，新页会静默拿到
    //   `kControlsHubGroups`，症状是"新设置屏里显示的是 Controls 那几项"。
    //   逐个列出之后，加一页编译器会指名道姓（实测：加 FontSettings 时它没吭声，
    //   而别的八处 switch 全都点名了）。
    case PageId::Controls:
    // UI-11 / A5：提示屏不是设置列表页，没有 addSmall 分组。
    case PageId::AdvancedGraphicsNotice:
    case PageId::Title:
    case PageId::WorldList:
    case PageId::CreateWorld:
    case PageId::EditWorld:
    case PageId::ConfirmDelete:
    case PageId::Loading:
    case PageId::Game:
    case PageId::Pause:
    case PageId::Death:
    case PageId::Language:
    case PageId::KeyBinds:
    case PageId::Accessibility:
    case PageId::ResourcePacks:
    case PageId::Count:
        break;
    }
    return kControlsHubGroups;
}


// 一页的设置项**在滚动窗口里**的可见范围，以及某个设置项落在哪个可见行。
//
// ★ 26.1 的 `OptionsList` 是**滚动列表**（`ContainerObjectSelectionList`）。这一点在
//   Controls 那一屏看不出来——它只有 5 行、装得下。Video Settings 装不下：本作只补了
//   有后端的项就已经 9 行，而 1280x720 @ scale 3 的内容区是 174 逻辑像素、只放得下 6 行
//   （26.1 那一屏有 28 项 = 15 行，更是必然要滚）。不滚的后果不是"看不到下面几项"，
//   是**最后两行压在页脚的 Done 上**。
//
// 窗口以**行**为单位，与绑定列表同构：装配只造窗口内的控件，页面因此永远不会超出
// 布局容量，也不会有画在屏幕外却仍能被 Tab 停留的控件。
struct OptionsWindow final {
    std::size_t firstRow = 0;
    std::size_t rowCount = 0;

    [[nodiscard]] constexpr bool contains(std::size_t row) const {
        return row >= firstRow && row < firstRow + rowCount;
    }
};

// 这一页的设置项一共占几行（不含页脚按钮）。
[[nodiscard]] constexpr std::size_t optionsRowCountOf(PageId page) {
    return optionsGroupedRowCount(optionsGroupsOf(page));
}

// 这一页一共有几个设置项。
[[nodiscard]] constexpr std::size_t optionsCountOf(std::span<const OptionsGroup> groups) {
    std::size_t total = 0;
    for (const OptionsGroup& group : groups) {
        total += group.count;
    }
    return total;
}

// **已装配控件的序号** → 它在视口里的格位。`optionsGroupedSlot` 的滚动版。
//
// ★ 这个函数存在的全部理由：装配只造窗口里的控件（PageBuilder 的 `optionVisible`），
//   所以 `Page` 里第 i 个控件**不是**第 i 个设置项——被滚上去的那些不占序号。布局若
//   直接把 i 喂给 `optionsGroupedSlot`，滚到第 3 行时每个控件都会画在它上面 3 行的位置：
//   屏幕上看着像"滚动条动了、内容没动"，实际是装配侧与布局侧对 i 的含义不一致。
//   这与 UI-6b 那次闪退是同一族缺陷（两处各自解释同一个下标），所以这里只有一个函数，
//   两侧都走它。
//
// 只需要 `firstRow` 不需要窗口高度：已装配的控件必定都在窗口内，因此"跳过滚上去的、
// 取第 i 个"就够了。
[[nodiscard]] constexpr OptionsSlot optionsScrolledSlot(std::span<const OptionsGroup> groups,
                                                        std::size_t firstRow,
                                                        std::size_t assembledIndex) {
    const std::size_t optionCount = optionsCountOf(groups);
    std::size_t assembled = 0;
    for (std::size_t option = 0; option < optionCount; ++option) {
        const OptionsSlot slot = optionsGroupedSlot(groups, option);
        if (slot.row < firstRow) {
            continue;
        }
        if (assembled == assembledIndex) {
            return OptionsSlot{slot.row - firstRow, slot.column, slot.big};
        }
        ++assembled;
    }
    return OptionsSlot{assembled, 0, false};
}


// UI-11 / A1：**Ctrl + 滚轮改 GUI 缩放**（spec §1.1，26.1
// `VideoSettingsScreen.mouseScrolled:219-240`）。它是视频设置那一屏自己的交互，
// UI-3 就登记了这一笔，一直没做。
//
// 26.1 那几行照抄，一条都不化简：
//
//     adjustedOld = (old == 0) ? maxInclusive + 1 : old      // ★ Auto 当成 max+1
//     newValue    = adjustedOld + signum(scrollY)
//     接受条件： newValue != 0 && newValue <= maxInclusive && newValue >= minInclusive
//
// ★ 那个 `Auto 当成 max + 1` 是这段的灵魂：Auto 解出来的档**就是** max，把它当成
//   "比 max 还大一档"，于是从 Auto 往下滚正好落到 max（看得见的档位不跳），
//   而往上滚会越界被拒——Auto 已经是最上面那一档了。
// ★ `newValue != 0` 同样不能省：从 1 往下滚得到 0，26.1 **拒绝**它，也就是
//   **滚轮切不回 Auto**（只能用按钮循环回去）。少了这条，用户会在 1 和 Auto 之间
//   反复横跳而不知道自己切到了哪一档。
//
// `scrollDirection` 用的是**本作滚轮回调的方向**（上推 = -1，见 glfwSetScrollCallback），
// 与 26.1 的 `signum(scrollY)` 正好相反，所以这里取负——上推滚轮 = 放大。
// 返回 nullopt 表示这一次滚动**不改变**缩放（越界），调用方应当保持原值。
[[nodiscard]] constexpr std::optional<int> guiScaleAfterCtrlScroll(int current,
                                                                   int scrollDirection,
                                                                   int maxInclusive) {
    constexpr int kMinInclusive = 1;
    if (maxInclusive < kMinInclusive) {
        return std::nullopt;
    }
    const int adjusted = current == 0 ? maxInclusive + 1 : current;
    const int step = scrollDirection < 0 ? 1 : scrollDirection > 0 ? -1 : 0;
    const int next = adjusted + step;
    if (next == 0 || next > maxInclusive || next < kMinInclusive) {
        return std::nullopt;
    }
    return next;
}

} // namespace mc::ui
