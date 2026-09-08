#pragma once

// 每一屏用哪一种按钮排版。
//
// 从前这是 `frontendButtonRect` 里一串 `if (page == …)`，末尾一个兜底 return。
// 那条链有两个毛病，都不是"长"：
//
//   1. **加一页不会有任何提示**，它会静默落到兜底的居中单列上。而"这一页的按钮
//      摆成了另一种版式"在画面上就是一屏摆得有点怪，没有东西会红。
//   2. 版式的**选择**与版式的**参数**混在一起读不开。
//
// 收成一个不带 `default` 的 switch：加一个 PageId，编译器 `-Wswitch` 会**指名道姓**
// 地报出来。这一条是刻意选 switch 而不是 `std::array` 表的理由——数组表要额外维护
// 一句 `static_assert(表长 == 枚举数)`，而它只会说"少了一个"，不说少了谁。

#include "ui/PageStack.hpp"

#include <cstdint>

namespace mc::ui {

enum class PageLayoutKind : std::uint8_t {
    // 屏幕正中一列（`HudLayout::menuButton`）。暂停、死亡、选项、实验性内容。
    CentredColumn,
    // 贴底的一条按钮带，单列（`bottomMenuButton`）。编辑世界、删除确认。
    BottomBand,
    // 贴底的按钮带，两列并排。世界列表、语言、绑定列表页脚。
    BottomBandTwoColumn,
    // 视频设置那张两列网格加独占一行的"完成"（`videoSettingsButton`）。
    VideoGrid,
    // 主菜单：spec §6.3 的整数版面，走 `titleScreenLayout`。
    TitleScreen,
    // 三段式版面里的一张 OptionsList 双列，页脚一个按钮（26.1 的 OptionsSubScreen）。
    HeaderFooterList,
};

// ★ 不带 `default:`。加一个 PageId 而不在这里给它一种版式，编译期就会被点名。
[[nodiscard]] constexpr PageLayoutKind pageLayoutKind(PageId page) {
    switch (page) {
    case PageId::Title:
        return PageLayoutKind::TitleScreen;
    case PageId::WorldList:
    case PageId::Language:
    case PageId::KeyBinds:
        return PageLayoutKind::BottomBandTwoColumn;
    case PageId::EditWorld:
    case PageId::ConfirmDelete:
        return PageLayoutKind::BottomBand;
    // UI-6d：视频设置改用与 Controls 同一套三段式双列（26.1 的 OptionsSubScreen）。
    // 从前它是本作自造的 videoSettingsButton 网格——那是"项数超出一列"时的权宜，
    // 而 26.1 的答案一直是 OptionsList。
    case PageId::VideoSettings:
    case PageId::Controls:
    case PageId::AdvancedGraphics:
        return PageLayoutKind::HeaderFooterList;
    // 其余都走屏幕正中那一列。`Game` 与 `Loading` 没有菜单按钮，取值仍要良定义：
    // 它们的页面装配是空的，所以这一档永远不会被真的用到。
    case PageId::CreateWorld:
    case PageId::Options:
    case PageId::Accessibility:
    case PageId::Pause:
    case PageId::Death:
    case PageId::Loading:
    case PageId::Game:
        break;
    }
    return PageLayoutKind::CentredColumn;
}

// 这一屏走哪个绘制函数。同样是不带 `default` 的 switch，理由同上。
//
// 从前这是 `drawHud` 里一串 `page == Options || page == VideoSettings || …`：
// 加一页忘了加进那串或运算，它会掉进后面"游戏内"的分支——症状是打开新页面却看到
// 游戏画面，而不是编译错误。
enum class PageDrawKind : std::uint8_t {
    Frontend,   // 主菜单与世界管理三屏
    Settings,   // 走 drawPauseMenu 的设置子屏
    Language,   // 语言页有自己的绘制（列表 + 提示行）
    InGame,     // 游戏内 HUD / 暂停 / 死亡 / 加载，由 drawHud 后半段处理
};

[[nodiscard]] constexpr PageDrawKind pageDrawKind(PageId page) {
    switch (page) {
    case PageId::Title:
    case PageId::WorldList:
    case PageId::CreateWorld:
    case PageId::EditWorld:
    case PageId::ConfirmDelete:
        return PageDrawKind::Frontend;
    case PageId::Options:
    case PageId::VideoSettings:
    case PageId::Controls:
    case PageId::KeyBinds:
    case PageId::Accessibility:
    case PageId::AdvancedGraphics:
        return PageDrawKind::Settings;
    case PageId::Language:
        return PageDrawKind::Language;
    case PageId::Loading:
    case PageId::Game:
    case PageId::Pause:
    case PageId::Death:
        break;
    }
    return PageDrawKind::InGame;
}

} // namespace mc::ui
