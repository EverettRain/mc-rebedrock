#include "ui/OptionCycle.hpp"

#include <cassert>
#include <set>
#include <string>
#include <string_view>

// The option table (ui/OptionCycle.hpp) is the single source for what a cycling
// option steps through and what its label reads. These pin the sequences to the
// ones the hand-rolled `(x + 1) % N` / `next()` / `>= 16 ? 1 : x * 2` callbacks
// produced, so the table is provably a refactor rather than a rewrite, and pin
// the table's own invariants (one row per id, values reachable, label styles).

namespace {

using namespace mc::ui;
using mc::config::GameOptions;

// Stands in for the renderer's language table: returns the English fallback, so
// a label assertion reads as the text a player with no translation sees.
std::string english(std::string_view, std::string_view fallback) {
    return std::string{fallback};
}

const OptionDesc& option(WidgetId id) {
    const OptionDesc* desc = findCyclingOption(id);
    assert(desc != nullptr);
    return *desc;
}

std::string valueLabel(WidgetId id, const GameOptions& options) {
    const OptionDesc& desc = option(id);
    return optionValueLabel(desc, readOption(desc, options), english);
}

}  // namespace

int main() {
    // --- Every row is well formed and uniquely keyed. ---
    {
        std::set<WidgetId> seen;
        for (const OptionDesc& desc : kCyclingOptions) {
            assert(desc.id != WidgetId::None);
            assert(seen.insert(desc.id).second && "two rows claim the same widget id");
            assert(!desc.values.empty());
            assert(!desc.nameKey.empty() && !desc.nameFallback.empty());
            // A value renders either from its own key or as a number; a row that
            // sets both a suffix and a template would be ambiguous.
            assert(desc.numberSuffix.empty() || desc.numberTemplateKey.empty());
            // Cycling the whole list must return to where it started, which is
            // only true if every value in it is actually reachable (i.e. the
            // values are distinct).
            GameOptions options;
            const int start = readOption(desc, options);
            for (std::size_t step = 0; step < desc.values.size(); ++step) {
                cycleOptionValue(desc, options, +1);
            }
            assert(readOption(desc, options) == start);
        }
        // Ids that are NOT cycling options must not resolve: the three settings
        // that live outside GameOptions, the sliders, and the page buttons.
        assert(findCyclingOption(WidgetId::Resolution) == nullptr);
        assert(findCyclingOption(WidgetId::GuiScale) == nullptr);
        assert(findCyclingOption(WidgetId::Difficulty) == nullptr);
        assert(findCyclingOption(WidgetId::MasterVolume) == nullptr);
        assert(findCyclingOption(WidgetId::Done) == nullptr);
    }

    // --- A bool option toggles and reads ON/OFF. ---
    {
        GameOptions options;
        options.vsync = false;
        const OptionDesc& desc = option(WidgetId::Vsync);
        assert(valueLabel(WidgetId::Vsync, options) == "OFF");
        cycleOptionValue(desc, options);
        assert(options.vsync);
        assert(valueLabel(WidgetId::Vsync, options) == "ON");
        cycleOptionValue(desc, options);
        assert(!options.vsync);
    }

    // --- Max Framerate: the vanilla list, ending on Unlimited (stored as 0). ---
    {
        GameOptions options;
        options.frameRateLimit = 30;
        const OptionDesc& desc = option(WidgetId::FrameRateLimit);
        assert(valueLabel(WidgetId::FrameRateLimit, options) == "30 fps");
        const int expected[] = {60, 120, 144, 240, 0, 30};
        for (const int value : expected) {
            cycleOptionValue(desc, options);
            assert(options.frameRateLimit == value);
        }
        options.frameRateLimit = 0;
        assert(valueLabel(WidgetId::FrameRateLimit, options) == "Unlimited");
    }

    // --- Anisotropy doubles to 16x then wraps, exactly like the old
    //     `anisotropy >= 16 ? 1 : anisotropy * 2`. ---
    {
        GameOptions options;
        options.anisotropy = 8;
        const OptionDesc& desc = option(WidgetId::Anisotropy);
        assert(valueLabel(WidgetId::Anisotropy, options) == "8x");
        cycleOptionValue(desc, options);
        assert(options.anisotropy == 16);
        assert(valueLabel(WidgetId::Anisotropy, options) == "16x");
        cycleOptionValue(desc, options);
        assert(options.anisotropy == 1);
    }

    // --- Smooth lighting cycles Off → Standard → High → Off. ---
    {
        using Quality = mc::world::SmoothLightingQuality;
        GameOptions options;
        options.smoothLightingQuality = Quality::Off;
        const OptionDesc& desc = option(WidgetId::SmoothLighting);
        assert(valueLabel(WidgetId::SmoothLighting, options) == "OFF");
        cycleOptionValue(desc, options);
        assert(options.smoothLightingQuality == Quality::Standard);
        assert(valueLabel(WidgetId::SmoothLighting, options) == "Minimum");
        cycleOptionValue(desc, options);
        assert(options.smoothLightingQuality == Quality::High);
        assert(valueLabel(WidgetId::SmoothLighting, options) == "Maximum");
        cycleOptionValue(desc, options);
        assert(options.smoothLightingQuality == Quality::Off);
    }

    // --- The integer submenus keep their (x + 1) % N sequences. ---
    {
        GameOptions options;
        // 雨模式只剩两档：0 = 贴图雨，1 = 异步粒子雨（实例化）
        // 中间那档"粒子雨"与异步粒子雨产出同一份视觉、只差 draw call 数量，
        // 是为做直接对照才临时留下的绘制方式，已整条移除
        options.rainMode = 0;
        const OptionDesc& rain = option(WidgetId::RainMode);
        for (const int value : {1, 0}) {
            cycleOptionValue(rain, options);
            assert(options.rainMode == value);
        }
        options.particleLevel = 1;
        const OptionDesc& particles = option(WidgetId::ParticleLevel);
        assert(valueLabel(WidgetId::ParticleLevel, options) == "Medium (1x)");
        for (const int value : {2, 3, 0, 1}) {
            cycleOptionValue(particles, options);
            assert(options.particleLevel == value);
        }
    }

    // --- Stepping backwards walks the same list the other way. This is what the
    //     hardcoded `next()` chains could not do, and what a two-way selector
    //     control needs. ---
    {
        GameOptions options;
        options.frameRateLimit = 30;
        const OptionDesc& desc = option(WidgetId::FrameRateLimit);
        cycleOptionValue(desc, options, -1);
        assert(options.frameRateLimit == 0);  // wraps to the end of the list
        cycleOptionValue(desc, options, -1);
        assert(options.frameRateLimit == 240);
        cycleOptionValue(desc, options, +1);
        assert(options.frameRateLimit == 0);
    }

    // --- A stored value that is not in the list (a hand-edited options file)
    //     snaps to the first entry rather than sticking. ---
    {
        GameOptions options;
        options.anisotropy = 7;
        const OptionDesc& desc = option(WidgetId::Anisotropy);
        assert(valueLabel(WidgetId::Anisotropy, options) == "1x");
        cycleOptionValue(desc, options);
        assert(options.anisotropy == 2);
    }

    return 0;
}
