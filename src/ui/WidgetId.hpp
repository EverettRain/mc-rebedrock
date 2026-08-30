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
    PlaySelected, CreateWorld, Edit, Back,
    CreateGameMode, CreateAllowCommands, CreateConfirm,
    SaveRename, DeleteWorld, DeleteConfirm, DeleteCancel,
    Resume, SaveQuit, Respawn, TitleScreen,
    MasterVolume, Difficulty, Controls, VideoSettings, Language, Experimental, Done,
    Resolution, GuiScale, ViewDistance, SimulationDistance, FrameRateLimit,
    AntiAliasing, Anisotropy, SmoothLighting, DynamicLight, Vsync,
    ViewBobbing, AutoJump, ForceUnicodeFont,
    RainMode, ParticleLevel, SunShadows, RainCollisionCache,
    WorldRow, LanguageRow,
    KeyBindRow, ResetKeyBinds,
    Subtitles,  // PX-6 Bug3: the sound-subtitles accessibility toggle

    // 哨兵，值等于 id 的个数，表因此能断言自己覆盖了每一个 id，ui/WidgetLabels.hpp 就是这么做的
    // 它永远不是一个控件，也永远排在最后
    Count,
};

}  // namespace mc::ui
