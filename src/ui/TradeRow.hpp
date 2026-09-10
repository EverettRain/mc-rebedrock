#pragma once

// MERCH-1：交易屏里**一行交易**的内部几何，以及那一屏几块固定装饰的落点
// （26.1 `MerchantScreen.extractContents():168-196` 与 `extractProgressBar` /
// `extractScroller` / `extractBackground`）。
//
// 单位一律逻辑像素，坐标相对**面板左上角**（调用方加上 `HudLayout::tradingPanel()`）。
//
// ★ 式子照抄，不做代数化简。26.1 写的是 `xo + 5 + 35 + 20`、`offerY = yo + 16 + 1`、
//   `decorHeight = offerY + 2` —— 那几个加数各有来历（5 是按钮左缘、35 是第二个代价格
//   相对按钮的偏移、20 是箭头相对第二格的偏移），合成一个 60 之后就没人知道改哪个了。

#include "ui/HudLayout.hpp"

namespace mc::ui {

// `MerchantScreen:40` NUMBER_OF_OFFER_BUTTONS。
inline constexpr int kTradeVisibleRows = 7;
// `TRADE_BUTTON_X` / `TRADE_BUTTON_WIDTH` / `TRADE_BUTTON_HEIGHT`（:41-43）。
inline constexpr int kTradeButtonX = 5;
inline constexpr int kTradeButtonWidth = 88;
inline constexpr int kTradeButtonHeight = 20;
// `SCROLLER_WIDTH` / `SCROLLER_HEIGHT` / `SCROLL_BAR_START_X` / `SCROLL_BAR_TOP_POS_Y`
// （:44-48）。轨道高 `SCROLL_BAR_HEIGHT = 139`，滑块能走的最大位移是 113（:143）。
inline constexpr int kTradeScrollerX = 94;
inline constexpr int kTradeScrollerY = 18;
inline constexpr int kTradeScrollerWidth = 6;
inline constexpr int kTradeScrollerHeight = 27;
inline constexpr int kTradeScrollerMaxOffset = 113;
// `PROGRESS_BAR_X` / `PROGRESS_BAR_Y`（:34-35），条宽 102 高 5（`extractProgressBar`）。
inline constexpr int kTradeLevelBarX = 136;
inline constexpr int kTradeLevelBarY = 16;
inline constexpr int kTradeLevelBarWidth = 102;
inline constexpr int kTradeLevelBarHeight = 5;
// 选中那条交易缺货时盖在结果区上的大叉：`leftPos + 83 + 99, topPos + 35`，28x21
// （`extractBackground`）。★ 它是**整屏一个**、只属于选中的那一条，不是逐行的。
inline constexpr int kTradeOutOfStockX = 83 + 99;
inline constexpr int kTradeOutOfStockY = 35;
inline constexpr int kTradeOutOfStockWidth = 28;
inline constexpr int kTradeOutOfStockHeight = 21;
// 26.1 的村民等级上限：到顶就不画等级条（`extractProgressBar` 的 `traderLevel < 5`）。
inline constexpr int kTradeMaxVillagerLevel = 5;

// 第 `row` 个交易按钮的上缘：首行 18，行距 20（`MerchantScreen:74-78` 建按钮时的
// `yo + 18 + i * 20`）。★ 它比同一行的**物品**高 1 像素——物品走的是
// `offerY = 16 + 1`、按钮走的是 18。两者差 1 不是笔误，照抄。
[[nodiscard]] constexpr int kTradeButtonY(int row) { return 18 + row * 20; }

// 一行交易里那三样东西的落点。
struct TradeRowParts final {
    UiRect button{};   // 88x20 的行底（26.1 是一个 Button.Plain）
    UiRect wantsA{};   // 第一份代价
    UiRect wantsB{};   // 第二份代价（本作恒空，位置仍照画）
    UiRect arrow{};    // 10x9 的箭头（缺货时换另一张精灵）
    UiRect gives{};    // 给的东西
};

[[nodiscard]] constexpr TradeRowParts tradeRowParts(int row) {
    // `offerY = yo + 16 + 1`，每行 +20；`decorHeight = offerY + 2`。
    const int offerY = 16 + 1 + row * 20;
    const int decorHeight = offerY + 2;
    const auto slot = [decorHeight](int x) {
        return UiRect{static_cast<float>(x), static_cast<float>(decorHeight), 16.0F, 16.0F};
    };
    return {
        {static_cast<float>(kTradeButtonX),
         static_cast<float>(kTradeButtonY(row)),
         static_cast<float>(kTradeButtonWidth),
         static_cast<float>(kTradeButtonHeight)},
        slot(kTradeButtonX + 5),        // sellItem1X = xo + 5 + 5
        slot(kTradeButtonX + 35),       // costB     = xo + 5 + 35
        {static_cast<float>(kTradeButtonX + 35 + 20), static_cast<float>(decorHeight + 3), 10.0F,
         9.0F},                          // arrow     = xo + 5 + 35 + 20, decorHeight + 3
        slot(kTradeButtonX + 68),       // result    = xo + 5 + 68
    };
}

} // namespace mc::ui
