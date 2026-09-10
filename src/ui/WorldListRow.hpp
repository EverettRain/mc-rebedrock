#pragma once

// UI-11 / A6：世界选择列表里**一行**的内部几何（26.1
// `WorldSelectionList.WorldListEntry.extractContent():497-506` 与
// `AbstractSelectionList.Entry.getContentX/Y/Width/Height():471-501`）。
//
// 本作从前的世界行是自造的：行高 22、上下两层深灰底、名字加一行 0.75 倍缩放的
// "Seed 12345"。26.1 的那一行是 **36 高**、左边一张 32x32 的世界缩略图、右边三行字：
//   1. 世界显示名（白）
//   2. `<存档目录名> (<最后游玩日期>)`（灰 0xFF808080）
//   3. 游戏模式 + 版本（同灰）
//
// ★ spec §7 的那张 `<WorldRow>` 表里第三行写的是 `top + 24`，**是旧值**：
//   26.1 源码是 `getContentY() + 9 + 9 + 3` = contentY + 21。已按源码更正并登记。
//
// 单位一律逻辑像素，整数。乘 GUI 缩放是调用方最后一步的事。

#include "ui/HudLayout.hpp"
#include "ui/ScrollList.hpp"

#include <string>
#include <string_view>

namespace mc::ui {

// `WorldSelectionList` 的构造：`super(minecraft, width, height, 0, 36)`（:116）。
inline constexpr int kWorldRowHeight = 36;
// `WorldListEntry.ICON_SIZE`（:403）。
inline constexpr int kWorldIconSize = 32;
// `getTextX() = getContentX() + 32 + 3`（:571-573）。
inline constexpr int kWorldRowIconTextGap = 3;
// `Entry.getContentX/Y` 各 +2，`getContentWidth/Height` 各 -4（:471-481）。
inline constexpr int kWorldRowContentInset = 2;

// 一行里那几块矩形，坐标相对**行的左上角**（调用方加上行的 x/y）。
struct WorldRowParts final {
    UiRect icon{};      // 32x32 的缩略图
    float textX = 0.0F; // 三行字共用的左缘
    float nameY = 0.0F; // 第 1 行
    float metaY = 0.0F; // 第 2 行：目录名 (日期)
    float infoY = 0.0F; // 第 3 行：模式 + 版本
};

[[nodiscard]] constexpr WorldRowParts worldRowParts(const UiRect& row) {
    const auto inset = static_cast<float>(kWorldRowContentInset);
    const float contentX = row.x + inset;
    const float contentY = row.y + inset;
    return {
        {contentX, contentY, static_cast<float>(kWorldIconSize),
         static_cast<float>(kWorldIconSize)},
        contentX + static_cast<float>(kWorldIconSize + kWorldRowIconTextGap),
        // ★ 三条 y 照抄 `extractContent`，**不要**化简成 `+1 + i*10`：
        //     name: getContentY() + 1
        //     meta: getContentY() + 9 + 3
        //     info: getContentY() + 9 + 9 + 3
        //   第一行到第二行差 11，第二行到第三行差 9——它本来就不是等距的。
        contentY + 1.0F,
        contentY + 9.0F + 3.0F,
        contentY + 9.0F + 9.0F + 3.0F,
    };
}

// 三行字各自的最大宽度（超出就裁）。
//
// ★ 26.1 写的是 `list.getRowWidth() - this.getTextX() - 2`（:421）——一个宽度**减去
//   一个绝对 x**。它之所以还对，是因为这一行在**构造函数**里执行，那时 entry 的
//   x 还是 0，于是 `getTextX()` 恰好等于 `2 + 32 + 3 = 37`，算出 270 - 37 - 2 = 231。
//   照抄这个数，但不照抄那条式子——它只在 x == 0 时成立，写成函数会在别处说假话。
inline constexpr int kWorldRowMaxTextWidth =
    kWorldSelectionRowWidth - (kWorldRowContentInset + kWorldIconSize + kWorldRowIconTextGap) - 2;

// 第 2、3 行的颜色：26.1 `withColor(-8355712)` = 0xFF808080。
inline constexpr float kWorldRowSecondaryChannel = 128.0F / 255.0F;

// 26.1 `extractContent:507`：悬停时在图标上盖一层 `-1601138544` = 0xA0909090。
inline constexpr float kWorldIconHoverChannel = 144.0F / 255.0F;
inline constexpr float kWorldIconHoverAlpha = 160.0F / 255.0F;

// 第 3 行：26.1 `LevelSummary.createInfo():166-186`。
//
//     gameMode.<模式名>  [, selectWorld.commands]  [, selectWorld.experimental]
//     , selectWorld.version <版本名>
//
// ★ 本作的 `SaveSummary` 没有"开了作弊吗"与"用了实验性数据包吗"，那两段因此不画；
//   两条差异登记在偏差表里，**不在这里编一个来源**。
// ★ 译文由调用方给（这一层不碰语言表），拼接留在这里是为了那两个逗号与那个空格
//   有地方被断言——26.1 是 `", "` 加 `selectWorld.version` 加 `CommonComponents.SPACE`。
[[nodiscard]] inline std::string worldRowInfoLine(std::string_view gameModeLabel,
                                                  std::string_view versionWord,
                                                  std::string_view versionName) {
    std::string line{gameModeLabel};
    if (versionName.empty()) {
        // 旧存档的版本块是重建出来的，`versionName` 是空串（SaveVersionHeader::derived）。
        // 那时 26.1 会画 `getWorldVersionName()` 的兜底，本作没有那份兜底，就不画这一段。
        return line;
    }
    line += ", ";
    line += versionWord;
    line += ' ';
    line += versionName;
    return line;
}

// 第 2 行：`<存档目录名> (<最后游玩日期>)`（`WorldSelectionList:429-434`）。
//
// ★ 26.1 的判据是 `lastPlayed != -1L`——"没有记录"是 -1，不是 0。本作的
//   `SaveSummary::lastPlayedUnixSeconds` 默认 0 且从不写负数，所以这里按
//   **非正数即没有记录**处理，并把这条差异写在这里而不是让调用方各自猜。
//
// ★ 日期串由调用方给：格式化要 `<ctime>` 与本地时区，那不是版面的事，
//   而且做成参数之后这条拼接是纯的、测得到。
[[nodiscard]] inline std::string worldRowMetaLine(std::string_view identifier,
                                                  std::string_view formattedDate) {
    std::string line{identifier};
    if (!formattedDate.empty()) {
        line += " (";
        line += formattedDate;
        line += ")";
    }
    return line;
}

} // namespace mc::ui
