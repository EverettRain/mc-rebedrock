#include "render/vulkan/VulkanRenderer.hpp"
#include "render/vulkan/VulkanResources.hpp"
#include "render/vulkan/GpuSceneBuffer.hpp"
#include "render/vulkan/OffscreenTarget.hpp"

#include "animation/AnimationAssets.hpp"
#include "animation/DisplayEntityAnimation.hpp"
#include "animation/HingeAnimation.hpp"
#include "animation/ModelAnimationSystem.hpp"
#include "animation/PlayerModelAnimator.hpp"
#include "animation/SkeletalModel.hpp"
#include "assets/ImageData.hpp"
#include "audio/AudioSystem.hpp"
#include "gameplay/ChestSystem.hpp"
#include "gameplay/command/CommandDispatcher.hpp"
#include "gameplay/command/GameplayArguments.hpp"
#include "gameplay/ContentRegistry.hpp"
#include "gameplay/CraftingSystem.hpp"
#include "gameplay/EntitySystem.hpp"
#include "gameplay/GameMode.hpp"
#include "gameplay/GameRules.hpp"
#include "gameplay/GameSession.hpp"
#include "gameplay/entities/CowEntity.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "gameplay/entities/PigEntity.hpp"
#include "gameplay/entities/SpeciesRenderData.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/ItemEntitySystem.hpp"
#include "gameplay/ItemPlacement.hpp"
#include "gameplay/SpawnEggItems.hpp"
#include "gameplay/MiningSystem.hpp"
#include "gameplay/PlayerController.hpp"
#include "gameplay/PlayerVitals.hpp"
#include "gameplay/WorldSimulation.hpp"
#include "persistence/SaveRepository.hpp"
#include "render/Frustum.hpp"
#include "render/ParticleSystem.hpp"
#include "render/RainSystem.hpp"
#include "render/PerspectiveCamera.hpp"
#include "render/StreamingBudget.hpp"
#include "ui/BitmapFontMetrics.hpp"
#include "ui/ButtonControl.hpp"
#include "ui/ChatHistory.hpp"
#include "ui/HudLayout.hpp"
#include "ui/MenuSystem.hpp"
#include "ui/UiFrameData.hpp"
#include "ui/Language.hpp"
#include "ui/TextFont.hpp"
#include "ui/PageStack.hpp"
#include "world/BlockPlacement.hpp"
#include "world/ChunkMesher.hpp"
#include "world/ChunkStreamer.hpp"
#include "world/DayNightCycle.hpp"
#include "world/VoxelRaycast.hpp"
#include "world/WorldConstants.hpp"
#include "world/WorldLightEngine.hpp"
#include "world/gen/Biome.hpp"
#include "world/gen/LayeredBiomeSource.hpp"

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <vk_mem_alloc.h>

#include <glm/ext/matrix_transform.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <bit>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mc::render {
namespace {

constexpr std::size_t kFramesInFlight = 2;
// Occlusion queries gate a section's opaque draw behind the depth the closer
// terrain wrote earlier in the same frame. Each in-flight frame owns a
// contiguous slot range; results are read back after the frame's fence. The
// pool stays well under Metal's 64 KiB visibility-result buffer: at 8 bytes a
// precise query, 4096 queries would sit exactly on that edge.
constexpr std::size_t kOcclusionQueriesPerFrame = 2048;
constexpr std::size_t kOcclusionQueryPoolSize = kOcclusionQueriesPerFrame * kFramesInFlight;
constexpr std::uint32_t kOcclusionHysteresisFrames = 2;
constexpr std::uint32_t kWaterAnimationFrameCount = 32;

[[nodiscard]] world::SmoothLightingQuality nextSmoothLightingQuality(
    world::SmoothLightingQuality quality) {
    switch (quality) {
    case world::SmoothLightingQuality::Off: return world::SmoothLightingQuality::Standard;
    case world::SmoothLightingQuality::Standard: return world::SmoothLightingQuality::High;
    case world::SmoothLightingQuality::High: return world::SmoothLightingQuality::Off;
    }
    return world::SmoothLightingQuality::Standard;
}
// The block/entity/effect atlas opens with a fixed special section (the
// animated water/lava frames, the player-skin cuboids, the chest parts, the
// destroy stages, the sun and the moon phases), in a deterministic order so the
// mesher and the sky shader can keep constexpr bases for them. After it come
// the block textures, resolved by name from the block registry at startup (no
// placeholders — every layer is a real texture), then one layer per registered
// item. The total is computed at runtime from the built atlas.
constexpr std::uint32_t kLavaStillFrameCount = 20;
constexpr std::uint32_t kLavaFlowFrameCount = 16;
constexpr std::uint32_t kWaterStillLayer = 0U;
constexpr std::uint32_t kWaterFlowLayer = 32U;
constexpr std::uint32_t kLavaStillLayer = 64U;
constexpr std::uint32_t kLavaFlowLayer = 84U;
// The first block-texture layer: everything before it is the fixed special
// section (water 0-63, lava 64-99, player skin 100-135, destroy 136-145,
// chest parts 146-163, chest item faces 164-166, furnace 167-168, moon 169-176,
// sun 177).
constexpr std::uint32_t kFirstBlockTextureLayer = 178U;
constexpr float kPlayerHeadFirstLayer = 100.0F;
constexpr float kPlayerBodyFirstLayer = 106.0F;
constexpr float kPlayerRightArmFirstLayer = 112.0F;
constexpr float kPlayerLeftArmFirstLayer = 118.0F;
constexpr float kPlayerRightLegFirstLayer = 124.0F;
constexpr float kPlayerLeftLegFirstLayer = 130.0F;
constexpr float kDestroyStageFirstLayer = 136.0F;
constexpr float kChestBaseFirstLayer = 146.0F;
constexpr float kChestLidFirstLayer = 152.0F;
constexpr float kChestItemTopLayer = 164.0F;
constexpr float kChestItemFrontLayer = 165.0F;
constexpr float kChestItemSideLayer = 166.0F;
constexpr float kFurnaceFrontLayer = 167.0F;
// The burning furnace's front face (furnace_front_on.png) is the fixed layer
// right after the unlit front; only the mesher needs it.
// Eight vanilla moon_phases.png tiles (4x2 grid) fill the fixed layers 169-176
// and sun.png sits at 177. The sky shader reads them through the
// CameraUniform.celestialLayers uniform (which the renderer fills from these
// bases), so the two never drift when the atlas layout changes.
constexpr float kMoonPhaseFirstLayer = 169.0F;
constexpr float kSunLayer = 177.0F;
// Two-phase world entry: a world opens with a small chunk area around the gameSession.player()
// (vanilla enters with a small initial area and streams the view distance in
// during play), so a large render distance does not block the load screen on the
// whole (2·radius+1)² area before the gameSession.player() can move.
constexpr int kSpawnChunkRadius = 4;
// guiTextures array layers, kept in sync with createGuiTexture(): 11 is 1.16.1's
// misc/vignette.png and 12 is the baked Screen.renderBackground dim gradient.
constexpr float kVignetteGuiLayer = 11.0F;
constexpr float kScreenDimGuiLayer = 12.0F;
// The 1.16.1 title screen ships six panorama faces that form the world behind
// the logo; the title carousel cycles them as slides.
constexpr std::size_t kPanoramaFaces = 6U;


// Streaming one batch of generated chunks can hand the render thread hundreds
// of section meshes at once (each new chunk drags up to eight re-meshed
// neighbours with it); uploading all of them the same frame spikes the GPU, so
// the per-frame budget is spread adaptively (see render/StreamingBudget.hpp):
// the budget member is raised when the GPU is idle and lowered when stressed.
// The byte cap still bounds the absolute work per frame, and gameplay edits
// (place/break, fluid/sand cascades) jump ahead of streaming on a separate,
// budget-exempt bucket so a placed or broken block appears the same frame
// instead of waiting behind a streaming backlog; that bucket is still capped
// per frame so a large fluid cascade cannot stall the frame.
constexpr VkDeviceSize kMaxUploadBytesPerFrame = 8U * 1024U * 1024U;
constexpr std::size_t kMaxPrioritySectionUploadsPerFrame = 24;
// Ceiling on the render thread's queued-but-not-yet-uploaded section meshes.
// The worker generates far faster than the per-frame upload budget drains, and
// an unbounded queue lets CPU-side mesh data pile up (peaks >4500 sections were
// measured). The oldest low-priority entry is evicted beyond this.
constexpr std::size_t kMaxPendingSectionUpdates = 2048;
// Stream-mesh buffers are pooled by power-of-two size class and reused across
// section uploads instead of created/destroyed per mesh. MoltenVK does not hand
// freed MTLBuffer memory back to the OS, so reusing a buffer beats freeing it:
// the pool pins the graphics high-water mark at the working set rather than
// letting each streaming burst stack a fresh set of VMA blocks.
constexpr std::array<VkDeviceSize, 8> kStreamBufferClassSizes{
    16U * 1024U, 32U * 1024U, 64U * 1024U, 128U * 1024U,
    256U * 1024U, 512U * 1024U, 1024U * 1024U, 2U * 1024U * 1024U};
// Above this resident total the pool hands surplus free buffers back to the
// driver instead of hoarding them.
constexpr VkDeviceSize kMaxStreamBufferPoolBytes = 256U * 1024U * 1024U;
// Vertex and index buffers share one pool (a buffer may carry both usage bits).
constexpr VkBufferUsageFlags kStreamBufferDeviceUsage =
    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
    VK_BUFFER_USAGE_TRANSFER_DST_BIT;
constexpr const char* kPortabilityEnumeration = "VK_KHR_portability_enumeration";
constexpr const char* kPortabilitySubset = "VK_KHR_portability_subset";
constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr glm::vec4 kMenuBackgroundTint{0.25F, 0.25F, 0.25F, 1.0F};



struct PersistentEditPosition final {
    int x;
    int y;
    int z;

    [[nodiscard]] bool operator==(const PersistentEditPosition&) const = default;
};

struct PersistentEditPositionHash final {
    [[nodiscard]] std::size_t operator()(const PersistentEditPosition& position) const noexcept {
        std::size_t seed = std::hash<int>{}(position.x);
        seed ^= std::hash<int>{}(position.y) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
        seed ^= std::hash<int>{}(position.z) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
        return seed;
    }
};

// The windowed resolutions the Resolution button cycles through. Vanilla 1.16.1
// lists the monitor's fullscreen display modes; this app renders into a
// resizable window, so the list is a generous spread of common window sizes
// (from 4:3 laptops up to 16:9 desktop panels). The window itself may still be
// resized or maximized to any size — the list only drives the menu cycle.


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



// Six category tabs on the top row, plus Spawn Eggs and Inventory on the bottom.
constexpr std::size_t kCreativeTabCount = 8U;

enum class ContainerScreen : std::uint8_t {
    PlayerInventory,
    CraftingTable,
    Furnace,
    Chest,
};

// Camera view mode, cycled with F5 like Java Edition: first person, third person
// behind the gameSession.player(), then third person in front looking back.
enum class CameraPerspective : std::uint8_t {
    FirstPerson,
    ThirdPersonBack,
    ThirdPersonFront,
};

[[nodiscard]] constexpr CameraPerspective nextPerspective(CameraPerspective perspective) {
    switch (perspective) {
        case CameraPerspective::FirstPerson:
            return CameraPerspective::ThirdPersonBack;
        case CameraPerspective::ThirdPersonBack:
            return CameraPerspective::ThirdPersonFront;
        case CameraPerspective::ThirdPersonFront:
            break;
    }
    return CameraPerspective::FirstPerson;
}

#ifndef NDEBUG
constexpr bool kRequestValidation = true;
#else
constexpr bool kRequestValidation = false;
#endif

VKAPI_ATTR VkBool32 VKAPI_CALL
debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
              const VkDebugUtilsMessengerCallbackDataEXT* callbackData, void*) {
    const char* prefix = severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT
                             ? "Vulkan validation error"
                             : "Vulkan validation";
    std::cerr << prefix << ": " << callbackData->pMessage << '\n';
    return VK_FALSE;
}

[[nodiscard]] VkDebugUtilsMessengerCreateInfoEXT debugMessengerInfo() {
    auto info = vkStructure<VkDebugUtilsMessengerCreateInfoEXT>(
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT);
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = debugCallback;
    return info;
}

struct CameraUniform final {
    alignas(16) glm::mat4 model{1.0F};
    alignas(16) glm::mat4 view{1.0F};
    alignas(16) glm::mat4 projection{1.0F};
    alignas(16) glm::vec4 cameraPosition{0.0F};
    alignas(16) glm::vec4 sunDirection{0.0F};
    alignas(16) glm::vec4 horizonFog{0.0F};
    alignas(16) glm::vec4 renderSettings{0.0F};
    alignas(16) std::array<glm::vec4, 8> pointLights{};
    alignas(16) std::array<glm::vec4, 8> lightColors{};
    alignas(16) glm::vec4 lightingSettings{0.0F};
    // x = the sun's atlas layer, y = the first moon-phase atlas layer. The
    // fixed special section's layout is derived at startup, so the sky shader
    // reads the real layers from the uniform instead of hardcoding stale
    // numbers that drift when the atlas changes.
    alignas(16) glm::vec4 celestialLayers{0.0F};
    // x/y = frame-interpolated rain/thunder gradients, z = the vanilla visual
    // sky-light multiplier, w = celestial visibility (1 - rain). These are
    // presentation-only: the world and mesh light levels remain untouched.
    alignas(16) glm::vec4 weatherSettings{0.0F};
    // The sun-space view-projection the shadow pre-pass writes the depth map
    // with; the terrain shader projects each fragment into it to sample the map.
    // One frame behind the pre-pass's own matrix, which is the standard shadow
    // map lag.
    alignas(16) glm::mat4 lightViewProj{1.0F};
};

struct HudPush final {
    glm::vec4 rect;
    glm::vec4 color;
    glm::vec4 uvRect;
    glm::vec4 data;
};

// Title-screen panorama cube: x = yaw, y = pitch (radians), z = tan(fov/2),
// w = aspect ratio. The vertex shader builds the 85-degree view rays from it
// and the fragment shader ray-marches the cube faces.
struct PanoramaPush final {
    glm::vec4 rotationFov;
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

// Push constants for the sun-space shadow pre-pass: the light view-projection
// and the per-section origin, packed under Vulkan's 128-byte guarantee.
struct ShadowPush final {
    alignas(16) glm::mat4 lightViewProj;
    alignas(16) glm::vec4 sectionOrigin;
};
static_assert(sizeof(ShadowPush) <= 128U, "Shadow push constants must fit Vulkan's guaranteed minimum");

// The three rain render paths (MC_REBEDROCK_RAIN_MODE). All consume the same
// CPU-simulated drops; only the draw strategy differs, so the particle-async
// performance claim can be tested against a cheap texture baseline and the
// legacy per-particle draw path.
enum class RainMode { Texture, Particles, Async };

struct RainSheetPush final {
    glm::vec4 positionSize;      // xyz centre, w sheet size
    glm::vec4 timeOpacityLayer;  // x scroll time, y opacity, z atlas layer
};

struct QueueFamilyIndices final {
    std::optional<std::uint32_t> graphics;
    std::optional<std::uint32_t> present;

    [[nodiscard]] bool complete() const { return graphics.has_value() && present.has_value(); }
};

struct SwapchainSupport final {
    VkSurfaceCapabilitiesKHR capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

struct GpuMeshLayer final {
    VkDeviceSize vertexOffset = 0;
    VkDeviceSize indexOffset = 0;
    std::uint32_t indexCount = 0;
};

struct GpuMesh final {
    AllocatedBuffer vertexBuffer;
    AllocatedBuffer indexBuffer;
    GpuMeshLayer opaque;
    GpuMeshLayer cutout;
    GpuMeshLayer translucent;
    Aabb bounds;
    // Section origin the packed vertex positions are relative to; pushed to the
    // terrain shader per draw. Computed from the SectionPosition (a sparse
    // section's bounds.minimum is not the origin).
    glm::vec3 sectionOrigin{};
};

struct BufferCopyJob final {
    VkBuffer source = VK_NULL_HANDLE;
    VkBuffer destination = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
};

struct FrameContext final {
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkFence inFlight = VK_NULL_HANDLE;
    AllocatedBuffer uniformBuffer;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    std::vector<BufferCopyJob> uploadCopies;
    std::vector<AllocatedBuffer> retiredBuffers;
    // Occlusion queries recorded this frame: the count, the slot->section map
    // needed to apply results, and the readback scratch the next submission
    // of this frame index fills before the range is reused.
    std::uint32_t occlusionQueryCount = 0U;
    std::vector<world::SectionPosition> occlusionQuerySections;
    std::vector<std::uint64_t> occlusionQueryResults;
};

// Reusable stream-mesh buffers: free lists per size class plus per-frame
// deferred returns. A buffer stays deferred for kFramesInFlight frames — until
// the same frame slot's fence is waited in drawFrame — before it is handed back
// to the free list, guaranteeing the GPU has finished reading it.
struct StreamBufferPool final {
    std::array<std::vector<AllocatedBuffer>, kStreamBufferClassSizes.size()> freeByClass;
    std::array<std::vector<AllocatedBuffer>, kFramesInFlight> deferred;
    VkDeviceSize totalBytes = 0;
};

// A section's draw gating as far as occlusion is concerned. Unknown sections
// are drawn and queried; Visible ones keep drawing while re-checked; Occluded
// ones are skipped until a passing query proves them visible again.
enum class OcclusionState : std::uint8_t { Unknown, Visible, Occluded };

struct OcclusionQueryPushConstants final {
    alignas(16) glm::vec4 aabbMinimum;
    alignas(16) glm::vec4 aabbMaximum;
};

struct TextureArrayPixels final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
};

[[nodiscard]] std::vector<std::uint32_t> readSpirv(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file) {
        throw std::runtime_error("Unable to open shader: " + path.string());
    }
    const auto end = file.tellg();
    if (end <= 0 || static_cast<std::uint64_t>(end) % sizeof(std::uint32_t) != 0U) {
        throw std::runtime_error("Invalid SPIR-V file: " + path.string());
    }
    const auto byteCount = static_cast<std::size_t>(end);
    std::vector<std::uint32_t> code(byteCount / sizeof(std::uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(byteCount));
    if (!file) {
        throw std::runtime_error("Unable to read shader: " + path.string());
    }
    return code;
}

void requireSameSize(const assets::ImageData& first, const assets::ImageData& other) {
    if (first.width != other.width || first.height != other.height) {
        throw std::runtime_error("Grass block texture layers must have identical dimensions");
    }
}

[[nodiscard]] std::vector<assets::ImageData> animatedSquareFrames(const assets::ImageData& image,
                                                                  int targetSize) {
    if (image.width <= 0 || image.height < image.width || image.height % image.width != 0) {
        throw std::runtime_error("Animated block texture has invalid frame dimensions");
    }
    const int frameCount = image.height / image.width;
    std::vector<assets::ImageData> frames;
    frames.reserve(static_cast<std::size_t>(frameCount));
    for (int frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
        assets::ImageData frame;
        frame.width = targetSize;
        frame.height = targetSize;
        frame.rgba.resize(static_cast<std::size_t>(targetSize * targetSize * 4));
        for (int y = 0; y < targetSize; ++y) {
            const int sourceY = frameIndex * image.width + y * image.width / targetSize;
            for (int x = 0; x < targetSize; ++x) {
                const int sourceX = x * image.width / targetSize;
                const std::size_t source =
                    static_cast<std::size_t>((sourceY * image.width + sourceX) * 4);
                const std::size_t destination = static_cast<std::size_t>((y * targetSize + x) * 4);
                std::copy_n(image.rgba.begin() + static_cast<std::ptrdiff_t>(source), 4,
                            frame.rgba.begin() + static_cast<std::ptrdiff_t>(destination));
            }
        }
        frames.push_back(std::move(frame));
    }
    return frames;
}

[[nodiscard]] assets::ImageData resizedRegion(const assets::ImageData& image, int sourceX,
                                              int sourceY, int sourceWidth, int sourceHeight,
                                              int targetSize) {
    assets::ImageData result;
    result.width = targetSize;
    result.height = targetSize;
    result.rgba.resize(static_cast<std::size_t>(targetSize * targetSize * 4));
    for (int y = 0; y < targetSize; ++y) {
        for (int x = 0; x < targetSize; ++x) {
            const int sx = sourceX + x * sourceWidth / targetSize;
            const int sy = sourceY + y * sourceHeight / targetSize;
            const std::size_t source = static_cast<std::size_t>((sy * image.width + sx) * 4);
            const std::size_t target = static_cast<std::size_t>((y * targetSize + x) * 4);
            std::copy_n(image.rgba.begin() + static_cast<std::ptrdiff_t>(source), 4,
                        result.rgba.begin() + static_cast<std::ptrdiff_t>(target));
        }
    }
    return result;
}

using PlayerSkinFaces = std::array<assets::ImageData, 6>;

[[nodiscard]] PlayerSkinFaces playerSkinCuboidFaces(const assets::ImageData& skin, int textureX,
                                                    int textureY, int width, int height, int depth,
                                                    int targetSize) {
    // Minecraft's cuboid unwrap is top/bottom in the first row, followed by
    // right/front/left/back.  Store faces in the exact order emitted by
    // item_entity.vert: +X, -X, +Y, -Y, +Z, -Z.
    return {
        resizedRegion(skin, textureX + depth + width, textureY + depth, depth, height, targetSize),
        resizedRegion(skin, textureX, textureY + depth, depth, height, targetSize),
        resizedRegion(skin, textureX + depth, textureY, width, depth, targetSize),
        resizedRegion(skin, textureX + depth + width, textureY, width, depth, targetSize),
        resizedRegion(skin, textureX + depth, textureY + depth, width, height, targetSize),
        resizedRegion(skin, textureX + depth * 2 + width, textureY + depth, width, height,
                      targetSize),
    };
}

void overlayScaled(assets::ImageData& destination, const assets::ImageData& source,
                   int destinationX, int destinationY, int destinationWidth,
                   int destinationHeight) {
    for (int y = 0; y < destinationHeight; ++y) {
        for (int x = 0; x < destinationWidth; ++x) {
            const int sourceX = x * source.width / destinationWidth;
            const int sourceY = y * source.height / destinationHeight;
            const std::size_t sourceIndex =
                static_cast<std::size_t>((sourceY * source.width + sourceX) * 4);
            const std::size_t destinationIndex = static_cast<std::size_t>(
                ((destinationY + y) * destination.width + destinationX + x) * 4);
            const float alpha = static_cast<float>(source.rgba[sourceIndex + 3U]) / 255.0F;
            for (std::size_t channel = 0; channel < 3U; ++channel) {
                const float blended =
                    static_cast<float>(source.rgba[sourceIndex + channel]) * alpha +
                    static_cast<float>(destination.rgba[destinationIndex + channel]) *
                        (1.0F - alpha);
                destination.rgba[destinationIndex + channel] =
                    static_cast<std::uint8_t>(std::lround(blended));
            }
            destination.rgba[destinationIndex + 3U] = 255U;
        }
    }
}

[[nodiscard]] assets::ImageData stackedChestFace(const assets::ImageData& lid,
                                                 const assets::ImageData& base) {
    assets::ImageData result = base;
    for (int y = 0; y < result.height; ++y) {
        const int sourceRow = y * 15 / result.height;
        const bool lidRow = sourceRow < 5;
        const auto& source = lidRow ? lid : base;
        const int partRow = lidRow ? sourceRow : sourceRow - 5;
        const int partHeight = lidRow ? 5 : 10;
        const int sourceY = partRow * source.height / partHeight;
        for (int x = 0; x < result.width; ++x) {
            const std::size_t sourceIndex =
                static_cast<std::size_t>((sourceY * source.width + x) * 4);
            const std::size_t destinationIndex =
                static_cast<std::size_t>((y * result.width + x) * 4);
            std::copy_n(source.rgba.begin() + static_cast<std::ptrdiff_t>(sourceIndex), 4,
                        result.rgba.begin() + static_cast<std::ptrdiff_t>(destinationIndex));
        }
    }
    return result;
}

[[nodiscard]] std::uint8_t tintedChannel(std::uint8_t source, float tint) {
    const auto value = static_cast<int>(std::lround(static_cast<float>(source) * tint));
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

// Spawn-egg icon (1.16.1): `spawn_egg.png` paints only part of the egg's
// silhouette while `spawn_egg_overlay.png` supplies the spots that fill the
// rest; the two are tinted with the species' SpawnEggColors and blended by the
// overlay's alpha into one 16×16 layer. Pixels survive whenever EITHER texture
// is opaque: skipping every pixel with a transparent base would drop the spots
// (they sit on transparent base pixels) and leave only the shell colour.
[[nodiscard]] assets::ImageData buildSpawnEggIcon(
    const std::filesystem::path& itemDir,
    gameplay::entities::SpawnEggColors eggColors) {
    assets::ImageData egg = assets::ImageData::loadRgba(itemDir / "spawn_egg.png");
    const auto eggOverlay = assets::ImageData::loadRgba(itemDir / "spawn_egg_overlay.png");
    requireSameSize(egg, eggOverlay);
    const auto unpack = [](std::uint32_t rgb) {
        return std::array<int, 3>{static_cast<int>((rgb >> 16) & 0xFFU),
                                  static_cast<int>((rgb >> 8) & 0xFFU),
                                  static_cast<int>(rgb & 0xFFU)};
    };
    const std::array<int, 3> primary = unpack(eggColors.primary);
    const std::array<int, 3> secondary = unpack(eggColors.secondary);
    for (std::size_t p = 0; p + 3U < egg.rgba.size(); p += 4U) {
        const float baseAlpha = static_cast<float>(egg.rgba[p + 3U]) / 255.0F;
        const float overlayAlpha = static_cast<float>(eggOverlay.rgba[p + 3U]) / 255.0F;
        if (baseAlpha == 0.0F && overlayAlpha == 0.0F) {
            continue; // outside the egg shape entirely
        }
        std::array<std::uint8_t, 3> baseColor{};
        for (std::size_t c = 0; c < 3U; ++c) {
            if (baseAlpha > 0.0F) {
                baseColor[c] =
                    static_cast<std::uint8_t>(egg.rgba[p + c] * primary[c] / 255);
            }
        }
        // Spot-only pixels would inherit the transparent base's alpha and
        // vanish, so the output is opaque wherever either source has ink.
        egg.rgba[p + 3U] = 255U;
        for (std::size_t c = 0; c < 3U; ++c) {
            const float spot = static_cast<float>(eggOverlay.rgba[p + c] * secondary[c] / 255);
            const float base = static_cast<float>(baseColor[c]);
            egg.rgba[p + c] =
                static_cast<std::uint8_t>(spot * overlayAlpha + base * (1.0F - overlayAlpha));
        }
    }
    return egg;
}

[[nodiscard]] TextureArrayPixels loadGrassBlockTextures(const std::filesystem::path& root) {
    auto top = assets::ImageData::loadRgba(root / "grass_block_top.png");
    auto side = assets::ImageData::loadRgba(root / "grass_block_side.png");
    const auto overlay = assets::ImageData::loadRgba(root / "grass_block_side_overlay.png");
    const auto dirt = assets::ImageData::loadRgba(root / "dirt.png");
    auto grassPlant = assets::ImageData::loadRgba(root / "grass.png");
    auto oakLeaves = assets::ImageData::loadRgba(root / "oak_leaves.png");
    // The animated and entity frames that fill the fixed special section. Item
    // icons no longer live here; they append after the block textures.
    auto waterStillFrames =
        animatedSquareFrames(assets::ImageData::loadRgba(root / "water_still.png"), top.width);
    auto waterFlowFrames =
        animatedSquareFrames(assets::ImageData::loadRgba(root / "water_flow.png"), top.width);
    auto lavaStillFrames =
        animatedSquareFrames(assets::ImageData::loadRgba(root / "lava_still.png"), top.width);
    auto lavaFlowFrames =
        animatedSquareFrames(assets::ImageData::loadRgba(root / "lava_flow.png"), top.width);
    auto sunFrames = animatedSquareFrames(
        assets::ImageData::loadRgba(root.parent_path() / "environment" / "sun.png"), top.width);
    const auto moonPhasesImage =
        assets::ImageData::loadRgba(root.parent_path() / "environment" / "moon_phases.png");
    std::array<assets::ImageData, 8> moonPhaseTiles;
    for (int phase = 0; phase < 8; ++phase) {
        const int column = phase % 4;
        const int row = phase / 4;
        moonPhaseTiles[static_cast<std::size_t>(phase)] = resizedRegion(
            moonPhasesImage, column * moonPhasesImage.width / 4,
            row * moonPhasesImage.height / 2, moonPhasesImage.width / 4,
            moonPhasesImage.height / 2, top.width);
    }
    const auto playerSkin =
        assets::ImageData::loadRgba(root.parent_path() / "entity" / "steve.png");
    const std::array playerParts{
        playerSkinCuboidFaces(playerSkin, 0, 0, 8, 8, 8, top.width),
        playerSkinCuboidFaces(playerSkin, 16, 16, 8, 12, 4, top.width),
        playerSkinCuboidFaces(playerSkin, 40, 16, 4, 12, 4, top.width),
        playerSkinCuboidFaces(playerSkin, 32, 48, 4, 12, 4, top.width),
        playerSkinCuboidFaces(playerSkin, 0, 16, 4, 12, 4, top.width),
        playerSkinCuboidFaces(playerSkin, 16, 48, 4, 12, 4, top.width),
    };
    const auto chestTexture =
        assets::ImageData::loadRgba(root.parent_path() / "entity" / "chest" / "normal.png");
    const auto furnaceFront = assets::ImageData::loadRgba(root / "furnace_front.png");
    const auto furnaceFrontOn = assets::ImageData::loadRgba(root / "furnace_front_on.png");
    auto chestParts = std::array{
        playerSkinCuboidFaces(chestTexture, 0, 19, 14, 10, 14, top.width),
        playerSkinCuboidFaces(chestTexture, 0, 0, 14, 5, 14, top.width),
        playerSkinCuboidFaces(chestTexture, 0, 0, 2, 4, 1, top.width),
    };
    std::swap(chestParts[1][2], chestParts[1][3]);
    const auto latchUpper = resizedRegion(chestTexture, 1, 1, 2, 2, top.width);
    const auto latchLower = resizedRegion(chestTexture, 1, 3, 2, 2, top.width);
    overlayScaled(chestParts[1][4], latchUpper, 7, 10, 2, 6);
    overlayScaled(chestParts[0][4], latchLower, 7, 0, 2, 3);
    const std::array chestItemTextures{
        chestParts[1][2],
        stackedChestFace(chestParts[1][4], chestParts[0][4]),
        stackedChestFace(chestParts[1][0], chestParts[0][0]),
    };
    std::array<assets::ImageData, 10> destroyStages;
    for (std::size_t stage = 0; stage < destroyStages.size(); ++stage) {
        destroyStages[stage] =
            assets::ImageData::loadRgba(root / ("destroy_stage_" + std::to_string(stage) + ".png"));
    }
    // The biome leaves and their tints (spruce/birch fixed, the rest the biome
    // foliage colour), the same set the tree shapes grow.
    constexpr std::array<float, 3> foliageTint{0.49F, 0.74F, 0.32F};
    constexpr std::array<float, 3> spruceTint{0x61 / 255.0F, 0x99 / 255.0F, 0x61 / 255.0F};
    constexpr std::array<float, 3> birchTint{0x80 / 255.0F, 0xA7 / 255.0F, 0x55 / 255.0F};
    std::array<assets::ImageData, 5> biomeLeafTextures{
        assets::ImageData::loadRgba(root / "spruce_leaves.png"),
        assets::ImageData::loadRgba(root / "birch_leaves.png"),
        assets::ImageData::loadRgba(root / "jungle_leaves.png"),
        assets::ImageData::loadRgba(root / "acacia_leaves.png"),
        assets::ImageData::loadRgba(root / "dark_oak_leaves.png"),
    };
    const std::array<std::array<float, 3>, 5> biomeLeafTints{
        spruceTint, birchTint, foliageTint, foliageTint, foliageTint};
    // Untinted leaf bases for the terrain (per-block biome tint happens per
    // vertex in the mesher), while the tinted ones above stay for items/GUI.
    const auto biomeLeafTexturesRaw = biomeLeafTextures;
    for (std::size_t leaf = 0; leaf < biomeLeafTextures.size(); ++leaf) {
        auto& pixels = biomeLeafTextures[leaf].rgba;
        for (std::size_t index = 0; index + 3U < pixels.size(); index += 4U) {
            for (std::size_t channel = 0; channel < 3U; ++channel) {
                pixels[index + channel] =
                    tintedChannel(pixels[index + channel], biomeLeafTints[leaf][channel]);
            }
        }
    }
    if (waterStillFrames.size() != kWaterAnimationFrameCount ||
        waterFlowFrames.size() != kWaterAnimationFrameCount) {
        throw std::runtime_error("Minecraft water textures must contain 32 animation frames");
    }
    if (lavaStillFrames.size() != kLavaStillFrameCount ||
        lavaFlowFrames.size() != kLavaFlowFrameCount) {
        throw std::runtime_error("Minecraft lava textures must contain 20/16 animation frames");
    }
    if (sunFrames.size() != 1U) {
        throw std::runtime_error("Minecraft sun texture must contain one square frame");
    }
    // Untinted grass family, kept for the per-biome colour variants below.
    const auto grassTopRaw = top;
    const auto grassSideBase = side;
    const auto grassOverlay = overlay;
    const auto grassPlantRaw = grassPlant;
    const auto leavesRaw = oakLeaves;
    // Tint the foliage and grass the vanilla colours before they enter the atlas.
    const auto tintInPlace = [](assets::ImageData& image, const std::array<float, 3>& tint) {
        for (std::size_t index = 0; index + 3U < image.rgba.size(); index += 4U) {
            for (std::size_t channel = 0; channel < 3U; ++channel) {
                image.rgba[index + channel] =
                    tintedChannel(image.rgba[index + channel], tint[channel]);
            }
        }
    };
    tintInPlace(top, foliageTint);
    for (std::size_t index = 0; index + 3U < side.rgba.size(); index += 4U) {
        const float alpha = static_cast<float>(overlay.rgba[index + 3U]) / 255.0F;
        for (std::size_t channel = 0; channel < 3U; ++channel) {
            const auto overlayColor = tintedChannel(overlay.rgba[index + channel], foliageTint[channel]);
            const float blended = static_cast<float>(side.rgba[index + channel]) * (1.0F - alpha) +
                                  static_cast<float>(overlayColor) * alpha;
            side.rgba[index + channel] = static_cast<std::uint8_t>(
                std::clamp(static_cast<int>(std::lround(blended)), 0, 255));
        }
        side.rgba[index + 3U] = 255U;
    }
    tintInPlace(grassPlant, foliageTint);
    tintInPlace(oakLeaves, foliageTint);
    const auto tintWaterFrames = [&](std::vector<assets::ImageData>& frames) {
        constexpr std::array<float, 3> waterTint{0.25F, 0.48F, 0.92F};
        for (auto& frame : frames) {
            for (std::size_t index = 0; index + 3U < frame.rgba.size(); index += 4U) {
                for (std::size_t channel = 0; channel < 3U; ++channel) {
                    frame.rgba[index + channel] =
                        tintedChannel(frame.rgba[index + channel], waterTint[channel]);
                }
                frame.rgba[index + 3U] = 155U;
            }
        }
    };
    tintWaterFrames(waterStillFrames);
    tintWaterFrames(waterFlowFrames);

    // ---- Fixed special section, in a deterministic order ----
    std::vector<assets::ImageData> layers;
    const auto append = [&](const assets::ImageData& image) {
        requireSameSize(top, image);
        layers.push_back(image);
    };
    for (const auto& frame : waterStillFrames) append(frame);  // 0..31
    for (const auto& frame : waterFlowFrames) append(frame);   // 32..63
    for (const auto& frame : lavaStillFrames) append(frame);   // 64..83
    for (const auto& frame : lavaFlowFrames) append(frame);    // 84..99
    for (const auto& part : playerParts) {
        for (const auto& face : part) append(face);            // 100..135
    }
    for (const auto& stage : destroyStages) append(stage);     // 136..145
    for (const auto& part : chestParts) {
        for (const auto& face : part) append(face);            // 146..163
    }
    for (const auto& texture : chestItemTextures) append(texture);  // 164..166
    append(furnaceFront);                                       // 167
    append(furnaceFrontOn);                                     // 168
    for (const auto& tile : moonPhaseTiles) append(tile);       // 169..176
    append(sunFrames.front());                                  // 177

    // ---- Dynamic block textures, name-driven from the block registry ----
    // Baked composites register by name so every block that reuses them finds
    // the same layer (grass_block_side, dirt, the tinted leaves, ...).
    if (layers.size() != kFirstBlockTextureLayer) {
        throw std::runtime_error("Fixed texture section does not match kFirstBlockTextureLayer");
    }
    std::unordered_map<std::string, float> layerByName;
    const auto assign = [&](const char* name) -> float {
        const auto existing = layerByName.find(name);
        if (existing != layerByName.end()) {
            return existing->second;
        }
        // The crop stages are a contiguous run from their stage-0 layer, so the
        // mesher can read stage0 + age.
        const std::string_view view{name};
        auto stageFor = [&](std::string_view prefix, int count) -> float {
            if (!view.starts_with(prefix)) {
                return -1.0F;
            }
            const float first = static_cast<float>(layers.size());
            for (int stage = 0; stage < count; ++stage) {
                const std::string file{prefix};
                const std::string image = file.substr(0, file.size() - 1) + std::to_string(stage);
                assets::ImageData pixels = assets::ImageData::loadRgba(root / (image + ".png"));
                requireSameSize(top, pixels);
                layers.push_back(std::move(pixels));
            }
            return first;
        };
        if (const float wheat = stageFor("wheat_stage0", 8); wheat >= 0.0F) {
            layerByName.emplace(name, wheat);
            return wheat;
        }
        if (const float carrot = stageFor("carrots_stage0", 4); carrot >= 0.0F) {
            layerByName.emplace(name, carrot);
            return carrot;
        }
        if (const float potato = stageFor("potatoes_stage0", 4); potato >= 0.0F) {
            layerByName.emplace(name, potato);
            return potato;
        }
        // Farmland's moist variant sits right after its dry face.
        if (view == "farmland") {
            const float first = static_cast<float>(layers.size());
            for (const char* file : {"farmland", "farmland_moist"}) {
                assets::ImageData pixels = assets::ImageData::loadRgba(root / (std::string(file) + ".png"));
                requireSameSize(top, pixels);
                layers.push_back(std::move(pixels));
            }
            layerByName.emplace(name, first);
            return first;
        }
        assets::ImageData pixels = assets::ImageData::loadRgba(root / (std::string(name) + ".png"));
        requireSameSize(top, pixels);
        const float index = static_cast<float>(layers.size());
        layers.push_back(std::move(pixels));
        layerByName.emplace(name, index);
        return index;
    };
    // The baked composites register first so reuses share their layer.
    layerByName.emplace("grass_block_top", static_cast<float>(layers.size()));
    layers.push_back(top);
    layerByName.emplace("grass_block_side", static_cast<float>(layers.size()));
    layers.push_back(side);
    layerByName.emplace("dirt", static_cast<float>(layers.size()));
    layers.push_back(dirt);
    layerByName.emplace("grass", static_cast<float>(layers.size()));
    layers.push_back(grassPlant);
    layerByName.emplace("oak_leaves", static_cast<float>(layers.size()));
    layers.push_back(oakLeaves);
    const std::array<const char*, 5> biomeLeafNames{
        "spruce_leaves", "birch_leaves", "jungle_leaves", "acacia_leaves", "dark_oak_leaves"};
    for (std::size_t leaf = 0; leaf < biomeLeafNames.size(); ++leaf) {
        layerByName.emplace(biomeLeafNames[leaf], static_cast<float>(layers.size()));
        layers.push_back(biomeLeafTextures[leaf]);
    }

    // ---- Per-biome grass/foliage colours (1.16.1 BiomeColors) ----
    // The vanilla grass and foliage colour maps are 256x256 lookups indexed by
    // temperature and rainfall. Each biome's grass and foliage colour comes
    // from its own map; the mesher blends them bilinearly per block (the way
    // 1.16.1's BlockView.getColor does) so a biome boundary reads as a smooth
    // colour gradient instead of a hard switch. Swamp and dark forest carry
    // their 1.16.1 overrides below.
    const auto loadColormap = [&](const char* name) {
        return assets::ImageData::loadRgba(root.parent_path() / "colormap" / name);
    };
    const auto grassColormap = loadColormap("grass.png");
    const auto foliageColormap = loadColormap("foliage.png");
    const auto colormapColor = [](const assets::ImageData& colormap, float temperature,
                                  float downfall) -> std::uint32_t {
        const float clampedTemperature = std::clamp(temperature, 0.0F, 1.0F);
        const float humidity = std::clamp(downfall, 0.0F, 1.0F) * clampedTemperature;
        const int xIndex = static_cast<int>((1.0 - clampedTemperature) * 255.0);
        const int yIndex = static_cast<int>((1.0 - humidity) * 255.0);
        const std::size_t pixel = static_cast<std::size_t>(yIndex * 256 + xIndex) * 4U;
        if (pixel + 3U >= colormap.rgba.size()) {
            return 0x00FF00U;
        }
        return (static_cast<std::uint32_t>(colormap.rgba[pixel]) << 16U) |
               (static_cast<std::uint32_t>(colormap.rgba[pixel + 1U]) << 8U) |
               static_cast<std::uint32_t>(colormap.rgba[pixel + 2U]);
    };
    const auto colorTint = [](std::uint32_t color) -> std::array<float, 3> {
        return {
            static_cast<float>((color >> 16U) & 0xFFU) / 255.0F,
            static_cast<float>((color >> 8U) & 0xFFU) / 255.0F,
            static_cast<float>(color & 0xFFU) / 255.0F,
        };
    };
    // The terrain grass family and oak-family leaves render the UNTINTED
    // textures and take their colour from the fragment shader's biome-colour
    // lookup (a linear-filtered texture sample, so the biome boundary blends as
    // a smooth per-pixel gradient). The grass SIDE keeps its baked per-biome
    // layer so the dirt under a cliff stays dirt, and spruce/birch leaves keep
    // their fixed 1.16.1 tones.
    const float terrainGrassTop = static_cast<float>(layers.size());
    layers.push_back(grassTopRaw);
    layerByName.emplace("grass_block_top:terrain", terrainGrassTop);
    const float terrainGrassPlant = static_cast<float>(layers.size());
    layers.push_back(grassPlantRaw);
    layerByName.emplace("grass:terrain", terrainGrassPlant);
    world::gen::setTerrainGrassLayers(terrainGrassTop, terrainGrassPlant);
    const std::array<world::Block, 6> leafBlocks{
        world::Block::OakLeaves, world::Block::SpruceLeaves, world::Block::BirchLeaves,
        world::Block::JungleLeaves, world::Block::AcaciaLeaves, world::Block::DarkOakLeaves,
    };
    const std::array<const assets::ImageData*, 6> leafTextures{
        &leavesRaw, &biomeLeafTexturesRaw[0], &biomeLeafTexturesRaw[1],
        &biomeLeafTexturesRaw[2], &biomeLeafTexturesRaw[3], &biomeLeafTexturesRaw[4],
    };
    const std::array<const char*, 6> leafNames{
        "oak", "spruce", "birch", "jungle", "acacia", "dark_oak"};
    const std::array<std::uint32_t, 6> leafFixedTints{
        0U, 0x619961U, 0x80A755U, 0U, 0U, 0U};
    for (std::size_t leaf = 0; leaf < leafBlocks.size(); ++leaf) {
        auto pixels = *leafTextures[leaf];
        if (leafFixedTints[leaf] != 0U) {
            const auto tint = colorTint(leafFixedTints[leaf]);
            for (std::size_t index = 0; index + 3U < pixels.rgba.size(); index += 4U) {
                for (std::size_t channel = 0; channel < 3U; ++channel) {
                    pixels.rgba[index + channel] =
                        tintedChannel(pixels.rgba[index + channel], tint[channel]);
                }
            }
        }
        const float layer = static_cast<float>(layers.size());
        layers.push_back(pixels);
        layerByName.emplace(std::string("leaves:terrain:") + leafNames[leaf], layer);
        world::gen::setTerrainLeafLayer(leafBlocks[leaf], layer);
    }
    // Baked per-biome grass family: the top/side/plant are tinted with the
    // biome's grass colour at atlas build time, so the rendered colour never
    // depends on per-vertex data reaching the fragment shader.
    const auto buildBiomeGrass = [&](std::string_view suffix, std::uint32_t color) {
        auto biomeTop = grassTopRaw;
        auto biomeSide = grassSideBase;
        auto biomePlant = grassPlantRaw;
        const auto tint = colorTint(color);
        for (std::size_t index = 0; index + 3U < biomeTop.rgba.size(); index += 4U) {
            for (std::size_t channel = 0; channel < 3U; ++channel) {
                biomeTop.rgba[index + channel] =
                    tintedChannel(biomeTop.rgba[index + channel], tint[channel]);
                biomePlant.rgba[index + channel] =
                    tintedChannel(biomePlant.rgba[index + channel], tint[channel]);
            }
        }
        for (std::size_t index = 0; index + 3U < biomeSide.rgba.size(); index += 4U) {
            const float alpha = static_cast<float>(grassOverlay.rgba[index + 3U]) / 255.0F;
            for (std::size_t channel = 0; channel < 3U; ++channel) {
                const auto overlayColor =
                    tintedChannel(grassOverlay.rgba[index + channel], tint[channel]);
                const float blended =
                    static_cast<float>(grassSideBase.rgba[index + channel]) * (1.0F - alpha) +
                    static_cast<float>(overlayColor) * alpha;
                biomeSide.rgba[index + channel] = static_cast<std::uint8_t>(
                    std::clamp(static_cast<int>(std::lround(blended)), 0, 255));
            }
            biomeSide.rgba[index + 3U] = 255U;
        }
        const std::string prefix{suffix};
        const float topLayer = static_cast<float>(layers.size());
        layers.push_back(biomeTop);
        layerByName.emplace("grass_block_top:" + prefix, topLayer);
        const float sideLayer = static_cast<float>(layers.size());
        layers.push_back(biomeSide);
        layerByName.emplace("grass_block_side:" + prefix, sideLayer);
        const float plantLayer = static_cast<float>(layers.size());
        layers.push_back(biomePlant);
        layerByName.emplace("grass:" + prefix, plantLayer);
        return world::BlockTextureLayers{topLayer, sideLayer, plantLayer};
    };
    // Baked per-biome foliage layer for the oak family.
    const auto buildLeafLayer = [&](std::string_view suffix, const assets::ImageData& texture,
                                    std::uint32_t color) {
        auto pixels = texture;
        const auto tint = colorTint(color);
        for (std::size_t index = 0; index + 3U < pixels.rgba.size(); index += 4U) {
            for (std::size_t channel = 0; channel < 3U; ++channel) {
                pixels.rgba[index + channel] =
                    tintedChannel(pixels.rgba[index + channel], tint[channel]);
            }
        }
        const float layer = static_cast<float>(layers.size());
        layers.push_back(pixels);
        layerByName.emplace(std::string("leaves:") + std::string{suffix}, layer);
        return layer;
    };
    for (int biomeIndex = 0; biomeIndex < static_cast<int>(world::gen::Biome::Count); ++biomeIndex) {
        const auto biome = static_cast<world::gen::Biome>(biomeIndex);
        const auto& definition = world::gen::biomeDefinition(biome);
        std::uint32_t grassColor =
            colormapColor(grassColormap, definition.temperature, definition.downfall);
        if (biome == world::gen::Biome::DarkForest) {
            // DarkForestBiome#getGrassColorAt darkens the colormap colour.
            grassColor = ((grassColor & 0xFEFEFEU) + 0x28340AU) >> 1U;
        }
        std::uint32_t foliageColor =
            colormapColor(foliageColormap, definition.temperature, definition.downfall);
        if (biome == world::gen::Biome::Swamp) {
            // SwampBiome#getFoliageColor is the fixed 0x6A7039.
            foliageColor = 0x6A7039U;
        }
        if (biome == world::gen::Biome::Swamp) {
            // SwampBiome#getGrassColorAt picks 0x6A7039 or 0x4C763C by noise;
            // the mesher chooses the per-block tone from FOLIAGE_NOISE.
            world::gen::setBiomeGrassLayers(biome, buildBiomeGrass("swamp", 0x6A7039U));
            world::gen::setSwampDarkGrassLayers(buildBiomeGrass("swamp_dark", 0x4C763CU));
        } else {
            world::gen::setBiomeGrassLayers(
                biome, buildBiomeGrass(definition.identifier, grassColor));
        }
        // Baked per-biome oak-family foliage; spruce/birch keep the fixed terrain
        // layers built above.
        const std::string prefix{definition.identifier};
        world::gen::setBiomeFoliageLayer(
            biome, world::Block::OakLeaves,
            buildLeafLayer(prefix + ":oak", leavesRaw, foliageColor));
        world::gen::setBiomeFoliageLayer(
            biome, world::Block::JungleLeaves,
            buildLeafLayer(prefix + ":jungle", biomeLeafTexturesRaw[2], foliageColor));
        world::gen::setBiomeFoliageLayer(
            biome, world::Block::AcaciaLeaves,
            buildLeafLayer(prefix + ":acacia", biomeLeafTexturesRaw[3], foliageColor));
        world::gen::setBiomeFoliageLayer(
            biome, world::Block::DarkOakLeaves,
            buildLeafLayer(prefix + ":dark_oak", biomeLeafTexturesRaw[4], foliageColor));
    }

    for (const auto& definition : world::kBlockRegistry) {
        const auto block = definition.block;
        if (block == world::Block::Air) {
            continue;
        }
        if (block == world::Block::Water) {
            world::setBlockTextureLayers(
                block, {static_cast<float>(kWaterStillLayer),
                        static_cast<float>(kWaterFlowLayer),
                        static_cast<float>(kWaterFlowLayer)});
            continue;
        }
        if (block == world::Block::Lava) {
            world::setBlockTextureLayers(
                block, {static_cast<float>(kLavaStillLayer),
                        static_cast<float>(kLavaFlowLayer),
                        static_cast<float>(kLavaFlowLayer)});
            continue;
        }
        if (block == world::Block::Chest) {
            // The dropped chest item draws the baked chest-item faces.
            world::setBlockTextureLayers(
                block, {static_cast<float>(kChestItemTopLayer),
                        static_cast<float>(kChestItemSideLayer),
                        static_cast<float>(kChestItemSideLayer)});
            continue;
        }
        world::BlockTextureLayers resolved;
        resolved.top = assign(definition.textures.top);
        resolved.side = assign(definition.textures.side);
        resolved.bottom = assign(definition.textures.bottom);
        world::setBlockTextureLayers(block, resolved);
    }

    TextureArrayPixels output;
    output.width = static_cast<std::uint32_t>(top.width);
    output.height = static_cast<std::uint32_t>(top.height);
    for (const auto& layer : layers) {
        output.rgba.insert(output.rgba.end(), layer.rgba.begin(), layer.rgba.end());
    }
    const std::uint32_t baseLayerCount = static_cast<std::uint32_t>(layers.size());
    // Item icons: one appended layer per registered item, in registry order.
    const auto itemDir = root.parent_path() / "item";
    std::uint32_t itemIndex = 0U;
    const auto appendItemIcon = [&](const gameplay::Item* item) {
        assets::ImageData icon;
        if (const auto* spawnEgg = gameplay::asSpawnEgg(item)) {
            icon = buildSpawnEggIcon(itemDir, spawnEgg->entityType().spawnEgg());
        } else {
            icon = assets::ImageData::loadRgba(itemDir /
                                               (std::string{item->textureName} + ".png"));
        }
        requireSameSize(top, icon);
        output.rgba.insert(output.rgba.end(), icon.rgba.begin(), icon.rgba.end());
        gameplay::setItemTextureLayer(
            item, static_cast<float>(baseLayerCount + itemIndex));
        ++itemIndex;
    };
    for (const gameplay::Item* item : gameplay::kItemRegistry)
        appendItemIcon(item);
    for (const gameplay::Item* item : gameplay::kSpawnEggItems)
        appendItemIcon(item);
    // Lava's animation frames live in the fixed section (64..99); nothing trails
    // the item icons, so the whole atlas is exactly the layers above.
    return output;
}

[[nodiscard]] assets::ImageData repeatTileToAtlas(const assets::ImageData& tile, int width,
                                                  int height, int repeats) {
    assets::ImageData atlas;
    atlas.width = width;
    atlas.height = height;
    atlas.rgba.resize(static_cast<std::size_t>(width * height * 4));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int sourceX = (x * tile.width * repeats / width) % tile.width;
            const int sourceY = (y * tile.height * repeats / height) % tile.height;
            const std::size_t sourceIndex =
                static_cast<std::size_t>((sourceY * tile.width + sourceX) * 4);
            const std::size_t destinationIndex = static_cast<std::size_t>((y * width + x) * 4);
            std::copy_n(tile.rgba.begin() + static_cast<std::ptrdiff_t>(sourceIndex), 4,
                        atlas.rgba.begin() + static_cast<std::ptrdiff_t>(destinationIndex));
        }
    }
    return atlas;
}

[[nodiscard]] assets::ImageData singleChestGui(const assets::ImageData& generic) {
    assets::ImageData result = generic;
    std::ranges::fill(result.rgba, 0U);
    const auto copyRows = [&](int sourceY, int destinationY, int rowCount) {
        for (int row = 0; row < rowCount; ++row) {
            const auto source = generic.rgba.begin() +
                                static_cast<std::ptrdiff_t>(((sourceY + row) * generic.width) * 4);
            const auto destination =
                result.rgba.begin() +
                static_cast<std::ptrdiff_t>(((destinationY + row) * result.width) * 4);
            std::copy_n(source, static_cast<std::size_t>(generic.width * 4), destination);
        }
    };
    // GenericContainerScreen stitches the upper chest rows to the lower
    // gameSession.player()-gameSession.inventory() region. Bake the three-row variant into one atlas layer.
    copyRows(0, 0, 71);
    copyRows(126, 71, 96);
    return result;
}

} // namespace

struct VulkanRenderer::Impl final : public gameplay::SimulationHost {
    Impl(std::filesystem::path shaderDirectory, std::filesystem::path textureDirectory,
         std::filesystem::path soundDirectory, world::ChunkStreamer& streamer,
         config::GameOptions initialOptions, std::filesystem::path initialOptionsPath,
         std::filesystem::path saveRoot,
         std::optional<TestSceneOptions> initialTestScene)
        : shaderRoot(std::move(shaderDirectory)), blockTextureRoot(std::move(textureDirectory)),
          optionsPath(std::move(initialOptionsPath)), saveRepository(std::move(saveRoot)),
          options(std::move(initialOptions)),
          testScene(initialTestScene),
          audioSystem(std::move(soundDirectory), options.masterVolume), chunkStreamer(streamer),
          camera(initialTestScene.has_value() && initialTestScene->occlusionScene
                     ? glm::vec3{8.0F, 60.0F, -8.0F}
                     : (initialTestScene.has_value() ? glm::vec3{10.7F, 66.2F, 12.1F}
                                                     : glm::vec3{24.0F, 78.0F, 24.0F}),
                 initialTestScene.has_value() && initialTestScene->occlusionScene
                     ? glm::vec3{8.0F, 47.0F, 16.0F}
                     : (initialTestScene.has_value() ? glm::vec3{8.5F, 64.5F, 8.5F}
                                                     : glm::vec3{8.0F, 61.0F, 8.0F}),
                 65.0F) {
        viewDistanceChunks = chunkStreamer.loadRadius();
        simulationDistanceChunks = std::clamp(options.simulationDistance, 2, 12);
        gameSession.setSimulationRadius(static_cast<float>(simulationDistanceChunks) *
                                        static_cast<float>(world::kChunkWidth));
        menuSystem.guiScaleSetting = options.guiScale;
        const auto resolution =
            std::ranges::find_if(ui::kDisplayResolutions, [this](const ui::DisplayResolution& candidate) {
                return candidate.width == options.windowWidth &&
                       candidate.height == options.windowHeight;
            });
        menuSystem.resolutionIndex =
            resolution == ui::kDisplayResolutions.end()
                ? 0U
                : static_cast<std::size_t>(std::distance(ui::kDisplayResolutions.begin(), resolution));
        // The command tree owns every command. Registering through it gives each
        // command typed arguments, argument validation and tab completion — the
        // tables behind the arguments (items, blocks, game modes, rules) feed
        // completion and validation from one source of truth, so adding an entry
        // to any table makes it appear in the command for free.
        registerGameCommands();


        // Let the held-item and gameSession.player()-preview animators pick up any authored
        // clips shipped under resources/animation. blockTextureRoot points at
        // resources/vanilla/1.16.1/textures/minecraft/block, so its fifth parent
        // is the resource root. Both animators keep their built-in clips if the
        // files are absent, so this is best-effort and never fatal.
        try {
            const auto animationRoot = blockTextureRoot.parent_path()
                                           .parent_path()
                                           .parent_path()
                                           .parent_path()
                                           .parent_path() /
                                       "animation";
            heldItemAnimation.load(animationRoot);
            playerModelAnimator.load(animationRoot);
            // The gameSession.inventory()/creative preview turns its whole body toward the
            // cursor (the look clip rotates the body at half the head's yaw).
            // The world gameSession.player() keeps body-follow off: its body already follows
            // the look direction through the renderer's separate body-yaw logic.
            playerModelAnimator.setBodyFollowsLook(true);
            worldPlayerAnimator.load(animationRoot);
        } catch (const std::exception& exception) {
            std::cerr << "Animation assets unavailable, using built-in clips: "
                      << exception.what() << '\n';
        }
    }

    void registerGameCommands() {
        commandDispatcher.literal("gamemode")
            .argument("mode", gameplay::command::kGameModeArgument)
            .executes([this](const gameplay::command::CommandContext& context) {
                const auto mode = context.find<gameplay::GameMode>("mode");
                if (!mode.has_value()) {
                    return gameplay::CommandResult{false,
                                                   "Usage: /gamemode <survival|creative>"};
                }
                setGameMode(*mode);
                return gameplay::CommandResult{
                    true, "Set own game mode to " + std::string{gameplay::gameModeName(*mode)}};
            });
        commandDispatcher.literal("time")
            .then("set")
            .argument("time", gameplay::command::kTimeArgument)
            .executes([this](const gameplay::command::CommandContext& context) {
                const auto ticks = context.find<double>("time");
                if (!ticks.has_value()) {
                    return gameplay::CommandResult{
                        false, "Usage: /time set <day|noon|night|midnight|ticks>"};
                }
                double elapsedTicks = *ticks - world::DayNightCycle::kNewWorldTick;
                if (elapsedTicks < 0.0)
                    elapsedTicks += world::DayNightCycle::kTicksPerDay;
                gameSession.gameTimeSeconds() = elapsedTicks / world::DayNightCycle::kTicksPerSecond;
                return gameplay::CommandResult{
                    true, "Set the time to " + std::to_string(static_cast<int>(*ticks))};
            });
        commandDispatcher.literal("give")
            .argument("item", gameplay::command::kGiveItemArgument)
            .argument("count", gameplay::command::kIntArgument)
            .executes([this](const gameplay::command::CommandContext& context) {
                const auto itemToken = context.find<std::string>("item");
                const auto count = context.find<std::int64_t>("count");
                if (!itemToken.has_value() || !count.has_value()) {
                    return gameplay::CommandResult{false,
                                                   "Usage: /give <item|index> [count]"};
                }
                // GiveItemArgument guarantees the token is a catalog index or a
                // known item/block identifier. A bare number is an index into the
                // creative catalog; anything else is a block or item identifier
                // (the `rebedrock:` key, the vanilla alias, or the bare path).
                const bool numeric =
                    std::all_of(itemToken->begin(), itemToken->end(),
                                [](char c) { return c >= '0' && c <= '9'; });
                gameplay::ItemStack requested;
                std::string identifier;
                if (numeric) {
                    std::size_t index = 0;
                    for (const char c : *itemToken) {
                        index = index * 10U + static_cast<std::size_t>(c - '0');
                    }
                    const auto catalog = gameplay::contentRegistry().allCatalog();
                    if (index >= catalog.size()) {
                        return gameplay::CommandResult{
                            false, "Catalog index out of range (0.." +
                                std::to_string(catalog.size() - 1U) + ")"};
                    }
                    requested = catalog[index];
                    identifier = gameplay::itemName(requested);
                } else if (const auto block = world::blockFromIdentifier(*itemToken);
                           block.has_value()) {
                    requested = {*block, 1U, gameplay::blockItemFor(*block)};
                    identifier = world::blockDefinition(*block).identifier.toString();
                } else if (const auto* item = gameplay::itemFromIdentifier(*itemToken);
                           item != nullptr) {
                    requested = {world::Block::Air, 1U, item};
                    identifier = item->identifier.toString();
                }
                if (*count <= 0) {
                    return gameplay::CommandResult{false, "Count must be positive"};
                }
                requested.count =
                    static_cast<std::uint8_t>(std::min<std::int64_t>(*count, 255));
                // Hand over as many whole stacks as needed; anything that will
                // not fit spills onto the ground at the gameSession.player()'s feet.
                auto& inventory = activeInventory();
                const auto maximum = gameplay::itemMaximumStackSize(requested);
                std::size_t given = 0;
                while (!requested.empty()) {
                    gameplay::ItemStack stack = requested;
                    stack.count = std::min(requested.count, maximum);
                    // add() consumes what it places, so remember the amount
                    // handed to it for the success tally.
                    const std::uint8_t intended = stack.count;
                    if (gameSession.inventory().add(stack)) {
                        given += intended;
                    } else {
                        gameSession.itemEntities().spawn(gameSession.player().position(), stack, {0.0F, 0.2F, 0.0F});
                    }
                    requested.count = static_cast<std::uint8_t>(requested.count - intended);
                }
                return gameplay::CommandResult{
                    true, "Gave " + std::to_string(given) + "x " + identifier};
            });
        // The change handler survives world switches; loading a save re-attaches
        // it because copying the save's GameRules brings a null handler along.
        attachGameRuleHandlers();
        // gamerule keeps GameRules as its rule engine; the tree only supplies the
        // validated rule name and the raw value string it parses. The rule node
        // is both executable (the query form) and parent of the optional value,
        // which is how the `[<value>]` trailing argument is expressed.
        commandDispatcher.literal("gamerule")
            .argument("rule", gameplay::command::kGameRuleArgument)
            .executes([this](const gameplay::command::CommandContext& context) {
                const auto rule = context.find<std::string>("rule");
                if (!rule.has_value()) {
                    return gameplay::CommandResult{false, "Usage: /gamerule <rule> [<value>]"};
                }
                return gameSession.gameRules().query(*rule);
            })
            .argument("value", gameplay::command::kStringArgument)
            .executes([this](const gameplay::command::CommandContext& context) {
                const auto rule = context.find<std::string>("rule");
                const auto value = context.find<std::string>("value");
                if (!rule.has_value() || !value.has_value()) {
                    return gameplay::CommandResult{false, "Usage: /gamerule <rule> [<value>]"};
                }
                return gameSession.gameRules().setFromCommand(*rule, *value);
            });
        // Every built-in species is registered up front (the registry is normally
        // populated lazily on first spawn), so entity-target commands resolve from
        // the very first world.
        gameplay::entities::registerBuiltinEntities();
        // /tp <x> <y> <z> [<yaw> <pitch>] teleports the gameSession.player() to a position
        // (relative `~` axes allowed); /tp <entity> teleports onto a creature.
        // The destination node is both executable (the position and entity forms
        // without rotation) and parent of the optional rotation argument.
        commandDispatcher.literal("tp")
            .argument("destination", gameplay::command::kTeleportDestinationArgument)
            .executes([this](const gameplay::command::CommandContext& context) {
                return teleportWithContext(context, false);
            })
            .argument("rotation", gameplay::command::kRotationArgument)
            .executes([this](const gameplay::command::CommandContext& context) {
                return teleportWithContext(context, true);
            });
        // /kill kills the gameSession.player(); /kill <entity> kills every spawned creature of
        // a registered species, the way 1.16.1's KillCommand targets a selector.
        // Both sides route through the shared kill() pipeline — OutOfWorld damage
        // at infinite magnitude — exactly like vanilla KillCommand's entity.kill().
        commandDispatcher.literal("kill")
            .executes([this](const gameplay::command::CommandContext&) {
                gameSession.killPlayer(*this);
                return gameplay::CommandResult{true, "Killed the player"};
            })
            .argument("target", gameplay::command::kEntityTargetArgument)
            .executes([this](const gameplay::command::CommandContext& context) {
                const auto target = context.find<std::string>("target");
                if (!target.has_value()) {
                    return gameplay::CommandResult{false, "Usage: /kill [<entity>]"};
                }
                if (*target == "player") {
                    gameSession.killPlayer(*this);
                    return gameplay::CommandResult{true, "Killed the player"};
                }
                std::size_t killed = 0U;
                for (const auto& entity : gameSession.worldEntities().entities()) {
                    if (entity.type != nullptr &&
                        (entity.type->id().matches(*target) ||
                         entity.type->vanillaId().matches(*target))) {
                        gameSession.worldEntities().kill(entity.id);
                        ++killed;
                    }
                }
                if (killed == 0U) {
                    return gameplay::CommandResult{
                        false, "No entities of that species are spawned"};
                }
                return gameplay::CommandResult{
                    true, "Killed " + std::to_string(killed) + "x " + *target};
            });
        // /spawnpoint [<pos>]: sets the player's personal spawn point, the way
        // 1.16.1's SpawnPointCommand calls ServerPlayerEntity#setSpawnPoint.
        // Without a position the command uses the player's own block position;
        // with one, `~` axes resolve relative to the player. Death then respawns
        // here. 1.16.1 carries no angle, so neither does the command.
        commandDispatcher.literal("spawnpoint")
            .executes([this](const gameplay::command::CommandContext&) {
                return applySpawnPoint(std::nullopt);
            })
            .argument("pos", gameplay::command::kTeleportDestinationArgument)
            .executes([this](const gameplay::command::CommandContext& context) {
                const auto position = context.find<gameplay::command::Position3>("pos");
                if (!position.has_value()) {
                    return gameplay::CommandResult{false, "Usage: /spawnpoint [<x> <y> <z>]"};
                }
                const glm::vec3 base = gameSession.player().position();
                const glm::vec3 target{
                    position->relativeX ? base.x + static_cast<float>(position->x)
                                        : static_cast<float>(position->x),
                    position->relativeY ? base.y + static_cast<float>(position->y)
                                        : static_cast<float>(position->y),
                    position->relativeZ ? base.z + static_cast<float>(position->z)
                                        : static_cast<float>(position->z),
                };
                return applySpawnPoint(target);
            });
        // /weather clear|rain [<duration>] sets the world's weather the way
        // 1.16.1's WeatherCommand does: no-arg applies a 6000-tick spell (five
        // minutes), a duration argument is seconds converted to ticks at 20 per
        // second. clear sets setWeather(ticks, 0, false, false), rain sets
        // setWeather(0, ticks, true, false); once a spell expires the
        // doWeatherCycle-driven auto-cycle takes over. The `duration` node sits
        // under the literal the way WeatherCommand nests it, so both forms
        // execute from the one tree.
        const auto setClearWeather = [this](int ticks) {
            gameSession.weatherSystem().setWeather(ticks, 0, false, false);
            return gameplay::CommandResult{true, "Cleared the weather"};
        };
        const auto setRainWeather = [this](int ticks) {
            gameSession.weatherSystem().setWeather(0, ticks, true, false);
            return gameplay::CommandResult{true, "It started raining"};
        };
        commandDispatcher.literal("weather")
            .then("clear")
            .executes([setClearWeather](const gameplay::command::CommandContext&) {
                return setClearWeather(6000);
            })
            .argument("duration", gameplay::command::kWeatherDurationArgument)
            .executes([setClearWeather](const gameplay::command::CommandContext& context) {
                const auto seconds = context.find<std::int64_t>("duration");
                return seconds.has_value()
                    ? setClearWeather(static_cast<int>(*seconds * 20))
                    : gameplay::CommandResult{false, "Usage: /weather clear [<duration>]"};
            });
        // Registration is idempotent, so a second literal("weather") walk adds
        // the sibling `rain` branch under the same root node.
        commandDispatcher.literal("weather")
            .then("rain")
            .executes([setRainWeather](const gameplay::command::CommandContext&) {
                return setRainWeather(6000);
            })
            .argument("duration", gameplay::command::kWeatherDurationArgument)
            .executes([setRainWeather](const gameplay::command::CommandContext& context) {
                const auto seconds = context.find<std::int64_t>("duration");
                return seconds.has_value()
                    ? setRainWeather(static_cast<int>(*seconds * 20))
                    : gameplay::CommandResult{false, "Usage: /weather rain [<duration>]"};
            });
        // /weather thunder [<duration>] installs a storm (rain + thunder the way
        // WeatherCommand's thunder branch does): setWeather(0, ticks, true, true)
        // with the same duration handling as the clear/rain branches. No
        // lightning or thunder sound yet — that is the next step.
        const auto setThunderWeather = [this](int ticks) {
            gameSession.weatherSystem().setWeather(0, ticks, true, true);
            return gameplay::CommandResult{true, "It started thundering"};
        };
        commandDispatcher.literal("weather")
            .then("thunder")
            .executes([setThunderWeather](const gameplay::command::CommandContext&) {
                return setThunderWeather(6000);
            })
            .argument("duration", gameplay::command::kWeatherDurationArgument)
            .executes([setThunderWeather](const gameplay::command::CommandContext& context) {
                const auto seconds = context.find<std::int64_t>("duration");
                return seconds.has_value()
                    ? setThunderWeather(static_cast<int>(*seconds * 20))
                    : gameplay::CommandResult{false, "Usage: /weather thunder [<duration>]"};
            });
    }

    ~Impl() { shutdown(); }

    // SimulationHost: the render-side reactions the game session's tick drives.
    // submitWorldEdit and previewBlockEdit are the Impl's own methods, marked
    // override at their definitions; the remaining host methods are here.
    void playBlockBreak(world::Block block, glm::vec3 position) override {
        audioSystem.playBlockBreak(block, position);
    }
    void playItemPickup(glm::vec3 position) override { audioSystem.playItemPickup(position); }
    void playEat(glm::vec3 position) override { audioSystem.playEat(position); }
    void playPlayerHurt(glm::vec3 position) override { audioSystem.playPlayerHurt(position); }
    void playPlayerFall(glm::vec3 position, float damage) override {
        audioSystem.playPlayerFall(position, damage);
    }
    void playBurp(glm::vec3 position) override { audioSystem.playBurp(position); }
    void playCreatureHurt(const gameplay::entities::EntityType& type, glm::vec3 position) override {
        audioSystem.playCreatureHurt(type.soundProfile(), position);
    }
    void playCreatureDeath(const gameplay::entities::EntityType& type, glm::vec3 position) override {
        audioSystem.playCreatureDeath(type.soundProfile(), position);
    }
    void playCreatureAmbient(const gameplay::entities::EntityType& type, glm::vec3 position) override {
        audioSystem.playCreatureAmbient(type.soundProfile(), position);
    }
    void playCreatureStep(const gameplay::entities::EntityType& type, glm::vec3 position) override {
        audioSystem.playCreatureStep(type.soundProfile(), position);
    }
    void playFootstep(world::Block ground, glm::vec3 position, float volume) override {
        audioSystem.playFootstep(ground, position, volume);
    }
    void playSplash(glm::vec3 position, float volume) override {
        audioSystem.playSplash(position, volume);
    }
    void spawnBlockBreakParticles(glm::ivec3 position, world::Block block) override {
        particleSystem.spawnBlockBreak(position, block);
    }
    void onPlayerDied() override {
        std::cout << "Player died\n";
        if (inventoryOpen) {
            setInventoryOpen(false);
        }
        if (chatOpen) {
            chatInputText.clear();
            chatOpen = false;
        }
        // The gameplay half — scatter the inventory unless keepInventory — runs
        // after the inventory closes, so the crafting grid has already stowed.
        gameSession.onPlayerDeath();
        paused = true;
        menuSystem.pageStack.reset(ui::PageId::Death);
        gameSession.input() = {};
        gameSession.jumpPressed() = false;
        releaseInteractionButtons();
        dropRequested = false;
        pressedMenuButton = MenuButton::None;
        firstMouseSample = true;
        unlockCursor();
    }
    void onFurnaceStateChanged() override { updateFurnaceLitState(); }
    void onEatingStarted() override {
        // The held-item Eat animation starts the meal; the generic.eat sound is
        // the chew loop GameSession::tickEating drives, not a one-shot here.
        heldItemAnimation.trigger(animation::ModelAction::Eat);
    }
    void onEatingCancelled() override { heldItemAnimation.trigger(animation::ModelAction::None); }

    void initialize() {
        if (glfwInit() != GLFW_TRUE) {
            throw std::runtime_error("GLFW initialization failed");
        }
        glfwInitialized = true;
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
        window = glfwCreateWindow(options.windowWidth, options.windowHeight,
                                  "MC Rebedrock - Vulkan 3D Grass Block", nullptr, nullptr);
        if (window == nullptr) {
            throw std::runtime_error("GLFW window creation failed");
        }
        glfwSetWindowUserPointer(window, this);
        glfwSetFramebufferSizeCallback(window, [](GLFWwindow* callbackWindow, int, int) {
            static_cast<Impl*>(glfwGetWindowUserPointer(callbackWindow))->framebufferResized = true;
        });
        glfwSetKeyCallback(window, [](GLFWwindow* callbackWindow, int key, int, int action,
                                      int modifiers) {
            auto* renderer = static_cast<Impl*>(glfwGetWindowUserPointer(callbackWindow));
            const auto currentPage = renderer->menuSystem.pageStack.current();
            if (currentPage == ui::PageId::ConfirmDelete) {
                if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
                    renderer->menuSystem.pageStack.pop();
                }
                return;
            }
            if (currentPage == ui::PageId::CreateWorld ||
                currentPage == ui::PageId::EditWorld) {
                auto& name = currentPage == ui::PageId::CreateWorld
                                 ? renderer->menuSystem.createWorldName
                                 : renderer->menuSystem.editWorldName;
                if (key == GLFW_KEY_BACKSPACE && (action == GLFW_PRESS || action == GLFW_REPEAT) &&
                    !name.empty()) {
                    name.pop_back();
                } else if ((key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) &&
                           action == GLFW_PRESS) {
                    renderer->playUiClick();
                    if (currentPage == ui::PageId::CreateWorld) {
                        renderer->startNewWorld();
                    } else {
                        renderer->applyRename();
                    }
                } else if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
                    renderer->menuSystem.pageStack.pop();
                }
                return;
            }
            if (renderer->chatOpen) {
                if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
                    renderer->setChatOpen(false);
                } else if ((key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) &&
                           action == GLFW_PRESS) {
                    renderer->submitChatInput();
                } else if (key == GLFW_KEY_TAB && action == GLFW_PRESS) {
                    renderer->cycleChatSuggestion();
                } else if (key == GLFW_KEY_BACKSPACE &&
                           (action == GLFW_PRESS || action == GLFW_REPEAT) &&
                           !renderer->chatInputText.empty()) {
                    renderer->chatInputText.pop_back();
                    renderer->refreshChatSuggestions();
                }
                return;
            }
            if ((key == GLFW_KEY_T || key == GLFW_KEY_SLASH) && action == GLFW_PRESS &&
                !renderer->inventoryOpen && !renderer->paused) {
                renderer->setChatOpen(true);
                if (key == GLFW_KEY_SLASH) {
                    renderer->chatInputText = "/";
                    renderer->suppressedOpeningChatCodepoint = static_cast<unsigned int>('/');
                } else {
                    renderer->suppressedOpeningChatCodepoint = static_cast<unsigned int>('t');
                }
                return;
            }
            if (key == GLFW_KEY_F3 && action == GLFW_PRESS) {
                renderer->debugOverlayOpen = !renderer->debugOverlayOpen;
                return;
            }
            if (key == GLFW_KEY_F5 && action == GLFW_PRESS) {
                renderer->cameraPerspective = nextPerspective(renderer->cameraPerspective);
                return;
            }
            if (key == GLFW_KEY_W && action == GLFW_PRESS && !renderer->inventoryOpen &&
                !renderer->paused) {
                renderer->gameSession.forwardPressed() = true;
            }
            if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
                const auto page = renderer->menuSystem.pageStack.current();
                if (page == ui::PageId::VideoSettings) {
                    renderer->menuSystem.pageStack.pop();
                    renderer->pressedMenuButton = MenuButton::None;
                    renderer->menuSystem.viewDistanceSliderDragging = false;
                    renderer->menuSystem.simulationDistanceSliderDragging = false;
                } else if (page == ui::PageId::Experimental) {
                    renderer->menuSystem.pageStack.pop();
                    renderer->pressedMenuButton = MenuButton::None;
                } else if (page == ui::PageId::Options) {
                    renderer->menuSystem.pageStack.pop();
                    renderer->menuSystem.optionsOpen = false;
                    renderer->pressedMenuButton = MenuButton::None;
                    renderer->menuSystem.viewDistanceSliderDragging = false;
                    renderer->menuSystem.simulationDistanceSliderDragging = false;
                    renderer->menuSystem.masterVolumeSliderDragging = false;
                } else if (page == ui::PageId::Pause) {
                    renderer->setPaused(false);
                } else if (page == ui::PageId::WorldList) {
                    renderer->menuSystem.pageStack.pop();
                } else if (page == ui::PageId::Game) {
                    renderer->setPaused(true);
                }
                return;
            }
            if (key == GLFW_KEY_E && action == GLFW_PRESS && !renderer->paused) {
                renderer->setInventoryOpen(!renderer->inventoryOpen);
            }
            if (key == GLFW_KEY_SPACE && action == GLFW_PRESS) {
                if (!renderer->inventoryOpen && !renderer->paused) {
                    renderer->gameSession.jumpPressed() = true;
                }
            }
            if (!renderer->paused && action == GLFW_PRESS && key >= GLFW_KEY_1 &&
                key <= GLFW_KEY_9) {
                renderer->activeInventory().selectHotbar(
                    static_cast<std::size_t>(key - GLFW_KEY_1));
            }
            if (key == GLFW_KEY_Q && action == GLFW_PRESS && !renderer->inventoryOpen &&
                !renderer->paused) {
                renderer->dropRequested = true;
                renderer->dropWholeStack = (modifiers & GLFW_MOD_CONTROL) != 0;
            }
        });
        glfwSetCharCallback(window, [](GLFWwindow* callbackWindow, unsigned int codepoint) {
            auto* renderer = static_cast<Impl*>(glfwGetWindowUserPointer(callbackWindow));
            const auto currentPage = renderer->menuSystem.pageStack.current();
            if (currentPage == ui::PageId::CreateWorld ||
                currentPage == ui::PageId::EditWorld) {
                auto& name = currentPage == ui::PageId::CreateWorld
                                 ? renderer->menuSystem.createWorldName
                                 : renderer->menuSystem.editWorldName;
                if (codepoint >= 32U && codepoint <= 126U && name.size() < 32U) {
                    name.push_back(static_cast<char>(codepoint));
                }
                return;
            }
            if (!renderer->chatOpen) {
                return;
            }
            const bool suppressedT =
                renderer->suppressedOpeningChatCodepoint == static_cast<unsigned int>('t') &&
                (codepoint == static_cast<unsigned int>('t') ||
                 codepoint == static_cast<unsigned int>('T'));
            if (codepoint == renderer->suppressedOpeningChatCodepoint || suppressedT) {
                renderer->suppressedOpeningChatCodepoint = 0U;
                return;
            }
            renderer->suppressedOpeningChatCodepoint = 0U;
            if (codepoint >= 32U && codepoint <= 126U && renderer->chatInputText.size() < 256U) {
                renderer->chatInputText.push_back(static_cast<char>(codepoint));
                renderer->refreshChatSuggestions();
            }
        });
        glfwSetScrollCallback(window, [](GLFWwindow* callbackWindow, double, double yOffset) {
            auto* renderer = static_cast<Impl*>(glfwGetWindowUserPointer(callbackWindow));
            if (renderer->menuSystem.pageStack.current() == ui::PageId::WorldList && yOffset != 0.0) {
                renderer->scrollWorldList(yOffset > 0.0 ? -1 : 1);
            } else if (renderer->menuSystem.pageStack.current() == ui::PageId::Language &&
                       yOffset != 0.0) {
                renderer->scrollLanguageList(yOffset > 0.0 ? -1 : 1);
            } else if (renderer->inventoryOpen &&
                       renderer->gameSession.gameMode() == gameplay::GameMode::Creative && yOffset != 0.0) {
                renderer->scrollCreative(yOffset > 0.0 ? -1 : 1);
            } else if (!renderer->inventoryOpen && !renderer->paused && !renderer->chatOpen &&
                       yOffset != 0.0) {
                renderer->activeInventory().scrollHotbar(yOffset > 0.0 ? -1 : 1);
            }
        });
        glfwSetMouseButtonCallback(
            window, [](GLFWwindow* callbackWindow, int button, int action, int modifiers) {
                auto* renderer = static_cast<Impl*>(glfwGetWindowUserPointer(callbackWindow));
                if (renderer->chatOpen) {
                    return;
                }
                if (renderer->paused) {
                    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) {
                        renderer->handleMenuButtonPress();
                    } else if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_RELEASE) {
                        renderer->handleMenuButtonRelease();
                    }
                    return;
                }
                if (renderer->inventoryOpen) {
                    if (action == GLFW_RELEASE) {
                        // Both buttons end a drag; the release also stops the
                        // creative scrollbar drag.
                        renderer->handleInventoryButtonRelease();
                        return;
                    }
                    if (action != GLFW_PRESS) {
                        return;
                    }
                    if (button == GLFW_MOUSE_BUTTON_LEFT || button == GLFW_MOUSE_BUTTON_RIGHT) {
                        renderer->handleInventoryClick(button == GLFW_MOUSE_BUTTON_RIGHT
                                                           ? gameplay::InventoryMouseButton::Right
                                                           : gameplay::InventoryMouseButton::Left,
                                                       (modifiers & GLFW_MOD_SHIFT) != 0);
                    }
                    return;
                }
                if (button == GLFW_MOUSE_BUTTON_LEFT) {
                    renderer->breakButtonHeld = action != GLFW_RELEASE;
                    if (action == GLFW_PRESS) {
                        renderer->breakBlockRequested = true;
                    } else if (action == GLFW_RELEASE) {
                        renderer->miningTarget.reset();
                    }
                } else if (button == GLFW_MOUSE_BUTTON_RIGHT) {
                    renderer->useButtonHeld = action != GLFW_RELEASE;
                    if (action == GLFW_PRESS) {
                        renderer->placeBlockRequested = true;
                    }
                }
            });
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        if (glfwRawMouseMotionSupported() == GLFW_TRUE) {
            glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        }
        glfwSetCursorPosCallback(window, [](GLFWwindow* callbackWindow, double x, double y) {
            auto* renderer = static_cast<Impl*>(glfwGetWindowUserPointer(callbackWindow));
            if (renderer->inventoryOpen) {
                if (renderer->creativeScrollbarDragging) {
                    renderer->updateCreativeScrollFromCursor();
                }
                if (renderer->inventoryDragActive) {
                    renderer->collectInventoryDragSlot(x, y);
                }
                return;
            }
            if (renderer->paused &&
                (renderer->menuSystem.viewDistanceSliderDragging ||
                 renderer->menuSystem.simulationDistanceSliderDragging ||
                 renderer->menuSystem.masterVolumeSliderDragging)) {
                if (renderer->menuSystem.viewDistanceSliderDragging) {
                    renderer->updateViewDistanceFromCursor();
                } else if (renderer->menuSystem.simulationDistanceSliderDragging) {
                    renderer->updateSimulationDistanceFromCursor();
                } else {
                    renderer->updateMasterVolumeFromCursor();
                }
                return;
            }
            if (renderer->paused || renderer->chatOpen) {
                return;
            }
            if (renderer->firstMouseSample) {
                renderer->lastMouseX = x;
                renderer->lastMouseY = y;
                renderer->firstMouseSample = false;
                return;
            }
            const float deltaX = static_cast<float>(x - renderer->lastMouseX);
            const float deltaY = static_cast<float>(renderer->lastMouseY - y);
            renderer->lastMouseX = x;
            renderer->lastMouseY = y;
            renderer->camera.rotate(deltaX * 0.10F, deltaY * 0.10F);
        });

        createInstance();
        checkVk(glfwCreateWindowSurface(instance, window, nullptr, &surface),
                "glfwCreateWindowSurface");
        pickPhysicalDevice();
        createLogicalDevice();
        createAllocator();
        resources_ = VulkanResources{physicalDevice, device, allocator};
        createCommandPool();
        createDescriptorSetLayout();
        createTextureArray();
        createBiomeTextureResources();
        loadLanguage();
        createFontTexture();
        createGuiTexture();
        createPanoramaTexture();
        createPanoramaSampler();
        createEntityTextureArray();
        createUniformBuffers();
        createDescriptorPoolAndSets();
        createSceneDescriptorResources();
        createShadowResources();
        createOcclusionQueryResources();
        createSwapchainResources();
        createCommandBuffers();
        createSyncObjects();
        refreshSaveList();
        if (testScene.has_value()) initializeTestScene();
        // MC_REBEDROCK_RAIN_MODE=texture|particles|async selects the rain draw
        // path; MC_REBEDROCK_RAIN_COUNT overrides the drop target count.
        // The options menu is the canonical control (实验性内容 submenu); the
        // env vars remain dev/perf-harness overrides for smoke runs.
        if (const char* modeValue = std::getenv("MC_REBEDROCK_RAIN_MODE")) {
            const std::string_view mode{modeValue};
            if (mode == "texture") {
                rainMode_ = RainMode::Texture;
            } else if (mode == "particles") {
                rainMode_ = RainMode::Particles;
            } else {
                rainMode_ = RainMode::Async;
            }
        } else {
            rainMode_ = static_cast<RainMode>(std::clamp(options.rainMode, 0, 2));
        }
        if (const char* countValue = std::getenv("MC_REBEDROCK_RAIN_COUNT")) {
            rainCountOverride_ = std::strtoul(countValue, nullptr, 10);
        }
        // MC_REBEDROCK_PARTICLE_LEVEL=0..3 selects the 粒子效果 level the same
        // way the rain env vars override the menu; the menu is canonical.
        if (const char* levelValue = std::getenv("MC_REBEDROCK_PARTICLE_LEVEL")) {
            options.particleLevel =
                std::clamp(static_cast<int>(std::strtol(levelValue, nullptr, 10)), 0, 3);
        }
        applyParticleLevel();
        // MC_REBEDROCK_RAIN_COLLISION_CACHE=0 forces the direct per-drop
        // collision path headlessly; the menu option is the canonical control.
        rainSystem.setCollisionCache(options.rainCollisionCache);
        if (const char* cacheValue = std::getenv("MC_REBEDROCK_RAIN_COLLISION_CACHE")) {
            rainSystem.setCollisionCache(std::strcmp(cacheValue, "0") != 0);
        }
        // The smoke test always exercises the sun-shadow path so the pre-pass
        // and the terrain sampling are validated on every run.
        if (std::getenv("MC_REBEDROCK_SMOKE_TEST") != nullptr) {
            options.sunShadows = true;
        }
        shadowDisabled = !options.sunShadows || std::getenv("MC_REBEDROCK_SHADOW_DISABLE") != nullptr;
    }

    // The target rain-drop count for the selected mode: few large sheets for
    // 贴图雨, a few hundred per-particle draws for 粒子雨, and thousands in one
    // instanced draw for 异步粒子雨. MC_REBEDROCK_RAIN_COUNT overrides all.
    [[nodiscard]] std::size_t rainTargetCount() const {
        // A thunderstorm drenches the world: the rain volume scales with the
        // thunder gradient up to double the plain-rain count, and the async
        // path's capacity is what makes thousands of extra drops free to draw.
        const float thunderBoost = 1.0F + gameSession.weatherSystem().thunderGradient();
        // The user-facing rain bump (plain and thunder rain both 1.5x of the
        // pre-bump baseline) rides the 粒子效果 level: 中 (1x) yields the 1.5x
        // budget, 高 doubles it, 疯狂 triples it, 低 halves it.
        const float rainScale = 1.5F * particleLevelMultiplier(options.particleLevel);
        std::size_t base = 0U;
        if (rainCountOverride_ > 0U) {
            base = rainCountOverride_;
        } else {
            switch (rainMode_) {
                // The wider rain box (±24 blocks) needs a denser population to
                // read as rain across the whole field, so the bases rise a
                // quarter over the old ±16-box values.
                case RainMode::Texture: base = 30U; break;
                case RainMode::Particles: base = 1000U; break;
                case RainMode::Async: base = 5000U; break;
            }
        }
        return static_cast<std::size_t>(static_cast<float>(base) * rainScale * thunderBoost);
    }

    // The topmost full-collision surface in a column (vanilla's MOTION_BLOCKING
    // top position), restricted to a y window around the camera so the rain
    // search and the roof probe never walk the whole 256-block column. Returns
    // the resting position of a drop on that surface, or nothing when the window
    // holds no collision. The scan walks top-down and stops at the first hit,
    // which is the surface the rain drops land on.
    [[nodiscard]] static std::optional<glm::vec3> weatherSurface(
        const world::World& world, int blockX, int blockZ, int lowestY, int highestY) {
        const int top = std::min(highestY, world::kWorldHeight - 1);
        const int bottom = std::max(lowestY, 0);
        for (int y = top; y >= bottom; --y) {
            if (world::hasCollision(world.block(blockX, y, blockZ))) {
                return glm::vec3{static_cast<float>(blockX) + 0.5F,
                                 static_cast<float>(y + 1),
                                 static_cast<float>(blockZ) + 0.5F};
            }
        }
        return std::nullopt;
    }

    // WorldRenderer#tickRainSplashing's sound half, ported (1.16.1). While it
    // rains the client fires a short weather.rain clip every frame or two at
    // the surface the drops hit, so the storm is audible around the camera.
    // When that surface is a roof above the camera — the player under cover —
    // the muffled weather.rain.above clip (0.1 volume, 0.5 pitch) takes over:
    // vanilla's "indoor" rain. Volume follows the smoothed rain gradient, so a
    // drizzling sky stays faint and a full storm gets loud, and the thunder
    // gradient boosts it another half again so 雷雨天 rains harder than plain
    // rain (1.16.1 leaves the clip at a flat 0.2; the ramp is the adaptation
    // the gradient-volume ask calls for).
    void updateWeatherSound(world::World& world) {
        const float rainGradient = gameSession.weatherSystem().rainGradient();
        if (rainGradient <= 0.0F) {
            return;
        }
        const glm::ivec3 cameraBlock{
            static_cast<int>(std::floor(camera.position().x)),
            static_cast<int>(std::floor(camera.position().y)),
            static_cast<int>(std::floor(camera.position().z))};
        // Vanilla samples up to ten random columns in a ±10 radius and keeps the
        // last surface no more than +10 blocks above the camera — exactly where
        // a roof-above case lives — so the window hugs the camera the same way.
        std::optional<glm::vec3> surface;
        for (int sample = 0; sample < 10; ++sample) {
            weatherSoundRng_ = weatherSoundRng_ * 1664525U + 1013904223U;
            const int dx = static_cast<int>(weatherSoundRng_ % 21U) - 10;
            weatherSoundRng_ = weatherSoundRng_ * 1664525U + 1013904223U;
            const int dz = static_cast<int>(weatherSoundRng_ % 21U) - 10;
            const auto candidate = weatherSurface(world, cameraBlock.x + dx, cameraBlock.z + dz,
                                                  cameraBlock.y - 12, cameraBlock.y + 12);
            if (candidate.has_value() &&
                candidate->y <= static_cast<float>(cameraBlock.y + 11)) {
                surface = candidate;
            }
        }
        if (!surface.has_value()) {
            return;
        }
        // Vanilla gates the clip on random.nextInt(3) < field_20793++: the
        // counter starts at zero (never fires), then the gate opens 1-in-3,
        // 2-in-3 and finally every frame until a clip resets it — a clip roughly
        // every one to two frames.
        weatherSoundRng_ = weatherSoundRng_ * 1664525U + 1013904223U;
        if (static_cast<int>(weatherSoundRng_ % 3U) >= weatherSoundCadence_++) {
            return;
        }
        weatherSoundCadence_ = 0;
        // Under cover: the camera's own column has a collision above it and the
        // found surface is that roof, so the drops are landing overhead — play
        // the muffled rain-above clip at the roof.
        const bool underRoof =
            weatherSurface(world, cameraBlock.x, cameraBlock.z, cameraBlock.y + 1,
                           cameraBlock.y + 12)
                .has_value();
        const bool rainAbove = surface->y > static_cast<float>(cameraBlock.y + 1);
        const float volumeScale =
            rainGradient * (1.0F + 0.5F * gameSession.weatherSystem().thunderGradient());
        const bool underCover = rainAbove && underRoof;
        if (underCover) {
            audioSystem.playWeatherRainAbove(*surface, 0.1F * volumeScale);
        } else {
            audioSystem.playWeatherRain(*surface, 0.2F * volumeScale);
        }
        // One-time diagnostic: proves the weather-sound loop reaches the audio
        // system at the gradient-scaled volume, and which clip the roof rule
        // picked for the first clip of the storm.
        static bool weatherSoundReported = false;
        if (!weatherSoundReported) {
            weatherSoundReported = true;
            std::cout << "[weather-sound] first rain clip volume="
                      << (underCover ? 0.1F : 0.2F) * volumeScale
                      << (underCover ? " (muffled under roof)" : "") << '\n';
        }
    }

    void createRainSheetPipeline() {
        const auto vertexCode = readSpirv(shaderRoot / "rain_sheet.vert.spv");
        const auto fragmentCode = readSpirv(shaderRoot / "rain_sheet.frag.spv");
        const auto vertexModule = createShaderModule(vertexCode);
        const auto fragmentModule = createShaderModule(fragmentCode);
        auto vertexStage = vkStructure<VkPipelineShaderStageCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
        vertexStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertexStage.module = vertexModule;
        vertexStage.pName = "main";
        auto fragmentStage = vertexStage;
        fragmentStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragmentStage.module = fragmentModule;
        const std::array stages{vertexStage, fragmentStage};
        auto vertexInput = vkStructure<VkPipelineVertexInputStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
        auto inputAssembly = vkStructure<VkPipelineInputAssemblyStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        auto viewportState = vkStructure<VkPipelineViewportStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;
        auto rasterization = vkStructure<VkPipelineRasterizationStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
        rasterization.cullMode = VK_CULL_MODE_NONE;
        rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterization.lineWidth = 1.0F;
        auto multisampling = vkStructure<VkPipelineMultisampleStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
        multisampling.rasterizationSamples = renderSampleCount();
        auto depthStencil = vkStructure<VkPipelineDepthStencilStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO);
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState colorAttachment{};
        colorAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        colorAttachment.blendEnable = VK_TRUE;
        colorAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        colorAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        colorAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        colorAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        auto blending = vkStructure<VkPipelineColorBlendStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
        blending.attachmentCount = 1;
        blending.pAttachments = &colorAttachment;
        const std::array dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        auto dynamic = vkStructure<VkPipelineDynamicStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
        dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
        dynamic.pDynamicStates = dynamicStates.data();
        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        push.offset = 0;
        push.size = sizeof(RainSheetPush);
        auto layoutInfo =
            vkStructure<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &descriptorSetLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &push;
        checkVk(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &rainSheetPipelineLayout),
                "vkCreatePipelineLayout(rain sheet)");
        auto pipelineInfo = vkStructure<VkGraphicsPipelineCreateInfo>(
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
        pipelineInfo.stageCount = static_cast<std::uint32_t>(stages.size());
        pipelineInfo.pStages = stages.data();
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterization;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &blending;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = rainSheetPipelineLayout;
        pipelineInfo.renderPass = renderPass;
        checkVk(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                          &rainSheetPipeline),
                "vkCreateGraphicsPipelines(rain sheet)");
        vkDestroyShaderModule(device, vertexModule, nullptr);
        vkDestroyShaderModule(device, fragmentModule, nullptr);
    }

    void initializeTestScene() {
        if (testScene->occlusionScene) {
            // A controlled occlusion scene: a flat stone platform with its
            // surface at y=47. Section 1 (y 16-31) is hollowed out into a buried
            // cave; section 2 (y 32-47) keeps a 2x2 surface opening. The camera
            // sits at y=51, just above the surface, looking +Z along the
            // platform, so the query results are predictable:
            //   section 2 (surface) must stay visible (>0);
            //   section 1 (buried cave) must be culled (0).
            world::Chunk chunk;
            for (int y = 0; y < 48; ++y) {
                for (int z = 0; z < world::kChunkDepth; ++z) {
                    for (int x = 0; x < world::kChunkWidth; ++x) {
                        chunk.setBlock(x, y, z, world::Block::Stone);
                    }
                }
            }
            // A buried 4x4x4 cave inside section 1 (its walls live in the same
            // section, so the mesh is non-empty and the section stays testable).
            for (int y = 20; y < 24; ++y) {
                for (int z = 6; z < 10; ++z) {
                    for (int x = 6; x < 10; ++x) {
                        chunk.setBlock(x, y, z, world::Block::Air);
                    }
                }
            }
            for (int y = 44; y < 48; ++y) {
                for (int z = 4; z < 6; ++z) {
                    for (int x = 4; x < 6; ++x) {
                        chunk.setBlock(x, y, z, world::Block::Air);
                    }
                }
            }
            interactionWorld.setChunk({0, 0}, std::move(chunk));
            world::WorldLightEngine lighting;
            const std::array positions{world::ChunkPosition{0, 0}};
            lighting.initializeChunks(interactionWorld, positions);
            for (const int sectionY : {1, 2}) {
                world::SectionMeshUpdate update;
                update.position = {0, sectionY, 0};
                update.mesh =
                    world::ChunkMesher::buildSection(interactionWorld, {0, 0}, sectionY);
                update.revision = static_cast<std::uint64_t>(sectionY);
                pendingSectionOrder.push_back(update.position);
                latestSectionRevisions.insert_or_assign(update.position, update.revision);
                pendingSectionUpdates.insert_or_assign(update.position, std::move(update));
            }
            loadedCpuChunkCount = 1U;
            // The camera follows the gameSession.player()'s eye, so pin the gameSession.player() just above
            // the platform surface (y=47), looking along +Z at the scene.
            gameSession.player().setPosition({8.0F, 49.4F, -8.0F});
            camera.setPosition(gameSession.player().eyePosition());
            worldReady = true;
            paused = true;
            menuSystem.pageStack.reset(ui::PageId::Game);
            gameSession.gameTimeSeconds() = 0.0;
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            std::cout << "Test scene: occlusion (platform + buried cave)\n";
            return;
        }
        constexpr glm::ivec3 blockPosition{8, 64, 8};
        constexpr std::array orientations{
            world::BlockOrientation::North, world::BlockOrientation::East,
            world::BlockOrientation::South, world::BlockOrientation::West,
            world::BlockOrientation::Up, world::BlockOrientation::Down};
        world::Chunk chunk;
        chunk.setBlock(blockPosition.x, blockPosition.y, blockPosition.z,
                       testScene->block);
        chunk.setOrientation(blockPosition.x, blockPosition.y, blockPosition.z,
                             orientations[static_cast<std::size_t>(testScene->stage) %
                                          orientations.size()]);
        interactionWorld.setChunk({0, 0}, std::move(chunk));
        world::WorldLightEngine lighting;
        const std::array positions{world::ChunkPosition{0, 0}};
        lighting.initializeChunks(interactionWorld, positions);
        world::SectionMeshUpdate update;
        update.position = {0, blockPosition.y / world::kSectionSize, 0};
        update.mesh = world::ChunkMesher::buildSection(
            interactionWorld, {0, 0}, update.position.sectionY);
        update.revision = 1U;
        pendingSectionOrder.push_back(update.position);
        latestSectionRevisions.insert_or_assign(update.position, update.revision);
        pendingSectionUpdates.insert_or_assign(update.position, std::move(update));
        if (testScene->block == world::Block::Chest) {
            static_cast<void>(gameSession.chestSystem().place(
                {blockPosition.x, blockPosition.y, blockPosition.z}));
        }
        loadedCpuChunkCount = 1U;
        worldReady = true;
        paused = true;
        menuSystem.pageStack.reset(ui::PageId::Game);
        gameSession.gameTimeSeconds() = static_cast<double>(testScene->stage) *
            (world::DayNightCycle::kTicksPerDay / 10.0) /
            world::DayNightCycle::kTicksPerSecond;
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        std::cout << "Test scene: "
                  << world::blockDefinition(testScene->block).identifier.toString()
                  << " stage " << testScene->stage << '\n';
    }

    void run() {
        const bool smokeTest = std::getenv("MC_REBEDROCK_SMOKE_TEST") != nullptr;
        // Stress mode: MC_REBEDROCK_STRESS_FRAMES overrides the smoke test's
        // 704-gameplay-frame cap and walks the gameSession.player() forward, churning chunk
        // streaming and the occlusion queries, so long-run memory/GPU faults
        // surface in a scripted run instead of after minutes at the keyboard.
        const char* stressFramesValue = std::getenv("MC_REBEDROCK_STRESS_FRAMES");
        stressFrames =
            stressFramesValue != nullptr ? std::strtoull(stressFramesValue, nullptr, 10) : 0U;
        // One extra slot past the last scripted step (706) holds the return-to-
        // title gate.
        const std::size_t smokeFrameLimit = stressFrames > 0U ? stressFrames : 710U;
        std::size_t renderedFrames = 0;
        std::size_t smokeGameplayFrames = 0;
        std::size_t smokeReturnFrame = 0;
        bool smokeWorldStarted = false;
        bool smokeReturnedToTitle = false;
        // The apple count before the smoke test holds right-click to eat, used
        // to verify the meal actually consumed one.
        std::uint8_t smokeAppleCount = 0U;
        auto previousFrameTime = std::chrono::steady_clock::now();
        float physicsAccumulator = 0.0F;
        while (glfwWindowShouldClose(window) == GLFW_FALSE) {
            glfwPollEvents();
            const auto currentFrameTime = std::chrono::steady_clock::now();
            const float deltaSeconds = std::min(
                std::chrono::duration<float>(currentFrameTime - previousFrameTime).count(), 0.1F);
            previousFrameTime = currentFrameTime;
            // Keep a short EMA of the frame time and adapt the streaming upload
            // budget to it: when the GPU is being ground down by a streaming
            // batch of dense sections, upload fewer per frame so the load damps;
            // when the frame time recovers, upload faster and let the region
            // fill in. The hysteresis in StreamingBudget.hpp keeps the budget
            // from oscillating around a single threshold.
            smoothedFrameSeconds_ = smoothedFrameSeconds_ * 0.7F + deltaSeconds * 0.3F;
            streamingUploadBudget_ = mc::render::streamingUploadBudgetForFrameMs(
                smoothedFrameSeconds_ * 1000.0F, streamingUploadBudget_);
            uiTimeSeconds += static_cast<double>(deltaSeconds);
            fpsSampleSeconds += deltaSeconds;
            ++fpsSampleFrames;
            if (fpsSampleSeconds >= 0.5F) {
                displayedFps = static_cast<int>(
                    std::lround(static_cast<float>(fpsSampleFrames) / fpsSampleSeconds));
                fpsSampleSeconds = 0.0F;
                fpsSampleFrames = 0U;
            }
            processInput();
            if (inventoryOpen) {
                const ui::HudLayout animationLayout{
                    static_cast<float>(std::max(swapchainExtent.width, 1U)),
                    static_cast<float>(std::max(swapchainExtent.height, 1U)), menuSystem.guiScaleSetting};
                const auto cursor = currentFramebufferCursor();
                const auto preview =
                    animationLayout.playerPreview(gameSession.gameMode() == gameplay::GameMode::Creative);
                playerModelAnimator.setCursorLook(
                    (cursor.x - preview.lookOrigin.x) / (40.0F * animationLayout.scale()),
                    (cursor.y - preview.lookOrigin.y) / (40.0F * animationLayout.scale()));
            }
            const glm::vec2 horizontalVelocity{gameSession.player().velocity().x, gameSession.player().velocity().z};
            const bool playerWalking = glm::length(horizontalVelocity) > 0.02F;
            playerModelAnimator.update(deltaSeconds, playerWalking);
            // Head leads, body follows: the head turns freely up to a limit, and
            // only when it reaches that limit does it drag the body around. While
            // walking, the body eases toward the look direction so movement stays
            // forward-facing (vanilla behaviour).
            const glm::vec3 lookDir = camera.direction();
            const float lookYaw = std::atan2(lookDir.x, lookDir.z);
            const auto wrapAngle = [](float angle) {
                constexpr float pi = 3.14159265358979F;
                angle = std::fmod(angle + pi, 2.0F * pi);
                if (angle < 0.0F) angle += 2.0F * pi;
                return angle - pi;
            };
            if (!worldBodyYawInitialized) {
                worldBodyYaw = lookYaw;
                worldBodyYawInitialized = true;
            }
            constexpr float kMaxHeadYaw = 0.9599F; // 55 degrees, the head yaw range
            const float lagDiff = wrapAngle(lookYaw - worldBodyYaw);
            if (lagDiff > kMaxHeadYaw) {
                worldBodyYaw = lookYaw - kMaxHeadYaw;
            } else if (lagDiff < -kMaxHeadYaw) {
                worldBodyYaw = lookYaw + kMaxHeadYaw;
            }
            if (playerWalking) {
                worldBodyYaw +=
                    wrapAngle(lookYaw - worldBodyYaw) * std::min(1.0F, deltaSeconds * 8.0F);
            }
            const float headRelative = wrapAngle(lookYaw - worldBodyYaw);
            worldPlayerAnimator.setCursorLook(headRelative / kMaxHeadYaw, -lookDir.y);
            worldPlayerAnimator.update(deltaSeconds, playerWalking, gameSession.player().sneaking());
            if (!paused && worldReady) {
                if (gameSession.gameRules().get<bool>(gameplay::GameRuleId::DoDaylightCycle)) {
                    gameSession.gameTimeSeconds() += static_cast<double>(deltaSeconds);
                }
                heldItemAnimation.update(deltaSeconds);
                updateVignetteDarkness(deltaSeconds);
                particleSystem.update(deltaSeconds, interactionWorld);
                // Rain: CPU-simulated drops driven by the weather system's
                // smoothed gradient. All three render modes consume the same
                // drops; only the draw strategy differs. The drops collide with
                // the world (splash on water surfaces and solid ground) and drift
                // downwind — a per-drop world lookup, pure CPU and identical for
                // every mode.
                const float thunderGradient = gameSession.weatherSystem().thunderGradient();
                // The wind holds a heading for 10-20 s, then veers to a new
                // one over a couple of seconds — an occasional, slow shift
                // instead of the old constant rotation that kept the whole rain
                // field spinning and never let it settle on a slant.
                constexpr float kTwoPi = 6.28318530718F;
                if (windShiftTimer_ <= 0.0F) {
                    weatherSoundRng_ = weatherSoundRng_ * 1664525U + 1013904223U;
                    windTargetAngle_ =
                        static_cast<float>(weatherSoundRng_ >> 8) / 16777216.0F * kTwoPi;
                    weatherSoundRng_ = weatherSoundRng_ * 1664525U + 1013904223U;
                    windShiftTimer_ = 10.0F +
                        static_cast<float>(weatherSoundRng_ >> 8) / 16777216.0F * 10.0F;
                }
                windShiftTimer_ -= deltaSeconds;
                const float windTurn = wrapAngle(windTargetAngle_ - rainWindAngle_);
                rainWindAngle_ += windTurn * std::min(1.0F, deltaSeconds * 0.8F);
                const float windSpeed = 1.5F + thunderGradient * 4.5F;
                const glm::vec2 wind{std::cos(rainWindAngle_) * windSpeed,
                                     std::sin(rainWindAngle_) * windSpeed};
                rainSystem.update(deltaSeconds, camera.position(),
                                  gameSession.weatherSystem().rainGradient(), rainTargetCount(),
                                  interactionWorld, wind);
                for (const auto& splash : rainSystem.splashes()) {
                    particleSystem.spawnRainSplash(splash.position, splash.direction);
                }
                // tickRainSplashing also drives the rain *sound*: a weather.rain
                // clip at the surface the drops hit, muffled when the player is
                // under a roof, all scaled by the smoothed rain gradient.
                updateWeatherSound(interactionWorld);
                static bool stormReported = false;
                if (!stormReported && gameSession.weatherSystem().isThundering() &&
                    rainSystem.drops().size() > 5000U) {
                    stormReported = true;
                    std::cout << "[thunder] storm drops=" << rainSystem.drops().size()
                              << " wind=" << windSpeed << "\n";
                }
                static bool splashReported = false;
                if (!splashReported && rainSystem.splashes().size() >= 20U) {
                    splashReported = true;
                    std::cout << "[rain] splash=" << rainSystem.splashes().size()
                              << " particles=" << particleSystem.particles().size() << "\n";
                }
                // One-time diagnostic: the per-frame world-lookup count the
                // column surface cache reduced collision to (the old code did
                // one world lookup per drop per frame — 22,500 at the crazy
                // storm). Sampled once the population is full AND the cache is
                // warm (a warm frame makes far fewer world lookups than it has
                // drops), so the number is the steady state, not the first
                // frames' probe warmup.
                static bool collisionReported = false;
                if (!collisionReported &&
                    rainSystem.drops().size() >= rainTargetCount() * 9U / 10U &&
                    rainSystem.lastUpdateLookups() < rainSystem.drops().size() / 4U) {
                    collisionReported = true;
                    std::cout << "[rain] collision lookups/frame="
                              << rainSystem.lastUpdateLookups() << " drops="
                              << rainSystem.drops().size() << "\n";
                }
                rainTime_ += deltaSeconds;
                physicsAccumulator = std::min(physicsAccumulator + deltaSeconds, 0.25F);
                bool fluidUpdatePhaseConsumed = false;
                while (physicsAccumulator >= gameplay::PlayerController::kTickSeconds) {
                    gameSession.tick(interactionWorld, *this);
                    physicsAccumulator -= gameplay::PlayerController::kTickSeconds;
                }
            } else {
                physicsAccumulator = 0.0F;
            }
            const float physicsAlpha =
                physicsAccumulator / gameplay::PlayerController::kTickSeconds;
            renderInterpolationAlpha = std::clamp(physicsAlpha, 0.0F, 1.0F);
            const glm::vec3 renderedFeetPosition =
                gameSession.physicsPreviousPosition() +
                (gameSession.physicsCurrentPosition() - gameSession.physicsPreviousPosition()) * physicsAlpha;
            // Capture the HUD snapshot from the settled session state.
            uiFrameData_.health = gameSession.vitals().health();
            uiFrameData_.foodLevel = gameSession.vitals().foodLevel();
            uiFrameData_.airTicks = gameSession.vitals().airTicks();
            uiFrameData_.ticksSinceDamage = gameSession.vitals().ticksSinceDamage();
            uiFrameData_.gameMode = gameSession.gameMode();
            uiFrameData_.eating = gameSession.eating();
            uiFrameData_.selectedStack = gameSession.inventory().selectedStack();
            uiFrameData_.selectedHotbarSlot = gameSession.inventory().selectedHotbarSlot();
                        camera.setPosition(renderedFeetPosition + glm::vec3{0.0F, gameSession.player().eyeHeight(), 0.0F});
            // The occlusion test scene pins the camera above the platform's
            // surface (y=47), looking down across it so both the surface and
            // the buried cave section fall inside the frustum.
            if (testScene.has_value() && testScene->occlusionScene) {
                camera.setPosition({8.0F, 60.0F, -8.0F});
            }
            // Stress mode turns the camera each frame, churning the occlusion
            // queries as sections stream in and out of the frustum.
            if (stressFrames > 0U) {
                // Yaw spin + a slow pitch-down so the view sweeps toward the
                // ground like a flying player glancing around, and an expanding
                // outward spiral that keeps the streaming window loading fresh
                // chunks the whole run.
                camera.rotate(2.0F, -0.05F);
                const std::size_t stressClock =
                    std::getenv("MC_REBEDROCK_LOAD_SAVE") != nullptr ? renderedFrames
                                                                     : smokeGameplayFrames;
                const float flightAngle = static_cast<float>(stressClock) * 0.06F;
                const float radius = 40.0F + static_cast<float>(stressClock) * 0.4F;
                const glm::vec3 stressPos{
                    std::cos(flightAngle) * radius,
                    120.0F + std::sin(static_cast<float>(stressClock) * 0.012F) * 80.0F,
                    std::sin(flightAngle) * radius,
                };
                camera.setPosition(stressPos);
                // DR repro: fly the player along the spiral so chunk streaming
                // follows the movement like real play (the real save is loaded
                // in creative, so no fall damage).
                gameSession.player().setPosition(
                    stressPos - glm::vec3{0.0F, gameSession.player().eyeHeight(), 0.0F});
            }
            // GameRenderer#getFov: the base FOV times the gameSession.player()'s movement
            // multiplier, interpolated across the physics tick the same way the
            // eye position is. Sprinting widens it to 1.15x, creative flight to
            // 1.1x, and both ease in over a few ticks.
            const float fovMultiplier =
                gameSession.player().previousFieldOfViewMultiplier() +
                (gameSession.player().fieldOfViewMultiplier() - gameSession.player().previousFieldOfViewMultiplier()) *
                    physicsAlpha;
            camera.setFieldOfViewDegrees(baseFieldOfViewDegrees * fovMultiplier);
            audioSystem.updateListener(camera.position(), camera.direction(), {0.0F, 1.0F, 0.0F});
            audioSystem.update();
            if (worldSessionActive)
                processChunkStreaming();
            updateBlockInteraction();
            updateItemDrop();
            drawFrame();
            ++renderedFrames;
            // The occlusion test scene renders a few frames so the two-frame
            // query latency resolves, then exits and dumps the diagnostics.
            if (testScene.has_value() && testScene->occlusionScene && renderedFrames >= 30U) {
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }
            if (options.frameRateLimit > 0) {
                const auto targetFrameDuration = std::chrono::duration<double>(
                    1.0 / static_cast<double>(options.frameRateLimit));
                const auto deadline = currentFrameTime +
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        targetFrameDuration);
                // sleep_until wakes 1-2ms late on macOS (the scheduler's timer
                // resolution), which alone would pace a 120fps cap at ~100fps —
                // every frame overshoots by the wake latency. Sleep most of the
                // way there, then busy-wait the final two milliseconds so the
                // pacing lands on the target instead of the OS wake granularity.
                const auto spinStart = deadline - std::chrono::milliseconds(2);
                if (std::chrono::steady_clock::now() < spinStart) {
                    std::this_thread::sleep_until(spinStart);
                }
                while (std::chrono::steady_clock::now() < deadline) {
                    // Busy-wait the tail for precise pacing.
                }
            }
            // DR repro hook: MC_REBEDROCK_LOAD_SAVE auto-loads the first real save
            // (bypassing the menu) and then stress-files via the stress camera.
            if (std::getenv("MC_REBEDROCK_LOAD_SAVE") != nullptr && !loadSaveStarted) {
                loadSaveStarted = true;
                const auto summaries = saveRepository.list();
                if (summaries.empty()) {
                    throw std::runtime_error("MC_REBEDROCK_LOAD_SAVE: no saves found");
                }
                startWorld(saveRepository.load(summaries.front().identifier));
            }
            if (smokeTest && !smokeWorldStarted && renderedFrames == 2U) {
                if (menuSystem.pageStack.current() != ui::PageId::Title) {
                    throw std::runtime_error("Smoke test did not start at title page");
                }
                menuSystem.optionsOpen = true;
                menuSystem.pageStack.push(ui::PageId::Options);
            } else if (smokeTest && !smokeWorldStarted && renderedFrames == 3U) {
                menuSystem.pageStack.push(ui::PageId::VideoSettings);
            } else if (smokeTest && !smokeWorldStarted && renderedFrames == 4U) {
                if (menuSystem.pageStack.current() != ui::PageId::VideoSettings) {
                    throw std::runtime_error("Smoke test did not open video settings");
                }
                menuSystem.pageStack.pop();
                // The 实验性内容 sub-page must open as a menu page (not fall
                // through to the terrain-loading screen) with its five options.
                menuSystem.pageStack.push(ui::PageId::Experimental);
                if (menuSystem.pageStack.current() != ui::PageId::Experimental ||
                    menuButtonCount() != 5U) {
                    throw std::runtime_error("Smoke test experimental content page failed");
                }
                menuSystem.pageStack.pop();
                menuSystem.optionsOpen = false;
                menuSystem.pageStack.pop();
                menuSystem.pageStack.push(ui::PageId::WorldList);
            } else if (smokeTest && !smokeWorldStarted && renderedFrames == 6U) {
                menuSystem.pageStack.push(ui::PageId::CreateWorld);
            } else if (smokeTest && !smokeWorldStarted && renderedFrames == 8U) {
                persistence::SaveGame smokeWorld;
                smokeWorld.summary.displayName = "Smoke Test";
                smokeWorld.summary.seed = 0x5EEDULL;
                gameplay::Inventory smokeInventory;
                smokeWorld.inventory = smokeInventory.slots();
                smokeWorld.selectedHotbarSlot = smokeInventory.selectedHotbarSlot();
                startWorld(std::move(smokeWorld));
                smokeWorldStarted = true;
            }
            if (smokeTest && worldReady && !smokeReturnedToTitle) {
                ++smokeGameplayFrames;
            }
            if (smokeTest && smokeGameplayFrames == 16U) {
                setInventoryOpen(true);
            } else if (smokeTest && smokeGameplayFrames == 20U) {
                gameSession.inventory().clickCreativeItem({world::Block::Air, 1U, &gameplay::items::Diamond},
                                            gameplay::InventoryMouseButton::Left, false);
            } else if (smokeTest && smokeGameplayFrames == 24U) {
                gameSession.inventory().clickSlot(0U, gameplay::InventoryMouseButton::Left, false);
            } else if (smokeTest && smokeGameplayFrames == 28U) {
                scrollCreative(1);
            } else if (smokeTest && smokeGameplayFrames == 32U) {
                setInventoryOpen(false);
            } else if (smokeTest && smokeGameplayFrames == 34U) {
                setPaused(true);
            } else if (smokeTest && smokeGameplayFrames == 36U) {
                menuSystem.optionsOpen = true;
                menuSystem.pageStack.push(ui::PageId::Options);
            } else if (smokeTest && smokeGameplayFrames == 38U) {
                menuSystem.optionsOpen = false;
                menuSystem.pageStack.pop();
                setPaused(false);
            } else if (smokeTest && smokeGameplayFrames == 40U) {
                gameSession.itemEntities().spawn(gameSession.player().position() + glm::vec3{1.8F, 1.0F, 0.0F},
                                   {world::Block::DiamondOre, 3}, {0.0F, 0.12F, 0.0F});
            } else if (smokeTest && smokeGameplayFrames == 44U) {
                debugOverlayOpen = true;
            } else if (smokeTest && smokeGameplayFrames == 48U) {
                debugOverlayOpen = false;
            } else if (smokeTest && smokeGameplayFrames == 50U) {
                setChatOpen(true);
                chatInputText = "/gamemode survival";
            } else if (smokeTest && smokeGameplayFrames == 54U) {
                submitChatInput();
            } else if (smokeTest && smokeGameplayFrames == 56U) {
                if (gameSession.gameMode() != gameplay::GameMode::Survival) {
                    throw std::runtime_error("Smoke test failed to enter survival mode");
                }
                if (gameSession.inventory().slot(0).item != &gameplay::items::Diamond) {
                    throw std::runtime_error(
                        "Smoke test lost the shared inventory during mode switch");
                }
            } else if (smokeTest && smokeGameplayFrames == 58U) {
                setInventoryOpen(true);
            } else if (smokeTest && smokeGameplayFrames == 60U) {
                // Deterministically exercise the instanced particle path: a
                // block-break burst next to the player produces hundreds of
                // particles in a single vkCmdDraw.
                const glm::vec3 spawn = gameSession.player().position();
                particleSystem.spawnBlockBreak(
                    {static_cast<int>(std::floor(spawn.x)),
                     static_cast<int>(std::floor(spawn.y)) - 2,
                     static_cast<int>(std::floor(spawn.z))},
                    world::Block::Dirt);
            } else if (smokeTest && smokeGameplayFrames == 62U) {
                setInventoryOpen(false);
            } else if (smokeTest && smokeGameplayFrames == 66U) {
                setChatOpen(true);
                chatInputText = "/gamemode creative";
            } else if (smokeTest && smokeGameplayFrames == 70U) {
                submitChatInput();
            } else if (smokeTest && smokeGameplayFrames == 72U) {
                if (gameSession.gameMode() != gameplay::GameMode::Creative) {
                    throw std::runtime_error("Smoke test failed to return to creative mode");
                }
                if (gameSession.inventory().slot(0).item != &gameplay::items::Diamond) {
                    throw std::runtime_error("Smoke test lost the shared inventory state");
                }
            } else if (smokeTest && smokeGameplayFrames == 74U) {
                setChatOpen(true);
                chatInputText = "/give 0 1";
            } else if (smokeTest && smokeGameplayFrames == 76U) {
                submitChatInput();
            } else if (smokeTest && smokeGameplayFrames == 78U) {
                // Catalog index 0 is the first registered building block (grass).
                const bool foundGrass = std::ranges::any_of(
                    gameSession.inventory().slots(), [](const gameplay::ItemStack& stack) {
                        return stack.block == world::Block::Grass && stack.count >= 1U;
                    });
                if (!foundGrass) {
                    throw std::runtime_error("Smoke test failed to /give by catalog index");
                }
            } else if (smokeTest && smokeGameplayFrames == 80U) {
                setChatOpen(true);
                chatInputText = "/give minecraft:acacia_planks 3";
            } else if (smokeTest && smokeGameplayFrames == 82U) {
                submitChatInput();
            } else if (smokeTest && smokeGameplayFrames == 84U) {
                const bool foundAcacia = std::ranges::any_of(
                    gameSession.inventory().slots(), [](const gameplay::ItemStack& stack) {
                        return stack.block == world::Block::AcaciaPlanks && stack.count >= 3U;
                    });
                if (!foundAcacia) {
                    throw std::runtime_error("Smoke test failed to /give by identifier");
                }
            } else if (smokeTest && smokeGameplayFrames == 86U) {
                setChatOpen(true);
                chatInputText = "/time set midnight";
            } else if (smokeTest && smokeGameplayFrames == 88U) {
                submitChatInput();
            } else if (smokeTest && smokeGameplayFrames == 90U) {
                const auto tick = world::DayNightCycle::worldTick(gameSession.gameTimeSeconds());
                if (std::abs(tick - 18000.0) > 4.0) {
                    throw std::runtime_error("Smoke test failed to set world time");
                }
            } else if (smokeTest && smokeGameplayFrames == 92U) {
                setInventoryOpen(true);
            } else if (smokeTest && smokeGameplayFrames == 94U) {
                // Put a full stack of apples into the last hotbar slot, then
                // close the screen and select it.
                gameSession.inventory().clickCreativeItem(
                    {world::Block::Air, 1U, &gameplay::items::Apple},
                    gameplay::InventoryMouseButton::Left, false);
                gameSession.inventory().clickSlot(8U, gameplay::InventoryMouseButton::Left, false);
                setInventoryOpen(false);
                gameSession.inventory().selectHotbar(8U);
            } else if (smokeTest && smokeGameplayFrames == 96U) {
                smokeAppleCount = gameSession.inventory().selectedStack().count;
                if (smokeAppleCount == 0U) {
                    throw std::runtime_error("Smoke test apple stack missing");
                }
                // In creative the meal must not spend the food (Java 1.16.1).
                useButtonHeld = true;
            } else if (smokeTest && smokeGameplayFrames == 400U) {
                useButtonHeld = false;
                if (gameSession.inventory().selectedStack().count != smokeAppleCount) {
                    throw std::runtime_error("Smoke test creative eating consumed food");
                }
            } else if (smokeTest && smokeGameplayFrames == 402U) {
                setChatOpen(true);
                chatInputText = "/gamemode survival";
            } else if (smokeTest && smokeGameplayFrames == 404U) {
                submitChatInput();
            } else if (smokeTest && smokeGameplayFrames == 406U) {
                if (gameSession.gameMode() != gameplay::GameMode::Survival) {
                    throw std::runtime_error("Smoke test failed to return to survival mode");
                }
                // The apple survived creative gameSession.eating(); survival should spend it.
                gameSession.inventory().selectHotbar(8U);
                smokeAppleCount = gameSession.inventory().selectedStack().count;
                if (smokeAppleCount == 0U) {
                    throw std::runtime_error("Smoke test apple stack missing in survival");
                }
                useButtonHeld = true;
            } else if (smokeTest && smokeGameplayFrames == 410U) {
                // Snap the weather to full rain instantly (test helper, not
                // chat) so the smoke exercises the rain path at full intensity
                // regardless of frame rate; the three render modes compare
                // identical drop counts.
                gameSession.weatherSystem().forceRainGradient(1.0F);
            } else if (smokeTest && smokeGameplayFrames == 420U) {
                // Escalate to a full storm so the smoke also exercises the
                // thunder-boosted rain volume and cross-wind.
                gameSession.weatherSystem().forceThunderGradient(1.0F);
            } else if (smokeTest && smokeGameplayFrames == 700U) {
                useButtonHeld = false;
                if (gameSession.inventory().selectedStack().count >= smokeAppleCount) {
                    throw std::runtime_error("Smoke test survival eating did not consume an apple");
                }
            } else if (smokeTest && smokeGameplayFrames == 702U) {
                setChatOpen(true);
                chatInputText = "/tp 8 200 8";
            } else if (smokeTest && smokeGameplayFrames == 704U) {
                submitChatInput();
            } else if (smokeTest && smokeGameplayFrames == 706U) {
                if (gameSession.player().position().y < 150.0F) {
                    throw std::runtime_error("Smoke test /tp did not teleport the player");
                }
            }
            if (smokeTest && !smokeReturnedToTitle && smokeGameplayFrames >= smokeFrameLimit &&
                completedStreamBatchCount >= 2U && completedBlockEditCount >= 1U &&
                pendingSectionUpdates.empty()) {
                smokeReturnedToTitle = true;
                smokeReturnFrame = renderedFrames;
                returnToTitle(false);
            }
            if (smokeTest && smokeReturnedToTitle && renderedFrames >= smokeReturnFrame + 4U) {
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }
            // DR repro: the LOAD_SAVE run ends after stressFrames rendered frames.
            if (std::getenv("MC_REBEDROCK_LOAD_SAVE") != nullptr && stressFrames > 0U &&
                renderedFrames >= smokeFrameLimit) {
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }
        }
        checkVk(vkDeviceWaitIdle(device), "vkDeviceWaitIdle");
        std::cout << "Rendered 3D frames: " << renderedFrames << '\n';
        std::cout << "Streaming upload: "
                  << static_cast<double>(totalUploadedBytes) / (1024.0 * 1024.0)
                  << " MiB total | peak pending sections: "
                  << std::max(peakPendingSectionCount, lastSessionPeakPendingSectionCount) << '\n';
    }

    [[nodiscard]] gameplay::Inventory& activeInventory() { return gameSession.inventory(); }

    [[nodiscard]] const gameplay::Inventory& activeInventory() const {
        return gameSession.inventory();
    }

    void refreshSaveList() {
        menuSystem.saveSummaries = saveRepository.list();
        if (menuSystem.saveSummaries.empty())
            menuSystem.selectedWorldIndex = 0U;
        else
            menuSystem.selectedWorldIndex = std::min(menuSystem.selectedWorldIndex, menuSystem.saveSummaries.size() - 1U);
        const std::size_t visibleRows = saveListVisibleRowCount();
        const std::size_t maximumFirst =
            menuSystem.saveSummaries.size() > visibleRows ? menuSystem.saveSummaries.size() - visibleRows : 0U;
        menuSystem.worldListFirstIndex = std::min(menuSystem.worldListFirstIndex, maximumFirst);
    }

    void scrollWorldList(int rows) {
        const std::size_t visibleRows = saveListVisibleRowCount();
        const std::size_t maximumFirst =
            menuSystem.saveSummaries.size() > visibleRows ? menuSystem.saveSummaries.size() - visibleRows : 0U;
        const auto requested = static_cast<long long>(menuSystem.worldListFirstIndex) + rows;
        menuSystem.worldListFirstIndex = static_cast<std::size_t>(
            std::clamp<long long>(requested, 0LL, static_cast<long long>(maximumFirst)));
    }

    void scrollLanguageList(int rows) {
        const std::size_t visibleRows = languageVisibleRowCount();
        const std::size_t maximumFirst =
            menuSystem.languageCodes.size() > visibleRows ? menuSystem.languageCodes.size() - visibleRows : 0U;
        const auto requested = static_cast<long long>(menuSystem.languageListFirstIndex) + rows;
        menuSystem.languageListFirstIndex = static_cast<std::size_t>(
            std::clamp<long long>(requested, 0LL, static_cast<long long>(maximumFirst)));
    }

    void applyRename() {
        if (menuSystem.editWorldIdentifier.empty()) {
            menuSystem.pageStack.pop();
            return;
        }
        try {
            saveRepository.rename(menuSystem.editWorldIdentifier, menuSystem.editWorldName);
            refreshSaveList();
        } catch (const std::exception& exception) {
            menuSystem.saveStatus = "Rename failed: " + std::string{exception.what()};
        }
        menuSystem.pageStack.pop(); // back to the world list
    }

    void deleteSelectedWorld() {
        const std::string identifier = menuSystem.editWorldIdentifier;
        try {
            saveRepository.remove(identifier);
            refreshSaveList();
        } catch (const std::exception& exception) {
            menuSystem.saveStatus = "Delete failed: " + std::string{exception.what()};
        }
        // Leave both the confirmation and the edit page, back to the list.
        menuSystem.pageStack.pop();
        menuSystem.pageStack.pop();
    }

    void rememberWorldEdit(world::PersistentBlockEdit edit) {
        if (!currentSave.has_value())
            return;
        const PersistentEditPosition position{edit.x, edit.y, edit.z};
        const auto found = savedEditIndices.find(position);
        if (found == savedEditIndices.end()) {
            savedEditIndices.emplace(position, currentSave->edits.size());
            currentSave->edits.push_back(edit);
        } else {
            currentSave->edits[found->second] = edit;
        }
    }

    void submitWorldEdit(int x, int y, int z, world::Block block,
                         std::uint8_t fluidLevel = 0U,
                         std::optional<world::BlockOrientation> orientation = std::nullopt) override {
        const auto resolvedOrientation = orientation.value_or(world::defaultOrientation(block));
        rememberWorldEdit({x, y, z, block, fluidLevel, resolvedOrientation});
        chunkStreamer.setBlock(x, y, z, block, fluidLevel, resolvedOrientation);
    }

    // Rebuild the sections touched by a gameplay edit directly on the render
    // thread using the already-updated interactionWorld, so a placed or broken
    // block is visible the same frame instead of waiting for the background
    // worker round-trip. The worker still performs the authoritative rebuild;
    // its strictly higher revision replaces this transient preview through the
    // normal revision guard in queueStreamBatch.
    //
    // This mirrors ChunkStreamer::applyBlockEdits: edit batches from the worker
    // only carry meshes, never light, so interactionWorld's stored light would
    // otherwise stay frozen at the last generated snapshot. Running the same
    // incremental light propagation here keeps the preview correctly lit (e.g.
    // torches light up immediately instead of flashing dark) and self-consistent
    // across successive edits. updateBlock is a bounded incremental BFS, not the
    // full per-chunk propagation that previously stalled the render thread.
    void previewBlockEdit(int worldX, int y, int worldZ) override {
        interactionLightEngine.updateBlock(interactionWorld, worldX, y, worldZ);

        std::vector<world::SectionPosition> sections;
        const auto mark = [&](world::SectionPosition position) {
            if (position.sectionY < 0 || position.sectionY >= world::kSectionCount) return;
            if (!interactionWorld.hasChunk({position.chunkX, position.chunkZ})) return;
            if (std::ranges::find(sections, position) == sections.end()) {
                sections.push_back(position);
            }
        };

        // Geometry and vertex AO sample one voxel beyond a section boundary, so
        // the edited voxel can dirty up to its 26 neighbours' sections.
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int sampleY = y + dy;
                    if (sampleY < 0 || sampleY >= world::kWorldHeight) continue;
                    const auto chunk = world::chunkPositionFromWorld(
                        static_cast<float>(worldX + dx), static_cast<float>(worldZ + dz));
                    mark({chunk.x, sampleY / world::kSectionSize, chunk.z});
                }
            }
        }
        // Light changes can reach well beyond the edited voxel (a torch spreads
        // up to 14 blocks). Remesh every section whose light actually changed,
        // skipping empty ones that hold no vertices to relight.
        for (const auto position : interactionLightEngine.takeDirtySections()) {
            const world::Chunk* chunk =
                interactionWorld.chunk({position.chunkX, position.chunkZ});
            if (chunk != nullptr && !chunk->section(position.sectionY).empty()) {
                mark({position.chunkX, position.sectionY, position.chunkZ});
            }
        }

        if (sections.empty()) return;
        // Cheap sampler *view*: O(1) reads of the light we just propagated into
        // interactionWorld above. (The per-chunk constructor re-propagates a
        // ~48x256x48 region with two BFS passes and must not run per edit here.)
        const world::ChunkLightSampler lighting{interactionWorld};
        for (const auto position : sections) {
            remeshSectionImmediate(position, lighting);
        }
    }

    void remeshSectionImmediate(world::SectionPosition position,
                                const world::ChunkLightSampler& lighting) {
        world::SectionMeshUpdate update;
        update.position = position;
        update.mesh = chunkStreamer.acquireMeshData();
        static_cast<void>(world::ChunkMesher::buildSection(
            interactionWorld, {position.chunkX, position.chunkZ}, position.sectionY, lighting,
            update.mesh));
        update.remove = update.mesh.empty();
        update.highPriority = true;
        // Intentionally leave latestSectionRevisions untouched: this preview is a
        // transient overlay. The worker's authoritative rebuild carries a strictly
        // higher revision and overrides it when its batch is polled.
        if (!pendingSectionUpdates.contains(position)) {
            pendingSectionOrder.push_front(position);
        }
        pendingSectionUpdates.insert_or_assign(position, std::move(update));
    }

    void clearRenderedWorld() {
        checkVk(vkDeviceWaitIdle(device), "vkDeviceWaitIdle(world reset)");
        // The device is idle, so every pooled buffer — including those still in
        // the deferred slots — is safe to hand back to the free lists, letting
        // the next world reuse the same buffers instead of reallocating.
        for (auto& slot : deviceBufferPool_.deferred) {
            for (auto& buffer : slot) {
                releaseStreamBufferNow(deviceBufferPool_, buffer);
            }
            slot.clear();
        }
        for (auto& slot : stagingBufferPool_.deferred) {
            for (auto& buffer : slot) {
                releaseStreamBufferNow(stagingBufferPool_, buffer);
            }
            slot.clear();
        }
        for (auto& [position, mesh] : gpuMeshes) {
            static_cast<void>(position);
            releaseStreamBufferNow(deviceBufferPool_, mesh.vertexBuffer);
            releaseStreamBufferNow(deviceBufferPool_, mesh.indexBuffer);
        }
        gpuMeshes.clear();
        occlusionStates.clear();
        occlusionMissCount.clear();
        pendingSectionOrder.clear();
        pendingSectionUpdates.clear();
        latestSectionRevisions.clear();
        // Re-anchor the baked quality at the saved option: a fresh world starts
        // meshing at the stored quality (Off keeps Standard-baked meshes, since
        // the shader ignores the smooth light channels anyway).
        qualityRemeshPending.clear();
        currentMeshQuality = options.smoothLightingQuality != world::SmoothLightingQuality::Off
            ? options.smoothLightingQuality
            : world::SmoothLightingQuality::Standard;
        targetMeshQuality = currentMeshQuality;
        chunkStreamer.setSmoothLightingQuality(currentMeshQuality);
        interactionWorld = {};
        gameSession.worldSimulation() = {};
        gameSession.itemEntities() = {};
        gameSession.worldEntities().clear();
        particleSystem = {};
        activeFurnacePosition.reset();
        gameSession.craftingSystem() = {};
        gameSession.chestSystem() = {};
        activeChest.reset();
        savedEditIndices.clear();
        completedStreamBatchCount = 0U;
        completedBlockEditCount = 0U;
        loadedCpuChunkCount = 0U;
        peakPendingSectionCount = 0U;
        spawnPositionInitialized = false;
        worldReady = false;
    }

    void attachGameRuleHandlers() {
        gameSession.gameRules().setChangeHandler([this](gameplay::GameRuleId id,
                                          const gameplay::GameRuleValueData& value) {
            // randomTickSpeed is the one rule with a runtime mirror (the
            // simulation reads it every tick); doDaylightCycle and keepInventory
            // are read straight from gameSession.gameRules() at their use sites instead.
            if (id == gameplay::GameRuleId::RandomTickSpeed) {
                gameSession.worldSimulation().setRandomTickSpeed(std::get<std::int32_t>(value));
            }
        });
    }

    void startWorld(persistence::SaveGame save) {
        clearRenderedWorld();
        // A new world starts a fresh chat: commands and their results from the
        // previous session must not leak into the bottom-left of the next map.
        chatHistory.clear();
        lastSessionPeakPendingSectionCount = 0U;
        currentSave = std::move(save);
        savedEditIndices.reserve(currentSave->edits.size());
        for (std::size_t index = 0; index < currentSave->edits.size(); ++index) {
            const auto& edit = currentSave->edits[index];
            savedEditIndices.insert_or_assign(PersistentEditPosition{edit.x, edit.y, edit.z},
                                              index);
        }
        gameSession.inventory().restore(currentSave->inventory, currentSave->selectedHotbarSlot);
        gameSession.chestSystem().restore(currentSave->chests);
        // Restore the herd a saved world carried, resolving species by their
        // registered id so a species this build no longer knows is skipped
        // instead of failing to open the world.
        for (const auto& record : currentSave->entities) {
            const auto* type = gameplay::entities::entityTypeRegistry().byId(record.species);
            if (type == nullptr) {
                continue;
            }
            gameSession.worldEntities().restore(
                {record.x, record.y, record.z}, *type, record.yaw,
                {record.vx, record.vy, record.vz}, record.health, record.angerTicks,
                record.ageTicks, record.rngState);
        }
        gameSession.gameMode() = currentSave->gameMode;
        // The world owns its difficulty, the way level.dat does in vanilla.
        gameSession.setDifficulty(currentSave->difficulty);
        // Game rules travel with the world too. The copy from the loaded save
        // carries a null change handler, so it is re-attached and the one rule
        // with a runtime mirror is applied.
        gameSession.gameRules() = currentSave->gameRules;
        attachGameRuleHandlers();
        gameSession.worldSimulation().setRandomTickSpeed(
            gameSession.gameRules().get<std::int32_t>(gameplay::GameRuleId::RandomTickSpeed));
        gameSession.gameTimeSeconds() = currentSave->gameTimeSeconds;
        // The weather travels with the save too; restore() also fades the
        // gradients straight to their flags (World#initWeatherGradients), so a
        // world saved mid-rain reopens raining instead of fading up.
        gameSession.weatherSystem().restore(currentSave->weather);
        const glm::vec3 initialFeet =
            currentSave->hasPlayerPosition
                ? glm::vec3{currentSave->playerX, currentSave->playerY, currentSave->playerZ}
                : glm::vec3{24.0F, 76.38F, 24.0F};
        gameSession.player() = gameplay::PlayerController{initialFeet};
        gameSession.vitals().restore(currentSave->playerHealth, currentSave->playerFoodLevel,
                       currentSave->playerSaturation, currentSave->playerAirTicks);
        // A world saved with an empty health bar reopens with a live gameSession.player().
        if (gameSession.vitals().dead()) {
            gameSession.vitals().reset();
        }
        gameSession.worldSpawnPosition() = initialFeet;
        gameSession.physicsPreviousPosition() = initialFeet;
        gameSession.physicsCurrentPosition() = initialFeet;
        // The /spawnpoint result, if the save carried one; death respawns there.
        gameSession.hasPlayerSpawn() = currentSave->hasSpawnPoint;
        gameSession.playerSpawnPosition() =
            glm::vec3{currentSave->spawnX, currentSave->spawnY, currentSave->spawnZ};
        gameSession.playerSpawnYaw() = currentSave->spawnYaw;
        camera.setPosition(gameSession.player().eyePosition());
        spawnPositionInitialized = currentSave->hasPlayerPosition;
        // Keep the loaded spawn's chunks loaded for the session, vanilla-style.
        chunkStreamer.protectChunks(
            world::chunkPositionFromWorld(initialFeet.x, initialFeet.z), kSpawnChunkRadius);
        worldEpoch = chunkStreamer.resetWorld(currentSave->summary.seed, currentSave->edits);
        updateBiomeColorTextures(currentSave->summary.seed);
        // Natural spawning reads the biome map from the same seed that drives
        // the terrain, so spawns follow the biome being generated.
        gameSession.setWorldSeed(currentSave->summary.seed);
        gameSession.lootRandomState() = static_cast<std::uint32_t>(currentSave->summary.seed) ^
            static_cast<std::uint32_t>(currentSave->summary.seed >> 32U) ^ 0x9E3779B9U;
        // The weather auto-cycle's RNG is seeded from the world the same way the
        // loot RNG is; the timers themselves come from the save above.
        gameSession.weatherSystem().seedRandom(
            static_cast<std::uint32_t>(currentSave->summary.seed) ^
            static_cast<std::uint32_t>(currentSave->summary.seed >> 32U) ^ 0x57E4F10AU);
        // Two-phase load: ask for a small area around the gameSession.player() first so the
        // world opens quickly, then widen to the full render distance once the
        // load screen clears (see the worldReady block) and let the rest stream
        // in during play. The unload radius stays at the full view distance so
        // nothing wrongly evicts while the area is still small.
        const int spawnRadius = std::min(viewDistanceChunks, kSpawnChunkRadius);
        chunkStreamer.setRadii(spawnRadius, viewDistanceChunks);
        chunkStreamer.request(world::chunkPositionFromWorld(initialFeet.x, initialFeet.z));
        worldSessionActive = true;
        paused = true;
        menuSystem.optionsOpen = false;
        menuSystem.pageStack.reset(ui::PageId::Loading);
        menuSystem.saveStatus.clear();
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }

    void startNewWorld() {
        try {
            const auto seed = static_cast<std::uint64_t>(
                std::chrono::high_resolution_clock::now().time_since_epoch().count());
            auto save = saveRepository.create(menuSystem.createWorldName, seed);
            save.gameMode = menuSystem.createWorldGameMode;
            // A new world starts on Normal difficulty, exactly like vanilla;
            // each world then owns the setting from here on.
            save.difficulty = gameplay::Difficulty::Normal;
            gameplay::Inventory initialInventory;
            save.inventory = initialInventory.slots();
            save.selectedHotbarSlot = initialInventory.selectedHotbarSlot();
            saveRepository.save(save);
            refreshSaveList();
            startWorld(std::move(save));
        } catch (const std::exception& exception) {
            menuSystem.saveStatus = "Create failed: " + std::string{exception.what()};
        }
    }

    void saveCurrentWorld() {
        if (!currentSave.has_value() || currentSave->summary.identifier.empty()) {
            menuSystem.saveStatus = "World saving is disabled for this session";
            return;
        }
        currentSave->hasPlayerPosition = true;
        const auto position = gameSession.player().position();
        currentSave->playerX = position.x;
        currentSave->playerY = position.y;
        currentSave->playerZ = position.z;
        currentSave->gameTimeSeconds = gameSession.gameTimeSeconds();
        // The weather timers and flags ride along like game time; the gradients
        // are recomputed from them on load.
        currentSave->weather = gameSession.weatherSystem().state();
        currentSave->gameMode = gameSession.gameMode();
        currentSave->gameRules = gameSession.gameRules();
        // The /spawnpoint result rides along like the player's own position.
        currentSave->hasSpawnPoint = gameSession.hasPlayerSpawn();
        const auto spawnPosition = gameSession.playerSpawnPosition();
        currentSave->spawnX = spawnPosition.x;
        currentSave->spawnY = spawnPosition.y;
        currentSave->spawnZ = spawnPosition.z;
        currentSave->spawnYaw = gameSession.playerSpawnYaw();
        // Difficulty already lives on the save; the button below mutates it in
        // place and world.dat serialises it.
        currentSave->inventory = gameSession.inventory().slots();
        currentSave->selectedHotbarSlot = gameSession.inventory().selectedHotbarSlot();
        currentSave->playerHealth = gameSession.vitals().health();
        currentSave->playerFoodLevel = gameSession.vitals().foodLevel();
        currentSave->playerSaturation = gameSession.vitals().saturation();
        currentSave->playerAirTicks = gameSession.vitals().airTicks();
        currentSave->chests.assign(gameSession.chestSystem().entities().begin(), gameSession.chestSystem().entities().end());
        // The live creatures ride along like the chests: a world saved mid-session
        // reopens with its herd where it was. Species are stored by their
        // registered id path and resolved through the registry on load.
        currentSave->entities.clear();
        currentSave->entities.reserve(gameSession.worldEntities().entities().size());
        for (const auto& entity : gameSession.worldEntities().entities()) {
            if (entity.type == nullptr) {
                continue;
            }
            persistence::PersistentEntity record;
            record.species = std::string{entity.type->id().path};
            record.x = entity.position.x;
            record.y = entity.position.y;
            record.z = entity.position.z;
            record.yaw = entity.yaw;
            record.vx = entity.velocity.x;
            record.vy = entity.velocity.y;
            record.vz = entity.velocity.z;
            record.health = entity.damage.health;
            record.angerTicks = entity.angerTicks;
            record.ageTicks = entity.ageTicks;
            record.rngState = entity.rngState;
            currentSave->entities.push_back(std::move(record));
        }
        try {
            saveRepository.save(*currentSave);
            menuSystem.saveStatus = "World saved";
            refreshSaveList();
        } catch (const std::exception& exception) {
            menuSystem.saveStatus = "Save failed: " + std::string{exception.what()};
        }
    }

    void returnToTitle(bool saveFirst) {
        if (inventoryOpen)
            setInventoryOpen(false);
        if (saveFirst)
            saveCurrentWorld();
        worldSessionActive = false;
        paused = true;
        menuSystem.optionsOpen = false;
        inventoryOpen = false;
        chatOpen = false;
        lastSessionPeakPendingSectionCount = peakPendingSectionCount;
        clearRenderedWorld();
        worldEpoch = chunkStreamer.resetWorld(0U);
        currentSave.reset();
        menuSystem.pageStack.reset(ui::PageId::Title);
        refreshSaveList();
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }

    void processInput() {
        if (!worldReady || inventoryOpen || paused || chatOpen) {
            gameSession.input() = {};
            gameSession.input().flightAllowed = gameSession.gameMode() == gameplay::GameMode::Creative;
            gameSession.jumpPressed() = false;
            gameSession.forwardPressed() = false;
            return;
        }
        gameSession.input().forward = (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS ? 1.0F : 0.0F) -
                              (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS ? 1.0F : 0.0F);
        gameSession.input().strafe = (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS ? 1.0F : 0.0F) -
                             (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS ? 1.0F : 0.0F);
        gameSession.input().lookDirection = camera.direction();
        const bool jumpKeyDown = glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;
        if (jumpKeyDown && !previousJumpKeyDown) {
            gameSession.jumpPressed() = true;
        }
        previousJumpKeyDown = jumpKeyDown;
        gameSession.input().jumpHeld = jumpKeyDown;
        gameSession.input().descendHeld = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS;
        gameSession.input().sneakHeld = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS;
        gameSession.input().sprintHeld = glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS;
        if (stressFrames > 0U) {
            gameSession.forwardPressed() = true;
        }
        gameSession.input().forwardPressed = gameSession.forwardPressed();
        gameSession.input().flightAllowed = gameSession.gameMode() == gameplay::GameMode::Creative;
        // Vanilla only lets a gameSession.player() sprint above six food points, unless they
        // may fly. A creative gameSession.player() is never gated on hunger.
        gameSession.input().sprintAllowed = gameSession.input().flightAllowed || gameSession.vitals().foodLevel() > 6;
        // Bedrock-style auto-jump: walking forward into a one-block rise jumps
        // on its own; the gameSession.player() physics decides when the obstacle is jumpable.
        gameSession.input().autoJump = options.autoJump;
    }


    // The gameSession.player()'s external damage entry: any hit the world deals to the gameSession.player()
    // routes through the shared gameSession.vitals() pipeline, then raises the death screen on
    // the tick it kills — the gameSession.player()'s own onDeath handler.

    // Entity#kill / LivingEntity#kill for the gameSession.player(): OutOfWorld damage at
    // infinite magnitude, the same path /kill <entity> routes a creature through.


    // The mouse callback returns early while a screen is up, so a release that
    // happens behind one never reaches the held flags. Every transition into a
    // screen therefore has to drop them itself.
    void releaseInteractionButtons() {
        breakBlockRequested = false;
        placeBlockRequested = false;
        breakButtonHeld = false;
        useButtonHeld = false;
        miningTarget.reset();
        lastMiningSoundAt = -1.0;
        nextCreativeBreakSeconds = 0.0;
        nextUseSeconds = 0.0;
    }

    void respawnPlayer() {
        gameSession.respawn();
        camera.setPosition(gameSession.player().eyePosition());
        // PlayerManager#respawnPlayer snaps the new body to the spawn's stored
        // angle (vanilla yaw 0) instead of carrying the death look over. The
        // /tp conversion applies here: vanilla yaw 0 faces +Z.
        camera.setRotation(gameSession.playerSpawnYaw() + 90.0F, 0.0F);
        chunkStreamer.request(
            world::chunkPositionFromWorld(gameSession.worldSpawnPosition().x, gameSession.worldSpawnPosition().z));
        setPaused(false);
    }

    void setGameMode(gameplay::GameMode mode) {
        if (gameSession.gameMode() == mode) {
            return;
        }
        gameSession.setGameMode(mode);
        std::cout << "Game mode: " << gameplay::gameModeName(gameSession.gameMode()) << '\n';
        menuSystem.creativeScrollRow = 0U;
        creativeScrollbarDragging = false;
        lastPlayerMode.clear();
    }

    // Vanilla recenters the cursor onto the screen when a menu takes over from
    // the captured gameplay cursor, instead of leaving it where the virtual
    // (captured) position had drifted. firstMouseSample is set before calling
    // this so the recentering event is swallowed by the mouse callback.
    void unlockCursor() {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        int windowWidth = 0;
        int windowHeight = 0;
        glfwGetWindowSize(window, &windowWidth, &windowHeight);
        glfwSetCursorPos(window, windowWidth * 0.5, windowHeight * 0.5);
    }

    void setChatOpen(bool open) {
        chatOpen = open;
        firstMouseSample = true;
        gameSession.jumpPressed() = false;
        releaseInteractionButtons();
        dropRequested = false;
        chatSuggestions_.clear();
        chatSuggestionIndex_ = 0;
        if (!open) {
            suppressedOpeningChatCodepoint = 0U;
        }
        if (open) {
            unlockCursor();
        } else if (!inventoryOpen && !paused) {
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        }
    }

    void submitChatInput() {
        if (chatInputText.empty()) {
            setChatOpen(false);
            return;
        }
        if (chatInputText.front() == '/') {
            const auto result = commandDispatcher.execute(chatInputText);
            chatHistory.push(result.message, result.success, gameSession.gameTimeSeconds());
        } else {
            chatHistory.push("<Player> " + chatInputText, true, gameSession.gameTimeSeconds());
        }
        chatInputText.clear();
        setChatOpen(false);
    }

    // Recomputes the completion list for the token under the (end-of-input)
    // cursor. Called whenever the input changes; Tab cycles without recomputing.
    void refreshChatSuggestions() {
        chatSuggestions_ = commandDispatcher.suggestions(chatInputText, chatInputText.size());
        chatSuggestionIndex_ = 0;
    }

    // Tab: advances the highlighted suggestion and applies it. Applying replaces
    // the partial token from suggestion.start to the end of the line, and the
    // stored list keeps its offsets, so pressing Tab again swaps in the next
    // candidate.
    void cycleChatSuggestion() {
        if (chatSuggestions_.empty()) {
            refreshChatSuggestions();
        }
        if (chatSuggestions_.empty()) {
            return;
        }
        chatSuggestionIndex_ = (chatSuggestionIndex_ + 1U) % chatSuggestions_.size();
        const auto& suggestion = chatSuggestions_[chatSuggestionIndex_];
        if (suggestion.start <= chatInputText.size()) {
            chatInputText.replace(suggestion.start, chatInputText.size() - suggestion.start,
                                  suggestion.text);
        }
    }

    // /spawnpoint's shared effect: record the player's personal spawn point and
    // persist it with the save. `position` absent means "the player's current
    // block position". The stored angle is always 0 because 1.16.1 respawns
    // facing due north; the field exists for the save format's future.
    gameplay::CommandResult applySpawnPoint(const std::optional<glm::vec3>& position) {
        const glm::vec3 spawn = position.value_or(gameSession.player().position());
        gameSession.hasPlayerSpawn() = true;
        gameSession.playerSpawnPosition() = spawn;
        gameSession.playerSpawnYaw() = 0.0F;
        saveCurrentWorld();
        return gameplay::CommandResult{
            true, "Set the spawn point to " + std::to_string(static_cast<int>(spawn.x)) + " " +
                      std::to_string(static_cast<int>(spawn.y)) + " " +
                      std::to_string(static_cast<int>(spawn.z))};
    }

    // The two /tp forms share destination resolution: a Position3 resolves
    // relative axes against the gameSession.player()'s feet and teleports there (applying the
    // optional rotation); a std::string destination is an entity id to teleport
    // onto. `withRotation` marks the `/tp <x> <y> <z> <yaw> <pitch>` form.
    gameplay::CommandResult teleportWithContext(const gameplay::command::CommandContext& context,
                                                bool withRotation) {
        if (const auto position = context.find<gameplay::command::Position3>("destination");
            position.has_value()) {
            const glm::vec3 base = gameSession.player().position();
            const glm::vec3 target{
                position->relativeX ? base.x + static_cast<float>(position->x)
                                    : static_cast<float>(position->x),
                position->relativeY ? base.y + static_cast<float>(position->y)
                                    : static_cast<float>(position->y),
                position->relativeZ ? base.z + static_cast<float>(position->z)
                                    : static_cast<float>(position->z),
            };
            teleportPlayerTo(target);
            if (withRotation) {
                const auto rotation = context.find<gameplay::command::Rotation2>("rotation");
                if (!rotation.has_value()) {
                    return gameplay::CommandResult{
                        false, "Usage: /tp <x> <y> <z> [<yaw> <pitch>]"};
                }
                setPlayerLook(*rotation);
            }
            return gameplay::CommandResult{
                true, "Teleported to " + std::to_string(static_cast<int>(target.x)) + " " +
                          std::to_string(static_cast<int>(target.y)) + " " +
                          std::to_string(static_cast<int>(target.z))};
        }
        const auto entityId = context.find<std::string>("destination");
        if (!entityId.has_value()) {
            return gameplay::CommandResult{
                false, "Usage: /tp <entity> | <x> <y> <z> [<yaw> <pitch>]"};
        }
        if (withRotation) {
            return gameplay::CommandResult{false, "A rotation can only follow a position"};
        }
        const auto foundEntityId = entityIdById(*entityId);
        if (!foundEntityId.has_value()) {
            return gameplay::CommandResult{false, "No entity found: " + *entityId};
        }
        const auto* creature = gameSession.worldEntities().byId(*foundEntityId);
        if (creature == nullptr) {
            return gameplay::CommandResult{false, "No entity found: " + *entityId};
        }
        teleportPlayerTo(creature->position);
        return gameplay::CommandResult{true, "Teleported to the " + *entityId};
    }

    // Entity#teleport without the render interpolation glitch: the gameSession.player(), the
    // physics interpolation endpoints and the camera all snap together, and the
    // chunk streamer recentres so the destination is loaded.
    void teleportPlayerTo(glm::vec3 target) {
        gameSession.player().setPosition(target);
        gameSession.physicsPreviousPosition() = target;
        gameSession.physicsCurrentPosition() = target;
        camera.setPosition(gameSession.player().eyePosition());
        chunkStreamer.request(world::chunkPositionFromWorld(target.x, target.z));
    }

    // Vanilla's /tp yaw is 0 facing +Z; the camera's yaw uses atan2(z, x) with 0
    // facing +X, and vanilla pitch is positive looking down while the camera's
    // is positive looking up. Both convert here.
    void setPlayerLook(const gameplay::command::Rotation2& rotation) {
        camera.setRotation(static_cast<float>(rotation.yaw) + 90.0F,
                           static_cast<float>(-rotation.pitch));
    }

    // The first spawned creature whose registered id (either namespace) matches,
    // as its stable entity id.
    [[nodiscard]] std::optional<std::uint64_t> entityIdById(std::string_view id) const {
        for (const auto& entity : gameSession.worldEntities().entities()) {
            if (entity.type == nullptr) {
                continue;
            }
            if (entity.type->id().matches(id) || entity.type->vanillaId().matches(id)) {
                return entity.id;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::span<const gameplay::ItemStack> activeCreativeCatalog() const {
        if (menuSystem.creativeTab == ui::CreativeTab::Inventory)
            return {};
        return gameplay::creativeCatalog(static_cast<gameplay::CreativeCategory>(menuSystem.creativeTab));
    }

    [[nodiscard]] std::size_t creativeMaximumScrollRow() const {
        const std::size_t rowCount = (activeCreativeCatalog().size() + 8U) / 9U;
        return rowCount > 5U ? rowCount - 5U : 0U;
    }

    [[nodiscard]] float creativeScrollPosition() const {
        const std::size_t maximum = creativeMaximumScrollRow();
        return maximum == 0U ? 0.0F
                             : static_cast<float>(menuSystem.creativeScrollRow) / static_cast<float>(maximum);
    }

    void scrollCreative(int rows) {
        const int maximum = static_cast<int>(creativeMaximumScrollRow());
        const int next = std::clamp(static_cast<int>(menuSystem.creativeScrollRow) + rows, 0, maximum);
        menuSystem.creativeScrollRow = static_cast<std::size_t>(next);
    }

    void setCreativeTab(ui::CreativeTab tab) {
        menuSystem.creativeTab = tab;
        menuSystem.creativeScrollRow = 0U;
        creativeScrollbarDragging = false;
    }

    void updateCreativeScrollFromCursor() {
        if (gameSession.gameMode() != gameplay::GameMode::Creative) {
            return;
        }
        const std::size_t maximum = creativeMaximumScrollRow();
        if (maximum == 0U) {
            menuSystem.creativeScrollRow = 0U;
            return;
        }
        int framebufferWidth = 0;
        int framebufferHeight = 0;
        glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
        const ui::HudLayout layout{static_cast<float>(framebufferWidth),
                                   static_cast<float>(framebufferHeight), menuSystem.guiScaleSetting};
        const auto cursor = currentFramebufferCursor();
        const auto track = layout.creativeScrollbarTrack();
        const float travel = std::max(track.height - 15.0F * layout.scale(), 1.0F);
        const float normalized =
            std::clamp((cursor.y - track.y - 7.5F * layout.scale()) / travel, 0.0F, 1.0F);
        menuSystem.creativeScrollRow =
            static_cast<std::size_t>(std::lround(normalized * static_cast<float>(maximum)));
    }

    void setInventoryOpen(bool open) {
        if (!open) {
            activeInventory().stowCursorStack();
            gameSession.craftingSystem().stowAll(activeInventory());
            if (activeChest.has_value()) {
                gameSession.chestSystem().close(*activeChest);
                activeChest.reset();
            }
            containerScreen = ContainerScreen::PlayerInventory;
        }
        inventoryOpen = open;
        creativeScrollbarDragging = false;
        firstMouseSample = true;
        gameSession.jumpPressed() = false;
        releaseInteractionButtons();
        if (open) {
            unlockCursor();
        } else if (!paused && !chatOpen) {
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        }
    }

    void openContainer(ContainerScreen screen) {
        containerScreen = screen;
        setInventoryOpen(true);
        containerScreen = screen;
    }

    // Keeps the furnace the gameSession.player() last opened in the burning block state while
    // the shared furnace state burns. The swap is transient: it updates the
    // render world and the worker but is not recorded as a save edit, so a
    // reload opens with plain unlit furnaces (furnace contents are not saved).
    void updateFurnaceLitState() {
        if (!activeFurnacePosition.has_value()) {
            return;
        }
        const auto position = *activeFurnacePosition;
        const world::Block current = interactionWorld.block(position.x, position.y, position.z);
        if (current != world::Block::Furnace && current != world::Block::LitFurnace) {
            return; // the furnace was mined or replaced
        }
        const world::Block desired = gameSession.craftingSystem().furnaceFuelProgress() > 0.0F
            ? world::Block::LitFurnace
            : world::Block::Furnace;
        if (current == desired) {
            return;
        }
        const auto orientation = interactionWorld.orientation(position.x, position.y, position.z);
        interactionWorld.setBlock(position.x, position.y, position.z, desired);
        interactionWorld.setOrientation(position.x, position.y, position.z, orientation);
        chunkStreamer.setBlock(position.x, position.y, position.z, desired, 0U, orientation);
        previewBlockEdit(position.x, position.y, position.z);
    }

    void openChest(gameplay::ChestPosition position) {
        if (!gameSession.chestSystem().open(position))
            return;
        activeChest = position;
        openContainer(ContainerScreen::Chest);
    }

    void setPaused(bool pause) {
        if (!worldSessionActive)
            return;
        if (pause && inventoryOpen) {
            setInventoryOpen(false);
        }
        if (pause && chatOpen) {
            chatInputText.clear();
            chatOpen = false;
        }
        paused = pause;
        menuSystem.optionsOpen = false;
        if (pause)
            menuSystem.pageStack.push(ui::PageId::Pause);
        else
            menuSystem.pageStack.reset(ui::PageId::Game);
        pressedMenuButton = MenuButton::None;
        menuSystem.viewDistanceSliderDragging = false;
        menuSystem.simulationDistanceSliderDragging = false;
        menuSystem.masterVolumeSliderDragging = false;
        firstMouseSample = true;
        gameSession.input() = {};
        gameSession.jumpPressed() = false;
        releaseInteractionButtons();
        dropRequested = false;
        gameSession.physicsPreviousPosition() = gameSession.player().position();
        gameSession.physicsCurrentPosition() = gameSession.player().position();
        if (pause) {
            unlockCursor();
        } else {
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        }
    }

    [[nodiscard]] ui::UiPoint currentFramebufferCursor() const {
        double cursorX = 0.0;
        double cursorY = 0.0;
        int windowWidth = 0;
        int windowHeight = 0;
        int framebufferWidth = 0;
        int framebufferHeight = 0;
        glfwGetCursorPos(window, &cursorX, &cursorY);
        glfwGetWindowSize(window, &windowWidth, &windowHeight);
        glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
        return ui::windowToFramebuffer(cursorX, cursorY, windowWidth, windowHeight,
                                       framebufferWidth, framebufferHeight);
    }

    [[nodiscard]] MenuButton menuButtonForIndex(std::size_t index) const {
        switch (menuSystem.pageStack.current()) {
        case ui::PageId::Title: {
            constexpr std::array buttons{MenuButton::Singleplayer, MenuButton::Options,
                                         MenuButton::Exit};
            return buttons.at(index);
        }
        case ui::PageId::WorldList: {
            // Two columns of two: left Play/Create, right Edit/Back.
            constexpr std::array buttons{MenuButton::PlaySelected, MenuButton::CreateWorld,
                                         MenuButton::Edit, MenuButton::Back};
            return buttons.at(index);
        }
        case ui::PageId::CreateWorld: {
            constexpr std::array buttons{MenuButton::CreateGameMode, MenuButton::CreateConfirm,
                                         MenuButton::Back};
            return buttons.at(index);
        }
        case ui::PageId::EditWorld: {
            constexpr std::array buttons{MenuButton::SaveRename, MenuButton::DeleteWorld,
                                         MenuButton::Back};
            return buttons.at(index);
        }
        case ui::PageId::ConfirmDelete: {
            constexpr std::array buttons{MenuButton::DeleteConfirm, MenuButton::DeleteCancel};
            return buttons.at(index);
        }
        case ui::PageId::Options: {
            // The Difficulty button only exists while a world is open: the
            // setting is per-save and has no meaning on the title-screen
            // options page (vanilla does not list it there either).
            if (currentSave.has_value()) {
                constexpr std::array buttons{
                    MenuButton::MasterVolume, MenuButton::Difficulty, MenuButton::Controls,
                    MenuButton::VideoSettings, MenuButton::Language, MenuButton::Experimental,
                    MenuButton::Done};
                return buttons.at(index);
            }
            constexpr std::array buttons{
                MenuButton::MasterVolume, MenuButton::Controls, MenuButton::VideoSettings,
                MenuButton::Language, MenuButton::Experimental, MenuButton::Done};
            return buttons.at(index);
        }
        case ui::PageId::Experimental: {
            constexpr std::array buttons{MenuButton::RainMode, MenuButton::ParticleLevel,
                                         MenuButton::SunShadows, MenuButton::RainCollisionCache,
                                         MenuButton::Back};
            return buttons.at(index);
        }
        case ui::PageId::Language: {
            constexpr std::array buttons{MenuButton::ForceUnicodeFont, MenuButton::Done};
            return buttons.at(index);
        }
        case ui::PageId::VideoSettings: {
            constexpr std::array buttons{
                MenuButton::Resolution, MenuButton::GuiScale, MenuButton::ViewDistance,
                MenuButton::SimulationDistance, MenuButton::FrameRateLimit,
                MenuButton::AntiAliasing, MenuButton::Anisotropy, MenuButton::SmoothLighting,
                MenuButton::DynamicLight, MenuButton::Vsync, MenuButton::Done};
            return buttons.at(index);
        }
        case ui::PageId::Controls: {
            constexpr std::array buttons{MenuButton::ViewBobbing, MenuButton::AutoJump,
                                         MenuButton::Done};
            return buttons.at(index);
        }
        case ui::PageId::Pause: {
            constexpr std::array buttons{MenuButton::Resume, MenuButton::Options,
                                         MenuButton::SaveQuit};
            return buttons.at(index);
        }
        case ui::PageId::Death: {
            constexpr std::array buttons{MenuButton::Respawn, MenuButton::TitleScreen};
            return buttons.at(index);
        }
        default:
            return MenuButton::None;
        }
    }

    [[nodiscard]] std::size_t menuButtonCount() const {
        switch (menuSystem.pageStack.current()) {
        case ui::PageId::Title:
            return 3U;
        case ui::PageId::WorldList:
            return 4U;
        case ui::PageId::CreateWorld:
            return 3U;
        case ui::PageId::EditWorld:
            return 3U;
        case ui::PageId::ConfirmDelete:
            return 2U;
        case ui::PageId::Options:
            // One fewer button without a world open (no Difficulty entry).
            return currentSave.has_value() ? 7U : 6U;
        case ui::PageId::Experimental:
            return 5U;
        case ui::PageId::VideoSettings:
            return 11U;
        case ui::PageId::Controls:
            return 3U;
        case ui::PageId::Language:
            return 2U;
        case ui::PageId::Pause:
            return 3U;
        case ui::PageId::Death:
            return 2U;
        default:
            return 0U;
        }
    }

    // Save-screen list rows live in the band between the title and the
    // bottom-anchored function buttons; saveListVisibleRowCount decides how
    // many fit so the rows never collide with the buttons at any resolution.
    [[nodiscard]] ui::UiRect worldListRow(std::size_t index, const ui::HudLayout& layout) const {
        const float scale = layout.scale();
        const float width =
            std::min(300.0F * scale, static_cast<float>(swapchainExtent.width) - 20.0F * scale);
        return {
            (static_cast<float>(swapchainExtent.width) - width) * 0.5F,
            (34.0F + static_cast<float>(index) * 22.0F) * scale,
            width,
            20.0F * scale,
        };
    }

    // How many world rows fit in the list band at the current canvas size. The
    // list fills the space between the title and the bottom buttons instead of
    // a fixed three rows, mirroring 1.16.1's three-layer save screen.
    [[nodiscard]] std::size_t saveListVisibleRowCount() const {
        const ui::HudLayout layout{static_cast<float>(swapchainExtent.width),
                                   static_cast<float>(swapchainExtent.height), menuSystem.guiScaleSetting};
        const float scale = layout.scale();
        constexpr float kListTop = 34.0F;  // first row's top edge, in scale units
        constexpr float kRowStep = 22.0F;  // vertical distance between row tops
        // The world list's four function buttons sit in two columns of two,
        // so the block occupies exactly two rows on the bottom band.
        constexpr float kButtonRows = 2.0F;
        constexpr float kButtonHeight = 20.0F;
        constexpr float kButtonStep = 24.0F;
        constexpr float kBottomMargin = 16.0F; // canvas bottom to last button's bottom
        constexpr float kListToButtonGap = 12.0F;
        const float logicalHeight = static_cast<float>(swapchainExtent.height) / scale;
        const float buttonBlockTop =
            logicalHeight - kBottomMargin - kButtonHeight - (kButtonRows - 1.0F) * kButtonStep;
        const float available = buttonBlockTop - kListToButtonGap - kListTop;
        const float rows = std::max(available / kRowStep, 1.0F);
        return static_cast<std::size_t>(rows);
    }

    // The language screen's selection list: a full-width dark box that runs from
    // the left to the right screen edge (the save-selection screen's band look).
    // It is sized to its rows and centred vertically in the space between the
    // title and the grey warning line, so the languages sit absolutely centred
    // rather than packed against the top of a tall box.
    [[nodiscard]] ui::UiRect languageListBox(const ui::HudLayout& layout) const {
        const float scale = layout.scale();
        constexpr float kRowStep = 22.0F;
        const float topBound = 44.0F * scale;
        const float warningY = languageWarningY(layout);
        const float bottomBound = warningY - 8.0F * scale;
        const float width = static_cast<float>(swapchainExtent.width);
        // Content-sized height: as many rows as fit in the band.
        const std::size_t rows = std::max<std::size_t>(
            static_cast<std::size_t>((bottomBound - topBound) / (kRowStep * scale)), 1U);
        const float height = static_cast<float>(rows) * kRowStep * scale;
        const float top = topBound + (bottomBound - topBound - height) * 0.5F;
        return {0.0F, top, width, height};
    }

    // The grey "(" + options.languageWarning + ")" line below the list, exactly
    // where 1.16.1's LanguageOptionsScreen draws it (between list and buttons).
    [[nodiscard]] float languageWarningY(const ui::HudLayout& layout) const {
        const float scale = layout.scale();
        const auto firstButton = layout.bottomMenuButton(0U, 2U, 2U);
        return firstButton.y - 16.0F * scale;
    }

    // One row of the language list inside the full-width dark box.
    [[nodiscard]] ui::UiRect languageRow(
        std::size_t index,
        const ui::HudLayout& layout) const {
        const float scale = layout.scale();
        const auto box = languageListBox(layout);
        constexpr float kRowStep = 22.0F;
        return {
            box.x + 2.0F * scale,
            box.y + static_cast<float>(index) * kRowStep * scale,
            box.width - 4.0F * scale,
            20.0F * scale,
        };
    }

    // How many language rows fit inside the black box at the current canvas.
    [[nodiscard]] std::size_t languageVisibleRowCount() const {
        const ui::HudLayout layout{static_cast<float>(swapchainExtent.width),
                                   static_cast<float>(swapchainExtent.height), menuSystem.guiScaleSetting};
        const float scale = layout.scale();
        constexpr float kRowStep = 22.0F;
        const float rows = std::max(languageListBox(layout).height / (kRowStep * scale), 1.0F);
        return static_cast<std::size_t>(rows);
    }

    // Shared button geometry for the frontend pages: the save screen and its
    // edit/delete pages anchor their buttons to the bottom (the world list in
    // two columns), while the title and create screens keep the centred menu.
    [[nodiscard]] ui::UiRect frontendButtonRect(const ui::HudLayout& layout, ui::PageId page,
                                                std::size_t index,
                                                std::size_t buttonCount) const {
        if (page == ui::PageId::WorldList) {
            return layout.bottomMenuButton(index, buttonCount, 2U);
        }
        // The video page grew past one column's worth of buttons: its settings
        // stack in two centred columns with "Done" on its own row beneath, so
        // the bottom button stays on screen instead of falling off the queue.
        if (page == ui::PageId::VideoSettings) {
            return layout.videoSettingsButton(index, buttonCount);
        }
        if (page == ui::PageId::EditWorld || page == ui::PageId::ConfirmDelete) {
            return layout.bottomMenuButton(index, buttonCount);
        }
        // 1.16.1's LanguageOptionsScreen places "Force Unicode Font" and "Done"
        // side by side at the bottom (width/2 - 155 and +160), not stacked.
        if (page == ui::PageId::Language) {
            return layout.bottomMenuButton(index, buttonCount, 2U);
        }
        return layout.menuButton(index, buttonCount);
    }

    [[nodiscard]] MenuButton hoveredMenuButton() const {
        const auto cursor = currentFramebufferCursor();
        const ui::HudLayout layout{static_cast<float>(swapchainExtent.width),
                                   static_cast<float>(swapchainExtent.height), menuSystem.guiScaleSetting};
        const std::size_t buttonCount = menuButtonCount();
        for (std::size_t index = 0; index < buttonCount; ++index) {
            const auto rectangle =
                frontendButtonRect(layout, menuSystem.pageStack.current(), index, buttonCount);
            if (rectangle.contains(cursor.x, cursor.y)) {
                return menuButtonForIndex(index);
            }
        }
        return MenuButton::None;
    }

    void handleMenuButtonPress() {
        if (menuSystem.pageStack.current() == ui::PageId::WorldList) {
            const auto cursor = currentFramebufferCursor();
            const ui::HudLayout layout{static_cast<float>(swapchainExtent.width),
                                       static_cast<float>(swapchainExtent.height), menuSystem.guiScaleSetting};
            const std::size_t visibleRows = saveListVisibleRowCount();
            const std::size_t maximumFirst =
                menuSystem.saveSummaries.size() > visibleRows ? menuSystem.saveSummaries.size() - visibleRows : 0U;
            const std::size_t first = std::min(menuSystem.worldListFirstIndex, maximumFirst);
            const std::size_t visible = std::min<std::size_t>(
                menuSystem.saveSummaries.size() - std::min(first, menuSystem.saveSummaries.size()), visibleRows);
            for (std::size_t index = 0; index < visible; ++index) {
                if (worldListRow(index, layout).contains(cursor.x, cursor.y)) {
                    menuSystem.selectedWorldIndex = first + index;
                    break;
                }
            }
        }
        pressedMenuButton = hoveredMenuButton();
        if (pressedMenuButton == MenuButton::ViewDistance) {
            menuSystem.viewDistanceSliderDragging = true;
            updateViewDistanceFromCursor();
        } else if (pressedMenuButton == MenuButton::SimulationDistance) {
            menuSystem.simulationDistanceSliderDragging = true;
            updateSimulationDistanceFromCursor();
        } else if (pressedMenuButton == MenuButton::MasterVolume) {
            menuSystem.masterVolumeSliderDragging = true;
            updateMasterVolumeFromCursor();
        }
        // Clicking a language row in the language screen selects it at once,
        // like 1.16.1's LanguageSelectionList.
        if (menuSystem.pageStack.current() == ui::PageId::Language) {
            const auto cursor = currentFramebufferCursor();
            const ui::HudLayout layout{static_cast<float>(swapchainExtent.width),
                                       static_cast<float>(swapchainExtent.height), menuSystem.guiScaleSetting};
            const std::size_t visible = languageVisibleRowCount();
            const std::size_t maximumFirst =
                menuSystem.languageCodes.size() > visible ? menuSystem.languageCodes.size() - visible : 0U;
            const std::size_t first = std::min(menuSystem.languageListFirstIndex, maximumFirst);
            for (std::size_t row = 0; row < visible; ++row) {
                const std::size_t index = first + row;
                if (index >= menuSystem.languageCodes.size()) {
                    break;
                }
                if (languageRow(row, layout).contains(cursor.x, cursor.y)) {
                    selectLanguage(menuSystem.languageCodes[index]);
                    playUiClick();
                    break;
                }
            }
        }
    }

    void updateViewDistanceFromCursor() {
        if (!menuSystem.optionsOpen) {
            return;
        }
        const auto cursor = currentFramebufferCursor();
        const ui::HudLayout layout{static_cast<float>(swapchainExtent.width),
                                   static_cast<float>(swapchainExtent.height), menuSystem.guiScaleSetting};
        const auto slider = frontendButtonRect(layout, menuSystem.pageStack.current(), 2U,
                                               menuButtonCount());
        const float inset = 4.0F * layout.scale();
        const float travel = std::max(slider.width - inset * 2.0F, 1.0F);
        const float normalized = std::clamp((cursor.x - slider.x - inset) / travel, 0.0F, 1.0F);
        const int requested =
            std::clamp(2 + static_cast<int>(std::lround(normalized * 34.0F)), 2, 36);
        if (requested == viewDistanceChunks) {
            return;
        }
        viewDistanceChunks = requested;
        chunkStreamer.setRadii(viewDistanceChunks, viewDistanceChunks);
    }

    // Vanilla's Simulation Distance slider: the frozen-entity radius, in chunks,
    // applied to the session's tick gate so it stays independent of the render
    // distance. Range 2..12 chunks, the same units the view-distance slider uses.
    void updateSimulationDistanceFromCursor() {
        if (!menuSystem.optionsOpen) {
            return;
        }
        const auto cursor = currentFramebufferCursor();
        const ui::HudLayout layout{static_cast<float>(swapchainExtent.width),
                                   static_cast<float>(swapchainExtent.height), menuSystem.guiScaleSetting};
        const auto slider = frontendButtonRect(layout, menuSystem.pageStack.current(), 3U,
                                               menuButtonCount());
        const float inset = 4.0F * layout.scale();
        const float travel = std::max(slider.width - inset * 2.0F, 1.0F);
        const float normalized = std::clamp((cursor.x - slider.x - inset) / travel, 0.0F, 1.0F);
        const int requested =
            std::clamp(2 + static_cast<int>(std::lround(normalized * 10.0F)), 2, 12);
        if (requested == simulationDistanceChunks) {
            return;
        }
        simulationDistanceChunks = requested;
        gameSession.setSimulationRadius(static_cast<float>(simulationDistanceChunks) *
                                        static_cast<float>(world::kChunkWidth));
    }

    void updateMasterVolumeFromCursor() {
        if (!menuSystem.optionsOpen) {
            return;
        }
        const auto cursor = currentFramebufferCursor();
        const ui::HudLayout layout{static_cast<float>(swapchainExtent.width),
                                   static_cast<float>(swapchainExtent.height), menuSystem.guiScaleSetting};
        const auto slider = layout.menuButton(0U, 3U);
        const float inset = 4.0F * layout.scale();
        const float travel = std::max(slider.width - inset * 2.0F, 1.0F);
        options.masterVolume = std::clamp((cursor.x - slider.x - inset) / travel, 0.0F, 1.0F);
        audioSystem.setMasterVolume(options.masterVolume);
    }

    void persistOptions() noexcept {
        const auto resolution = ui::kDisplayResolutions[menuSystem.resolutionIndex];
        options.windowWidth = resolution.width;
        options.windowHeight = resolution.height;
        options.guiScale = menuSystem.guiScaleSetting;
        options.viewDistance = std::clamp(viewDistanceChunks, 2, 36);
        options.simulationDistance = std::clamp(simulationDistanceChunks, 2, 12);
        try {
            options.save(optionsPath);
        } catch (const std::exception& exception) {
            std::cerr << "Unable to persist options: " << exception.what() << '\n';
        }
    }

    void cycleResolution() {
        menuSystem.resolutionIndex = (menuSystem.resolutionIndex + 1U) % ui::kDisplayResolutions.size();
        const auto resolution = ui::kDisplayResolutions[menuSystem.resolutionIndex];
        // On macOS a maximized window keeps its maximized flag when the client
        // size is set directly, so the requested size never lands. Restore the
        // window first, then apply the new resolution as a plain windowed size.
        if (glfwGetWindowAttrib(window, GLFW_MAXIMIZED) == GLFW_TRUE) {
            glfwRestoreWindow(window);
        }
        glfwSetWindowSize(window, resolution.width, resolution.height);
        persistOptions();
    }

    void cycleGuiScale() {
        int framebufferWidth = 0;
        int framebufferHeight = 0;
        glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
        const int maximumScale =
            ui::HudLayout::calculateGuiScale(framebufferWidth, framebufferHeight, 0);
        menuSystem.guiScaleSetting = (menuSystem.guiScaleSetting + 1) % (maximumScale + 1);
        persistOptions();
    }

    void handleMenuButtonRelease() {
        if (menuSystem.viewDistanceSliderDragging) {
            updateViewDistanceFromCursor();
            persistOptions();
        }
        if (menuSystem.masterVolumeSliderDragging) {
            updateMasterVolumeFromCursor();
            persistOptions();
            // Vanilla sliders play feedback on release. Keep the preview at
            // the listener so attenuation cannot hide whether audio works.
            if (options.masterVolume > 0.0F) {
                audioSystem.playItemPickup(camera.position());
            }
        }
        const MenuButton releasedButton = hoveredMenuButton();
        const MenuButton activatedButton =
            releasedButton == pressedMenuButton ? releasedButton : MenuButton::None;
        pressedMenuButton = MenuButton::None;
        menuSystem.viewDistanceSliderDragging = false;
        menuSystem.simulationDistanceSliderDragging = false;
        menuSystem.masterVolumeSliderDragging = false;
        // A button that actually fired (pressed and released over the same spot)
        // clicks; the two sliders are drags and keep their own feedback.
        if (activatedButton != MenuButton::None &&
            activatedButton != MenuButton::ViewDistance &&
            activatedButton != MenuButton::MasterVolume) {
            playUiClick();
        }
        switch (activatedButton) {
        case MenuButton::Resume:
            setPaused(false);
            break;
        case MenuButton::Options:
            menuSystem.optionsOpen = true;
            menuSystem.pageStack.push(ui::PageId::Options);
            break;
        case MenuButton::Quit:
            glfwSetWindowShouldClose(window, GLFW_TRUE);
            break;
        case MenuButton::Resolution:
            cycleResolution();
            break;
        case MenuButton::GuiScale:
            cycleGuiScale();
            break;
        case MenuButton::ViewDistance:
            break;
        case MenuButton::MasterVolume:
            break;
        case MenuButton::VideoSettings:
            menuSystem.pageStack.push(ui::PageId::VideoSettings);
            break;
        case MenuButton::Controls:
            menuSystem.pageStack.push(ui::PageId::Controls);
            break;
        case MenuButton::AutoJump:
            options.autoJump = !options.autoJump;
            persistOptions();
            break;
        case MenuButton::FrameRateLimit: {
            constexpr std::array limits{30, 60, 120, 144, 240, 0};
            const auto found = std::ranges::find(limits, options.frameRateLimit);
            const std::size_t current = found == limits.end()
                ? 0U : static_cast<std::size_t>(std::distance(limits.begin(), found));
            options.frameRateLimit = limits[(current + 1U) % limits.size()];
            persistOptions();
            break;
        }
        case MenuButton::AntiAliasing:
            options.antiAliasing = !options.antiAliasing;
            persistOptions();
            recreateSwapchain();
            break;
        case MenuButton::Vsync:
            options.vsync = !options.vsync;
            persistOptions();
            // The present mode lives on the swapchain, so toggling sync has to
            // rebuild it with the matching FIFO/MAILBOX choice.
            recreateSwapchain();
            break;
        case MenuButton::Anisotropy:
            options.anisotropy = options.anisotropy >= 16 ? 1 : options.anisotropy * 2;
            persistOptions();
            recreateTextureSampler();
            break;
        case MenuButton::ViewBobbing:
            options.viewBobbing = !options.viewBobbing;
            persistOptions();
            break;
        case MenuButton::SmoothLighting: {
            options.smoothLightingQuality =
                nextSmoothLightingQuality(options.smoothLightingQuality);
            persistOptions();
            // The mesh is baked at the active quality (the packed vertex carries
            // one AO set), so changing the baked tier re-meshes every loaded
            // section; switching to Off keeps the last baked meshes and the
            // shader just ignores the smooth light channels.
            const auto baked = options.smoothLightingQuality == world::SmoothLightingQuality::Off
                ? currentMeshQuality
                : options.smoothLightingQuality;
            if (baked != currentMeshQuality) {
                targetMeshQuality = baked;
                qualityRemeshPending.clear();
                for (const auto& [position, mesh] : gpuMeshes) {
                    qualityRemeshPending.insert(position);
                }
                for (const auto& [position, update] : pendingSectionUpdates) {
                    qualityRemeshPending.insert(position);
                }
                chunkStreamer.setSmoothLightingQuality(baked);
                chunkStreamer.requestFullRemesh();
            }
            break;
        }
        case MenuButton::DynamicLight:
            options.dynamicLight = !options.dynamicLight;
            persistOptions();
            break;
        case MenuButton::Difficulty:
            // Vanilla's Difficulty button cycles the open world's setting in
            // place; it has no meaning outside a world, which is why the button
            // only exists on the in-world options page.
            if (currentSave.has_value()) {
                currentSave->difficulty =
                    gameplay::nextDifficulty(currentSave->difficulty);
                gameSession.setDifficulty(currentSave->difficulty);
            }
            break;
        case MenuButton::Experimental:
            menuSystem.pageStack.push(ui::PageId::Experimental);
            break;
        case MenuButton::RainMode:
            options.rainMode = (options.rainMode + 1) % 3;
            rainMode_ = static_cast<RainMode>(options.rainMode);
            persistOptions();
            break;
        case MenuButton::ParticleLevel:
            options.particleLevel = (options.particleLevel + 1) % 4;
            applyParticleLevel();
            persistOptions();
            break;
        case MenuButton::SunShadows:
            options.sunShadows = !options.sunShadows;
            shadowDisabled = !options.sunShadows;
            persistOptions();
            break;
        case MenuButton::RainCollisionCache:
            options.rainCollisionCache = !options.rainCollisionCache;
            rainSystem.setCollisionCache(options.rainCollisionCache);
            persistOptions();
            break;
        case MenuButton::Language:
            refreshLanguageNames();
            menuSystem.pageStack.push(ui::PageId::Language);
            break;
        case MenuButton::ForceUnicodeFont:
            options.forceUnicodeFont = !options.forceUnicodeFont;
            textFont.setForceUnicode(options.forceUnicodeFont);
            recreateFontTexture();
            persistOptions();
            break;
        case MenuButton::Done:
            menuSystem.pageStack.pop();
            menuSystem.optionsOpen = menuSystem.pageStack.current() == ui::PageId::Options;
            break;
        case MenuButton::Singleplayer:
            refreshSaveList();
            menuSystem.pageStack.push(ui::PageId::WorldList);
            break;
        case MenuButton::Exit:
            glfwSetWindowShouldClose(window, GLFW_TRUE);
            break;
        case MenuButton::PlaySelected:
            if (menuSystem.selectedWorldIndex < menuSystem.saveSummaries.size()) {
                try {
                    startWorld(saveRepository.load(menuSystem.saveSummaries[menuSystem.selectedWorldIndex].identifier));
                } catch (const std::exception& exception) {
                    menuSystem.saveStatus = "Load failed: " + std::string{exception.what()};
                }
            }
            break;
        case MenuButton::CreateWorld:
            menuSystem.createWorldName.clear();
            menuSystem.createWorldGameMode = gameplay::GameMode::Survival;
            menuSystem.pageStack.push(ui::PageId::CreateWorld);
            break;
        case MenuButton::Edit:
            if (menuSystem.selectedWorldIndex < menuSystem.saveSummaries.size()) {
                menuSystem.editWorldIdentifier = menuSystem.saveSummaries[menuSystem.selectedWorldIndex].identifier;
                menuSystem.editWorldName = menuSystem.saveSummaries[menuSystem.selectedWorldIndex].displayName;
                menuSystem.pageStack.push(ui::PageId::EditWorld);
            }
            break;
        case MenuButton::SaveRename:
            applyRename();
            break;
        case MenuButton::DeleteWorld:
            menuSystem.pageStack.push(ui::PageId::ConfirmDelete);
            break;
        case MenuButton::DeleteConfirm:
            deleteSelectedWorld();
            break;
        case MenuButton::DeleteCancel:
            menuSystem.pageStack.pop();
            break;
        case MenuButton::Back:
            menuSystem.pageStack.pop();
            break;
        case MenuButton::CreateConfirm:
            startNewWorld();
            break;
        case MenuButton::CreateGameMode:
            menuSystem.createWorldGameMode = menuSystem.createWorldGameMode == gameplay::GameMode::Survival
                                      ? gameplay::GameMode::Creative
                                      : gameplay::GameMode::Survival;
            break;
        case MenuButton::SaveQuit:
            returnToTitle(true);
            break;
        case MenuButton::Respawn:
            respawnPlayer();
            break;
        case MenuButton::TitleScreen:
            returnToTitle(true);
            break;
        case MenuButton::None:
            break;
        }
    }

    // The slot and creative hit-testing shared by a press and by the release
    // that ends a drag: resolve the cursor's position and act on whatever is
    // under it (a container slot, a gameSession.player() slot, a creative tab, or drop the
    // cursor stack on empty space).
    void dispatchInventoryClick(gameplay::InventoryMouseButton button, bool shiftHeld) {
        double cursorX = 0.0;
        double cursorY = 0.0;
        int windowWidth = 0;
        int windowHeight = 0;
        int framebufferWidth = 0;
        int framebufferHeight = 0;
        glfwGetCursorPos(window, &cursorX, &cursorY);
        glfwGetWindowSize(window, &windowWidth, &windowHeight);
        glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
        if (windowWidth <= 0 || windowHeight <= 0) {
            return;
        }
        const auto framebufferCursor = ui::windowToFramebuffer(
            cursorX, cursorY, windowWidth, windowHeight, framebufferWidth, framebufferHeight);
        const ui::HudLayout layout{static_cast<float>(framebufferWidth),
                                   static_cast<float>(framebufferHeight), menuSystem.guiScaleSetting};
        // Inventory and container slots are silent in vanilla: only actual
        // buttons (AbstractButtonWidget) play ui.button.click, so picking up
        // or moving an item never clicks. The menu buttons above are the only
        // sound in this screen family.
    if (containerScreen == ContainerScreen::Chest && activeChest.has_value()) {
        for (std::size_t index = 0; index < gameplay::ChestBlockEntity::kSlotCount; ++index) {
            if (layout.chestSlot(index).contains(framebufferCursor.x, framebufferCursor.y)) {
                gameSession.chestSystem().clickSlot(*activeChest, index, gameSession.inventory(), button, shiftHeld);
                return;
            }
        }
    } else if (containerScreen == ContainerScreen::CraftingTable) {
        for (std::size_t index = 0; index < 9U; ++index) {
            if (layout.tableCraftingSlot(index).contains(framebufferCursor.x,
                                                         framebufferCursor.y)) {
                gameSession.craftingSystem().clickTableSlot(gameSession.inventory(), index, button, shiftHeld);
                return;
            }
        }
        if (layout.tableCraftingOutput().contains(framebufferCursor.x, framebufferCursor.y)) {
            // Shift-click is QUICK_MOVE: the whole result goes to the gameSession.player()
            // gameSession.inventory() instead of the cursor, like vanilla's result slot.
            static_cast<void>(gameSession.craftingSystem().craftTable(gameSession.inventory(), shiftHeld));
            return;
        }
    } else if (containerScreen == ContainerScreen::Furnace) {
        if (layout.furnaceInputSlot().contains(framebufferCursor.x, framebufferCursor.y)) {
            gameSession.craftingSystem().clickFurnaceInput(gameSession.inventory(), button, shiftHeld);
            return;
        }
        if (layout.furnaceFuelSlot().contains(framebufferCursor.x, framebufferCursor.y)) {
            gameSession.craftingSystem().clickFurnaceFuel(gameSession.inventory(), button, shiftHeld);
            return;
        }
        if (layout.furnaceOutputSlot().contains(framebufferCursor.x, framebufferCursor.y)) {
            gameSession.craftingSystem().clickFurnaceOutput(gameSession.inventory(), shiftHeld);
            return;
        }
    } else if (gameSession.gameMode() == gameplay::GameMode::Survival) {
        for (std::size_t index = 0; index < 4U; ++index) {
            if (layout.playerCraftingSlot(index).contains(framebufferCursor.x,
                                                          framebufferCursor.y)) {
                gameSession.craftingSystem().clickPlayerSlot(gameSession.inventory(), index, button, shiftHeld);
                return;
            }
        }
        if (layout.playerCraftingOutput().contains(framebufferCursor.x, framebufferCursor.y)) {
            // Shift-click is QUICK_MOVE: the whole result goes to the gameSession.player()
            // gameSession.inventory() instead of the cursor, like vanilla's result slot.
            static_cast<void>(gameSession.craftingSystem().craftPlayer(gameSession.inventory(), shiftHeld));
            return;
        }
    }
    if (containerScreen != ContainerScreen::PlayerInventory) {
        for (std::size_t index = 0; index < gameplay::Inventory::kSlotCount; ++index) {
            const auto slot = containerScreen == ContainerScreen::Chest
                                  ? layout.chestInventorySlot(index)
                                  : layout.inventorySlot(index);
            if (slot.contains(framebufferCursor.x, framebufferCursor.y)) {
                if (shiftHeld) {
                    // QUICK_MOVE from the gameSession.player() gameSession.inventory() into the open
                    // container, the reverse of the container-slot shift.
                    moveInventorySlotToContainer(index);
                } else {
                    gameSession.inventory().clickSlot(index, button, false);
                }
                return;
            }
        }
        if (!layout.inventoryPanel().contains(framebufferCursor.x, framebufferCursor.y)) {
            spawnDroppedStack(gameSession.inventory().takeCursorStack());
        }
        return;
    }
    if (gameSession.gameMode() == gameplay::GameMode::Creative) {
        if (button == gameplay::InventoryMouseButton::Left) {
            for (std::size_t tabIndex = 0; tabIndex < kCreativeTabCount; ++tabIndex) {
                if (layout.creativeTab(tabIndex).contains(framebufferCursor.x,
                                                          framebufferCursor.y)) {
                    setCreativeTab(static_cast<ui::CreativeTab>(tabIndex));
                    return;
                }
            }
            if (layout.creativeScrollbarTrack().contains(framebufferCursor.x,
                                                         framebufferCursor.y) &&
                creativeMaximumScrollRow() > 0U) {
                creativeScrollbarDragging = true;
                updateCreativeScrollFromCursor();
                return;
            }
        }
        if (menuSystem.creativeTab == ui::CreativeTab::Inventory) {
            if (layout.creativeDeleteSlot().contains(framebufferCursor.x,
                                                     framebufferCursor.y)) {
                gameSession.inventory().clearCursorStack();
                return;
            }
            for (std::size_t index = 0; index < gameplay::Inventory::kSlotCount; ++index) {
                if (layout.creativeInventorySlot(index).contains(framebufferCursor.x,
                                                                 framebufferCursor.y)) {
                    gameSession.inventory().clickSlot(index, button, shiftHeld);
                    return;
                }
            }
            if (!layout.creativePanel().contains(framebufferCursor.x, framebufferCursor.y)) {
                spawnDroppedStack(gameSession.inventory().takeCursorStack());
            }
            return;
        }
        const auto catalog = activeCreativeCatalog();
        const std::size_t firstCatalogIndex = menuSystem.creativeScrollRow * 9U;
        for (std::size_t visibleIndex = 0; visibleIndex < ui::HudLayout::kCreativeVisibleSlots;
             ++visibleIndex) {
            const std::size_t catalogIndex = firstCatalogIndex + visibleIndex;
            if (layout.creativeSlot(visibleIndex)
                    .contains(framebufferCursor.x, framebufferCursor.y)) {
                if (catalogIndex >= catalog.size()) {
                    // Empty cells in the creative catalogue are delete
                    // targets. Only clicks outside the panel create an
                    // item entity, matching the vanilla container.
                    gameSession.inventory().clearCursorStack();
                    return;
                }
                gameSession.inventory().clickCreativeItem(catalog[catalogIndex], button, shiftHeld);
                return;
            }
        }
        for (std::size_t index = 0; index < gameplay::Inventory::kHotbarSize; ++index) {
            if (layout.creativeHotbarSlot(index).contains(framebufferCursor.x,
                                                          framebufferCursor.y)) {
                gameSession.inventory().clickSlot(index, button, shiftHeld);
                return;
            }
        }
        if (!layout.creativePanel().contains(framebufferCursor.x, framebufferCursor.y)) {
            spawnDroppedStack(gameSession.inventory().takeCursorStack());
        }
        return;
    }
    for (std::size_t index = 0; index < gameplay::Inventory::kSlotCount; ++index) {
        if (layout.inventorySlot(index).contains(framebufferCursor.x, framebufferCursor.y)) {
            activeInventory().clickSlot(index, button, shiftHeld);
            return;
        }
    }
    if (!layout.inventoryPanel().contains(framebufferCursor.x, framebufferCursor.y)) {
        spawnDroppedStack(gameSession.inventory().takeCursorStack());
    }

    }

    // SlotActionType.QUICK_CRAFT press. Vanilla splits the click into two
    // phases: a press with an EMPTY cursor acts immediately (PICKUP, QUICK_MOVE
    // or a creative click), while a press with a full cursor only begins a
    // drag — the stack stays on the cursor until release distributes it across
    // the swept slots or, with no movement, places it into the released slot.
    void handleInventoryClick(gameplay::InventoryMouseButton button, bool shiftHeld) {
        // HandledScreen#mouseClicked tracks the last slot/button/time so a
        // second press of the same slot within 250 ms is a double-click, which
        // the release turns into a PICKUP_ALL gather.
        const double now = glfwGetTime();
        gameplay::ItemStack* clickedSlot = slotUnderCursor();
        isDoubleClicking = button == gameplay::InventoryMouseButton::Left &&
            clickedSlot != nullptr && clickedSlot == lastClickedSlot &&
            now - lastClickTime < 0.25;
        lastClickedSlot = clickedSlot;
        lastClickTime = now;
        if (gameSession.inventory().cursorStack().empty() ||
            immediateCreativeControlUnderCursor()) {
            dispatchInventoryClick(button, shiftHeld);
            cancelNextInventoryRelease = true;
            inventoryDragActive = false;
            inventoryDragSlots.clear();
            return;
        }
        inventoryDragActive = true;
        inventoryDragButton = button;
        inventoryDragSlots.clear();
    }

    // Creative catalogue cells and controls act on the press itself: a catalogue
    // click creates/adjusts the cursor stack, tabs switch immediately, the delete
    // slot clears immediately, and the scrollbar starts its own drag. Real item
    // slots are deliberately excluded so QUICK_CRAFT and PICKUP_ALL use the same
    // state machine in both game modes and in creative-opened containers.
    [[nodiscard]] bool immediateCreativeControlUnderCursor() const {
        if (gameSession.gameMode() != gameplay::GameMode::Creative ||
            containerScreen != ContainerScreen::PlayerInventory) {
            return false;
        }
        int framebufferWidth = 0;
        int framebufferHeight = 0;
        glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
        if (framebufferWidth <= 0 || framebufferHeight <= 0) {
            return false;
        }
        const ui::HudLayout layout{static_cast<float>(framebufferWidth),
                                   static_cast<float>(framebufferHeight),
                                   menuSystem.guiScaleSetting};
        const auto cursor = currentFramebufferCursor();
        for (std::size_t tabIndex = 0; tabIndex < kCreativeTabCount; ++tabIndex) {
            if (layout.creativeTab(tabIndex).contains(cursor.x, cursor.y)) {
                return true;
            }
        }
        if (menuSystem.creativeTab == ui::CreativeTab::Inventory) {
            return layout.creativeDeleteSlot().contains(cursor.x, cursor.y);
        }
        if (layout.creativeScrollbarTrack().contains(cursor.x, cursor.y)) {
            return true;
        }
        for (std::size_t index = 0; index < ui::HudLayout::kCreativeVisibleSlots; ++index) {
            if (layout.creativeSlot(index).contains(cursor.x, cursor.y)) {
                return true;
            }
        }
        return false;
    }

    // The exact storage of the slot under the mouse, for double-click tracking.
    [[nodiscard]] gameplay::ItemStack* slotUnderCursor() {
        double cursorX = 0.0;
        double cursorY = 0.0;
        int windowWidth = 0;
        int windowHeight = 0;
        int framebufferWidth = 0;
        int framebufferHeight = 0;
        glfwGetCursorPos(window, &cursorX, &cursorY);
        glfwGetWindowSize(window, &windowWidth, &windowHeight);
        glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
        if (windowWidth <= 0 || windowHeight <= 0) {
            return nullptr;
        }
        const auto cursor = ui::windowToFramebuffer(
            cursorX, cursorY, windowWidth, windowHeight, framebufferWidth, framebufferHeight);
        const ui::HudLayout layout{static_cast<float>(framebufferWidth),
                                   static_cast<float>(framebufferHeight), menuSystem.guiScaleSetting};
        return dragSlotAt(layout, cursor);
    }

    // Every slot the gameSession.player() can reach in the current screen — the container's
    // input slots plus the full gameSession.player() gameSession.inventory() — for the PICKUP_ALL gather.
    [[nodiscard]] std::vector<gameplay::ItemStack*> allScreenSlots() {
        std::vector<gameplay::ItemStack*> slots;
        if (containerScreen == ContainerScreen::Chest && activeChest.has_value()) {
            if (auto* chest = gameSession.chestSystem().find(*activeChest); chest != nullptr) {
                for (auto& item : chest->items) {
                    slots.push_back(&item);
                }
            }
        } else if (containerScreen == ContainerScreen::CraftingTable) {
            for (std::size_t index = 0; index < 9U; ++index) {
                slots.push_back(&gameSession.craftingSystem().tableGridSlot(index));
            }
        } else if (containerScreen == ContainerScreen::Furnace) {
            slots.push_back(&gameSession.craftingSystem().furnaceInputRef());
            slots.push_back(&gameSession.craftingSystem().furnaceFuelRef());
        } else if (gameSession.gameMode() == gameplay::GameMode::Survival) {
            for (std::size_t index = 0; index < 4U; ++index) {
                slots.push_back(&gameSession.craftingSystem().playerGridSlot(index));
            }
        }
        for (std::size_t index = 0; index < gameplay::Inventory::kSlotCount; ++index) {
            slots.push_back(&gameSession.inventory().mutableSlot(index));
        }
        return slots;
    }

    // QUICK_MOVE's gameSession.inventory() direction for a gameSession.player() slot while a container is
    // open: hand the stack to the open container, which decides where it goes.
    void moveInventorySlotToContainer(std::size_t index) {
        gameplay::ItemStack& stack = gameSession.inventory().mutableSlot(index);
        switch (containerScreen) {
        case ContainerScreen::Chest:
            if (activeChest.has_value()) {
                static_cast<void>(gameSession.chestSystem().moveInto(*activeChest, stack));
            }
            break;
        case ContainerScreen::CraftingTable:
            static_cast<void>(gameSession.craftingSystem().moveTableInto(stack));
            break;
        case ContainerScreen::Furnace:
            static_cast<void>(gameSession.craftingSystem().moveFurnaceInto(stack));
            break;
        case ContainerScreen::PlayerInventory:
            break;
        }
    }

    // The exact storage of the slot under the cursor in the current container
    // context, or nullptr when there is none (a blank area, an output slot that
    // cannot take items, or a creative tab). QUICK_CRAFT collects these.
    [[nodiscard]] gameplay::ItemStack* dragSlotAt(
        const ui::HudLayout& layout,
        const ui::UiPoint& cursor) {
        if (containerScreen == ContainerScreen::Chest && activeChest.has_value()) {
            auto* chest = gameSession.chestSystem().find(*activeChest);
            if (chest != nullptr) {
                for (std::size_t index = 0;
                     index < gameplay::ChestBlockEntity::kSlotCount; ++index) {
                    if (layout.chestSlot(index).contains(cursor.x, cursor.y)) {
                        return &chest->items[index];
                    }
                }
            }
        } else if (containerScreen == ContainerScreen::CraftingTable) {
            for (std::size_t index = 0; index < 9U; ++index) {
                if (layout.tableCraftingSlot(index).contains(cursor.x, cursor.y)) {
                    return &gameSession.craftingSystem().tableGridSlot(index);
                }
            }
        } else if (containerScreen == ContainerScreen::Furnace) {
            if (layout.furnaceInputSlot().contains(cursor.x, cursor.y)) {
                return &gameSession.craftingSystem().furnaceInputRef();
            }
            if (layout.furnaceFuelSlot().contains(cursor.x, cursor.y)) {
                return &gameSession.craftingSystem().furnaceFuelRef();
            }
            // The result slot never accepts items, so it is not a drag target.
        } else if (gameSession.gameMode() == gameplay::GameMode::Survival) {
            for (std::size_t index = 0; index < 4U; ++index) {
                if (layout.playerCraftingSlot(index).contains(cursor.x, cursor.y)) {
                    return &gameSession.craftingSystem().playerGridSlot(index);
                }
            }
        }
        const bool creativePlayerScreen =
            containerScreen == ContainerScreen::PlayerInventory &&
            gameSession.gameMode() == gameplay::GameMode::Creative;
        for (std::size_t index = 0; index < gameplay::Inventory::kSlotCount; ++index) {
            if (creativePlayerScreen &&
                menuSystem.creativeTab != ui::CreativeTab::Inventory &&
                index >= gameplay::Inventory::kHotbarSize) {
                continue;
            }
            const auto slot = containerScreen == ContainerScreen::Chest
                                  ? layout.chestInventorySlot(index)
                                  : creativePlayerScreen
                                      ? (menuSystem.creativeTab == ui::CreativeTab::Inventory
                                             ? layout.creativeInventorySlot(index)
                                             : layout.creativeHotbarSlot(index))
                                      : layout.inventorySlot(index);
            if (slot.contains(cursor.x, cursor.y)) {
                return &gameSession.inventory().mutableSlot(index);
            }
        }
        return nullptr;
    }

    // The on-screen rectangle of a slot the cursor swept during a QUICK_CRAFT
    // drag, or nullopt when the pointer no longer belongs to the current screen
    // (a closed container, for example). Pointer equality is the source of truth,
    // matching dragSlotAt's exact storage so the preview always lands on the
    // slot the drag would write.
    [[nodiscard]] std::optional<ui::UiRect> dragSlotRectangle(
        const ui::HudLayout& layout, const gameplay::ItemStack* slot) const {
        if (slot == nullptr) {
            return std::nullopt;
        }
        if (containerScreen == ContainerScreen::Chest && activeChest.has_value()) {
            if (const auto* chest = gameSession.chestSystem().find(*activeChest); chest != nullptr) {
                for (std::size_t index = 0;
                     index < gameplay::ChestBlockEntity::kSlotCount; ++index) {
                    if (&chest->items[index] == slot) {
                        return layout.chestSlot(index);
                    }
                }
            }
        } else if (containerScreen == ContainerScreen::CraftingTable) {
            for (std::size_t index = 0; index < 9U; ++index) {
                if (&gameSession.craftingSystem().tableSlot(index) == slot) {
                    return layout.tableCraftingSlot(index);
                }
            }
        } else if (containerScreen == ContainerScreen::Furnace) {
            if (&gameSession.craftingSystem().furnaceInput() == slot) {
                return layout.furnaceInputSlot();
            }
            if (&gameSession.craftingSystem().furnaceFuel() == slot) {
                return layout.furnaceFuelSlot();
            }
        } else if (gameSession.gameMode() == gameplay::GameMode::Survival) {
            for (std::size_t index = 0; index < 4U; ++index) {
                if (&gameSession.craftingSystem().playerSlot(index) == slot) {
                    return layout.playerCraftingSlot(index);
                }
            }
        }
        const bool creativePlayerScreen =
            containerScreen == ContainerScreen::PlayerInventory &&
            gameSession.gameMode() == gameplay::GameMode::Creative;
        for (std::size_t index = 0; index < gameplay::Inventory::kSlotCount; ++index) {
            if (creativePlayerScreen &&
                menuSystem.creativeTab != ui::CreativeTab::Inventory &&
                index >= gameplay::Inventory::kHotbarSize) {
                continue;
            }
            const auto rect = containerScreen == ContainerScreen::Chest
                                  ? layout.chestInventorySlot(index)
                                  : creativePlayerScreen
                                      ? (menuSystem.creativeTab == ui::CreativeTab::Inventory
                                             ? layout.creativeInventorySlot(index)
                                             : layout.creativeHotbarSlot(index))
                                      : layout.inventorySlot(index);
            if (&gameSession.inventory().slot(index) == slot) {
                return rect;
            }
        }
        return std::nullopt;
    }

    // The amount the ongoing drag would place in each collected slot, mirroring
    // Inventory::dragDistribute exactly: a left drag shares the cursor stack as
    // evenly as the accepting slots allow, a right drag drops one item per slot.
    // Zero marks a collected slot that cannot take the dragged item (its stack
    // is full or a different item), which the preview skips just like the real
    // distribution does.
    [[nodiscard]] std::vector<std::uint8_t> dragPlacementCounts() const {
        std::vector<std::uint8_t> counts(inventoryDragSlots.size(), 0U);
        const auto& cursor = gameSession.inventory().cursorStack();
        if (cursor.empty() || inventoryDragSlots.empty()) {
            return counts;
        }
        const auto accepts = [&cursor](const gameplay::ItemStack* target) {
            return target->empty() ||
                (gameplay::sameItem(*target, cursor) &&
                 target->count < gameplay::itemMaximumStackSize(*target));
        };
        if (inventoryDragButton == gameplay::InventoryMouseButton::Right) {
            for (std::size_t index = 0; index < inventoryDragSlots.size(); ++index) {
                if (accepts(inventoryDragSlots[index])) {
                    counts[index] = 1U;
                }
            }
            return counts;
        }
        std::size_t fillable = 0;
        for (const gameplay::ItemStack* target : inventoryDragSlots) {
            if (accepts(target)) ++fillable;
        }
        if (fillable == 0U) {
            return counts;
        }
        const auto maximum = gameplay::itemMaximumStackSize(cursor);
        std::uint8_t perSlot = static_cast<std::uint8_t>(cursor.count / fillable);
        std::uint8_t extra = static_cast<std::uint8_t>(cursor.count % fillable);
        for (std::size_t index = 0; index < inventoryDragSlots.size(); ++index) {
            gameplay::ItemStack* target = inventoryDragSlots[index];
            if (!accepts(target)) continue;
            std::uint8_t amount = perSlot;
            if (extra > 0U) {
                ++amount;
                --extra;
            }
            amount = std::min(amount, static_cast<std::uint8_t>(maximum - target->count));
            counts[index] = amount;
        }
        return counts;
    }

    // Draws the would-be placement of an in-progress QUICK_CRAFT drag in the
    // slots the cursor has swept, so the destination is visible before the
    // button is released. Vanilla's HandledScreen.drawSlot tints the swept slot
    // dark and paints a copy of the cursor stack with the amount that would land
    // there; this reproduces that preview on top of the already-drawn slots.
    void drawDragPreview(VkCommandBuffer commandBuffer, const ui::HudLayout& layout) const {
        if (!inventoryDragActive || gameSession.inventory().cursorStack().empty()) {
            return;
        }
        const auto counts = dragPlacementCounts();
        for (std::size_t index = 0; index < inventoryDragSlots.size(); ++index) {
            const auto rect = dragSlotRectangle(layout, inventoryDragSlots[index]);
            if (!rect.has_value() || counts[index] == 0U) {
                continue;
            }
            drawHudQuad(commandBuffer, *rect, {0.0F, 0.0F, 0.0F, 0.5F});
            drawHudItemIcon(commandBuffer, *rect, gameSession.inventory().cursorStack());
            const std::string count = std::to_string(counts[index]);
            const float textScale = layout.scale();
            drawHudText(commandBuffer, count,
                        rect->x + 17.0F * textScale - hudTextWidth(count, textScale),
                        rect->y + 9.0F * textScale, textScale, {1.0F, 1.0F, 1.0F, 1.0F});
        }
    }

    void handleInventoryButtonRelease() {
        creativeScrollbarDragging = false;
        if (cancelNextInventoryRelease) {
            // A press that already acted (a pickup or quick-move from an empty
            // cursor) leaves the release with nothing to do; the double-click
            // flag is discarded so a later release cannot act on it.
            cancelNextInventoryRelease = false;
            isDoubleClicking = false;
            return;
        }
        if (isDoubleClicking) {
            // SlotActionType.PICKUP_ALL: double-clicking a slot gathers every
            // matching stack in the screen into the cursor, like vanilla. The
            // second press already began a drag, so clear that state too.
            auto sources = allScreenSlots();
            gameSession.inventory().gatherAllIntoCursor(sources);
            isDoubleClicking = false;
            inventoryDragActive = false;
            inventoryDragSlots.clear();
            return;
        }
        if (!inventoryDragActive) {
            return;
        }
        if (!inventoryDragSlots.empty()) {
            // The drag swept real slots: QUICK_CRAFT distributes the cursor
            // stack across them (left = evenly, right = one per slot).
            gameSession.inventory().dragDistribute(inventoryDragSlots, inventoryDragButton);
        } else {
            // No movement: a plain press-release places the cursor stack into
            // the released slot (vanilla's PICKUP on release). A full-cursor
            // press began a drag, never a quick-move, so shift is irrelevant.
            dispatchInventoryClick(inventoryDragButton, false);
        }
        inventoryDragActive = false;
        inventoryDragSlots.clear();
    }

    // QUICK_CRAFT's add-slot pass: resolve the slot under the cursor and add it
    // to the drag set, once, while the button stays held. Vanilla only collects
    // a slot while the cursor holds more items than slots already collected, so
    // a drag can never ask for more than the stack it carries.
    void collectInventoryDragSlot(double windowX, double windowY) {
        if (!inventoryDragActive) {
            return;
        }
        int windowWidth = 0;
        int windowHeight = 0;
        int framebufferWidth = 0;
        int framebufferHeight = 0;
        glfwGetWindowSize(window, &windowWidth, &windowHeight);
        glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
        if (windowWidth <= 0 || windowHeight <= 0) {
            return;
        }
        const auto cursor = ui::windowToFramebuffer(
            windowX, windowY, windowWidth, windowHeight, framebufferWidth, framebufferHeight);
        const ui::HudLayout layout{static_cast<float>(framebufferWidth),
                                   static_cast<float>(framebufferHeight), menuSystem.guiScaleSetting};
        gameplay::ItemStack* slot = dragSlotAt(layout, cursor);
        if (slot == nullptr) {
            return;
        }
        if (static_cast<std::size_t>(gameSession.inventory().cursorStack().count) <=
            inventoryDragSlots.size()) {
            return;
        }
        if (std::ranges::find(inventoryDragSlots, slot) == inventoryDragSlots.end()) {
            inventoryDragSlots.push_back(slot);
        }
    }

    void shutdown() noexcept {
        if (device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device);
            cleanupSwapchain();
            if (descriptorPool != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(device, descriptorPool, nullptr);
            }
            if (textureSampler != VK_NULL_HANDLE) {
                vkDestroySampler(device, textureSampler, nullptr);
            }
            if (textureView != VK_NULL_HANDLE) {
                vkDestroyImageView(device, textureView, nullptr);
            }
            if (fontTextureView != VK_NULL_HANDLE) {
                vkDestroyImageView(device, fontTextureView, nullptr);
            }
            if (guiTextureView != VK_NULL_HANDLE) {
                vkDestroyImageView(device, guiTextureView, nullptr);
            }
            if (entityTextureView != VK_NULL_HANDLE) {
                vkDestroyImageView(device, entityTextureView, nullptr);
            }
            if (panoramaTextureView != VK_NULL_HANDLE) {
                vkDestroyImageView(device, panoramaTextureView, nullptr);
            }
            if (panoramaSampler != VK_NULL_HANDLE) {
                vkDestroySampler(device, panoramaSampler, nullptr);
            }
            if (biomeGrassView != VK_NULL_HANDLE) {
                vkDestroyImageView(device, biomeGrassView, nullptr);
            }
            if (biomeFoliageView != VK_NULL_HANDLE) {
                vkDestroyImageView(device, biomeFoliageView, nullptr);
            }
            if (biomeSampler != VK_NULL_HANDLE) {
                vkDestroySampler(device, biomeSampler, nullptr);
            }
            if (allocator != VK_NULL_HANDLE) {
                destroyImage(textureImage);
                destroyImage(fontTextureImage);
                destroyImage(guiTextureImage);
                destroyImage(entityTextureImage);
                destroyImage(panoramaTextureImage);
                destroyImage(biomeGrassImage);
                destroyImage(biomeFoliageImage);
                for (auto& [position, mesh] : gpuMeshes) {
                    static_cast<void>(position);
                    destroyBuffer(mesh.indexBuffer);
                    destroyBuffer(mesh.vertexBuffer);
                }
                const auto destroyStreamPool = [this](StreamBufferPool& pool) {
                    for (auto& slot : pool.deferred) {
                        for (auto& buffer : slot) {
                            destroyBuffer(buffer);
                        }
                        slot.clear();
                    }
                    for (auto& freeList : pool.freeByClass) {
                        for (auto& buffer : freeList) {
                            destroyBuffer(buffer);
                        }
                        freeList.clear();
                    }
                };
                destroyStreamPool(deviceBufferPool_);
                destroyStreamPool(stagingBufferPool_);
                for (auto& frame : frames) {
                    for (auto& buffer : frame.retiredBuffers) {
                        destroyBuffer(buffer);
                    }
                    frame.retiredBuffers.clear();
                    destroyBuffer(frame.uniformBuffer);
                }
                gpuSceneBuffer.destroy();
                shadowTarget.destroy();
            }
            for (auto& frame : frames) {
                if (frame.imageAvailable != VK_NULL_HANDLE) {
                    vkDestroySemaphore(device, frame.imageAvailable, nullptr);
                }
                if (frame.inFlight != VK_NULL_HANDLE) {
                    vkDestroyFence(device, frame.inFlight, nullptr);
                }
            }
            if (descriptorSetLayout != VK_NULL_HANDLE) {
                vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
            }
            if (sceneDescriptorSetLayout != VK_NULL_HANDLE) {
                vkDestroyDescriptorSetLayout(device, sceneDescriptorSetLayout, nullptr);
            }
            if (sceneDescriptorPool != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(device, sceneDescriptorPool, nullptr);
            }
            if (shadowDebugSetLayout != VK_NULL_HANDLE) {
                vkDestroyDescriptorSetLayout(device, shadowDebugSetLayout, nullptr);
            }
            if (shadowDebugPool != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(device, shadowDebugPool, nullptr);
            }
            if (shadowDebugSampler != VK_NULL_HANDLE) {
                vkDestroySampler(device, shadowDebugSampler, nullptr);
            }
            if (shadowDebugPipelineLayout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(device, shadowDebugPipelineLayout, nullptr);
            }
            if (shadowPipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(device, shadowPipeline, nullptr);
            }
            if (shadowPipelineLayout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(device, shadowPipelineLayout, nullptr);
            }
            if (occlusionQueryPool != VK_NULL_HANDLE) {
                vkDestroyQueryPool(device, occlusionQueryPool, nullptr);
            }
            if (occlusionQueryLayout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(device, occlusionQueryLayout, nullptr);
            }
            if (occlusionBoxVertexBuffer.buffer != VK_NULL_HANDLE) {
                destroyBuffer(occlusionBoxVertexBuffer);
            }
            if (occlusionBoxIndexBuffer.buffer != VK_NULL_HANDLE) {
                destroyBuffer(occlusionBoxIndexBuffer);
            }
            if (commandPool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(device, commandPool, nullptr);
            }
            if (allocator != VK_NULL_HANDLE) {
                vmaDestroyAllocator(allocator);
                allocator = VK_NULL_HANDLE;
            }
            vkDestroyDevice(device, nullptr);
            device = VK_NULL_HANDLE;
        }
        if (surface != VK_NULL_HANDLE && instance != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance, surface, nullptr);
        }
        destroyDebugMessenger();
        if (instance != VK_NULL_HANDLE) {
            vkDestroyInstance(instance, nullptr);
        }
        if (window != nullptr) {
            glfwDestroyWindow(window);
        }
        if (glfwInitialized) {
            glfwTerminate();
        }
    }

    [[nodiscard]] bool instanceExtensionAvailable(const char* wanted) const {
        std::uint32_t count = 0;
        checkVk(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr),
                "vkEnumerateInstanceExtensionProperties");
        std::vector<VkExtensionProperties> extensions(count);
        checkVk(vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()),
                "vkEnumerateInstanceExtensionProperties");
        return std::ranges::any_of(extensions, [wanted](const auto& extension) {
            return std::strcmp(extension.extensionName, wanted) == 0;
        });
    }

    [[nodiscard]] bool validationLayerAvailable() const {
        std::uint32_t count = 0;
        checkVk(vkEnumerateInstanceLayerProperties(&count, nullptr),
                "vkEnumerateInstanceLayerProperties");
        std::vector<VkLayerProperties> layers(count);
        checkVk(vkEnumerateInstanceLayerProperties(&count, layers.data()),
                "vkEnumerateInstanceLayerProperties");
        return std::ranges::any_of(layers, [](const auto& layer) {
            return std::strcmp(layer.layerName, kValidationLayer) == 0;
        });
    }

    void createDebugMessenger() {
        if (!validationEnabled) {
            return;
        }
        const auto function = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        if (function == nullptr) {
            throw std::runtime_error("VK_EXT_debug_utils is enabled but unavailable");
        }
        const auto info = debugMessengerInfo();
        checkVk(function(instance, &info, nullptr, &debugMessenger),
                "vkCreateDebugUtilsMessengerEXT");
    }

    void destroyDebugMessenger() noexcept {
        if (debugMessenger == VK_NULL_HANDLE || instance == VK_NULL_HANDLE) {
            return;
        }
        const auto function = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (function != nullptr) {
            function(instance, debugMessenger, nullptr);
        }
        debugMessenger = VK_NULL_HANDLE;
    }

    void createInstance() {
        if (glfwVulkanSupported() != GLFW_TRUE) {
            throw std::runtime_error("GLFW could not find a Vulkan loader");
        }
        auto appInfo = vkStructure<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
        appInfo.pApplicationName = "MC Rebedrock";
        appInfo.applicationVersion = VK_MAKE_VERSION(0, 2, 0);
        appInfo.pEngineName = "MC Rebedrock";
        appInfo.engineVersion = VK_MAKE_VERSION(0, 2, 0);
        appInfo.apiVersion = VK_API_VERSION_1_2;

        std::uint32_t extensionCount = 0;
        const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&extensionCount);
        if (glfwExtensions == nullptr) {
            throw std::runtime_error("GLFW did not provide Vulkan surface extensions");
        }
        std::vector<const char*> extensions(glfwExtensions, glfwExtensions + extensionCount);
        VkInstanceCreateFlags flags = 0;
        if (instanceExtensionAvailable(kPortabilityEnumeration)) {
            extensions.push_back(kPortabilityEnumeration);
            flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
        validationEnabled = kRequestValidation && validationLayerAvailable();
        if (kRequestValidation && !validationEnabled) {
            std::cerr << "Warning: VK_LAYER_KHRONOS_validation is not installed.\n";
        }
        if (validationEnabled) {
            if (!instanceExtensionAvailable(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
                throw std::runtime_error("Validation requested but VK_EXT_debug_utils is missing");
            }
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
        auto createInfo = vkStructure<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
        createInfo.flags = flags;
        createInfo.pApplicationInfo = &appInfo;
        createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();
        const char* validationLayers[]{kValidationLayer};
        auto messengerInfo = debugMessengerInfo();
        if (validationEnabled) {
            createInfo.enabledLayerCount = 1;
            createInfo.ppEnabledLayerNames = validationLayers;
            createInfo.pNext = &messengerInfo;
        }
        checkVk(vkCreateInstance(&createInfo, nullptr, &instance), "vkCreateInstance");
        createDebugMessenger();
        std::cout << "Vulkan validation: " << (validationEnabled ? "enabled" : "disabled") << '\n';
    }

    [[nodiscard]] QueueFamilyIndices findQueueFamilies(VkPhysicalDevice candidate) const {
        std::uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, families.data());
        QueueFamilyIndices indices;
        for (std::uint32_t index = 0; index < count; ++index) {
            if ((families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U) {
                indices.graphics = index;
            }
            VkBool32 presentSupported = VK_FALSE;
            checkVk(
                vkGetPhysicalDeviceSurfaceSupportKHR(candidate, index, surface, &presentSupported),
                "vkGetPhysicalDeviceSurfaceSupportKHR");
            if (presentSupported == VK_TRUE) {
                indices.present = index;
            }
            if (indices.complete()) {
                break;
            }
        }
        return indices;
    }

    [[nodiscard]] std::vector<VkExtensionProperties>
    deviceExtensions(VkPhysicalDevice candidate) const {
        std::uint32_t count = 0;
        checkVk(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &count, nullptr),
                "vkEnumerateDeviceExtensionProperties");
        std::vector<VkExtensionProperties> extensions(count);
        checkVk(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &count, extensions.data()),
                "vkEnumerateDeviceExtensionProperties");
        return extensions;
    }

    [[nodiscard]] bool hasDeviceExtension(const std::vector<VkExtensionProperties>& extensions,
                                          const char* wanted) const {
        return std::ranges::any_of(extensions, [wanted](const auto& extension) {
            return std::strcmp(extension.extensionName, wanted) == 0;
        });
    }

    [[nodiscard]] SwapchainSupport querySwapchain(VkPhysicalDevice candidate) const {
        SwapchainSupport support;
        checkVk(
            vkGetPhysicalDeviceSurfaceCapabilitiesKHR(candidate, surface, &support.capabilities),
            "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        std::uint32_t formatCount = 0;
        checkVk(vkGetPhysicalDeviceSurfaceFormatsKHR(candidate, surface, &formatCount, nullptr),
                "vkGetPhysicalDeviceSurfaceFormatsKHR");
        support.formats.resize(formatCount);
        if (formatCount > 0U) {
            checkVk(vkGetPhysicalDeviceSurfaceFormatsKHR(candidate, surface, &formatCount,
                                                         support.formats.data()),
                    "vkGetPhysicalDeviceSurfaceFormatsKHR");
        }
        std::uint32_t presentCount = 0;
        checkVk(
            vkGetPhysicalDeviceSurfacePresentModesKHR(candidate, surface, &presentCount, nullptr),
            "vkGetPhysicalDeviceSurfacePresentModesKHR");
        support.presentModes.resize(presentCount);
        if (presentCount > 0U) {
            checkVk(vkGetPhysicalDeviceSurfacePresentModesKHR(candidate, surface, &presentCount,
                                                              support.presentModes.data()),
                    "vkGetPhysicalDeviceSurfacePresentModesKHR");
        }
        return support;
    }

    [[nodiscard]] bool suitable(VkPhysicalDevice candidate) const {
        const auto indices = findQueueFamilies(candidate);
        const auto extensions = deviceExtensions(candidate);
        if (!indices.complete() ||
            !hasDeviceExtension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            return false;
        }
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        if (VK_VERSION_MAJOR(properties.apiVersion) < 1U ||
            (VK_VERSION_MAJOR(properties.apiVersion) == 1U &&
             VK_VERSION_MINOR(properties.apiVersion) < 2U)) {
            return false;
        }
        const auto support = querySwapchain(candidate);
        return !support.formats.empty() && !support.presentModes.empty();
    }

    void pickPhysicalDevice() {
        std::uint32_t count = 0;
        checkVk(vkEnumeratePhysicalDevices(instance, &count, nullptr),
                "vkEnumeratePhysicalDevices");
        std::vector<VkPhysicalDevice> candidates(count);
        checkVk(vkEnumeratePhysicalDevices(instance, &count, candidates.data()),
                "vkEnumeratePhysicalDevices");
        int bestScore = std::numeric_limits<int>::min();
        for (const auto candidate : candidates) {
            if (!suitable(candidate)) {
                continue;
            }
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(candidate, &properties);
            int score = static_cast<int>(properties.limits.maxImageDimension2D);
            if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                score += 100'000;
            }
            if (score > bestScore) {
                bestScore = score;
                physicalDevice = candidate;
            }
        }
        if (physicalDevice == VK_NULL_HANDLE) {
            throw std::runtime_error("No Vulkan 1.2 GPU supports the required swapchain features");
        }
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physicalDevice, &properties);
        VkPhysicalDeviceFeatures supportedFeatures{};
        vkGetPhysicalDeviceFeatures(physicalDevice, &supportedFeatures);
        samplerAnisotropySupported = supportedFeatures.samplerAnisotropy == VK_TRUE;
        maximumSamplerAnisotropy = properties.limits.maxSamplerAnisotropy;
        const auto supportedSamples = properties.limits.framebufferColorSampleCounts &
                                      properties.limits.framebufferDepthSampleCounts;
        // Capped at 2x: this MoltenVK build does not map transient attachments
        // to on-tile memory, so 4x MSAA on the 2x-resolution framebuffer costs
        // ~425 MB of real DRAM for no measurable fill win (the renderer is
        // vertex-bound, never fill-bound). 2x keeps the silhouette smoothing at
        // roughly half that memory cost.
        maximumMsaaSamples = (supportedSamples & VK_SAMPLE_COUNT_2_BIT) != 0U
            ? VK_SAMPLE_COUNT_2_BIT
            : VK_SAMPLE_COUNT_1_BIT;
        std::cout << "Vulkan GPU: " << properties.deviceName << '\n';
    }

    void createLogicalDevice() {
        queueFamilies = findQueueFamilies(physicalDevice);
        const std::set<std::uint32_t> uniqueFamilies{queueFamilies.graphics.value(),
                                                     queueFamilies.present.value()};
        constexpr float priority = 1.0F;
        std::vector<VkDeviceQueueCreateInfo> queueInfos;
        for (const auto family : uniqueFamilies) {
            auto info =
                vkStructure<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
            info.queueFamilyIndex = family;
            info.queueCount = 1;
            info.pQueuePriorities = &priority;
            queueInfos.push_back(info);
        }
        std::vector<const char*> extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        const auto available = deviceExtensions(physicalDevice);
        if (hasDeviceExtension(available, kPortabilitySubset)) {
            extensions.push_back(kPortabilitySubset);
        }
        VkPhysicalDeviceFeatures features{};
        features.samplerAnisotropy = samplerAnisotropySupported ? VK_TRUE : VK_FALSE;
        // MoltenVK's approximate occlusion queries return a bogus count of ~1,
        // which culls every visible section; precise queries report the real
        // sample count, so the occlusion test can tell visible from buried.
        features.occlusionQueryPrecise = VK_TRUE;
        auto createInfo = vkStructure<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
        createInfo.queueCreateInfoCount = static_cast<std::uint32_t>(queueInfos.size());
        createInfo.pQueueCreateInfos = queueInfos.data();
        createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();
        createInfo.pEnabledFeatures = &features;
        checkVk(vkCreateDevice(physicalDevice, &createInfo, nullptr, &device), "vkCreateDevice");
        vkGetDeviceQueue(device, queueFamilies.graphics.value(), 0, &graphicsQueue);
        vkGetDeviceQueue(device, queueFamilies.present.value(), 0, &presentQueue);
    }

    void createAllocator() {
        VmaAllocatorCreateInfo info{};
        info.instance = instance;
        info.physicalDevice = physicalDevice;
        info.device = device;
        info.vulkanApiVersion = VK_API_VERSION_1_2;
        // The default 256 MB block is far larger than any single allocation this
        // renderer makes, so oversized blocks end up mostly slack and are never
        // returned. A 32 MB block keeps the pool's granularity near the working
        // set and lets emptied blocks be released to the driver.
        info.preferredLargeHeapBlockSize = 32U * 1024U * 1024U;
        checkVk(vmaCreateAllocator(&info, &allocator), "vmaCreateAllocator");
    }

    [[nodiscard]] AllocatedBuffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                               bool hostVisible) const {
        return resources_.createBuffer(size, usage, hostVisible);
    }

    [[nodiscard]] AllocatedImage createImage(std::uint32_t width, std::uint32_t height,
                                             std::uint32_t layers, VkFormat format,
                                             VkImageUsageFlags usage,
                                             VkSampleCountFlagBits samples =
                                                 VK_SAMPLE_COUNT_1_BIT) const {
        return resources_.createImage(width, height, layers, format, usage, samples);
    }

    void destroyBuffer(AllocatedBuffer& buffer) const noexcept { resources_.destroyBuffer(buffer); }

    void destroyImage(AllocatedImage& image) const noexcept { resources_.destroyImage(image); }

    void createCommandPool() {
        auto info =
            vkStructure<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
        info.flags =
            VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        info.queueFamilyIndex = queueFamilies.graphics.value();
        checkVk(vkCreateCommandPool(device, &info, nullptr, &commandPool), "vkCreateCommandPool");
    }

    [[nodiscard]] VkCommandBuffer beginSingleUseCommands() const {
        auto allocateInfo = vkStructure<VkCommandBufferAllocateInfo>(
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
        allocateInfo.commandPool = commandPool;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = 1;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        checkVk(vkAllocateCommandBuffers(device, &allocateInfo, &commandBuffer),
                "vkAllocateCommandBuffers");
        auto beginInfo =
            vkStructure<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        checkVk(vkBeginCommandBuffer(commandBuffer, &beginInfo), "vkBeginCommandBuffer");
        return commandBuffer;
    }

    void endSingleUseCommands(VkCommandBuffer commandBuffer) const {
        checkVk(vkEndCommandBuffer(commandBuffer), "vkEndCommandBuffer");
        auto submitInfo = vkStructure<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        checkVk(vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE), "vkQueueSubmit");
        checkVk(vkQueueWaitIdle(graphicsQueue), "vkQueueWaitIdle");
        vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
    }

    void queueStreamBatch(world::ChunkStreamBatch batch) {
        if (batch.worldEpoch != worldEpoch)
            return;
        loadedCpuChunkCount = batch.loadedChunkCount;
        completedBlockEditCount += batch.appliedBlockEditCount;
        const bool generatedOrUnloadedChunks = batch.appliedBlockEditCount == 0U;
        for (auto& update : batch.chunkUpdates) {
            if (update.remove) {
                interactionWorld.removeChunk(update.position);
            } else if (generatedOrUnloadedChunks) {
                // The gameplay world has already applied local edits and may
                // have advanced several fluid ticks beyond this worker
                // snapshot. Replacing the whole chunk here used to rewind
                // water selectively, producing one-direction flow and stale
                // holes. Only generation batches introduce CPU chunks;
                // edit batches contribute meshes but never overwrite state.
                interactionWorld.setChunk(update.position, std::move(update.chunk));
            }
        }
        for (auto& update : batch.sectionUpdates) {
            const auto latest = latestSectionRevisions.find(update.position);
            if (latest != latestSectionRevisions.end() && update.revision < latest->second) {
                continue;
            }
            latestSectionRevisions.insert_or_assign(update.position, update.revision);
            update.highPriority = batch.highPriority;
            if (!pendingSectionUpdates.contains(update.position)) {
                // Cap the mesh backlog. High-priority updates push_front and
                // repeat positions never re-queue, so order.back() is always the
                // oldest low-priority entry; evict it and roll back its revision
                // so a later batch re-queues the section instead of skipping it
                // as stale.
                if (!update.highPriority &&
                    pendingSectionUpdates.size() >= kMaxPendingSectionUpdates &&
                    !pendingSectionOrder.empty()) {
                    const world::SectionPosition victim = pendingSectionOrder.back();
                    const auto victimFound = pendingSectionUpdates.find(victim);
                    if (victimFound != pendingSectionUpdates.end() &&
                        !victimFound->second.highPriority) {
                        latestSectionRevisions.erase(victim);
                        pendingSectionUpdates.erase(victimFound);
                        pendingSectionOrder.pop_back();
                        // Dropping a section that was queued but never uploaded
                        // would leave a permanent hole once the worker's streaming
                        // backlog drains (the symptom: missing chunks that only
                        // reappear when a placed block forces a remesh). Re-request
                        // a remesh so the section is delivered again after the
                        // backlog clears. Sections already on the GPU only lose a
                        // re-mesh, so they need no re-request.
                        if (!gpuMeshes.contains(victim)) {
                            chunkStreamer.requestSectionRemesh(victim);
                        }
                    }
                }
                // Gameplay edit batches jump ahead of streaming so recent world
                // changes are not stuck behind a queue of distant chunk meshes.
                if (batch.highPriority) {
                    pendingSectionOrder.push_front(update.position);
                } else {
                    pendingSectionOrder.push_back(update.position);
                }
            }
            pendingSectionUpdates.insert_or_assign(update.position, std::move(update));
        }
        peakPendingSectionCount = std::max(peakPendingSectionCount, pendingSectionUpdates.size());
        // std::cout << "Chunk stream center: " << batch.center.x << "," << batch.center.z
        //           << " | CPU chunks: " << batch.loadedChunkCount
        //           << " | queued sections: " << pendingSectionUpdates.size() << '\n';
        ++completedStreamBatchCount;
        if (!spawnPositionInitialized) {
            initializeSpawnPosition();
        }
        if (completedStreamBatchCount == 1U && std::getenv("MC_REBEDROCK_SMOKE_TEST") != nullptr) {
            const glm::vec3 oldPosition = gameSession.player().position();
            gameSession.player().setPosition(glm::vec3{52.284F, oldPosition.y, -4.284F});
            gameSession.physicsPreviousPosition() = gameSession.player().position();
            gameSession.physicsCurrentPosition() = gameSession.player().position();
            camera.setPosition(gameSession.player().eyePosition());
        }
        if (completedStreamBatchCount == 2U && std::getenv("MC_REBEDROCK_SMOKE_TEST") != nullptr) {
            interactionWorld.setBlock(52, 70, -4, world::Block::Glass);
            interactionWorld.setBlock(54, 72, -4, world::Block::Sand);
            interactionWorld.setBlock(50, 70, -4, world::Block::Water);
            interactionWorld.setFluidLevel(50, 70, -4, 0U);
            submitWorldEdit(52, 70, -4, world::Block::Glass);
            submitWorldEdit(54, 72, -4, world::Block::Sand);
            submitWorldEdit(50, 70, -4, world::Block::Water, 0U);
        }
        if (completedStreamBatchCount == 3U && std::getenv("MC_REBEDROCK_SMOKE_TEST") != nullptr) {
            gameSession.worldSimulation().notifyPlaced({54, 72, -4}, world::Block::Sand);
            gameSession.worldSimulation().notifyPlaced({50, 70, -4}, world::Block::Water);
        }
        lastVisibleMeshCount = std::numeric_limits<std::size_t>::max();
    }

    void initializeSpawnPosition() {
        const glm::ivec2 preferred{24, 24};
        std::optional<glm::ivec3> best;
        int bestDistance = std::numeric_limits<int>::max();
        for (int radius = 0; radius <= 8 && !best.has_value(); ++radius) {
            for (int z = preferred.y - radius; z <= preferred.y + radius; ++z) {
                for (int x = preferred.x - radius; x <= preferred.x + radius; ++x) {
                    if (std::max(std::abs(x - preferred.x), std::abs(z - preferred.y)) != radius) {
                        continue;
                    }
                    const auto chunk =
                        world::chunkPositionFromWorld(static_cast<float>(x), static_cast<float>(z));
                    if (!interactionWorld.hasChunk(chunk))
                        continue;
                    for (int y = world::kWorldHeight - 3; y >= 1; --y) {
                        const auto ground = interactionWorld.block(x, y, z);
                        const bool naturalSurface =
                            ground == world::Block::Grass || ground == world::Block::Sand ||
                            ground == world::Block::Dirt || ground == world::Block::Stone ||
                            ground == world::Block::Gravel || ground == world::Block::SnowBlock ||
                            ground == world::Block::CoarseDirt || ground == world::Block::Podzol;
                        // Water blocks nothing, so the two cells the gameSession.player() would
                        // stand in have to be checked for it directly: otherwise
                        // the search happily picks a seabed.
                        const bool submerged =
                            world::isFluid(interactionWorld.block(x, y + 1, z)) ||
                            world::isFluid(interactionWorld.block(x, y + 2, z));
                        if (!naturalSurface || submerged ||
                            world::hasCollision(interactionWorld.block(x, y + 1, z)) ||
                            world::hasCollision(interactionWorld.block(x, y + 2, z))) {
                            continue;
                        }
                        // A cave floor satisfies every check above too — stone
                        // with air overhead — so the candidate must also be
                        // exposed to the sky: any solid block between the spawn
                        // cells and the world top marks this as a cave and the
                        // column is skipped. Vanilla spawns on the heightmap's
                        // top block, which is exactly this surface. (y+1 and y+2
                        // are the cells the 2-tall gameSession.player() occupies; the scan
                        // therefore starts above them.) Leaves are passed over:
                        // an overhanging canopy is still outdoors, unlike a cave
                        // roof, so it must not reject a forest floor column.
                        bool exposedToSky = true;
                        for (int above = y + 3; above < world::kWorldHeight; ++above) {
                            const auto aboveBlock = interactionWorld.block(x, above, z);
                            if (world::hasCollision(aboveBlock) && !world::isLeaves(aboveBlock)) {
                                exposedToSky = false;
                                break;
                            }
                        }
                        if (!exposedToSky) {
                            continue;
                        }
                        const int distance = std::abs(x - preferred.x) + std::abs(z - preferred.y);
                        if (distance < bestDistance) {
                            best = glm::ivec3{x, y + 1, z};
                            bestDistance = distance;
                        }
                        break;
                    }
                }
            }
        }
        if (!best.has_value()) {
            // The strict scan found nothing — a large sea or a fully covered
            // area around the preferred centre. Vanilla's getSpawnPos never
            // gives up, and neither may we: a world whose spawn search returns
            // without a position leaves spawnPositionInitialized false and the
            // loading screen forever, on this run and on every reload of the
            // same seed. Fall back to the highest solid surface at the centre
            // (a seabed is fine — the player can swim up), and if even that
            // fails, to the default feet. Only once the centre chunk is loaded,
            // so the fallback reads real terrain instead of empty air.
            if (!interactionWorld.hasChunk(
                    world::chunkPositionFromWorld(24.0F, 24.0F))) {
                return;
            }
            for (int y = world::kWorldHeight - 3; y >= 1; --y) {
                const auto ground = interactionWorld.block(24, y, 24);
                if (world::hasCollision(ground) || world::isFluid(ground)) {
                    best = glm::ivec3{24, y + 1, 24};
                    break;
                }
            }
            if (!best.has_value()) {
                best = glm::ivec3{24, 1, 24};
            }
        }
        const glm::vec3 feet{static_cast<float>(best->x) + 0.5F,
                             static_cast<float>(best->y) + 0.001F,
                             static_cast<float>(best->z) + 0.5F};
        gameSession.player().setPosition(feet);
        gameSession.worldSpawnPosition() = feet;
        gameSession.physicsPreviousPosition() = feet;
        gameSession.physicsCurrentPosition() = feet;
        camera.setPosition(gameSession.player().eyePosition());
        spawnPositionInitialized = true;
        // Vanilla keeps its spawn chunks loaded for the server's lifetime; mark
        // the world spawn's chunk neighbourhood so it never streams out under
        // the player (ServerChunkManager#updateChunks).
        chunkStreamer.protectChunks(
            world::chunkPositionFromWorld(feet.x, feet.z), kSpawnChunkRadius);
        std::cout << "Spawn position: " << feet.x << "," << feet.y << "," << feet.z << '\n';
    }

    void processChunkStreaming() {
        if (!worldSessionActive)
            return;
        const auto position = camera.position();
        // Look ahead along the movement direction so a fast-flying gameSession.player() never
        // reaches the boundary of the generated world: the request centre leads
        // the gameSession.player() by roughly a second of travel, and the worker generates
        // nearest-first around that leading position. Vanilla never stalls a
        // moving gameSession.player() on terrain because its gameSession.player() tickets already keep the
        // chunks in front of the gameSession.player() generated; this is the client-side
        // equivalent. The lead is capped so the gameSession.player()'s own chunk stays inside
        // the unload radius.
        const glm::vec2 velocity{
            gameSession.physicsCurrentPosition().x - gameSession.physicsPreviousPosition().x,
            gameSession.physicsCurrentPosition().z - gameSession.physicsPreviousPosition().z,
        };
        glm::vec3 requestPosition = position;
        // A gameSession.player() turning (rather than moving) reveals area in the direction
        // they look, so bias the request centre forward by a fraction of the
        // view distance. Skipped while spinning (the forward is unstable) so a
        // rapid pan does not thrash the loaded disk.
        const glm::vec3 forward = camera.direction();
        if (hasLastStreamingForward) {
            constexpr float kSpinGuardRotation = 0.01F;  // ~8°/frame
            if (1.0F - glm::dot(forward, lastStreamingForward) < kSpinGuardRotation) {
                const float maxLead = std::max(
                    0.0F,
                    static_cast<float>(chunkStreamer.loadRadius() * world::kChunkWidth) - 8.0F);
                requestPosition += forward * std::min(renderDistanceBlocks() * 0.4F, maxLead);
            }
        }
        lastStreamingForward = forward;
        hasLastStreamingForward = true;
        const float speed = glm::length(velocity);
        if (speed > 0.001F) {
            const float maxLead = std::max(
                0.0F,
                static_cast<float>(chunkStreamer.loadRadius() * world::kChunkWidth) - 8.0F);
            const float leadBlocks = std::min(speed * 20.0F, maxLead);
            const glm::vec2 direction = velocity / speed;
            requestPosition += glm::vec3{direction.x, 0.0F, direction.y} * leadBlocks;
        }
        chunkStreamer.request(
            world::chunkPositionFromWorld(requestPosition.x, requestPosition.z));
        while (auto batch = chunkStreamer.poll()) {
            queueStreamBatch(std::move(*batch));
        }
    }

    // The gameSession.player()'s movement is never blocked on terrain generation: processChunkStreaming
    // already leads the request centre in the direction of travel, and if the worker ever
    // falls behind, PlayerController's unloaded-column wall simply stops the gameSession.player() in
    // place (a normal collision, not a stall). Blocking the render thread here is what
    // caused the visible hitch at the boundary, so there is deliberately no synchronous
    // wait anymore.

    // Every button click plays the vanilla ui.button.click sound at the
    // listener, so the master-category click is always fully audible. Menu
    // buttons, creative tabs and every gameSession.inventory()/container slot go through this
    // one helper; drags (the two sliders, the creative scrollbar) do not.
    void playUiClick() { audioSystem.playButtonClick(camera.position()); }

    // Rolls a broken block's loot table and drops whatever came out on top of the
    // cell it left behind. Several stacks fan out on the golden angle so they do
    // not stack into one another.
    // ToolItem#postMine / #postHit: spends the held tool's durability and plays
    // the vanilla break sound when it gives out. Creative never wears a tool
    // down, so callers only reach this in survival. `blockHardness` is ignored
    // for an attack; pass the mined block's hardness otherwise, because vanilla
    // charges nothing for a block that gives way instantly.





    // Minecraft#doAttack: one swing, and if the ray reaches a creature before it
    // reaches a block, that creature takes the hit instead. Returns true when a
    // creature was struck, so the caller skips the mining path for this click.
    [[nodiscard]] bool attackTargetedEntity() {
        const auto hit = gameSession.worldEntities().raycast(camera.position(), camera.direction(),
                                               gameplay::EntitySystem::kAttackReach);
        if (!hit.has_value()) {
            return false;
        }
        // The block pick already records the ray distance to the block's entry
        // face, so compare it directly against the creature's box distance rather
        // than the camera-to-centre length — a block the ray actually reaches
        // first still wins over a creature behind it.
        if (targetedBlock.has_value() && targetedBlock->distance < hit->distance) {
            return false;
        }
        heldItemAnimation.trigger(animation::ModelAction::Break);
        // Player#getAttackDamage: one point bare-handed, otherwise the tool's
        // own attack damage. The cooldown-based sweep multiplier is not modelled.
        // Creative deals the same damage as survival — no instant kill; only the
        // durability and exhaustion side effects stay survival-only, matching
        // vanilla's creative mode.
        const auto& weapon = activeInventory().selectedStack();
        const auto attributes =
            gameplay::toolAttributes(gameplay::toolType(weapon), gameplay::toolTier(weapon));
        const float damage =
            gameplay::toolType(weapon) == gameplay::ToolType::None ? 1.0F : attributes.attackDamage;
        if (gameSession.worldEntities().hurt(hit->entityId, damage, camera.position())) {
            if (gameSession.gameMode() == gameplay::GameMode::Survival) {
                // Player#attack adds a flat 0.1 exhaustion per landed hit.
                gameSession.vitals().addExhaustion(0.1F);
                if (gameSession.damageHeldTool(gameplay::ToolUse::AttackEntity, 0.0F)) {
                    audioSystem.playItemBreak(camera.position());
                }
            }
        }
        return true;
    }

    // Drains what the last entity tick produced: the hurt and death sounds, and
    // the loot a finished death left on the ground.

    void updateBlockInteraction() {
        if (!worldSessionActive || !worldReady) {
            targetedBlock.reset();
            breakBlockRequested = false;
            placeBlockRequested = false;
            return;
        }
        // Minecraft#startAttack fires once per click and Minecraft#continueAttack
        // runs every tick the button stays down, so an ongoing dig keeps swinging
        // whether or not the block ever breaks.
        const bool attackActive = breakButtonHeld || breakBlockRequested;
        // Minecraft#handleKeybinds gates the use action on rightClickDelay rather
        // than on the click edge, so holding the button repeats it every 4 ticks.
        const bool useActive = useButtonHeld || placeBlockRequested;
        const auto& selectedStack = activeInventory().selectedStack();
        const bool collectingWater = selectedStack.item == &gameplay::items::Bucket;
        const float blockReach = gameSession.gameMode() == gameplay::GameMode::Creative ? 5.0F : 4.5F;
        targetedBlock = world::raycastVoxels(
            interactionWorld, camera.position(), camera.direction(), blockReach, collectingWater);
        // A creature's collision box blocks the ray exactly like a block's shape
        // (vanilla's HitResult is the nearest of block-or-entity): looking past a
        // mob never reveals — let alone digs or places through — the block behind
        // it. Same reach as the block pick, so a mob just inside the pick range
        // still shields its backdrop. Runs every frame, not just on the click
        // edge, so a held dig cannot keep carving through a creature.
        const auto creatureHit =
            gameSession.worldEntities().raycast(camera.position(), camera.direction(), blockReach);
        if (creatureHit.has_value() &&
            (!targetedBlock.has_value() || creatureHit->distance < targetedBlock->distance)) {
            targetedBlock.reset();
        }
        // A container the click would open wins over a meal; otherwise holding
        // food starts (or keeps) the vanilla 32-tick eat, independently of the
        // 4-tick rightClickDelay. Attacking during a meal cancels it.
        const bool targetedContainer =
            targetedBlock.has_value() &&
            [&] {
                const auto block = interactionWorld.block(targetedBlock->block.x,
                                                          targetedBlock->block.y,
                                                          targetedBlock->block.z);
                return block == world::Block::CraftingTable || block == world::Block::Furnace ||
                       block == world::Block::Chest;
            }();
        // Carrot and potato are both food and seed. Right-clicking farmland with
        // one plants the crop; the plant wins over gameSession.eating(), exactly like vanilla's
        // item useOn running before the food use. The target cell below the
        // placement position is the same farmland check the seed's useOn runs.
        const bool aimsAtPlantableFarmland = [&] {
            if (!targetedBlock.has_value()) {
                return false;
            }
            const auto* held = selectedStack.item;
            if (held == nullptr || gameplay::cropForSeedItem(held) == world::Block::Air) {
                return false;
            }
            const auto interacted =
                interactionWorld.block(targetedBlock->block.x, targetedBlock->block.y,
                                       targetedBlock->block.z);
            const glm::ivec3 placeTarget = world::isReplaceable(interacted)
                ? targetedBlock->block
                : targetedBlock->adjacent;
            const glm::ivec3 below{placeTarget.x, placeTarget.y - 1, placeTarget.z};
            return world::isFarmland(interactionWorld.block(below.x, below.y, below.z));
        }();
        const bool foodInHand = gameplay::isFood(selectedStack.item);
        if (useActive && foodInHand && !targetedContainer && !aimsAtPlantableFarmland &&
            !gameSession.eating()) {
            gameSession.beginEating(selectedStack.item, *this);
        } else if (gameSession.eating() &&
                   (!useActive || !foodInHand || selectedStack.item != gameSession.eatingKind() ||
                    targetedContainer)) {
            gameSession.cancelEating(*this);
        }
        if (attackActive && gameSession.eating()) {
            gameSession.cancelEating(*this);
        }
        // Minecraft#doAttack picks the creature over the block when the ray
        // reaches it first, and an attack is a click edge, not a hold. Hitting a
        // creature cancels the dig for this frame but leaves the use action to
        // its own branch below.
        const bool struckEntity = breakBlockRequested && attackTargetedEntity();
        if (struckEntity) {
            breakBlockRequested = false;
            miningTarget.reset();
            lastMiningSoundAt = -1.0;
        }
        bool performBreak = false;
        if (!struckEntity && targetedBlock.has_value()) {
            if (attackActive) {
                // The swing is driven by the attack itself, not by the block
                // finally giving way; LivingEntity#swing only restarts the arc
                // once it is past halfway, which yields the vanilla cadence.
                heldItemAnimation.trigger(animation::ModelAction::Break);
            }
            if (gameSession.gameMode() == gameplay::GameMode::Creative) {
                // Creative keeps destroying while held, one block per destroyDelay.
                performBreak = attackActive && gameSession.gameTimeSeconds() >= nextCreativeBreakSeconds;
            } else if (breakButtonHeld) {
                if (!miningTarget.has_value() || *miningTarget != targetedBlock->block) {
                    miningTarget = targetedBlock->block;
                    miningStartedAt = gameSession.gameTimeSeconds();
                    lastMiningSoundAt = -1.0;
                }
                // Minecraft#continueAttack evaluates the accumulated damage on the
                // same tick the dig starts, so a zero-hardness block is already gone
                // before any destroy stage can be drawn.
                const auto blockPosition = targetedBlock->block;
                const auto target =
                    interactionWorld.block(blockPosition.x, blockPosition.y, blockPosition.z);
                const float duration = gameplay::miningSeconds(
                    target, selectedStack, gameSession.player().inWater(), !gameSession.player().onGround());
                performBreak = gameSession.gameTimeSeconds() - miningStartedAt >= duration;
                if (!performBreak && miningTarget.has_value() &&
                    (lastMiningSoundAt < 0.0 || gameSession.gameTimeSeconds() - lastMiningSoundAt >= 0.20)) {
                    const auto position = *miningTarget;
                    const auto target = interactionWorld.block(position.x, position.y, position.z);
                    audioSystem.playBlockHit(target, glm::vec3{position} + glm::vec3{0.5F});
                    lastMiningSoundAt = gameSession.gameTimeSeconds();
                }
            } else if (gameSession.gameMode() == gameplay::GameMode::Survival) {
                miningTarget.reset();
                lastMiningSoundAt = -1.0;
            }
        } else if (!struckEntity) {
            miningTarget.reset();
            lastMiningSoundAt = -1.0;
            if (breakBlockRequested) {
                // A click that hits nothing still swings, exactly like vanilla.
                heldItemAnimation.trigger(animation::ModelAction::Break);
            }
        }
        if (performBreak && targetedBlock.has_value()) {
            const auto block = targetedBlock->block;
            const auto brokenBlock = interactionWorld.block(block.x, block.y, block.z);
            // A crop's age lives in its orientation state; read it before the
            // cell is cleared so the loot table rolls against the right stage.
            const auto brokenOrientation =
                interactionWorld.orientation(block.x, block.y, block.z);
            if (!world::isFluid(brokenBlock) &&
                (gameSession.gameMode() == gameplay::GameMode::Creative ||
                 world::blockDefinition(brokenBlock).hardness >= 0.0F) &&
                interactionWorld.setBlock(block.x, block.y, block.z, world::Block::Air)) {
                submitWorldEdit(block.x, block.y, block.z, world::Block::Air);
                previewBlockEdit(block.x, block.y, block.z);
                audioSystem.playBlockBreak(brokenBlock, {static_cast<float>(block.x) + 0.5F,
                                                         static_cast<float>(block.y) + 0.5F,
                                                         static_cast<float>(block.z) + 0.5F});
                particleSystem.spawnBlockBreak({block.x, block.y, block.z}, brokenBlock);
                gameSession.worldSimulation().notifyNeighborChanged(interactionWorld,
                                                      {block.x, block.y, block.z});
                if (brokenBlock == world::Block::Chest) {
                    const auto removed = gameSession.chestSystem().remove({block.x, block.y, block.z});
                    if (removed.has_value()) {
                        std::size_t dropIndex = 0U;
                        for (const auto& stack : removed->items) {
                            if (!stack.empty()) {
                                const float angle = static_cast<float>(dropIndex) * 2.39996323F;
                                gameSession.itemEntities().spawn(
                                    glm::vec3{block} + glm::vec3{0.5F, 0.65F, 0.5F}, stack,
                                    {std::cos(angle) * 0.08F, 0.12F, std::sin(angle) * 0.08F});
                                ++dropIndex;
                            }
                        }
                    }
                }
                if (gameSession.gameMode() == gameplay::GameMode::Survival) {
                    // Player#destroyBlock adds a flat exhaustion per broken block.
                    gameSession.vitals().addExhaustion(0.005F);
                    gameSession.spawnBlockDrops({block.x, block.y, block.z}, brokenBlock, selectedStack,
                                    brokenOrientation);
                    if (gameSession.damageHeldTool(gameplay::ToolUse::BreakBlock,
                                                   world::blockDefinition(brokenBlock).hardness)) {
                        audioSystem.playItemBreak(camera.position());
                    }
                }
                miningTarget.reset();
                miningStartedAt = gameSession.gameTimeSeconds();
                lastMiningSoundAt = -1.0;
                nextCreativeBreakSeconds =
                    gameSession.gameTimeSeconds() + 5.0 * gameplay::PlayerController::kTickSeconds;
            }
        }
        const bool performUse = useActive && gameSession.gameTimeSeconds() >= nextUseSeconds && !gameSession.eating();
        if (performUse) {
            nextUseSeconds = gameSession.gameTimeSeconds() + 4.0 * gameplay::PlayerController::kTickSeconds;
        }
        if (performUse && targetedBlock.has_value()) {
            const auto interactedBlock = interactionWorld.block(
                targetedBlock->block.x, targetedBlock->block.y, targetedBlock->block.z);
            // BlockPlaceContext#getClickedPos: clicking a replaceable block such
            // as tall grass builds into that cell instead of next to it.
            const glm::ivec3 placeTarget = world::isReplaceable(interactedBlock)
                ? targetedBlock->block
                : targetedBlock->adjacent;
            // BlockBehaviour#onBlockUse: the container a clicked block opens is
            // read from its registry entry, never compared by block. Opening one
            // consumes the click, so no item is used against it.
            switch (world::blockDefinition(interactedBlock).container) {
            case world::ContainerType::CraftingTable:
                openContainer(ContainerScreen::CraftingTable);
                break;
            case world::ContainerType::Furnace:
                activeFurnacePosition = targetedBlock->block;
                openContainer(ContainerScreen::Furnace);
                break;
            case world::ContainerType::Chest:
                openChest({targetedBlock->block.x, targetedBlock->block.y,
                           targetedBlock->block.z});
                break;
            default: {
                // Item#useOn: the held item decides what right-clicking does,
                // resolved by its own class instead of a switch in this loop.
                // The item answers with the outcome; the side effects (world
                // edit, audio, animation) are applied below.
                const world::PlacementContext placement{
                    targetedBlock->block,
                    placeTarget,
                    world::orientationFromOffset(targetedBlock->adjacent -
                                                 targetedBlock->block),
                    camera.direction(),
                };
                const gameplay::ItemUseResult use =
                    selectedStack.item != nullptr
                        ? gameplay::itemUseOn(selectedStack.item, interactionWorld, placement)
                        : gameplay::legacyBlockStackUseOn(selectedStack, interactionWorld,
                                                          placement);
                switch (use.action) {
                case gameplay::ItemUseAction::CollectWater: {
                    // BucketItem#use + ItemUsage#method_30012: both modes scoop a
                    // still source into a full bucket. Creative keeps the bucket
                    // forever (the swap below never spends it); survival spends
                    // the empty bucket by turning it into the full one.
                    const auto block = targetedBlock->block;
                    if (interactionWorld.setBlock(block.x, block.y, block.z,
                                                  world::Block::Air)) {
                        submitWorldEdit(block.x, block.y, block.z, world::Block::Air);
                        previewBlockEdit(block.x, block.y, block.z);
                        gameSession.worldSimulation().notifyNeighborChanged(interactionWorld,
                                                              {block.x, block.y, block.z});
                        audioSystem.playSplash({static_cast<float>(block.x) + 0.5F,
                                                static_cast<float>(block.y) + 0.5F,
                                                static_cast<float>(block.z) + 0.5F},
                                               0.5F);
                        particleSystem.spawnWaterSplash({static_cast<float>(block.x) + 0.5F,
                                                         static_cast<float>(block.y) + 0.7F,
                                                         static_cast<float>(block.z) + 0.5F});
                        heldItemAnimation.trigger(animation::ModelAction::Use);
                        // The empty bucket becomes a full water bucket in hand.
                        gameSession.inventory().replaceSelected(
                            {world::Block::Air, 1U, &gameplay::items::WaterBucket});
                    }
                    break;
                }
                case gameplay::ItemUseAction::PlaceWater: {
                    const auto block = placeTarget;
                    if (interactionWorld.setBlock(block.x, block.y, block.z,
                                                  world::Block::Water)) {
                        interactionWorld.setFluidLevel(block.x, block.y, block.z, 0U);
                        submitWorldEdit(block.x, block.y, block.z, world::Block::Water, 0U);
                        previewBlockEdit(block.x, block.y, block.z);
                        gameSession.worldSimulation().notifyPlaced({block.x, block.y, block.z},
                                                     world::Block::Water);
                        gameSession.worldSimulation().notifyNeighborChanged(interactionWorld,
                                                              {block.x, block.y, block.z});
                        audioSystem.playSplash({static_cast<float>(block.x) + 0.5F,
                                                static_cast<float>(block.y) + 0.5F,
                                                static_cast<float>(block.z) + 0.5F});
                        particleSystem.spawnWaterSplash({static_cast<float>(block.x) + 0.5F,
                                                         static_cast<float>(block.y) + 1.0F,
                                                         static_cast<float>(block.z) + 0.5F});
                        heldItemAnimation.trigger(animation::ModelAction::Use);
                        // BucketItem#getEmptiedStack: survival reverts the full
                        // bucket to an empty one; creative keeps pouring without
                        // spending it.
                        if (gameSession.gameMode() == gameplay::GameMode::Survival) {
                            gameSession.inventory().replaceSelected(
                                {world::Block::Air, 1U, &gameplay::items::Bucket});
                        }
                    }
                    break;
                }
                case gameplay::ItemUseAction::SpawnEntity: {
                    // The egg dispatches to its species' EntityType through the
                    // stored supplier; only spawn once that species' model is
                    // loaded so the creature does not appear as a missing mesh.
                    if (const auto* spawnEgg = gameplay::asSpawnEgg(selectedStack.item)) {
                        const auto& eggType = spawnEgg->entityType();
                        const auto block = placeTarget;
                        if (entityModelReady(&eggType)) {
                            gameSession.worldEntities().spawn({static_cast<float>(block.x) + 0.5F,
                                                 static_cast<float>(block.y) + 0.02F,
                                                 static_cast<float>(block.z) + 0.5F},
                                                eggType);
                            heldItemAnimation.trigger(animation::ModelAction::Use);
                            if (gameSession.gameMode() == gameplay::GameMode::Survival) {
                                static_cast<void>(gameSession.inventory().consumeSelected());
                            }
                        }
                    }
                    break;
                }
                case gameplay::ItemUseAction::PlaceBlock: {
                    const auto block = placeTarget;
                    const auto existingBlock = interactionWorld.block(block.x, block.y, block.z);
                    const world::Block placedBlock = use.block;
                    if (world::isRenderable(placedBlock) && world::isReplaceable(existingBlock) &&
                        (!world::hasCollision(placedBlock) ||
                         !gameSession.player().intersectsBlock(block.x, block.y, block.z)) &&
                        interactionWorld.setBlock(block.x, block.y, block.z, placedBlock)) {
                        interactionWorld.setOrientation(block.x, block.y, block.z, use.orientation);
                        submitWorldEdit(block.x, block.y, block.z, placedBlock, 0U,
                                        use.orientation);
                        previewBlockEdit(block.x, block.y, block.z);
                        gameSession.worldSimulation().notifyPlaced({block.x, block.y, block.z}, placedBlock);
                        audioSystem.playBlockPlace(
                            placedBlock, {static_cast<float>(block.x) + 0.5F,
                                          static_cast<float>(block.y) + 0.5F,
                                          static_cast<float>(block.z) + 0.5F});
                        heldItemAnimation.trigger(animation::ModelAction::Use);
                        if (placedBlock == world::Block::Chest) {
                            static_cast<void>(gameSession.chestSystem().place({block.x, block.y, block.z}));
                        }
                        if (gameSession.gameMode() == gameplay::GameMode::Survival) {
                            static_cast<void>(gameSession.inventory().consumeSelected());
                        }
                    }
                    break;
                }
                case gameplay::ItemUseAction::TilGround: {
                    // HoeItem#useOn converts the clicked block in place (dirt and
                    // grass become farmland, coarse dirt becomes dirt again). The
                    // tool is not consumed; in survival it pays one durability.
                    const auto block = targetedBlock->block;
                    const auto existing = interactionWorld.block(block.x, block.y, block.z);
                    const world::Block tilled = use.block;
                    if (world::isRenderable(tilled) && existing != tilled &&
                        interactionWorld.setBlock(block.x, block.y, block.z, tilled)) {
                        interactionWorld.setOrientation(
                            block.x, block.y, block.z, world::farmlandOrientation(0));
                        submitWorldEdit(block.x, block.y, block.z, tilled, 0U,
                                        world::farmlandOrientation(0));
                        previewBlockEdit(block.x, block.y, block.z);
                        audioSystem.playBlockPlace(
                            tilled, {static_cast<float>(block.x) + 0.5F,
                                     static_cast<float>(block.y) + 0.5F,
                                     static_cast<float>(block.z) + 0.5F});
                        heldItemAnimation.trigger(animation::ModelAction::Use);
                        gameSession.worldSimulation().notifyPlaced({block.x, block.y, block.z}, tilled);
                        if (gameSession.gameMode() == gameplay::GameMode::Survival) {
                            if (gameSession.damageHeldTool(gameplay::ToolUse::Till,
                                                           world::blockDefinition(existing).hardness)) {
                                audioSystem.playItemBreak(camera.position());
                            }
                        }
                    }
                    break;
                }
                default:
                    break;
                }
                break;
            }
            }
        }
        breakBlockRequested = false;
        placeBlockRequested = false;
    }


    void spawnDroppedStack(gameplay::ItemStack stack) {
        if (stack.empty()) {
            return;
        }
        const glm::vec3 direction = camera.direction();
        gameSession.itemEntities().spawn(gameSession.player().eyePosition() + direction * 0.45F, stack,
                           direction * 0.28F + glm::vec3{0.0F, 0.12F, 0.0F});
    }

    void updateItemDrop() {
        if (!dropRequested) {
            return;
        }
        dropRequested = false;
        spawnDroppedStack(activeInventory().takeSelected(dropWholeStack));
        dropWholeStack = false;
    }

    [[nodiscard]] static std::size_t streamBufferClassIndex(VkDeviceSize bytes) {
        for (std::size_t index = 0; index < kStreamBufferClassSizes.size(); ++index) {
            if (bytes <= kStreamBufferClassSizes[index]) {
                return index;
            }
        }
        return kStreamBufferClassSizes.size() - 1U;
    }

    [[nodiscard]] AllocatedBuffer acquireStreamBuffer(StreamBufferPool& pool,
                                                      VkDeviceSize bytes,
                                                      VkBufferUsageFlags usage,
                                                      bool hostVisible) {
        const std::size_t classIndex = streamBufferClassIndex(bytes);
        auto& freeList = pool.freeByClass[classIndex];
        if (!freeList.empty()) {
            AllocatedBuffer result = freeList.back();
            freeList.pop_back();
            return result;
        }
        const VkDeviceSize classBytes = kStreamBufferClassSizes[classIndex];
        AllocatedBuffer result = createBuffer(classBytes, usage, hostVisible);
        result.pooledSizeClass = static_cast<std::uint8_t>(classIndex + 1U);
        pool.totalBytes += classBytes;
        return result;
    }

    // Return to the free list immediately; only safe after the device is idle
    // (world reset) or when the buffer was never submitted.
    void releaseStreamBufferNow(StreamBufferPool& pool, AllocatedBuffer& buffer) {
        if (buffer.pooledSizeClass == 0U) {
            destroyBuffer(buffer);
            return;
        }
        const std::size_t classIndex = buffer.pooledSizeClass - 1U;
        pool.freeByClass[classIndex].push_back(buffer);
        buffer = {};
    }

    // Hold for kFramesInFlight frames; tickStreamBufferPool returns it once the
    // same frame slot's fence confirms the GPU is done with it.
    void deferStreamBufferRelease(StreamBufferPool& pool, AllocatedBuffer& buffer) {
        if (buffer.pooledSizeClass == 0U) {
            destroyBuffer(buffer);
            return;
        }
        pool.deferred[currentFrame].push_back(buffer);
        buffer = {};
    }

    void tickStreamBufferPool(StreamBufferPool& pool) {
        auto& released = pool.deferred[currentFrame];
        for (auto& buffer : released) {
            const std::size_t classIndex = buffer.pooledSizeClass - 1U;
            pool.freeByClass[classIndex].push_back(buffer);
        }
        released.clear();
        // Return surplus free buffers to the driver once the pool overflows, so
        // a huge burst (teleport, world reset) does not hoard it permanently.
        for (std::size_t index = pool.freeByClass.size();
             index-- > 0U && pool.totalBytes > kMaxStreamBufferPoolBytes;) {
            auto& freeList = pool.freeByClass[index];
            while (!freeList.empty() && pool.totalBytes > kMaxStreamBufferPoolBytes) {
                pool.totalBytes -= kStreamBufferClassSizes[index];
                destroyBuffer(freeList.back());
                freeList.pop_back();
            }
        }
    }

    void releaseFrameResources(FrameContext& frame) {
        for (auto& buffer : frame.retiredBuffers) {
            destroyBuffer(buffer);
        }
        frame.retiredBuffers.clear();
        frame.uploadCopies.clear();
        tickStreamBufferPool(deviceBufferPool_);
        tickStreamBufferPool(stagingBufferPool_);
    }

    void retireMesh(FrameContext& frame, GpuMesh& mesh) {
        static_cast<void>(frame);
        deferStreamBufferRelease(deviceBufferPool_, mesh.indexBuffer);
        deferStreamBufferRelease(deviceBufferPool_, mesh.vertexBuffer);
        mesh = {};
    }

    [[nodiscard]] static VkDeviceSize meshByteSize(const MeshData& mesh) {
        return static_cast<VkDeviceSize>(mesh.vertices.size() * sizeof(VoxelVertex)) +
               static_cast<VkDeviceSize>(mesh.indices.size() * sizeof(std::uint32_t));
    }

    void uploadRenderMesh(FrameContext& frame, const render::RenderMeshData& source,
                          GpuMesh& destination) {
        const std::array layers{&source.mesh, &source.cutoutMesh, &source.translucentMesh};
        VkDeviceSize vertexBytes = 0;
        VkDeviceSize indexBytes = 0;
        for (const auto* layer : layers) {
            vertexBytes += static_cast<VkDeviceSize>(layer->vertices.size() * sizeof(VoxelVertex));
            indexBytes += static_cast<VkDeviceSize>(layer->indices.size() * sizeof(std::uint32_t));
        }
        auto vertexStaging =
            acquireStreamBuffer(stagingBufferPool_, vertexBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                true);
        auto indexStaging =
            acquireStreamBuffer(stagingBufferPool_, indexBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                true);
        std::array<GpuMeshLayer*, 3> destinations{&destination.opaque, &destination.cutout,
                                                  &destination.translucent};
        VkDeviceSize vertexOffset = 0;
        VkDeviceSize indexOffset = 0;
        for (std::size_t index = 0; index < layers.size(); ++index) {
            const auto* layer = layers[index];
            auto* gpuLayer = destinations[index];
            const VkDeviceSize layerVertexBytes =
                static_cast<VkDeviceSize>(layer->vertices.size() * sizeof(VoxelVertex));
            const VkDeviceSize layerIndexBytes =
                static_cast<VkDeviceSize>(layer->indices.size() * sizeof(std::uint32_t));
            gpuLayer->vertexOffset = vertexOffset;
            gpuLayer->indexOffset = indexOffset;
            gpuLayer->indexCount = static_cast<std::uint32_t>(layer->indices.size());
            if (layerVertexBytes > 0U) {
                std::memcpy(static_cast<std::byte*>(vertexStaging.mapped) + vertexOffset,
                            layer->vertices.data(), static_cast<std::size_t>(layerVertexBytes));
            }
            if (layerIndexBytes > 0U) {
                std::memcpy(static_cast<std::byte*>(indexStaging.mapped) + indexOffset,
                            layer->indices.data(), static_cast<std::size_t>(layerIndexBytes));
            }
            vertexOffset += layerVertexBytes;
            indexOffset += layerIndexBytes;
        }
        checkVk(vmaFlushAllocation(allocator, vertexStaging.allocation, 0, VK_WHOLE_SIZE),
                "vmaFlushAllocation(streaming vertices)");
        checkVk(vmaFlushAllocation(allocator, indexStaging.allocation, 0, VK_WHOLE_SIZE),
                "vmaFlushAllocation(streaming indices)");

        destination.vertexBuffer =
            acquireStreamBuffer(deviceBufferPool_, vertexBytes, kStreamBufferDeviceUsage, false);
        destination.indexBuffer =
            acquireStreamBuffer(deviceBufferPool_, indexBytes, kStreamBufferDeviceUsage, false);
        frame.uploadCopies.push_back(
            {vertexStaging.buffer, destination.vertexBuffer.buffer, vertexBytes});
        frame.uploadCopies.push_back(
            {indexStaging.buffer, destination.indexBuffer.buffer, indexBytes});
        deferStreamBufferRelease(stagingBufferPool_, vertexStaging);
        deferStreamBufferRelease(stagingBufferPool_, indexStaging);
    }

    void prepareStreamingUpdates(FrameContext& frame) {
        uploadedSectionsThisFrame = 0;
        uploadedBytesThisFrame = 0;
        std::size_t processedUpdates = 0;
        std::size_t priorityUploads = 0;
        constexpr std::size_t kMaxRemovalsPerFrame = 256;

        while (!pendingSectionOrder.empty()) {
            const world::SectionPosition position = pendingSectionOrder.front();
            const auto found = pendingSectionUpdates.find(position);
            if (found == pendingSectionUpdates.end()) {
                pendingSectionOrder.pop_front();
                continue;
            }

            const bool uploadsMesh = !found->second.remove && !found->second.mesh.empty();
            const bool priority = uploadsMesh && found->second.highPriority;
            const VkDeviceSize updateBytes =
                uploadsMesh ? meshByteSize(found->second.mesh.mesh) +
                                  meshByteSize(found->second.mesh.cutoutMesh) +
                                  meshByteSize(found->second.mesh.translucentMesh)
                            : 0;
            if (priority) {
                // Edits upload on a separate capped bucket, exempt from the
                // streaming section/byte budget, so they land the same frame.
                if (priorityUploads >= kMaxPrioritySectionUploadsPerFrame) {
                    break;
                }
            } else if (uploadsMesh) {
                if (uploadedSectionsThisFrame >= streamingUploadBudget_) {
                    break;
                }
                if (uploadedSectionsThisFrame > 0U &&
                    uploadedBytesThisFrame + updateBytes > kMaxUploadBytesPerFrame) {
                    break;
                }
            }
            if (!uploadsMesh && processedUpdates >= kMaxRemovalsPerFrame) {
                break;
            }

            world::SectionMeshUpdate update = std::move(found->second);
            pendingSectionUpdates.erase(found);
            pendingSectionOrder.pop_front();
            ++processedUpdates;
            // A section the quality remesh was waiting on has been replaced
            // (either uploaded or retired), so it no longer gates the uniform
            // flip at the bottom.
            qualityRemeshPending.erase(position);

            const auto existing = gpuMeshes.find(position);
            if (existing != gpuMeshes.end()) {
                retireMesh(frame, existing->second);
                gpuMeshes.erase(existing);
            }
            occlusionStates.erase(position);
            occlusionMissCount.erase(position);
            if (!uploadsMesh) {
                chunkStreamer.releaseMeshData(std::move(update.mesh));
                continue;
            }

            GpuMesh gpuMesh;
            gpuMesh.bounds = update.mesh.bounds;
            gpuMesh.sectionOrigin = {
                static_cast<float>(position.chunkX) * world::kChunkWidth,
                static_cast<float>(position.sectionY) * world::kSectionSize,
                static_cast<float>(position.chunkZ) * world::kChunkDepth};
            uploadRenderMesh(frame, update.mesh, gpuMesh);
            // The worker built this mesh into a pooled RenderMeshData; hand it
            // back so the capacity is reused by the next section build.
            chunkStreamer.releaseMeshData(std::move(update.mesh));
            gpuMeshes.insert_or_assign(position, gpuMesh);
            // A fresh mesh must be drawn and queried before occlusion can trust
            // it, so it starts Unknown instead of inheriting a stale result.
            occlusionStates[position] = OcclusionState::Unknown;
            if (priority) {
                ++priorityUploads;
            } else {
                ++uploadedSectionsThisFrame;
                uploadedBytesThisFrame += updateBytes;
            }
            totalUploadedBytes += updateBytes;
        }
        // Once every section re-baked at targetMeshQuality has landed, flip the
        // quality the shader's High branch expects so it never reads the new AO
        // curve off a stale Standard mesh (or vice versa).
        if (qualityRemeshPending.empty() && currentMeshQuality != targetMeshQuality) {
            currentMeshQuality = targetMeshQuality;
        }
    }

    void transitionTextureImage(const AllocatedImage& image, std::uint32_t layerCount,
                                VkImageLayout oldLayout, VkImageLayout newLayout,
                                VkAccessFlags sourceAccess, VkAccessFlags destinationAccess,
                                VkPipelineStageFlags sourceStage,
                                VkPipelineStageFlags destinationStage) const {
        const auto commandBuffer = beginSingleUseCommands();
        auto barrier = vkStructure<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image.image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = layerCount;
        barrier.srcAccessMask = sourceAccess;
        barrier.dstAccessMask = destinationAccess;
        vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0, 0, nullptr, 0,
                             nullptr, 1, &barrier);
        endSingleUseCommands(commandBuffer);
    }

    void createTextureArray() {
        const auto pixels = loadGrassBlockTextures(blockTextureRoot);
        const auto byteSize = static_cast<VkDeviceSize>(pixels.rgba.size());
        // The atlas layer count is whatever the name-driven build produced
        // (fixed special section + block textures + item icons), so it is
        // derived from the bytes rather than a compile-time constant.
        const auto layerSize =
            static_cast<VkDeviceSize>(pixels.width) * static_cast<VkDeviceSize>(pixels.height) * 4U;
        if (byteSize % layerSize != 0U) {
            throw std::runtime_error("Block texture array data is not whole layers");
        }
        const std::uint32_t layerCount =
            static_cast<std::uint32_t>(byteSize / layerSize);
        auto staging = createBuffer(byteSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        std::memcpy(staging.mapped, pixels.rgba.data(), pixels.rgba.size());
        checkVk(vmaFlushAllocation(allocator, staging.allocation, 0, VK_WHOLE_SIZE),
                "vmaFlushAllocation(texture staging)");
        textureImage =
            createImage(pixels.width, pixels.height, layerCount, VK_FORMAT_R8G8B8A8_SRGB,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        transitionTextureImage(textureImage, layerCount, VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                               VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT);

        std::vector<VkBufferImageCopy> regions(layerCount);
        for (std::uint32_t layer = 0; layer < layerCount; ++layer) {
            regions[layer].bufferOffset = layerSize * layer;
            regions[layer].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            regions[layer].imageSubresource.mipLevel = 0;
            regions[layer].imageSubresource.baseArrayLayer = layer;
            regions[layer].imageSubresource.layerCount = 1;
            regions[layer].imageExtent = {pixels.width, pixels.height, 1};
        }
        const auto commandBuffer = beginSingleUseCommands();
        vkCmdCopyBufferToImage(commandBuffer, staging.buffer, textureImage.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<std::uint32_t>(regions.size()), regions.data());
        endSingleUseCommands(commandBuffer);
        transitionTextureImage(
            textureImage, layerCount, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        destroyBuffer(staging);

        auto viewInfo =
            vkStructure<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
        viewInfo.image = textureImage.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = layerCount;
        checkVk(vkCreateImageView(device, &viewInfo, nullptr, &textureView),
                "vkCreateImageView(texture)");

        createTextureSampler();
        std::cout << "Loaded block texture array: " << pixels.width << 'x' << pixels.height << " x "
                  << layerCount << '\n';
    }

    // Loads every shipped species' model + skin into a dedicated 2D-array
    // texture (binding 4), one layer per species in speciesModels order (pig
    // layer 0, zombie layer 1). Each species' geometry, animation and skin come
    // from its registered EntityType render descriptor, so a new creature only
    // has to declare them on its type and be listed in loadSpecies below. If a
    // .png is missing the loader falls back to a procedural skin painted
    // through the exact same animation::boxUvFaceRect the box-UV shader samples
    // with, so geometry and texture always agree.
    void createEntityTextureArray() {
        const auto resourceRoot = blockTextureRoot.parent_path()
                                      .parent_path()
                                      .parent_path()
                                      .parent_path()
                                      .parent_path();
        // Species geometry/animations and skins are loaded by the gameplay
        // layer, so a new creature needs no renderer code: registering an
        // EntityType with a render descriptor is enough for buildSpeciesModels
        // to pick it up. Built-in models cover missing resource files.
        speciesModels = gameplay::entities::buildSpeciesModels(
            resourceRoot, gameplay::entities::entityTypeRegistry());

        // The entity texture array: one layer per loaded species, in
        // speciesModels order. Array layers share one extent, so it is the
        // largest declared skin among the shipped species. Each species' skin
        // is nearest-neighbour scaled onto its full layer: the shader
        // normalizes box-UV texels by the species' *declared* size, so sampling
        // a full-extent layer finds every texel only if the skin is stretched
        // to fill it (the pig's 64x32 skin doubles vertically, the 64x64
        // zombie skin stays 1:1).
        std::uint32_t atlasWidth = 64U;
        std::uint32_t atlasHeight = 64U;
        // The declared size of a species model, falling back to the current
        // atlas extent when the geometry omits its texture_width/height.
        const auto declaredSize = [&](const animation::SkeletalModel& model) {
            return gameplay::entities::entityTextureSize(
                model, {static_cast<float>(atlasWidth), static_cast<float>(atlasHeight)});
        };
        for (const auto& species : speciesModels) {
            if (!species.loaded) {
                continue;
            }
            const glm::vec2 declared = declaredSize(species.model.model);
            atlasWidth = std::max(atlasWidth, static_cast<std::uint32_t>(declared.x));
            atlasHeight = std::max(atlasHeight, static_cast<std::uint32_t>(declared.y));
        }
        entityTextureWidth = atlasWidth;
        entityTextureHeight = atlasHeight;
        const std::uint32_t layerCount = static_cast<std::uint32_t>(speciesModels.size());
        std::vector<std::uint8_t> atlas(static_cast<std::size_t>(atlasWidth) * atlasHeight * 4U *
                                            layerCount,
                                        0U);
        for (std::size_t index = 0; index < speciesModels.size(); ++index) {
            const auto& species = speciesModels[index];
            if (!species.loaded) {
                continue;
            }
            const auto skin = gameplay::entities::buildSpeciesSkin(
                resourceRoot, blockTextureRoot.parent_path(), species.model.model,
                species.type->render().texturePath,
                {static_cast<float>(atlasWidth), static_cast<float>(atlasHeight)});
            const glm::vec2 declared = declaredSize(species.model.model);
            const std::uint32_t skinWidth = static_cast<std::uint32_t>(declared.x);
            const std::uint32_t skinHeight = static_cast<std::uint32_t>(declared.y);
            for (std::uint32_t layerY = 0; layerY < atlasHeight; ++layerY) {
                const std::uint32_t srcY =
                    std::min(skinHeight - 1U, layerY * skinHeight / atlasHeight);
                for (std::uint32_t layerX = 0; layerX < atlasWidth; ++layerX) {
                    const std::uint32_t srcX =
                        std::min(skinWidth - 1U, layerX * skinWidth / atlasWidth);
                    const std::size_t src =
                        (static_cast<std::size_t>(srcY) * skinWidth + srcX) * 4U;
                    const std::size_t dst =
                        (index * atlasWidth * atlasHeight + layerY * atlasWidth + layerX) * 4U;
                    std::memcpy(&atlas[dst], &skin[src], 4U);
                }
            }
        }

        const VkDeviceSize byteSize = static_cast<VkDeviceSize>(atlas.size());
        auto staging = createBuffer(byteSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        std::memcpy(staging.mapped, atlas.data(), atlas.size());
        checkVk(vmaFlushAllocation(allocator, staging.allocation, 0, VK_WHOLE_SIZE),
                "vmaFlushAllocation(entity staging)");
        entityTextureImage = createImage(atlasWidth, atlasHeight, layerCount,
                                         VK_FORMAT_R8G8B8A8_SRGB,
                                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        transitionTextureImage(entityTextureImage, layerCount, VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        std::vector<VkBufferImageCopy> regions(layerCount);
        for (std::uint32_t layer = 0; layer < layerCount; ++layer) {
            regions[layer].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            regions[layer].imageSubresource.baseArrayLayer = layer;
            regions[layer].imageSubresource.layerCount = 1U;
            regions[layer].imageExtent = {atlasWidth, atlasHeight, 1U};
            regions[layer].bufferOffset =
                static_cast<VkDeviceSize>(layer) * atlasWidth * atlasHeight * 4U;
        }
        const auto commandBuffer = beginSingleUseCommands();
        vkCmdCopyBufferToImage(commandBuffer, staging.buffer, entityTextureImage.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layerCount, regions.data());
        endSingleUseCommands(commandBuffer);
        transitionTextureImage(
            entityTextureImage, layerCount, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        destroyBuffer(staging);

        auto viewInfo =
            vkStructure<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
        viewInfo.image = entityTextureImage.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = layerCount;
        checkVk(vkCreateImageView(device, &viewInfo, nullptr, &entityTextureView),
                "vkCreateImageView(entity)");
        std::cout << "Loaded entity texture atlas: " << atlasWidth << 'x' << atlasHeight << " x "
                  << layerCount << '\n';
    }

    void createTextureSampler() {
        auto samplerInfo = vkStructure<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
        samplerInfo.magFilter = VK_FILTER_NEAREST;
        samplerInfo.minFilter = VK_FILTER_NEAREST;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.anisotropyEnable = samplerAnisotropySupported && options.anisotropy > 1
            ? VK_TRUE : VK_FALSE;
        samplerInfo.maxAnisotropy = samplerInfo.anisotropyEnable == VK_TRUE
            ? std::min(static_cast<float>(options.anisotropy), maximumSamplerAnisotropy)
            : 1.0F;
        samplerInfo.maxLod = 0.0F;
        checkVk(vkCreateSampler(device, &samplerInfo, nullptr, &textureSampler), "vkCreateSampler");
    }

    void recreateTextureSampler() {
        checkVk(vkDeviceWaitIdle(device), "vkDeviceWaitIdle(anisotropy)");
        if (descriptorPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device, descriptorPool, nullptr);
            descriptorPool = VK_NULL_HANDLE;
        }
        if (textureSampler != VK_NULL_HANDLE) {
            vkDestroySampler(device, textureSampler, nullptr);
            textureSampler = VK_NULL_HANDLE;
        }
        createTextureSampler();
        createDescriptorPoolAndSets();
    }

    // The font lives in one 256x256 R8 texture array: layer 0 is ascii.png
    // upscaled two times so every layer shares a size, and each following layer
    // is one legacy unicode page needed by the active language.
    void createFontTexture() {
        const auto fontRoot = blockTextureRoot.parent_path() / "font";
        const auto ascii = assets::ImageData::loadRgba(fontRoot / "ascii.png");
        fontMetrics = ui::BitmapFontMetrics::fromRgba(ascii.rgba, ascii.width, ascii.height);
        textFont.setAsciiMetrics(fontMetrics);
        textFont.clearUnicodePages();
        textFont.setForceUnicode(options.forceUnicodeFont);

        constexpr std::uint32_t kFontPageSize = 256U;
        constexpr std::size_t kFontLayerBytes =
            static_cast<std::size_t>(kFontPageSize) * kFontPageSize;
        std::vector<std::uint8_t> pixels;
        pixels.resize(kFontLayerBytes);
        // Nearest-neighbour upscale of the 128x128 sheet keeps its normalized
        // UVs and its on-screen pixels identical.
        for (std::uint32_t y = 0; y < kFontPageSize; ++y) {
            for (std::uint32_t x = 0; x < kFontPageSize; ++x) {
                const auto sourceX =
                    std::min(x * static_cast<std::uint32_t>(ascii.width) / kFontPageSize,
                             static_cast<std::uint32_t>(ascii.width) - 1U);
                const auto sourceY =
                    std::min(y * static_cast<std::uint32_t>(ascii.height) / kFontPageSize,
                             static_cast<std::uint32_t>(ascii.height) - 1U);
                pixels[y * kFontPageSize + x] =
                    ascii.rgba[(static_cast<std::size_t>(sourceY) *
                                    static_cast<std::size_t>(ascii.width) + sourceX) * 4U + 3U];
            }
        }

        // glyph_sizes.bin gives the used column range of every BMP codepoint.
        // Without it there is no way to advance the cursor per glyph, so the
        // unicode pages are skipped entirely.
        textFont.setUnicodeSizes(loadGlyphSizes());
        std::uint32_t layerCount = 1U;
        for (const int page : textFont.hasUnicodePages() ? requiredUnicodePages()
                                                         : std::set<int>{}) {
            std::ostringstream name;
            name << "unicode_page_" << std::hex << std::setfill('0') << std::setw(2) << page
                 << ".png";
            const auto path = fontRoot / name.str();
            std::error_code error;
            if (!std::filesystem::is_regular_file(path, error)) {
                continue;
            }
            try {
                const auto image = assets::ImageData::loadRgba(path);
                if (image.width != static_cast<int>(kFontPageSize) ||
                    image.height != static_cast<int>(kFontPageSize)) {
                    continue;
                }
                const std::size_t offset = pixels.size();
                pixels.resize(offset + kFontLayerBytes);
                for (std::size_t index = 0; index < kFontLayerBytes; ++index) {
                    pixels[offset + index] = image.rgba[index * 4U + 3U];
                }
                textFont.setUnicodePageLayer(page, static_cast<int>(layerCount));
                ++layerCount;
            } catch (const std::exception&) {
                // A missing or damaged page just falls back to the ASCII sheet.
            }
        }

        const auto byteSize = static_cast<VkDeviceSize>(pixels.size());
        auto staging = createBuffer(byteSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        std::memcpy(staging.mapped, pixels.data(), pixels.size());
        checkVk(vmaFlushAllocation(allocator, staging.allocation, 0, VK_WHOLE_SIZE),
                "vmaFlushAllocation(font staging)");
        fontTextureImage = createImage(
            kFontPageSize, kFontPageSize, layerCount, VK_FORMAT_R8_UNORM,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        transitionTextureImage(fontTextureImage, layerCount, VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                               VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT);

        std::vector<VkBufferImageCopy> regions(layerCount);
        for (std::uint32_t layer = 0; layer < layerCount; ++layer) {
            regions[layer].bufferOffset = static_cast<VkDeviceSize>(kFontLayerBytes) * layer;
            regions[layer].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            regions[layer].imageSubresource.baseArrayLayer = layer;
            regions[layer].imageSubresource.layerCount = 1;
            regions[layer].imageExtent = {kFontPageSize, kFontPageSize, 1};
        }
        const auto commandBuffer = beginSingleUseCommands();
        vkCmdCopyBufferToImage(commandBuffer, staging.buffer, fontTextureImage.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<std::uint32_t>(regions.size()), regions.data());
        endSingleUseCommands(commandBuffer);
        transitionTextureImage(fontTextureImage, layerCount,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        destroyBuffer(staging);

        auto viewInfo =
            vkStructure<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
        viewInfo.image = fontTextureImage.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        viewInfo.format = VK_FORMAT_R8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = layerCount;
        checkVk(vkCreateImageView(device, &viewInfo, nullptr, &fontTextureView),
                "vkCreateImageView(font)");
        std::cout << "Loaded Minecraft font array: " << kFontPageSize << 'x' << kFontPageSize
                  << " x " << layerCount << " (ascii + " << (layerCount - 1U)
                  << " unicode pages)\n";
    }

    [[nodiscard]] std::vector<std::uint8_t> loadGlyphSizes() const {
        const auto path = blockTextureRoot.parent_path().parent_path().parent_path() / "fonts" /
            "minecraft" / "glyph_sizes.bin";
        std::ifstream input{path, std::ios::binary};
        if (!input) {
            std::cout << "No glyph_sizes.bin at " << path << "; unicode text is unavailable\n";
            return {};
        }
        std::vector<std::uint8_t> sizes(0x10000U);
        input.read(reinterpret_cast<char*>(sizes.data()),
                   static_cast<std::streamsize>(sizes.size()));
        if (input.gcount() != static_cast<std::streamsize>(sizes.size())) {
            std::cout << "glyph_sizes.bin is truncated; unicode text is unavailable\n";
            return {};
        }
        return sizes;
    }

    // The unicode pages the active language actually needs, plus page 0 so
    // forced-unicode Latin text has glyphs to draw. The language screen lists
    // every available language in its own script (简体中文 beside English), so
    // those display names keep their glyph pages in the font too, or they would
    // render as "?" whenever the active language is pure ASCII.
    [[nodiscard]] std::set<int> requiredUnicodePages() const {
        if (language.empty() && !options.forceUnicodeFont) {
            return {};
        }
        auto pages = language.requiredUnicodePages();
        pages.insert(0);
        for (const auto& name : menuSystem.languageDisplayNames) {
            for (const char32_t codepoint : ui::decodeUtf8(name)) {
                if (codepoint <= 0xFFFF) {
                    pages.insert(static_cast<int>(codepoint >> 8U));
                }
            }
        }
        return pages;
    }

    // Rebuilds the font array after a language or force-unicode change.
    void recreateFontTexture() {
        checkVk(vkDeviceWaitIdle(device), "vkDeviceWaitIdle(font)");
        if (descriptorPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device, descriptorPool, nullptr);
            descriptorPool = VK_NULL_HANDLE;
        }
        if (fontTextureView != VK_NULL_HANDLE) {
            vkDestroyImageView(device, fontTextureView, nullptr);
            fontTextureView = VK_NULL_HANDLE;
        }
        destroyImage(fontTextureImage);
        createFontTexture();
        createDescriptorPoolAndSets();
    }

    void createGuiTexture() {
        const auto guiRoot = blockTextureRoot.parent_path() / "gui";
        const auto underwater = repeatTileToAtlas(
            assets::ImageData::loadRgba(guiRoot.parent_path() / "misc" / "underwater.png"), 256,
            256, 4);
        const auto loadingDirt = repeatTileToAtlas(
            assets::ImageData::loadRgba(guiRoot / "options_background.png"), 256, 256, 8);
        const auto chestGui =
            singleChestGui(assets::ImageData::loadRgba(guiRoot / "container" / "generic_54.png"));
        // 1.16.1's Screen.renderBackground paints a vertical gradient from
        // rgba(0x10,0x10,0x10,0xC0) at the top to rgba(0x10,0x10,0x10,0xD0) at
        // the bottom over every in-game screen. Bake that into a 256x256 layer
        // so each screen draws the exact vanilla backdrop with one sprite.
        assets::ImageData screenDimGradient;
        screenDimGradient.width = 256;
        screenDimGradient.height = 256;
        screenDimGradient.rgba.resize(256U * 256U * 4U);
        for (std::uint32_t gradientY = 0U; gradientY < 256U; ++gradientY) {
            const std::uint8_t gradientAlpha = static_cast<std::uint8_t>(
                0xC0U + (0xD0U - 0xC0U) * gradientY / 255U);
            for (std::uint32_t gradientX = 0U; gradientX < 256U; ++gradientX) {
                const std::size_t offset =
                    static_cast<std::size_t>(gradientY * 256U + gradientX) * 4U;
                screenDimGradient.rgba[offset + 0] = 0x10U;
                screenDimGradient.rgba[offset + 1] = 0x10U;
                screenDimGradient.rgba[offset + 2] = 0x10U;
                screenDimGradient.rgba[offset + 3] = gradientAlpha;
            }
        }
        const std::array images{
            assets::ImageData::loadRgba(guiRoot / "widgets.png"),
            assets::ImageData::loadRgba(guiRoot / "icons.png"),
            assets::ImageData::loadRgba(guiRoot / "container" / "inventory.png"),
            assets::ImageData::loadRgba(guiRoot / "container" / "creative_inventory" /
                                        "tab_items.png"),
            assets::ImageData::loadRgba(guiRoot / "container" / "creative_inventory" / "tabs.png"),
            assets::ImageData::loadRgba(guiRoot / "container" / "creative_inventory" /
                                        "tab_inventory.png"),
            underwater,
            assets::ImageData::loadRgba(guiRoot / "container" / "crafting_table.png"),
            assets::ImageData::loadRgba(guiRoot / "container" / "furnace.png"),
            loadingDirt,
            chestGui,
            assets::ImageData::loadRgba(guiRoot.parent_path() / "misc" / "vignette.png"),
            screenDimGradient,
        };
        constexpr std::uint32_t kGuiLayerCount = 13U;
        const int width = images.front().width;
        const int height = images.front().height;
        for (const auto& image : images) {
            if (image.width != width || image.height != height) {
                throw std::runtime_error("Minecraft GUI textures must share one size");
            }
        }
        std::vector<std::uint8_t> pixels;
        const std::size_t layerBytes = images.front().rgba.size();
        pixels.reserve(layerBytes * images.size());
        for (const auto& image : images) {
            pixels.insert(pixels.end(), image.rgba.begin(), image.rgba.end());
        }
        const auto byteSize = static_cast<VkDeviceSize>(pixels.size());
        auto staging = createBuffer(byteSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        std::memcpy(staging.mapped, pixels.data(), pixels.size());
        checkVk(vmaFlushAllocation(allocator, staging.allocation, 0, VK_WHOLE_SIZE),
                "vmaFlushAllocation(gui staging)");
        guiTextureImage = createImage(
            static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), kGuiLayerCount,
            VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        transitionTextureImage(guiTextureImage, kGuiLayerCount, VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                               VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT);

        std::array<VkBufferImageCopy, kGuiLayerCount> regions{};
        for (std::uint32_t layer = 0; layer < kGuiLayerCount; ++layer) {
            regions[layer].bufferOffset = static_cast<VkDeviceSize>(layerBytes) * layer;
            regions[layer].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            regions[layer].imageSubresource.baseArrayLayer = layer;
            regions[layer].imageSubresource.layerCount = 1;
            regions[layer].imageExtent = {
                static_cast<std::uint32_t>(width),
                static_cast<std::uint32_t>(height),
                1,
            };
        }
        const auto commandBuffer = beginSingleUseCommands();
        vkCmdCopyBufferToImage(commandBuffer, staging.buffer, guiTextureImage.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<std::uint32_t>(regions.size()), regions.data());
        endSingleUseCommands(commandBuffer);
        transitionTextureImage(
            guiTextureImage, kGuiLayerCount, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        destroyBuffer(staging);

        auto viewInfo =
            vkStructure<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
        viewInfo.image = guiTextureImage.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = kGuiLayerCount;
        checkVk(vkCreateImageView(device, &viewInfo, nullptr, &guiTextureView),
                "vkCreateImageView(gui)");
        std::cout << "Loaded Minecraft GUI texture array: " << width << 'x' << height << " x "
                  << kGuiLayerCount << '\n';
    }

    // The title-screen panorama faces are 1024x1024 photographs, so they get
    // their own array at native resolution instead of a layer in the 256px GUI
    // array. One layer per 1.16.1 panorama face.
    void createPanoramaTexture() {
        const auto guiRoot = blockTextureRoot.parent_path() / "gui";
        const auto backgroundRoot = guiRoot / "title" / "background";
        std::array<assets::ImageData, kPanoramaFaces> faces{};
        for (std::size_t index = 0; index < kPanoramaFaces; ++index) {
            faces[index] = assets::ImageData::loadRgba(
                backgroundRoot / ("panorama_" + std::to_string(index) + ".png"));
        }
        const int width = faces.front().width;
        const int height = faces.front().height;
        for (const auto& face : faces) {
            if (face.width != width || face.height != height) {
                throw std::runtime_error("Minecraft panorama faces must share one size");
            }
        }
        std::vector<std::uint8_t> pixels;
        const std::size_t layerBytes = faces.front().rgba.size();
        pixels.reserve(layerBytes * kPanoramaFaces);
        for (const auto& face : faces) {
            pixels.insert(pixels.end(), face.rgba.begin(), face.rgba.end());
        }
        const auto byteSize = static_cast<VkDeviceSize>(pixels.size());
        auto staging = createBuffer(byteSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        std::memcpy(staging.mapped, pixels.data(), pixels.size());
        checkVk(vmaFlushAllocation(allocator, staging.allocation, 0, VK_WHOLE_SIZE),
                "vmaFlushAllocation(panorama staging)");
        panoramaTextureImage = createImage(
            static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
            static_cast<std::uint32_t>(kPanoramaFaces), VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        transitionTextureImage(panoramaTextureImage, static_cast<std::uint32_t>(kPanoramaFaces),
                               VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                               VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT);
        std::array<VkBufferImageCopy, kPanoramaFaces> regions{};
        for (std::size_t layer = 0; layer < kPanoramaFaces; ++layer) {
            regions[layer].bufferOffset = static_cast<VkDeviceSize>(layerBytes) * layer;
            regions[layer].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            regions[layer].imageSubresource.baseArrayLayer = static_cast<std::uint32_t>(layer);
            regions[layer].imageSubresource.layerCount = 1;
            regions[layer].imageExtent = {static_cast<std::uint32_t>(width),
                                          static_cast<std::uint32_t>(height), 1};
        }
        const auto commandBuffer = beginSingleUseCommands();
        vkCmdCopyBufferToImage(commandBuffer, staging.buffer, panoramaTextureImage.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<std::uint32_t>(regions.size()), regions.data());
        endSingleUseCommands(commandBuffer);
        transitionTextureImage(panoramaTextureImage, static_cast<std::uint32_t>(kPanoramaFaces),
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                               VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        destroyBuffer(staging);
        auto viewInfo =
            vkStructure<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
        viewInfo.image = panoramaTextureImage.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = static_cast<std::uint32_t>(kPanoramaFaces);
        checkVk(vkCreateImageView(device, &viewInfo, nullptr, &panoramaTextureView),
                "vkCreateImageView(panorama)");
        std::cout << "Loaded Minecraft title panorama: " << width << 'x' << height << " x "
                  << kPanoramaFaces << " faces\n";
    }

    void createPanoramaSampler() {
        auto samplerInfo = vkStructure<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
        // Panorama faces are upscaled photographs, so they need linear filtering
        // instead of the nearest-neighbour pixel-art sampler.
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.maxLod = 0.0F;
        checkVk(vkCreateSampler(device, &samplerInfo, nullptr, &panoramaSampler),
                "vkCreateSampler(panorama)");
    }

    void createBiomeTextureResources() {
        const std::uint32_t size = static_cast<std::uint32_t>(kBiomeTextureSize);
        biomeGrassImage = createImage(size, size, 1, VK_FORMAT_R8G8B8A8_SRGB,
                                      VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                          VK_IMAGE_USAGE_SAMPLED_BIT);
        biomeFoliageImage = createImage(size, size, 1, VK_FORMAT_R8G8B8A8_SRGB,
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                            VK_IMAGE_USAGE_SAMPLED_BIT);
        auto samplerInfo = vkStructure<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
        // Linear filtering turns the per-cell biome colours into a smooth,
        // hardware-interpolated per-pixel gradient across biome boundaries.
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxLod = 0.0F;
        checkVk(vkCreateSampler(device, &samplerInfo, nullptr, &biomeSampler),
                "vkCreateSampler(biome)");
        biomeGrassView = createImageView(biomeGrassImage.image, VK_FORMAT_R8G8B8A8_SRGB,
                                         VK_IMAGE_ASPECT_COLOR_BIT);
        biomeFoliageView = createImageView(biomeFoliageImage.image, VK_FORMAT_R8G8B8A8_SRGB,
                                           VK_IMAGE_ASPECT_COLOR_BIT);
    }

    // Regenerates the biome colour lookup textures for a world seed: each texel
    // holds the grass/foliage colour (from the vanilla colour maps, with the
    // swamp/dark-forest overrides) of the biome at that 4-block cell. The
    // fragment shader samples them with linear filtering, so adjacent cells
    // blend into a per-pixel gradient — the GPU-side stand-in for 1.16.1's
    // per-vertex BiomeColors.
    void updateBiomeColorTextures(std::uint64_t seed) {
        constexpr int size = kBiomeTextureSize;
        constexpr int span = kBiomeTextureBlockSpan;
        constexpr int halfBlocks = size * span / 2;
        const auto grassColormap =
            assets::ImageData::loadRgba(blockTextureRoot.parent_path() / "colormap" / "grass.png");
        const auto foliageColormap =
            assets::ImageData::loadRgba(blockTextureRoot.parent_path() / "colormap" / "foliage.png");
        const auto colorAt = [](const assets::ImageData& colormap, float temperature,
                                float downfall) -> std::uint32_t {
            const float clampedTemperature = std::clamp(temperature, 0.0F, 1.0F);
            const float humidity = std::clamp(downfall, 0.0F, 1.0F) * clampedTemperature;
            const int xIndex = static_cast<int>((1.0 - clampedTemperature) * 255.0);
            const int yIndex = static_cast<int>((1.0 - humidity) * 255.0);
            const std::size_t pixel = static_cast<std::size_t>(yIndex * 256 + xIndex) * 4U;
            if (pixel + 3U >= colormap.rgba.size()) {
                return 0x00FF00U;
            }
            return (static_cast<std::uint32_t>(colormap.rgba[pixel]) << 16U) |
                   (static_cast<std::uint32_t>(colormap.rgba[pixel + 1U]) << 8U) |
                   static_cast<std::uint32_t>(colormap.rgba[pixel + 2U]);
        };
        world::gen::LayeredBiomeSource biomes{seed};
        std::vector<std::uint8_t> grassPixels;
        std::vector<std::uint8_t> foliagePixels;
        grassPixels.reserve(static_cast<std::size_t>(size) * size * 4U);
        foliagePixels.reserve(static_cast<std::size_t>(size) * size * 4U);
        for (int texelZ = 0; texelZ < size; ++texelZ) {
            for (int texelX = 0; texelX < size; ++texelX) {
                const int worldX = texelX * span - halfBlocks;
                const int worldZ = texelZ * span - halfBlocks;
                const auto biome = biomes.sample(worldX >> 2, worldZ >> 2);
                const auto& definition = world::gen::biomeDefinition(biome);
                std::uint32_t grassColor =
                    colorAt(grassColormap, definition.temperature, definition.downfall);
                if (biome == world::gen::Biome::DarkForest) {
                    grassColor = ((grassColor & 0xFEFEFEU) + 0x28340AU) >> 1U;
                }
                std::uint32_t foliageColor =
                    colorAt(foliageColormap, definition.temperature, definition.downfall);
                if (biome == world::gen::Biome::Swamp) {
                    foliageColor = 0x6A7039U;
                }
                for (int channel = 2; channel >= 0; --channel) {
                    grassPixels.push_back(static_cast<std::uint8_t>(
                        (grassColor >> (channel * 8U)) & 0xFFU));
                    foliagePixels.push_back(static_cast<std::uint8_t>(
                        (foliageColor >> (channel * 8U)) & 0xFFU));
                }
                grassPixels.push_back(255U);
                foliagePixels.push_back(255U);
            }
        }
        const auto upload = [&](AllocatedImage& image, const std::vector<std::uint8_t>& pixels) {
            const VkDeviceSize byteSize = pixels.size();
            auto staging = createBuffer(byteSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
            std::memcpy(staging.mapped, pixels.data(), static_cast<std::size_t>(byteSize));
            checkVk(vmaFlushAllocation(allocator, staging.allocation, 0, VK_WHOLE_SIZE),
                    "vmaFlushAllocation(biome texture)");
            transitionTextureImage(image, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                                   VK_ACCESS_TRANSFER_WRITE_BIT,
                                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy region{};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = {static_cast<std::uint32_t>(size),
                                  static_cast<std::uint32_t>(size), 1};
            const auto commandBuffer = beginSingleUseCommands();
            vkCmdCopyBufferToImage(commandBuffer, staging.buffer, image.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            endSingleUseCommands(commandBuffer);
            transitionTextureImage(
                image, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            destroyBuffer(staging);
        };
        upload(biomeGrassImage, grassPixels);
        upload(biomeFoliageImage, foliagePixels);
    }

    void createDescriptorSetLayout() {
        VkDescriptorSetLayoutBinding uniformBinding{};
        uniformBinding.binding = 0;
        uniformBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        uniformBinding.descriptorCount = 1;
        uniformBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutBinding samplerBinding{};
        samplerBinding.binding = 1;
        samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        samplerBinding.descriptorCount = 1;
        samplerBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutBinding fontSamplerBinding = samplerBinding;
        fontSamplerBinding.binding = 2;
        VkDescriptorSetLayoutBinding guiSamplerBinding = samplerBinding;
        guiSamplerBinding.binding = 3;
        VkDescriptorSetLayoutBinding entitySamplerBinding = samplerBinding;
        entitySamplerBinding.binding = 4;
        VkDescriptorSetLayoutBinding panoramaSamplerBinding = samplerBinding;
        panoramaSamplerBinding.binding = 5;
        VkDescriptorSetLayoutBinding biomeGrassSamplerBinding = samplerBinding;
        biomeGrassSamplerBinding.binding = 6;
        VkDescriptorSetLayoutBinding biomeFoliageSamplerBinding = samplerBinding;
        biomeFoliageSamplerBinding.binding = 7;
        // The sun shadow depth map, sampled by the terrain fragment shader. A
        // separate binding so only grass_block.frag (and future lit passes) see
        // it; the other pipelines just leave it unwritten-safe.
        VkDescriptorSetLayoutBinding shadowSamplerBinding{};
        shadowSamplerBinding.binding = 8;
        shadowSamplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        shadowSamplerBinding.descriptorCount = 1;
        shadowSamplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        const std::array bindings{uniformBinding, samplerBinding, fontSamplerBinding,
                                  guiSamplerBinding, entitySamplerBinding,
                                  panoramaSamplerBinding, biomeGrassSamplerBinding,
                                  biomeFoliageSamplerBinding, shadowSamplerBinding};
        auto info = vkStructure<VkDescriptorSetLayoutCreateInfo>(
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
        info.bindingCount = static_cast<std::uint32_t>(bindings.size());
        info.pBindings = bindings.data();
        checkVk(vkCreateDescriptorSetLayout(device, &info, nullptr, &descriptorSetLayout),
                "vkCreateDescriptorSetLayout");
    }

    void createUniformBuffers() {
        for (auto& frame : frames) {
            frame.uniformBuffer =
                createBuffer(sizeof(CameraUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        }
    }

    void createDescriptorPoolAndSets() {
        const std::array<VkDescriptorPoolSize, 2> poolSizes{{
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, static_cast<std::uint32_t>(kFramesInFlight)},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             static_cast<std::uint32_t>(kFramesInFlight * 8U)},
        }};
        auto poolInfo =
            vkStructure<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
        poolInfo.maxSets = static_cast<std::uint32_t>(kFramesInFlight);
        poolInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        checkVk(vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool),
                "vkCreateDescriptorPool");

        const std::array<VkDescriptorSetLayout, kFramesInFlight> layouts{descriptorSetLayout,
                                                                         descriptorSetLayout};
        auto allocateInfo = vkStructure<VkDescriptorSetAllocateInfo>(
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
        allocateInfo.descriptorPool = descriptorPool;
        allocateInfo.descriptorSetCount = static_cast<std::uint32_t>(layouts.size());
        allocateInfo.pSetLayouts = layouts.data();
        std::array<VkDescriptorSet, kFramesInFlight> sets{};
        checkVk(vkAllocateDescriptorSets(device, &allocateInfo, sets.data()),
                "vkAllocateDescriptorSets");
        for (std::size_t index = 0; index < kFramesInFlight; ++index) {
            frames[index].descriptorSet = sets[index];
            VkDescriptorBufferInfo bufferInfo{};
            bufferInfo.buffer = frames[index].uniformBuffer.buffer;
            bufferInfo.range = sizeof(CameraUniform);
            VkDescriptorImageInfo imageInfo{};
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfo.imageView = textureView;
            imageInfo.sampler = textureSampler;
            VkDescriptorImageInfo fontImageInfo{};
            fontImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            fontImageInfo.imageView = fontTextureView;
            fontImageInfo.sampler = textureSampler;
            VkDescriptorImageInfo guiImageInfo{};
            guiImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            guiImageInfo.imageView = guiTextureView;
            guiImageInfo.sampler = textureSampler;
            VkDescriptorImageInfo entityImageInfo{};
            entityImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            entityImageInfo.imageView = entityTextureView;
            entityImageInfo.sampler = textureSampler;
            VkDescriptorImageInfo panoramaImageInfo{};
            panoramaImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            panoramaImageInfo.imageView = panoramaTextureView;
            panoramaImageInfo.sampler = panoramaSampler;
            VkDescriptorImageInfo biomeGrassImageInfo{};
            biomeGrassImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            biomeGrassImageInfo.imageView = biomeGrassView;
            biomeGrassImageInfo.sampler = biomeSampler;
            VkDescriptorImageInfo biomeFoliageImageInfo{};
            biomeFoliageImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            biomeFoliageImageInfo.imageView = biomeFoliageView;
            biomeFoliageImageInfo.sampler = biomeSampler;
            std::array<VkWriteDescriptorSet, 8> writes{};
            writes[0] = vkStructure<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
            writes[0].dstSet = sets[index];
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[0].pBufferInfo = &bufferInfo;
            writes[1] = vkStructure<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
            writes[1].dstSet = sets[index];
            writes[1].dstBinding = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[1].pImageInfo = &imageInfo;
            writes[2] = vkStructure<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
            writes[2].dstSet = sets[index];
            writes[2].dstBinding = 2;
            writes[2].descriptorCount = 1;
            writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[2].pImageInfo = &fontImageInfo;
            writes[3] = vkStructure<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
            writes[3].dstSet = sets[index];
            writes[3].dstBinding = 3;
            writes[3].descriptorCount = 1;
            writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[3].pImageInfo = &guiImageInfo;
            writes[4] = vkStructure<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
            writes[4].dstSet = sets[index];
            writes[4].dstBinding = 4;
            writes[4].descriptorCount = 1;
            writes[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[4].pImageInfo = &entityImageInfo;
            writes[5] = vkStructure<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
            writes[5].dstSet = sets[index];
            writes[5].dstBinding = 5;
            writes[5].descriptorCount = 1;
            writes[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[5].pImageInfo = &panoramaImageInfo;
            writes[6] = vkStructure<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
            writes[6].dstSet = sets[index];
            writes[6].dstBinding = 6;
            writes[6].descriptorCount = 1;
            writes[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[6].pImageInfo = &biomeGrassImageInfo;
            writes[7] = vkStructure<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
            writes[7].dstSet = sets[index];
            writes[7].dstBinding = 7;
            writes[7].descriptorCount = 1;
            writes[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[7].pImageInfo = &biomeFoliageImageInfo;
            vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()), writes.data(),
                                   0, nullptr);
        }
    }

    // The scene descriptor set (set 1) holds the per-frame storage buffer the
    // instanced particle pipeline reads. A separate layout keeps the shared
    // camera/texture set 0 untouched: none of the existing pipelines bind more
    // than one set, and the storage-buffer stage flags can grow to COMPUTE later
    // without disturbing them.
    void createSceneDescriptorResources() {
        // 3 MiB holds ~65,536 ParticleRecords: the 疯狂 particle level's
        // thundering rain (4000 x1.5 x3 x2 = 36,000 drops) plus the 9,000-particle
        // block-dust/splash budget all fit in one per-frame buffer.
        constexpr std::size_t kSceneBufferBytes = 3U * 1024U * 1024U;
        gpuSceneBuffer.init({&resources_, kFramesInFlight, kSceneBufferBytes});

        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        auto layoutInfo = vkStructure<VkDescriptorSetLayoutCreateInfo>(
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &binding;
        checkVk(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &sceneDescriptorSetLayout),
                "vkCreateDescriptorSetLayout(scene)");

        const VkDescriptorPoolSize poolSize{
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, static_cast<std::uint32_t>(kFramesInFlight)};
        auto poolInfo = vkStructure<VkDescriptorPoolCreateInfo>(
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
        poolInfo.maxSets = static_cast<std::uint32_t>(kFramesInFlight);
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        checkVk(vkCreateDescriptorPool(device, &poolInfo, nullptr, &sceneDescriptorPool),
                "vkCreateDescriptorPool(scene)");

        const std::array<VkDescriptorSetLayout, kFramesInFlight> layouts{sceneDescriptorSetLayout,
                                                                         sceneDescriptorSetLayout};
        auto allocateInfo = vkStructure<VkDescriptorSetAllocateInfo>(
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
        allocateInfo.descriptorPool = sceneDescriptorPool;
        allocateInfo.descriptorSetCount = static_cast<std::uint32_t>(layouts.size());
        allocateInfo.pSetLayouts = layouts.data();
        checkVk(vkAllocateDescriptorSets(device, &allocateInfo, sceneDescriptorSets.data()),
                "vkAllocateDescriptorSets(scene)");
        for (std::size_t index = 0; index < kFramesInFlight; ++index) {
            VkDescriptorBufferInfo bufferInfo{};
            bufferInfo.buffer = gpuSceneBuffer.frame(index).buffer;
            bufferInfo.range = VK_WHOLE_SIZE;
            auto write = vkStructure<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
            write.dstSet = sceneDescriptorSets[index];
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &bufferInfo;
            vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        }
    }

    // The instanced particle pipeline reads per-particle records from the scene
    // storage buffer (set 1) and expands the camera-facing quad in the vertex
    // shader, replacing the old per-particle vkCmdDraw with a single draw. Empty
    // vertex input: all particle data arrives through the SSBO + gl_InstanceIndex.
    void createParticlePipeline() {
        const auto vertexCode = readSpirv(shaderRoot / "particle_instanced.vert.spv");
        const auto fragmentCode = readSpirv(shaderRoot / "particle_instanced.frag.spv");
        const auto vertexModule = createShaderModule(vertexCode);
        const auto fragmentModule = createShaderModule(fragmentCode);
        auto vertexStage = vkStructure<VkPipelineShaderStageCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
        vertexStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertexStage.module = vertexModule;
        vertexStage.pName = "main";
        auto fragmentStage = vertexStage;
        fragmentStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragmentStage.module = fragmentModule;
        const std::array stages{vertexStage, fragmentStage};

        auto vertexInput = vkStructure<VkPipelineVertexInputStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
        auto inputAssembly = vkStructure<VkPipelineInputAssemblyStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        auto viewportState = vkStructure<VkPipelineViewportStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;
        auto rasterization = vkStructure<VkPipelineRasterizationStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
        // Camera-facing billboards have no meaningful back face.
        rasterization.cullMode = VK_CULL_MODE_NONE;
        rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterization.lineWidth = 1.0F;
        auto multisampling = vkStructure<VkPipelineMultisampleStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
        multisampling.rasterizationSamples = renderSampleCount();
        auto depthStencil = vkStructure<VkPipelineDepthStencilStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO);
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState colorAttachment{};
        colorAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        colorAttachment.blendEnable = VK_TRUE;
        colorAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        colorAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        colorAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        colorAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        auto blending = vkStructure<VkPipelineColorBlendStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
        blending.attachmentCount = 1;
        blending.pAttachments = &colorAttachment;
        const std::array dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        auto dynamic = vkStructure<VkPipelineDynamicStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
        dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
        dynamic.pDynamicStates = dynamicStates.data();

        const std::array setLayouts{descriptorSetLayout, sceneDescriptorSetLayout};
        auto layoutInfo =
            vkStructure<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
        layoutInfo.setLayoutCount = static_cast<std::uint32_t>(setLayouts.size());
        layoutInfo.pSetLayouts = setLayouts.data();
        checkVk(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &particlePipelineLayout),
                "vkCreatePipelineLayout(particle)");

        auto pipelineInfo = vkStructure<VkGraphicsPipelineCreateInfo>(
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
        pipelineInfo.stageCount = static_cast<std::uint32_t>(stages.size());
        pipelineInfo.pStages = stages.data();
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterization;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &blending;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = particlePipelineLayout;
        pipelineInfo.renderPass = renderPass;
        checkVk(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                          &particlePipeline),
                "vkCreateGraphicsPipelines(particle)");
        vkDestroyShaderModule(device, vertexModule, nullptr);
        vkDestroyShaderModule(device, fragmentModule, nullptr);
    }

    // The sun-space shadow pre-pass renders in-frustum terrain into an offscreen
    // depth map (Step B's offscreen/multi-pass validation). The pass needs no
    // descriptor sets: 80 bytes of push constants (light view-projection +
    // section origin) plus the same VoxelVertex buffers as the main pass. The
    // target, pipeline and layout are all swapchain-independent, created once.
    void createShadowResources() {
        shadowTarget.init({&resources_, device, 2048U, 2048U});

        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        push.offset = 0;
        push.size = sizeof(ShadowPush);
        auto layoutInfo =
            vkStructure<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
        layoutInfo.setLayoutCount = 0;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &push;
        checkVk(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &shadowPipelineLayout),
                "vkCreatePipelineLayout(shadow)");

        const auto vertexCode = readSpirv(shaderRoot / "shadow.vert.spv");
        const auto fragmentCode = readSpirv(shaderRoot / "shadow.frag.spv");
        const auto vertexModule = createShaderModule(vertexCode);
        const auto fragmentModule = createShaderModule(fragmentCode);
        auto vertexStage = vkStructure<VkPipelineShaderStageCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
        vertexStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertexStage.module = vertexModule;
        vertexStage.pName = "main";
        auto fragmentStage = vertexStage;
        fragmentStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragmentStage.module = fragmentModule;
        const std::array stages{vertexStage, fragmentStage};

        VkVertexInputBindingDescription binding{0, sizeof(VoxelVertex),
                                                VK_VERTEX_INPUT_RATE_VERTEX};
        const std::array<VkVertexInputAttributeDescription, 5> attributes{{
            {0, 0, VK_FORMAT_R16G16_UINT, offsetof(VoxelVertex, positionX)},
            {1, 0, VK_FORMAT_R16G16_UINT, offsetof(VoxelVertex, positionZ)},
            {2, 0, VK_FORMAT_R16G16_UINT, offsetof(VoxelVertex, uvX)},
            {3, 0, VK_FORMAT_R32_UINT, offsetof(VoxelVertex, textureLayer)},
            {4, 0, VK_FORMAT_R8G8B8A8_UINT, offsetof(VoxelVertex, skyLight)},
        }};
        auto vertexInput = vkStructure<VkPipelineVertexInputStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount =
            static_cast<std::uint32_t>(attributes.size());
        vertexInput.pVertexAttributeDescriptions = attributes.data();
        auto inputAssembly = vkStructure<VkPipelineInputAssemblyStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        auto viewportState = vkStructure<VkPipelineViewportStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;
        auto rasterization = vkStructure<VkPipelineRasterizationStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
        rasterization.cullMode = VK_CULL_MODE_BACK_BIT;
        rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterization.lineWidth = 1.0F;
        auto multisampling = vkStructure<VkPipelineMultisampleStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        auto depthStencil = vkStructure<VkPipelineDepthStencilStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO);
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
        // No color attachment: the depth-only pass needs no blend state.
        auto blending = vkStructure<VkPipelineColorBlendStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
        blending.attachmentCount = 0;
        const std::array dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        auto dynamic = vkStructure<VkPipelineDynamicStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
        dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
        dynamic.pDynamicStates = dynamicStates.data();

        auto pipelineInfo = vkStructure<VkGraphicsPipelineCreateInfo>(
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
        pipelineInfo.stageCount = static_cast<std::uint32_t>(stages.size());
        pipelineInfo.pStages = stages.data();
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterization;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &blending;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = shadowPipelineLayout;
        pipelineInfo.renderPass = shadowTarget.renderPass();
        checkVk(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                          &shadowPipeline),
                "vkCreateGraphicsPipelines(shadow)");
        vkDestroyShaderModule(device, vertexModule, nullptr);
        vkDestroyShaderModule(device, fragmentModule, nullptr);

        createShadowDebugResources();

        // Point every frame's set 0 at the shadow depth map (binding 8) so the
        // terrain shader can sample it. The image view and sampler exist now;
        // the map's contents are rewritten by the pre-pass each frame, which the
        // descriptor's SHADER_READ_ONLY layout already matches.
        for (std::size_t index = 0; index < kFramesInFlight; ++index) {
            VkDescriptorImageInfo shadowImageInfo{};
            shadowImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            shadowImageInfo.imageView = shadowTarget.view();
            shadowImageInfo.sampler = shadowDebugSampler;
            auto write = vkStructure<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
            write.dstSet = frames[index].descriptorSet;
            write.dstBinding = 8;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &shadowImageInfo;
            vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        }
    }

    // The shadow-map debug overlay samples the offscreen depth texture in a
    // corner quad, so the pre-pass's output is visible during development. The
    // set layout, sampler, descriptor set and pipeline layout are created once;
    // only the pipeline is swapchain-bound (it renders into the main pass with
    // the current MSAA sample count).
    void createShadowDebugResources() {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        auto layoutInfo = vkStructure<VkDescriptorSetLayoutCreateInfo>(
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &binding;
        checkVk(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &shadowDebugSetLayout),
                "vkCreateDescriptorSetLayout(shadow debug)");
        const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
        auto poolInfo = vkStructure<VkDescriptorPoolCreateInfo>(
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        checkVk(vkCreateDescriptorPool(device, &poolInfo, nullptr, &shadowDebugPool),
                "vkCreateDescriptorPool(shadow debug)");
        auto allocateInfo = vkStructure<VkDescriptorSetAllocateInfo>(
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
        allocateInfo.descriptorPool = shadowDebugPool;
        allocateInfo.descriptorSetCount = 1;
        allocateInfo.pSetLayouts = &shadowDebugSetLayout;
        checkVk(vkAllocateDescriptorSets(device, &allocateInfo, &shadowDebugSet),
                "vkAllocateDescriptorSets(shadow debug)");
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_NEAREST;
        samplerInfo.minFilter = VK_FILTER_NEAREST;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        checkVk(vkCreateSampler(device, &samplerInfo, nullptr, &shadowDebugSampler),
                "vkCreateSampler(shadow debug)");
        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView = shadowTarget.view();
        imageInfo.sampler = shadowDebugSampler;
        auto write = vkStructure<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        write.dstSet = shadowDebugSet;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        push.offset = 0;
        push.size = sizeof(glm::vec4);
        auto pushInfo = vkStructure<VkPipelineLayoutCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
        pushInfo.setLayoutCount = 1;
        pushInfo.pSetLayouts = &shadowDebugSetLayout;
        pushInfo.pushConstantRangeCount = 1;
        pushInfo.pPushConstantRanges = &push;
        checkVk(vkCreatePipelineLayout(device, &pushInfo, nullptr, &shadowDebugPipelineLayout),
                "vkCreatePipelineLayout(shadow debug)");
    }

    void createShadowDebugPipeline() {
        const auto vertexCode = readSpirv(shaderRoot / "shadow_debug.vert.spv");
        const auto fragmentCode = readSpirv(shaderRoot / "shadow_debug.frag.spv");
        const auto vertexModule = createShaderModule(vertexCode);
        const auto fragmentModule = createShaderModule(fragmentCode);
        auto vertexStage = vkStructure<VkPipelineShaderStageCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
        vertexStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertexStage.module = vertexModule;
        vertexStage.pName = "main";
        auto fragmentStage = vertexStage;
        fragmentStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragmentStage.module = fragmentModule;
        const std::array stages{vertexStage, fragmentStage};
        auto vertexInput = vkStructure<VkPipelineVertexInputStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
        auto inputAssembly = vkStructure<VkPipelineInputAssemblyStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        auto viewportState = vkStructure<VkPipelineViewportStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;
        auto rasterization = vkStructure<VkPipelineRasterizationStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
        rasterization.cullMode = VK_CULL_MODE_NONE;
        rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterization.lineWidth = 1.0F;
        auto multisampling = vkStructure<VkPipelineMultisampleStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
        multisampling.rasterizationSamples = renderSampleCount();
        auto depthStencil = vkStructure<VkPipelineDepthStencilStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO);
        depthStencil.depthTestEnable = VK_FALSE;
        depthStencil.depthWriteEnable = VK_FALSE;
        VkPipelineColorBlendAttachmentState colorAttachment{};
        colorAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        colorAttachment.blendEnable = VK_TRUE;
        colorAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        colorAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        colorAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        colorAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        auto blending = vkStructure<VkPipelineColorBlendStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
        blending.attachmentCount = 1;
        blending.pAttachments = &colorAttachment;
        const std::array dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        auto dynamic = vkStructure<VkPipelineDynamicStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
        dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
        dynamic.pDynamicStates = dynamicStates.data();
        auto pipelineInfo = vkStructure<VkGraphicsPipelineCreateInfo>(
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
        pipelineInfo.stageCount = static_cast<std::uint32_t>(stages.size());
        pipelineInfo.pStages = stages.data();
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterization;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &blending;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = shadowDebugPipelineLayout;
        pipelineInfo.renderPass = renderPass;
        checkVk(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                          &shadowDebugPipeline),
                "vkCreateGraphicsPipelines(shadow debug)");
        vkDestroyShaderModule(device, vertexModule, nullptr);
        vkDestroyShaderModule(device, fragmentModule, nullptr);
    }

    // Records the sun-space depth pre-pass ahead of the main render pass: cull
    // the loaded sections against the light frustum, draw each caster's opaque
    // layer with the shadow pipeline, then transition the depth image to
    // shader-readable for the main pass (and the debug overlay) to sample.
    // Recomputes the sun-space view-projection the shadow pre-pass writes and
    // the terrain shader samples with. Called once per frame BEFORE updateUniform
    // (which copies it into the UBO), so the matrix the shader projects with and
    // the matrix the pre-pass renders the depth map with are the same frame's —
    // no camera-movement lag between the two.
    void updateShadowMatrix() {
        if (shadowDisabled) {
            return;
        }
        const auto daylight = world::DayNightCycle::state(gameSession.gameTimeSeconds());
        const glm::vec3 sun = glm::normalize(daylight.sunDirection);
        const glm::vec3 eye = camera.position();
        const glm::mat4 lightView = glm::lookAt(eye + sun * 96.0F, eye - sun * 96.0F,
                                                glm::vec3{0.0F, 1.0F, 0.0F});
        const glm::mat4 lightProj = glm::ortho(-64.0F, 64.0F, -64.0F, 64.0F, 0.1F, 320.0F);
        shadowLightViewProj = lightProj * lightView;
    }

    void recordShadowPass(FrameContext& frame) {
        if (shadowDisabled) {
            return;
        }
        const glm::vec3 eye = camera.position();
        const Frustum lightFrustum(shadowLightViewProj);
        std::vector<const GpuMesh*> casters;
        casters.reserve(gpuMeshes.size());
        for (const auto& [position, mesh] : gpuMeshes) {
            static_cast<void>(position);
            if (lightFrustum.intersects(mesh.bounds)) {
                casters.push_back(&mesh);
            }
        }
        // Cap the pre-pass's draw load: when the eye flies high or the light
        // frustum spans a dense area the caster list can grow to thousands,
        // re-rendering all of them every frame is exactly the kind of heavy GPU
        // frame that can push the device toward a lost. Keep the nearest 512.
        constexpr std::size_t kMaxShadowCasters = 512;
        if (casters.size() > kMaxShadowCasters) {
            std::ranges::sort(casters, [&eye](const GpuMesh* first, const GpuMesh* second) {
                const glm::vec3 firstDelta =
                    (first->bounds.minimum + first->bounds.maximum) * 0.5F - eye;
                const glm::vec3 secondDelta =
                    (second->bounds.minimum + second->bounds.maximum) * 0.5F - eye;
                return glm::dot(firstDelta, firstDelta) < glm::dot(secondDelta, secondDelta);
            });
            casters.resize(kMaxShadowCasters);
        }
        // Always begin and end the pass (even with zero casters) so the depth
        // image's layout ends every frame in SHADER_READ_ONLY_OPTIMAL — the
        // debug overlay samples it unconditionally, and a frame that skipped the
        // transition would leave it in UNDEFINED and trip the validation layers.
        VkClearValue clear{};
        clear.depthStencil = {1.0F, 0};
        auto passInfo =
            vkStructure<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
        passInfo.renderPass = shadowTarget.renderPass();
        passInfo.framebuffer = shadowTarget.framebuffer();
        passInfo.renderArea.extent = {shadowTarget.width(), shadowTarget.height()};
        passInfo.clearValueCount = 1;
        passInfo.pClearValues = &clear;
        vkCmdBeginRenderPass(frame.commandBuffer, &passInfo, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport viewport{};
        viewport.width = static_cast<float>(shadowTarget.width());
        viewport.height = static_cast<float>(shadowTarget.height());
        viewport.maxDepth = 1.0F;
        vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
        VkRect2D scissor{{0, 0}, {shadowTarget.width(), shadowTarget.height()}};
        vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline);
        for (const auto* mesh : casters) {
            if (mesh->opaque.indexCount == 0U) {
                continue;
            }
            const ShadowPush push{shadowLightViewProj, glm::vec4{mesh->sectionOrigin, 1.0F}};
            vkCmdPushConstants(frame.commandBuffer, shadowPipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
            vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &mesh->vertexBuffer.buffer,
                                   &mesh->opaque.vertexOffset);
            vkCmdBindIndexBuffer(frame.commandBuffer, mesh->indexBuffer.buffer,
                                 mesh->opaque.indexOffset, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(frame.commandBuffer, mesh->opaque.indexCount, 1, 0, 0, 0);
        }
        vkCmdEndRenderPass(frame.commandBuffer);
        shadowTarget.transitionToShaderRead(frame.commandBuffer);
        static bool reported = false;
        if (!reported && !casters.empty()) {
            reported = true;
            std::cout << "[shadow] pre-pass " << casters.size() << " casters\n";
        }
    }

    // Debug-only overlay (MC_REBEDROCK_SHADOW_DEBUG=1): samples the shadow depth
    // texture into a top-right corner quad so the pre-pass's output is visible.
    void drawShadowDebugOverlay(VkCommandBuffer commandBuffer) const {
        if (!shadowDebugOverlay || shadowDisabled) {
            return;
        }
        const float width = static_cast<float>(swapchainExtent.width);
        const float height = static_cast<float>(swapchainExtent.height);
        constexpr float kSize = 256.0F;
        const glm::vec4 rect{
            1.0F - 2.0F * kSize / width, 1.0F - 2.0F * kSize / height,
            2.0F * kSize / width, 2.0F * kSize / height,
        };
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowDebugPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                shadowDebugPipelineLayout, 0, 1, &shadowDebugSet, 0, nullptr);
        vkCmdPushConstants(commandBuffer, shadowDebugPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(rect), &rect);
        vkCmdDraw(commandBuffer, 6U, 1, 0, 0);
    }

    [[nodiscard]] VkSurfaceFormatKHR
    chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const {
        for (const auto& format : formats) {
            if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
                format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                return format;
            }
        }
        return formats.front();
    }

    [[nodiscard]] VkPresentModeKHR
    choosePresentMode(const std::vector<VkPresentModeKHR>& modes, bool vsync) const {
        // FIFO is true vsync: the presentation engine waits for the display
        // refresh, capping the frame rate at the monitor with no tearing, and
        // costs no CPU (the swap blocks instead). Without it, MAILBOX presents
        // the latest ready image as fast as the app submits, dropping frames —
        // the "unlimited" path. MAILBOX is not universal, so FIFO is the
        // fallback either way.
        if (vsync) {
            return VK_PRESENT_MODE_FIFO_KHR;
        }
        return std::ranges::find(modes, VK_PRESENT_MODE_MAILBOX_KHR) != modes.end()
                   ? VK_PRESENT_MODE_MAILBOX_KHR
                   : VK_PRESENT_MODE_FIFO_KHR;
    }

    [[nodiscard]] VkExtent2D chooseExtent(const VkSurfaceCapabilitiesKHR& capabilities) const {
        if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
            return capabilities.currentExtent;
        }
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        VkExtent2D extent{static_cast<std::uint32_t>(std::max(width, 0)),
                          static_cast<std::uint32_t>(std::max(height, 0))};
        extent.width = std::clamp(extent.width, capabilities.minImageExtent.width,
                                  capabilities.maxImageExtent.width);
        extent.height = std::clamp(extent.height, capabilities.minImageExtent.height,
                                   capabilities.maxImageExtent.height);
        return extent;
    }

    void createSwapchain() {
        const auto support = querySwapchain(physicalDevice);
        const auto format = chooseSurfaceFormat(support.formats);
        swapchainExtent = chooseExtent(support.capabilities);
        std::uint32_t imageCount = support.capabilities.minImageCount + 1U;
        if (support.capabilities.maxImageCount > 0U &&
            imageCount > support.capabilities.maxImageCount) {
            imageCount = support.capabilities.maxImageCount;
        }
        auto info =
            vkStructure<VkSwapchainCreateInfoKHR>(VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR);
        info.surface = surface;
        info.minImageCount = imageCount;
        info.imageFormat = format.format;
        info.imageColorSpace = format.colorSpace;
        info.imageExtent = swapchainExtent;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        const std::uint32_t families[]{queueFamilies.graphics.value(),
                                       queueFamilies.present.value()};
        if (queueFamilies.graphics != queueFamilies.present) {
            info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            info.queueFamilyIndexCount = 2;
            info.pQueueFamilyIndices = families;
        } else {
            info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }
        info.preTransform = support.capabilities.currentTransform;
        info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        info.presentMode = choosePresentMode(support.presentModes, options.vsync);
        info.clipped = VK_TRUE;
        checkVk(vkCreateSwapchainKHR(device, &info, nullptr, &swapchain), "vkCreateSwapchainKHR");
        checkVk(vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr),
                "vkGetSwapchainImagesKHR");
        swapchainImages.resize(imageCount);
        checkVk(vkGetSwapchainImagesKHR(device, swapchain, &imageCount, swapchainImages.data()),
                "vkGetSwapchainImagesKHR");
        swapchainFormat = format.format;
        imagesInFlight.assign(imageCount, VK_NULL_HANDLE);
    }

    [[nodiscard]] VkImageView createImageView(VkImage image, VkFormat format,
                                              VkImageAspectFlags aspect) const {
        return resources_.createImageView(image, format, aspect);
    }

    void createSwapchainImageViews() {
        swapchainImageViews.reserve(swapchainImages.size());
        for (const auto image : swapchainImages) {
            swapchainImageViews.push_back(
                createImageView(image, swapchainFormat, VK_IMAGE_ASPECT_COLOR_BIT));
        }
    }

    [[nodiscard]] VkFormat chooseDepthFormat() const { return resources_.chooseDepthFormat(); }

    [[nodiscard]] bool depthFormatHasStencil(VkFormat format) const {
        return VulkanResources::depthFormatHasStencil(format);
    }

    [[nodiscard]] VkSampleCountFlagBits renderSampleCount() const {
        return options.antiAliasing ? maximumMsaaSamples : VK_SAMPLE_COUNT_1_BIT;
    }

    void createColorTargets() {
        if (renderSampleCount() == VK_SAMPLE_COUNT_1_BIT) return;
        colorTargets.resize(swapchainImages.size());
        for (auto& target : colorTargets) {
            target.image = createImage(
                swapchainExtent.width, swapchainExtent.height, 1, swapchainFormat,
                VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                renderSampleCount());
            target.view = createImageView(target.image.image, swapchainFormat,
                                          VK_IMAGE_ASPECT_COLOR_BIT);
        }
    }

    void createDepthTargets() {
        depthFormat = chooseDepthFormat();
        depthTargets.resize(swapchainImages.size());
        for (auto& target : depthTargets) {
            // The depth attachment is cleared, written and never read back
            // within the pass, so marking it transient lets tile-based GPUs
            // (Apple) keep it in on-tile memory instead of a ~250 MB
            // multi-sampled render-target allocation. The render pass already
            // uses loadOp CLEAR / storeOp DONT_CARE, which is the memoryless
            // shape; drivers without memoryless support degrade to a normal
            // allocation with no correctness change.
            target.image = createImage(
                swapchainExtent.width, swapchainExtent.height, 1, depthFormat,
                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                    VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                renderSampleCount());
            VkImageAspectFlags aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
            if (depthFormatHasStencil(depthFormat)) {
                aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
            }
            target.view = createImageView(target.image.image, depthFormat, aspect);
        }
    }

    void createRenderPass() {
        VkAttachmentDescription color{};
        color.format = swapchainFormat;
        color.samples = renderSampleCount();
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = renderSampleCount() == VK_SAMPLE_COUNT_1_BIT
            ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        // Note: a resolved MSAA color attachment keeps COLOR_ATTACHMENT_OPTIMAL
        // here; UNDEFINED is rejected by the validation layers and this MoltenVK
        // build does not map transient attachments to on-tile memory anyway.
        color.finalLayout = renderSampleCount() == VK_SAMPLE_COUNT_1_BIT
            ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentDescription depth{};
        depth.format = depthFormat;
        depth.samples = renderSampleCount();
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentDescription resolve{};
        resolve.format = swapchainFormat;
        resolve.samples = VK_SAMPLE_COUNT_1_BIT;
        resolve.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        resolve.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        resolve.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        resolve.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        resolve.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        resolve.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        const std::array attachments{color, depth, resolve};
        VkAttachmentReference colorReference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depthReference{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkAttachmentReference resolveReference{2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorReference;
        subpass.pDepthStencilAttachment = &depthReference;
        if (renderSampleCount() != VK_SAMPLE_COUNT_1_BIT) {
            subpass.pResolveAttachments = &resolveReference;
        }
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstStageMask = dependency.srcStageMask;
        dependency.dstAccessMask =
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        auto info = vkStructure<VkRenderPassCreateInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);
        info.attachmentCount = renderSampleCount() == VK_SAMPLE_COUNT_1_BIT
            ? 2U : static_cast<std::uint32_t>(attachments.size());
        info.pAttachments = attachments.data();
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        info.dependencyCount = 1;
        info.pDependencies = &dependency;
        checkVk(vkCreateRenderPass(device, &info, nullptr, &renderPass), "vkCreateRenderPass");
    }

    [[nodiscard]] VkShaderModule createShaderModule(const std::vector<std::uint32_t>& code) const {
        auto info =
            vkStructure<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
        info.codeSize = code.size() * sizeof(std::uint32_t);
        info.pCode = code.data();
        VkShaderModule module = VK_NULL_HANDLE;
        checkVk(vkCreateShaderModule(device, &info, nullptr, &module), "vkCreateShaderModule");
        return module;
    }

    void createGraphicsPipeline() {
        const auto vertexCode = readSpirv(shaderRoot / "grass_block.vert.spv");
        const auto fragmentCode = readSpirv(shaderRoot / "grass_block.frag.spv");
        const auto vertexModule = createShaderModule(vertexCode);
        const auto fragmentModule = createShaderModule(fragmentCode);
        auto vertexStage = vkStructure<VkPipelineShaderStageCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
        vertexStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertexStage.module = vertexModule;
        vertexStage.pName = "main";
        auto fragmentStage = vertexStage;
        fragmentStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragmentStage.module = fragmentModule;
        const std::array stages{vertexStage, fragmentStage};

        VkVertexInputBindingDescription binding{0, sizeof(VoxelVertex),
                                                VK_VERTEX_INPUT_RATE_VERTEX};
        // PackedVoxelVertex: five 4-byte-aligned integer attributes. The pad
        // byte inside location 1 carries the fragment's biome mask (which
        // biome-colour lookup to apply); the 24-byte vertex keeps spare tint
        // bytes after the lights that are simply not consumed.
        const std::array<VkVertexInputAttributeDescription, 5> attributes{{
            {0, 0, VK_FORMAT_R16G16_UINT, offsetof(VoxelVertex, positionX)},
            {1, 0, VK_FORMAT_R16G16_UINT, offsetof(VoxelVertex, positionZ)},
            {2, 0, VK_FORMAT_R16G16_UINT, offsetof(VoxelVertex, uvX)},
            {3, 0, VK_FORMAT_R32_UINT, offsetof(VoxelVertex, textureLayer)},
            {4, 0, VK_FORMAT_R8G8B8A8_UINT, offsetof(VoxelVertex, skyLight)},
        }};
        auto vertexInput = vkStructure<VkPipelineVertexInputStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
        vertexInput.pVertexAttributeDescriptions = attributes.data();
        auto inputAssembly = vkStructure<VkPipelineInputAssemblyStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        auto viewportState = vkStructure<VkPipelineViewportStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;
        auto rasterization = vkStructure<VkPipelineRasterizationStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
        rasterization.cullMode = VK_CULL_MODE_BACK_BIT;
        rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterization.lineWidth = 1.0F;
        auto multisampling = vkStructure<VkPipelineMultisampleStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
        multisampling.rasterizationSamples = renderSampleCount();
        auto depthStencil = vkStructure<VkPipelineDepthStencilStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO);
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState colorAttachment{};
        colorAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        auto blending = vkStructure<VkPipelineColorBlendStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
        blending.attachmentCount = 1;
        blending.pAttachments = &colorAttachment;
        const std::array dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        auto dynamic = vkStructure<VkPipelineDynamicStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
        dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
        dynamic.pDynamicStates = dynamicStates.data();
        // Terrain meshes store positions relative to their section origin; the
        // origin is pushed per draw so the vertex shader can rebuild world
        // coordinates. The sky pass shares this layout and ignores the range.
        VkPushConstantRange terrainPushConstant{};
        terrainPushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        terrainPushConstant.offset = 0;
        terrainPushConstant.size = sizeof(glm::vec4);
        auto layoutInfo =
            vkStructure<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &descriptorSetLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &terrainPushConstant;
        checkVk(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout),
                "vkCreatePipelineLayout");
        auto pipelineInfo = vkStructure<VkGraphicsPipelineCreateInfo>(
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
        pipelineInfo.stageCount = static_cast<std::uint32_t>(stages.size());
        pipelineInfo.pStages = stages.data();
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterization;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &blending;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = pipelineLayout;
        pipelineInfo.renderPass = renderPass;
        const auto result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                                      nullptr, &graphicsPipeline);
        checkVk(result, "vkCreateGraphicsPipelines");

        depthStencil.depthWriteEnable = VK_FALSE;
        colorAttachment.blendEnable = VK_TRUE;
        colorAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        colorAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        colorAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        colorAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        const auto translucentResult = vkCreateGraphicsPipelines(
            device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &translucentPipeline);
        checkVk(translucentResult, "vkCreateGraphicsPipelines(translucent)");
        depthStencil.depthWriteEnable = VK_TRUE;
        colorAttachment.blendEnable = VK_FALSE;

        const auto cutoutFragmentCode = readSpirv(shaderRoot / "block_cutout.frag.spv");
        const auto cutoutFragmentModule = createShaderModule(cutoutFragmentCode);
        auto cutoutFragmentStage = fragmentStage;
        cutoutFragmentStage.module = cutoutFragmentModule;
        const std::array cutoutStages{vertexStage, cutoutFragmentStage};
        // Cutout-mipped leaves in Java use normal back-face culling. Plants
        // carry explicit reverse-winding triangles, so they stay two-sided.
        rasterization.cullMode = VK_CULL_MODE_BACK_BIT;
        pipelineInfo.pStages = cutoutStages.data();
        const auto cutoutResult = vkCreateGraphicsPipelines(
            device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &cutoutPipeline);
        vkDestroyShaderModule(device, cutoutFragmentModule, nullptr);
        vkDestroyShaderModule(device, fragmentModule, nullptr);
        vkDestroyShaderModule(device, vertexModule, nullptr);
        checkVk(cutoutResult, "vkCreateGraphicsPipelines(cutout)");

        const auto outlineVertexCode = readSpirv(shaderRoot / "block_outline.vert.spv");
        const auto outlineFragmentCode = readSpirv(shaderRoot / "block_outline.frag.spv");
        const auto outlineVertexModule = createShaderModule(outlineVertexCode);
        const auto outlineFragmentModule = createShaderModule(outlineFragmentCode);
        vertexStage.module = outlineVertexModule;
        fragmentStage.module = outlineFragmentModule;
        const std::array outlineStages{vertexStage, fragmentStage};

        auto outlineVertexInput = vkStructure<VkPipelineVertexInputStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        rasterization.cullMode = VK_CULL_MODE_NONE;
        depthStencil.depthWriteEnable = VK_FALSE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPushConstantRange pushConstant{};
        pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pushConstant.size = sizeof(glm::vec4) * 3; // blockOrigin + boundsMin + boundsMax
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushConstant;
        checkVk(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &outlinePipelineLayout),
                "vkCreatePipelineLayout(outline)");
        pipelineInfo.pStages = outlineStages.data();
        pipelineInfo.pVertexInputState = &outlineVertexInput;
        pipelineInfo.layout = outlinePipelineLayout;
        const auto outlineResult = vkCreateGraphicsPipelines(
            device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &outlinePipeline);
        checkVk(outlineResult, "vkCreateGraphicsPipelines(outline)");
        depthStencil.depthTestEnable = VK_FALSE;
        vkDestroyShaderModule(device, outlineFragmentModule, nullptr);
        vkDestroyShaderModule(device, outlineVertexModule, nullptr);

        const auto skyVertexCode = readSpirv(shaderRoot / "sky.vert.spv");
        const auto skyFragmentCode = readSpirv(shaderRoot / "sky.frag.spv");
        const auto skyVertexModule = createShaderModule(skyVertexCode);
        const auto skyFragmentModule = createShaderModule(skyFragmentCode);
        vertexStage.module = skyVertexModule;
        fragmentStage.module = skyFragmentModule;
        const std::array skyStages{vertexStage, fragmentStage};
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        pipelineInfo.pStages = skyStages.data();
        pipelineInfo.layout = pipelineLayout;
        const auto skyResult = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                                         nullptr, &skyPipeline);
        vkDestroyShaderModule(device, skyFragmentModule, nullptr);
        vkDestroyShaderModule(device, skyVertexModule, nullptr);
        checkVk(skyResult, "vkCreateGraphicsPipelines(sky)");

        const auto hudVertexCode = readSpirv(shaderRoot / "hud.vert.spv");
        const auto hudFragmentCode = readSpirv(shaderRoot / "hud.frag.spv");
        const auto hudVertexModule = createShaderModule(hudVertexCode);
        const auto hudFragmentModule = createShaderModule(hudFragmentCode);
        vertexStage.module = hudVertexModule;
        fragmentStage.module = hudFragmentModule;
        const std::array hudStages{vertexStage, fragmentStage};
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        rasterization.cullMode = VK_CULL_MODE_NONE;
        depthStencil.depthTestEnable = VK_FALSE;
        depthStencil.depthWriteEnable = VK_FALSE;
        colorAttachment.blendEnable = VK_TRUE;
        colorAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        colorAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        colorAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        colorAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        VkPushConstantRange hudPushConstant{};
        hudPushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        hudPushConstant.size = sizeof(HudPush);
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &hudPushConstant;
        checkVk(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &hudPipelineLayout),
                "vkCreatePipelineLayout(hud)");
        pipelineInfo.pStages = hudStages.data();
        pipelineInfo.pVertexInputState = &outlineVertexInput;
        pipelineInfo.layout = hudPipelineLayout;
        const auto hudResult = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                                         nullptr, &hudPipeline);
        checkVk(hudResult, "vkCreateGraphicsPipelines(hud)");

        // Title-screen panorama cube: a fullscreen triangle whose fragment
        // shader ray-marches the six panorama faces (1.16.1's CubeMap). It
        // only needs the panorama sampler from the shared descriptor set plus
        // the yaw/pitch/FOV push constant.
        const auto panoramaVertexCode = readSpirv(shaderRoot / "panorama.vert.spv");
        const auto panoramaFragmentCode = readSpirv(shaderRoot / "panorama.frag.spv");
        const auto panoramaVertexModule = createShaderModule(panoramaVertexCode);
        const auto panoramaFragmentModule = createShaderModule(panoramaFragmentCode);
        vertexStage.module = panoramaVertexModule;
        fragmentStage.module = panoramaFragmentModule;
        const std::array panoramaStages{vertexStage, fragmentStage};
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        rasterization.cullMode = VK_CULL_MODE_NONE;
        depthStencil.depthTestEnable = VK_FALSE;
        depthStencil.depthWriteEnable = VK_FALSE;
        colorAttachment.blendEnable = VK_TRUE;
        colorAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        colorAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        colorAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        colorAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        VkPushConstantRange panoramaPushConstant{};
        panoramaPushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        panoramaPushConstant.size = sizeof(PanoramaPush);
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &panoramaPushConstant;
        checkVk(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &panoramaPipelineLayout),
                "vkCreatePipelineLayout(panorama)");
        pipelineInfo.pStages = panoramaStages.data();
        pipelineInfo.pVertexInputState = &outlineVertexInput;
        pipelineInfo.layout = panoramaPipelineLayout;
        const auto panoramaResult = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
                                                              &pipelineInfo, nullptr,
                                                              &panoramaPipeline);
        vkDestroyShaderModule(device, panoramaFragmentModule, nullptr);
        vkDestroyShaderModule(device, panoramaVertexModule, nullptr);
        checkVk(panoramaResult, "vkCreateGraphicsPipelines(panorama)");

        // The crosshair and vignette pipelines below reuse the shared
        // pipelineInfo, so put the hud layout and stage array back (the
        // panorama block swapped them for its own 16-byte push-constant layout
        // and a local stage array that is now out of scope).
        pipelineInfo.layout = hudPipelineLayout;
        pipelineInfo.pStages = hudStages.data();

        // Minecraft 1.16.1 draws icons.png's 15x15 crosshair with an
        // inversion blend so it remains visible over both bright and dark
        // terrain. This is intentionally a separate blend-state pipeline.
        colorAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        colorAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        colorAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        colorAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        const auto crosshairResult = vkCreateGraphicsPipelines(
            device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &crosshairPipeline);
        checkVk(crosshairResult, "vkCreateGraphicsPipelines(crosshair)");

        // 1.16.1's InGameHud draws the vignette texture with a multiplicative
        // blend (dst * (1 - src)) so dark corners darken the scene while the
        // centre is untouched. Another dedicated blend-state pipeline.
        colorAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        colorAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        colorAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        colorAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        const auto vignetteResult = vkCreateGraphicsPipelines(
            device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &vignettePipeline);
        checkVk(vignetteResult, "vkCreateGraphicsPipelines(vignette)");

        colorAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        colorAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        colorAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        vkDestroyShaderModule(device, hudFragmentModule, nullptr);
        vkDestroyShaderModule(device, hudVertexModule, nullptr);

        const auto itemVertexCode = readSpirv(shaderRoot / "item_entity.vert.spv");
        const auto itemFragmentCode = readSpirv(shaderRoot / "item_entity.frag.spv");
        const auto itemVertexModule = createShaderModule(itemVertexCode);
        const auto itemFragmentModule = createShaderModule(itemFragmentCode);
        vertexStage.module = itemVertexModule;
        fragmentStage.module = itemFragmentModule;
        const std::array itemStages{vertexStage, fragmentStage};
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        VkPushConstantRange itemPushConstant{};
        itemPushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        itemPushConstant.size = sizeof(ItemPush);
        layoutInfo.pPushConstantRanges = &itemPushConstant;
        checkVk(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &itemPipelineLayout),
                "vkCreatePipelineLayout(item)");
        pipelineInfo.pStages = itemStages.data();
        pipelineInfo.layout = itemPipelineLayout;
        const auto itemResult = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                                          nullptr, &itemPipeline);
        checkVk(itemResult, "vkCreateGraphicsPipelines(item)");
        depthStencil.depthWriteEnable = VK_FALSE;
        const auto itemShadowResult = vkCreateGraphicsPipelines(
            device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &itemShadowPipeline);
        checkVk(itemShadowResult, "vkCreateGraphicsPipelines(item shadow)");
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
        const auto heldItemResult = vkCreateGraphicsPipelines(
            device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &heldItemPipeline);
        checkVk(heldItemResult, "vkCreateGraphicsPipelines(held item)");
        vkDestroyShaderModule(device, itemFragmentModule, nullptr);
        vkDestroyShaderModule(device, itemVertexModule, nullptr);
    }

    // Device-level occlusion-query resources: the query pool, the pipeline
    // layout that carries the per-draw AABB push constants, and the unit-cube
    // vertex/index buffers the query pass draws for every section.
    void createOcclusionQueryResources() {
        auto poolInfo =
            vkStructure<VkQueryPoolCreateInfo>(VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);
        poolInfo.queryType = VK_QUERY_TYPE_OCCLUSION;
        poolInfo.queryCount = static_cast<std::uint32_t>(kOcclusionQueryPoolSize);
        checkVk(vkCreateQueryPool(device, &poolInfo, nullptr, &occlusionQueryPool),
                "vkCreateQueryPool(occlusion)");

        constexpr VkPushConstantRange pushRange{
            VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(OcclusionQueryPushConstants)};
        auto layoutInfo =
            vkStructure<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &descriptorSetLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
        checkVk(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &occlusionQueryLayout),
                "vkCreatePipelineLayout(occlusion query)");

        // A unit cube in [0,1]^3; the query vertex shader expands it to each
        // section's AABB with per-draw push constants. Culling is off and the
        // winding is irrelevant, so the box is tested from any viewpoint.
        constexpr std::array<glm::vec3, 8> kUnitCubeCorners{{
            {0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F},
            {0.0F, 1.0F, 0.0F}, {1.0F, 1.0F, 0.0F},
            {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 1.0F},
            {0.0F, 1.0F, 1.0F}, {1.0F, 1.0F, 1.0F},
        }};
        constexpr std::array<std::uint32_t, 36> kUnitCubeIndices{{
            0, 2, 3, 0, 3, 1,  // -y
            4, 6, 7, 4, 7, 5,  // +y
            0, 4, 6, 0, 6, 2,  // -x
            1, 3, 7, 1, 7, 5,  // +x
            0, 1, 5, 0, 5, 4,  // -z
            2, 3, 7, 2, 7, 6,  // +z
        }};
        occlusionBoxVertexBuffer = createBuffer(
            sizeof(kUnitCubeCorners), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
        std::memcpy(occlusionBoxVertexBuffer.mapped, kUnitCubeCorners.data(),
                    sizeof(kUnitCubeCorners));
        checkVk(vmaFlushAllocation(allocator, occlusionBoxVertexBuffer.allocation, 0,
                                   VK_WHOLE_SIZE),
                "vmaFlushAllocation(occlusion box vertices)");
        occlusionBoxIndexBuffer = createBuffer(
            sizeof(kUnitCubeIndices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
        std::memcpy(occlusionBoxIndexBuffer.mapped, kUnitCubeIndices.data(),
                    sizeof(kUnitCubeIndices));
        checkVk(vmaFlushAllocation(allocator, occlusionBoxIndexBuffer.allocation, 0,
                                   VK_WHOLE_SIZE),
                "vmaFlushAllocation(occlusion box indices)");
    }

    // Swapchain-coupled: the query pipeline binds the current render pass, so
    // it is rebuilt alongside the other pipelines when the swapchain changes.
    void createOcclusionQueryPipeline() {
        const auto vertexCode = readSpirv(shaderRoot / "occlusion_query.vert.spv");
        const auto fragmentCode = readSpirv(shaderRoot / "occlusion_query.frag.spv");
        const auto vertexModule = createShaderModule(vertexCode);
        const auto fragmentModule = createShaderModule(fragmentCode);
        auto vertexStage = vkStructure<VkPipelineShaderStageCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
        vertexStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertexStage.module = vertexModule;
        vertexStage.pName = "main";
        auto fragmentStage = vertexStage;
        fragmentStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragmentStage.module = fragmentModule;
        const std::array stages{vertexStage, fragmentStage};

        VkVertexInputBindingDescription binding{0, sizeof(glm::vec3),
                                                VK_VERTEX_INPUT_RATE_VERTEX};
        const std::array<VkVertexInputAttributeDescription, 1> attributes{{
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
        }};
        auto vertexInput = vkStructure<VkPipelineVertexInputStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount =
            static_cast<std::uint32_t>(attributes.size());
        vertexInput.pVertexAttributeDescriptions = attributes.data();
        auto inputAssembly = vkStructure<VkPipelineInputAssemblyStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        auto viewportState = vkStructure<VkPipelineViewportStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;
        auto rasterization = vkStructure<VkPipelineRasterizationStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
        rasterization.cullMode = VK_CULL_MODE_NONE;
        rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterization.lineWidth = 1.0F;
        auto multisampling = vkStructure<VkPipelineMultisampleStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
        multisampling.rasterizationSamples = renderSampleCount();
        auto depthStencil = vkStructure<VkPipelineDepthStencilStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO);
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_FALSE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState colorAttachment{};
        colorAttachment.colorWriteMask = 0;
        auto blending = vkStructure<VkPipelineColorBlendStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
        blending.attachmentCount = 1;
        blending.pAttachments = &colorAttachment;
        const std::array dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        auto dynamic = vkStructure<VkPipelineDynamicStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
        dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
        dynamic.pDynamicStates = dynamicStates.data();
        auto pipelineInfo = vkStructure<VkGraphicsPipelineCreateInfo>(
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
        pipelineInfo.stageCount = static_cast<std::uint32_t>(stages.size());
        pipelineInfo.pStages = stages.data();
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterization;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &blending;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = occlusionQueryLayout;
        pipelineInfo.renderPass = renderPass;
        const auto result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                                      nullptr, &occlusionQueryPipeline);
        checkVk(result, "vkCreateGraphicsPipelines(occlusion query)");
        vkDestroyShaderModule(device, vertexModule, nullptr);
        vkDestroyShaderModule(device, fragmentModule, nullptr);
    }

    void createFramebuffers() {
        framebuffers.resize(swapchainImageViews.size());
        for (std::size_t index = 0; index < framebuffers.size(); ++index) {
            std::array<VkImageView, 3> attachments{};
            std::uint32_t attachmentCount = 2U;
            if (renderSampleCount() == VK_SAMPLE_COUNT_1_BIT) {
                attachments[0] = swapchainImageViews[index];
                attachments[1] = depthTargets[index].view;
            } else {
                attachments[0] = colorTargets[index].view;
                attachments[1] = depthTargets[index].view;
                attachments[2] = swapchainImageViews[index];
                attachmentCount = 3U;
            }
            auto info =
                vkStructure<VkFramebufferCreateInfo>(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
            info.renderPass = renderPass;
            info.attachmentCount = attachmentCount;
            info.pAttachments = attachments.data();
            info.width = swapchainExtent.width;
            info.height = swapchainExtent.height;
            info.layers = 1;
            checkVk(vkCreateFramebuffer(device, &info, nullptr, &framebuffers[index]),
                    "vkCreateFramebuffer");
        }
    }

    void createSwapchainResources() {
        createSwapchain();
        createPresentSemaphores();
        createSwapchainImageViews();
        createColorTargets();
        createDepthTargets();
        createRenderPass();
        createGraphicsPipeline();
        createOcclusionQueryPipeline();
        createParticlePipeline();
        createShadowDebugPipeline();
        createRainSheetPipeline();
        createFramebuffers();
    }

    void cleanupSwapchain() noexcept {
        for (const auto framebuffer : framebuffers) {
            vkDestroyFramebuffer(device, framebuffer, nullptr);
        }
        framebuffers.clear();
        if (graphicsPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, graphicsPipeline, nullptr);
            graphicsPipeline = VK_NULL_HANDLE;
        }
        if (translucentPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, translucentPipeline, nullptr);
            translucentPipeline = VK_NULL_HANDLE;
        }
        if (cutoutPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, cutoutPipeline, nullptr);
            cutoutPipeline = VK_NULL_HANDLE;
        }
        if (skyPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, skyPipeline, nullptr);
            skyPipeline = VK_NULL_HANDLE;
        }
        if (outlinePipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, outlinePipeline, nullptr);
            outlinePipeline = VK_NULL_HANDLE;
        }
        if (occlusionQueryPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, occlusionQueryPipeline, nullptr);
            occlusionQueryPipeline = VK_NULL_HANDLE;
        }
        if (crosshairPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, crosshairPipeline, nullptr);
            crosshairPipeline = VK_NULL_HANDLE;
        }
        if (vignettePipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, vignettePipeline, nullptr);
            vignettePipeline = VK_NULL_HANDLE;
        }
        if (hudPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, hudPipeline, nullptr);
            hudPipeline = VK_NULL_HANDLE;
        }
        if (panoramaPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, panoramaPipeline, nullptr);
            panoramaPipeline = VK_NULL_HANDLE;
        }
        if (itemPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, itemPipeline, nullptr);
            itemPipeline = VK_NULL_HANDLE;
        }
        if (itemShadowPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, itemShadowPipeline, nullptr);
            itemShadowPipeline = VK_NULL_HANDLE;
        }
        if (heldItemPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, heldItemPipeline, nullptr);
            heldItemPipeline = VK_NULL_HANDLE;
        }
        if (particlePipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, particlePipeline, nullptr);
            particlePipeline = VK_NULL_HANDLE;
        }
        if (particlePipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, particlePipelineLayout, nullptr);
            particlePipelineLayout = VK_NULL_HANDLE;
        }
        if (shadowDebugPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, shadowDebugPipeline, nullptr);
            shadowDebugPipeline = VK_NULL_HANDLE;
        }
        if (rainSheetPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, rainSheetPipeline, nullptr);
            rainSheetPipeline = VK_NULL_HANDLE;
        }
        if (rainSheetPipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, rainSheetPipelineLayout, nullptr);
            rainSheetPipelineLayout = VK_NULL_HANDLE;
        }
        if (pipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
            pipelineLayout = VK_NULL_HANDLE;
        }
        if (outlinePipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, outlinePipelineLayout, nullptr);
            outlinePipelineLayout = VK_NULL_HANDLE;
        }
        if (hudPipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, hudPipelineLayout, nullptr);
            hudPipelineLayout = VK_NULL_HANDLE;
        }
        if (panoramaPipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, panoramaPipelineLayout, nullptr);
            panoramaPipelineLayout = VK_NULL_HANDLE;
        }
        if (itemPipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, itemPipelineLayout, nullptr);
            itemPipelineLayout = VK_NULL_HANDLE;
        }
        if (renderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device, renderPass, nullptr);
            renderPass = VK_NULL_HANDLE;
        }
        for (auto& target : depthTargets) {
            if (target.view != VK_NULL_HANDLE) {
                vkDestroyImageView(device, target.view, nullptr);
            }
            if (allocator != VK_NULL_HANDLE) {
                destroyImage(target.image);
            }
        }
        depthTargets.clear();
        for (auto& target : colorTargets) {
            if (target.view != VK_NULL_HANDLE) {
                vkDestroyImageView(device, target.view, nullptr);
            }
            if (allocator != VK_NULL_HANDLE) destroyImage(target.image);
        }
        colorTargets.clear();
        for (const auto semaphore : presentSemaphores) {
            vkDestroySemaphore(device, semaphore, nullptr);
        }
        presentSemaphores.clear();
        for (const auto view : swapchainImageViews) {
            vkDestroyImageView(device, view, nullptr);
        }
        swapchainImageViews.clear();
        swapchainImages.clear();
        if (swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device, swapchain, nullptr);
            swapchain = VK_NULL_HANDLE;
        }
    }

    void recreateSwapchain() {
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        while ((width == 0 || height == 0) && glfwWindowShouldClose(window) == GLFW_FALSE) {
            glfwWaitEvents();
            glfwGetFramebufferSize(window, &width, &height);
        }
        if (glfwWindowShouldClose(window) == GLFW_TRUE) {
            return;
        }
        checkVk(vkDeviceWaitIdle(device), "vkDeviceWaitIdle");
        cleanupSwapchain();
        createSwapchainResources();
    }

    void createCommandBuffers() {
        auto info = vkStructure<VkCommandBufferAllocateInfo>(
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
        info.commandPool = commandPool;
        info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        info.commandBufferCount = static_cast<std::uint32_t>(frames.size());
        std::array<VkCommandBuffer, kFramesInFlight> commandBuffers{};
        checkVk(vkAllocateCommandBuffers(device, &info, commandBuffers.data()),
                "vkAllocateCommandBuffers");
        for (std::size_t index = 0; index < frames.size(); ++index) {
            frames[index].commandBuffer = commandBuffers[index];
        }
    }

    void createSyncObjects() {
        auto semaphoreInfo =
            vkStructure<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
        auto fenceInfo = vkStructure<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (auto& frame : frames) {
            checkVk(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &frame.imageAvailable),
                    "vkCreateSemaphore");
            checkVk(vkCreateFence(device, &fenceInfo, nullptr, &frame.inFlight), "vkCreateFence");
        }
    }

    void createPresentSemaphores() {
        auto info = vkStructure<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
        presentSemaphores.resize(swapchainImages.size(), VK_NULL_HANDLE);
        for (auto& semaphore : presentSemaphores) {
            checkVk(vkCreateSemaphore(device, &info, nullptr, &semaphore),
                    "vkCreateSemaphore(present)");
        }
    }

    [[nodiscard]] bool cameraSubmergedInWater() const {
        return submergedInWater(camera.position());
    }

    // Whether the point sits below the water surface of the cell it is in.
    [[nodiscard]] bool submergedInWater(glm::vec3 position) const {
        const int x = static_cast<int>(std::floor(position.x));
        const int y = static_cast<int>(std::floor(position.y));
        const int z = static_cast<int>(std::floor(position.z));
        if (!world::isFluid(interactionWorld.block(x, y, z))) {
            return false;
        }
        float surfaceHeight = 1.0F;
        if (!world::isFluid(interactionWorld.block(x, y + 1, z))) {
            const std::uint8_t level = interactionWorld.fluidLevel(x, y, z);
            surfaceHeight = level >= 8U ? 1.0F : static_cast<float>(8U - level) / 9.0F;
        }
        return position.y < static_cast<float>(y) + surfaceHeight;
    }

    [[nodiscard]] float renderDistanceBlocks() const {
        return static_cast<float>(std::max(viewDistanceChunks, 2) * world::kChunkWidth);
    }

    [[nodiscard]] float cameraFarPlane() const {
        return std::max(renderDistanceBlocks() + 32.0F, 100.0F);
    }

    [[nodiscard]] glm::mat4 viewBobbingMatrix() const {
        if (!options.viewBobbing || gameSession.player().flying()) return glm::mat4{1.0F};
        const float alpha = renderInterpolationAlpha;
        const float phase = -std::lerp(gameSession.player().previousHorizontalSpeed(),
                                       gameSession.player().horizontalSpeed(), alpha);
        const float stride = std::lerp(gameSession.player().previousStrideDistance(),
                                       gameSession.player().strideDistance(), alpha);
        constexpr float pi = 3.14159265358979323846F;
        glm::mat4 transform{1.0F};
        transform = glm::translate(
            transform,
            {std::sin(phase * pi) * stride * 0.5F,
             -std::abs(std::cos(phase * pi) * stride), 0.0F});
        transform = glm::rotate(transform,
                                glm::radians(std::sin(phase * pi) * stride * 3.0F),
                                {0.0F, 0.0F, 1.0F});
        transform = glm::rotate(
            transform,
            glm::radians(std::abs(std::cos(phase * pi - 0.2F) * stride) * 5.0F),
            {1.0F, 0.0F, 0.0F});
        return transform;
    }

    // The actual eye the scene is rendered from. The camera object always sits
    // at the gameSession.player()'s eye; third person pulls the render eye back (or pushes it
    // in front, looking back) along the look direction. Both the view matrix and
    // the culling frustum are derived from this so they always agree.
    struct RenderEye {
        glm::vec3 position{0.0F};
        glm::vec3 forward{0.0F, 0.0F, 1.0F};
    };
    [[nodiscard]] RenderEye renderEyeState() const {
        const glm::vec3 eyePivot = camera.position();
        const glm::vec3 forwardDir = camera.direction();
        RenderEye result{eyePivot, forwardDir};
        if (cameraPerspective == CameraPerspective::FirstPerson) {
            return result;
        }
        constexpr float kThirdPersonDistance = 4.0F;
        const bool front = cameraPerspective == CameraPerspective::ThirdPersonFront;
        const glm::vec3 boomDirection = front ? forwardDir : -forwardDir;
        // Pull the camera in when a solid block is between it and the gameSession.player() so it
        // never clips through walls (a small margin keeps it off the surface).
        float boom = kThirdPersonDistance;
        const auto hit = world::raycastVoxels(interactionWorld, eyePivot, boomDirection,
                                              kThirdPersonDistance + 0.3F);
        if (hit.has_value()) {
            boom = std::clamp(hit->distance - 0.2F, 0.0F, kThirdPersonDistance);
        }
        result.position = eyePivot + boomDirection * boom;
        if (front) {
            result.forward = -forwardDir;
        }
        return result;
    }

    // View matrix for the scene, before view bobbing. Used for both the uniform
    // and the culling frustum so terrain is culled against the view that is
    // actually rendered (critical in third person, where the front view even
    // looks the opposite way from the first-person camera direction).
    [[nodiscard]] glm::mat4 renderViewMatrix() const {
        const RenderEye eye = renderEyeState();
        return glm::lookAt(eye.position, eye.position + eye.forward, glm::vec3{0.0F, 1.0F, 0.0F});
    }

    void updateUniform(FrameContext& frame) const {
        CameraUniform uniform;
        uniform.model = glm::mat4{1.0F};
        const RenderEye renderEye = renderEyeState();
        // View bobbing bobs the whole scene in every perspective, matching the
        // vanilla third-person camera shake.
        const glm::mat4 baseView = glm::lookAt(renderEye.position, renderEye.position + renderEye.forward,
                                               glm::vec3{0.0F, 1.0F, 0.0F});
        uniform.view = viewBobbingMatrix() * baseView;
        uniform.projection = camera.projectionMatrix(static_cast<float>(swapchainExtent.width) /
                                                         static_cast<float>(swapchainExtent.height),
                                                     cameraFarPlane());
        uniform.cameraPosition = glm::vec4{renderEye.position, 1.0F};
        const auto daylight = world::DayNightCycle::state(gameSession.gameTimeSeconds());
        uniform.sunDirection = glm::vec4{daylight.sunDirection, daylight.skyBrightness};
        // horizonFog.w drives the water/lava animation frames and the moon phase.
        // Both only depend on the time within the lunar cycle (8 days): the
        // shaders multiply it by 10 and mod again, so an unwrapped float loses
        // precision past ~19 days and the animation frames start jumping. Wrapping
        // here keeps the same phase DayNightCycle derives from the double, and 8
        // days is a whole number of both day lengths.
        constexpr double kLunarCycleSeconds = 8.0 * world::DayNightCycle::kSecondsPerDay;
        uniform.horizonFog = glm::vec4{daylight.horizonColor,
                                       static_cast<float>(std::fmod(gameSession.gameTimeSeconds(),
                                                                    kLunarCycleSeconds))};
        // renderSettings.z is the underwater EXP2 fog density. Vanilla uses 0.05;
        // a slightly denser 0.08 restores the murkier look (near-full fog by ~22
        // blocks) after the old hard distance cap was removed.
        uniform.renderSettings =
            glm::vec4{renderDistanceBlocks(), cameraSubmergedInWater() ? 1.0F : 0.0F, 0.08F, 24.0F};
        // The sky shader picks the sun sprite and the moon phase tiles from the
        // atlas; the layers come from the derived special-section bases rather
        // than hardcoded numbers so an atlas change cannot silently re-point
        // them at a block texture.
        uniform.celestialLayers = glm::vec4{kSunLayer, kMoonPhaseFirstLayer, 0.0F, 0.0F};
        const auto& weather = gameSession.weatherSystem();
        const float rainGradient = weather.rainGradientAt(renderInterpolationAlpha);
        const float thunderGradient = weather.thunderGradientAt(renderInterpolationAlpha);
        uniform.weatherSettings = glm::vec4{
            rainGradient,
            thunderGradient,
            weather.visualSkyLightFactorAt(renderInterpolationAlpha),
            1.0F - rainGradient,
        };
        std::size_t lightCount = 0U;
        const auto& heldStack = activeInventory().selectedStack();
        const bool holdingTorch = gameplay::emitsHeldLight(heldStack);
        if (options.dynamicLight && holdingTorch) {
            uniform.pointLights[lightCount] = glm::vec4{camera.position(), 7.7F};
            uniform.lightColors[lightCount] = {1.0F, 0.72F, 0.38F, 0.78F};
            ++lightCount;
        }
        uniform.lightingSettings.x = static_cast<float>(lightCount);
        uniform.lightingSettings.y =
            options.smoothLightingQuality != world::SmoothLightingQuality::Off ? 1.0F : 0.0F;
        uniform.lightingSettings.z = currentMeshQuality == world::SmoothLightingQuality::High
            ? 1.0F
            : 0.0F;
        // lightingSettings.w is the sun-shadow switch: 1.0 when the pre-pass ran
        // this frame, so the terrain shader only samples the map when it is live.
        uniform.lightingSettings.w = shadowDisabled ? 0.0F : 1.0F;
        uniform.lightViewProj = shadowLightViewProj;
        std::memcpy(frame.uniformBuffer.mapped, &uniform, sizeof(uniform));
        checkVk(vmaFlushAllocation(allocator, frame.uniformBuffer.allocation, 0, sizeof(uniform)),
                "vmaFlushAllocation(camera uniform)");
    }

    void drawHudQuad(VkCommandBuffer commandBuffer, const ui::UiRect& rectangle,
                     const glm::vec4& color, float textureLayer = 0.0F, bool textured = false,
                     glm::vec4 uvRectangle = {0.0F, 0.0F, 1.0F, 1.0F}, bool fontGlyph = false,
                     bool guiSprite = false) const {
        const float width = static_cast<float>(swapchainExtent.width);
        const float height = static_cast<float>(swapchainExtent.height);
        const auto clipRectangle = ui::framebufferToClip(rectangle, width, height);
        const HudPush push{
            {
                clipRectangle.x,
                clipRectangle.y,
                clipRectangle.width,
                clipRectangle.height,
            },
            color,
            uvRectangle,
            {guiSprite ? 3.0F : (fontGlyph ? 2.0F : (textured ? 1.0F : 0.0F)), textureLayer, 0.0F,
             0.0F},
        };
        vkCmdPushConstants(commandBuffer, hudPipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);
        vkCmdDraw(commandBuffer, 6, 1, 0, 0);
    }

    void drawHudBlockIcon(VkCommandBuffer commandBuffer, const ui::UiRect& rectangle,
                          world::Block block) const {
        const float width = static_cast<float>(swapchainExtent.width);
        const float height = static_cast<float>(swapchainExtent.height);
        const auto clipRectangle = ui::framebufferToClip(rectangle, width, height);
        const auto textures = world::textureLayers(block);
        const bool chest = block == world::Block::Chest;
        const bool furnace = block == world::Block::Furnace;
        const HudPush push{
            {clipRectangle.x, clipRectangle.y, clipRectangle.width, clipRectangle.height},
            {1.0F, 1.0F, 1.0F, 1.0F},
            {0.0F, 0.0F, 1.0F, 1.0F},
            {(chest || furnace) ? 4.25F : 4.0F, chest ? kChestItemTopLayer : textures.top,
             chest ? kChestItemFrontLayer : (furnace ? kFurnaceFrontLayer : textures.side),
             chest ? kChestItemSideLayer : textures.side},
        };
        vkCmdPushConstants(commandBuffer, hudPipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);
        vkCmdDraw(commandBuffer, 18, 1, 0, 0);
    }

    void drawHudItemIcon(VkCommandBuffer commandBuffer, const ui::UiRect& rectangle,
                         const gameplay::ItemStack& stack) const {
        if (gameplay::isBlockStack(stack) &&
            (world::blockDefinition(stack.block).model == world::BlockModel::Cube ||
             world::blockDefinition(stack.block).model == world::BlockModel::Chest)) {
            drawHudBlockIcon(commandBuffer, rectangle, stack.block);
            return;
        }
        drawHudQuad(commandBuffer, rectangle, {1.0F, 1.0F, 1.0F, 1.0F},
                    gameplay::itemTextureLayer(stack), true);
    }

    void drawGuiSprite(VkCommandBuffer commandBuffer, const ui::UiRect& destination, float layer,
                       const ui::UiRect& sourcePixels,
                       const glm::vec4& tint = {1.0F, 1.0F, 1.0F, 1.0F}) const {
        constexpr float atlasSize = 256.0F;
        drawHudQuad(commandBuffer, destination, tint, layer, false,
                    {sourcePixels.x / atlasSize, sourcePixels.y / atlasSize,
                     sourcePixels.width / atlasSize, sourcePixels.height / atlasSize},
                    false, true);
    }

    void drawMinecraftCrosshair(VkCommandBuffer commandBuffer, const ui::UiRect& rectangle) const {
        constexpr float atlasSize = 256.0F;
        const auto clipRectangle =
            ui::framebufferToClip(rectangle, static_cast<float>(swapchainExtent.width),
                                  static_cast<float>(swapchainExtent.height));
        const HudPush push{
            {clipRectangle.x, clipRectangle.y, clipRectangle.width, clipRectangle.height},
            {1.0F, 1.0F, 1.0F, 1.0F},
            {0.0F, 0.0F, 15.0F / atlasSize, 15.0F / atlasSize},
            {5.0F, 1.0F, 0.0F, 0.0F},
        };
        vkCmdPushConstants(commandBuffer, hudPipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);
        vkCmdDraw(commandBuffer, 6, 1, 0, 0);
    }

    void drawUnderwaterOverlay(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet) const {
        if (!cameraSubmergedInWater()) {
            return;
        }
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipelineLayout,
                                0, 1, &descriptorSet, 0, nullptr);
        drawGuiSprite(commandBuffer,
                      {0.0F, 0.0F, static_cast<float>(swapchainExtent.width),
                       static_cast<float>(swapchainExtent.height)},
                      6.0F, {0.0F, 0.0F, 256.0F, 256.0F}, {0.70F, 0.85F, 1.0F, 0.10F});
    }

    // 1.16.1's Screen.renderBackground darkens every open in-game screen with a
    // vertical gradient (top rgba(16,16,16,0xC0) -> bottom rgba(16,16,16,0xD0)),
    // baked into kScreenDimGuiLayer in createGuiTexture().
    void drawScreenDimOverlay(VkCommandBuffer commandBuffer) const {
        drawGuiSprite(commandBuffer,
                      {0.0F, 0.0F, static_cast<float>(swapchainExtent.width),
                       static_cast<float>(swapchainExtent.height)},
                      kScreenDimGuiLayer, {0.0F, 0.0F, 256.0F, 256.0F},
                      {1.0F, 1.0F, 1.0F, 1.0F});
    }

    // 1.16.1's InGameHud renders the vignette texture with a multiplicative
    // blend (dst * (1 - src)) so the dark corners darken the scene while the
    // centre is left untouched. It must run on the dedicated vignette pipeline;
    // the HUD pipeline is rebound afterwards so later HUD sprites keep their
    // normal alpha blending.
    void drawVignette(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet) const {
        if (vignetteDarkness_ <= 0.001F) {
            return;
        }
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vignettePipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipelineLayout,
                                0, 1, &descriptorSet, 0, nullptr);
        const float darkness = vignetteDarkness_;
        drawGuiSprite(commandBuffer,
                      {0.0F, 0.0F, static_cast<float>(swapchainExtent.width),
                       static_cast<float>(swapchainExtent.height)},
                      kVignetteGuiLayer, {0.0F, 0.0F, 256.0F, 256.0F},
                      {darkness, darkness, darkness, 1.0F});
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipeline);
    }

    void drawMinecraftButton(VkCommandBuffer commandBuffer, const ui::UiRect& rectangle,
                             std::string_view label, ui::ButtonVisualState state,
                             float scale,
                             glm::vec4 tint = glm::vec4{1.0F}) const {
        // Snap to whole framebuffer pixels. On a maximised or odd-sized window a
        // fractional button origin shifts the nearest-neighbour sprite sampling
        // by a sub-texel, which can make the 1px border render unevenly or catch
        // a neighbouring texel at the top edge.
        const ui::UiRect snapped{std::floor(rectangle.x), std::floor(rectangle.y),
                                 std::floor(rectangle.width + 0.5F),
                                 std::floor(rectangle.height + 0.5F)};
        const float sourceY = state == ui::ButtonVisualState::Disabled
                                  ? 46.0F
                                  : (state == ui::ButtonVisualState::Normal ? 66.0F : 86.0F);
        const float halfWidth = snapped.width * 0.5F;
        const float sourceHalfWidth = std::min(100.0F, halfWidth / scale);
        // Pressed darkens the caller's tint instead of hard-coding grey, so the
        // red delete button keeps a coherent colour in every state.
        const glm::vec4 buttonTint =
            state == ui::ButtonVisualState::Pressed
                ? glm::vec4{tint.r * 0.78F, tint.g * 0.78F, tint.b * 0.78F, 1.0F}
                : tint;
        drawGuiSprite(commandBuffer, {snapped.x, snapped.y, halfWidth, snapped.height}, 0.0F,
                      {0.0F, sourceY, sourceHalfWidth, 20.0F}, buttonTint);
        drawGuiSprite(commandBuffer,
                      {snapped.x + halfWidth, snapped.y, halfWidth, snapped.height}, 0.0F,
                      {200.0F - sourceHalfWidth, sourceY, sourceHalfWidth, 20.0F}, buttonTint);
        const glm::vec4 textColor = state == ui::ButtonVisualState::Disabled
                                        ? glm::vec4{0.63F, 0.63F, 0.63F, 1.0F}
                                        : (state == ui::ButtonVisualState::Hovered ||
                                                   state == ui::ButtonVisualState::Pressed
                                               ? glm::vec4{1.0F, 1.0F, 0.63F, 1.0F}
                                               : glm::vec4{1.0F});
        const float textY =
            snapped.y + (6.0F + (state == ui::ButtonVisualState::Pressed ? 1.0F : 0.0F)) * scale;
        drawHudText(commandBuffer, label,
                    snapped.x + (snapped.width - hudTextWidth(label, scale)) * 0.5F, textY,
                    scale, textColor);
    }

    void drawMinecraftSlider(VkCommandBuffer commandBuffer, const ui::UiRect& rectangle,
                             std::string_view label, ui::ButtonVisualState state, float value,
                             float scale) const {
        // SliderWidget#getYImage always returns zero: Java therefore uses the
        // dark y=46 row for the track and overlays only the movable knob from
        // the normal/hover rows.
        constexpr float sourceY = 46.0F;
        const ui::UiRect snapped{std::floor(rectangle.x), std::floor(rectangle.y),
                                 std::floor(rectangle.width + 0.5F),
                                 std::floor(rectangle.height + 0.5F)};
        const float halfWidth = snapped.width * 0.5F;
        const float sourceHalfWidth = std::min(100.0F, halfWidth / scale);
        const glm::vec4 tint = state == ui::ButtonVisualState::Pressed
                                   ? glm::vec4{0.78F, 0.78F, 0.78F, 1.0F}
                                   : glm::vec4{1.0F};
        drawGuiSprite(commandBuffer, {snapped.x, snapped.y, halfWidth, snapped.height}, 0.0F,
                      {0.0F, sourceY, sourceHalfWidth, 20.0F}, tint);
        drawGuiSprite(commandBuffer,
                      {snapped.x + halfWidth, snapped.y, halfWidth, snapped.height}, 0.0F,
                      {200.0F - sourceHalfWidth, sourceY, sourceHalfWidth, 20.0F}, tint);
        const float clampedValue = std::clamp(value, 0.0F, 1.0F);
        const float knobX =
            snapped.x + clampedValue * std::max(snapped.width - 8.0F * scale, 0.0F);
        const float knobSourceY = state == ui::ButtonVisualState::Normal ? 66.0F : 86.0F;
        drawGuiSprite(commandBuffer, {knobX, snapped.y, 4.0F * scale, snapped.height}, 0.0F,
                      {0.0F, knobSourceY, 4.0F, 20.0F});
        drawGuiSprite(commandBuffer,
                      {knobX + 4.0F * scale, snapped.y, 4.0F * scale, snapped.height}, 0.0F,
                      {196.0F, knobSourceY, 4.0F, 20.0F});
        const glm::vec4 textColor = state == ui::ButtonVisualState::Disabled
                                        ? glm::vec4{0.63F, 0.63F, 0.63F, 1.0F}
                                        : (state == ui::ButtonVisualState::Hovered ||
                                                   state == ui::ButtonVisualState::Pressed
                                               ? glm::vec4{1.0F, 1.0F, 0.63F, 1.0F}
                                               : glm::vec4{1.0F});
        const float textY =
            snapped.y + (6.0F + (state == ui::ButtonVisualState::Pressed ? 1.0F : 0.0F)) * scale;
        drawHudText(commandBuffer, label,
                    snapped.x + (snapped.width - hudTextWidth(label, scale)) * 0.5F, textY,
                    scale, textColor);
    }

    [[nodiscard]] float hudTextWidth(std::string_view text, float scale) const {
        return textFont.textWidth(text, scale);
    }

    void drawHudText(VkCommandBuffer commandBuffer, std::string_view text, float x, float y,
                     float scale, const glm::vec4& color, bool shadow = true) const {
        float cursorX = x;
        for (const char32_t codepoint : ui::decodeUtf8(text)) {
            const auto metrics = textFont.glyph(codepoint);
            const glm::vec4 uv{
                metrics.u,
                metrics.v,
                metrics.uvWidth,
                metrics.uvHeight,
            };
            const ui::UiRect glyph{
                cursorX,
                y,
                metrics.pixelWidth * scale,
                metrics.pixelHeight * scale,
            };
            if (metrics.visible && shadow) {
                auto shadowColor = color;
                shadowColor.r *= 0.18F;
                shadowColor.g *= 0.18F;
                shadowColor.b *= 0.18F;
                drawHudQuad(commandBuffer,
                            {glyph.x + scale, glyph.y + scale, glyph.width, glyph.height},
                            shadowColor, metrics.layer, false, uv, true);
            }
            if (metrics.visible) {
                drawHudQuad(commandBuffer, glyph, color, metrics.layer, false, uv, true);
            }
            cursorX += metrics.advance * scale;
        }
    }

    // The active translation, falling back to the English text baked into the
    // call site when the language file has no entry.
    [[nodiscard]] std::string_view translate(std::string_view key,
                                             std::string_view fallback) const {
        return language.translate(key, fallback);
    }

    [[nodiscard]] std::string translated(std::string_view key, std::string_view fallback) const {
        return std::string{translate(key, fallback)};
    }

    // Block names still localize through vanilla's translation keys; item names
    // come straight from the English/Chinese strings each item was registered
    // with, picked by the active language.
    [[nodiscard]] std::string itemDisplayName(const gameplay::ItemStack& stack) const {
        if (gameplay::isBlockStack(stack)) {
            const auto& definition = world::blockDefinition(stack.block);
            const auto& source = world::translationIdentifier(stack.block);
            return std::string{
                translate(ui::translationKey("block", source.space, source.path),
                          definition.displayName)};
        }
        const bool chinese = language.code().rfind("zh", 0) == 0;
        const char* name = chinese && stack.item->zh[0] != '\0' ? stack.item->zh : stack.item->en;
        return std::string{name};
    }

    // blockTextureRoot is <vanilla>/1.16.1/textures/minecraft/block; the
    // localization files live three parents up under localization/minecraft.
    [[nodiscard]] std::filesystem::path localizationRoot() const {
        return blockTextureRoot.parent_path().parent_path().parent_path() /
            "localization" / "minecraft";
    }

    // Builds the language screen's display names: each language's own
    // `language.name` and `language.region`, the way 1.16.1's
    // LanguageDefinition#getLocalizedString renders the list entries.
    void refreshLanguageNames() {
        menuSystem.languageDisplayNames.clear();
        menuSystem.languageDisplayNames.reserve(menuSystem.languageCodes.size());
        for (const auto& code : menuSystem.languageCodes) {
            std::string name = code;
            try {
                const auto file = ui::Language::fromFile(localizationRoot() / (code + ".json"));
                name = std::string{file.translate("language.name", code)};
                const auto region = file.translate("language.region", "");
                if (!region.empty()) {
                    name += " (" + std::string{region} + ")";
                }
            } catch (const std::exception&) {
                // A missing file keeps the bare code as its own name.
            }
            menuSystem.languageDisplayNames.push_back(std::move(name));
        }
        menuSystem.languageListFirstIndex = 0U;
    }

    void loadLanguage() {
        const auto root = localizationRoot();
        menuSystem.languageCodes = ui::availableLanguageCodes(root);
        language = {};
        if (options.language == ui::kDefaultLanguageCode) {
            language.setCode(options.language);
        } else {
            try {
                language = ui::Language::fromFile(root / (options.language + ".json"));
                std::cout << "Loaded language " << options.language << ": " << language.size()
                          << " entries\n";
            } catch (const std::exception& exception) {
                std::cout << "Language " << options.language
                          << " unavailable, falling back to en_us: " << exception.what() << '\n';
                options.language = ui::kDefaultLanguageCode;
                language = {};
            }
        }
        textFont.setForceUnicode(options.forceUnicodeFont);
        // The language screen's display names are needed for the font pages too,
        // so build them now (not only when the screen opens).
        refreshLanguageNames();
    }

    void selectLanguage(const std::string& code) {
        if (options.language == code) {
            return;
        }
        options.language = code;
        loadLanguage();
        recreateFontTexture();
        persistOptions();
    }

    // ItemRenderer#renderGuiItemOverlay's damage bar: a black 13x2 strip along
    // the bottom of the 16x16 icon, with a coloured strip on top whose length
    // is the remaining durability and whose hue runs green to red across the
    // tool's life. Undamaged items draw nothing, exactly like vanilla.
    void drawDurabilityBar(VkCommandBuffer commandBuffer, const ui::UiRect& icon,
                           const gameplay::ItemStack& stack) const {
        const std::uint16_t maximumDamage = gameplay::itemMaximumDamage(stack);
        if (maximumDamage == 0U || stack.damage == 0U) {
            return;
        }
        const float spent =
            static_cast<float>(stack.damage) / static_cast<float>(maximumDamage);
        const float unit = icon.width / 16.0F;
        const float remainingWidth = std::round(13.0F * (1.0F - spent));
        drawHudQuad(commandBuffer,
                    {icon.x + 2.0F * unit, icon.y + 13.0F * unit, 13.0F * unit, 2.0F * unit},
                    {0.0F, 0.0F, 0.0F, 1.0F});
        // MathHelper.hsvToRgb((1 - spent) / 3, 1, 1) with full saturation and
        // value, which only ever mixes red and green.
        const float hue = (1.0F - spent) / 3.0F * 6.0F;
        const glm::vec4 color = hue < 1.0F
            ? glm::vec4{1.0F, hue, 0.0F, 1.0F}
            : glm::vec4{std::max(2.0F - hue, 0.0F), 1.0F, 0.0F, 1.0F};
        drawHudQuad(commandBuffer,
                    {icon.x + 2.0F * unit, icon.y + 13.0F * unit, remainingWidth * unit, unit},
                    color);
    }

    void drawHudSlot(VkCommandBuffer commandBuffer, const ui::UiRect& rectangle,
                     const gameplay::ItemStack& stack, bool selected, bool hovered = false,
                     bool minecraftStyle = false) const {
        if (!minecraftStyle) {
            const float border = selected ? 3.0F : 2.0F;
            drawHudQuad(commandBuffer, rectangle,
                        selected ? glm::vec4{0.96F, 0.82F, 0.28F, 0.96F}
                                 : glm::vec4{0.08F, 0.08F, 0.10F, 0.88F});
            const ui::UiRect inner{
                rectangle.x + border,
                rectangle.y + border,
                rectangle.width - border * 2.0F,
                rectangle.height - border * 2.0F,
            };
            drawHudQuad(commandBuffer, inner, {0.24F, 0.24F, 0.27F, 0.90F});
        }
        if (hovered) {
            drawHudQuad(commandBuffer, rectangle, {1.0F, 1.0F, 1.0F, 0.34F});
        }
        if (!stack.empty()) {
            const float iconInset = minecraftStyle ? 0.0F : rectangle.width * 0.16F;
            const ui::UiRect icon{
                rectangle.x + iconInset,
                rectangle.y + iconInset,
                rectangle.width - iconInset * 2.0F,
                rectangle.height - iconInset * 2.0F,
            };
            drawHudItemIcon(commandBuffer, icon, stack);
            drawDurabilityBar(commandBuffer, icon, stack);
            if (stack.count > 1U) {
                const std::string count = std::to_string(stack.count);
                const float textScale =
                    minecraftStyle ? rectangle.width / 16.0F : rectangle.width / 40.0F;
                drawHudText(commandBuffer, count,
                            rectangle.x + 17.0F * textScale - hudTextWidth(count, textScale),
                            rectangle.y + 9.0F * textScale, textScale, {1.0F, 1.0F, 1.0F, 1.0F});
            }
        }
    }

    [[nodiscard]] std::string rainModeLabel(int mode) const {
        switch (mode) {
            case 0: return translated("options.rainMode.texture", "贴图雨");
            case 1: return translated("options.rainMode.particles", "粒子雨");
            default: return translated("options.rainMode.async", "异步粒子雨");
        }
    }

    // The particle-level multiplier (粒子效果): 低 0.5x / 中 1.0x (the default) /
    // 高 2x / 疯狂 3x. Scales the rain-drop budget and the particle system.
    [[nodiscard]] static float particleLevelMultiplier(int level) {
        switch (level) {
            case 0: return 0.5F;
            case 2: return 2.0F;
            case 3: return 3.0F;
            default: return 1.0F;
        }
    }

    [[nodiscard]] std::string particleLevelLabel(int level) const {
        switch (level) {
            case 0: return translated("options.particleLevel.low", "低") + " (0.5x)";
            case 2: return translated("options.particleLevel.high", "高") + " (2x)";
            case 3: return translated("options.particleLevel.crazy", "疯狂") + " (3x)";
            default: return translated("options.particleLevel.medium", "中") + " (1x)";
        }
    }

    // Applies the option to the live particle system (the spawn-count and
    // live-cap scaling); the rain budget reads options.particleLevel directly.
    void applyParticleLevel() {
        particleSystem.setLevelScale(particleLevelMultiplier(options.particleLevel));
    }

    [[nodiscard]] std::string menuButtonLabel(MenuButton button) const {
        // Every label carries its English text as the fallback, so a language
        // without the vanilla key still reads correctly.
        const auto toggle = [this](bool value) {
            return translated(value ? "options.on" : "options.off", value ? "ON" : "OFF");
        };
        switch (button) {
        case MenuButton::Resume:
            return translated("menu.returnToGame", "Back to Game");
        case MenuButton::Options:
            return translated("menu.options", "Options...");
        case MenuButton::Quit:
            return translated("menu.quit", "Quit Game");
        case MenuButton::Resolution: {
            // The label shows the live window size so a maximized or manually
            // resized window reads correctly instead of echoing the last preset.
            const auto resolution = ui::kDisplayResolutions[menuSystem.resolutionIndex];
            int windowWidth = 0;
            int windowHeight = 0;
            glfwGetWindowSize(window, &windowWidth, &windowHeight);
            if (windowWidth == resolution.width && windowHeight == resolution.height) {
                return translated("options.fullscreen.resolution", "Resolution") + ": " +
                       std::to_string(resolution.width) + "x" +
                       std::to_string(resolution.height);
            }
            return translated("options.fullscreen.resolution", "Resolution") + ": " +
                   std::to_string(windowWidth) + "x" + std::to_string(windowHeight) + " " +
                   translated("options.resolution.windowed", "(windowed)");
        }
        case MenuButton::GuiScale:
            return translated("options.guiScale", "GUI Scale") + ": " +
                   (menuSystem.guiScaleSetting == 0 ? translated("options.guiScale.auto", "Auto")
                                         : std::to_string(menuSystem.guiScaleSetting));
        case MenuButton::ViewDistance:
            return translated("options.renderDistance", "Render Distance") + ": " +
                   formatTemplate(translated("options.chunks", "%s chunks"),
                                  std::to_string(viewDistanceChunks));
        case MenuButton::SimulationDistance:
            return translated("options.simulationDistance", "Simulation Distance") + ": " +
                   formatTemplate(translated("options.chunks", "%s chunks"),
                                  std::to_string(simulationDistanceChunks));
        case MenuButton::MasterVolume:
            return translated("soundCategory.master", "Master Volume") + ": " +
                   std::to_string(static_cast<int>(std::lround(options.masterVolume * 100.0F))) +
                   "%";
        case MenuButton::VideoSettings:
            return translated("options.videoTitle", "Video Settings") + "...";
        case MenuButton::Controls:
            return translated("options.controls", "Controls") + "...";
        case MenuButton::AutoJump:
            return translated("options.autoJump", "Auto-Jump") + ": " +
                   toggle(options.autoJump);
        case MenuButton::FrameRateLimit:
            return translated("options.framerateLimit", "Max Framerate") + ": " +
                   (options.frameRateLimit == 0
                        ? translated("options.framerateLimit.max", "Unlimited")
                        : formatTemplate(translated("options.framerate", "%s fps"),
                                         std::to_string(options.frameRateLimit)));
        case MenuButton::AntiAliasing:
            return translated("options.antiAliasing", "Anti-Aliasing") + ": " +
                   toggle(options.antiAliasing);
        case MenuButton::Anisotropy:
            return translated("options.anisotropicFiltering", "Anisotropic Filtering") + ": " +
                   std::to_string(options.anisotropy) + "x";
        case MenuButton::ViewBobbing:
            return translated("options.viewBobbing", "View Bobbing") + ": " +
                   toggle(options.viewBobbing);
        case MenuButton::SmoothLighting:
            switch (options.smoothLightingQuality) {
            case world::SmoothLightingQuality::Off:
                return translated("options.ao", "Smooth Lighting") + ": " +
                       translated("options.ao.off", "OFF");
            case world::SmoothLightingQuality::High:
                return translated("options.ao", "Smooth Lighting") + ": " +
                       translated("options.ao.max", "Maximum");
            case world::SmoothLightingQuality::Standard:
                return translated("options.ao", "Smooth Lighting") + ": " +
                       translated("options.ao.min", "Minimum");
            }
            return {};
        case MenuButton::DynamicLight:
            return translated("options.dynamicLights", "Dynamic Lighting") + ": " +
                   toggle(options.dynamicLight);
        case MenuButton::Vsync:
            return translated("options.vsync", "Use VSync") + ": " + toggle(options.vsync);
        case MenuButton::Difficulty:
            // Only present on the in-world options page, where a save is open.
            return translated("options.difficulty", "Difficulty") + ": " +
                   translated(
                       gameplay::difficultyTranslationKey(currentSave.has_value()
                                                              ? currentSave->difficulty
                                                              : gameplay::Difficulty::Normal),
                       gameplay::difficultyName(currentSave.has_value()
                                                    ? currentSave->difficulty
                                                    : gameplay::Difficulty::Normal));
        case MenuButton::Experimental:
            return translated("menu.experimental", "实验性内容") + "...";
        case MenuButton::RainMode:
            return translated("options.rainMode", "雨模式") + ": " + rainModeLabel(options.rainMode);
        case MenuButton::ParticleLevel:
            return translated("options.particleLevel", "粒子效果") + ": " +
                   particleLevelLabel(options.particleLevel);
        case MenuButton::SunShadows:
            return translated("options.sunShadows", "太阳阴影") + ": " + toggle(options.sunShadows);
        case MenuButton::RainCollisionCache:
            return translated("options.rainCollisionCache", "雨碰撞缓存") + ": " +
                   toggle(options.rainCollisionCache);
        case MenuButton::Language:
            return translated("options.language", "Language") + "...";
        case MenuButton::ForceUnicodeFont:
            return translated("options.forceUnicodeFont", "Force Unicode Font") + ": " +
                   toggle(options.forceUnicodeFont);
        case MenuButton::Done:
            return translated("gui.done", "Done");
        case MenuButton::Singleplayer:
            return translated("menu.singleplayer", "Singleplayer");
        case MenuButton::Exit:
            return translated("menu.quit", "Quit Game");
        case MenuButton::PlaySelected:
            return translated("selectWorld.select", "Play Selected World");
        case MenuButton::CreateWorld:
            return translated("selectWorld.create", "Create New World");
        case MenuButton::Edit:
            return translated("selectWorld.edit", "Edit");
        case MenuButton::SaveRename:
            return translated("gui.done", "Done");
        case MenuButton::DeleteWorld:
            return translated("selectWorld.delete", "Delete");
        case MenuButton::DeleteConfirm:
            return translated("selectWorld.deleteButton", "Delete");
        case MenuButton::DeleteCancel:
            return translated("gui.cancel", "Cancel");
        case MenuButton::Back:
            return translated("gui.back", "Back");
        case MenuButton::CreateConfirm:
            return translated("selectWorld.create", "Create World");
        case MenuButton::CreateGameMode:
            return translated("selectWorld.gameMode", "Game Mode") + ": " +
                   gameModeLabel(menuSystem.createWorldGameMode);
        case MenuButton::SaveQuit:
            return translated("menu.returnToMenu", "Save and Quit to Title");
        case MenuButton::Respawn:
            return translated("deathScreen.respawn", "Respawn");
        case MenuButton::TitleScreen:
            return translated("deathScreen.titleScreen", "Title Screen");
        case MenuButton::None:
            return {};
        }
        return {};
    }

    [[nodiscard]] std::string gameModeLabel(gameplay::GameMode mode) const {
        return mode == gameplay::GameMode::Survival
            ? translated("selectWorld.gameMode.survival", "survival")
            : translated("selectWorld.gameMode.creative", "creative");
    }

    // Replaces the single "%s" placeholder vanilla language files use.
    [[nodiscard]] static std::string formatTemplate(std::string text, std::string_view value) {
        const auto placeholder = text.find("%s");
        if (placeholder == std::string::npos) {
            return text + " " + std::string{value};
        }
        return text.replace(placeholder, 2U, value);
    }

    // Frontend screen titles. The edit page shows the selected world's name,
    // matching 1.16.1's Edit World screen; the delete confirmation uses the
    // vanilla delete question as its heading.
    [[nodiscard]] std::string frontendTitle(ui::PageId page) const {
        if (page == ui::PageId::Title) return "MC Rebedrock";
        if (page == ui::PageId::WorldList)
            return translated("menu.singleplayer", "Singleplayer");
        if (page == ui::PageId::CreateWorld)
            return translated("selectWorld.create", "Create New World");
        if (page == ui::PageId::ConfirmDelete)
            return translated("selectWorld.deleteQuestion", "Delete World?");
        if (menuSystem.selectedWorldIndex < menuSystem.saveSummaries.size())
            return menuSystem.saveSummaries[menuSystem.selectedWorldIndex].displayName;
        return translated("selectWorld.edit", "Edit World");
    }

    void drawWorldNameField(VkCommandBuffer commandBuffer, const ui::HudLayout& layout,
                            const std::string& value) const {
        const float scale = layout.scale();
        const float width = 200.0F * scale;
        const ui::UiRect field{(static_cast<float>(swapchainExtent.width) - width) * 0.5F,
                               static_cast<float>(swapchainExtent.height) * 0.5F - 58.0F * scale,
                               width, 20.0F * scale};
        drawHudText(commandBuffer, translated("selectWorld.enterName", "World Name"), field.x,
                    field.y - 12.0F * scale, scale, {0.85F, 0.85F, 0.85F, 1.0F});
        drawHudQuad(commandBuffer, field, {0.02F, 0.02F, 0.02F, 0.95F});
        drawHudQuad(commandBuffer,
                    {field.x + scale, field.y + scale, field.width - 2.0F * scale,
                     field.height - 2.0F * scale},
                    {0.12F, 0.12F, 0.12F, 0.98F});
        drawHudText(commandBuffer, value, field.x + 4.0F * scale, field.y + 6.0F * scale, scale,
                    {1.0F, 1.0F, 1.0F, 1.0F});
    }

    // 1.16.1's CubeMap background: an 85-degree perspective view from inside
    // the panorama cube. The camera slowly turns — a full 360° yaw over
    // kCycleSeconds so every one of the six faces gets a long turn in front of
    // the view, a gentle pitch sweep dips down to panorama_4 and up to
    // panorama_5, and a faint vanilla-style sine sway keeps it from feeling
    // mechanical. The dark quad afterwards keeps the white title and the menu
    // buttons readable over the scene.
    void drawTitleCarousel(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet) const {
        // One full 360° yaw turn every five minutes, so each of the four side
        // faces stays centred for well over a minute. The pitch sweeps once per
        // turn and the vanilla-style sway rate matches the slower pace.
        constexpr double kCycleSeconds = 300.0;
        constexpr double kPi = 3.14159265358979323846;
        const double progress = uiTimeSeconds / kCycleSeconds;
        // Full 360° turn plus a small sine sway; pitch oscillates once per turn.
        const float yaw =
            static_cast<float>(progress * 2.0 * kPi) +
            static_cast<float>(std::sin(uiTimeSeconds * 0.024) * 0.04);
        const float pitch =
            static_cast<float>(std::sin(progress * 2.0 * kPi)) * 0.44F;
        const float tanHalfFov =
            std::tan(static_cast<float>(85.0 * kPi / 180.0) * 0.5F);
        const float aspect =
            static_cast<float>(swapchainExtent.width) / static_cast<float>(swapchainExtent.height);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, panoramaPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                panoramaPipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
        const PanoramaPush push{{yaw, pitch, tanHalfFov, aspect}};
        vkCmdPushConstants(commandBuffer, panoramaPipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipelineLayout,
                                0, 1, &descriptorSet, 0, nullptr);
        drawHudQuad(commandBuffer,
                    {0.0F, 0.0F, static_cast<float>(swapchainExtent.width),
                     static_cast<float>(swapchainExtent.height)},
                    {0.0F, 0.0F, 0.0F, 0.30F});
    }

    void drawFrontend(VkCommandBuffer commandBuffer, const ui::HudLayout& layout,
                      VkDescriptorSet descriptorSet) const {
        if (menuSystem.pageStack.current() == ui::PageId::Title) {
            drawTitleCarousel(commandBuffer, descriptorSet);
        } else {
            drawGuiSprite(commandBuffer,
                          {0.0F, 0.0F, static_cast<float>(swapchainExtent.width),
                           static_cast<float>(swapchainExtent.height)},
                          9.0F,
                          ui::tiledBackgroundSource(static_cast<float>(swapchainExtent.width),
                                                    static_cast<float>(swapchainExtent.height),
                                                    layout.scale()),
                          kMenuBackgroundTint);
        }
        const float scale = layout.scale();
        const auto cursor = currentFramebufferCursor();
        const auto page = menuSystem.pageStack.current();
        const std::string title = frontendTitle(page);
        drawHudText(commandBuffer, title,
                    (static_cast<float>(swapchainExtent.width) -
                     hudTextWidth(title, scale * (page == ui::PageId::Title ? 2.0F : 1.0F))) *
                        0.5F,
                    14.0F * scale, scale * (page == ui::PageId::Title ? 2.0F : 1.0F),
                    {1.0F, 1.0F, 1.0F, 1.0F});

        if (page == ui::PageId::WorldList) {
            const std::size_t visibleRows = saveListVisibleRowCount();
            const std::size_t maximumFirst =
                menuSystem.saveSummaries.size() > visibleRows ? menuSystem.saveSummaries.size() - visibleRows : 0U;
            const std::size_t first = std::min(menuSystem.worldListFirstIndex, maximumFirst);
            const std::size_t remaining =
                menuSystem.saveSummaries.size() - std::min(first, menuSystem.saveSummaries.size());
            const std::size_t visible = std::min(remaining, visibleRows);
            // 1.16.1's AbstractSelectionList darkens the list area with
            // options_background tiled under a solid (32,32,32) tint — half the
            // menu dirt's (64,64,64) — so the save rows sit on a deep,
            // near-black panel that still reads as the dirt texture.
            const auto firstRow = worldListRow(0, layout);
            const float listBandHeight =
                static_cast<float>(visibleRows) * 22.0F * scale + 8.0F * scale;
            drawGuiSprite(commandBuffer,
                          {0.0F, firstRow.y - 4.0F * scale,
                           static_cast<float>(swapchainExtent.width), listBandHeight},
                          9.0F,
                          ui::tiledBackgroundSource(static_cast<float>(swapchainExtent.width),
                                                    listBandHeight, scale),
                          {32.0F / 255.0F, 32.0F / 255.0F, 32.0F / 255.0F, 1.0F});
            if (visible == 0U) {
                const std::string_view empty = "No worlds yet. Create one to begin.";
                drawHudText(
                    commandBuffer, empty,
                    (static_cast<float>(swapchainExtent.width) - hudTextWidth(empty, scale)) * 0.5F,
                    34.0F * scale, scale, {0.85F, 0.85F, 0.85F, 1.0F});
            }
            for (std::size_t visibleIndex = 0; visibleIndex < visible; ++visibleIndex) {
                const std::size_t index = first + visibleIndex;
                const auto rectangle = worldListRow(visibleIndex, layout);
                const bool selected = index == menuSystem.selectedWorldIndex;
                drawHudQuad(commandBuffer, rectangle,
                            selected ? glm::vec4{0.95F, 0.95F, 0.95F, 0.95F}
                                     : glm::vec4{0.10F, 0.10F, 0.10F, 0.90F});
                drawHudQuad(commandBuffer,
                            {rectangle.x + scale, rectangle.y + scale,
                             rectangle.width - 2.0F * scale, rectangle.height - 2.0F * scale},
                            selected ? glm::vec4{0.28F, 0.28F, 0.28F, 0.96F}
                                     : glm::vec4{0.18F, 0.18F, 0.18F, 0.96F});
                drawHudText(commandBuffer, menuSystem.saveSummaries[index].displayName,
                            rectangle.x + 4.0F * scale, rectangle.y + 2.0F * scale, scale,
                            {1.0F, 1.0F, 1.0F, 1.0F});
                const std::string details = "Seed " + std::to_string(menuSystem.saveSummaries[index].seed);
                drawHudText(commandBuffer, details, rectangle.x + 4.0F * scale,
                            rectangle.y + 11.0F * scale, scale * 0.75F,
                            {0.70F, 0.70F, 0.70F, 1.0F});
            }
        } else if (page == ui::PageId::CreateWorld) {
            const std::string value =
                menuSystem.createWorldName + (static_cast<int>(uiTimeSeconds * 2.0) % 2 == 0 ? "_" : "");
            drawWorldNameField(commandBuffer, layout, value);
        } else if (page == ui::PageId::EditWorld) {
            const std::string value =
                menuSystem.editWorldName + (static_cast<int>(uiTimeSeconds * 2.0) % 2 == 0 ? "_" : "");
            drawWorldNameField(commandBuffer, layout, value);
        } else if (page == ui::PageId::ConfirmDelete) {
            const std::string worldName = menuSystem.selectedWorldIndex < menuSystem.saveSummaries.size()
                                              ? menuSystem.saveSummaries[menuSystem.selectedWorldIndex].displayName
                                              : std::string{};
            const std::string warning = formatTemplate(
                translated("selectWorld.deleteWarning", "\"%s\" will be permanently lost!"),
                worldName);
            const float warningY = static_cast<float>(swapchainExtent.height) * 0.5F - 20.0F * scale;
            drawHudText(commandBuffer, warning,
                        (static_cast<float>(swapchainExtent.width) - hudTextWidth(warning, scale)) *
                            0.5F,
                        warningY, scale, {1.0F, 1.0F, 1.0F, 1.0F});
        }

        const std::size_t buttonCount = menuButtonCount();
        for (std::size_t index = 0; index < buttonCount; ++index) {
            const auto button = menuButtonForIndex(index);
            const auto rectangle = frontendButtonRect(layout, page, index, buttonCount);
            const bool enabled = (button != MenuButton::PlaySelected &&
                                  button != MenuButton::Edit) ||
                                 !menuSystem.saveSummaries.empty();
            const glm::vec4 tint = button == MenuButton::DeleteConfirm
                                       ? glm::vec4{0.72F, 0.22F, 0.22F, 1.0F}
                                       : glm::vec4{1.0F};
            drawMinecraftButton(
                commandBuffer, rectangle, menuButtonLabel(button),
                ui::buttonVisualState(rectangle, cursor.x, cursor.y, enabled,
                                      pressedMenuButton == button),
                scale, tint);
        }
        if (!menuSystem.saveStatus.empty()) {
            drawHudText(commandBuffer, menuSystem.saveStatus, 4.0F * scale,
                        static_cast<float>(swapchainExtent.height) - 12.0F * scale, scale,
                        {1.0F, 0.75F, 0.35F, 1.0F});
        }
    }

    // Gui#renderPlayerHealth: hearts on the left of the hotbar, hunger on the
    // right, and the air row above the hunger row while submerged.
    void drawSurvivalStatusBars(VkCommandBuffer commandBuffer,
                                const ui::HudLayout& layout) const {
        const float scale = layout.scale();
        const auto hotbar = layout.hotbarBackground();
        const float left = hotbar.x;
        const float right = hotbar.x + hotbar.width;
        const float top = hotbar.y - 17.0F * scale;
        const float icon = 9.0F * scale;
        const float step = 8.0F * scale;
        const auto iconRect = [&](float x, float y) {
            return ui::UiRect{x, y, icon, icon};
        };
        // A recent hit swaps the empty heart for the blinking white container.
        const bool flashing = uiFrameData_.ticksSinceDamage < 10;
        const int health = static_cast<int>(std::ceil(uiFrameData_.health));
        for (int index = 9; index >= 0; --index) {
            const auto rectangle = iconRect(left + static_cast<float>(index) * step, top);
            drawGuiSprite(commandBuffer, rectangle, 1.0F,
                          {flashing ? 25.0F : 16.0F, 0.0F, 9.0F, 9.0F});
            if (index * 2 + 1 < health) {
                drawGuiSprite(commandBuffer, rectangle, 1.0F, {52.0F, 0.0F, 9.0F, 9.0F});
            } else if (index * 2 + 1 == health) {
                drawGuiSprite(commandBuffer, rectangle, 1.0F, {61.0F, 0.0F, 9.0F, 9.0F});
            }
        }
        const int food = uiFrameData_.foodLevel;
        for (int index = 0; index < 10; ++index) {
            const auto rectangle =
                iconRect(right - static_cast<float>(index) * step - icon, top);
            drawGuiSprite(commandBuffer, rectangle, 1.0F, {16.0F, 27.0F, 9.0F, 9.0F});
            if (index * 2 + 1 < food) {
                drawGuiSprite(commandBuffer, rectangle, 1.0F, {52.0F, 27.0F, 9.0F, 9.0F});
            } else if (index * 2 + 1 == food) {
                drawGuiSprite(commandBuffer, rectangle, 1.0F, {61.0F, 27.0F, 9.0F, 9.0F});
            }
        }
        if (cameraSubmergedInWater()) {
            constexpr float maximumAir =
                static_cast<float>(gameplay::PlayerVitals::kMaximumAirTicks);
            const float air = std::clamp(static_cast<float>(uiFrameData_.airTicks), 0.0F, maximumAir);
            const int full = static_cast<int>(std::ceil((air - 2.0F) * 10.0F / maximumAir));
            const int partial =
                static_cast<int>(std::ceil(air * 10.0F / maximumAir)) - full;
            for (int index = 0; index < full + partial; ++index) {
                drawGuiSprite(
                    commandBuffer,
                    iconRect(right - static_cast<float>(index) * step - icon, top - 10.0F * scale),
                    1.0F, {index < full ? 16.0F : 25.0F, 18.0F, 9.0F, 9.0F});
            }
        }
    }

    // Gui#renderExperienceBar: the 182x5 bar centred over the hotbar, seven
    // logical pixels above it. The experience system is not wired up yet, so
    // the bar always draws a static placeholder fill instead of reading gameSession.player()
    // progress; swap the constant for the real progress fraction when it lands.
    void drawExperienceBar(VkCommandBuffer commandBuffer,
                           const ui::HudLayout& layout) const {
        const float scale = layout.scale();
        const auto bar = layout.experienceBar();
        // Background (icons.png 0,64) then the green fill (icons.png 0,69).
        drawGuiSprite(commandBuffer, bar, 1.0F, {0.0F, 64.0F, 182.0F, 5.0F});
        constexpr float kPlaceholderExperienceProgress = 0.5F;
        if (kPlaceholderExperienceProgress > 0.0F) {
            // A partial fill samples only the leading columns of the sprite,
            // the way vanilla's blit(x, y, 0, 69, progressWidth, 5) does.
            const float filledWidth = kPlaceholderExperienceProgress * 182.0F * scale;
            drawGuiSprite(commandBuffer, {bar.x, bar.y, filledWidth, bar.height}, 1.0F,
                          {0.0F, 69.0F, kPlaceholderExperienceProgress * 182.0F, 5.0F});
        }
    }

    // 1.16.1's InGameHud#updateVignetteDarkness: darkness eases toward
    // clamp(1 - brightnessAtEyes, 0, 1) at 1% per tick. Brightness is the
    // overworld light-level curve g/(4 - 3g) with g = light / 15. Sky light is
    // dimmed by the same daylight factor the sky shader uses, so the dark
    // corners also appear at night and in caves.
    void updateVignetteDarkness(float deltaSeconds) {
        const auto daylight = world::DayNightCycle::state(gameSession.gameTimeSeconds());
        const float daylightFactor =
            std::clamp((daylight.skyBrightness - 0.08F) / 0.92F, 0.0F, 1.0F);
        const glm::vec3 eye = gameSession.player().eyePosition();
        const int eyeX = static_cast<int>(std::floor(eye.x));
        const int eyeY = static_cast<int>(std::floor(eye.y));
        const int eyeZ = static_cast<int>(std::floor(eye.z));
        const float sky = static_cast<float>(interactionWorld.skyLight(eyeX, eyeY, eyeZ)) / 15.0F;
        const float block =
            static_cast<float>(interactionWorld.blockLight(eyeX, eyeY, eyeZ)) / 15.0F;
        const float light = std::max(block, sky * daylightFactor);
        const float brightness = light / (4.0F - 3.0F * light);
        const float target = std::clamp(1.0F - brightness, 0.0F, 1.0F);
        vignetteDarkness_ += (target - vignetteDarkness_) * std::min(1.0F, 0.2F * deltaSeconds);
    }

    // ScreenEffectRenderer's damage tint, simplified to a full-screen red wash
    // that fades over the invulnerability window.
    void drawDamageOverlay(VkCommandBuffer commandBuffer) const {
        if (uiFrameData_.gameMode != gameplay::GameMode::Survival) {
            return;
        }
        constexpr int kFlashTicks = 10;
        if (uiFrameData_.ticksSinceDamage >= kFlashTicks) {
            return;
        }
        const float fade =
            1.0F - static_cast<float>(uiFrameData_.ticksSinceDamage) / static_cast<float>(kFlashTicks);
        drawHudQuad(commandBuffer,
                    {0.0F, 0.0F, static_cast<float>(swapchainExtent.width),
                     static_cast<float>(swapchainExtent.height)},
                    {0.65F, 0.0F, 0.0F, 0.32F * fade});
    }

    // 1.16.1's LanguageScreen: a "Language" title, a black scrollable list of
    // the available languages (each shown in its own language), the Force
    // Unicode Font toggle and the Done button. Clicking a row switches the
    // language at once, exactly like the vanilla LanguageSelectionList.
    void drawLanguageScreen(VkCommandBuffer commandBuffer, const ui::HudLayout& layout) const {
        const auto cursor = currentFramebufferCursor();
        const float scale = layout.scale();
        const std::string title = translated("options.language", "Language");
        drawHudText(commandBuffer, title,
                    (static_cast<float>(swapchainExtent.width) - hudTextWidth(title, scale)) *
                        0.5F,
                    14.0F * scale, scale, {1.0F, 1.0F, 1.0F, 1.0F});
        // The centred dark list box, styled like the save-selection screen's
        // list band: the dirt tile darkened to a near-black panel.
        const auto box = languageListBox(layout);
        drawGuiSprite(commandBuffer, box, 9.0F,
                      ui::tiledBackgroundSource(box.width, box.height, scale),
                      {32.0F / 255.0F, 32.0F / 255.0F, 32.0F / 255.0F, 1.0F});
        const std::size_t visible = languageVisibleRowCount();
        const std::size_t maximumFirst =
            menuSystem.languageCodes.size() > visible ? menuSystem.languageCodes.size() - visible : 0U;
        const std::size_t first = std::min(menuSystem.languageListFirstIndex, maximumFirst);
        for (std::size_t row = 0; row < visible; ++row) {
            const std::size_t index = first + row;
            if (index >= menuSystem.languageCodes.size()) {
                break;
            }
            const auto rectangle = languageRow(row, layout);
            const bool selected = menuSystem.languageCodes[index] == options.language;
            const bool hovered = rectangle.contains(cursor.x, cursor.y);
            if (selected || hovered) {
                drawHudQuad(commandBuffer, rectangle,
                            selected ? glm::vec4{0.30F, 0.30F, 0.30F, 0.95F}
                                     : glm::vec4{0.16F, 0.16F, 0.16F, 0.90F});
            }
            const std::string& name = index < menuSystem.languageDisplayNames.size()
                ? menuSystem.languageDisplayNames[index]
                : menuSystem.languageCodes[index];
            // Centred inside the box, exactly like 1.16.1's LanguageEntry#render,
            // which draws each name at width/2 - textWidth/2.
            drawHudText(commandBuffer, name,
                        rectangle.x + (rectangle.width - hudTextWidth(name, scale)) * 0.5F,
                        rectangle.y + 2.0F * scale, scale,
                        selected ? glm::vec4{1.0F, 1.0F, 1.0F, 1.0F}
                                 : glm::vec4{0.85F, 0.85F, 0.85F, 1.0F});
        }
        // A scrollbar thumb on the box's right edge when the list overflows,
        // mirroring the vanilla EntryListWidget's grey track.
        if (menuSystem.languageCodes.size() > visible) {
            const float trackTop = box.y + 2.0F * scale;
            const float trackHeight = box.height - 4.0F * scale;
            const float thumbHeight =
                std::max(trackHeight * static_cast<float>(visible) /
                             static_cast<float>(menuSystem.languageCodes.size()),
                         8.0F * scale);
            const float travel = std::max(trackHeight - thumbHeight, 1.0F);
            const float thumbY = trackTop +
                (maximumFirst > 0 ? static_cast<float>(first) / maximumFirst : 0.0F) * travel;
            drawHudQuad(commandBuffer,
                        {box.x + box.width - 7.0F * scale, thumbY, 4.0F * scale, thumbHeight},
                        {0.55F, 0.55F, 0.55F, 0.95F});
        }
        // The grey warning line between the list and the buttons, as vanilla
        // draws it at height - 56.
        const std::string warning = translated("options.languageWarning", "");
        if (!warning.empty()) {
            const std::string label = "(" + warning + ")";
            const float warningY = languageWarningY(layout);
            drawHudText(commandBuffer, label,
                        (static_cast<float>(swapchainExtent.width) -
                         hudTextWidth(label, scale)) *
                            0.5F,
                        warningY, scale, {0.5F, 0.5F, 0.5F, 1.0F});
        }
        const std::size_t buttonCount = menuButtonCount();
        for (std::size_t index = 0; index < buttonCount; ++index) {
            const auto button = menuButtonForIndex(index);
            const auto rectangle =
                frontendButtonRect(layout, ui::PageId::Language, index, buttonCount);
            drawMinecraftButton(commandBuffer, rectangle, menuButtonLabel(button),
                                ui::buttonVisualState(rectangle, cursor.x, cursor.y, true,
                                                      pressedMenuButton == button),
                                scale, glm::vec4{1.0F});
        }
    }

    void drawPauseMenu(VkCommandBuffer commandBuffer, const ui::HudLayout& layout) const {
        const auto cursor = currentFramebufferCursor();
        const bool deathScreen = menuSystem.pageStack.current() == ui::PageId::Death;
        // 1.16.1's PauseScreen calls Screen.renderBackground(), which paints the
        // same dark gray gradient used by the gameSession.inventory() screens over the frozen
        // world. The death screen keeps its dark red wash instead, and the
        // options screen opened from the title (no world) shows the plain
        // optimized dirt backdrop like every other menu screen.
        if (deathScreen) {
            drawHudQuad(commandBuffer,
                        {0.0F, 0.0F, static_cast<float>(swapchainExtent.width),
                         static_cast<float>(swapchainExtent.height)},
                        {0.25F, 0.0F, 0.0F, 0.58F});
        } else if (worldSessionActive) {
            drawScreenDimOverlay(commandBuffer);
        }
        const float scale = layout.scale();
        const std::string title = menuSystem.pageStack.current() == ui::PageId::Options
            ? translated("options.title", "Options")
            : (menuSystem.pageStack.current() == ui::PageId::Experimental
                   ? translated("menu.experimental", "实验性内容")
                   : (menuSystem.pageStack.current() == ui::PageId::VideoSettings
                   ? translated("options.videoTitle", "Video Settings")
                   : (menuSystem.pageStack.current() == ui::PageId::Controls
                          ? translated("controls.title", "Controls")
                          : (deathScreen ? translated("deathScreen.title", "You Died!")
                                         : translated("menu.game", "Game Menu")))));
        const std::size_t buttonCount = menuButtonCount();
        const auto firstButton =
            frontendButtonRect(layout, menuSystem.pageStack.current(), 0, buttonCount);
        const float titleScale = deathScreen ? scale * 2.0F : scale;
        drawHudText(commandBuffer, title,
                    (static_cast<float>(swapchainExtent.width) -
                     hudTextWidth(title, titleScale)) * 0.5F,
                    firstButton.y - 30.0F * titleScale, titleScale,
                    {1.0F, 1.0F, 1.0F, 1.0F});
        for (std::size_t index = 0; index < buttonCount; ++index) {
            const auto button = menuButtonForIndex(index);
            const auto rectangle =
                frontendButtonRect(layout, menuSystem.pageStack.current(), index, buttonCount);
            const auto state = ui::buttonVisualState(rectangle, cursor.x, cursor.y, true,
                                                     pressedMenuButton == button);
            if (button == MenuButton::ViewDistance || button == MenuButton::SimulationDistance ||
                button == MenuButton::MasterVolume) {
                const float value = button == MenuButton::ViewDistance
                    ? static_cast<float>(viewDistanceChunks - 2) / 34.0F
                    : (button == MenuButton::SimulationDistance
                           ? static_cast<float>(simulationDistanceChunks - 2) / 10.0F
                           : options.masterVolume);
                drawMinecraftSlider(commandBuffer, rectangle, menuButtonLabel(button), state,
                                    value, scale);
            } else {
                drawMinecraftButton(commandBuffer, rectangle, menuButtonLabel(button), state,
                                    scale);
            }
        }
        if (!menuSystem.saveStatus.empty()) {
            drawHudText(commandBuffer, menuSystem.saveStatus, 4.0F * scale,
                        static_cast<float>(swapchainExtent.height) - 12.0F * scale, scale,
                        {1.0F, 1.0F, 1.0F, 1.0F});
        }
    }

    void drawPlayerPreview(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet,
                           const ui::HudLayout& layout) const {
        const auto& pose = playerModelAnimator.pose();
        const auto preview = layout.playerPreview(uiFrameData_.gameMode == gameplay::GameMode::Creative);
        // Vanilla's drawEntity Y coordinate is the feet anchor.  Our cuboid
        // coordinate system spans -16..+16 around its origin, so convert the
        // anchor to a model center before projecting it into view space.
        // Vanilla model coordinates use 16 units per block. drawEntity's
        // scale therefore maps one model unit to entityScale / 16 pixels.
        const float modelPixelsPerUnit = preview.entityScale / 16.0F;
        const float pixelX = preview.feetAnchor.x;
        const float pixelY = preview.feetAnchor.y - 16.0F * modelPixelsPerUnit * layout.scale();
        const float ndcX = pixelX / static_cast<float>(swapchainExtent.width) * 2.0F - 1.0F;
        const float ndcY = 1.0F - pixelY / static_cast<float>(swapchainExtent.height) * 2.0F;
        constexpr float depth = 2.35F;
        const glm::mat4 projection = camera.projectionMatrix(
            static_cast<float>(swapchainExtent.width) / static_cast<float>(swapchainExtent.height),
            cameraFarPlane());
        const float viewUnitsPerGuiPixel =
            2.0F * depth /
            (static_cast<float>(swapchainExtent.height) * std::abs(projection[1][1])) *
            layout.scale();
        const float modelUnit = viewUnitsPerGuiPixel * modelPixelsPerUnit;
        const glm::vec3 origin{
            ndcX * depth / projection[0][0],
            ndcY * depth / std::abs(projection[1][1]) + pose.idleBob * 16.0F * modelUnit, -depth};

        VkClearAttachment depthClear{};
        depthClear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        depthClear.clearValue.depthStencil = {1.0F, 0U};
        const VkClearRect clearRect{{{0, 0}, swapchainExtent}, 0U, 1U};
        vkCmdClearAttachments(commandBuffer, 1U, &depthClear, 1U, &clearRect);
        const int scissorX = std::max(0, static_cast<int>(std::floor(preview.clip.x)));
        const int scissorY = std::max(0, static_cast<int>(std::floor(preview.clip.y)));
        const int scissorRight =
            std::min(static_cast<int>(swapchainExtent.width),
                     static_cast<int>(std::ceil(preview.clip.x + preview.clip.width)));
        const int scissorBottom =
            std::min(static_cast<int>(swapchainExtent.height),
                     static_cast<int>(std::ceil(preview.clip.y + preview.clip.height)));
        const VkRect2D previewScissor{
            {scissorX, scissorY},
            {static_cast<std::uint32_t>(std::max(scissorRight - scissorX, 0)),
             static_cast<std::uint32_t>(std::max(scissorBottom - scissorY, 0))},
        };
        vkCmdSetScissor(commandBuffer, 0, 1, &previewScissor);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, heldItemPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, itemPipelineLayout,
                                0, 1, &descriptorSet, 0, nullptr);
        // The preview now renders through the same skeletal pose as the world
        // player (drawWorldPlayer): each bone's cube is drawn from
        // modelRoot * boneWorld * cubeRotation * T(centre), so the bone
        // hierarchy — head and arms as children of the body — composes, and the
        // whole figure turns rigidly with the cursor look instead of every part
        // spinning around its own centre. `origin` is in the camera's view
        // space, so the matrix cuboid is the matrixViewModel mode (data.x=6)
        // that projects through camera.projection without a second view pass.
        const auto& previewModel = playerModelAnimator.model();
        const auto& skeletonPose = playerModelAnimator.skeletonPose();
        // The geometry's feet sit at model y=0 while the previous hardcoded
        // layout anchored them 16 units below `origin`; shift the model root so
        // the figure lands in the same well.
        const glm::mat4 modelRoot =
            glm::translate(glm::mat4{1.0F}, origin) *
            glm::scale(glm::mat4{1.0F}, glm::vec3{modelUnit}) *
            glm::translate(glm::mat4{1.0F}, glm::vec3{0.0F, -16.0F, 0.0F});
        const auto layerForBone = [](std::string_view name) -> float {
            if (name == "head") return kPlayerHeadFirstLayer;
            if (name == "body") return kPlayerBodyFirstLayer;
            if (name == "rightArm") return kPlayerRightArmFirstLayer;
            if (name == "leftArm") return kPlayerLeftArmFirstLayer;
            if (name == "rightLeg") return kPlayerRightLegFirstLayer;
            if (name == "leftLeg") return kPlayerLeftLegFirstLayer;
            return -1.0F;
        };
        const auto pushPreviewCuboid = [&](const glm::mat4& cubeWorld, glm::vec3 dimensions,
                                           float layer) {
            const ItemPush push{
                {0.0F, 0.0F, 0.0F, 1.0F},
                {layer, 0.0F, 0.0F, 1.0F},
                {6.0F, 0.0F, 0.0F, 1.0F},
                {dimensions.x, dimensions.y, dimensions.z, 0.0F},
                cubeWorld,
            };
            vkCmdPushConstants(commandBuffer, itemPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                               sizeof(push), &push);
            vkCmdDraw(commandBuffer, 36U, 1, 0, 0);
        };
        for (std::size_t index = 0; index < previewModel.boneCount(); ++index) {
            const auto& bone = previewModel.bones()[index];
            const float layer = layerForBone(bone.name);
            if (layer < 0.0F) {
                continue;
            }
            const glm::mat4 boneWorld = skeletonPose.worldMatrix(static_cast<int>(index));
            for (const auto& cube : bone.cubes) {
                const glm::mat4 cubeRotation =
                    cube.hasRotation ? animation::rotationAboutPivot(cube.rotation, cube.pivot)
                                     : glm::mat4{1.0F};
                const glm::mat4 cubeWorld = modelRoot * boneWorld * cubeRotation *
                                            glm::translate(glm::mat4{1.0F}, cube.center());
                pushPreviewCuboid(cubeWorld, cube.renderSize(), layer);
            }
        }
        const VkRect2D fullScissor{{0, 0}, swapchainExtent};
        vkCmdSetScissor(commandBuffer, 0, 1, &fullScissor);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipelineLayout,
                                0, 1, &descriptorSet, 0, nullptr);
    }

    void drawWorkContainer(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet,
                           const ui::HudLayout& layout) const {
        drawScreenDimOverlay(commandBuffer);
        const auto panel = layout.inventoryPanel();
        const bool chestScreen = containerScreen == ContainerScreen::Chest;
        drawGuiSprite(
            commandBuffer, panel,
            chestScreen ? 10.0F : (containerScreen == ContainerScreen::CraftingTable ? 7.0F : 8.0F),
            {0.0F, 0.0F, 176.0F, 166.0F});
        if (chestScreen) {
            drawHudText(commandBuffer, translated("container.chest", "Chest"),
                        panel.x + 8.0F * layout.scale(),
                        panel.y + 6.0F * layout.scale(), layout.scale(),
                        {0.25F, 0.25F, 0.25F, 1.0F}, false);
            drawHudText(commandBuffer, translated("container.inventory", "Inventory"),
                        panel.x + 8.0F * layout.scale(),
                        panel.y + 73.0F * layout.scale(), layout.scale(),
                        {0.25F, 0.25F, 0.25F, 1.0F}, false);
            if (activeChest.has_value()) {
                if (const auto* chest = gameSession.chestSystem().find(*activeChest); chest != nullptr) {
                    for (std::size_t index = 0; index < gameplay::ChestBlockEntity::kSlotCount;
                         ++index) {
                        drawHudSlot(commandBuffer, layout.chestSlot(index), chest->items[index],
                                    false, false, true);
                    }
                }
            }
        } else if (containerScreen == ContainerScreen::CraftingTable) {
            for (std::size_t index = 0; index < 9U; ++index) {
                drawHudSlot(commandBuffer, layout.tableCraftingSlot(index),
                            gameSession.craftingSystem().tableSlot(index), false, false, true);
            }
            drawHudSlot(commandBuffer, layout.tableCraftingOutput(), gameSession.craftingSystem().tableOutput(),
                        false, false, true);
        } else {
            drawHudSlot(commandBuffer, layout.furnaceInputSlot(), gameSession.craftingSystem().furnaceInput(),
                        false, false, true);
            drawHudSlot(commandBuffer, layout.furnaceFuelSlot(), gameSession.craftingSystem().furnaceFuel(),
                        false, false, true);
            drawHudSlot(commandBuffer, layout.furnaceOutputSlot(), gameSession.craftingSystem().furnaceOutput(),
                        false, false, true);
            const float scale = layout.scale();
            const float fuel = std::clamp(gameSession.craftingSystem().furnaceFuelProgress(), 0.0F, 1.0F);
            if (fuel > 0.0F) {
                const float height = std::ceil(13.0F * fuel);
                drawGuiSprite(commandBuffer,
                              {panel.x + 57.0F * scale, panel.y + (36.0F + 13.0F - height) * scale,
                               14.0F * scale, height * scale},
                              8.0F, {176.0F, 13.0F - height, 14.0F, height});
            }
            const float progress = std::clamp(gameSession.craftingSystem().furnaceProgress(), 0.0F, 1.0F);
            if (progress > 0.0F) {
                const float width = std::ceil(24.0F * progress);
                drawGuiSprite(commandBuffer,
                              {panel.x + 79.0F * scale, panel.y + 34.0F * scale, width * scale,
                               17.0F * scale},
                              8.0F, {176.0F, 14.0F, width, 17.0F});
            }
        }
        for (std::size_t index = 0; index < gameplay::Inventory::kSlotCount; ++index) {
            const auto slot =
                chestScreen ? layout.chestInventorySlot(index) : layout.inventorySlot(index);
            drawHudSlot(commandBuffer, slot, gameSession.inventory().slot(index),
                        index == uiFrameData_.selectedHotbarSlot, false, true);
        }
        // An in-progress drag previews the would-be placement in every swept
        // slot before the release, on top of the slots but under the cursor.
        drawDragPreview(commandBuffer, layout);
        if (!gameSession.inventory().cursorStack().empty()) {
            const auto cursor = currentFramebufferCursor();
            const float size = 16.0F * layout.scale();
            drawHudSlot(commandBuffer, {cursor.x - size * 0.5F, cursor.y - size * 0.5F, size, size},
                        gameSession.inventory().cursorStack(), false, false, true);
        }
        static_cast<void>(descriptorSet);
    }

    void drawCreativeInventory(VkCommandBuffer commandBuffer, const ui::HudLayout& layout) const {
        const auto cursor = currentFramebufferCursor();
        const float scale = layout.scale();
        const auto panel = layout.creativePanel();
        drawScreenDimOverlay(commandBuffer);

        const std::size_t selectedTabIndex = static_cast<std::size_t>(menuSystem.creativeTab);
        // Tabs 0..5 sit on the top row; Spawn Eggs (6) and Inventory (7) share
        // the bottom row and use the bottom tab sprite.
        const std::size_t firstBottomTab = static_cast<std::size_t>(ui::CreativeTab::SpawnEggs);
        for (std::size_t tabIndex = 0; tabIndex < kCreativeTabCount; ++tabIndex) {
            const bool selected = tabIndex == selectedTabIndex;
            if (!selected) {
                const bool bottomTab = tabIndex >= firstBottomTab;
                drawGuiSprite(commandBuffer, layout.creativeTab(tabIndex), 4.0F,
                              {bottomTab ? 140.0F : static_cast<float>(tabIndex) * 28.0F,
                               bottomTab ? 64.0F : 0.0F, 28.0F, 32.0F});
            }
        }
        drawGuiSprite(commandBuffer, panel, menuSystem.creativeTab == ui::CreativeTab::Inventory ? 5.0F : 3.0F,
                      {0.0F, 0.0F, 195.0F, 136.0F});
        if (menuSystem.creativeTab == ui::CreativeTab::Inventory) {
            drawPlayerPreview(commandBuffer, frames[currentFrame].descriptorSet, layout);
        }

        const bool selectedBottomTab = selectedTabIndex >= firstBottomTab;
        drawGuiSprite(commandBuffer, layout.creativeTab(selectedTabIndex), 4.0F,
                      {selectedBottomTab ? 140.0F : static_cast<float>(selectedTabIndex) * 28.0F,
                       selectedBottomTab ? 96.0F : 32.0F, 28.0F, 32.0F});
        const std::array<gameplay::ItemStack, kCreativeTabCount> tabIcons{{
            {world::Block::Bricks, 1U},
            {world::Block::Dandelion, 1U},
            {world::Block::Chest, 1U},
            {world::Block::Air, 1U, &gameplay::items::IronIngot},
            {world::Block::Air, 1U, &gameplay::items::Apple},
            {world::Block::Air, 1U, &gameplay::items::DiamondPickaxe},
            {world::Block::Air, 1U, &gameplay::items::PigSpawnEgg},
            {world::Block::CraftingTable, 1U},
        }};
        for (std::size_t tabIndex = 0; tabIndex < tabIcons.size(); ++tabIndex) {
            const auto tab = layout.creativeTab(tabIndex);
            drawHudItemIcon(commandBuffer,
                            {tab.x + 6.0F * scale,
                             tab.y + (tabIndex >= firstBottomTab ? 7.0F : 9.0F) * scale,
                             16.0F * scale, 16.0F * scale},
                            tabIcons[tabIndex]);
        }

        std::optional<gameplay::ItemStack> hoveredStack;
        if (menuSystem.creativeTab == ui::CreativeTab::Inventory) {
            for (std::size_t index = 0; index < gameplay::Inventory::kSlotCount; ++index) {
                const auto slot = layout.creativeInventorySlot(index);
                const bool hovered = slot.contains(cursor.x, cursor.y);
                if (hovered && !gameSession.inventory().slot(index).empty()) {
                    hoveredStack = gameSession.inventory().slot(index);
                }
                drawHudSlot(commandBuffer, slot, gameSession.inventory().slot(index),
                            index == uiFrameData_.selectedHotbarSlot, hovered, true);
            }
            const auto deleteSlot = layout.creativeDeleteSlot();
            if (deleteSlot.contains(cursor.x, cursor.y)) {
                drawHudQuad(commandBuffer, deleteSlot, {1.0F, 0.25F, 0.25F, 0.34F});
            }
        } else {
            constexpr std::array<std::pair<std::string_view, std::string_view>, 7> titles{{
                {"itemGroup.buildingBlocks", "Building Blocks"},
                {"itemGroup.decorations", "Decoration Blocks"},
                {"itemGroup.redstone", "Functional Blocks"},
                {"itemGroup.materials", "Materials"},
                {"itemGroup.food", "Foodstuffs"},
                {"itemGroup.tools", "Tools"},
                {"itemGroup.misc", "Spawn Eggs"},
            }};
            const auto title =
                translated(titles[selectedTabIndex].first, titles[selectedTabIndex].second);
            drawHudText(commandBuffer, title, panel.x + 8.0F * scale, panel.y + 6.0F * scale, scale,
                        {0.25F, 0.25F, 0.25F, 1.0F}, false);

            const bool hasScrollbar = creativeMaximumScrollRow() > 0U;
            drawGuiSprite(commandBuffer, layout.creativeScrollbarThumb(creativeScrollPosition()),
                          4.0F, {hasScrollbar ? 232.0F : 244.0F, 0.0F, 12.0F, 15.0F});

            const auto catalog = activeCreativeCatalog();
            const std::size_t firstCatalogIndex = menuSystem.creativeScrollRow * 9U;
            for (std::size_t visibleIndex = 0; visibleIndex < ui::HudLayout::kCreativeVisibleSlots;
                 ++visibleIndex) {
                const std::size_t catalogIndex = firstCatalogIndex + visibleIndex;
                if (catalogIndex >= catalog.size()) {
                    break;
                }
                const auto slot = layout.creativeSlot(visibleIndex);
                const bool hovered = slot.contains(cursor.x, cursor.y);
                if (hovered) {
                    hoveredStack = catalog[catalogIndex];
                }
                drawHudSlot(commandBuffer, slot, catalog[catalogIndex], false, hovered, true);
            }
            for (std::size_t index = 0; index < gameplay::Inventory::kHotbarSize; ++index) {
                const auto slot = layout.creativeHotbarSlot(index);
                drawHudSlot(commandBuffer, slot, gameSession.inventory().slot(index),
                            index == uiFrameData_.selectedHotbarSlot,
                            slot.contains(cursor.x, cursor.y), true);
            }
        }

        // Real creative inventory/hotbar slots share QUICK_CRAFT with survival;
        // show the same prospective per-slot counts before the button is released.
        drawDragPreview(commandBuffer, layout);

        if (hoveredStack.has_value()) {
            const std::string label = itemDisplayName(*hoveredStack);
            const ui::UiRect tooltip{
                cursor.x + 12.0F * scale,
                cursor.y + 12.0F * scale,
                hudTextWidth(label, scale) + 8.0F * scale,
                14.0F * scale,
            };
            drawHudQuad(commandBuffer, tooltip, {0.05F, 0.03F, 0.08F, 0.94F});
            drawHudText(commandBuffer, label, tooltip.x + 4.0F * scale, tooltip.y + 3.0F * scale,
                        scale, {1.0F, 1.0F, 1.0F, 1.0F});
        }
        if (!gameSession.inventory().cursorStack().empty()) {
            const float iconSize = 16.0F * scale;
            const ui::UiRect cursorRectangle{
                cursor.x - iconSize * 0.5F,
                cursor.y - iconSize * 0.5F,
                iconSize,
                iconSize,
            };
            drawHudItemIcon(commandBuffer, cursorRectangle, gameSession.inventory().cursorStack());
            drawDurabilityBar(commandBuffer, cursorRectangle, gameSession.inventory().cursorStack());
            if (gameSession.inventory().cursorStack().count > 1U) {
                const std::string count = std::to_string(gameSession.inventory().cursorStack().count);
                drawHudText(commandBuffer, count,
                            cursorRectangle.x + 17.0F * scale - hudTextWidth(count, scale),
                            cursorRectangle.y + 9.0F * scale, scale, {1.0F, 1.0F, 1.0F, 1.0F});
            }
        }
    }

    void drawChatOverlay(VkCommandBuffer commandBuffer, const ui::HudLayout& layout) const {
        const float scale = layout.scale();
        float messageY = chatOpen ? layout.chatInput().y - 12.0F * scale
                                  : static_cast<float>(swapchainExtent.height) - 28.0F * scale;
        const auto messages = chatHistory.messages();
        for (auto message = messages.rbegin(); message != messages.rend(); ++message) {
            if (!chatOpen && gameSession.gameTimeSeconds() >= message->createdAt + 5.0) {
                continue;
            }
            if (messageY < 2.0F * scale) {
                break;
            }
            drawHudQuad(commandBuffer,
                        {2.0F * scale, messageY, hudTextWidth(message->text, scale) + 4.0F * scale,
                         11.0F * scale},
                        {0.0F, 0.0F, 0.0F, 0.55F});
            drawHudText(commandBuffer, message->text, 4.0F * scale, messageY + scale, scale,
                        message->successful ? glm::vec4{1.0F, 1.0F, 1.0F, 1.0F}
                                            : glm::vec4{1.0F, 0.35F, 0.35F, 1.0F},
                        false);
            messageY -= 11.0F * scale;
        }
        if (!chatOpen) {
            return;
        }
        // Vanilla sizes the chat input to the whole screen width (GuiChat 1.16.1
        // gives its EditBox a width of windowWidth - 8), so the dark backdrop
        // always runs from the left edge to the right edge instead of hugging
        // the typed text.
        const ui::UiRect input = layout.chatInput();
        drawHudQuad(commandBuffer, input, {0.0F, 0.0F, 0.0F, 0.72F});
        const bool cursorVisible = static_cast<int>(gameSession.gameTimeSeconds() * 2.0) % 2 == 0;
        const std::string visibleText = chatInputText + (cursorVisible ? "_" : "");
        drawHudText(commandBuffer, visibleText, input.x + 2.0F * scale, input.y + 2.0F * scale,
                    scale, {1.0F, 1.0F, 1.0F, 1.0F}, false);
        // 1.16.1's CommandSuggestor renders up to eight completions stacked above
        // the input, the currently selected (Tab-cycled) row highlighted.
        const std::size_t maxRows = std::min<std::size_t>(chatSuggestions_.size(), 8U);
        for (std::size_t row = 0; row < maxRows; ++row) {
            const auto& suggestion = chatSuggestions_[row];
            const float rowY = input.y - (static_cast<float>(row) + 1.0F) * 11.0F * scale;
            const bool selected = row == chatSuggestionIndex_;
            const std::string label =
                suggestion.text +
                (suggestion.hint.empty() ? "" : " — " + suggestion.hint);
            drawHudQuad(commandBuffer,
                        {2.0F * scale, rowY, hudTextWidth(label, scale) + 4.0F * scale,
                         11.0F * scale},
                        selected ? glm::vec4{0.2F, 0.3F, 0.6F, 0.8F}
                                 : glm::vec4{0.0F, 0.0F, 0.0F, 0.6F});
            drawHudText(commandBuffer, label, 4.0F * scale, rowY + scale, scale,
                        {1.0F, 1.0F, 1.0F, 1.0F}, false);
        }
    }

    // 1.16.1's InGameHud.render, consolidated into a single layer so every
    // element that belongs to the HUD — the damage tint, vignette, first-person
    // held item, hotbar, survival status bars, crosshair and the held-item name —
    // is drawn together in vanilla order. drawHud then draws any open screen
    // (gameSession.inventory(), container, pause) on top, so its renderBackground gradient
    // darkens this whole layer uniformly instead of leaving individual elements
    // floating bright over the overlay.
    void drawInGameHudLayer(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet,
                            const ui::HudLayout& layout) const {
        // 1.16.1's GameRenderer renders the hand right after the world, against
        // a fresh depth buffer, and InGameHud.render (vignette, hotbar, ...)
        // follows it. So the hand sits at the very bottom of the HUD layer: the
        // damage tint, the vignette and any open screen's gradient are all drawn
        // over it afterwards.
        VkClearAttachment heldDepthClear{};
        heldDepthClear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        heldDepthClear.clearValue.depthStencil = {1.0F, 0U};
        const VkClearRect heldDepthRect{
            {{0, 0}, swapchainExtent},
            0U,
            1U,
        };
        vkCmdClearAttachments(commandBuffer, 1U, &heldDepthClear, 1U, &heldDepthRect);
        drawHeldItem(commandBuffer, descriptorSet);
        drawUnderwaterOverlay(commandBuffer, descriptorSet);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipeline);

        drawDamageOverlay(commandBuffer);
        drawVignette(commandBuffer, descriptorSet);

        // HUD hotbar and the survival status bars. The gameSession.player() gameSession.inventory() keeps
        // the HUD hotbar on screen in both survival and creative; container
        // screens keep their previous look, and status bars stay survival-only.
        const bool playerInventoryOpen =
            inventoryOpen && containerScreen == ContainerScreen::PlayerInventory;
        if (!inventoryOpen || playerInventoryOpen) {
            drawGuiSprite(commandBuffer, layout.hotbarBackground(), 0.0F,
                          {0.0F, 0.0F, 182.0F, 22.0F});
            drawGuiSprite(commandBuffer,
                          layout.hotbarSelection(uiFrameData_.selectedHotbarSlot), 0.0F,
                          {0.0F, 22.0F, 24.0F, 24.0F});
            for (std::size_t index = 0; index < gameplay::Inventory::kHotbarSize; ++index) {
                drawHudSlot(commandBuffer, layout.hotbarSlot(index), activeInventory().slot(index),
                            index == uiFrameData_.selectedHotbarSlot, false, true);
            }
            if (uiFrameData_.gameMode == gameplay::GameMode::Survival) {
                drawSurvivalStatusBars(commandBuffer, layout);
                drawExperienceBar(commandBuffer, layout);
            }
        }

        if (!inventoryOpen) {
            const float textScale = layout.scale();
            // GuiIngame#renderSelectedItemName: the held item's name appears
            // when the selection changes and fades out over two seconds, and an
            // empty hand shows nothing at all (no "Air" label).
            const auto& selectedStack = uiFrameData_.selectedStack;
            if (!selectedStack.empty()) {
                const std::size_t selectedSlot = uiFrameData_.selectedHotbarSlot;
                const bool selectionChanged =
                    selectedNameSlot_ == static_cast<std::size_t>(-1) ||
                    selectedSlot != selectedNameSlot_ ||
                    !gameplay::sameItem(selectedStack, selectedNameStack_);
                if (selectionChanged) {
                    selectedNameSlot_ = selectedSlot;
                    selectedNameStack_ = selectedStack;
                    selectedNameShownAt_ = gameSession.gameTimeSeconds();
                }
                const double elapsed = gameSession.gameTimeSeconds() - selectedNameShownAt_;
                float alpha = 0.0F;
                if (elapsed < 2.0) {
                    // Full brightness for the first 1.5 s, then a half-second
                    // fade, mirroring the vanilla highlight's tail.
                    alpha = elapsed <= 1.5
                                ? 1.0F
                                : static_cast<float>((2.0 - elapsed) / 0.5);
                }
                if (alpha > 0.0F) {
                    const std::string selectedName = itemDisplayName(selectedStack);
                    drawHudText(commandBuffer, selectedName,
                                (static_cast<float>(swapchainExtent.width) -
                                 hudTextWidth(selectedName, textScale)) *
                                    0.5F,
                                layout.hotbarBackground().y -
                                    (uiFrameData_.gameMode == gameplay::GameMode::Survival ? 30.0F
                                                                              : 12.0F) *
                                        textScale,
                                textScale, {1.0F, 1.0F, 1.0F, alpha});
                }
            } else {
                selectedNameSlot_ = static_cast<std::size_t>(-1);
                selectedNameStack_ = {};
                selectedNameShownAt_ = -1.0;
            }
        }

        // Crosshair — vanilla InGameHud draws it every frame while in-game, and
        // an open screen's gradient darkens it along with the rest of the layer.
        const ui::HudLayout crosshairLayout{static_cast<float>(swapchainExtent.width),
                                            static_cast<float>(swapchainExtent.height),
                                            menuSystem.guiScaleSetting};
        const float crosshairSize = 15.0F * crosshairLayout.scale();
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, crosshairPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipelineLayout,
                                0, 1, &descriptorSet, 0, nullptr);
        drawMinecraftCrosshair(
            commandBuffer,
            {(static_cast<float>(swapchainExtent.width) - crosshairSize) * 0.5F,
             (static_cast<float>(swapchainExtent.height) - crosshairSize) * 0.5F, crosshairSize,
             crosshairSize});
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipeline);
    }

    void drawHud(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet) const {
        if (testScene.has_value()) return;
        const ui::HudLayout layout{static_cast<float>(swapchainExtent.width),
                                   static_cast<float>(swapchainExtent.height), menuSystem.guiScaleSetting};
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipelineLayout,
                                0, 1, &descriptorSet, 0, nullptr);

        const auto page = menuSystem.pageStack.current();
        if (page == ui::PageId::Title || page == ui::PageId::WorldList ||
            page == ui::PageId::CreateWorld || page == ui::PageId::EditWorld ||
            page == ui::PageId::ConfirmDelete) {
            drawFrontend(commandBuffer, layout, descriptorSet);
            return;
        }

        if (page == ui::PageId::Options || page == ui::PageId::VideoSettings ||
            page == ui::PageId::Controls || page == ui::PageId::Language ||
            page == ui::PageId::Experimental) {
            if (!worldSessionActive) {
                drawGuiSprite(commandBuffer,
                              {0.0F, 0.0F, static_cast<float>(swapchainExtent.width),
                               static_cast<float>(swapchainExtent.height)},
                              9.0F,
                              ui::tiledBackgroundSource(static_cast<float>(swapchainExtent.width),
                                                        static_cast<float>(swapchainExtent.height),
                                                        layout.scale()),
                              kMenuBackgroundTint);
            }
            if (page == ui::PageId::Language) {
                drawLanguageScreen(commandBuffer, layout);
            } else {
                drawPauseMenu(commandBuffer, layout);
            }
            return;
        }

        if (!worldReady) {
            drawGuiSprite(commandBuffer,
                          {0.0F, 0.0F, static_cast<float>(swapchainExtent.width),
                           static_cast<float>(swapchainExtent.height)},
                          9.0F,
                          ui::tiledBackgroundSource(static_cast<float>(swapchainExtent.width),
                                                    static_cast<float>(swapchainExtent.height),
                                                    layout.scale()),
                          kMenuBackgroundTint);
            const float scale = layout.scale();
            const float progress = peakPendingSectionCount == 0U
                                       ? 0.0F
                                       : 1.0F - static_cast<float>(pendingSectionUpdates.size()) /
                                                    static_cast<float>(peakPendingSectionCount);
            const std::string message = spawnPositionInitialized
                                            ? translated("multiplayer.downloadingTerrain",
                                                         "Loading terrain...") + " " +
                                                  std::to_string(static_cast<int>(std::lround(
                                                      std::clamp(progress, 0.0F, 1.0F) * 100.0F))) +
                                                  "%"
                                            : translated("menu.generatingTerrain",
                                                         "Preparing spawn area...");
            drawHudText(
                commandBuffer, message,
                (static_cast<float>(swapchainExtent.width) - hudTextWidth(message, scale)) * 0.5F,
                static_cast<float>(swapchainExtent.height) * 0.5F, scale, {1.0F, 1.0F, 1.0F, 1.0F});
            return;
        }

        if (paused) {
            drawPauseMenu(commandBuffer, layout);
            return;
        }

        drawInGameHudLayer(commandBuffer, descriptorSet, layout);

        if (inventoryOpen && containerScreen != ContainerScreen::PlayerInventory) {
            drawWorkContainer(commandBuffer, descriptorSet, layout);
        }

        if (inventoryOpen && containerScreen == ContainerScreen::PlayerInventory &&
            uiFrameData_.gameMode == gameplay::GameMode::Creative) {
            drawCreativeInventory(commandBuffer, layout);
        }

        if (inventoryOpen && containerScreen == ContainerScreen::PlayerInventory &&
            uiFrameData_.gameMode == gameplay::GameMode::Survival) {
            double cursorWindowX = 0.0;
            double cursorWindowY = 0.0;
            int windowWidth = 0;
            int windowHeight = 0;
            int framebufferWidth = 0;
            int framebufferHeight = 0;
            glfwGetCursorPos(window, &cursorWindowX, &cursorWindowY);
            glfwGetWindowSize(window, &windowWidth, &windowHeight);
            glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
            // Scale by the live framebuffer size, not the swapchain extent: right
            // after a resize or maximize the extent is still the previous frame's,
            // which would shift the cursor (and the white slot highlight) by a
            // few pixels until the swapchain is recreated.
            const auto framebufferCursor = ui::windowToFramebuffer(
                cursorWindowX, cursorWindowY, windowWidth, windowHeight,
                framebufferWidth, framebufferHeight);
            const float cursorX = framebufferCursor.x;
            const float cursorY = framebufferCursor.y;
            drawScreenDimOverlay(commandBuffer);
            const auto panel = layout.inventoryPanel();
            const float textScale = layout.scale();
            drawGuiSprite(commandBuffer, panel, 2.0F, {0.0F, 0.0F, 176.0F, 166.0F});
            drawPlayerPreview(commandBuffer, descriptorSet, layout);
            for (std::size_t index = 0; index < 4U; ++index) {
                drawHudSlot(commandBuffer, layout.playerCraftingSlot(index),
                            gameSession.craftingSystem().playerSlot(index), false, false, true);
            }
            drawHudSlot(commandBuffer, layout.playerCraftingOutput(), gameSession.craftingSystem().playerOutput(),
                        false, false, true);
            std::optional<std::size_t> hoveredSlot;
            for (std::size_t index = 0; index < gameplay::Inventory::kSlotCount; ++index) {
                const bool hovered = layout.inventorySlot(index).contains(cursorX, cursorY);
                if (hovered) {
                    hoveredSlot = index;
                }
                drawHudSlot(commandBuffer, layout.inventorySlot(index), gameSession.inventory().slot(index),
                            index == uiFrameData_.selectedHotbarSlot, hovered, true);
            }
            if (hoveredSlot.has_value() && !gameSession.inventory().slot(*hoveredSlot).empty()) {
                const auto& hoveredStack = gameSession.inventory().slot(*hoveredSlot);
                const std::string label = itemDisplayName(hoveredStack) + " x" +
                                          std::to_string(hoveredStack.count);
                const float labelWidth = hudTextWidth(label, textScale) + 8.0F * textScale;
                const ui::UiRect tooltip{
                    cursorX + 12.0F * textScale,
                    cursorY + 12.0F * textScale,
                    labelWidth,
                    14.0F * textScale,
                };
                drawHudQuad(commandBuffer, tooltip, {0.05F, 0.03F, 0.08F, 0.94F});
                drawHudText(commandBuffer, label, tooltip.x + 4.0F * textScale,
                            tooltip.y + 3.0F * textScale, textScale, {1.0F, 1.0F, 1.0F, 1.0F});
            }
            // An in-progress drag previews the would-be placement in every swept
            // slot before the release, on top of the slots but under the cursor.
            drawDragPreview(commandBuffer, layout);
            if (!gameSession.inventory().cursorStack().empty()) {
                const float cursorIconSize = 16.0F * layout.scale();
                const ui::UiRect cursorRectangle{cursorX - cursorIconSize * 0.5F,
                                                 cursorY - cursorIconSize * 0.5F, cursorIconSize,
                                                 cursorIconSize};
                drawHudItemIcon(commandBuffer, cursorRectangle, gameSession.inventory().cursorStack());
                drawDurabilityBar(commandBuffer, cursorRectangle, gameSession.inventory().cursorStack());
                if (gameSession.inventory().cursorStack().count > 1U) {
                    const std::string count = std::to_string(gameSession.inventory().cursorStack().count);
                    const float textScale = layout.scale();
                    drawHudText(
                        commandBuffer, count,
                        cursorRectangle.x + 17.0F * textScale - hudTextWidth(count, textScale),
                        cursorRectangle.y + 9.0F * textScale, textScale, {1.0F, 1.0F, 1.0F, 1.0F});
                }
            }
        }

        if (debugOverlayOpen) {
            std::ostringstream coordinates;
            coordinates << std::fixed << std::setprecision(3) << "XYZ: " << gameSession.player().position().x
                        << " / " << gameSession.player().position().y << " / " << gameSession.player().position().z;
            // Vanilla DebugHud samples the block the gameSession.player()'s feet are in, and
            // a resting gameSession.player()'s feet sit exactly on the integer boundary, so
            // floor() lands on the air cell above the ground block. Rebedrock
            // rests the feet a collision epsilon below that boundary, which
            // would round down into the solid block (block light 0 by
            // construction); nudging past the epsilon reproduces vanilla's
            // sample point.
            const glm::ivec3 playerBlock{static_cast<int>(std::floor(gameSession.player().position().x)),
                                         static_cast<int>(std::floor(gameSession.player().position().y + 0.001F)),
                                         static_cast<int>(std::floor(gameSession.player().position().z))};
            const std::array labels{
                options.version + " | FPS: " + std::to_string(displayedFps),
                coordinates.str(),
                std::string{"Light: sky "} +
                    std::to_string(
                        interactionWorld.skyLight(playerBlock.x, playerBlock.y, playerBlock.z)) +
                    " / block " +
                    std::to_string(
                        interactionWorld.blockLight(playerBlock.x, playerBlock.y, playerBlock.z)),
            };
            const float scale = layout.scale();
            const float textX = 2.0F * scale;
            const float textY = 2.0F * scale;
            for (std::size_t line = 0; line < labels.size(); ++line) {
                const float y = textY + static_cast<float>(line) * 10.0F * scale;
                drawHudQuad(commandBuffer,
                            {textX - scale, y - scale,
                             hudTextWidth(labels[line], scale) + 4.0F * scale, 11.0F * scale},
                            {0.0F, 0.0F, 0.0F, 0.55F});
                drawHudText(commandBuffer, labels[line], textX, y, scale,
                            {0.92F, 0.92F, 0.92F, 1.0F}, false);
            }
        }
        drawChatOverlay(commandBuffer, layout);
    }

    void drawItemEntities(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet) const {
        if (gameSession.itemEntities().entities().empty() && gameSession.worldSimulation().fallingBlocks().empty()) {
            return;
        }
        if (!gameSession.itemEntities().entities().empty()) {
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, itemShadowPipeline);
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    itemPipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
            for (const auto& entity : gameSession.itemEntities().entities()) {
                const glm::vec3 renderedPosition =
                    entity.previousPosition +
                    (entity.position - entity.previousPosition) * renderInterpolationAlpha;
                const int startY = static_cast<int>(std::floor(renderedPosition.y));
                std::optional<float> groundY;
                for (int y = startY; y >= std::max(0, startY - 12); --y) {
                    if (world::hasCollision(interactionWorld.block(
                            static_cast<int>(std::floor(renderedPosition.x)), y,
                            static_cast<int>(std::floor(renderedPosition.z))))) {
                        groundY = static_cast<float>(y + 1) + 0.003F;
                        break;
                    }
                }
                if (!groundY.has_value()) {
                    continue;
                }
                const float height = std::max(renderedPosition.y - *groundY, 0.0F);
                const float opacity = 0.30F * std::clamp(1.0F - height / 8.0F, 0.0F, 1.0F);
                if (opacity <= 0.001F) {
                    continue;
                }
                const ItemPush shadowPush{
                    {renderedPosition.x, *groundY, renderedPosition.z, 0.15F},
                    {0.0F, 0.0F, 0.0F, 0.0F},
                    {2.0F, opacity, 0.0F, 0.0F},
                    {0.0F, 0.0F, 0.0F, 0.0F},
                };
                vkCmdPushConstants(commandBuffer, itemPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                                   sizeof(shadowPush), &shadowPush);
                vkCmdDraw(commandBuffer, 36, 1, 0, 0);
            }
        }
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, itemPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, itemPipelineLayout,
                                0, 1, &descriptorSet, 0, nullptr);
        // The held item's generated 2.5D slab vertex count (item_entity.vert's
        // data.x in (6.5,7.5) mode): 12 front/back verts plus 16x16 edge quads.
        constexpr std::uint32_t kGeneratedItemVertexCount = 12U + 16U * 16U * 4U * 6U;
        // That mode projects through the view matrix baked into the transform
        // (gl_Position = projection * viewModelTransform), so dropped items get
        // the same view the world is drawn with — view bobbing included.
        const glm::mat4 cameraView = viewBobbingMatrix() * renderViewMatrix();
        for (const auto& entity : gameSession.itemEntities().entities()) {
            const glm::vec3 renderedPosition =
                entity.previousPosition +
                (entity.position - entity.previousPosition) * renderInterpolationAlpha;
            const bool cubeModel =
                gameplay::isBlockStack(entity.stack) &&
                (world::blockDefinition(entity.stack.block).model == world::BlockModel::Cube ||
                 world::blockDefinition(entity.stack.block).model == world::BlockModel::Chest);
            const auto layers =
                cubeModel ? world::textureLayers(entity.stack.block)
                          : world::BlockTextureLayers{gameplay::itemTextureLayer(entity.stack),
                                                      gameplay::itemTextureLayer(entity.stack),
                                                      gameplay::itemTextureLayer(entity.stack)};
            const float previousAge =
                entity.ageTicks == 0U ? 0.0F : static_cast<float>(entity.ageTicks - 1U);
            const float age = previousAge + renderInterpolationAlpha;
            // Float and spin are now driven by the animation library's display
            // entity preset (Molang-authored curves).
            const auto motion = itemDisplayAnimation.at(age, entity.visualPhase);
            const float bob = motion.bobHeight;
            const float rotation = motion.yawRadians;
            // Half a block up: the drop rests on the ground, so its own y rounds
            // into the block underneath it.
            const float packedLight =
                packedSceneLight(renderedPosition + glm::vec3{0.0F, 0.5F, 0.0F});
            if (cubeModel) {
                const ItemPush push{
                    {renderedPosition.x, renderedPosition.y + 0.18F + bob, renderedPosition.z,
                     0.30F},
                    {layers.top, layers.side, layers.bottom, rotation},
                    {1.0F, 0.0F, 0.0F, 0.0F},
                    {0.0F, 0.0F, 0.0F, packedLight},
                };
                vkCmdPushConstants(commandBuffer, itemPipelineLayout,
                                   VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
                vkCmdDraw(commandBuffer, 36U, 1, 0, 0);
            } else {
                // Non-block items share the held item's single-layer 3D model
                // instead of a flat camera-facing billboard: the item icon as a
                // thin slab with extruded edges, spinning about Y — the way
                // vanilla's ItemEntityRenderer draws the same ItemRenderer model
                // in GROUND transform.
                glm::mat4 dropTransform{1.0F};
                dropTransform = glm::translate(
                    dropTransform, {renderedPosition.x, renderedPosition.y + 0.18F + bob,
                                    renderedPosition.z});
                dropTransform = glm::rotate(dropTransform, rotation, {0.0F, 1.0F, 0.0F});
                dropTransform = glm::scale(dropTransform, glm::vec3{0.30F});
                const ItemPush push{
                    {0.0F, 0.0F, 0.0F, 0.30F},
                    {layers.top, layers.side, layers.bottom, 0.0F},
                    {7.0F, 0.0F, 0.0F, 0.0F},
                    {1.0F, 1.0F, 0.0625F, packedLight},
                    cameraView * dropTransform,
                };
                vkCmdPushConstants(commandBuffer, itemPipelineLayout,
                                   VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
                vkCmdDraw(commandBuffer, kGeneratedItemVertexCount, 1, 0, 0);
            }
        }
        for (const auto& entity : gameSession.worldSimulation().fallingBlocks()) {
            const glm::vec3 renderedPosition =
                entity.previousPosition +
                (entity.position - entity.previousPosition) * renderInterpolationAlpha;
            const auto layers = world::textureLayers(entity.block);
            // The falling block is drawn centred on its position, and the cell it
            // is falling through is air, so sample it directly.
            const ItemPush push{
                {renderedPosition.x, renderedPosition.y, renderedPosition.z, 1.0F},
                {layers.top, layers.side, layers.bottom, 0.0F},
                {1.0F, 0.0F, 0.0F, 0.0F},
                {0.0F, 0.0F, 0.0F, packedSceneLight(renderedPosition)},
            };
            vkCmdPushConstants(commandBuffer, itemPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                               sizeof(push), &push);
            vkCmdDraw(commandBuffer, 36, 1, 0, 0);
        }
    }

    // Particles are their own pass because vanilla draws them after the
    // translucent terrain layer, unlike the entities above, which belong to the
    // entity stage that runs before it.
    std::size_t drawParticles(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet) {
        const auto& particles = particleSystem.particles();
        if (particles.empty()) {
            return 0U;
        }
        // MC_REBEDROCK_LEGACY_PARTICLES keeps the old per-particle push-constant
        // draw so the instanced path can be compared directly.
        if (legacyParticles) {
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, itemPipeline);
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    itemPipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
            for (const auto& particle : particles) {
                const ItemPush push{
                    {particle.position.x, particle.position.y, particle.position.z, particle.size},
                    {particle.textureLayer, 0.0F, 0.0F, particle.opacity},
                    {-1.0F, particle.uvOrigin.x, particle.uvOrigin.y, particle.uvScale},
                    {0.0F, 0.0F, 0.0F, packedSceneLight(particle.position)},
                };
                vkCmdPushConstants(commandBuffer, itemPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                                   sizeof(push), &push);
                vkCmdDraw(commandBuffer, 6U, 1, 0, 0);
            }
            return 0U;
        }
        const std::size_t capacity = gpuSceneBuffer.capacityBytes() / sizeof(ParticleRecord);
        const std::size_t count = std::min(particles.size(), capacity);
        if (count == 0U) {
            return 0U;
        }
        std::vector<ParticleRecord> records;
        records.reserve(count);
        for (const auto& particle : particles) {
            if (records.size() == count) {
                break;
            }
            records.push_back(ParticleRecord{
                {particle.position.x, particle.position.y, particle.position.z, particle.size},
                {particle.uvOrigin.x, particle.uvOrigin.y, particle.uvScale, particle.opacity},
                {particle.textureLayer, packedSceneLight(particle.position), 0.0F, 0.0F},
            });
        }
        // Write the records into this frame's storage-buffer slot and flush. The
        // per-frame fence waited at the top of drawFrame orders these host writes
        // against the prior submission's read of the same slot, so no barrier is
        // needed; vmaFlushAllocation covers non-coherent heaps (a no-op on
        // unified Apple memory).
        auto& buffer = gpuSceneBuffer.frame(currentFrame);
        const std::size_t bytes = records.size() * sizeof(ParticleRecord);
        std::memcpy(buffer.mapped, records.data(), bytes);
        checkVk(vmaFlushAllocation(allocator, buffer.allocation, 0, bytes),
                "vmaFlushAllocation(particle scene buffer)");
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, particlePipeline);
        const std::array<VkDescriptorSet, 2> sets{descriptorSet, sceneDescriptorSets[currentFrame]};
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                particlePipelineLayout, 0, 2, sets.data(), 0, nullptr);
        vkCmdDraw(commandBuffer, 6U, static_cast<std::uint32_t>(records.size()), 0, 0);
        static bool reported = false;
        if (!reported) {
            reported = true;
            std::cout << "[particles] instanced 1 draw for " << records.size()
                      << " records (legacy = " << particles.size() << " draws)\n";
        }
        return records.size();
    }

    // Draws the weather rain through one of three paths sharing the same
    // CPU-simulated drops, so the particle-async claim can be measured against
    // a cheap texture baseline and the legacy per-particle path:
    //   texture   -> a few large scrolled water quads (rain_sheet pipeline)
    //   particles -> the old per-particle item-pipeline billboards
    //   async     -> one instanced draw from the scene storage buffer, with
    //                baseInstance pointing past the block-dust records
    void drawRain(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet,
                  std::size_t baseRecordCount) {
        const auto& drops = rainSystem.drops();
        if (drops.empty()) {
            return;
        }
        static bool reported = false;
        if (rainMode_ == RainMode::Texture) {
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, rainSheetPipeline);
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    rainSheetPipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
            for (const auto& drop : drops) {
                const RainSheetPush push{
                    {drop.position.x, drop.position.y, drop.position.z, drop.size * 30.0F},
                    {rainTime_, 0.30F, static_cast<float>(kWaterStillLayer), 0.0F},
                };
                vkCmdPushConstants(commandBuffer, rainSheetPipelineLayout,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof(push), &push);
                vkCmdDraw(commandBuffer, 6U, 1, 0, 0);
            }
            if (!reported && drops.size() >= rainTargetCount() * 9U / 10U) {
                reported = true;
                std::cout << "[rain] mode=texture sheets=" << drops.size()
                          << " draws=" << drops.size() << "\n";
            }
            return;
        }
        if (rainMode_ == RainMode::Particles) {
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, itemPipeline);
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    itemPipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
            for (const auto& drop : drops) {
                const ItemPush push{
                    {drop.position.x, drop.position.y, drop.position.z, drop.size},
                    {static_cast<float>(kWaterStillLayer), 0.0F, 0.0F, 0.6F},
                    {-1.0F, 0.0F, 0.0F, 1.0F},
                    {0.0F, 0.0F, 0.0F, packedSceneLight(drop.position)},
                };
                vkCmdPushConstants(commandBuffer, itemPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                                   sizeof(push), &push);
                vkCmdDraw(commandBuffer, 6U, 1, 0, 0);
            }
            if (!reported && drops.size() >= rainTargetCount() * 9U / 10U) {
                reported = true;
                std::cout << "[rain] mode=particles drops=" << drops.size()
                          << " draws=" << drops.size() << "\n";
            }
            return;
        }
        // Async: append the rain records after the block-dust records in the
        // same scene buffer and draw once with baseInstance past them.
        const std::size_t capacity = gpuSceneBuffer.capacityBytes() / sizeof(ParticleRecord);
        const std::size_t count = std::min(drops.size(), capacity - baseRecordCount);
        if (count == 0U) {
            return;
        }
        std::vector<ParticleRecord> records;
        records.reserve(count);
        for (const auto& drop : drops) {
            if (records.size() == count) {
                break;
            }
            records.push_back(ParticleRecord{
                {drop.position.x, drop.position.y, drop.position.z, drop.size},
                {0.0F, 0.0F, 1.0F, 0.6F},
                {static_cast<float>(kWaterStillLayer), packedSceneLight(drop.position), 0.0F, 0.0F},
            });
        }
        auto& buffer = gpuSceneBuffer.frame(currentFrame);
        const std::size_t baseOffset = baseRecordCount * sizeof(ParticleRecord);
        const std::size_t bytes = records.size() * sizeof(ParticleRecord);
        std::memcpy(static_cast<char*>(buffer.mapped) + baseOffset, records.data(), bytes);
        checkVk(vmaFlushAllocation(allocator, buffer.allocation, baseOffset, bytes),
                "vmaFlushAllocation(rain scene buffer)");
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, particlePipeline);
        const std::array<VkDescriptorSet, 2> sets{descriptorSet, sceneDescriptorSets[currentFrame]};
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                particlePipelineLayout, 0, 2, sets.data(), 0, nullptr);
        vkCmdDraw(commandBuffer, 6U, static_cast<std::uint32_t>(records.size()), 0,
                  static_cast<std::uint32_t>(baseRecordCount));
        if (!reported && records.size() >= rainTargetCount() * 9U / 10U) {
            reported = true;
            std::cout << "[rain] mode=async drops=" << records.size() << " draws=1\n";
        }
    }

    // Draws one axis-aligned cuboid transformed by a full world matrix through
    // the item shader's world-space skinned-cuboid mode (data.x = 8). The matrix
    // carries translation + orientation; `dimensions` is the box size in world
    // units; faces sample texture layers [layer, layer+5]. Shared by the chest
    // and the third-person gameSession.player() so a part's rotation is bound to its own local
    // frame rather than a fixed world axis.
    // `packedLight` is the entity's scene lightmap sample (see packedSceneLight);
    // 0 keeps the legacy fixed light. Every future block entity that draws
    // through here gets scene lighting by passing it.
    void pushWorldCuboid(VkCommandBuffer commandBuffer, const glm::mat4& worldMatrix,
                         glm::vec3 dimensions, float textureLayer,
                         float packedLight = 0.0F) const {
        const ItemPush push{
            {0.0F, 0.0F, 0.0F, 1.0F},
            {textureLayer, 0.0F, 0.0F, 0.0F},
            {8.0F, 0.0F, 0.0F, 0.0F},
            {dimensions.x, dimensions.y, dimensions.z, packedLight},
            worldMatrix,
        };
        vkCmdPushConstants(commandBuffer, itemPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(push), &push);
        vkCmdDraw(commandBuffer, 36U, 1, 0, 0);
    }

    void drawChestEntities(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet) const {
        if (gameSession.chestSystem().entities().empty())
            return;
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, itemPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, itemPipelineLayout,
                                0, 1, &descriptorSet, 0, nullptr);
        const auto drawWorldCuboid = [&](const glm::mat4& worldMatrix, glm::vec3 dimensions,
                                         float textureLayer, float packedLight) {
            pushWorldCuboid(commandBuffer, worldMatrix, dimensions, textureLayer, packedLight);
        };
        for (const auto& chest : gameSession.chestSystem().entities()) {
            const glm::vec3 origin{static_cast<float>(chest.position.x),
                                   static_cast<float>(chest.position.y),
                                   static_cast<float>(chest.position.z)};
            const auto orientation = interactionWorld.orientation(
                chest.position.x, chest.position.y, chest.position.z);
            const float yaw = orientation == world::BlockOrientation::East
                ? 1.57079632679F
                : (orientation == world::BlockOrientation::North
                       ? 3.14159265359F
                       : (orientation == world::BlockOrientation::West
                              ? -1.57079632679F : 0.0F));
            const auto rotateHorizontal = [yaw](glm::vec3 offset) {
                const float cosine = std::cos(yaw);
                const float sine = std::sin(yaw);
                return glm::vec3{cosine * offset.x + sine * offset.z, offset.y,
                                 -sine * offset.x + cosine * offset.z};
            };
            const glm::vec3 blockCenter = origin + glm::vec3{0.5F};
            const glm::mat4 yawMatrix = glm::rotate(glm::mat4{1.0F}, yaw, {0.0F, 1.0F, 0.0F});
            // A chest is a cutout block, so light propagates into its own cell:
            // sampling there is what vanilla does for block entities.
            const float packedLight = packedSceneLight(blockCenter);

            const glm::vec3 baseCenter = blockCenter + rotateHorizontal({0.0F, -0.1875F, 0.0F});
            drawWorldCuboid(glm::translate(glm::mat4{1.0F}, baseCenter) * yawMatrix,
                            {0.875F, 0.625F, 0.875F}, kChestBaseFirstLayer, packedLight);

            const float interpolatedLid =
                chest.previousLidAngle +
                (chest.lidAngle - chest.previousLidAngle) * renderInterpolationAlpha;
            // The lift angle comes from the data-driven hinge animation (its
            // Bezier tangents reproduce the previous cubic ease-out exactly).
            const float pitch = chestLidAnimation.liftRadians(interpolatedLid);
            // Rigid rotation about the hinge line: translate to the hinge, rotate
            // about the hinge's X axis, then translate the lid box from the hinge
            // to its closed centre. Composing these (rather than rotating the box
            // about its own centre and separately sliding the centre along the
            // hinge arc) keeps the hinge edge pinned so the lid pivots instead of
            // sliding across the chest opening. Negative pitch lifts the front
            // edge upward. Local frame first, then yaw to the placement heading.
            constexpr glm::vec3 hingeLocal{0.0F, 0.125F, -0.4375F};
            constexpr glm::vec3 closedCentreFromHinge{0.0F, 0.15625F, 0.4375F};
            const glm::mat4 lidMatrix =
                glm::translate(glm::mat4{1.0F}, blockCenter) * yawMatrix *
                glm::translate(glm::mat4{1.0F}, hingeLocal) *
                glm::rotate(glm::mat4{1.0F}, -pitch, {1.0F, 0.0F, 0.0F}) *
                glm::translate(glm::mat4{1.0F}, closedCentreFromHinge);
            drawWorldCuboid(lidMatrix, {0.875F, 0.3125F, 0.875F}, kChestLidFirstLayer,
                            packedLight);
        }
    }

    // Renders the gameSession.player() as multi-bone skinned cuboids in the world for the
    // third-person perspectives, driven by the same animation library as the
    // gameSession.inventory() preview but by the gameSession.player()'s own look/movement.
    void drawWorldPlayer(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet) const {
        if (cameraPerspective == CameraPerspective::FirstPerson || !worldReady) {
            return;
        }
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, itemPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, itemPipelineLayout,
                                0, 1, &descriptorSet, 0, nullptr);

        // The camera sits at the interpolated eye; anchor the model at the feet.
        const glm::vec3 feet = camera.position() - glm::vec3{0.0F, gameSession.player().eyeHeight(), 0.0F};
        // The body faces the lagged body yaw (the head turns relative to it via
        // the animator). If the model renders facing backwards, change
        // kFacingOffset to 3.14159265F.
        constexpr float kFacingOffset = 0.0F;
        const float facingYaw = worldBodyYaw + kFacingOffset;
        constexpr float kModelUnitsToBlocks = 1.0F / 16.0F;
        const glm::mat4 modelRoot =
            glm::translate(glm::mat4{1.0F}, feet) *
            glm::rotate(glm::mat4{1.0F}, facingYaw, glm::vec3{0.0F, 1.0F, 0.0F}) *
            glm::scale(glm::mat4{1.0F}, glm::vec3{kModelUnitsToBlocks});
        // Both third-person views share this path, so the gameSession.player() darkens with the
        // scene the same way the creatures around them do.
        const float packedLight = packedSceneLight(feet + glm::vec3{0.0F, 0.9F, 0.0F});

        const auto layerForBone = [](std::string_view name) -> float {
            if (name == "head") return kPlayerHeadFirstLayer;
            if (name == "body") return kPlayerBodyFirstLayer;
            if (name == "rightArm") return kPlayerRightArmFirstLayer;
            if (name == "leftArm") return kPlayerLeftArmFirstLayer;
            if (name == "rightLeg") return kPlayerRightLegFirstLayer;
            if (name == "leftLeg") return kPlayerLeftLegFirstLayer;
            return -1.0F; // e.g. the hat layer, skipped for now
        };

        const auto& model = worldPlayerAnimator.model();
        const auto& pose = worldPlayerAnimator.skeletonPose();
        for (std::size_t index = 0; index < model.boneCount(); ++index) {
            const auto& bone = model.bones()[index];
            const float layer = layerForBone(bone.name);
            if (layer < 0.0F) {
                continue;
            }
            const glm::mat4 boneWorld = pose.worldMatrix(static_cast<int>(index));
            for (const auto& cube : bone.cubes) {
                const glm::mat4 cubeRotation =
                    cube.hasRotation ? animation::rotationAboutPivot(cube.rotation, cube.pivot)
                                     : glm::mat4{1.0F};
                const glm::mat4 cubeWorld = modelRoot * boneWorld * cubeRotation *
                                            glm::translate(glm::mat4{1.0F}, cube.center());
                pushWorldCuboid(commandBuffer, cubeWorld, cube.renderSize(), layer, packedLight);
            }
        }
    }

    // The loaded species bound for `type`, or nullptr when that species has not
    // been loaded (its model failed to parse or it is not one of the shipped
    // set).
    [[nodiscard]] const gameplay::entities::SpeciesRenderModel* speciesFor(
        const gameplay::entities::EntityType* type) const {
        for (const auto& species : speciesModels) {
            if (species.type == type) {
                return &species;
            }
        }
        return nullptr;
    }

    // Whether the species' model is bound and ready to render. The spawn path
    // gates on this so an egg does not summon a creature that would appear as
    // a missing mesh.
    [[nodiscard]] bool entityModelReady(const gameplay::entities::EntityType* type) const {
        const auto* species = speciesFor(type);
        return species != nullptr && species->loaded;
    }

    // Samples the world lightmap once for a whole entity, the way vanilla does
    // (Entity#getLightmapCoordinates): one sky level and one block level taken at
    // the block the entity's body occupies, not per bone or per fragment. The
    // pair is packed into a single float because ItemPush already sits exactly on
    // Vulkan's guaranteed 128-byte push-constant limit and dimensions.w is the
    // only slot the cuboid modes leave free. 0 is reserved for "no scene light",
    // so the encoding is biased by one; item_entity.vert decodes it.
    // `samplePoint` is the point whose block is read, so each caller states where
    // its entity's body actually is: a ground-anchored entity has to sample half
    // a block up, because the feet sample rounds into the solid block underneath
    // and reads as pitch black.
    [[nodiscard]] float packedSceneLight(glm::vec3 samplePoint) const {
        const int blockX = static_cast<int>(std::floor(samplePoint.x));
        const int blockY = static_cast<int>(std::floor(samplePoint.y));
        const int blockZ = static_cast<int>(std::floor(samplePoint.z));
        const float sky = static_cast<float>(interactionWorld.skyLight(blockX, blockY, blockZ));
        const float block = static_cast<float>(interactionWorld.blockLight(blockX, blockY, blockZ));
        return 1.0F + sky + block * 16.0F;
    }

    // Draws one box-UV skinned cuboid (mode 9): the world matrix carries the
    // bone/cube transform, `renderSize` is the drawn cube extent in model units,
    // `uvSize` the (uninflated) size whose box-UV net is sampled and `uv` that
    // net's origin. `textureSize` is the model's declared texture_width/height:
    // the shader divides texel coordinates by it, exactly like Bedrock, so an
    // entity skin at a different pixel resolution than the declaration still
    // maps face-for-face. Samples the entity texture array (binding 4).
    void pushBoxUvCuboid(VkCommandBuffer commandBuffer, const glm::mat4& worldMatrix,
                         glm::vec3 renderSize, glm::vec3 uvSize, glm::vec2 uv, bool mirror,
                         glm::vec2 textureSize, std::uint32_t faceOverride, float layer,
                         float packedLight = 0.0F, float hurtFlash = 0.0F) const {
        // textureLayersRotation.w is free in the box-UV path; it carries the
        // per-face source/rotate override (ModelCube::faceOverride) as raw bits,
        // which the shader recovers with floatBitsToUint. dimensions.w is the
        // packed scene lightmap (see packedSceneLight); 0 keeps the fixed light.
        // positionSize.w is likewise free here, so it carries OverlayTexture's
        // hurt-row strength.
        const ItemPush push{
            {uvSize.x, uvSize.y, uvSize.z, hurtFlash},
            {layer, textureSize.x, textureSize.y, std::bit_cast<float>(faceOverride)},
            {9.0F, uv.x, uv.y, mirror ? 1.0F : 0.0F},
            {renderSize.x, renderSize.y, renderSize.z, packedLight},
            worldMatrix,
        };
        vkCmdPushConstants(commandBuffer, itemPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(push), &push);
        vkCmdDraw(commandBuffer, 36U, 1, 0, 0);
    }

    // Renders the free-roaming creatures as box-UV skinned pigs, driven by
    // the same animation library and interpolated between physics ticks like the
    // dropped items. The first real consumer of the box-UV entity pipeline.
    void drawWorldEntities(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet) const {
        if (!worldReady || gameSession.worldEntities().entities().empty()) {
            return;
        }
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, itemPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, itemPipelineLayout,
                                0, 1, &descriptorSet, 0, nullptr);
        constexpr float kPi = 3.14159265358979323846F;
        constexpr float kModelUnitsToBlocks = 1.0F / 16.0F;
        // Every shipped creature faces -Z (Minecraft's front), so a half turn
        // points its forward end along the wander heading. Adjust if a model
        // faces +Z.
        constexpr float kEntityFacingOffset = kPi;

        for (const auto& entity : gameSession.worldEntities().entities()) {
            // Each creature renders through its own species' model, animations
            // and texture layer; an entity whose species failed to load is
            // skipped rather than drawn as the wrong creature.
            const gameplay::entities::SpeciesRenderModel* species = speciesFor(entity.type);
            if (species == nullptr || !species->loaded) {
                continue;
            }
            // The clip identifiers come from the species' registered render
            // descriptor (its "geo mapping"), not from literals baked into the
            // renderer, so a new creature only has to declare them on its type.
            const auto& render = entity.type->render();
            const animation::AnimationClip* walk =
                species->model.animations.find(render.walkAnimation);
            const animation::AnimationClip* idle =
                species->model.animations.find(render.idleAnimation);

            const glm::vec3 position =
                entity.previousPosition +
                (entity.position - entity.previousPosition) * renderInterpolationAlpha;
            // Interpolate yaw along the shortest arc so a re-picked heading does
            // not spin the model the long way round.
            float deltaYaw = entity.yaw - entity.previousYaw;
            while (deltaYaw > kPi) deltaYaw -= 2.0F * kPi;
            while (deltaYaw < -kPi) deltaYaw += 2.0F * kPi;
            const float yaw = entity.previousYaw + deltaYaw * renderInterpolationAlpha;
            const float walkDistance =
                entity.previousWalkDistance +
                (entity.walkDistance - entity.previousWalkDistance) * renderInterpolationAlpha;
            const float perTickStride = entity.walkDistance - entity.previousWalkDistance;

            animation::Animator animator;
            animator.setModel(&species->model.model);
            animator.context().setVariable("walk_amount",
                                           std::clamp(perTickStride * 18.0F, 0.0F, 1.0F));
            animator.clearLayers();
            if (walk != nullptr) {
                // One leg cycle per block travelled reads as a natural gait.
                animator.addLayer(*walk, walk->localTime(walkDistance), 1.0F);
            }
            if (idle != nullptr) {
                animator.addLayer(*idle, idle->localTime(static_cast<float>(gameSession.gameTimeSeconds())), 1.0F);
            }
            const animation::SkeletonPose pose = animator.evaluate();

            // LivingEntityRenderer#getLyingAngle: a dying body tips ninety
            // degrees over the twenty ticks of deathTime, easing as it lands.
            float deathRoll = 0.0F;
            if (entity.damage.deathTicks > 0) {
                const float progress = std::min(
                    (static_cast<float>(entity.damage.deathTicks) +
                     renderInterpolationAlpha - 1.0F) /
                        20.0F * 1.6F,
                    1.0F);
                deathRoll = std::sqrt(std::max(progress, 0.0F)) * (kPi * 0.5F);
            }
            // LivingEntityRenderer#getOverlay: the hurt row is on for every tick
            // of hurtTime, and stays on for the whole death animation.
            const float hurtFlash =
                entity.damage.hurtTicks > 0 || entity.damage.deathTicks > 0 ? 1.0F : 0.0F;
            const glm::mat4 modelRoot =
                glm::translate(glm::mat4{1.0F}, position) *
                glm::rotate(glm::mat4{1.0F}, yaw + kEntityFacingOffset, glm::vec3{0.0F, 1.0F, 0.0F}) *
                glm::rotate(glm::mat4{1.0F}, deathRoll, glm::vec3{0.0F, 0.0F, 1.0F}) *
                glm::scale(glm::mat4{1.0F}, glm::vec3{kModelUnitsToBlocks});

            // One lightmap sample for the whole creature, so it darkens at dusk,
            // goes black in an unlit cave and picks up torchlight like the blocks
            // around it. Sampled from the interpolated render position, not the
            // tick position, so it changes smoothly as the creature walks. Half a
            // block up is the body, the height EntitySystem also tests for walls.
            const float packedLight = packedSceneLight(position + glm::vec3{0.0F, 0.5F, 0.0F});

            const auto& model = species->model.model;
            // The declared texture size is the box-UV coordinate space; the
            // atlas pixels only have to be a scaled copy of it. Cuboids sample
            // the species' own layer of the entity texture array.
            const glm::vec2 textureSize = gameplay::entities::entityTextureSize(
                model, {static_cast<float>(entityTextureWidth),
                        static_cast<float>(entityTextureHeight)});
            for (std::size_t index = 0; index < model.boneCount(); ++index) {
                const auto& bone = model.bones()[index];
                if (bone.neverRender) {
                    continue;
                }
                const glm::mat4 boneWorld = pose.worldMatrix(static_cast<int>(index));
                for (const auto& cube : bone.cubes) {
                    // Per-cube rotation happens around the cube's own pivot,
                    // inside the bone; `inflate` then grows the box about its
                    // centre without moving the UV net.
                    const glm::mat4 cubeRotation =
                        cube.hasRotation
                            ? animation::rotationAboutPivot(cube.rotation, cube.pivot)
                            : glm::mat4{1.0F};
                    const glm::mat4 cubeWorld =
                        modelRoot * boneWorld * cubeRotation *
                        glm::translate(glm::mat4{1.0F}, cube.center());
                    pushBoxUvCuboid(commandBuffer, cubeWorld, cube.renderSize(), cube.size, cube.uv,
                                    cube.mirror, textureSize, cube.faceOverride,
                                    species->textureLayer, packedLight, hurtFlash);
                }
            }
        }
    }

    void drawMiningProgress(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet) const {
        if (uiFrameData_.gameMode != gameplay::GameMode::Survival || !breakButtonHeld ||
            !miningTarget.has_value() || !targetedBlock.has_value() ||
            *miningTarget != targetedBlock->block) {
            return;
        }
        const auto block = *miningTarget;
        const auto target = interactionWorld.block(block.x, block.y, block.z);
        const float duration = gameplay::miningSeconds(target, uiFrameData_.selectedStack,
                                                       gameSession.player().inWater(), !gameSession.player().onGround());
        if (!std::isfinite(duration) || duration <= 0.0F)
            return;
        const float progress = std::clamp(
            static_cast<float>((gameSession.gameTimeSeconds() - miningStartedAt) / duration), 0.0F, 0.999F);
        // ClientPlayerInteractionManager reports (progress * 10) - 1, so the first
        // tenth of the dig carries no crack overlay at all.
        const int stage = std::clamp(static_cast<int>(progress * 10.0F) - 1, -1, 9);
        if (stage < 0) return;
        const float layer = kDestroyStageFirstLayer + static_cast<float>(stage);
        const ItemPush push{
            {static_cast<float>(block.x) + 0.5F, static_cast<float>(block.y) + 0.5F,
             static_cast<float>(block.z) + 0.5F, 1.006F},
            {layer, layer, layer, 0.0F},
            {1.0F, 0.0F, 0.0F, 0.0F},
            {0.0F, 0.0F, 0.0F, 0.0F},
        };
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, itemPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, itemPipelineLayout,
                                0, 1, &descriptorSet, 0, nullptr);
        vkCmdPushConstants(commandBuffer, itemPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(push), &push);
        vkCmdDraw(commandBuffer, 36U, 1U, 0U, 0U);
    }

    void drawHeldItem(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet) const {
        // The held item stays visible while the gameSession.inventory() or chat is open, as it
        // does in the original game; only a paused game or a non-first-person
        // camera hides the hand.
        if (!worldReady || paused || cameraPerspective != CameraPerspective::FirstPerson) {
            return;
        }
        const auto& stack = uiFrameData_.selectedStack;
        const bool emptyHand = stack.empty();
        const auto& pose = heldItemAnimation.pose();
        const bool cubeModel =
            !emptyHand && gameplay::isBlockStack(stack) &&
            (world::blockDefinition(stack.block).model == world::BlockModel::Cube ||
             world::blockDefinition(stack.block).model == world::BlockModel::Chest);
        const auto layers =
            emptyHand
                ? world::BlockTextureLayers{kPlayerRightArmFirstLayer, kPlayerRightArmFirstLayer,
                                            kPlayerRightArmFirstLayer}
            : cubeModel ? world::textureLayers(stack.block)
                        : world::BlockTextureLayers{gameplay::itemTextureLayer(stack),
                                                    gameplay::itemTextureLayer(stack),
                                                    gameplay::itemTextureLayer(stack)};
        const glm::mat4 heldTransform = viewBobbingMatrix() *
            (emptyHand ? animation::firstPersonArmTransform(pose)
                       : uiFrameData_.eating ? animation::firstPersonEatTransform(pose, cubeModel)
                                : animation::firstPersonItemTransform(pose, cubeModel));
        const float heldFrontLayer = !emptyHand && gameplay::isBlockStack(stack)
            ? (stack.block == world::Block::Chest
                   ? kChestItemFrontLayer
                   : (stack.block == world::Block::Furnace ? kFurnaceFrontLayer : 0.0F))
            : 0.0F;
        // The held hand/block follows the ambient light at the gameSession.player()'s eye so
        // it darkens at night instead of staying under the fixed legacy light.
        const float heldLight = packedSceneLight(camera.position());
        const ItemPush push{
            {0.0F, 0.0F, 0.0F, 1.0F},
            {layers.top, layers.side, layers.bottom, heldFrontLayer},
            {(!emptyHand && !cubeModel) ? 7.0F : 6.0F,
             0.0F, emptyHand ? 1.0F : 0.0F, emptyHand ? 1.0F : 0.0F},
            emptyHand ? glm::vec4{0.25F, 0.75F, 0.25F, heldLight}
                      : (cubeModel ? glm::vec4{1.0F, 1.0F, 1.0F, heldLight}
                                   : glm::vec4{1.0F, 1.0F, 0.0625F, heldLight}),
            heldTransform,
        };
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, heldItemPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, itemPipelineLayout,
                                0, 1, &descriptorSet, 0, nullptr);
        vkCmdPushConstants(commandBuffer, itemPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(push), &push);
        constexpr std::uint32_t generatedItemVertexCount = 12U + 16U * 16U * 4U * 6U;
        vkCmdDraw(commandBuffer,
                  !emptyHand && !cubeModel ? generatedItemVertexCount : 36U,
                  1, 0, 0);
    }

    [[nodiscard]] std::size_t recordCommandBuffer(FrameContext& frame,
                                                  std::uint32_t imageIndex) {
        auto beginInfo =
            vkStructure<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
        checkVk(vkBeginCommandBuffer(frame.commandBuffer, &beginInfo), "vkBeginCommandBuffer");
        for (const auto& copy : frame.uploadCopies) {
            VkBufferCopy region{};
            region.size = copy.size;
            vkCmdCopyBuffer(frame.commandBuffer, copy.source, copy.destination, 1, &region);
        }
        if (!frame.uploadCopies.empty()) {
            VkMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
            vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &barrier, 0, nullptr, 0,
                                 nullptr);
        }
        if (occlusionQueryPool != VK_NULL_HANDLE) {
            // Clear this frame's slot range before it is reused. Results for
            // the previous submission were already read back this drawFrame.
            vkCmdResetQueryPool(frame.commandBuffer, occlusionQueryPool,
                                static_cast<std::uint32_t>(currentFrame *
                                                           kOcclusionQueriesPerFrame),
                                static_cast<std::uint32_t>(kOcclusionQueriesPerFrame));
        }
        std::array<VkClearValue, 2> clears{};
        clears[0].color = {{0.055F, 0.080F, 0.110F, 1.0F}};
        clears[1].depthStencil = {1.0F, 0};
        auto passInfo =
            vkStructure<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
        passInfo.renderPass = renderPass;
        passInfo.framebuffer = framebuffers[imageIndex];
        passInfo.renderArea.extent = swapchainExtent;
        passInfo.clearValueCount = static_cast<std::uint32_t>(clears.size());
        passInfo.pClearValues = clears.data();
        // The sun-space shadow pre-pass writes an offscreen depth map that the
        // main pass (and the debug overlay) samples; it must run after the mesh
        // uploads above and before the main render pass.
        recordShadowPass(frame);
        vkCmdBeginRenderPass(frame.commandBuffer, &passInfo, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport viewport{};
        viewport.width = static_cast<float>(swapchainExtent.width);
        viewport.height = static_cast<float>(swapchainExtent.height);
        viewport.maxDepth = 1.0F;
        vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
        VkRect2D scissor{{0, 0}, swapchainExtent};
        vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, skyPipeline);
        vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout, 0, 1, &frame.descriptorSet, 0, nullptr);
        vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);

        const float aspect =
            static_cast<float>(swapchainExtent.width) / static_cast<float>(swapchainExtent.height);
        // Cull against the eye/direction the scene is actually rendered from, not
        // the first-person camera. In third person the render eye is boomed back
        // (and the front view looks the opposite way), so using camera.viewMatrix()
        // here culled most of the on-screen terrain.
        const Frustum frustum(camera.projectionMatrix(aspect, cameraFarPlane()) *
                              viewBobbingMatrix() * renderViewMatrix());
        // Occlusion results are two frames old, so a section that swings into
        // the frustum while the view is moving fast is still marked Occluded
        // from the previous eye and would pop out as a hole for those frames.
        // Measure motion on the render eye — the same eye the frustum is built
        // from; in third person the camera object sits elsewhere — and, when
        // the view moves quickly, draw every in-frustum section this frame
        // while still re-querying, so geometry never pops out during a pan.
        const RenderEye renderEye = renderEyeState();
        // Occlusion query results are two frames old, so stale "Occluded"
        // states are only trustworthy while the eye stays still. Accumulate
        // rotation/translation since the last validation point and drop the
        // whole map (every in-frustum section draws and re-queries) once the
        // eye has moved enough — a smooth fast pan that never trips the per-
        // frame fast-motion check still invalidates within a few degrees.
        if (!occlusionValidityInitialized) {
            occlusionValidityInitialized = true;
            occlusionRotationAccumulatorDegrees = 0.0F;
            occlusionTranslationAccumulator = 0.0F;
        }
        if (hasLastRenderEye) {
            occlusionRotationAccumulatorDegrees += glm::degrees(std::acos(std::clamp(
                glm::dot(renderEye.forward, lastRenderEye.forward), -1.0F, 1.0F)));
            occlusionTranslationAccumulator +=
                glm::length(renderEye.position - lastRenderEye.position);
        }
        constexpr float kOcclusionRotationInvalidateDegrees = 5.0F;
        constexpr float kOcclusionTranslationInvalidateBlocks = 3.0F;
        if (occlusionRotationAccumulatorDegrees > kOcclusionRotationInvalidateDegrees ||
            occlusionTranslationAccumulator > kOcclusionTranslationInvalidateBlocks) {
            occlusionStates.clear();
            occlusionMissCount.clear();
            occlusionRotationAccumulatorDegrees = 0.0F;
            occlusionTranslationAccumulator = 0.0F;
        }
        const bool cameraMovingFast =
            hasLastRenderEye &&
            (glm::length(renderEye.position - lastRenderEye.position) > 0.6F ||
             (1.0F - glm::dot(renderEye.forward, lastRenderEye.forward)) > 0.001F);
        lastRenderEye = renderEye;
        hasLastRenderEye = true;
        // The occlusion pass processes sections front to back: closer terrain is
        // drawn first (writing depth), then a farther section's AABB is tested
        // against the accumulated depth before its own mesh is drawn. Buried
        // caves therefore stop being shaded behind the surface above them. The
        // query and opaque pipelines alternate section by section, so both the
        // pipeline and its descriptor set are bound before every draw — a
        // bind-once latch would leave the previous pipeline's set active and
        // trigger VUID-vkCmdDrawIndexed-None-08600.
        struct FrustumEntry final {
            const GpuMesh* mesh;
            world::SectionPosition position;
            float distanceSquared;
        };
        const glm::vec3 cameraPosition = camera.position();
        std::vector<FrustumEntry> frustumEntries;
        frustumEntries.reserve(gpuMeshes.size());
        for (const auto& [position, mesh] : gpuMeshes) {
            if (!frustum.intersects(mesh.bounds)) {
                continue;
            }
            const glm::vec3 center = (mesh.bounds.minimum + mesh.bounds.maximum) * 0.5F;
            const glm::vec3 delta = center - cameraPosition;
            frustumEntries.push_back({&mesh, position, glm::dot(delta, delta)});
        }
        std::ranges::sort(frustumEntries, [](const FrustumEntry& first,
                                             const FrustumEntry& second) {
            return first.distanceSquared < second.distanceSquared;
        });

        // TEMP DIAGNOSTIC: report the scene's mesh/frustum state once.
        static bool reported = false;
        if (!reported && testScene.has_value() && testScene->occlusionScene) {
            reported = true;
            const glm::vec3 camPos = camera.position();
            const glm::vec3 camDir = camera.direction();
            std::cerr << "[scene] gpuMeshes=" << gpuMeshes.size()
                      << " frustumEntries=" << frustumEntries.size() << " cam=" << camPos.x << ','
                      << camPos.y << ',' << camPos.z << " dir=" << camDir.x << ',' << camDir.y
                      << ',' << camDir.z << '\n';
            for (const auto& entry : frustumEntries) {
                std::cerr << "  entry(" << entry.position.chunkX << ','
                          << entry.position.sectionY << ',' << entry.position.chunkZ
                          << ") opaque=" << entry.mesh->opaque.indexCount << '\n';
            }
        }

        frame.occlusionQueryCount = 0U;
        frame.occlusionQuerySections.clear();
        const std::uint32_t queryFirstSlot =
            static_cast<std::uint32_t>(currentFrame * kOcclusionQueriesPerFrame);
        std::size_t visibleCount = 0;
        std::vector<const GpuMesh*> visibleCutoutMeshes;
        std::vector<const GpuMesh*> visibleTranslucentMeshes;
        for (const auto& entry : frustumEntries) {
            const auto& mesh = *entry.mesh;
            const auto stateIt = occlusionStates.find(entry.position);
            const OcclusionState state =
                stateIt == occlusionStates.end() ? OcclusionState::Unknown : stateIt->second;
            // Capture the budget before this section records its query, so the
            // section that fills the last slot and the ones past the budget are
            // gated consistently: anything beyond the budget draws unconditionally
            // (its stale Occluded state would otherwise hide it forever).
            const bool withinQueryBudget = frame.occlusionQueryCount < kOcclusionQueriesPerFrame;

            // Every in-frustum section is re-queried, so a hidden cave becomes
            // visible the instant it is looked at and a visible one is dropped
            // the moment it hides. The result gates the draw two frames later.
            if (!occlusionDisabled && occlusionQueryPool != VK_NULL_HANDLE &&
                withinQueryBudget) {
                vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  occlusionQueryPipeline);
                vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        occlusionQueryLayout, 0, 1, &frame.descriptorSet, 0,
                                        nullptr);
                const VkDeviceSize boxOffset = 0;
                vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1,
                                       &occlusionBoxVertexBuffer.buffer, &boxOffset);
                vkCmdBindIndexBuffer(frame.commandBuffer, occlusionBoxIndexBuffer.buffer, 0,
                                     VK_INDEX_TYPE_UINT32);
                const OcclusionQueryPushConstants push{
                    glm::vec4{mesh.bounds.minimum, 1.0F},
                    glm::vec4{mesh.bounds.maximum, 1.0F},
                };
                vkCmdPushConstants(frame.commandBuffer, occlusionQueryLayout,
                                   VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
                const std::uint32_t slot = queryFirstSlot + frame.occlusionQueryCount;
                vkCmdBeginQuery(frame.commandBuffer, occlusionQueryPool, slot,
                                VK_QUERY_CONTROL_PRECISE_BIT);
                vkCmdDrawIndexed(frame.commandBuffer, 36, 1, 0, 0, 0);
                vkCmdEndQuery(frame.commandBuffer, occlusionQueryPool, slot);
                frame.occlusionQuerySections.push_back(entry.position);
                ++frame.occlusionQueryCount;
            }

            // Unknown and Visible sections draw now so geometry never pops in;
            // Occluded sections wait for a passing query to prove them visible —
            // unless the query budget ran out (their stale state would hide them
            // permanently) or the view is moving fast (their state is from the
            // old eye). In both cases they still draw while being re-queried.
            if (!occlusionDisabled && withinQueryBudget && !cameraMovingFast &&
                state == OcclusionState::Occluded) {
                continue;
            }
            if (mesh.opaque.indexCount > 0U) {
                vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  graphicsPipeline);
                vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        pipelineLayout, 0, 1, &frame.descriptorSet, 0, nullptr);
                vkCmdPushConstants(frame.commandBuffer, pipelineLayout,
                                   VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::vec4),
                                   &mesh.sectionOrigin);
                vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &mesh.vertexBuffer.buffer,
                                       &mesh.opaque.vertexOffset);
                vkCmdBindIndexBuffer(frame.commandBuffer, mesh.indexBuffer.buffer,
                                     mesh.opaque.indexOffset, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(frame.commandBuffer, mesh.opaque.indexCount, 1, 0, 0, 0);
                ++visibleCount;
            }
            if (mesh.translucent.indexCount > 0U) {
                visibleTranslucentMeshes.push_back(&mesh);
            }
            if (mesh.cutout.indexCount > 0U) {
                visibleCutoutMeshes.push_back(&mesh);
            }
        }
        if (!visibleCutoutMeshes.empty()) {
            vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, cutoutPipeline);
            // The occlusion pass can leave the query pipeline's descriptor set
            // bound, so the cutout pipeline re-binds its own before drawing.
            vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    pipelineLayout, 0, 1, &frame.descriptorSet, 0, nullptr);
            for (const auto* mesh : visibleCutoutMeshes) {
                vkCmdPushConstants(frame.commandBuffer, pipelineLayout,
                                   VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::vec4),
                                   &mesh->sectionOrigin);
                vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &mesh->vertexBuffer.buffer,
                                       &mesh->cutout.vertexOffset);
                vkCmdBindIndexBuffer(frame.commandBuffer, mesh->indexBuffer.buffer,
                                     mesh->cutout.indexOffset, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(frame.commandBuffer, mesh->cutout.indexCount, 1, 0, 0, 0);
            }
        }
        // Entities belong to the entity stage, between the cutout terrain and the
        // translucent terrain, exactly as vanilla orders them. Drawing them after
        // the translucent pass made every creature, item and gameSession.player() float in
        // front of water and glass: the translucent pipeline keeps depth writes
        // off, so nothing it drew could reject a later draw. From here the depth
        // buffer resolves it both ways — water in front of a creature blends over
        // it, water behind it fails the depth test.
        drawChestEntities(frame.commandBuffer, frame.descriptorSet);
        drawItemEntities(frame.commandBuffer, frame.descriptorSet);
        drawWorldEntities(frame.commandBuffer, frame.descriptorSet);
        drawWorldPlayer(frame.commandBuffer, frame.descriptorSet);
        std::ranges::sort(visibleTranslucentMeshes, [&cameraPosition](const GpuMesh* first,
                                                                      const GpuMesh* second) {
            const glm::vec3 firstCenter = (first->bounds.minimum + first->bounds.maximum) * 0.5F;
            const glm::vec3 secondCenter = (second->bounds.minimum + second->bounds.maximum) * 0.5F;
            return glm::dot(firstCenter - cameraPosition, firstCenter - cameraPosition) >
                   glm::dot(secondCenter - cameraPosition, secondCenter - cameraPosition);
        });
        if (!visibleTranslucentMeshes.empty()) {
            vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              translucentPipeline);
            vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    pipelineLayout, 0, 1, &frame.descriptorSet, 0, nullptr);
            for (const auto* mesh : visibleTranslucentMeshes) {
                vkCmdPushConstants(frame.commandBuffer, pipelineLayout,
                                   VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::vec4),
                                   &mesh->sectionOrigin);
                vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &mesh->vertexBuffer.buffer,
                                       &mesh->translucent.vertexOffset);
                vkCmdBindIndexBuffer(frame.commandBuffer, mesh->indexBuffer.buffer,
                                     mesh->translucent.indexOffset, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(frame.commandBuffer, mesh->translucent.indexCount, 1, 0, 0, 0);
            }
        }
        // Particles stay behind the translucent terrain pass, which is where
        // vanilla draws them too.
        const std::size_t particleRecordCount =
            drawParticles(frame.commandBuffer, frame.descriptorSet);
        drawRain(frame.commandBuffer, frame.descriptorSet, particleRecordCount);
        drawMiningProgress(frame.commandBuffer, frame.descriptorSet);
        if (!inventoryOpen && !paused && !chatOpen && targetedBlock.has_value()) {
            const world::Block targeted = interactionWorld.block(
                targetedBlock->block.x, targetedBlock->block.y, targetedBlock->block.z);
            // The outline now traces the block's actual shape, so sub-block
            // blocks (torch, plants, chest) no longer show a full-cube marker.
            // Crops and farmland read their shape from the cell's state.
            const world::BlockBounds bounds = world::blockSelectionBounds(
                interactionWorld, targetedBlock->block, targeted);
            vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              outlinePipeline);
            vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    outlinePipelineLayout, 0, 1, &frame.descriptorSet, 0, nullptr);
            const std::array<glm::vec4, 3> outlinePush{
                glm::vec4{static_cast<float>(targetedBlock->block.x),
                          static_cast<float>(targetedBlock->block.y),
                          static_cast<float>(targetedBlock->block.z), 0.0F},
                glm::vec4{bounds.minimum, 0.0F},
                glm::vec4{bounds.maximum, 0.0F},
            };
            vkCmdPushConstants(frame.commandBuffer, outlinePipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(outlinePush),
                               outlinePush.data());
            vkCmdDraw(frame.commandBuffer, 24, 1, 0, 0);
        }
        // The in-game HUD layer (held item, underwater, vignette, hotbar, status
        // bars, crosshair, held-item name) and any open screens on top of it are
        // drawn by drawHud, in the 1.16.1 layer order.
        drawHud(frame.commandBuffer, frame.descriptorSet);
        drawShadowDebugOverlay(frame.commandBuffer);
        vkCmdEndRenderPass(frame.commandBuffer);
        checkVk(vkEndCommandBuffer(frame.commandBuffer), "vkEndCommandBuffer");
        return visibleCount;
    }

    // Apply the occlusion query results recorded two submissions ago. The
    // frame's fence was just waited, so its queries are complete; reading them
    // here, before this frame's slot range is reset and reused, keeps the
    // per-section draw gate exactly two frames old.
    void readBackOcclusionQueries() {
        if (occlusionQueryPool == VK_NULL_HANDLE) {
            return;
        }
        auto& frame = frames[currentFrame];
        const std::uint32_t count = frame.occlusionQueryCount;
        if (count == 0U) {
            return;
        }
        const std::uint32_t firstSlot =
            static_cast<std::uint32_t>(currentFrame * kOcclusionQueriesPerFrame);
        frame.occlusionQueryResults.resize(count);
        // The frame's fence was just waited, so every query here is already
        // complete; WAIT_BIT therefore never blocks, but it makes the host read
        // explicitly synchronize with the Metal visibility result buffer instead
        // of relying on MoltenVK's deferred accumulation having finished.
        const VkResult result = vkGetQueryPoolResults(
            device, occlusionQueryPool, firstSlot, count, count * sizeof(std::uint64_t),
            frame.occlusionQueryResults.data(), sizeof(std::uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        if (result != VK_SUCCESS) {
            return;
        }
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto& position = frame.occlusionQuerySections[index];
            if (frame.occlusionQueryResults[index] > 0U) {
                occlusionStates[position] = OcclusionState::Visible;
                occlusionMissCount.erase(position);
            } else {
                std::uint32_t& misses = occlusionMissCount[position];
                ++misses;
                if (misses >= kOcclusionHysteresisFrames) {
                    occlusionStates[position] = OcclusionState::Occluded;
                }
            }
        }
        // The controlled occlusion scene dumps its query results so the box test
        // can be verified against the known geometry.
        if (testScene.has_value() && testScene->occlusionScene) {
            for (std::uint32_t index = 0; index < count; ++index) {
                const auto& position = frame.occlusionQuerySections[index];
                std::cerr << "[query] section(" << position.chunkX << ',' << position.sectionY
                          << ',' << position.chunkZ << ") count="
                          << frame.occlusionQueryResults[index] << '\n';
            }
        }
    }

    void drawFrame() {
        auto& frame = frames[currentFrame];
        checkVk(vkWaitForFences(device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX),
                "vkWaitForFences");
        // Tell VMA which frame this is so it can reuse allocations released a
        // frame-index window ago instead of growing new blocks every burst.
        vmaSetCurrentFrameIndex(allocator, frameNumber_);
        releaseFrameResources(frame);
        readBackOcclusionQueries();
        std::uint32_t imageIndex = 0;
        const auto acquire = vkAcquireNextImageKHR(
            device, swapchain, UINT64_MAX, frame.imageAvailable, VK_NULL_HANDLE, &imageIndex);
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
            recreateSwapchain();
            return;
        }
        if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
            checkVk(acquire, "vkAcquireNextImageKHR");
        }
        if (imagesInFlight[imageIndex] != VK_NULL_HANDLE) {
            checkVk(vkWaitForFences(device, 1, &imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX),
                    "vkWaitForFences(swapchain image)");
        }
        imagesInFlight[imageIndex] = frame.inFlight;
        prepareStreamingUpdates(frame);
        if (worldSessionActive && !worldReady && completedStreamBatchCount > 0U &&
            spawnPositionInitialized && pendingSectionUpdates.empty()) {
            worldReady = true;
            paused = false;
            menuSystem.pageStack.reset(ui::PageId::Game);
            gameSession.physicsPreviousPosition() = gameSession.player().position();
            gameSession.physicsCurrentPosition() = gameSession.player().position();
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            std::cout << "Terrain loading complete\n";
            // The spawn area is in; widen to the full render distance. The
            // per-frame streaming loop keeps requesting, so the rest of the view
            // distance fills in progressively during play, the way vanilla
            // streams chunks past its initial entry area.
            chunkStreamer.setRadii(viewDistanceChunks, viewDistanceChunks);
        }
        updateShadowMatrix();
        updateUniform(frame);
        checkVk(vkResetFences(device, 1, &frame.inFlight), "vkResetFences");
        checkVk(vkResetCommandBuffer(frame.commandBuffer, 0), "vkResetCommandBuffer");
        const std::size_t visibleCount = recordCommandBuffer(frame, imageIndex);
        const std::string movementMode = gameSession.player().flying()
                                             ? (gameSession.player().sprinting() ? "FLY SPRINT" : "FLY")
                                             : (gameSession.player().sprinting() ? "SPRINT" : "WALK");
        const std::string playerMode =
            std::string{gameplay::gameModeName(gameSession.gameMode())} + " " + movementMode;
        const gameplay::ItemStack selectedItem = activeInventory().selectedStack();
        if (visibleCount != lastVisibleMeshCount || gpuMeshes.size() != lastGpuMeshCount ||
            pendingSectionUpdates.size() != lastPendingSectionCount ||
            playerMode != lastPlayerMode || selectedItem != lastSelectedItem) {
            lastVisibleMeshCount = visibleCount;
            lastGpuMeshCount = gpuMeshes.size();
            lastPendingSectionCount = pendingSectionUpdates.size();
            lastPlayerMode = playerMode;
            lastSelectedItem = selectedItem;
            const std::string title =
                "MC Rebedrock - " + playerMode + " | visible " + std::to_string(visibleCount) +
                "/" + std::to_string(gpuMeshes.size()) + " | pending " +
                std::to_string(pendingSectionUpdates.size()) + " | CPU chunks " +
                std::to_string(loadedCpuChunkCount) + " | " + gameplay::itemName(selectedItem);
            glfwSetWindowTitle(window, title.c_str());
        }

        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        const auto presentSemaphore = presentSemaphores[imageIndex];
        auto submit = vkStructure<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &frame.imageAvailable;
        submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &frame.commandBuffer;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &presentSemaphore;
        checkVk(vkQueueSubmit(graphicsQueue, 1, &submit, frame.inFlight), "vkQueueSubmit");
        auto present = vkStructure<VkPresentInfoKHR>(VK_STRUCTURE_TYPE_PRESENT_INFO_KHR);
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &presentSemaphore;
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &imageIndex;
        const auto result = vkQueuePresentKHR(presentQueue, &present);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
            framebufferResized) {
            framebufferResized = false;
            recreateSwapchain();
        } else {
            checkVk(result, "vkQueuePresentKHR");
        }
        currentFrame = (currentFrame + 1U) % kFramesInFlight;
        ++frameNumber_;
    }

    std::filesystem::path shaderRoot;
    std::filesystem::path blockTextureRoot;
    std::filesystem::path optionsPath;
    persistence::SaveRepository saveRepository;
    config::GameOptions options;
    std::optional<TestSceneOptions> testScene;
    audio::AudioSystem audioSystem;
    world::ChunkStreamer& chunkStreamer;
    world::World interactionWorld;
    // Mirrors the worker's incremental lighting on the render thread so instant
    // edit previews are built with correct light instead of stale stored values.
    world::WorldLightEngine interactionLightEngine;
    std::unordered_map<world::SectionPosition, GpuMesh, world::SectionPositionHash> gpuMeshes;
    StreamBufferPool deviceBufferPool_;
    StreamBufferPool stagingBufferPool_;
    // Per-section occlusion state, fed by the occlusion query results so an
    // opaque mesh is skipped once its AABB stops passing the depth test.
    std::unordered_map<world::SectionPosition, OcclusionState, world::SectionPositionHash>
        occlusionStates;
    // Consecutive zero-count queries per section, so the Visible->Occluded edge
    // needs a couple of failed queries instead of a single borderline one.
    std::unordered_map<world::SectionPosition, std::uint32_t, world::SectionPositionHash>
        occlusionMissCount;
    VkQueryPool occlusionQueryPool = VK_NULL_HANDLE;
    VkPipeline occlusionQueryPipeline = VK_NULL_HANDLE;
    VkPipelineLayout occlusionQueryLayout = VK_NULL_HANDLE;
    AllocatedBuffer occlusionBoxVertexBuffer;
    AllocatedBuffer occlusionBoxIndexBuffer;
    std::deque<world::SectionPosition> pendingSectionOrder;
    std::unordered_map<world::SectionPosition, world::SectionMeshUpdate, world::SectionPositionHash>
        pendingSectionUpdates;
    std::unordered_map<world::SectionPosition, std::uint64_t, world::SectionPositionHash>
        latestSectionRevisions;
    // Smooth-lighting quality the meshes on the GPU were baked with, versus the
    // one the worker is re-baking towards. uniform.lightingSettings.z follows
    // currentMeshQuality so the shader never applies the High AO curve to a
    // Standard mesh; the flip is gated on qualityRemeshPending draining.
    world::SmoothLightingQuality currentMeshQuality = world::SmoothLightingQuality::Standard;
    world::SmoothLightingQuality targetMeshQuality = world::SmoothLightingQuality::Standard;
    std::unordered_set<world::SectionPosition, world::SectionPositionHash> qualityRemeshPending;
    gameplay::GameSession gameSession;
    ui::MenuSystem menuSystem;
    // The HUD-facing gameplay state, captured once per frame so the draw
    // passes read a consistent snapshot instead of live gameplay objects.
    mutable ui::UiFrameData uiFrameData_;
    PerspectiveCamera camera;
    // The unmodified FOV the camera was built with. Every frame multiplies it by
    // the gameSession.player()'s movement FOV multiplier, so the base has to be kept aside.
    float baseFieldOfViewDegrees = 65.0F;
    // Health, hunger and environmental damage. Only ticked in survival.
    gameplay::command::CommandDispatcher commandDispatcher;
    // The world's game rules, owned here and mirrored into the systems that
    // consume them; persisted as a sparse self-describing block in world.dat.
    // Minimal free-roaming creatures and the pig skeleton they render with.
    // The first consumer of the box-UV entity pipeline.
    // The species the box-UV entity pipeline has bound: one entry per loaded
    // creature (pig, zombie), each carrying its geometry + animations and its
    // layer in the entity texture array. Filled by createEntityTextureArray.
    std::vector<gameplay::entities::SpeciesRenderModel> speciesModels;
    animation::ModelAnimationSystem heldItemAnimation;
    // The gameSession.inventory() preview gameSession.player() and the in-world third-person gameSession.player() use
    // separate animator instances driven by different inputs (cursor vs. the
    // gameSession.player()'s own look/movement).
    animation::PlayerModelAnimator playerModelAnimator;
    animation::PlayerModelAnimator worldPlayerAnimator;
    // Data-driven motion for the chest lid (Bezier ease-out hinge) and the
    // dropped-item float/spin, evaluated through the animation library.
    animation::HingeAnimation chestLidAnimation;
    animation::DisplayEntityAnimation itemDisplayAnimation;
    CameraPerspective cameraPerspective = CameraPerspective::FirstPerson;
    // Third-person body yaw, which lags the look direction so the head leads the
    // turn and only drags the body once it hits its rotation limit.
    float worldBodyYaw = 0.0F;
    bool worldBodyYawInitialized = false;
    ParticleSystem particleSystem;
    bool glfwInitialized = false;
    bool framebufferResized = false;
    bool validationEnabled = false;
    bool firstMouseSample = true;
    bool breakBlockRequested = false;
    bool breakButtonHeld = false;
    bool placeBlockRequested = false;
    bool useButtonHeld = false;
    bool previousJumpKeyDown = false;
    bool inventoryOpen = false;
    bool spawnPositionInitialized = false;
    bool worldReady = false;
    // DR repro hook: MC_REBEDROCK_LOAD_SAVE auto-loads the first real save.
    bool loadSaveStarted = false;
    bool creativeScrollbarDragging = false;
    // SlotActionType.QUICK_CRAFT drag state: the button held and the storage of
    // every slot the cursor swept over. A drag starts when a press leaves a
    // stack on the cursor, collects slots while the button is held, and
    // distributes on release.
    bool inventoryDragActive = false;
    gameplay::InventoryMouseButton inventoryDragButton = gameplay::InventoryMouseButton::Left;
    std::vector<gameplay::ItemStack*> inventoryDragSlots;
    // Vanilla sets this on a press that already acted (a pickup or quick-move
    // from an empty cursor) so the release does not place or distribute again.
    bool cancelNextInventoryRelease = false;
    // SlotActionType.PICKUP_ALL double-click state: the last slot pressed (by
    // storage identity), when, and whether the press was the second within the
    // vanilla 250 ms window. On release the same-type stacks gather into the
    // cursor.
    gameplay::ItemStack* lastClickedSlot = nullptr;
    double lastClickTime = 0.0;
    bool isDoubleClicking = false;
    bool paused = true;
    bool debugOverlayOpen = false;
    bool dropRequested = false;
    bool dropWholeStack = false;
    bool chatOpen = false;
    // 1.16.1's InGameHud vignette: starts at full darkness and lerps toward
    // 1 - brightnessAtEyes at 1% per tick (see updateVignetteDarkness).
    float vignetteDarkness_ = 1.0F;
    unsigned int suppressedOpeningChatCodepoint = 0U;
    int viewDistanceChunks = 4;
    // Vanilla's Simulation Distance: how many chunks around the player stay
    // simulated (entities beyond it are frozen but rendered). In chunks, so it
    // reads like the view-distance slider next to it.
    int simulationDistanceChunks = 4;
    ContainerScreen containerScreen = ContainerScreen::PlayerInventory;
    MenuButton pressedMenuButton = MenuButton::None;
    std::optional<world::VoxelRaycastHit> targetedBlock;
    std::optional<gameplay::ChestPosition> activeChest;
    // The furnace block the gameSession.player() last opened; while the shared furnace state
    // is burning, that block swaps to the lit state (texture + light).
    std::optional<glm::ivec3> activeFurnacePosition;
    std::optional<glm::ivec3> miningTarget;
    std::string chatInputText;
    ui::ChatHistory chatHistory;
    // Tab completion state for the open chat line: the candidates for the token
    // under the cursor, rebuilt when the input changes, and the currently
    // highlighted row (Tab cycles through the stored list without recomputing).
    std::vector<gameplay::command::Suggestion> chatSuggestions_;
    std::size_t chatSuggestionIndex_ = 0;
    // GuiIngame's held-item name highlight: which hotbar slot and stack the name
    // on screen belongs to, and when it started showing. The name fades out
    // after two seconds and is cleared entirely while the hand is empty. Written
    // from the (const) HUD pass, so mutable like the other UI animation state.
    mutable std::size_t selectedNameSlot_ = static_cast<std::size_t>(-1);
    mutable gameplay::ItemStack selectedNameStack_;
    mutable double selectedNameShownAt_ = -1.0;
    double uiTimeSeconds = 0.0;
    double miningStartedAt = 0.0;
    double lastMiningSoundAt = -1.0;
    // Level#random's stand-in for loot rolls. Re-seeded from the world seed on
    // load so a given world replays its drops rather than the same fixed run.
    // MultiPlayerGameMode#destroyDelay (5 ticks) and Minecraft#rightClickDelay
    // (4 ticks): the earliest world times a held button may act again.
    double nextCreativeBreakSeconds = 0.0;
    double nextUseSeconds = 0.0;
    // Eating state: right-click on food starts the vanilla 32-tick (1.6 s) eat,
    // during which the held item is raised to the mouth; the meal lands when the
    // timer expires. Release the button or swap items to cancel.
    double lastMouseX = 0.0;
    double lastMouseY = 0.0;
    float renderInterpolationAlpha = 0.0F;
    float fpsSampleSeconds = 0.0F;
    std::size_t fpsSampleFrames = 0U;
    int displayedFps = 0;
    GLFWwindow* window = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    bool samplerAnisotropySupported = false;
    float maximumSamplerAnisotropy = 1.0F;
    VkSampleCountFlagBits maximumMsaaSamples = VK_SAMPLE_COUNT_1_BIT;
    VkDevice device = VK_NULL_HANDLE;
    VmaAllocator allocator = VK_NULL_HANDLE;
    VulkanResources resources_;
    QueueFamilyIndices queueFamilies;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue presentQueue = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout sceneDescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool sceneDescriptorPool = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kFramesInFlight> sceneDescriptorSets{};
    GpuSceneBuffer gpuSceneBuffer;
    VkPipeline particlePipeline = VK_NULL_HANDLE;
    VkPipelineLayout particlePipelineLayout = VK_NULL_HANDLE;
    bool legacyParticles = std::getenv("MC_REBEDROCK_LEGACY_PARTICLES") != nullptr;
    OffscreenTarget shadowTarget;
    VkPipelineLayout shadowPipelineLayout = VK_NULL_HANDLE;
    VkPipeline shadowPipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout shadowDebugSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool shadowDebugPool = VK_NULL_HANDLE;
    VkDescriptorSet shadowDebugSet = VK_NULL_HANDLE;
    VkSampler shadowDebugSampler = VK_NULL_HANDLE;
    VkPipelineLayout shadowDebugPipelineLayout = VK_NULL_HANDLE;
    VkPipeline shadowDebugPipeline = VK_NULL_HANDLE;
    glm::mat4 shadowLightViewProj{1.0F};
    bool shadowDisabled = std::getenv("MC_REBEDROCK_SHADOW_DISABLE") != nullptr;
    bool shadowDebugOverlay = std::getenv("MC_REBEDROCK_SHADOW_DEBUG") != nullptr;
    render::RainSystem rainSystem;
    RainMode rainMode_ = RainMode::Async;
    std::size_t rainCountOverride_ = 0U;
    float rainTime_ = 0.0F;
    // The wind heading and its occasional shift: `rainWindAngle_` eases toward
    // `windTargetAngle_`, and `windShiftTimer_` counts down to the next veer.
    float rainWindAngle_ = 0.0F;
    float windTargetAngle_ = 0.0F;
    float windShiftTimer_ = 0.0F;
    // The weather-sound scheduler, ported from WorldRenderer#tickRainSplashing's
    // sound half: `weatherSoundCadence_` is vanilla's field_20793, the gate
    // counter that makes the rain clip fire every frame or two, and the LCG
    // feeds the column and gate rolls without touching the audio system's own
    // random state.
    int weatherSoundCadence_ = 0;
    std::uint32_t weatherSoundRng_ = 0x5EED11U;
    VkPipeline rainSheetPipeline = VK_NULL_HANDLE;
    VkPipelineLayout rainSheetPipelineLayout = VK_NULL_HANDLE;
    AllocatedImage textureImage;
    VkImageView textureView = VK_NULL_HANDLE;
    AllocatedImage fontTextureImage;
    VkImageView fontTextureView = VK_NULL_HANDLE;
    AllocatedImage guiTextureImage;
    VkImageView guiTextureView = VK_NULL_HANDLE;
    AllocatedImage entityTextureImage;
    VkImageView entityTextureView = VK_NULL_HANDLE;
    std::uint32_t entityTextureWidth = 0U;
    std::uint32_t entityTextureHeight = 0U;
    // The six 1.16.1 title-screen panorama faces, one array layer each, sampled
    // with a dedicated linear sampler because they are photographs, not pixel
    // art. Kept out of the 256px GUI array so they stay at native resolution.
    AllocatedImage panoramaTextureImage;
    VkImageView panoramaTextureView = VK_NULL_HANDLE;
    VkSampler panoramaSampler = VK_NULL_HANDLE;
    // The 1.16.1 biome colour lookup textures (grass + foliage), sampled by the
    // terrain fragment shader with a linear sampler so biome boundaries blend as
    // a smooth per-pixel gradient — the GPU-side equivalent of Java's per-vertex
    // BiomeColors, but robust because the colour comes from a texture fetch
    // rather than a per-vertex attribute. Generated from the world seed and the
    // vanilla colour maps when a world loads; the pixel data is regenerated per
    // seed, the images/sampler are created once.
    AllocatedImage biomeGrassImage;
    VkImageView biomeGrassView = VK_NULL_HANDLE;
    AllocatedImage biomeFoliageImage;
    VkImageView biomeFoliageView = VK_NULL_HANDLE;
    VkSampler biomeSampler = VK_NULL_HANDLE;
    // Blocks per texel of the biome colour textures (the 1:4 biome map).
    static constexpr int kBiomeTextureBlockSpan = 4;
    // Texels per side; kBiomeTextureBlockSpan * kBiomeTextureSize blocks covered.
    static constexpr int kBiomeTextureSize = 512;
    ui::BitmapFontMetrics fontMetrics;
    // ascii.png metrics plus the legacy unicode pages, and the translation
    // table the interface reads its strings from.
    ui::TextFont textFont;
    ui::Language language;
    // The language screen's entries: each language's name in its own language,
    // e.g. "English (United States)" / "简体中文 (中国)", plus the list's scroll
    // offset. Built lazily when the screen opens.
    VkSampler textureSampler = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainImageViews;
    VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchainExtent{};
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    std::vector<DepthTarget> depthTargets;
    std::vector<ColorTarget> colorTargets;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline graphicsPipeline = VK_NULL_HANDLE;
    VkPipeline translucentPipeline = VK_NULL_HANDLE;
    VkPipeline cutoutPipeline = VK_NULL_HANDLE;
    VkPipeline skyPipeline = VK_NULL_HANDLE;
    VkPipelineLayout outlinePipelineLayout = VK_NULL_HANDLE;
    VkPipeline outlinePipeline = VK_NULL_HANDLE;
    VkPipeline crosshairPipeline = VK_NULL_HANDLE;
    VkPipelineLayout hudPipelineLayout = VK_NULL_HANDLE;
    VkPipeline hudPipeline = VK_NULL_HANDLE;
    VkPipelineLayout panoramaPipelineLayout = VK_NULL_HANDLE;
    VkPipeline panoramaPipeline = VK_NULL_HANDLE;
    VkPipeline vignettePipeline = VK_NULL_HANDLE;
    VkPipelineLayout itemPipelineLayout = VK_NULL_HANDLE;
    VkPipeline itemPipeline = VK_NULL_HANDLE;
    VkPipeline itemShadowPipeline = VK_NULL_HANDLE;
    VkPipeline heldItemPipeline = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkFence> imagesInFlight;
    std::vector<VkSemaphore> presentSemaphores;
    std::array<FrameContext, kFramesInFlight> frames{};
    std::size_t currentFrame = 0;
    std::uint32_t frameNumber_ = 0;
    // Stress-test frame cap; 0 disables it. Read from MC_REBEDROCK_STRESS_FRAMES.
    std::size_t stressFrames = 0;
    // MC_REBEDROCK_DISABLE_OCCLUSION turns off the occlusion pass, so a crash
    // that vanishes with it set can be attributed to the queries.
    bool occlusionDisabled = std::getenv("MC_REBEDROCK_DISABLE_OCCLUSION") != nullptr;
    // The render eye of the previous frame, so the occlusion pass can tell when
    // the view is moving fast enough that two-frame-old query results are stale.
    bool hasLastRenderEye = false;
    RenderEye lastRenderEye{};
    // Rotation/translation accumulated since the last occlusion validation
    // point. Two-frame-old "Occluded" results are only trustworthy while the
    // eye is near-stationary; once the accumulated motion passes a small
    // threshold the whole occlusionStates map is dropped so stale sections draw
    // and re-query, even under a smooth fast pan that never trips the per-frame
    // cameraMovingFast check.
    bool occlusionValidityInitialized = false;
    float occlusionRotationAccumulatorDegrees = 0.0F;
    float occlusionTranslationAccumulator = 0.0F;
    // The stream request centre leads toward the view direction as well as the
    // movement direction, so turning reveals area is already generating. A
    // dedicated forward (not the render eye) keeps the look-ahead independent of
    // the first/third-person eye; the spin guard drops the lead while the view
    // swings so the loaded disk does not thrash.
    bool hasLastStreamingForward = false;
    glm::vec3 lastStreamingForward{0.0F, 0.0F, 1.0F};
    std::size_t completedStreamBatchCount = 0;
    std::size_t completedBlockEditCount = 0;
    std::size_t loadedCpuChunkCount = 0;
    std::size_t uploadedSectionsThisFrame = 0;
    std::size_t peakPendingSectionCount = 0;
    std::size_t lastSessionPeakPendingSectionCount = 0;
    VkDeviceSize uploadedBytesThisFrame = 0;
    VkDeviceSize totalUploadedBytes = 0;
    // Streaming upload budget, adapted each frame from the smoothed frame time:
    // idle (GPU headroom) raises it, stress (frame time climbing) lowers it. See
    // render/StreamingBudget.hpp for the hysteresis.
    float smoothedFrameSeconds_ = 0.0F;
    std::size_t streamingUploadBudget_ = mc::render::kMaxStreamingBudgetHigh;
    std::size_t lastVisibleMeshCount = std::numeric_limits<std::size_t>::max();
    std::size_t lastGpuMeshCount = std::numeric_limits<std::size_t>::max();
    std::size_t lastPendingSectionCount = std::numeric_limits<std::size_t>::max();
    std::string lastPlayerMode;
    gameplay::ItemStack lastSelectedItem{};
    std::optional<persistence::SaveGame> currentSave;
    std::unordered_map<PersistentEditPosition, std::size_t, PersistentEditPositionHash>
        savedEditIndices;
    // The save being edited, captured when Edit is pressed so the edit/delete
    // flow keeps working even if the list refreshes in between.
    bool worldSessionActive = false;
    std::uint64_t worldEpoch = 0U;
};

VulkanRenderer::VulkanRenderer(std::filesystem::path shaderRoot,
                               std::filesystem::path blockTextureRoot,
                               std::filesystem::path soundRoot, world::ChunkStreamer& chunkStreamer,
                               config::GameOptions options, std::filesystem::path optionsPath,
                               std::filesystem::path saveRoot,
                               std::optional<TestSceneOptions> testScene)
    : impl_(std::make_unique<Impl>(std::move(shaderRoot), std::move(blockTextureRoot),
                                   std::move(soundRoot), chunkStreamer, std::move(options),
                                   std::move(optionsPath), std::move(saveRoot), testScene)) {
    impl_->initialize();
}

VulkanRenderer::~VulkanRenderer() = default;

void VulkanRenderer::run() { impl_->run(); }

} // namespace mc::render
