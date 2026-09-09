#pragma once

// UI-6e：创建世界那一屏的版面（26.1 `CreateWorldScreen`，spec §8.2）。
//
// ★ 它存在的理由是一次真实的版面事故。表单本来是**挂在按钮块上沿往上堆**的：
//
//       buttonTop  = logicalHeight / 2 - 按钮数 * 12      // menuButton 的居中算法
//       名字标签 y = buttonTop - 80
//
//   1280x720 @ scale 3 的逻辑画布是 427x240，于是 buttonTop = 120 - 60 = 60，
//   名字标签落在 **-20** —— 画布外。截图里世界名输入框只剩下半截，"Save folder"
//   那行还和标题糊在一起。加一个种子框就撞上了，因为往上堆的高度从 30 涨到了 80。
//
//   **往上堆的版面没有下界**：它的起点由画布中线决定，而它要多高由内容决定，
//   两者一冲突就出界，而且是静默的——没有任何断言会红，只有截图能看出来。
//
// 26.1 的做法是三段式：页眉标题、内容区、页脚 Create/Cancel
// （`CreateWorldScreen` 用 `HeaderAndFooterLayout` + `TabManager`）。内容**从上往下**排，
// 所以它永远不会越过页眉；装不下时溢出的是下边，那是可以被断言抓住的。
//
// UI-9：**标签页落地了**（偏差 D21）。26.1 `CreateWorldScreen:254-255` 挂
// GameTab / WorldTab / MoreTab 三页，而标签栏**同时取代页眉**——
// `repositionElements` 把 `layout.setHeaderHeight(tabNavigationBar.getRectangle().bottom())`，
// 这一屏没有另一行标题。所以这里的三段式页眉高是 `kTabBarHeight`（24），不是 33。

#include "ui/HeaderAndFooterLayout.hpp"
#include "ui/HudLayout.hpp"
#include "ui/MenuSystem.hpp"
#include "ui/TabBar.hpp"
#include "ui/TextMetrics.hpp"

#include <cstddef>

namespace mc::ui {

// UI-9：三个标签页（26.1 GameTab / WorldTab / MoreTab）。
inline constexpr std::size_t kCreateWorldTabCount = 3U;
static_assert(kCreateWorldTabCount == static_cast<std::size_t>(CreateWorldTab::Count),
              "标签页数与 CreateWorldTab 必须一致——两份表述迟早分岔");

// 输入框尺寸（spec §2.4 的常用值，26.1 `CreateWorldScreen` 的两个 EditBox 同尺寸）。
inline constexpr int kCreateWorldFieldWidth = 200;
inline constexpr int kCreateWorldFieldHeight = 20;
// 标签行与它下面那个框之间的距离，以及两组之间的距离。
inline constexpr int kCreateWorldLabelGap = 2;
inline constexpr int kCreateWorldGroupGap = 6;
// ★ `ui::kFontLineHeight` 是 `float`（绘制侧要乘 scale 才好用），而版面必须在**整数**
//   网格上解（护栏 5）。这里显式取整一次，下面全用整数——让 int 与 float 在同一个
//   算式里混，narrowing 是静默的，而版面差一像素没有任何断言会红。
inline constexpr int kCreateWorldLineHeight = static_cast<int>(kFontLineHeight);
static_assert(kCreateWorldLineHeight == 9, "vanilla 的行高是 9 个逻辑像素");

// 文件夹提示那一行的高度（一行文字）。
inline constexpr int kCreateWorldHintHeight = kCreateWorldLineHeight;
// 内容区里那些循环按钮的高度与行距。
inline constexpr int kCreateWorldButtonHeight = 20;
inline constexpr int kCreateWorldButtonGap = 4;

// UI-9：这一屏的三段式——**页眉就是标签栏**（高 24）。
[[nodiscard]] constexpr HeaderAndFooterLayout createWorldFrame(int logicalWidth,
                                                               int logicalHeight) {
    return HeaderAndFooterLayout{logicalWidth, logicalHeight, kTabBarHeight,
                                 kHeaderAndFooterHeight};
}

// 一屏的全部矩形，**逻辑像素**。乘 scale 是调用方最后一步的事（护栏 5）。
//
// ★ 哪些字段有意义取决于**当前标签页**：Game 页有名字框与文件夹提示，World 页有种子框，
//   More 页两样都没有。不属于当前页的矩形是空的（宽高为 0）——绘制侧据此跳过，
//   而不是各自再判一次"现在是哪一页"。
struct CreateWorldLayout final {
    CreateWorldTab tab = CreateWorldTab::Game;
    UiRect nameLabel{};
    UiRect nameField{};
    UiRect folderHint{};
    UiRect seedLabel{};
    UiRect seedField{};
    // 内容区里的按钮带起点（游戏模式 / 难度 / 允许作弊按这个往下排）。
    int optionButtonsTop = 0;
    // 页脚那两个按钮（Create New World / Back），横排。
    UiRect footerLeft{};
    UiRect footerRight{};

    [[nodiscard]] constexpr bool operator==(const CreateWorldLayout&) const = default;
};

// Game 页表单部分（标签 + 框 + 一行提示）多高。
[[nodiscard]] constexpr int createWorldFormHeight() {
    return kCreateWorldLineHeight + kCreateWorldLabelGap + kCreateWorldFieldHeight +
           kCreateWorldLabelGap + kCreateWorldHintHeight;
}

[[nodiscard]] constexpr CreateWorldLayout createWorldLayout(int logicalWidth, int logicalHeight,
                                                            CreateWorldTab tab) {
    const auto frame = createWorldFrame(logicalWidth, logicalHeight);
    const auto content = frame.contentBox();
    const int left = logicalWidth / 2 - kCreateWorldFieldWidth / 2;
    const int width = kCreateWorldFieldWidth;

    // ★ 从内容区**顶部**往下排，不是从按钮往上堆。
    int y = static_cast<int>(content.y);

    CreateWorldLayout out;
    out.tab = tab;
    // Game 页：世界名输入框 + 文件夹提示（26.1 `GameTab`：labeledElement(nameEdit)）。
    if (tab == CreateWorldTab::Game) {
        out.nameLabel = {static_cast<float>(left), static_cast<float>(y),
                         static_cast<float>(width), static_cast<float>(kCreateWorldLineHeight)};
        y += kCreateWorldLineHeight + kCreateWorldLabelGap;
        out.nameField = {static_cast<float>(left), static_cast<float>(y),
                         static_cast<float>(width), static_cast<float>(kCreateWorldFieldHeight)};
        y += kCreateWorldFieldHeight + kCreateWorldLabelGap;
        out.folderHint = {static_cast<float>(left), static_cast<float>(y),
                          static_cast<float>(width), static_cast<float>(kCreateWorldHintHeight)};
        y += kCreateWorldHintHeight + kCreateWorldGroupGap;
    }
    // World 页：种子框（26.1 `WorldTab`：labeledElement(seedEdit)，跨两列）。
    if (tab == CreateWorldTab::World) {
        out.seedLabel = {static_cast<float>(left), static_cast<float>(y),
                         static_cast<float>(width), static_cast<float>(kCreateWorldLineHeight)};
        y += kCreateWorldLineHeight + kCreateWorldLabelGap;
        out.seedField = {static_cast<float>(left), static_cast<float>(y),
                         static_cast<float>(width), static_cast<float>(kCreateWorldFieldHeight)};
        y += kCreateWorldFieldHeight + kCreateWorldGroupGap;
    }
    out.optionButtonsTop = y;

    // 页脚两个按钮横排，间距 8（与语言页、绑定列表页的页脚同形：
    // `LinearLayout.horizontal().spacing(8)`）。
    constexpr int kFooterGap = 8;
    constexpr int kFooterWidth = 150;
    const auto footer = frame.footerBox();
    const int footerY =
        static_cast<int>(footer.y) + frame.footerHeight / 2 - kFooterButtonHeight / 2;
    const int pairLeft = logicalWidth / 2 - (kFooterWidth * 2 + kFooterGap) / 2;
    out.footerLeft = {static_cast<float>(pairLeft), static_cast<float>(footerY),
                      static_cast<float>(kFooterWidth),
                      static_cast<float>(kFooterButtonHeight)};
    out.footerRight = {static_cast<float>(pairLeft + kFooterWidth + kFooterGap),
                       static_cast<float>(footerY), static_cast<float>(kFooterWidth),
                       static_cast<float>(kFooterButtonHeight)};
    return out;
}

// 内容区里第 `index` 个循环按钮（游戏模式 / 难度 / 允许作弊）。
[[nodiscard]] constexpr UiRect createWorldOptionButton(const CreateWorldLayout& layout,
                                                       int logicalWidth, std::size_t index) {
    const int left = logicalWidth / 2 - kCreateWorldFieldWidth / 2;
    return {static_cast<float>(left),
            static_cast<float>(layout.optionButtonsTop +
                               static_cast<int>(index) *
                                   (kCreateWorldButtonHeight + kCreateWorldButtonGap)),
            static_cast<float>(kCreateWorldFieldWidth),
            static_cast<float>(kCreateWorldButtonHeight)};
}

} // namespace mc::ui
