#include "audio/SoundCategory.hpp"
#include "config/GameOptions.hpp"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

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
    assert(!defaults.windowMaximized);
    // Every sound sub-category defaults to full volume, and Directional Audio is
    // on (vanilla parity).
    for (const float categoryVolume : defaults.soundCategoryVolumes) {
        assert(std::fabs(categoryVolume - 1.0F) < 1e-5F);
    }
    assert(defaults.directionalAudio);

    defaults.windowWidth = 1600;
    defaults.windowHeight = 900;
    defaults.windowMaximized = true;
    defaults.guiScale = 3;
    defaults.viewDistance = 12;
    defaults.masterVolume = 0.35F;
    defaults.smoothLightingQuality = mc::world::SmoothLightingQuality::Off;
    defaults.dynamicLight = true;
    defaults.soundCategoryVolumes[static_cast<std::size_t>(mc::audio::SoundCategory::Block)] = 0.3F;
    defaults.soundCategoryVolumes[static_cast<std::size_t>(mc::audio::SoundCategory::Music)] = 0.0F;
    defaults.directionalAudio = false;
    // Mirror Master into the category array (and clamp) the way save/load will,
    // so the round-trip compares equal on the derived Master slot too.
    defaults.sanitize();
    defaults.save(path);
    const auto loaded = mc::config::GameOptions::load(path);
    assert(loaded == defaults);
    // The lowered categories round-trip exactly; the Directional Audio toggle too.
    assert(std::fabs(loaded.soundCategoryVolumes[static_cast<std::size_t>(
                         mc::audio::SoundCategory::Block)] -
                     0.3F) < 1e-5F);
    assert(loaded.soundCategoryVolumes[static_cast<std::size_t>(mc::audio::SoundCategory::Music)] ==
           0.0F);
    assert(!loaded.directionalAudio);
    {
        std::ifstream saved{path};
        const std::string contents{std::istreambuf_iterator<char>{saved},
                                   std::istreambuf_iterator<char>{}};
        assert(contents.find("window.maximized=true") != std::string::npos);
        // Sparse write: the two lowered buses appear, an untouched bus (weather,
        // still 1.0) does not, and Master is never written as a category line.
        assert(contents.find("audio.category.block=") != std::string::npos);
        assert(contents.find("audio.category.music=") != std::string::npos);
        assert(contents.find("audio.category.weather=") == std::string::npos);
        assert(contents.find("audio.category.master=") == std::string::npos);
        assert(contents.find("audio.directionalAudio=false") != std::string::npos);
    }

    // Backward compatibility: an options file with no category lines and no
    // directional line loads every sub-category at the 1.0 default and Directional
    // Audio on, and does not crash.
    {
        std::ofstream legacy{path, std::ios::trunc};
        legacy << "audio.masterVolume=0.5\n"
               << "render.distance=8\n";
    }
    const auto legacyLoaded = mc::config::GameOptions::load(path);
    for (std::size_t index = 0; index < mc::audio::kSoundCategoryCount; ++index) {
        const auto category = static_cast<mc::audio::SoundCategory>(index);
        const float expected =
            category == mc::audio::SoundCategory::Master ? 0.5F : 1.0F;
        assert(std::fabs(legacyLoaded.soundCategoryVolumes[index] - expected) < 1e-5F);
    }
    assert(legacyLoaded.directionalAudio);
    // An unknown category token is ignored rather than misrouted.
    {
        std::ofstream stray{path, std::ios::trunc};
        stray << "audio.category.bogus=0.1\n"
              << "audio.category.hostile=0.2\n";
    }
    const auto strayLoaded = mc::config::GameOptions::load(path);
    assert(std::fabs(strayLoaded.soundCategoryVolumes[static_cast<std::size_t>(
                         mc::audio::SoundCategory::Hostile)] -
                     0.2F) < 1e-5F);

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
    assert(!sanitized.windowMaximized);
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
