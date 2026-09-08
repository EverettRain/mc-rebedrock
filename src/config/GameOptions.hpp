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
    // Vanilla's "Entity Shadows" (Options.java:486, `createBoolean` with a true
    // default, shown in VideoSettingsScreen.java:51): the round shadow decal
    // under every creature, player, dropped item, experience orb and falling
    // block. On by default, as in vanilla -- it is ordinary presentation, not
    // the experimental sun-shadow pre-pass. An options file written before this
    // key existed simply has no line for it, and `load` starts from these
    // defaults, so an old file reads back as on.
    bool entityShadows = true;
    // Bedrock-style auto-jump: walking forward into a one-block rise jumps
    // automatically. Off by default, matching vanilla (which has no
    // auto-jump at all).
    bool autoJump = false;
    // Smooth lighting (ambient occlusion) is on/off, as in 26.1
    // (`OptionInstance.createBoolean("options.ao", true)`), and on by default for
    // the same reason. RN-19b removed the self-invented middle tier that used to
    // hold this default and that no vanilla setting corresponds to. Toggling it
    // no longer remeshes the world: one algorithm bakes the mesh either way and
    // Off just tells the shader to read the flat light and drop the AO channel.
    mc::world::SmoothLightingQuality smoothLightingQuality =
        mc::world::SmoothLightingQuality::On;
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
    // UI-5：菜单背景模糊强度（26.1 `Options.menuBackgroundBlurriness`，
    // options.accessibility.menu_background_blurriness）。整数档 0..10，默认 5，
    // 0 那一档在界面上显示为 OFF 而不是 "0"（`Options.genericValueOrOffLabel`）。
    // 它就是喂给 box_blur 的半径本身：blur.json 把 `Radius` uniform 写成 0，
    // 于是着色器取全局 MenuBlurRadius，也就是这个值。语义与档位判断在
    // ui/ScreenBackground.hpp（menuBlurEnabled / menuBlurRadius），这里只存。
    int menuBackgroundBlurriness = 5;
    // Experimental Content (实验性内容) submenu — test-only render features.
    // rainMode 选择降雨绘制路径：0 = 贴图雨（逐列贴图），1 = 异步粒子雨（实例化 SSBO）
    // 原来中间还夹着一档"粒子雨"：它与异步粒子雨用同一批雨滴、产出同一份视觉，
    // 只是逐雨滴发一次 draw call，是为了和异步路径做直接对照才临时留下的绘制方式，
    // 却被接进实验性内容子菜单成了玩家可选项（疯狂档满雨每帧 18000 次 draw call）
    // 它已整条移除，异步粒子雨从第 2 档顶到第 1 档
    // 设置里遗留的 2（异步粒子雨）被 clamp 回 1，仍然是它本人；遗留的 1（已删的粒子雨）
    // 也落到 1，即改用视觉完全相同、绘制便宜得多的异步路径
    int rainMode = 1;
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
