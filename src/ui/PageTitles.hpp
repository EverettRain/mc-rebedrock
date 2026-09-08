#pragma once

// UI-6c：每一屏页眉上那行标题，一张表。
//
// 从前它是 `drawPauseMenu` 里一串嵌套三目：Options / Experimental / VideoSettings /
// Controls / 死亡屏 / 其余。每加一屏就多嵌一层，而"某一屏的标题写成了另一屏的"
// 在画面上只是一行字不对——没有任何东西会红。UI-6c 一次加两屏（§7.8 绑定列表与
// §7.11 辅助功能），先把它收成表。
//
// 键与兜底逐条对 26.1（本地 vanilla 语言表已核实这些键都存在）。翻译由调用方做：
// 这个头文件不认识 Language，因此可以被无头断言。

#include "ui/PageStack.hpp"

#include <string_view>

namespace mc::ui {

struct PageTitle final {
    std::string_view key{};
    std::string_view fallback{};

    [[nodiscard]] constexpr bool empty() const { return key.empty(); }
    [[nodiscard]] constexpr bool operator==(const PageTitle&) const = default;
};

// 这一屏页眉上写什么。没有页眉标题的屏（主菜单画的是 logo、游戏内没有界面）返回空。
[[nodiscard]] constexpr PageTitle pageTitle(PageId page) {
    switch (page) {
    case PageId::Options:
        return {"options.title", "Options"};
    case PageId::VideoSettings:
        return {"options.videoTitle", "Video Settings"};
    case PageId::Controls:
        return {"controls.title", "Controls"};
    // ★ 绑定列表页的标题是 `controls.keybinds.title`（"按键绑定"），
    //   **不是** `controls.keybinds`（"按键绑定…"，那是跳过来的**按钮**上的字）。
    //   两个键只差一个后缀，而带省略号的标题看起来"也差不多对"。
    case PageId::KeyBinds:
        return {"controls.keybinds.title", "Key Binds"};
    // 同理：`options.accessibility.title` 是标题，`options.accessibility` 是按钮。
    case PageId::Accessibility:
        return {"options.accessibility.title", "Accessibility Settings"};
    case PageId::Language:
        return {"options.language.title", "Language"};
    case PageId::Experimental:
        return {"selectWorld.experimental", "Experimental"};
    case PageId::Death:
        return {"deathScreen.title", "You Died!"};
    case PageId::Pause:
        return {"menu.game", "Game Menu"};
    // 主菜单画的是 logo 贴图，不是一行标题；游戏内没有界面；其余几屏各自画自己的。
    case PageId::Title:
    case PageId::WorldList:
    case PageId::CreateWorld:
    case PageId::EditWorld:
    case PageId::ConfirmDelete:
    case PageId::Loading:
    case PageId::Game:
        break;
    }
    return {};
}

} // namespace mc::ui
