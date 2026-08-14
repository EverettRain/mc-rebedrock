#pragma once

// HUD/front-end types and constants shared between the renderer core
// (VulkanRenderer.cpp) and the extracted HUD drawing subsystem
// (HudRenderer). These were previously defined in VulkanRenderer.cpp's
// anonymous namespace, whose internal linkage prevented sharing across
// translation units; moving them into mc::render here lets both sides refer
// to the same definitions without drift.

#include "gameplay/ScreenHandler.hpp"

#include <cstddef>
#include <cstdint>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

namespace mc::render {

// guiTextures array layers, kept in sync with createGuiTexture(): 11 is
// 1.16.1's misc/vignette.png and 12 is the baked Screen.renderBackground dim
// gradient.
inline constexpr float kVignetteGuiLayer = 11.0F;
inline constexpr float kScreenDimGuiLayer = 12.0F;
inline constexpr float kMenuListBackgroundGuiLayer = 13.0F;
// The 1.16.1 title screen ships six panorama faces that form the world behind
// the logo; the title carousel cycles them as slides.
inline constexpr std::size_t kPanoramaFaces = 6U;
// Six category tabs on the top row, plus Spawn Eggs and Inventory on the bottom.
inline constexpr std::size_t kCreativeTabCount = 8U;

enum class MenuButton : std::uint8_t {
    None,
    Resume,
    Options,
    Quit,
    Resolution,
    GuiScale,
    ViewDistance,
    SimulationDistance,
    MasterVolume,
    VideoSettings,
    Controls,
    AutoJump,
    FrameRateLimit,
    AntiAliasing,
    Anisotropy,
    ViewBobbing,
    SmoothLighting,
    DynamicLight,
    Vsync,
    Done,
    Singleplayer,
    Exit,
    PlaySelected,
    CreateWorld,
    Edit,
    SaveRename,
    DeleteWorld,
    DeleteConfirm,
    DeleteCancel,
    Back,
    CreateConfirm,
    CreateGameMode,
    Respawn,
    TitleScreen,
    Language,
    ForceUnicodeFont,
    Difficulty,
    Experimental,
    RainMode,
    ParticleLevel,
    SunShadows,
    RainCollisionCache,
    SaveQuit,
};

// Which screen is open is a gameplay fact — the slot routing keys off it — so
// the enum lives with the ScreenHandler and the renderer just names it.
using ContainerScreen = gameplay::ContainerScreen;

struct HudPush final {
    glm::vec4 rect;
    glm::vec4 color;
    glm::vec4 uvRect;
    glm::vec4 data;
};

// Title-screen panorama cube: x = yaw, y = pitch (radians), z = tan(fov/2),
// w = aspect ratio. blur.x is the background-only blur radius in framebuffer
// pixels (26.1 defaults to 5); the remaining values are reserved.
struct PanoramaPush final {
    glm::vec4 rotationFov;
    glm::vec4 blur;
};

struct ItemPush final {
    glm::vec4 positionSize;
    glm::vec4 textureLayersRotation;
    glm::vec4 data;
    // Optional xyz dimensions for non-uniform cuboids. A zero vector keeps
    // the legacy scalar size stored in positionSize.w.
    glm::vec4 dimensions;
    glm::mat4 viewModelTransform{1.0F};
};

static_assert(sizeof(ItemPush) <= 128U, "Item push constants must fit Vulkan's guaranteed minimum");

} // namespace mc::render
