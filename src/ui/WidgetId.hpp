#pragma once

// Stable widget ids. They name a widget for tests, logging and — since the
// option table (ui/OptionCycle.hpp) keys on them — for looking an option's data
// up. Nothing dispatches BEHAVIOUR on them: a widget still carries its own
// callback, and the option table turns an id into data rather than into a branch.
//
// Their own header so the option table can name them without pulling in the page
// builder (which drags in the whole widget model and the input bindings).

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

    // Sentinel: the number of ids, so a table can assert it covers every one of
    // them (ui/WidgetLabels.hpp does). Never a widget; always last.
    Count,
};

}  // namespace mc::ui
