#pragma once

#include "audio/SoundCategory.hpp"
#include "world/WorldConstants.hpp"

#include <filesystem>
#include <string>

namespace mc::config {

struct GameOptions final {
    // The build's version identity is NOT a user option — it lives once in
    // core::kVersion (META's single source) and the F3 overlay reads it there.
    // It used to be a hardcoded string persisted here as `game.version`, which
    // let a stale options file misreport the running build; that scatter is gone.
    int windowWidth = 960;
    int windowHeight = 720;
    // The windowed restore size above remains meaningful while maximized: GLFW
    // uses it when the window is restored, while this flag recreates the native
    // maximized state on the next launch.
    bool windowMaximized = false;
    int guiScale = 0;
    int viewDistance = 4;
    // Simulation distance in chunks: creatures farther than this from the player
    // are frozen each tick but stay rendered. Kept as its own setting (vanilla's
    // Simulation Distance), independent of the render distance.
    int simulationDistance = 4;
    int frameRateLimit = 120;
    int anisotropy = 8;
    float masterVolume = 0.8F;
    // Per-category (non-master) sound volumes, indexed by mc::audio::SoundCategory
    // (Master's slot mirrors masterVolume and is never persisted here — the
    // existing audio.masterVolume key stays authoritative). Each sub-category
    // multiplies on top of Master at play time. Default 1 for every bus, and the
    // file writes each one sparsely under audio.category.<name>; an old options
    // file with no such lines loads every sub-category at 1, i.e. unchanged
    // behaviour.
    mc::audio::SoundCategoryVolumes soundCategoryVolumes = mc::audio::defaultSoundCategoryVolumes();
    // Vanilla's "Directional Audio" accessibility toggle (HRTF in vanilla; a pan
    // mode here — see AudioSystem). On by default, matching vanilla.
    bool directionalAudio = true;
    bool antiAliasing = true;
    bool viewBobbing = true;
    // Bedrock-style auto-jump: walking forward into a one-block rise jumps
    // automatically. Off by default, matching Java 1.16.1 (which has no
    // auto-jump at all).
    bool autoJump = false;
    // Smooth lighting is a tri-state quality: Off keeps the flat light values,
    // Standard is the binary-AO algorithm, High is the vanilla 1.16.1 AO. The
    // mesh is baked at the active quality (the packed vertex carries one AO
    // set), so changing it remeshes the world.
    mc::world::SmoothLightingQuality smoothLightingQuality =
        mc::world::SmoothLightingQuality::Standard;
    bool dynamicLight = false;
    // PX-6: show sound subtitles (26.1 accessibility captions). A client option,
    // not a gamerule; off by default, matching vanilla. Gates the subtitle
    // overlay feed — captions only appear when this is on.
    bool showSubtitles = false;
    // Present at the monitor's refresh rate (FIFO) instead of MAILBOX's
    // drop-on-demand presentation; zero CPU cost and no tearing, at the price
    // of never exceeding the display rate.
    bool vsync = false;
    // Interface language code, matching a vanilla lang file name (en_us, zh_cn).
    std::string language = "en_us";
    // Vanilla's "Force Unicode Font": draws Latin text from the unicode pages
    // too, which keeps mixed Latin/CJK lines visually consistent.
    bool forceUnicodeFont = false;
    // Experimental Content (实验性内容) submenu — test-only render features.
    // rainMode selects the rain draw path: 0 = 贴图雨 (texture sheets),
    // 1 = 粒子雨 (per-particle legacy draws), 2 = 异步粒子雨 (instanced SSBO).
    int rainMode = 2;
    // Toggles the sun-space shadow depth pre-pass. Off by default: the pre-pass
    // is pure infrastructure until the terrain actually samples the shadow map
    // (the roadmap's P2), and re-rendering every opaque section each frame is
    // wasted GPU load that can push heavy frames toward a device lost.
    bool sunShadows = false;
    // Particle-effect density (粒子效果): 0 = 低 (Low, 0.5x), 1 = 中 (Medium,
    // 1.0x, the default), 2 = 高 (High, 2x), 3 = 疯狂 (Crazy, 3x). Scales the
    // rain-drop budget and the particle system's live cap and spawn counts.
    int particleLevel = 1;
    // Rain collision caching (碰撞缓存): on by default — the first drop to enter
    // a column probes its surface and the rest fall to the cached value, so a
    // huge storm costs near-zero world lookups. Turning it off reverts to the
    // direct per-drop-per-frame probe for machines with headroom.
    bool rainCollisionCache = true;

    [[nodiscard]] static GameOptions load(const std::filesystem::path& path);
    void save(const std::filesystem::path& path) const;
    void sanitize();

    [[nodiscard]] bool operator==(const GameOptions&) const = default;
};

} // namespace mc::config
