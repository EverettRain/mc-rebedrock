#pragma once

// 稳定的控件 id
// 它们给控件起名字，供测试与日志使用，也供选项表按 id 查数据，因为 ui/OptionCycle.hpp 就以它们为键
// 没有任何行为按 id 派发：控件自带它的回调，选项表把 id 换成数据而不是换成一条分支
//
// 单独成一个头文件，选项表因此能指名这些 id 而不必拉进页面装配器
// 后者会连整个控件模型和输入绑定一起带进来

#include <cstdint>

namespace mc::ui {

enum class WidgetId : std::uint16_t {
    None = 0,
    Singleplayer, Options, Exit,
    // UI-2：26.1 主菜单的另外四个控件（spec §6.3）
    // TitleLanguage / TitleAccessibility 是图标钮，没有文字标签，因此登记为无标签
    // 它们与选项页那个带文字的 Language 按钮不是同一个控件，所以不共用 id
    Multiplayer, Realms, TitleLanguage, TitleAccessibility,
    PlaySelected, CreateWorld, Edit, Back,
    CreateGameMode, CreateAllowCommands, CreateConfirm,
    SaveRename, DeleteWorld, DeleteConfirm, DeleteCancel,
    Resume, SaveQuit, Respawn, TitleScreen,
    MasterVolume, Difficulty, Controls, VideoSettings, Language, Experimental, Done,
    Resolution, GuiScale, ViewDistance, SimulationDistance, FrameRateLimit,
    AntiAliasing, Anisotropy, SmoothLighting, DynamicLight, Vsync, EntityShadows,
    ViewBobbing, AutoJump, ForceUnicodeFont,
    RainMode, ParticleLevel, SunShadows, RainCollisionCache,
    WorldRow, LanguageRow,
    KeyBindRow, ResetKeyBinds,
    // UI-6c：**单行**的重置按钮（`controls.reset`），与页脚那个重置**所有**
    // （`controls.resetAll`，上面的 ResetKeyBinds）不是同一个控件。
    ResetKeyBind,
    Subtitles,  // PX-6 Bug3: the sound-subtitles accessibility toggle
    // UI-6c：26.1 §7.6 Controls 那一屏（偏差 D1/D3）。两个跳转按钮加六个设置项。
    // `MouseSettings` **没有**：本作没有鼠标设置屏，而"页面为空就完全不建"。
    // MouseSettings 是一个**置灰**按钮：26.1 §7.6 的第一行是
    // `addSmall(mouse_settings, keybinds)` 两个跳转，而 §7.7 鼠标设置屏本作没有。
    // 不放它，那一格就空着，版面比 vanilla 少半行；放一个能点的又会把玩家送进空页。
    // 置灰是第三条路——版面对上了，而"这个还没有"是看得出来的。
    // 主菜单的 Multiplayer / Realms 是同一种做法。
    MouseSettings, OpenKeyBinds, Accessibility,
    ToggleCrouch, ToggleSprint, ToggleAttack, ToggleUse, SprintWindow, OperatorItemsTab,

    // 哨兵，值等于 id 的个数，表因此能断言自己覆盖了每一个 id，ui/WidgetLabels.hpp 就是这么做的
    // 它永远不是一个控件，也永远排在最后
    Count,
};

}  // namespace mc::ui
