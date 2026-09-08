#pragma once

#include "audio/SoundCategory.hpp"
#include "world/WorldConstants.hpp"

#include <filesystem>
#include <string>

namespace mc::config {

// 抗锯齿档位。见 GameOptions::antiAliasing。
enum class AntiAliasingMode : int {
    Off = 0,
    // 多重采样。几何边缘有效，alpha-test 的边与着色器内部的高频（阴影边、AO 边）
    // 一概管不到：MSAA 每像素只跑一次片元着色器
    Msaa = 1,
    // 时间性抗锯齿（TAA 树）。抖动 + 历史累积，覆盖上面那两类 MSAA 够不着的边
    Taa = 2,
};

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
    // TAA：抗锯齿是**三档**，不是两个独立开关。MSAA 与 TAA 同时开是纯浪费——
    // TAA 已经覆盖几何边缘，而 MSAA 对 alpha-test 的边（树叶、草、栅栏走 discard，
    // 整个片元被丢掉）本来就无从插手，那正是画面里最脏的一块。
    // 存储值即档序，写进 options 文件的是数字；旧文件里的 `true` 读成 Msaa、
    // `false` 读成 Off，两者恰好就是 1 与 0，迁移因此不需要额外的分支。
    AntiAliasingMode antiAliasing = AntiAliasingMode::Msaa;
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
    // UI-6c：26.1 §7.6 Controls 那一屏的六个设置项（偏差 D3：本作一个都没有）。
    //
    // 四个 Hold/Toggle 是**输入语义**：false = 按住生效（vanilla 的默认），
    // true = 按一下切换。26.1 的 caption 复用动作名本身（`key.sneak` 等），
    // 值标签是 `options.key.hold` / `options.key.toggle`（`Options.java:563-576`）。
    //
    // ★ 这一轮它们**只是存起来**：玩法侧还没有读它们（潜行/疾跑/攻击/使用四条输入
    // 路径都还是按住语义）。控件是真的——有值、能切、能存盘——但功能没跟上，
    // 这是 UI 线既定的做法（"控件集要铺满，即使实际功能没跟上"）。已登记。
    bool toggleCrouch = false;
    bool toggleSprint = false;
    bool toggleAttack = false;
    bool toggleUse = false;
    // 双击前进键激活疾跑的最大时间间隔，单位 tick。0 = 关（`Options.java:578-583`，
    // 默认 7）。同样只存不用。
    int sprintWindow = 7;
    // 创造模式物品栏里的管理员用品页签（`options.operatorItemsTab`，默认 false）。
    bool operatorItemsTab = false;
    // Smooth lighting (ambient occlusion) is on/off, as in 26.1
    // (`OptionInstance.createBoolean("options.ao", true)`), and on by default for
    // the same reason. RN-19b removed the self-invented middle tier that used to
    // hold this default and that no vanilla setting corresponds to. Toggling it
    // no longer remeshes the world: one algorithm bakes the mesh either way and
    // Off just tells the shader to read the flat light and drop the AO channel.
    mc::world::SmoothLightingQuality smoothLightingQuality =
        mc::world::SmoothLightingQuality::On;
    bool dynamicLight = false;
    // RN-35：太阳阴影的级联。开时近段一张 16 格框的阴影图（纹素 1/128 格）盖住玩家
    // 周围，远段沿用 128 格那张；关时只画远段那一张，回到 RN-34 的形态。
    // 默认开——它买到的是「影子贴着投它的方块」，代价是多一趟阴影预通道。
    // ★ 这一档是**初始化期读一次**的：帧图在建交换链资源时编译，级联的那一步是
    // 编译期剪枝而不是运行期 if。改它必须重编译帧图（applyOptionChanged 里那一条），
    // 而离屏出图要钉死它就得钉在 glfwInit 之前（见 initialize()）。
    bool cascadedShadows = true;
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
    // UI-6e：视场角（26.1 `Options.fov`，`IntRange(30, 110)`，默认 70）。
    // ★ 在这之前**相机的 FOV 是构造时的常量**，玩家无从改起。
    // 26.1 里 70 显示 "Normal"、110 显示 "Quake Pro"，其余显示数字——注意 70 是
    // **默认值**不是最小值（最小是 30），那两个特例的判据是取值本身，不是"到头了"。
    int fieldOfView = 70;
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
