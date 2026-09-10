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
    RainMode, ParticleLevel, SunShadows, CascadedShadows, ShadowNearDistance,
    RainCollisionCache,
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

    // UI-6e ④：26.1 `OptionsScreen.init()` 的十个跳转 + 副页眉那两项。
    // 本作已有的：SoundSettings / VideoSettings / Controls / Language / Accessibility。
    // 下面这五个的目标屏本作**没有**，一律置灰在位（与 MouseSettings 同一做法）：
    SkinCustomization, ChatSettings, Telemetry, CreditsAndAttribution, OnlineOptions,
    // 资源包选择：26.1 的 §7.12。后端（PackManager 的生命周期、真实元数据、
    // 启用集合的持久化与重载）正在补，补齐前它也是置灰的。
    ResourcePacks,
    // 视场角滑块。26.1 把它放在 Options 的**副页眉**里（与 Difficulty/Online 并排）。
    FieldOfView,

    // UI-6e ③：资源包选择屏（26.1 §7.12）。左栏"可用"、右栏"已启用"，
    // 一行一个包，点一下转移到对面。
    // ★ 两栏各有自己的 id：**一行属于哪一栏**是布局要知道的事，而 debugId 是
    //   布局唯一能读到的东西。共用一个 id 就得另开一条"第几个之后算右栏"的旁路，
    //   那正是"同一事实两份表述"。
    PackRowAvailable, PackRowSelected,
    // 调序。26.1 把上下箭头画在行内，本作放页脚、作用于右栏选中的那一行（已登记偏差）。
    // UI-10 / D24：调序与取消选择现在都在**行内**（26.1 的做法）。
    // 三个共用同一个 32x32 图标位的三块热区，第几行由装配时的次序决定。
    PackMoveUp, PackMoveDown, PackUnselect,
    // UI-11 / A2：语言屏底部那个跳转，以及字体屏里本作没有后端的那一项。
    FontSettings, JapaneseGlyphVariants,
    // UI-11 / A5：提示屏上的五种控件。标题与正文都是 Label（正文**每行一个**），
    // 所以正文那个 id 会在一页里出现好几次——与三个页签共用一个 id 同理，
    // 第几行由装配时的次序决定。
    NoticeTitle, NoticeMessage, NoticeStopShowing, NoticeProceed,
    // UI-11 / A6：世界列表每一行左边那张 32x32 的存档缩略图。与三个页签共用一个 id
    // 同理——一页里有好几个，第几行由 `Widget::imageIndex` 说了算。
    WorldIcon,
    // 打开 resourcepacks/ 目录。本作没有"用默认程序打开路径"这条能力，置灰。
    PackOpenFolder,

    // A0：容器界面的非槽位控件。**槽位本身不用 id**——它的身份是 Widget 上的
    // `slotKind + slotIndex`（`gameplay::SlotRef` 那两个字段），而不是又一套编号。
    // ★ 一行里的第几个（三条选项条、十一个页签）由页面里的**次序**决定，与按键
    //   绑定行、资源包行同一个做法，不给每一条各开一个 id。
    EnchantOption, TradeOffer, CreativeTab, CreativeDeleteSlot, CreativeScrollbar,

    // UI-9：创建世界的三个标签页（26.1 `CreateWorldScreen` 的 GameTab/WorldTab/MoreTab）。
    // ★ 三个页签**共用一个 id**，第几个由页面里的次序决定——与创造背包页签、
    //   按键绑定行、资源包行同一个做法。页签上的字在装配时给（三个各不相同），
    //   所以它归"不经 widgetLabel 取标签"那一类。
    CreateWorldTabButton,
    // World 页：世界类型 / 奖励箱 / 生成结构。**三个都没有后端**（偏差 D22 已登记），
    // 按既定裁定"只补有后端的，其余置灰在位"——版面与 26.1 对上，且"没做"看得出来。
    CreateWorldType, CreateBonusChest, CreateGenerateStructures,
    // More 页：三个跳转，目标屏本作都没有，同样置灰在位。
    CreateGameRules, CreateExperiments, CreateDataPacks,
    // UI-10：创建世界的两个输入框。它们**第一次进 ui::Page**——从前是绘制侧自己画
    // 自己命中的东西（护栏 28 那一族）。文字仍由 TextFieldState 管，这两个 id 给的是
    // 几何、命中与提示框的归属。
    CreateWorldNameField, CreateWorldSeedField,

    // 哨兵，值等于 id 的个数，表因此能断言自己覆盖了每一个 id，ui/WidgetLabels.hpp 就是这么做的
    // 它永远不是一个控件，也永远排在最后
    Count,
};

}  // namespace mc::ui
