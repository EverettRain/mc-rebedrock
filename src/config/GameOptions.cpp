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

// lighting.smooth is a tri-state (off|standard|high) that also accepts the
// legacy booleans ("true"/"1"/"on" meant the old on/off toggle).
[[nodiscard]] mc::world::SmoothLightingQuality parseSmoothLighting(std::string_view value) {
    if (value == "high") return mc::world::SmoothLightingQuality::High;
    if (value == "off") return mc::world::SmoothLightingQuality::Off;
    return (value == "true" || value == "1" || value == "on" || value == "standard")
               ? mc::world::SmoothLightingQuality::Standard
               : mc::world::SmoothLightingQuality::Off;
}

[[nodiscard]] std::string_view smoothLightingName(mc::world::SmoothLightingQuality quality) {
    switch (quality) {
    case mc::world::SmoothLightingQuality::Off: return "off";
    case mc::world::SmoothLightingQuality::High: return "high";
    case mc::world::SmoothLightingQuality::Standard: return "standard";
    }
    return "standard";
}

} // namespace

void GameOptions::sanitize() {
    windowWidth = std::clamp(windowWidth, 640, 7680);
    windowHeight = std::clamp(windowHeight, 480, 4320);
    guiScale = std::clamp(guiScale, 0, 12);
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
    rainMode = std::clamp(rainMode, 0, 2);
    particleLevel = std::clamp(particleLevel, 0, 3);
    if (language.empty() ||
        language.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos) {
        language = "en_us";
    }
}

GameOptions GameOptions::load(const std::filesystem::path& path) {
    GameOptions options;
    bool hasVersion = false;
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
        if (key == "game.version") {
            if (!value.empty()) {
                options.version = std::string{value};
            }
            hasVersion = true;
        } else if (key == "window.width") {
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
            options.antiAliasing = value == "true" || value == "1" || value == "on";
        } else if (key == "render.viewBobbing") {
            options.viewBobbing = value == "true" || value == "1" || value == "on";
        } else if (key == "control.autoJump") {
            options.autoJump = value == "true" || value == "1" || value == "on";
        } else if (key == "audio.masterVolume") {
            static_cast<void>(parseNumber(value, options.masterVolume));
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
    if (!hasVersion) {
        options.save(path);
    }
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
           << "game.version=" << sanitized.version << '\n'
           << "window.width=" << sanitized.windowWidth << '\n'
           << "window.height=" << sanitized.windowHeight << '\n'
           << "window.maximized=" << (sanitized.windowMaximized ? "true" : "false") << '\n'
           << "gui.scale=" << sanitized.guiScale << '\n'
           << "render.distance=" << sanitized.viewDistance << '\n'
           << "render.simulationDistance=" << sanitized.simulationDistance << '\n'
           << "render.fpsLimit=" << sanitized.frameRateLimit << '\n'
           << "render.anisotropy=" << sanitized.anisotropy << '\n'
           << "render.antiAliasing=" << (sanitized.antiAliasing ? "true" : "false") << '\n'
           << "render.viewBobbing=" << (sanitized.viewBobbing ? "true" : "false") << '\n'
           << "control.autoJump=" << (sanitized.autoJump ? "true" : "false") << '\n'
           << "audio.masterVolume=" << sanitized.masterVolume << '\n'
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
