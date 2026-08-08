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
    if (frameRateLimit != 0) frameRateLimit = std::clamp(frameRateLimit, 30, 260);
    if (anisotropy <= 1) anisotropy = 1;
    else if (anisotropy <= 2) anisotropy = 2;
    else if (anisotropy <= 4) anisotropy = 4;
    else if (anisotropy <= 8) anisotropy = 8;
    else anisotropy = 16;
    masterVolume = std::clamp(masterVolume, 0.0F, 1.0F);
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
        } else if (key == "gui.scale") {
            static_cast<void>(parseNumber(value, options.guiScale));
        } else if (key == "render.distance") {
            static_cast<void>(parseNumber(value, options.viewDistance));
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
           << "gui.scale=" << sanitized.guiScale << '\n'
           << "render.distance=" << sanitized.viewDistance << '\n'
           << "render.fpsLimit=" << sanitized.frameRateLimit << '\n'
           << "render.anisotropy=" << sanitized.anisotropy << '\n'
           << "render.antiAliasing=" << (sanitized.antiAliasing ? "true" : "false") << '\n'
           << "render.viewBobbing=" << (sanitized.viewBobbing ? "true" : "false") << '\n'
           << "control.autoJump=" << (sanitized.autoJump ? "true" : "false") << '\n'
           << "audio.masterVolume=" << sanitized.masterVolume << '\n'
           << "lighting.smooth=" << smoothLightingName(sanitized.smoothLightingQuality) << '\n'
           << "lighting.dynamic=" << (sanitized.dynamicLight ? "true" : "false") << '\n'
           << "render.vsync=" << (sanitized.vsync ? "true" : "false") << '\n'
           << "text.language=" << sanitized.language << '\n'
           << "text.forceUnicodeFont=" << (sanitized.forceUnicodeFont ? "true" : "false") << '\n';
}

} // namespace mc::config
