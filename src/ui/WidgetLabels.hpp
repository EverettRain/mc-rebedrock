#pragma once

// 每个 WidgetId 的标签从哪来，答案是一张表而不是一串 case
//
// widgetLabel() 曾是一个 48 分支的 switch
// 其中二十来个分支只写着 return translated("some.key", "Some Text");
// 那是纯数据，却因为待在 switch 里而享受不到数据该有的待遇
// 它不能被测试遍历，不能被别处复用，加一个按钮还要先在 switch 中间找位置
// 它靠 -Wswitch 的穷尽性当护栏，等于用编译器兜住一个本该是表的东西
// 范式其实就在隔壁，ui/OptionCycle.hpp 早已把循环选项做成了表行
//
// 现在每个 id 都属于且只属于以下四类之一
//
//   Cycling 是循环选项，标签与点击时的步进都来自 OptionCycle 的同一行表数据
//   因此不可能出现按钮写的和它做的不一致
//   Static 的标签只由翻译键决定，可以再带一个后缀，因为 vanilla 的省略号没有独立的键
//   Runtime 的标签要读运行期状态，比如实时窗口尺寸、当前存档的难度、滑块的数值
//   这类文本仍由渲染器算，这里只登记它归渲染器管
//   None 不经 widgetLabel 取标签，世界、语言与按键这三种列表行各自带文本
//
// 覆盖性由文件末尾的 static_assert 保证，新增一个 WidgetId 而忘了归类，编译期就会停下
// 这与原先 -Wswitch 同等强度，但它护的是有没有明确归属，而不是有没有在 switch 里写一行
// 后者用一个 return {}; 就能敷衍过去

#include "ui/OptionCycle.hpp"
#include "ui/WidgetId.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mc::ui {

// 标签完全由一个翻译键决定的按钮
// suffix 原样追加在译文之后，vanilla 对 "Experimental..." 这类并没有单独的键，省略号是拼上去的
// 它因此是数据的一部分，而不是一处特例分支
struct StaticWidgetLabel final {
    WidgetId id = WidgetId::None;
    std::string_view key{};
    std::string_view fallback{};
    std::string_view suffix{};
};

// ★ **suffix 只给本项目自有的键**。vanilla 的译文自己就带省略号
//   （en_us.json 里 `options.online` = "Online..."、`options.resourcepack` =
//   "Resource Packs..."），再加一个 suffix 会显示成 "Online......" —— 实测如此。
//   本项目自有的 `options.rebedrock.advancedGraphics` 才需要它：那个键的按钮与标题
//   是同一个词，省略号是唯一的区别。下面有一条断言钉住这条规则。
inline constexpr std::array<StaticWidgetLabel, 51> kStaticWidgetLabels{{
    // 标题界面与世界列表
    {WidgetId::Singleplayer, "menu.singleplayer", "Singleplayer"},
    {WidgetId::Multiplayer, "menu.multiplayer", "Multiplayer"},
    // vanilla 的 menu.online 就是 "Minecraft Realms"，不是 "Realms"
    {WidgetId::Realms, "menu.online", "Minecraft Realms"},
    {WidgetId::Options, "menu.options", "Options..."},
    {WidgetId::Exit, "menu.quit", "Quit Game"},
    {WidgetId::PlaySelected, "selectWorld.select", "Play Selected World"},
    {WidgetId::CreateWorld, "selectWorld.create", "Create New World"},
    {WidgetId::Edit, "selectWorld.edit", "Edit"},
    {WidgetId::CreateConfirm, "selectWorld.create", "Create World"},
    {WidgetId::SaveRename, "gui.done", "Done"},
    {WidgetId::DeleteWorld, "selectWorld.delete", "Delete"},
    {WidgetId::DeleteConfirm, "selectWorld.deleteButton", "Delete"},
    {WidgetId::DeleteCancel, "gui.cancel", "Cancel"},
    {WidgetId::Back, "gui.back", "Back"},
    // 暂停与死亡界面
    {WidgetId::Resume, "menu.returnToGame", "Back to Game"},
    {WidgetId::SaveQuit, "menu.returnToMenu", "Save and Quit to Title"},
    {WidgetId::Respawn, "deathScreen.respawn", "Respawn"},
    {WidgetId::TitleScreen, "deathScreen.titleScreen", "Title Screen"},
    // 选项各页的入口与收尾
    {WidgetId::VideoSettings, "options.video", "Video Settings..."},
    {WidgetId::Controls, "options.controls", "Controls..."},
    // UI-6c：26.1 的 Controls 是个枢纽，它上面有一个跳到绑定列表的按钮（§7.8）。
    // 本作**没有** Mouse Settings 那个跳转：那一屏不存在，而空页不建。
    {WidgetId::MouseSettings, "options.mouse_settings", "Mouse Settings..."},
    // UI-6d：§7.3 顶上那个 graphics preset 大按钮（26.1 的键是 `options.graphics.preset`，
    // 不是 `options.graphics`——后者是"图像品质"这个类别名）。本作没有预设机制，置灰。
    {WidgetId::GraphicsPreset, "options.graphics.preset", "Preset"},
    // 本项目自有页，26.1 没有：太阳阴影与将来的内建光影参数都归它。
    // 省略号由 suffix 那一列拼——本项目自有的两个键（按钮与标题）因此是同一个词，
    // 差别只在这一列。「实验性内容」被取消后，suffix 的用户就是它了。
    {WidgetId::AdvancedGraphics, "options.rebedrock.advancedGraphics", "Advanced Graphics", "..."},
    {WidgetId::OpenKeyBinds, "controls.keybinds", "Key Binds..."},
    // 单行的重置按钮。★ 是 `controls.reset`（"重置"），不是页脚那个
    // `controls.resetAll`（"重置按键"）——两个键差一个 All，而一整排都写着"重置按键"
    // 看起来"也差不多对"。
    {WidgetId::ResetKeyBind, "controls.reset", "Reset"},
    // §7.11 辅助功能设置。View Bobbing 属于这里，不属于 Controls（偏差 D2）。
    {WidgetId::Accessibility, "options.accessibility", "Accessibility Settings..."},
    {WidgetId::Language, "options.language", "Language..."},
    // vanilla 的 selectWorld.experimental 只有 "Experimental"，省略号在代码里拼
    {WidgetId::Done, "gui.done", "Done"},
    {WidgetId::ResetKeyBinds, "controls.resetAll", "Reset Keys"},
    // UI-6e：§7.4 音乐与声音。26.1 `OptionsScreen` 上那个跳转的键是 `options.sounds`。
    {WidgetId::SoundSettings, "options.sounds", "Music & Sounds..."},
    // 同屏上本作没有后端的三项，置灰。键都取自 26.1：
    //   soundDevice     → `options.audioDevice`（26.1 addBig 独占一行）
    //   musicFrequency  → `options.musicFrequency`
    //   musicToast      → `options.showNowPlayingToast`
    {WidgetId::SoundDevice, "options.audioDevice", "Device"},
    {WidgetId::MusicFrequency, "options.musicFrequency", "Music Frequency"},
    {WidgetId::MusicToast, "options.showNowPlayingToast", "Show Now Playing Toast"},
    // UI-6e ④：26.1 `OptionsScreen` 的十个跳转里本作没有目标屏的那五个，加资源包。
    // 键全部取自 26.1 的 OptionsScreen 常量。
    {WidgetId::SkinCustomization, "options.skinCustomisation", "Skin Customization..."},
    {WidgetId::ChatSettings, "options.chat.title", "Chat Settings"},
    {WidgetId::Telemetry, "options.telemetry", "Telemetry Data..."},
    {WidgetId::CreditsAndAttribution, "options.credits_and_attribution",
     "Credits & Attribution..."},
    {WidgetId::OnlineOptions, "options.online", "Online..."},
    {WidgetId::ResourcePacks, "options.resourcepack", "Resource Packs..."},
    // UI-6e ③：资源包选择屏。26.1 的键：`pack.openFolder` 是页脚那个"打开文件夹"。
    // 上下移在 26.1 是行内的箭头精灵、没有文字；本作放页脚，用本项目自有的键。
    {WidgetId::PackOpenFolder, "pack.openFolder", "Open Pack Folder"},
    // UI-9：创建世界 World / More 两页的六个按钮。键全部取自 26.1
    // `CreateWorldScreen.WorldTab` / `.MoreTab` 里用它们的那几行。
    // UI-11 / A2：26.1 `LanguageSelectScreen:79` 的跳转按钮，文案自带省略号。
    {WidgetId::FontSettings, "options.font", "Font Settings..."},
    // UI-11 / A5：提示屏。正文与标题是本项目自己的键（26.1 那两条讲的是多人游戏），
    // 「不再显示」与两个按钮则直接用 vanilla 的 `multiplayerWarning.check` /
    // `gui.proceed` / `gui.back`——同一句话没有理由另起一个键。
    {WidgetId::NoticeMessage, "rebedrock.advancedGraphicsWarning.message",
     "The options on this screen trade frame time for image quality, and some of them "
     "cost far more than they look: sun shadows re-render every opaque section once more "
     "per frame, and dynamic lights relight a chunk on every light source that moves. "
     "Turn them on one at a time and watch the frame graph."},
    {WidgetId::NoticeStopShowing, "multiplayerWarning.check", "Do not show this screen again"},
    {WidgetId::NoticeProceed, "gui.proceed", "Proceed"},
    {WidgetId::JapaneseGlyphVariants, "options.japaneseGlyphVariants",
     "Japanese Glyph Variants"},
    {WidgetId::CreateWorldType, "selectWorld.mapType", "World Type"},
    {WidgetId::CreateBonusChest, "selectWorld.bonusItems", "Bonus Chest"},
    {WidgetId::CreateGenerateStructures, "selectWorld.mapFeatures", "Generate Structures"},
    {WidgetId::CreateGameRules, "selectWorld.gameRules", "Game Rules"},
    {WidgetId::CreateExperiments, "selectWorld.experiments", "Experiments"},
    {WidgetId::CreateDataPacks, "selectWorld.dataPacks", "Data Packs"},
}};

// 标签要读运行期状态，仍由渲染器的 widgetLabel 现算
// 登记在这里是为了让它有归属这件事可被编译期检查，而不是靠 switch 里恰好写了一行
inline constexpr std::array<WidgetId, 20> kRuntimeWidgetLabels{{
    // UI-11 / A5：提示屏的标题。它取自 `ui::pageTitle(PageId::AdvancedGraphicsNotice)`
    // ——那张表才是「这一屏叫什么」的唯一来源，在这里再抄一份静态标签就是两份表述。
    WidgetId::NoticeTitle,
    WidgetId::MenuBackgroundBlurriness,  // 滑块当前值，最低档显示 OFF
    WidgetId::Resolution,          // 实时窗口尺寸（可能被拖拽或最大化过）
    WidgetId::GuiScale,            // 菜单状态里的缩放档位，0 表示 Auto
    WidgetId::ViewDistance,        // 滑块当前值
    WidgetId::SimulationDistance,  // 滑块当前值
    WidgetId::MasterVolume,        // 滑块当前值，按百分比显示
    // 两处在用：世界内选项页读当前存档的难度，创建世界页读创建表单的暂存值。
    // 同一个 id、同一段标签算法——它们显示的是同一件事，只是取值的来源不同
    WidgetId::Difficulty,
    WidgetId::CreateGameMode,      // 创建世界表单的暂存状态
    WidgetId::CreateAllowCommands, // 同上
    // UI-6e：其余九类音量。与 MasterVolume 同类——标签是"类别名: 百分比"，
    // 而百分比要读运行期的值。它们的**名字**来自 ui/OptionSlider.hpp 那张表
    // （`soundCategory.<name>`），百分比由 widgetLabel 现算。
    WidgetId::MusicVolume,
    WidgetId::RecordVolume,
    WidgetId::WeatherVolume,
    WidgetId::BlockVolume,
    WidgetId::HostileVolume,
    WidgetId::NeutralVolume,
    WidgetId::PlayerVolume,
    WidgetId::AmbientVolume,
    WidgetId::VoiceVolume,
    // UI-6e ④：视场角。标签要读当前值，而且 70 / 110 两个取值显示为文字
    // （`options.fov.min` / `options.fov.max`）而不是数字。
    WidgetId::FieldOfView,
}};

// 不经 widgetLabel 取标签的 id
// 三种列表行的文本各自在页面装配时给出，分别是世界名、语言名与按键行
// None 则根本不是一个按钮
inline constexpr std::array<WidgetId, 18> kUnlabelledWidgets{{
    WidgetId::None,
    WidgetId::WorldRow,
    WidgetId::LanguageRow,
    WidgetId::KeyBindRow,
    // UI-6e ③：包行的文字（名字 + 描述）在装配时给出，与世界名/语言名同类。
    WidgetId::PackRowAvailable,
    WidgetId::PackRowSelected,
    // UI-2：主菜单的两个图标钮。26.1 用 SpriteIconButton 且 iconOnly=true，
    // 按钮上只有 15x15 的图标，没有文字（图标本身归 UI-4 的 IconButton）
    WidgetId::TitleLanguage,
    WidgetId::TitleAccessibility,
    // UI-9：创建世界的三个标签页共用一个 id，页签上的字（Game / World / More）在装配
    // 时给——与世界行、语言行、包行同类。
    WidgetId::CreateWorldTabButton,
    // UI-10：两个输入框里的字是玩家打的，不是标签。
    WidgetId::CreateWorldNameField,
    WidgetId::CreateWorldSeedField,
    // UI-10 / D24：行内的三块热区画的是箭头精灵，没有文字。
    WidgetId::PackUnselect,
    WidgetId::PackMoveUp,
    WidgetId::PackMoveDown,
    // A0：容器界面的四个非槽位控件。三条附魔选项条上的字是**乱码名 + 等级数字**，
    // 由绘制侧按附魔种子现算（`EnchantmentNames`）；页签、删除框与滚动条上根本
    // 没有文字，画的是精灵。都不经 widgetLabel。
    WidgetId::EnchantOption,
    WidgetId::CreativeTab,
    WidgetId::CreativeDeleteSlot,
    WidgetId::CreativeScrollbar,
}};

[[nodiscard]] constexpr const StaticWidgetLabel* findStaticLabel(WidgetId id) {
    for (const StaticWidgetLabel& row : kStaticWidgetLabels) {
        if (row.id == id) {
            return &row;
        }
    }
    return nullptr;
}

[[nodiscard]] constexpr bool hasRuntimeLabel(WidgetId id) {
    for (const WidgetId candidate : kRuntimeWidgetLabels) {
        if (candidate == id) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] constexpr bool isUnlabelled(WidgetId id) {
    for (const WidgetId candidate : kUnlabelledWidgets) {
        if (candidate == id) {
            return true;
        }
    }
    return false;
}

// ---- 覆盖性护栏 ----

// 每个 id 恰好属于一类
// 既不能没有归属，那对应加了按钮却忘了给标签
// 也不能同时属于两类，比如既登记成静态标签又留在循环选项表里，那样两处会各自演化出不同的文本
[[nodiscard]] constexpr bool everyWidgetIdHasExactlyOneLabelSource() {
    for (std::uint16_t raw = 0; raw < static_cast<std::uint16_t>(WidgetId::Count); ++raw) {
        const auto id = static_cast<WidgetId>(raw);
        const int sources = (findCyclingOption(id) != nullptr ? 1 : 0) +
                            (findStaticLabel(id) != nullptr ? 1 : 0) +
                            (hasRuntimeLabel(id) ? 1 : 0) + (isUnlabelled(id) ? 1 : 0);
        if (sources != 1) {
            return false;
        }
    }
    return true;
}

static_assert(everyWidgetIdHasExactlyOneLabelSource(),
              "每个 WidgetId 必须恰好属于一类标签来源：循环选项表、静态标签表、"
              "运行期标签登记表，或明确的无标签表");

// 四张表加起来正好覆盖枚举，也没有多余的行
// 多余的行指某个 id 被删掉之后它的表行还留着
static_assert(kCyclingOptions.size() + kStaticWidgetLabels.size() +
                  kRuntimeWidgetLabels.size() + kUnlabelledWidgets.size() ==
              static_cast<std::size_t>(WidgetId::Count));

}  // namespace mc::ui
