#pragma once

// The single source for a cycling option: the values it steps through, the
// GameOptions field it lives in, and the translation keys its label is built
// from — one table row per option.
//
// Each option used to be stated in three places that had to agree by hand: a
// `cb.cycleXxx` lambda in the renderer that hand-rolled the step (`(x + 1) % 3`
// here, a `switch (quality)` next-function there, a `find` over a local array
// somewhere else), a `case` in widgetLabel() that formatted the value, and the
// field itself. Adding an option meant three edits, and a missed one showed up
// as a button whose label disagreed with what it did.
//
// Here they are one row, and both the step and the label are derived from it:
//   * `cycleOptionValue(desc, options, +1)` steps forward, `-1` backwards — the
//     backwards direction exists because the list is ordered data rather than a
//     hardcoded `next()` chain, which is what makes a two-way selector control a
//     UI change instead of a rewrite of every option;
//   * `optionValueLabel(desc, value, translate)` renders the value half.
//
// The table is constexpr data: no allocation, no init order, and a headless test
// can walk it. The one thing it deliberately does NOT carry is the side effect a
// change has on the renderer (recreate the swapchain, remesh the world, retune
// the particle system) — those touch Vulkan and live in the renderer's own
// applyOptionChanged(), the single place a change is reacted to.

#include "config/GameOptions.hpp"
#include "ui/Language.hpp"
#include "ui/WidgetId.hpp"
#include "world/WorldConstants.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace mc::ui {

// One position in an option's value list. `value` is the stored value (a bool
// field stores 0/1, an enum field its underlying value). `key`/`fallback` name
// the translation for it; leaving them empty renders the number instead, through
// the row's numberSuffix/numberTemplate.
struct OptionValue final {
    int value = 0;
    std::string_view key{};
    std::string_view fallback{};
};

// The GameOptions field an option reads and writes. Three shapes cover every
// cycling option: an on/off flag, an integer, and the one tri-state enum.
using OptionField = std::variant<bool config::GameOptions::*, int config::GameOptions::*,
                                 world::SmoothLightingQuality config::GameOptions::*>;

struct OptionDesc final {
    WidgetId id = WidgetId::None;
    // The option's name, as the vanilla translation key plus its English text
    // (every label carries the fallback so a language missing the key still
    // reads correctly).
    std::string_view nameKey{};
    std::string_view nameFallback{};
    OptionField field{};
    // The values this option steps through, in cycle order.
    std::span<const OptionValue> values{};
    // How a value that carries no key of its own renders: bare, with a literal
    // suffix ("16" + "x"), or through a one-argument vanilla template ("%s fps").
    std::string_view numberSuffix{};
    std::string_view numberTemplateKey{};
    std::string_view numberTemplateFallback{};
};

// ---- shared value lists ---------------------------------------------------

inline constexpr std::array<OptionValue, 2> kOnOffValues{{
    {0, "options.off", "OFF"},
    {1, "options.on", "ON"},
}};

// Vanilla's Max Framerate cycle, ending on the unlimited setting (stored as 0).
inline constexpr std::array<OptionValue, 6> kFrameRateValues{{
    {30}, {60}, {120}, {144}, {240},
    {0, "options.framerateLimit.max", "Unlimited"},
}};

// Anisotropic filtering doubles up to 16x, then wraps — the same sequence the
// old `anisotropy >= 16 ? 1 : anisotropy * 2` produced, now stated as its values.
inline constexpr std::array<OptionValue, 5> kAnisotropyValues{{{1}, {2}, {4}, {8}, {16}}};

// Smooth lighting is a tri-state quality, cycling Off → Standard → High.
inline constexpr std::array<OptionValue, 3> kSmoothLightingValues{{
    {static_cast<int>(world::SmoothLightingQuality::Off), "options.ao.off", "OFF"},
    {static_cast<int>(world::SmoothLightingQuality::Standard), "options.ao.min", "Minimum"},
    {static_cast<int>(world::SmoothLightingQuality::High), "options.ao.max", "Maximum"},
}};

// The rain draw path (实验性内容): texture sheets, per-particle legacy draws,
// instanced SSBO particles.
inline constexpr std::array<OptionValue, 2> kRainModeValues{{
    {0, "options.rebedrock.rainMode.texture", "Texture Rain"},
    {1, "options.rebedrock.rainMode.async", "Asynchronous Particle Rain"},
}};

// Particle density (粒子效果), scaling the rain budget and the particle system's
// live cap and spawn counts.
inline constexpr std::array<OptionValue, 4> kParticleLevelValues{{
    {0, "options.rebedrock.particleLevel.low", "Low (0.5x)"},
    {1, "options.rebedrock.particleLevel.medium", "Medium (1x)"},
    {2, "options.rebedrock.particleLevel.high", "High (2x)"},
    {3, "options.rebedrock.particleLevel.crazy", "Crazy (3x)"},
}};

// ---- the table ------------------------------------------------------------
//
// Every option whose button steps through a fixed list of values. The sliders
// (render/simulation distance, master volume) are a different control and are
// not here; nor are the three settings that are not GameOptions fields at all —
// Resolution (the live window size), GuiScale (menu state) and Difficulty (the
// open save) — which the renderer still handles directly.

inline constexpr std::array<OptionDesc, 14> kCyclingOptions{{
    {WidgetId::AutoJump, "options.autoJump", "Auto-Jump", &config::GameOptions::autoJump,
     kOnOffValues},
    {WidgetId::FrameRateLimit, "options.framerateLimit", "Max Framerate",
     &config::GameOptions::frameRateLimit, kFrameRateValues, /*numberSuffix=*/{},
     "options.framerate", "%s fps"},
    {WidgetId::AntiAliasing, "options.rebedrock.antiAliasing", "Anti-Aliasing",
     &config::GameOptions::antiAliasing, kOnOffValues},
    {WidgetId::Anisotropy, "options.maxAnisotropy", "Anisotropic Filtering",
     &config::GameOptions::anisotropy, kAnisotropyValues, /*numberSuffix=*/"x"},
    {WidgetId::SmoothLighting, "options.ao", "Smooth Lighting",
     &config::GameOptions::smoothLightingQuality, kSmoothLightingValues},
    {WidgetId::DynamicLight, "options.rebedrock.dynamicLights", "Dynamic Lighting",
     &config::GameOptions::dynamicLight, kOnOffValues},
    {WidgetId::Vsync, "options.vsync", "VSync", &config::GameOptions::vsync, kOnOffValues},
    {WidgetId::ViewBobbing, "options.viewBobbing", "View Bobbing",
     &config::GameOptions::viewBobbing, kOnOffValues},
    {WidgetId::ForceUnicodeFont, "options.forceUnicodeFont", "Force Unicode Font",
     &config::GameOptions::forceUnicodeFont, kOnOffValues},
    {WidgetId::Subtitles, "options.showSubtitles", "Show Subtitles",
     &config::GameOptions::showSubtitles, kOnOffValues},
    {WidgetId::RainMode, "options.rebedrock.rainMode", "Rain Mode",
     &config::GameOptions::rainMode, kRainModeValues},
    {WidgetId::ParticleLevel, "options.particles", "Particles",
     &config::GameOptions::particleLevel, kParticleLevelValues},
    {WidgetId::SunShadows, "options.rebedrock.sunShadows", "Sun Shadows",
     &config::GameOptions::sunShadows, kOnOffValues},
    {WidgetId::RainCollisionCache, "options.rebedrock.rainCollisionCache",
     "Rain Collision Cache", &config::GameOptions::rainCollisionCache, kOnOffValues},
}};

// The option `id` names, or null when it is not a cycling option (a slider, a
// page button, or one of the three settings that live outside GameOptions).
// constexpr：ui/WidgetLabels.hpp 的覆盖性 static_assert 要在编译期问「这个 id
// 是不是一个循环选项」，因此这个查找必须能在常量求值里跑。
[[nodiscard]] constexpr const OptionDesc* findCyclingOption(WidgetId id) {
    for (const OptionDesc& desc : kCyclingOptions) {
        if (desc.id == id) {
            return &desc;
        }
    }
    return nullptr;
}

// ---- reading, writing, stepping -------------------------------------------

[[nodiscard]] inline int readOption(const OptionDesc& desc, const config::GameOptions& options) {
    return std::visit(
        [&options](auto member) { return static_cast<int>(options.*member); }, desc.field);
}

inline void writeOption(const OptionDesc& desc, config::GameOptions& options, int value) {
    std::visit(
        [&options, value](auto member) {
            using Field = std::remove_reference_t<decltype(options.*member)>;
            options.*member = static_cast<Field>(value);
        },
        desc.field);
}

// The index of `value` in the option's list, or 0 when the stored value is not
// one of them (an options file edited by hand, or a value retired from the list).
// Snapping to the first entry is what the old hand-rolled `find`-or-zero did.
[[nodiscard]] inline std::size_t optionValueIndex(const OptionDesc& desc, int value) {
    for (std::size_t index = 0; index < desc.values.size(); ++index) {
        if (desc.values[index].value == value) {
            return index;
        }
    }
    return 0;
}

// Steps the option one position, wrapping at both ends. `direction` is +1 for
// the next value (a left click, matching vanilla) and -1 for the previous.
inline void cycleOptionValue(const OptionDesc& desc, config::GameOptions& options,
                             int direction = 1) {
    if (desc.values.empty()) {
        return;
    }
    const std::size_t count = desc.values.size();
    const std::size_t current = optionValueIndex(desc, readOption(desc, options));
    const std::size_t step = direction < 0 ? count - 1U : 1U;
    writeOption(desc, options, desc.values[(current + step) % count].value);
}

// The value half of the option's label ("ON", "120 fps", "16x", "Minimum").
// `translate(key, fallback)` returns the localized text for a key; the caller
// supplies it so this stays free of the renderer's language table.
template <typename Translate>
[[nodiscard]] std::string optionValueLabel(const OptionDesc& desc, int value,
                                           Translate&& translate) {
    if (desc.values.empty()) {
        return {};
    }
    const OptionValue& entry = desc.values[optionValueIndex(desc, value)];
    if (!entry.key.empty()) {
        return translate(entry.key, entry.fallback);
    }
    const std::string number = std::to_string(entry.value);
    if (!desc.numberTemplateKey.empty()) {
        return formatTranslation(translate(desc.numberTemplateKey, desc.numberTemplateFallback),
                                 std::array<std::string_view, 1>{number});
    }
    return number + std::string{desc.numberSuffix};
}

}  // namespace mc::ui
