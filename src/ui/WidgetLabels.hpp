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

inline constexpr std::array<StaticWidgetLabel, 22> kStaticWidgetLabels{{
    // 标题界面与世界列表
    {WidgetId::Singleplayer, "menu.singleplayer", "Singleplayer"},
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
    {WidgetId::Language, "options.language", "Language..."},
    // vanilla 的 selectWorld.experimental 只有 "Experimental"，省略号在代码里拼
    {WidgetId::Experimental, "selectWorld.experimental", "Experimental", "..."},
    {WidgetId::Done, "gui.done", "Done"},
    {WidgetId::ResetKeyBinds, "controls.resetAll", "Reset Keys"},
}};

// 标签要读运行期状态，仍由渲染器的 widgetLabel 现算
// 登记在这里是为了让它有归属这件事可被编译期检查，而不是靠 switch 里恰好写了一行
inline constexpr std::array<WidgetId, 8> kRuntimeWidgetLabels{{
    WidgetId::Resolution,          // 实时窗口尺寸（可能被拖拽或最大化过）
    WidgetId::GuiScale,            // 菜单状态里的缩放档位，0 表示 Auto
    WidgetId::ViewDistance,        // 滑块当前值
    WidgetId::SimulationDistance,  // 滑块当前值
    WidgetId::MasterVolume,        // 滑块当前值，按百分比显示
    WidgetId::Difficulty,          // 当前打开的存档
    WidgetId::CreateGameMode,      // 创建世界表单的暂存状态
    WidgetId::CreateAllowCommands, // 同上
}};

// 不经 widgetLabel 取标签的 id
// 三种列表行的文本各自在页面装配时给出，分别是世界名、语言名与按键行
// None 则根本不是一个按钮
inline constexpr std::array<WidgetId, 4> kUnlabelledWidgets{{
    WidgetId::None,
    WidgetId::WorldRow,
    WidgetId::LanguageRow,
    WidgetId::KeyBindRow,
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
