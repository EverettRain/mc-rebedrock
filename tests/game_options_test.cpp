#include "config/GameOptions.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>

int main() {
    const auto root = std::filesystem::temp_directory_path() / "mc_rebedrock_game_options_test";
    const auto path = root / "config" / "options.properties";
    std::filesystem::remove_all(root);

    auto defaults = mc::config::GameOptions::load(path);
    assert(std::filesystem::is_regular_file(path));
    assert(defaults.viewDistance == 4);
    assert(defaults.smoothLightingQuality == mc::world::SmoothLightingQuality::Standard);
    assert(defaults.frameRateLimit == 120);
    assert(defaults.antiAliasing);
    assert(defaults.anisotropy == 8);
    assert(defaults.viewBobbing);
    assert(defaults.version == "ReBedrock beta2");

    defaults.windowWidth = 1600;
    defaults.windowHeight = 900;
    defaults.guiScale = 3;
    defaults.viewDistance = 12;
    defaults.masterVolume = 0.35F;
    defaults.smoothLightingQuality = mc::world::SmoothLightingQuality::Off;
    defaults.dynamicLight = true;
    defaults.version = "ReBedrock test-version";
    defaults.save(path);
    const auto loaded = mc::config::GameOptions::load(path);
    assert(loaded == defaults);

    {
        std::ofstream output{path, std::ios::trunc};
        output << "window.width=99999\n"
               << "render.distance=-5\n"
               << "render.fpsLimit=999\n"
               << "render.anisotropy=7\n"
               << "audio.masterVolume=2.0\n"
               << "unknown.key=ignored\n";
    }
    const auto sanitized = mc::config::GameOptions::load(path);
    assert(sanitized.windowWidth == 7680);
    assert(sanitized.viewDistance == 2);
    assert(sanitized.frameRateLimit == 260);
    assert(sanitized.anisotropy == 8);
    assert(sanitized.masterVolume == 1.0F);
    assert(sanitized.version == "ReBedrock beta2");
    // Difficulty is per-save (world.dat), not a game option: a legacy
    // game.difficulty line in options.properties is ignored entirely.
    {
        std::ofstream file{path, std::ios::trunc};
        file << "window.width=960\n"
             << "game.difficulty=hard\n";
    }
    const auto withoutDifficulty = mc::config::GameOptions::load(path);
    assert(withoutDifficulty.windowWidth == 960);
    // Tri-state smooth lighting round-trips, and the legacy boolean keys still
    // parse (true→Standard, false→Off).
    {
        defaults.smoothLightingQuality = mc::world::SmoothLightingQuality::High;
        defaults.save(path);
        const auto high = mc::config::GameOptions::load(path);
        assert(high.smoothLightingQuality == mc::world::SmoothLightingQuality::High);
        std::ofstream legacy{path, std::ios::trunc};
        legacy << "lighting.smooth=true\n";
        legacy.close();
        assert(mc::config::GameOptions::load(path).smoothLightingQuality ==
               mc::world::SmoothLightingQuality::Standard);
        std::ofstream legacyOff{path, std::ios::trunc};
        legacyOff << "lighting.smooth=false\n";
        legacyOff.close();
        assert(mc::config::GameOptions::load(path).smoothLightingQuality ==
               mc::world::SmoothLightingQuality::Off);
    }
    std::filesystem::remove_all(root);
    return 0;
}
