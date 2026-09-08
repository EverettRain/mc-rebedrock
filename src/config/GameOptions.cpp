#include "config/GameOptions.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mc::config {
namespace {

[[nodiscard]] std::string_view trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1U);
}

template <typename Number> [[nodiscard]] bool parseNumber(std::string_view text, Number& value) {
    const auto cleaned = trim(text);
    Number parsed{};
    const auto [end, error] =
        std::from_chars(cleaned.data(), cleaned.data() + cleaned.size(), parsed);
    if (error != std::errc{} || end != cleaned.data() + cleaned.size()) {
        return false;
    }
    value = parsed;
    return true;
}

// lighting.smooth is on|off, and reads every spelling this option has ever been
// written with. Two migrations live here:
//
//   * the original boolean ("true"/"1"/"on"), from before the tier split;
//   * the tier names "standard" and "high", from RN-19b's predecessor.
//
// **"standard" migrates to On, not Off.** A player whose options say "standard"
// had ambient occlusion switched ON — just in the weak tier that no longer
// exists — so migrating them to Off would silently turn off a feature they were
// using because the tier they picked was deleted. 26.1's own
// `OptionsAmbientOcclusionFix` does the same thing: every non-`OFF` value of the
// old three-way setting becomes `true`.
[[nodiscard]] mc::world::SmoothLightingQuality parseSmoothLighting(std::string_view value) {
    if (value == "off" || value == "false" || value == "0") {
        return mc::world::SmoothLightingQuality::Off;
    }
    return mc::world::SmoothLightingQuality::On;
}

[[nodiscard]] std::string_view smoothLightingName(mc::world::SmoothLightingQuality quality) {
    return quality == mc::world::SmoothLightingQuality::Off ? "off" : "on";
}

} // namespace

void GameOptions::sanitize() {
    windowWidth = std::clamp(windowWidth, 640, 7680);
    windowHeight = std::clamp(windowHeight, 480, 4320);
    guiScale = std::clamp(guiScale, 0, 12);
    // 26.1 `OptionInstance.IntRange(0, 10)`；上界同 GameRenderer.MAX_BLUR_RADIUS
    menuBackgroundBlurriness = std::clamp(menuBackgroundBlurriness, 0, 10);
    // 26.1 `Options.fov` 的 IntRange(30, 110)
    fieldOfView = std::clamp(fieldOfView, 30, 110);
    // 26.1 `Options.sprintWindow` 的 IntRange(0, 20)，0 显示为 OFF
    sprintWindow = std::clamp(sprintWindow, 0, 20);
    viewDistance = std::clamp(viewDistance, 2, 36);
    simulationDistance = std::clamp(simulationDistance, 2, 12);
    if (frameRateLimit != 0) frameRateLimit = std::clamp(frameRateLimit, 30, 260);
    if (anisotropy <= 1) anisotropy = 1;
    else if (anisotropy <= 2) anisotropy = 2;
    else if (anisotropy <= 4) anisotropy = 4;
    else if (anisotropy <= 8) anisotropy = 8;
    else anisotropy = 16;
    masterVolume = std::clamp(masterVolume, 0.0F, 1.0F);
    for (float& categoryVolume : soundCategoryVolumes) {
        categoryVolume = std::clamp(categoryVolume, 0.0F, 1.0F);
    }
    // Master's slot is not an independent setting; keep it a mirror of the
    // authoritative masterVolume so a reader that indexes the array by
    // SoundCategory::Master sees the right value.
    soundCategoryVolumes[static_cast<std::size_t>(mc::audio::SoundCategory::Master)] = masterVolume;
    // 手工改过的选项文件里可能是任何数字；档位表之外的取值一律回到 MSAA，
    // 而不是让 renderSampleCount() 去猜一个没有定义的档
    if (antiAliasing != AntiAliasingMode::Off && antiAliasing != AntiAliasingMode::Msaa &&
        antiAliasing != AntiAliasingMode::Taa) {
        antiAliasing = AntiAliasingMode::Msaa;
    }
    rainMode = std::clamp(rainMode, 0, 1);
    particleLevel = std::clamp(particleLevel, 0, 3);
    if (language.empty() ||
        language.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos) {
        language = "en_us";
    }
}

GameOptions GameOptions::load(const std::filesystem::path& path) {
    GameOptions options;
    std::ifstream input{path};
    if (!input) {
        options.save(path);
        return options;
    }

    std::string line;
    while (std::getline(input, line)) {
        const std::string_view cleaned = trim(line);
        if (cleaned.empty() || cleaned.front() == '#' || cleaned.front() == '!') {
            continue;
        }
        const auto separator = cleaned.find_first_of("=:");
        if (separator == std::string_view::npos) {
            continue;
        }
        const auto key = trim(cleaned.substr(0U, separator));
        const auto value = trim(cleaned.substr(separator + 1U));
        if (key == "window.width") {
            static_cast<void>(parseNumber(value, options.windowWidth));
        } else if (key == "window.height") {
            static_cast<void>(parseNumber(value, options.windowHeight));
        } else if (key == "window.maximized") {
            options.windowMaximized = value == "true" || value == "1" || value == "on";
        } else if (key == "gui.scale") {
            static_cast<void>(parseNumber(value, options.guiScale));
        } else if (key == "render.distance") {
            static_cast<void>(parseNumber(value, options.viewDistance));
        } else if (key == "render.simulationDistance") {
            static_cast<void>(parseNumber(value, options.simulationDistance));
        } else if (key == "render.fpsLimit") {
            static_cast<void>(parseNumber(value, options.frameRateLimit));
        } else if (key == "render.anisotropy") {
            static_cast<void>(parseNumber(value, options.anisotropy));
        } else if (key == "render.antiAliasing") {
            // 三档以数字存储，但这个键在做成三档之前写的是 true/false。两种拼法
            // 都要读得动，而且它们恰好不冲突：旧的 true 就是 Msaa(1)，false 就是 Off(0)
            int mode = static_cast<int>(AntiAliasingMode::Msaa);
            if (value == "true" || value == "on") {
                mode = static_cast<int>(AntiAliasingMode::Msaa);
            } else if (value == "false" || value == "off") {
                mode = static_cast<int>(AntiAliasingMode::Off);
            } else if (!parseNumber(value, mode)) {
                mode = static_cast<int>(AntiAliasingMode::Msaa);
            }
            options.antiAliasing = static_cast<AntiAliasingMode>(mode);
        } else if (key == "render.cascadedShadows") {
            options.cascadedShadows = value == "true" || value == "1" || value == "on";
        } else if (key == "render.entityShadows") {
            options.entityShadows = value == "true" || value == "1" || value == "on";
        } else if (key == "render.viewBobbing") {
            options.viewBobbing = value == "true" || value == "1" || value == "on";
        } else if (key == "control.autoJump") {
            options.autoJump = value == "true" || value == "1" || value == "on";
        } else if (key == "audio.masterVolume") {
            static_cast<void>(parseNumber(value, options.masterVolume));
        } else if (key == "accessibility.showSubtitles") {
            options.showSubtitles = value == "true" || value == "1" || value == "on";
        } else if (key == "audio.directionalAudio") {
            options.directionalAudio = value == "true" || value == "1" || value == "on";
        } else if (key.rfind("audio.category.", 0U) == 0U) {
            // A per-category sub-volume line, audio.category.<name>. An unknown
            // name (or the master line, which is written elsewhere) is ignored so
            // a stray token never lands in the wrong bus.
            const auto name = key.substr(std::string_view{"audio.category."}.size());
            const auto category = mc::audio::soundCategoryFromName(name);
            if (category != mc::audio::SoundCategory::Count &&
                category != mc::audio::SoundCategory::Master) {
                static_cast<void>(parseNumber(
                    value, options.soundCategoryVolumes[static_cast<std::size_t>(category)]));
            }
        } else if (key == "lighting.dynamic") {
            options.dynamicLight = value == "true" || value == "1" || value == "on";
        } else if (key == "lighting.smooth") {
            options.smoothLightingQuality = parseSmoothLighting(value);
        } else if (key == "render.vsync") {
            options.vsync = value == "true" || value == "1" || value == "on";
        } else if (key == "text.language") {
            if (!value.empty()) {
                options.language = std::string{value};
            }
        } else if (key == "controls.toggleCrouch") {
            options.toggleCrouch = value == "true" || value == "1" || value == "on";
        } else if (key == "controls.toggleSprint") {
            options.toggleSprint = value == "true" || value == "1" || value == "on";
        } else if (key == "controls.toggleAttack") {
            options.toggleAttack = value == "true" || value == "1" || value == "on";
        } else if (key == "controls.toggleUse") {
            options.toggleUse = value == "true" || value == "1" || value == "on";
        } else if (key == "controls.sprintWindow") {
            static_cast<void>(parseNumber(value, options.sprintWindow));
        } else if (key == "controls.operatorItemsTab") {
            options.operatorItemsTab = value == "true" || value == "1" || value == "on";
        } else if (key == "render.fov") {
            static_cast<void>(parseNumber(value, options.fieldOfView));
        } else if (key == "gui.menuBackgroundBlurriness") {
            static_cast<void>(parseNumber(value, options.menuBackgroundBlurriness));
        } else if (key == "text.forceUnicodeFont") {
            options.forceUnicodeFont = value == "true" || value == "1" || value == "on";
        } else if (key == "experimental.rainMode") {
            static_cast<void>(parseNumber(value, options.rainMode));
        } else if (key == "experimental.sunShadows") {
            options.sunShadows = value == "true" || value == "1" || value == "on";
        } else if (key == "experimental.particleLevel") {
            static_cast<void>(parseNumber(value, options.particleLevel));
        } else if (key == "experimental.rainCollisionCache") {
            options.rainCollisionCache = value == "true" || value == "1" || value == "on";
        }
    }
    options.sanitize();
    return options;
}

void GameOptions::save(const std::filesystem::path& path) const {
    GameOptions sanitized = *this;
    sanitized.sanitize();
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream output{path, std::ios::trunc};
    if (!output) {
        throw std::runtime_error("Unable to write game options: " + path.string());
    }
    output << "# MC Rebedrock options\n"
           << "window.width=" << sanitized.windowWidth << '\n'
           << "window.height=" << sanitized.windowHeight << '\n'
           << "window.maximized=" << (sanitized.windowMaximized ? "true" : "false") << '\n'
           << "gui.scale=" << sanitized.guiScale << '\n'
           << "gui.menuBackgroundBlurriness=" << sanitized.menuBackgroundBlurriness << '\n'
           << "render.fov=" << sanitized.fieldOfView << '\n'
           << "controls.toggleCrouch=" << (sanitized.toggleCrouch ? "true" : "false") << '\n'
           << "controls.toggleSprint=" << (sanitized.toggleSprint ? "true" : "false") << '\n'
           << "controls.toggleAttack=" << (sanitized.toggleAttack ? "true" : "false") << '\n'
           << "controls.toggleUse=" << (sanitized.toggleUse ? "true" : "false") << '\n'
           << "controls.sprintWindow=" << sanitized.sprintWindow << '\n'
           << "controls.operatorItemsTab=" << (sanitized.operatorItemsTab ? "true" : "false")
           << '\n'
           << "render.distance=" << sanitized.viewDistance << '\n'
           << "render.simulationDistance=" << sanitized.simulationDistance << '\n'
           << "render.fpsLimit=" << sanitized.frameRateLimit << '\n'
           << "render.anisotropy=" << sanitized.anisotropy << '\n'
           << "render.antiAliasing=" << static_cast<int>(sanitized.antiAliasing) << '\n'
           << "render.cascadedShadows=" << (sanitized.cascadedShadows ? "true" : "false")
           << '\n'
           << "render.viewBobbing=" << (sanitized.viewBobbing ? "true" : "false") << '\n'
           << "render.entityShadows=" << (sanitized.entityShadows ? "true" : "false") << '\n'
           << "control.autoJump=" << (sanitized.autoJump ? "true" : "false") << '\n'
           << "audio.masterVolume=" << sanitized.masterVolume << '\n'
           << "accessibility.showSubtitles=" << (sanitized.showSubtitles ? "true" : "false")
           << '\n'
           << "audio.directionalAudio=" << (sanitized.directionalAudio ? "true" : "false") << '\n';
    // Sparse per-category volumes: only a bus that a slider actually lowered from
    // the 1.0 default is written, so a fresh options file has no category lines at
    // all and stays forward/backward compatible. Master is skipped — it is the
    // audio.masterVolume line above.
    for (std::size_t index = 0; index < mc::audio::kSoundCategoryCount; ++index) {
        const auto category = static_cast<mc::audio::SoundCategory>(index);
        if (category == mc::audio::SoundCategory::Master) {
            continue;
        }
        const float volume = sanitized.soundCategoryVolumes[index];
        if (volume < 1.0F) {
            output << "audio.category." << mc::audio::soundCategoryName(category) << '=' << volume
                   << '\n';
        }
    }
    output << "lighting.smooth=" << smoothLightingName(sanitized.smoothLightingQuality) << '\n'
           << "lighting.dynamic=" << (sanitized.dynamicLight ? "true" : "false") << '\n'
           << "render.vsync=" << (sanitized.vsync ? "true" : "false") << '\n'
           << "text.language=" << sanitized.language << '\n'
           << "text.forceUnicodeFont=" << (sanitized.forceUnicodeFont ? "true" : "false") << '\n'
           << "experimental.rainMode=" << sanitized.rainMode << '\n'
           << "experimental.sunShadows=" << (sanitized.sunShadows ? "true" : "false") << '\n'
           << "experimental.particleLevel=" << sanitized.particleLevel << '\n'
           << "experimental.rainCollisionCache="
           << (sanitized.rainCollisionCache ? "true" : "false") << '\n';
}

} // namespace mc::config
