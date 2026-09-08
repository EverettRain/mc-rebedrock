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
    MasterVolume, Difficulty, Controls, VideoSettings, Language, Done,
    Resolution, GuiScale, ViewDistance, SimulationDistance, FrameRateLimit,
    AntiAliasing, Anisotropy, SmoothLighting, DynamicLight, Vsync, EntityShadows,
    ViewBobbing, AutoJump, ForceUnicodeFont,
    // UI-6d：菜单背景模糊强度（26.1 是滑块，IntRange(0,10)，UI-5 已做好存储）。
    // 它是 ui/OptionSlider.hpp 那张整数滑块表的第一个消费者。
    MenuBackgroundBlurriness,
    // UI-6d：26.1 §7.3 顶上那个 graphics preset 大按钮。本作没有预设机制，
    // 它是**置灰**的——少了它版面比 26.1 短一行，能点的又没东西可切。
    GraphicsPreset,
    // UI-6d：视频设置里跳进"高级图形"的按钮（本项目自有页，26.1 没有）。
    AdvancedGraphics,
    RainMode, ParticleLevel, SunShadows, CascadedShadows, RainCollisionCache,
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

    // UI-6e：26.1 §7.4 `SoundOptionsScreen`。十类音量各一个滑块——**主音量已经在上面**
    // （`MasterVolume`），这里是其余九类，顺序照 `SoundSource` 枚举。
    //
    // ★ 26.1 有**十一**类（多一个 `SoundSource.UI`，按钮音走它）。本作的
    //   `audio::SoundCategory` 只有十类，UI 音效走 Master——那是偏差 D6，归音频线。
    //   少的那一类在这里就是少一个滑块，所以这张清单与 vanilla 的差额是**可数的**。
    MusicVolume, RecordVolume, WeatherVolume, BlockVolume, HostileVolume,
    NeutralVolume, PlayerVolume, AmbientVolume, VoiceVolume,
    // 同屏上本作没有后端的三项，**置灰**（与 MouseSettings / GraphicsPreset 同一做法）：
    // 音频设备选择、音乐播放频率、"正在播放"提示条。
    SoundDevice, MusicFrequency, MusicToast,
    // 26.1 的"方向性音频"（vanilla 是 HRTF，本作是声道平移，见 AudioSystem）。
    // GameOptions::directionalAudio 一直存在且默认开，但在这之前**没有任何控件能改它**
    // ——存了、读了、也在用，玩家却碰不到。§7.4 是它在 26.1 里的位置。
    DirectionalAudio,
    // Options 主页上跳进这一屏的按钮。
    SoundSettings,

    // 哨兵，值等于 id 的个数，表因此能断言自己覆盖了每一个 id，ui/WidgetLabels.hpp 就是这么做的
    // 它永远不是一个控件，也永远排在最后
    Count,
};

}  // namespace mc::ui
