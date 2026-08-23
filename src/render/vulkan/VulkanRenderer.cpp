#include "render/vulkan/VulkanRenderer.hpp"
#include "render/vulkan/BlockAtlasLayout.hpp"
#include "render/vulkan/GpuSceneBuffer.hpp"
#include "render/vulkan/HudRenderer.hpp"
#include "render/vulkan/HudTypes.hpp"
#include "render/vulkan/OffscreenTarget.hpp"
#include "render/vulkan/TextureManager.hpp"
#include "render/vulkan/VulkanDevice.hpp"
#include "render/vulkan/VulkanResources.hpp"
#include "render/vulkan/WorldRenderTypes.hpp"
#include "render/vulkan/WorldRenderer.hpp"

#include "core/FrameTrace.hpp"

#include "animation/AnimationAssets.hpp"
#include "animation/DisplayEntityAnimation.hpp"
#include "animation/HingeAnimation.hpp"
#include "animation/ModelAnimationSystem.hpp"
#include "animation/PlayerModelAnimator.hpp"
#include "animation/SkeletalModel.hpp"
#include "assets/ImageData.hpp"
#include "audio/AudioSystem.hpp"
#include "client/ClientMirror.hpp"
#include "gameplay/ChestSystem.hpp"
#include "gameplay/ContentRegistry.hpp"
#include "gameplay/CraftingSystem.hpp"
#include "gameplay/EntitySystem.hpp"
#include "gameplay/GameMode.hpp"
#include "gameplay/GameRules.hpp"
#include "gameplay/GameSession.hpp"
#include "gameplay/GameplayMutationSink.hpp"
#include "gameplay/SimulationDriver.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/ItemEntitySystem.hpp"
#include "gameplay/ItemPlacement.hpp"
#include "gameplay/MiningSystem.hpp"
#include "gameplay/PlayerController.hpp"
#include "gameplay/PlayerVitals.hpp"
#include "gameplay/SpawnEggItems.hpp"
#include "gameplay/WorldSimulation.hpp"
#include "gameplay/command/CommandDispatcher.hpp"
#include "gameplay/command/GameplayArguments.hpp"
#include "input/GlfwInputBackend.hpp"
#include "input/InputActionRouting.hpp"
#include "input/InputNaming.hpp"
#include "input/InputSystem.hpp"
#include "input/KeyBindingScreen.hpp"
#include "gameplay/entities/CowEntity.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "gameplay/entities/PigEntity.hpp"
#include "gameplay/entities/SpeciesRenderData.hpp"
#include "persistence/SaveRepository.hpp"
#include "render/Frustum.hpp"
#include "runtime/GameRuntime.hpp"
#include "render/ParticleSystem.hpp"
#include "render/PerspectiveCamera.hpp"
#include "render/player/PlayerRenderState.hpp"
#include "render/RainSystem.hpp"
#include "render/StreamingBudget.hpp"
#include "ui/BitmapFontMetrics.hpp"
#include "ui/ButtonControl.hpp"
#include "ui/ChatHistory.hpp"
#include "ui/HudLayout.hpp"
#include "ui/Language.hpp"
#include "ui/MenuGeometry.hpp"
#include "ui/MenuInteraction.hpp"
#include "ui/MenuSystem.hpp"
#include "ui/PageBuilder.hpp"
#include "ui/PageStack.hpp"
#include "ui/SubtitleFeed.hpp"
#include "ui/Toast.hpp"
#include "ui/Widget.hpp"
#include "ui/TextFont.hpp"
#include "ui/UiFrameData.hpp"
#include "world/BlockPlacement.hpp"
#include "world/ChunkMesher.hpp"
#include "world/ChunkStreamer.hpp"
#include "world/DayNightCycle.hpp"
#include "world/VoxelRaycast.hpp"
#include "world/WorldConstants.hpp"
#include "world/WorldLightEngine.hpp"
#include "world/WorldLock.hpp"
#include "world/gen/Biome.hpp"
#include "world/gen/LayeredBiomeSource.hpp"

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <vk_mem_alloc.h>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <atomic>
#include <array>
#include <bit>
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

// Which volume bus a creature's sounds route through, derived from its
// SpawnGroup: monsters use Hostile, bats (Ambient category) use the Ambient
// bus, and every other animal uses Neutral — the same split vanilla applies in
// MobEntity#getSoundCategory (HOSTILE vs NEUTRAL, with bats on AMBIENT).
[[nodiscard]] audio::SoundCategory
creatureSoundCategory(const gameplay::entities::EntityType& type) {
    switch (type.category()) {
    case gameplay::entities::MobCategory::Monster:
        return audio::SoundCategory::Hostile;
    case gameplay::entities::MobCategory::Ambient:
        return audio::SoundCategory::Ambient;
    case gameplay::entities::MobCategory::Creature:
    case gameplay::entities::MobCategory::WaterCreature:
    case gameplay::entities::MobCategory::Misc:
        break;
    }
    return audio::SoundCategory::Neutral;
}

[[nodiscard]] bool disableOcclusionQueries() {
    if (std::getenv("MC_REBEDROCK_DISABLE_OCCLUSION") != nullptr) {
        return true;
    }
#if defined(__APPLE__)
    // macOS 27 + MoltenVK 1.4.2 can lose the Apple GPU after seconds or minutes
    // of sustained occlusion-query traffic in either Boolean or precise mode.
    // Vulkan validation and Metal API Validation remain clean; synchronous
    // MoltenVK queue submission masks it, and disabling only the active queries
    // eliminates it. Keep the path opt-in for controlled driver regression
    // tests, never for players.
    return std::getenv("MC_REBEDROCK_FORCE_OCCLUSION") == nullptr;
#else
    return false;
#endif
}

// kFramesInFlight now lives in render/vulkan/WorldRenderTypes.hpp (shared).
// Occlusion queries gate a section's opaque draw behind the depth the closer
// terrain wrote earlier in the same frame. Each in-flight frame owns a
// separate pool; results are read back after that frame's fence. macOS disables
// active queries by default below; the split pools remain the safer layout for
// native Vulkan and explicit MoltenVK driver regression runs.
// kOcclusionQueriesPerFrame, kOcclusionQueryPoolSize, kOcclusionHysteresisFrames
// now live in render/vulkan/WorldRenderTypes.hpp (shared).

[[nodiscard]] world::SmoothLightingQuality
nextSmoothLightingQuality(world::SmoothLightingQuality quality) {
    switch (quality) {
    case world::SmoothLightingQuality::Off:
        return world::SmoothLightingQuality::Standard;
    case world::SmoothLightingQuality::Standard:
        return world::SmoothLightingQuality::High;
    case world::SmoothLightingQuality::High:
        return world::SmoothLightingQuality::Off;
    }
    return world::SmoothLightingQuality::Standard;
}
// Two-phase world entry: a world opens with a small chunk area around the gameSession.player()
// (vanilla enters with a small initial area and streams the view distance in
// during play), so a large render distance does not block the load screen on the
// whole (2·radius+1)² area before the gameSession.player() can move.
constexpr int kSpawnChunkRadius = 4;
// kVignetteGuiLayer, kScreenDimGuiLayer and kPanoramaFaces now live in
// render/vulkan/HudTypes.hpp (shared with HudRenderer).

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
// kMaxUploadBytesPerFrame, kMaxPrioritySectionUploadsPerFrame now in WorldRenderTypes.hpp.
// Ceiling on the render thread's queued-but-not-yet-uploaded section meshes.
// The worker generates far faster than the per-frame upload budget drains, and
// an unbounded queue lets CPU-side mesh data pile up (peaks >4500 sections were
// measured). The oldest low-priority entry is evicted beyond this.
// kMaxPendingSectionUpdates now in WorldRenderTypes.hpp.
// kStreamBufferClassSizes now lives in render/vulkan/WorldRenderTypes.hpp
// (shared): stream-mesh buffers are pooled by power-of-two size class and
// reused across section uploads instead of created/destroyed per mesh. MoltenVK
// does not hand freed MTLBuffer memory back to the OS, so reusing a buffer beats
// freeing it; the pool pins the graphics high-water mark at the working set.
// Above this resident total the pool hands surplus free buffers back to the
// driver instead of hoarding them.
// kMaxStreamBufferPoolBytes now in WorldRenderTypes.hpp.
// Vertex and index buffers share one pool (a buffer may carry both usage bits).
// kStreamBufferDeviceUsage now in WorldRenderTypes.hpp.

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

// MenuButton, ContainerScreen and kCreativeTabCount now live in
// render/vulkan/HudTypes.hpp (shared with HudRenderer).

// Camera view mode, cycled with F5 like Java Edition: first person, third person
// behind the gameSession.player(), then third person in front looking back.
// CameraPerspective now in WorldRenderTypes.hpp.

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
    // Animated-fluid atlas contract. Keeping bases/counts in the CPU layout
    // source of truth prevents GLSL literals from drifting after an atlas edit.
    alignas(16) glm::vec4 fluidAnimationLayers{0.0F};
    alignas(16) glm::vec4 fluidAnimationFrameCounts{0.0F};
    alignas(16) glm::vec4 fluidAnimationFrameTimes{1.0F};
    // x = simulation animation tick including the render interpolation fraction.
    alignas(16) glm::vec4 fluidAnimationSettings{0.0F};
    // The sun-space view-projection the shadow pre-pass writes the depth map
    // with; the terrain shader projects each fragment into it to sample the map.
    // One frame behind the pre-pass's own matrix, which is the standard shadow
    // map lag.
    alignas(16) glm::mat4 lightViewProj{1.0F};
};

// HudPush, PanoramaPush and ItemPush now live in render/vulkan/HudTypes.hpp
// (shared with HudRenderer).

// ShadowPush, RainMode, rainBaseCount, kParticleRainBaseCount, GpuMeshLayer,
// GpuMesh, BufferCopyJob, FrameContext, StreamBufferPool, OcclusionState and
// OcclusionQueryPushConstants now live in render/vulkan/WorldRenderTypes.hpp
// (shared with the WorldRenderer extraction).

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

} // namespace

struct VulkanRenderer::Impl final : public gameplay::SimulationHost {
    Impl(std::filesystem::path shaderDirectory, const assets::ResourceProvider& provider, world::ChunkStreamer& streamer,
         config::GameOptions initialOptions, std::filesystem::path initialOptionsPath,
         std::filesystem::path saveRoot, std::optional<TestSceneOptions> initialTestScene)
        : shaderRoot(std::move(shaderDirectory)),
          resourceProvider(&provider), languageLoader(provider),
          optionsPath(std::move(initialOptionsPath)),
          runtime(*this, streamer, std::move(saveRoot)),
          saveRepository(runtime.saveRepository()),
          chunkStreamer(runtime.chunkStreamer()),
          interactionWorld(runtime.world()),
          gameSession(runtime.gameSession()),
          simulationDriver(runtime.simulationDriver()),
          simulationActive(runtime.simulationActive()),
          worldLock(runtime.lock()),
          currentSave(runtime.currentSaveSlot()),
          worldEpoch(runtime.worldEpoch()),
          options(std::move(initialOptions)),
          testScene(initialTestScene), audioSystem(provider, options.masterVolume),
          camera(initialTestScene.has_value() && initialTestScene->occlusionScene
                     ? glm::vec3{8.0F, 60.0F, -8.0F}
                     : (initialTestScene.has_value() ? glm::vec3{10.7F, 66.2F, 12.1F}
                                                     : glm::vec3{24.0F, 78.0F, 24.0F}),
                 initialTestScene.has_value() && initialTestScene->occlusionScene
                     ? glm::vec3{8.0F, 47.0F, 16.0F}
                     : (initialTestScene.has_value() ? glm::vec3{8.5F, 64.5F, 8.5F}
                                                     : glm::vec3{8.0F, 61.0F, 8.0F}),
                 65.0F) {
        // Push the persisted audio settings into the engine: the per-category
        // sub-volumes (Master already went in through the constructor) and the
        // Directional Audio toggle.
        audioSystem.setCategoryVolumes(options.soundCategoryVolumes);
        audioSystem.setDirectionalAudio(options.directionalAudio);
        viewDistanceChunks = chunkStreamer.loadRadius();
        simulationDistanceChunks = std::clamp(options.simulationDistance, 2, 12);
        gameSession.setSimulationRadius(static_cast<float>(simulationDistanceChunks) *
                                        static_cast<float>(world::kChunkWidth));
        menuSystem.guiScaleSetting = options.guiScale;
        const auto resolution = std::ranges::find_if(
            ui::kDisplayResolutions, [this](const ui::DisplayResolution& candidate) {
                return candidate.width == options.windowWidth &&
                       candidate.height == options.windowHeight;
            });
        menuSystem.resolutionIndex = resolution == ui::kDisplayResolutions.end()
                                         ? 0U
                                         : static_cast<std::size_t>(std::distance(
                                               ui::kDisplayResolutions.begin(), resolution));
        // The command tree owns every command. Registering through it gives each
        // command typed arguments, argument validation and tab completion — the
        // tables behind the arguments (items, blocks, game modes, rules) feed
        // completion and validation from one source of truth, so adding an entry
        // to any table makes it appear in the command for free.
        registerGameCommands();

        // Let the held-item and gameSession.player()-preview animators pick up any authored
        // clips shipped under resources/animation. Both animators keep their
        // built-in clips if the files are absent, so this is best-effort and
        // never fatal.
        try {
            const auto animationRoot = resourceProvider->resourceRoot() / "animation";
            heldItemAnimation.load(animationRoot);
            playerModelAnimator.load(animationRoot);
            // The gameSession.inventory()/creative preview turns its whole body toward the
            // cursor (the look clip rotates the body at half the head's yaw).
            // The world gameSession.player() keeps body-follow off: its body already follows
            // the look direction through the renderer's separate body-yaw logic.
            playerModelAnimator.setBodyFollowsLook(true);
            worldPlayerAnimator.load(animationRoot);
        } catch (const std::exception& exception) {
            std::cerr << "Animation assets unavailable, using built-in clips: " << exception.what()
                      << '\n';
        }
    }

    void registerGameCommands() {
        // Every built-in species is registered up front, so entity-target
        // commands resolve from the very first world (idempotent).
        gameplay::entities::registerBuiltinEntities();
        // /tp is registered here rather than in the runtime because its rotation
        // sets the camera (the player's look is camera-owned until N2's player
        // state), which only the renderer has. The authoritative commands
        // (gamemode, time, give, gamerule, kill, spawnpoint, weather) live on the
        // runtime's dispatcher so a headless server runs them too.
        auto& commandDispatcher = runtime.commandDispatcher();
        commandDispatcher.literal("tp")
            .argument("destination", gameplay::command::kTeleportDestinationArgument)
            .executes([this](const gameplay::command::CommandContext& context) {
                return teleportWithContext(context, false);
            })
            .argument("rotation", gameplay::command::kRotationArgument)
            .executes([this](const gameplay::command::CommandContext& context) {
                return teleportWithContext(context, true);
            });
    }


    ~Impl() { shutdown(); }

    // SimulationHost: the render-side reactions the game session's tick drives.
    // submitWorldEdit and previewBlockEdit are the Impl's own methods, marked
    // override at their definitions; the remaining host methods are here.
    // PX-6 Bug3: after each sound plays, feed its accessibility caption (if any)
    // to the subtitle overlay when subtitles are enabled. audioSystem.lastSubtitle
    // is the event just played; showSoundSubtitle no-ops on an empty caption or a
    // disabled option, so sounds without a subtitle simply do not show one.
    void emitLastSubtitle() { showSoundSubtitle(audioSystem.lastSubtitle()); }

    void playBlockBreak(world::Block block, glm::vec3 position) override {
        audioSystem.playBlockBreak(block, position);
        emitLastSubtitle();
    }
    void playItemPickup(glm::vec3 position) override {
        audioSystem.playItemPickup(position);
        emitLastSubtitle();
    }
    void playEat(glm::vec3 position) override {
        audioSystem.playEat(position);
        emitLastSubtitle();
    }
    void playPlayerHurt(glm::vec3 position) override {
        audioSystem.playPlayerHurt(position);
        emitLastSubtitle();
    }
    void playPlayerFall(glm::vec3 position, bool heavy) override {
        audioSystem.playPlayerFall(position, heavy);
        emitLastSubtitle();
    }
    void playBurp(glm::vec3 position) override {
        audioSystem.playBurp(position);
        emitLastSubtitle();
    }
    void playCreatureHurt(const gameplay::entities::EntityType& type, glm::vec3 position) override {
        audioSystem.playCreatureHurt(type.soundProfile(), creatureSoundCategory(type), position);
        emitLastSubtitle();
    }
    void playCreatureDeath(const gameplay::entities::EntityType& type,
                           glm::vec3 position) override {
        audioSystem.playCreatureDeath(type.soundProfile(), creatureSoundCategory(type), position);
        emitLastSubtitle();
    }
    void playCreatureAmbient(const gameplay::entities::EntityType& type,
                             glm::vec3 position) override {
        audioSystem.playCreatureAmbient(type.soundProfile(), creatureSoundCategory(type), position);
        emitLastSubtitle();
    }
    void playCreatureStep(const gameplay::entities::EntityType& type, glm::vec3 position) override {
        audioSystem.playCreatureStep(type.soundProfile(), creatureSoundCategory(type), position);
        emitLastSubtitle();
    }
    void playFootstep(world::Block ground, glm::vec3 position, float volume) override {
        audioSystem.playFootstep(ground, position, volume);
        emitLastSubtitle();  // vanilla footsteps carry no subtitle -> no-op
    }
    void playSplash(glm::vec3 position, float volume) override {
        audioSystem.playSplash(position, volume);
        emitLastSubtitle();
    }
    void spawnBlockBreakParticles(glm::ivec3 position, world::Block block) override {
        particleSystem.spawnBlockBreak(position, block);
    }
    // The interaction side effects the gameplay controller drives: these are the
    // host's render-side half of the moved updateBlockInteraction.
    void playBlockHit(world::Block block, glm::vec3 position) override {
        audioSystem.playBlockHit(block, position);
        emitLastSubtitle();
    }
    void playBlockPlace(world::Block block, glm::vec3 position) override {
        audioSystem.playBlockPlace(block, position);
        emitLastSubtitle();
    }
    void playItemBreak(glm::vec3 position) override {
        audioSystem.playItemBreak(position);
        emitLastSubtitle();
    }
    void spawnWaterSplash(glm::vec3 position) override {
        particleSystem.spawnWaterSplash(position);
    }
    void onOpenContainer(ContainerScreen screen, std::optional<glm::ivec3> position) override {
        // The simulation has already opened and bound the authoritative menu.
        // This callback runs from the main-thread event drain and only raises
        // the presentation; it must never write gameplay state or call GLFW
        // from the simulation thread.
        static_cast<void>(screen);
        static_cast<void>(position);
        setInventoryOpenLocked(true);
    }
    void onPlayerDied() override {
        std::cout << "Player died\n";
        if (inventoryOpen) {
            // Gameplay already closed/stowed the authoritative menu in die().
            // This callback owns presentation only.
            inventoryOpen = false;
            creativeScrollbarDragging = false;
            firstMouseSample = true;
        }
        if (chatOpen) {
            chatInputText.clear();
            chatOpen = false;
        }
        simulationActive.store(false, std::memory_order_release);
        paused = true;
        menuSystem.pageStack.reset(ui::PageId::Death);
        // D0: the player stops via the zeroed MovementInput processInput sends
        // while a screen is up; here we only drop the client-side edges.
        clearPendingInputEdges();
        releaseInteractionButtons();
        dropRequested = false;
        pressedMenuButton = ui::WidgetId::None;
        firstMouseSample = true;
        unlockCursor();
    }
    void onFurnaceStateChanged() override {}
    void onEatingStarted() override {
        // The meal lives on the item-use timeline now; beginEating already called
        // playerActions().startUsing, and this frame's bridge samples it. The
        // generic.eat sound is the chew loop GameSession::tickEating drives.
    }
    void onEatingCancelled() override {}

    void initialize() {
        if (glfwInit() != GLFW_TRUE) {
            throw std::runtime_error("GLFW initialization failed");
        }
        glfwInitialized = true;
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
        glfwWindowHint(GLFW_MAXIMIZED, options.windowMaximized ? GLFW_TRUE : GLFW_FALSE);
        window = glfwCreateWindow(options.windowWidth, options.windowHeight,
                                  "MC Rebedrock - Vulkan 3D Grass Block", nullptr, nullptr);
        if (window == nullptr) {
            throw std::runtime_error("GLFW window creation failed");
        }
        glfwSetWindowUserPointer(window, this);
        glfwSetFramebufferSizeCallback(window, [](GLFWwindow* callbackWindow, int, int) {
            static_cast<Impl*>(glfwGetWindowUserPointer(callbackWindow))->framebufferResized = true;
        });
        glfwSetWindowSizeCallback(window, [](GLFWwindow* callbackWindow, int width, int height) {
            static_cast<Impl*>(glfwGetWindowUserPointer(callbackWindow))
                ->noteWindowSizeChanged(width, height);
        });
        glfwSetWindowMaximizeCallback(window, [](GLFWwindow* callbackWindow, int maximized) {
            static_cast<Impl*>(glfwGetWindowUserPointer(callbackWindow))
                ->noteWindowMaximizeChanged(maximized == GLFW_TRUE);
        });
        glfwSetKeyCallback(window, [](GLFWwindow* callbackWindow, int key, int, int action, int) {
            auto* renderer = static_cast<Impl*>(glfwGetWindowUserPointer(callbackWindow));
            const auto currentPage = renderer->menuSystem.pageStack.current();
            // PX-5 Key Binds: while a Controls row is capturing, the next key press
            // IS the rebind — consume it here (writing through the InputSystem
            // single source) instead of letting it act as a menu/gameplay key.
            // Escape cancels the capture rather than binding Escape.
            if (renderer->keyBindScreen_.capturing() && action == GLFW_PRESS) {
                if (key == GLFW_KEY_ESCAPE) {
                    renderer->keyBindScreen_.cancelCapture();
                } else {
                    const input::Key captured = input::keyFromGlfw(key);
                    if (captured != input::Key::Unknown) {
                        renderer->keyBindScreen_.applyKey(captured);
                        renderer->playUiClick();
                    }
                }
                return;
            }
            if (currentPage == ui::PageId::ConfirmDelete) {
                if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
                    renderer->menuSystem.pageStack.pop();
                }
                return;
            }
            if (currentPage == ui::PageId::CreateWorld || currentPage == ui::PageId::EditWorld) {
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
            // PX-1: F3 (debug), F5 (perspective), W (sprint double-tap edge),
            // E (inventory), Space (jump edge), 1-9 (hotbar) and Q (drop) are no
            // longer read here — the InputSystem level-samples them in
            // processInput() and dispatchGameplayInputEvents() applies the edges.
            // Only the modal Back/Escape page-stack navigation (which must run
            // while a screen is up, where processInput's gameplay poll is gated
            // off) stays in the key callback; that is PX-4 menu territory.
            if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
                // Modal gameplay screens consume Back before the page stack.
                // Inventory is an overlay on PageId::Game, so letting the page
                // switch run first incorrectly opened Pause instead of closing
                // the inventory and returning directly to play.
                if (renderer->inventoryOpen) {
                    renderer->setInventoryOpen(false);
                    return;
                }
                const auto page = renderer->menuSystem.pageStack.current();
                if (page == ui::PageId::VideoSettings) {
                    renderer->menuSystem.pageStack.pop();
                    renderer->pressedMenuButton = ui::WidgetId::None;
                    renderer->menuSystem.viewDistanceSliderDragging = false;
                    renderer->menuSystem.simulationDistanceSliderDragging = false;
                } else if (page == ui::PageId::Experimental) {
                    renderer->menuSystem.pageStack.pop();
                    renderer->pressedMenuButton = ui::WidgetId::None;
                } else if (page == ui::PageId::Language) {
                    // Escape cancels the draft row selection. Only Done starts
                    // the asynchronous language reload.
                    renderer->menuSystem.pendingLanguageCode = renderer->options.language;
                    renderer->menuSystem.languageScrollbarDragging = false;
                    renderer->menuSystem.pageStack.pop();
                    renderer->pressedMenuButton = ui::WidgetId::None;
                } else if (page == ui::PageId::Options) {
                    renderer->menuSystem.pageStack.pop();
                    renderer->menuSystem.optionsOpen = false;
                    renderer->pressedMenuButton = ui::WidgetId::None;
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
        });
        glfwSetCharCallback(window, [](GLFWwindow* callbackWindow, unsigned int codepoint) {
            auto* renderer = static_cast<Impl*>(glfwGetWindowUserPointer(callbackWindow));
            const auto currentPage = renderer->menuSystem.pageStack.current();
            if (currentPage == ui::PageId::CreateWorld || currentPage == ui::PageId::EditWorld) {
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
            if (renderer->menuSystem.pageStack.current() == ui::PageId::WorldList &&
                yOffset != 0.0) {
                renderer->scrollWorldList(yOffset > 0.0 ? -1 : 1);
            } else if (renderer->menuSystem.pageStack.current() == ui::PageId::Language &&
                       yOffset != 0.0) {
                renderer->scrollLanguageList(yOffset > 0.0 ? -1 : 1);
            } else if (renderer->menuSystem.pageStack.current() == ui::PageId::Controls &&
                       yOffset != 0.0) {
                renderer->scrollControlsList(yOffset > 0.0 ? -1 : 1);
            } else if (renderer->inventoryOpen &&
                       renderer->uiFrameData_.gameMode == gameplay::GameMode::Creative &&
                       yOffset != 0.0) {
                renderer->scrollCreative(yOffset > 0.0 ? -1 : 1);
            } else if (!renderer->inventoryOpen && !renderer->paused && !renderer->chatOpen &&
                       yOffset != 0.0) {
                {
                    const auto& playerSnap = renderer->clientMirror_.player();
                    const std::size_t current = playerSnap.selectedHotbarSlot;
                    const std::size_t count = gameplay::Inventory::kHotbarSize;
                    std::size_t target = (yOffset > 0.0)
                                             ? (current + count - 1U) % count
                                             : (current + 1U) % count;
                    gameplay::SwapSlot swap;
                    swap.index = target;
                    renderer->runtime.enqueueClientCommand(std::move(swap));
                }
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
                    // The destroy lifecycle is a command: Start on press (with
                    // the aimed target), Abort on release. The server ticks it.
                    if (action == GLFW_PRESS) {
                        renderer->destroyButtonHeld = true;
                        renderer->enqueueDestroyStart();
                    } else if (action == GLFW_RELEASE) {
                        renderer->destroyButtonHeld = false;
                        renderer->lastDestroyAimBlock.reset();
                        renderer->enqueueDestroyAbort();
                    }
                } else if (button == GLFW_MOUSE_BUTTON_RIGHT) {
                    // The use lifecycle: UseItemOn on press (with the target),
                    // UseItemStop on release so a held meal/repeat ends.
                    if (action == GLFW_PRESS) {
                        renderer->enqueueUseStart();
                    } else if (action == GLFW_RELEASE) {
                        renderer->enqueueUseStop();
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
            if (renderer->paused && (renderer->menuSystem.languageScrollbarDragging ||
                                     renderer->menuSystem.viewDistanceSliderDragging ||
                                     renderer->menuSystem.simulationDistanceSliderDragging ||
                                     renderer->menuSystem.masterVolumeSliderDragging)) {
                if (renderer->menuSystem.languageScrollbarDragging) {
                    renderer->updateLanguageScrollFromCursor();
                } else if (renderer->menuSystem.viewDistanceSliderDragging) {
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

        vulkanDevice_.initialize(window);
        // Copy the handles out so the renderer's existing references keep working;
        // vulkanDevice_ stays the sole owner and destroyer.
        instance = vulkanDevice_.instance;
        debugMessenger = vulkanDevice_.debugMessenger;
        surface = vulkanDevice_.surface;
        physicalDevice = vulkanDevice_.physicalDevice;
        samplerAnisotropySupported = vulkanDevice_.samplerAnisotropySupported;
        maximumSamplerAnisotropy = vulkanDevice_.maximumSamplerAnisotropy;
        maximumMsaaSamples = vulkanDevice_.maximumMsaaSamples;
        device = vulkanDevice_.device;
        allocator = vulkanDevice_.allocator;
        queueFamilies = vulkanDevice_.queueFamilies;
        graphicsQueue = vulkanDevice_.graphicsQueue;
        presentQueue = vulkanDevice_.presentQueue;
        commandPool = vulkanDevice_.commandPool;
        validationEnabled = vulkanDevice_.validationEnabled;
        resources_ = VulkanResources{physicalDevice, device, allocator, commandPool, graphicsQueue};
        textures_ = TextureManager{&resources_,
                                   device,
                                   allocator,
                                   resourceProvider,
                                   samplerAnisotropySupported,
                                   maximumSamplerAnisotropy};
        createDescriptorSetLayout();
        textures_.createTextureArray(options.anisotropy);
        textures_.createRainTexture();
        textures_.createBiomeTextureResources();
        loadLanguage();
        textures_.createFontTexture(fontMetrics, textFont, requiredUnicodePages(),
                                    options.forceUnicodeFont);
        // Bind the event host once, so world edits raised outside the tick loop
        // (the interaction path's mutation sink) reach the render/persistence
        // pipeline too. tick() rebinds the same host harmlessly.
        gameSession.setEventHost(*this);
        textures_.createGuiTexture();
        textures_.createPanoramaTexture();
        textures_.createPanoramaSampler();
        textures_.createEntityTextureArray(speciesModels);
        createUniformBuffers();
        createDescriptorPoolAndSets();
        createSceneDescriptorResources();
        createShadowResources();
        createOcclusionQueryResources();
        createSwapchainResources();
        createCommandBuffers();
        createSyncObjects();
        refreshSaveList();
        if (testScene.has_value())
            initializeTestScene();
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
        shadowDisabled =
            !options.sunShadows || std::getenv("MC_REBEDROCK_SHADOW_DISABLE") != nullptr;
        if (occlusionDisabled) {
#if defined(__APPLE__)
            std::cout << "GPU occlusion queries: disabled on macOS (set "
                         "MC_REBEDROCK_FORCE_OCCLUSION=1 for driver diagnostics)\n";
#else
            std::cout << "GPU occlusion queries: disabled\n";
#endif
        }
    }

    // The CPU drop target: texture mode keeps a small population only for
    // landing splashes/audio, while particle and async render the same full
    // population. MC_REBEDROCK_RAIN_COUNT overrides all.
    [[nodiscard]] std::size_t rainTargetCount() const {
        // A thunderstorm drenches the world: the rain volume scales with the
        // thunder gradient up to double the plain-rain count, and the async
        // path's capacity is what makes thousands of extra drops free to draw.
        const float thunderBoost = 1.0F + clientMirror_.world().thunderGradient;
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
            case RainMode::Texture:
            case RainMode::Particles:
            case RainMode::Async:
                base = rainBaseCount(rainMode_);
                break;
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
    [[nodiscard]] static std::optional<glm::vec3>
    weatherSurface(const world::World& world, int blockX, int blockZ, int lowestY, int highestY) {
        const int top = std::min(highestY, world::kMaxY - 1);
        const int bottom = std::max(lowestY, world::kMinY);
        for (int y = top; y >= bottom; --y) {
            if (world::hasCollision(world.block(blockX, y, blockZ))) {
                return glm::vec3{static_cast<float>(blockX) + 0.5F, static_cast<float>(y + 1),
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
        const float rainGradient = clientMirror_.world().rainGradient;
        if (rainGradient <= 0.0F) {
            return;
        }
        const glm::ivec3 cameraBlock{static_cast<int>(std::floor(camera.position().x)),
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
            if (candidate.has_value() && candidate->y <= static_cast<float>(cameraBlock.y + 11)) {
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
        const bool underRoof = weatherSurface(world, cameraBlock.x, cameraBlock.z,
                                              cameraBlock.y + 1, cameraBlock.y + 12)
                                   .has_value();
        const bool rainAbove = surface->y > static_cast<float>(cameraBlock.y + 1);
        const float volumeScale =
            rainGradient * (1.0F + 0.5F * clientMirror_.world().thunderGradient);
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
        // Vanilla renders precipitation after translucent terrain with the
        // depth mask disabled. Columns still test against roofs/terrain, but a
        // near translucent strip must not punch holes in all strips behind it.
        depthStencil.depthWriteEnable = VK_FALSE;
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
        auto layoutInfo =
            vkStructure<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
        const std::array layouts{descriptorSetLayout, sceneDescriptorSetLayout};
        layoutInfo.setLayoutCount = static_cast<std::uint32_t>(layouts.size());
        layoutInfo.pSetLayouts = layouts.data();
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
            interactionWorld.setChunk({0, 0}, chunk);
            clientCache.setChunk({0, 0}, std::move(chunk));
            world::WorldLightEngine lighting;
            const std::array positions{world::ChunkPosition{0, 0}};
            lighting.initializeChunks(interactionWorld, positions);
            lighting.initializeChunks(clientCache, positions);
            for (const int sectionY : {1, 2}) {
                world::SectionMeshUpdate update;
                update.position = {0, sectionY, 0};
                update.mesh = world::ChunkMesher::buildSection(clientCache, {0, 0}, sectionY);
                update.revision = static_cast<std::uint64_t>(sectionY);
                pendingSectionOrder.push_back(update.position);
                latestSectionRevisions.insert_or_assign(update.position, update.revision);
                pendingSectionUpdates.insert_or_assign(update.position, std::move(update));
            }
            loadedCpuChunkCount = 1U;
            // The camera follows the gameSession.player()'s eye, so pin the gameSession.player()
            // just above the platform surface (y=47), looking along +Z at the scene.
            gameSession.teleportPlayer(gameplay::kPrimaryPlayerId, {8.0F, 49.4F, -8.0F});
            camera.setPosition(snapshotCameraEye());
            worldReady = true;
            paused = true;
            menuSystem.pageStack.reset(ui::PageId::Game);
            gameSession.clocks().setTotalTicks(
                world::ClockId::Overworld,
                static_cast<std::uint64_t>(world::DayNightCycle::kNewWorldTick));
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            std::cout << "Test scene: occlusion (platform + buried cave)\n";
            return;
        }
        constexpr glm::ivec3 blockPosition{8, 64, 8};
        constexpr std::array orientations{
            world::BlockOrientation::North, world::BlockOrientation::East,
            world::BlockOrientation::South, world::BlockOrientation::West,
            world::BlockOrientation::Up,    world::BlockOrientation::Down};
        world::Chunk chunk;
        chunk.setBlock(blockPosition.x, blockPosition.y, blockPosition.z, testScene->block);
        chunk.setOrientation(
            blockPosition.x, blockPosition.y, blockPosition.z,
            orientations[static_cast<std::size_t>(testScene->stage) % orientations.size()]);
        interactionWorld.setChunk({0, 0}, chunk);
        clientCache.setChunk({0, 0}, std::move(chunk));
        world::WorldLightEngine lighting;
        const std::array positions{world::ChunkPosition{0, 0}};
        lighting.initializeChunks(interactionWorld, positions);
        lighting.initializeChunks(clientCache, positions);
        world::SectionMeshUpdate update;
        update.position = {0, world::sectionIndexFromWorldY(blockPosition.y), 0};
        update.mesh =
            world::ChunkMesher::buildSection(clientCache, {0, 0}, update.position.sectionY);
        update.revision = 1U;
        pendingSectionOrder.push_back(update.position);
        latestSectionRevisions.insert_or_assign(update.position, update.revision);
        pendingSectionUpdates.insert_or_assign(update.position, std::move(update));
        if (testScene->block == world::Block::Chest) {
            gameSession.createChestBlockEntity(
                {blockPosition.x, blockPosition.y, blockPosition.z});
        }
        loadedCpuChunkCount = 1U;
        worldReady = true;
        paused = true;
        menuSystem.pageStack.reset(ui::PageId::Game);
        gameSession.clocks().setTotalTicks(
            world::ClockId::Overworld,
            static_cast<std::uint64_t>(static_cast<double>(testScene->stage) *
                                       (world::DayNightCycle::kTicksPerDay / 10.0)));
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        std::cout << "Test scene: "
                  << world::blockDefinition(testScene->block).identifier.toString() << " stage "
                  << testScene->stage << '\n';
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
        // P3 Step 2: the simulation moves off the render thread here. Smoke and
        // stress runs must exercise this path too; MC_REBEDROCK_SYNC_TICK keeps
        // the deterministic fallback available for diagnosing failures.
        startSimulationThread();
        std::size_t renderedFrames = 0;
        std::size_t smokeGameplayFrames = 0;
        std::size_t smokeReturnFrame = 0;
        bool smokeWorldStarted = false;
        bool smokeReturnedToTitle = false;
        // A chat-command effect check that is waiting for the command's server
        // tick to land (commands process on the next tick, up to ~50 ms after
        // submit, so a same-frame check would be racy).
        bool smokeWaitActive = false;
        std::size_t smokeWaitDeadline = 0U;
        std::function<bool()> smokeWaitCondition;
        std::function<void()> smokeWaitAction;
        std::string smokeWaitLabel;
        // The apple count before the smoke test holds right-click to eat, used
        // to verify the meal actually consumed one.
        std::uint8_t smokeAppleCount = 0U;
        auto previousFrameTime = std::chrono::steady_clock::now();
        while (glfwWindowShouldClose(window) == GLFW_FALSE) {
            const auto frameCpuStart = std::chrono::steady_clock::now();
            if (diag::traceEnabled()) {
                diag::frameTrace().reset();
            }
            glfwPollEvents();
            persistWindowPlacementIfSettled();
            pollLanguageLoad();
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
            // PX-6: advance the HUD overlays on the render clock (client
            // presentation, never the world tick), so toasts slide/expire and
            // subtitles fade at real time regardless of the sim rate.
            toastQueue_.advance(deltaSeconds);
            subtitleFeed_.advance(deltaSeconds);
            fpsSampleSeconds += deltaSeconds;
            ++fpsSampleFrames;
            if (fpsSampleSeconds >= 0.5F) {
                displayedFps = static_cast<int>(
                    std::lround(static_cast<float>(fpsSampleFrames) / fpsSampleSeconds));
                fpsSampleSeconds = 0.0F;
                fpsSampleFrames = 0U;
            }
            bool playerWalking = false;
            {
                // Input preparation writes the staged PlayerInput (guarded by
                // GameSession's own input mutex) and reads published snapshots and
                // renderer-local state — nothing that needs the world lock.
                processInput();
                // C-1b-2/1b-3: pump the channel at the very top of the frame.
                // Snapshot frames refresh the client mirror (so every read below
                // sees this frame's player/world); event frames apply the tick's
                // side effects (world edits to the client cache, sounds,
                // particles, container/eating reactions) to this renderer as the
                // host. Draining every frame keeps the channel from accumulating.
                {
                    const auto pumpStart = std::chrono::steady_clock::now();
                    static_cast<void>(clientMirror_.pump(runtime.clientChannel(), *this));
                    if (diag::traceEnabled()) {
                        diag::frameTrace().drainMs += diag::msSince(pumpStart);
                    }
                }
                if (inventoryOpen) {
                    const ui::HudLayout animationLayout{
                        static_cast<float>(std::max(swapchainExtent.width, 1U)),
                        static_cast<float>(std::max(swapchainExtent.height, 1U)),
                        menuSystem.guiScaleSetting};
                    const auto cursor = currentFramebufferCursor();
                    const auto preview = animationLayout.playerPreview(
                        clientMirror_.player().gameMode == gameplay::GameMode::Creative);
                    playerModelAnimator.setCursorLook(
                        (cursor.x - preview.lookOrigin.x) / (40.0F * animationLayout.scale()),
                        (cursor.y - preview.lookOrigin.y) / (40.0F * animationLayout.scale()));
                }
                // The animator's gait inputs come from the per-tick player
                // snapshot, not live gameplay objects. The snapshot is published
                // atomically, so this copy needs no lock.
                const auto playerSnap = clientMirror_.player();
                // `speed` is the accumulated gait phase and therefore never
                // returns to zero. The eased stride is the locomotion amount.
                playerWalking = playerSnap.stride > 0.002F ||
                                playerSnap.previousStride > 0.002F;
            }
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
                if (angle < 0.0F)
                    angle += 2.0F * pi;
                return angle - pi;
            };
            if (!worldBodyYawInitialized) {
                worldBodyYaw = lookYaw;
                worldBodyYawInitialized = true;
            }
            // ANIM A12 (26.1 §6/§7.3): the body follows the head with a dead zone
            // and a hard clamp. head-relative yaw is free within +/-50 deg (dead
            // zone); past that the body eases toward the look at ~30%/tick, plus an
            // extra 20%/tick beyond the 50 deg edge; the head-relative angle is
            // hard-clamped to +/-75 deg so the head never over-rotates. dt*20 maps
            // the per-tick rates onto the frame. (Rebedrock +Y sign / exact feel
            // is a mac visual-pass concern.)
            constexpr float kHeadYawDeadZone = 0.8727F;  // 50 deg
            constexpr float kHeadYawClamp = 1.3090F;     // 75 deg hard clamp
            const float tickAlpha = std::min(1.0F, deltaSeconds * 20.0F);
            float lagDiff = wrapAngle(lookYaw - worldBodyYaw);
            const float absLag = std::fabs(lagDiff);
            if (absLag > kHeadYawDeadZone || playerWalking) {
                // Base 30%/tick follow, plus 20%/tick more once past the dead zone.
                const float followRate = 0.30F + (absLag > kHeadYawDeadZone ? 0.20F : 0.0F);
                worldBodyYaw += lagDiff * followRate * tickAlpha;
                lagDiff = wrapAngle(lookYaw - worldBodyYaw);
            }
            // Hard clamp: the head-relative yaw can never exceed 75 deg.
            if (lagDiff > kHeadYawClamp) {
                worldBodyYaw = lookYaw - kHeadYawClamp;
            } else if (lagDiff < -kHeadYawClamp) {
                worldBodyYaw = lookYaw + kHeadYawClamp;
            }
            constexpr float kMaxHeadYaw = kHeadYawClamp;
            const float headRelative = wrapAngle(lookYaw - worldBodyYaw);
            worldPlayerAnimator.setCursorLook(headRelative / kMaxHeadYaw, -lookDir.y);
            // ANIM task2: the third-person world player now runs the SAME
            // PlayerModelAnimator controller stack as the inventory preview
            // (retiring HumanoidPoseSolver). It is fed the AUTHORITATIVE vanilla
            // WalkAnimationState the snapshot carries — the gait amplitude
            // (walkAmount, saturates to 1.0, decays to 0 on stop) and phase
            // (walkPosition) — plus sneaking and the render age, so idle -> no
            // swing and stopping returns the limbs to rest. body_look_amount stays
            // 0 (the world body yaw is applied at the model root, not in the clip).
            {
                const auto worldSnap = clientMirror_.player();
                const float worldAlpha = clientMirror_.interpolationAlpha();
                const float walkAmount = std::clamp(
                    worldSnap.previousWalkAmount +
                        (worldSnap.walkAmount - worldSnap.previousWalkAmount) * worldAlpha,
                    0.0F, 1.0F);
                const float walkPosition =
                    worldSnap.previousWalkPosition +
                    (worldSnap.walkPosition - worldSnap.previousWalkPosition) * worldAlpha;
                // Item/Block arm poses would raise one arm with no held-item layer
                // drawn yet, so ordinary holding stays at rest (matching the prior
                // solver wiring); an active use is still shown.
                const auto worldState = render::player::extractPlayerRenderState(
                    worldSnap, worldAlpha, lastWorldSwingSequence_);
                const bool holdingItem =
                    worldState.use.active &&
                    (worldState.rightArmPose == render::player::ArmPose::Eat);
                worldPlayerAnimator.setItemHold(holdingItem);
                const float ageInTicks =
                    static_cast<float>(worldSnap.serverTick) + worldAlpha;
                worldPlayerAnimator.updateWorldPlayer(deltaSeconds, walkAmount, walkPosition,
                                                      ageInTicks, worldSnap.sneaking);
            }
            if (!paused && worldReady) {
                // The sun no longer rides real frames: the Overworld clock advances
                // inside the fixed sim tick (gated there by doDaylightCycle), so all
                // that is left here is the frame-local animation clock.
                renderTimeSeconds += static_cast<double>(deltaSeconds);
                // The held-item pose is sampled from the per-tick player snapshot
                // (published atomically), interpolated with THIS frame's partial
                // tick — never the previous frame's alpha. The extractor snaps
                // across a swing restart (sequence change) so the arm never
                // replays back from the apex.
                {
                    // Copy the coherent snapshot, then render purely from the copy.
                    const auto playerSnapshot = clientMirror_.player();
                    const float currentAlpha = clientMirror_.interpolationAlpha();
                    const auto frame =
                        render::player::extractPlayerRenderState(playerSnapshot, currentAlpha,
                                                                 lastSwingSequence_);
                    if (frame.use.active) {
                        heldItemAnimation.setAction(animation::ModelAction::Eat,
                                                    std::clamp(frame.use.progress, 0.0F, 1.0F));
                    } else if (frame.swing.active) {
                        heldItemAnimation.setAction(
                            frame.swing.animation == gameplay::SwingAnimation::Use
                                ? animation::ModelAction::Use
                                : animation::ModelAction::Break,
                            frame.swing.progress);
                    } else {
                        heldItemAnimation.setAction(animation::ModelAction::None, 0.0F);
                    }
                }
                // One read section covers every world sample this frame's
                // effects make: particles, rain collision and the weather
                // ambience all raycast into the world.
                {
                    // Everything here reads the render-owned client cache and the
                    // atomically published world snapshot, so it needs no lock.
                    hud_.updateVignetteDarkness(deltaSeconds);
                    particleSystem.update(deltaSeconds, clientCache);
                // CPU rain drops follow the smoothed weather gradient and drive
                // landing splashes/audio in every mode. Particle and async also
                // render these exact drops; texture mode independently draws the
                // vanilla precipitation-column field.
                const float thunderGradient = clientMirror_.world().thunderGradient;
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
                    windShiftTimer_ =
                        10.0F + static_cast<float>(weatherSoundRng_ >> 8) / 16777216.0F * 10.0F;
                }
                windShiftTimer_ -= deltaSeconds;
                const float windTurn = wrapAngle(windTargetAngle_ - rainWindAngle_);
                rainWindAngle_ += windTurn * std::min(1.0F, deltaSeconds * 0.8F);
                const float windSpeed = 1.5F + thunderGradient * 4.5F;
                const glm::vec2 wind{std::cos(rainWindAngle_) * windSpeed,
                                     std::sin(rainWindAngle_) * windSpeed};
                rainSystem.update(deltaSeconds, camera.position(),
                                  clientMirror_.world().rainGradient, rainTargetCount(),
                                  clientCache, wind);
                if (rainMode_ == RainMode::Texture) {
                    rainSystem.emitTextureImpacts(deltaSeconds, camera.position(),
                                                  clientMirror_.world().rainGradient,
                                                  clientCache);
                }
                for (const auto& splash : rainSystem.splashes()) {
                    if (splash.sampledImpact) {
                        particleSystem.spawnRainImpact(splash.position, splash.onWater);
                    } else {
                        particleSystem.spawnRainSplash(splash.position, splash.direction);
                    }
                }
                // tickRainSplashing also drives the rain *sound*: a weather.rain
                // clip at the surface the drops hit, muffled when the player is
                // under a roof, all scaled by the smoothed rain gradient.
                    updateWeatherSound(clientCache);
                static bool stormReported = false;
                if (!stormReported && clientMirror_.world().thundering &&
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
                // column surface cache reduced collision to (the old direct
                // path did one world lookup per drop per frame). Sampled once
                // the population is full AND the cache is
                // warm (a warm frame makes far fewer world lookups than it has
                // drops), so the number is the steady state, not the first
                // frames' probe warmup.
                static bool collisionReported = false;
                if (!collisionReported &&
                    rainSystem.drops().size() >= rainTargetCount() * 9U / 10U &&
                    rainSystem.lastUpdateLookups() < rainSystem.drops().size() / 4U) {
                    collisionReported = true;
                    std::cout << "[rain] collision lookups/frame=" << rainSystem.lastUpdateLookups()
                              << " drops=" << rainSystem.drops().size() << "\n";
                }
                    rainTime_ += deltaSeconds;
                }
                // D0: processInput() already shipped this frame's movement over
                // the channel; the server stages it before the tick reads it, so
                // there is no separate commitInput() publish step any more.
                if (!simulationDriver.threaded()) {
                    // Synchronous fallback (MC_REBEDROCK_SYNC_TICK=1). Kept
                    // because it is the only way to bisect a threading problem
                    // against known-good behaviour.
                    static_cast<void>(
                        simulationDriver.advance(deltaSeconds, [this] { runtime.tick(); }));
                }
            } else if (!simulationDriver.threaded()) {
                simulationDriver.reset();
            }
            // A chat command the player submitted was executed on the runtime's
            // dispatcher inside the tick; append its result to the history once
            // it lands.
            if (const auto chatResult = runtime.takeChatResult(); chatResult.has_value()) {
                chatHistory.push(chatResult->message, chatResult->success, uiTimeSeconds);
            }
            // The alpha comes from the published snapshot's own timestamp
            // (gameSession) rather than SimulationDriver's separately-clocked
            // accumulator, so it stays in step with the endpoints this frame
            // reads instead of running a tick ahead of them at a tick boundary —
            // the phase race that made moving drops and the swung hand jitter.
            const float physicsAlpha = clientMirror_.interpolationAlpha();
            renderInterpolationAlpha = physicsAlpha;
            glm::vec3 renderedFeetPosition{};
            float playerEyeHeight = 0.0F;
            float fovMultiplier = 1.0F;
            {
                const auto playerSnap = clientMirror_.player();
                renderedFeetPosition = playerSnap.physicsPrevious +
                                       (playerSnap.physicsCurrent - playerSnap.physicsPrevious) *
                                           physicsAlpha;
                // Capture the HUD snapshot from the per-tick player snapshot
                // (published under the sim's write lock), not live gameplay.
                uiFrameData_.health = playerSnap.health;
                uiFrameData_.foodLevel = playerSnap.foodLevel;
                uiFrameData_.airTicks = playerSnap.airTicks;
                uiFrameData_.ticksSinceDamage = playerSnap.ticksSinceDamage;
                uiFrameData_.gameMode = playerSnap.gameMode;
                uiFrameData_.eating = playerSnap.eating;
                uiFrameData_.selectedStack = playerSnap.heldStack;
                uiFrameData_.selectedHotbarSlot = playerSnap.selectedHotbarSlot;
                const auto worldSnap = clientMirror_.world();
                uiFrameData_.containerScreen = worldSnap.openContainerScreen;
                uiFrameData_.activeChest = worldSnap.openChest;
                playerEyeHeight =
                    playerSnap.sneaking ? gameplay::PlayerController::kSneakingEyeHeight
                                        : gameplay::PlayerController::kEyeHeight;
                fovMultiplier = playerSnap.previousFieldOfViewMultiplier +
                                (playerSnap.fieldOfViewMultiplier -
                                 playerSnap.previousFieldOfViewMultiplier) *
                                    physicsAlpha;
            }
            camera.setPosition(renderedFeetPosition + glm::vec3{0.0F, playerEyeHeight, 0.0F});
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
                const std::size_t stressClock = std::getenv("MC_REBEDROCK_LOAD_SAVE") != nullptr
                                                    ? renderedFrames
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
                const auto stressWrite = worldLock.write();
                gameSession.teleportPlayer(
                    gameplay::kPrimaryPlayerId,
                    stressPos - glm::vec3{0.0F, snapshotEyeHeight(), 0.0F});
            }
            // GameRenderer#getFov: the base FOV times the gameSession.player()'s movement
            // multiplier, interpolated across the physics tick the same way the
            // eye position is. Sprinting widens it to 1.15x, creative flight to
            // 1.1x, and both ease in over a few ticks.
            camera.setFieldOfViewDegrees(baseFieldOfViewDegrees * fovMultiplier);
            audioSystem.updateListener(camera.position(), camera.direction(), {0.0F, 1.0F, 0.0F});
            audioSystem.update();
            driveAmbientMusic(deltaSeconds);
            if (worldSessionActive)
                world_.processChunkStreaming();
            {
                // The authoritative interaction now runs inside the simulation
                // tick (which owns the world's write section); this frame only
                // raycasts the aim target the input handlers package into
                // commands, plus the separate Q-key drop. The ray tests the
                // render-owned client cache, so no lock is needed.
                updateInteractionTarget();
            }
            {
                const auto dropWrite = worldLock.write();
                world_.updateItemDrop();
            }
            // C-1b-3: the simulation's side effects (world edits to the client
            // cache, sounds, particles, container/eating reactions) are applied
            // by the frame-top channel pump above, decoded from the server's
            // per-tick events instead of drained from the bridge here.
            drawFrame();
            ++renderedFrames;
            if (diag::traceEnabled()) {
                const double frameMs = diag::msSince(frameCpuStart);
                if (frameMs >= diag::traceThresholdMs()) {
                    const auto& t = diag::frameTrace();
                    std::cout << "[frametrace] frame=" << renderedFrames
                              << " cpuMs=" << frameMs
                              << " persistMs=" << t.persistMs
                              << " saveChunkMs=" << t.saveChunkMs
                              << " lockHoldMs=" << t.lockHoldMs
                              << " drainMs=" << t.drainMs
                              << " fenceWaitMs=" << t.fenceWaitMs
                              << " unloaded=" << t.unloadedChunks
                              << " saveChunkCalls=" << t.saveChunkCalls
                              << " batches=" << t.queueBatchCount
                              << " editScan=" << t.editScan
                              << " center=(" << t.newCenterX << ',' << t.newCenterZ << ')'
                              << " centerChanged=" << (t.centerChanged ? 1 : 0)
                              << '\n';
                }
            }
            // N-Mem (P1-2 data): periodic three-world resident report. The smoke
            // test only loads the spawn area, so its numbers understate the real
            // occupancy — this samples the three chunk copies during real play/
            // stress once every ~2s. Gated off by default. The server world is
            // read under a short read lock (the sim thread mutates it); the
            // client cache is render-owned; the worker world is an atomic sample.
            if (worldReady) {
                static const bool memoryReport =
                    std::getenv("MC_REBEDROCK_MEMORY_REPORT") != nullptr;
                if (memoryReport) {
                    static float memoryReportAccum = 0.0F;
                    memoryReportAccum += deltaSeconds;
                    if (memoryReportAccum >= 2.0F) {
                        memoryReportAccum = 0.0F;
                        std::size_t serverBytes = 0;
                        std::size_t serverUnique = 0;
                        {
                            const auto memRead = worldLock.read();
                            serverBytes = interactionWorld.residentBytes();
                            serverUnique = interactionWorld.uniqueResidentBytes();
                        }
                        const auto clientBytes = clientCache.residentBytes();
                        const auto clientUnique = clientCache.uniqueResidentBytes();
                        const auto workerBytes = chunkStreamer.workerWorldResidentBytes();
                        const auto workerUnique = chunkStreamer.workerWorldUniqueResidentBytes();
                        const auto total = serverBytes + clientBytes + workerBytes;
                        const auto uniqueTotal = serverUnique + clientUnique + workerUnique;
                        // total = sum of the three logical views (shared chunks
                        // counted once per holder). uniqueTotal = the exclusively
                        // owned part; the gap (total-uniqueTotal) is the COW-shared
                        // physical copy the P1-2 merge could reclaim.
                        std::cout << "[memory] server=" << serverBytes << "(u" << serverUnique
                                  << ") client=" << clientBytes << "(u" << clientUnique
                                  << ") worker=" << workerBytes << "(u" << workerUnique
                                  << ") total=" << total << " unique=" << uniqueTotal << " ("
                                  << (total / (1024U * 1024U)) << "MB/" << (uniqueTotal / (1024U * 1024U))
                                  << "MB)\n";
                        // §7.4 GPU-side ownership: total VMA allocation vs the big
                        // owners (world mesh vertex/index, staging, textures); the
                        // rest (offscreen/shadow/uniform/particle/rain) falls into
                        // `other`. cpuMeshPool is the CPU RenderMeshData reuse pool.
                        VmaTotalStatistics vmaStats{};
                        vmaCalculateStatistics(allocator, &vmaStats);
                        const auto gpuAllocated = vmaStats.total.statistics.allocationBytes;
                        const auto worldMeshGpu = deviceBufferPool_.totalBytes;
                        const auto stagingGpu = stagingBufferPool_.totalBytes;
                        const auto texturesGpu = textures_.residentImageBytes();
                        const auto knownGpu = worldMeshGpu + stagingGpu + texturesGpu;
                        const auto gpuOther =
                            gpuAllocated > knownGpu ? gpuAllocated - knownGpu : 0U;
                        const auto cpuMeshPool = chunkStreamer.cpuMeshPoolBytes();
                        // Break `other` open: the MSAA depth/color transient
                        // targets are the suspected bulk. Also detect whether they
                        // landed in lazily-allocated (memoryless) memory — VMA's
                        // allocationBytes counts the logical size either way, so
                        // this is how we tell the memoryless fix actually took.
                        const VkPhysicalDeviceMemoryProperties* memProps = nullptr;
                        vmaGetMemoryProperties(allocator, &memProps);
                        const auto targetBytes = [&](const auto& targets) {
                            std::pair<VkDeviceSize, VkDeviceSize> tl{0, 0};
                            for (const auto& t : targets) {
                                if (t.image.allocation == VK_NULL_HANDLE) {
                                    continue;
                                }
                                VmaAllocationInfo ai{};
                                vmaGetAllocationInfo(allocator, t.image.allocation, &ai);
                                tl.first += ai.size;
                                if ((memProps->memoryTypes[ai.memoryType].propertyFlags &
                                     VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) != 0U) {
                                    tl.second += ai.size;
                                }
                            }
                            return tl;
                        };
                        const auto depthTL = targetBytes(depthTargets);
                        const auto colorTL = targetBytes(colorTargets);
                        const auto targetsTotal = depthTL.first + colorTL.first;
                        const auto targetsLazy = depthTL.second + colorTL.second;
                        std::cout << "[gpumem] allocated=" << gpuAllocated << " ("
                                  << (gpuAllocated / (1024U * 1024U)) << "MB) worldMesh="
                                  << worldMeshGpu << " staging=" << stagingGpu
                                  << " textures=" << texturesGpu << " other=" << gpuOther
                                  << " depthColorTargets=" << targetsTotal
                                  << " (lazy=" << targetsLazy << ") | cpuMeshPool=" << cpuMeshPool
                                  << "\n";
                    }
                }
            }
            // The occlusion test scene renders a few frames so the two-frame
            // query latency resolves, then exits and dumps the diagnostics.
            if (testScene.has_value() && testScene->occlusionScene && renderedFrames >= 30U) {
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }
            if (options.frameRateLimit > 0) {
                const auto targetFrameDuration = std::chrono::duration<double>(
                    1.0 / static_cast<double>(options.frameRateLimit));
                const auto deadline =
                    currentFrameTime +
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
            // A chat-command effect may still be landing (commands process on the
            // next server tick, up to ~50 ms after submit). Check it in parallel
            // with the scripted steps, so the step sequence keeps its frame
            // schedule while each check polls for its effect.
            if (smokeTest && smokeWaitActive) {
                if (smokeWaitCondition()) {
                    smokeWaitActive = false;
                    if (smokeWaitAction) {
                        auto action = std::move(smokeWaitAction);
                        smokeWaitAction = nullptr;
                        action();
                    }
                } else if (smokeGameplayFrames >= smokeWaitDeadline) {
                    throw std::runtime_error("Smoke test timed out: " + smokeWaitLabel);
                }
            }
            if (smokeTest && smokeGameplayFrames == 16U) {
                setInventoryOpen(true);
            } else if (smokeTest && smokeGameplayFrames == 20U) {
                // The creative catalogue click goes through the command queue so
                // the smoke exercises the real interaction path.
                gameplay::ClickCreativeItem creative;
                creative.catalogStack = {world::Block::Air, 1U, &gameplay::items::Diamond};
                creative.button = gameplay::InventoryMouseButton::Left;
                runtime.enqueueClientCommand(std::move(creative));
            } else if (smokeTest && smokeGameplayFrames == 24U) {
                gameplay::ClickSlot click;
                click.kind = gameplay::SlotKind::PlayerInventory;
                click.slotIndex = 0U;
                click.button = 0;
                runtime.enqueueClientCommand(std::move(click));
            } else if (smokeTest && smokeGameplayFrames == 28U) {
                const auto smokeRead = worldLock.read();
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
                const auto smokeWrite = worldLock.write();
                gameSession.spawnItemEntity(
                    clientMirror_.player().physicsCurrent + glm::vec3{1.8F, 1.0F, 0.0F},
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
                // The /gamemode survival command lands on the next server tick.
                smokeWaitActive = true;
                smokeWaitDeadline = smokeGameplayFrames + 40U;
                smokeWaitLabel = "enter survival mode";
                smokeWaitCondition = [&] {
                    const auto smokeRead = worldLock.read();
                    return clientMirror_.player().gameMode == gameplay::GameMode::Survival;
                };
                if (clientMirror_.world().inventorySlots[0].item != &gameplay::items::Diamond) {
                    throw std::runtime_error(
                        "Smoke test lost the shared inventory during mode switch");
                }
            } else if (smokeTest && smokeGameplayFrames == 57U) {
                // Exercise the damage-tint draw immediately after the held-item
                // pass. This catches descriptor-set compatibility drift between
                // itemPipelineLayout and hudPipelineLayout under validation.
                const auto smokeWrite = worldLock.write();
                if (!gameSession.hurtPlayer(gameplay::kPrimaryPlayerId,
                                            gameplay::DamageType::Generic, 1.0F, *this)) {
                    throw std::runtime_error("Smoke test could not trigger damage overlay");
                }
            } else if (smokeTest && smokeGameplayFrames == 59U && stressFrames == 0U) {
                // Then run the rest of the script with the sun shadows *off*,
                // which is the default every player actually uses. Forcing them
                // on for the whole run left that path unvalidated, and it is
                // the one where the shadow depth map is never rendered into:
                // the descriptors still declare SHADER_READ_ONLY_OPTIMAL and
                // three fragment shaders still sample it. A layout that only
                // held because the pre-pass happened to run is exactly the bug
                // this alternation exists to catch.
                options.sunShadows = false;
                shadowDisabled = true;
            } else if (smokeTest && smokeGameplayFrames == 58U) {
                setInventoryOpen(true);
            } else if (smokeTest && smokeGameplayFrames == 60U) {
                // Deterministically exercise the instanced particle path: a
                // block-break burst next to the player produces hundreds of
                // particles in a single vkCmdDraw.
                const auto smokeRead = worldLock.read();
                const glm::vec3 spawn = clientMirror_.player().physicsCurrent;
                particleSystem.spawnBlockBreak({static_cast<int>(std::floor(spawn.x)),
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
                // The /gamemode command lands on the next server tick.
                smokeWaitActive = true;
                smokeWaitDeadline = smokeGameplayFrames + 40U;
                smokeWaitLabel = "return to creative mode";
                smokeWaitCondition = [&] {
                    const auto smokeRead = worldLock.read();
                    return clientMirror_.player().gameMode == gameplay::GameMode::Creative &&
                           clientMirror_.world().inventorySlots[0].item ==
                               &gameplay::items::Diamond;
                };
            } else if (smokeTest && smokeGameplayFrames == 74U) {
                setChatOpen(true);
                chatInputText = "/give 0 1";
            } else if (smokeTest && smokeGameplayFrames == 76U) {
                submitChatInput();
            } else if (smokeTest && smokeGameplayFrames == 78U) {
                // Catalog index 0 is the first registered building block (grass).
                // The /give command lands on the next server tick.
                smokeWaitActive = true;
                smokeWaitDeadline = smokeGameplayFrames + 40U;
                smokeWaitLabel = "/give by catalog index";
                smokeWaitCondition = [&] {
                    const auto smokeRead = worldLock.read();
                    return std::ranges::any_of(
                        clientMirror_.world().inventorySlots,
                        [](const gameplay::ItemStack& stack) {
                            return stack.block == world::Block::Grass && stack.count >= 1U;
                        });
                };
            } else if (smokeTest && smokeGameplayFrames == 80U) {
                setChatOpen(true);
                chatInputText = "/give minecraft:acacia_planks 3";
            } else if (smokeTest && smokeGameplayFrames == 82U) {
                submitChatInput();
            } else if (smokeTest && smokeGameplayFrames == 84U) {
                smokeWaitActive = true;
                smokeWaitDeadline = smokeGameplayFrames + 40U;
                smokeWaitLabel = "/give by identifier";
                smokeWaitCondition = [&] {
                    const auto smokeRead = worldLock.read();
                    return std::ranges::any_of(
                        clientMirror_.world().inventorySlots,
                        [](const gameplay::ItemStack& stack) {
                            return stack.block == world::Block::AcaciaPlanks && stack.count >= 3U;
                        });
                };
            } else if (smokeTest && smokeGameplayFrames == 86U) {
                setChatOpen(true);
                chatInputText = "/time set midnight";
            } else if (smokeTest && smokeGameplayFrames == 88U) {
                submitChatInput();
            } else if (smokeTest && smokeGameplayFrames == 90U) {
                smokeWaitActive = true;
                smokeWaitDeadline = smokeGameplayFrames + 40U;
                smokeWaitLabel = "set world time";
                smokeWaitCondition = [&] {
                    const auto smokeRead = worldLock.read();
                    const auto perDay =
                        static_cast<std::uint64_t>(world::DayNightCycle::kTicksPerDay);
                    const auto tick = std::fmod(clientMirror_.world().dayTimeTicks, static_cast<double>(perDay));
                    return std::abs(tick - 18000.0) <= 4.0;
                };
            } else if (smokeTest && smokeGameplayFrames == 92U) {
                setInventoryOpen(true);
            } else if (smokeTest && smokeGameplayFrames == 94U) {
                // Put a full stack of apples into the last hotbar slot, then
                // close the screen and select it — all through the command queue
                // so the smoke exercises the real interaction path.
                gameplay::ClickCreativeItem creative;
                creative.catalogStack = {world::Block::Air, 1U, &gameplay::items::Apple};
                creative.button = gameplay::InventoryMouseButton::Left;
                runtime.enqueueClientCommand(std::move(creative));
                gameplay::ClickSlot click;
                click.kind = gameplay::SlotKind::PlayerInventory;
                click.slotIndex = 8U;
                click.button = 0;
                runtime.enqueueClientCommand(std::move(click));
                const auto smokeWrite = worldLock.write();
                setInventoryOpenLocked(false);
                gameplay::SwapSlot swap;
                swap.index = 8U;
                runtime.enqueueClientCommand(std::move(swap));
            } else if (smokeTest && smokeGameplayFrames == 96U) {
                const auto smokeRead = worldLock.read();
                smokeAppleCount = clientMirror_.world()
                                      .inventorySlots[clientMirror_.player()
                                                          .selectedHotbarSlot]
                                      .count;
                if (smokeAppleCount == 0U) {
                    throw std::runtime_error("Smoke test apple stack missing");
                }
                // In creative the meal must not spend the food (Java 1.16.1).
                enqueueUseStart();
            } else if (smokeTest && smokeGameplayFrames == 400U) {
                enqueueUseStop();
                const auto smokeRead = worldLock.read();
                if (clientMirror_.world()
                        .inventorySlots[clientMirror_.player().selectedHotbarSlot]
                        .count != smokeAppleCount) {
                    throw std::runtime_error("Smoke test creative eating consumed food");
                }
            } else if (smokeTest && smokeGameplayFrames == 402U) {
                setChatOpen(true);
                chatInputText = "/gamemode survival";
            } else if (smokeTest && smokeGameplayFrames == 404U) {
                submitChatInput();
            } else if (smokeTest && smokeGameplayFrames == 406U) {
                // The /gamemode survival command lands on the next server tick.
                smokeWaitActive = true;
                smokeWaitDeadline = smokeGameplayFrames + 40U;
                smokeWaitLabel = "return to survival mode";
                smokeWaitCondition = [&] {
                    const auto smokeRead = worldLock.read();
                    return clientMirror_.player().gameMode == gameplay::GameMode::Survival;
                };
                // Once the mode lands, arm the meal: the apple survived creative
                // eating, survival should spend it.
                smokeWaitAction = [&] {
                    gameplay::SwapSlot swap;
                    swap.index = 8U;
                    runtime.enqueueClientCommand(std::move(swap));
                    smokeAppleCount = clientMirror_.world()
                                          .inventorySlots[8U]
                                          .count;
                    if (smokeAppleCount == 0U) {
                        throw std::runtime_error("Smoke test apple stack missing in survival");
                    }
                    enqueueUseStart();
                };
            } else if (smokeTest && smokeGameplayFrames == 410U) {
                // Snap the weather to full rain instantly (test helper, not
                // chat) so the smoke exercises the rain path at full intensity
                // regardless of frame rate; the three render modes compare
                // identical drop counts.
                const auto smokeWrite = worldLock.write();
                gameSession.weatherSystem().forceRainGradient(1.0F);
            } else if (smokeTest && smokeGameplayFrames == 420U) {
                // Escalate to a full storm so the smoke also exercises the
                // thunder-boosted rain volume and cross-wind.
                const auto smokeWrite = worldLock.write();
                gameSession.weatherSystem().forceThunderGradient(1.0F);
            } else if (smokeTest && smokeGameplayFrames == 700U) {
                enqueueUseStop();
                const auto smokeRead = worldLock.read();
                if (clientMirror_.world()
                        .inventorySlots[clientMirror_.player().selectedHotbarSlot]
                        .count >= smokeAppleCount) {
                    throw std::runtime_error("Smoke test survival eating did not consume an apple");
                }
            } else if (smokeTest && smokeGameplayFrames == 702U) {
                setChatOpen(true);
                chatInputText = "/tp 8 200 8";
            } else if (smokeTest && smokeGameplayFrames == 704U) {
                submitChatInput();
            } else if (smokeTest && smokeGameplayFrames == 706U) {
                // The /tp command lands on the next server tick.
                smokeWaitActive = true;
                smokeWaitDeadline = smokeGameplayFrames + 40U;
                smokeWaitLabel = "/tp teleport";
                smokeWaitCondition = [&] {
                    const auto smokeRead = worldLock.read();
                    return clientMirror_.player().physicsCurrent.y >= 150.0F;
                };
            }
            if (smokeTest && !smokeReturnedToTitle && smokeGameplayFrames >= smokeFrameLimit &&
                completedStreamBatchCount >= 2U && completedBlockEditCount >= 1U &&
                pendingSectionUpdates.empty()) {
                // M-Chunk B-5: the spawn chunk must have reached the client
                // cache — an empty cache means the dual-world split lost the
                // chunks and every presentation read (raycast, light, mesh
                // preview) would silently see air.
                if (!clientCache.hasChunk(world::chunkPositionFromWorld(24.5F, 24.5F))) {
                    throw std::runtime_error(
                        "Smoke test: client cache lost the spawn chunk");
                }
                // M-Chunk B-5: the client cache must mirror the server world —
                // same batches, same edits — so the spawn column agrees cell for
                // cell. A broken edit-sync or a missed state delta would diverge
                // them and the render would silently show the wrong world.
                for (int syncY = world::kMinY; syncY < world::kMaxY; ++syncY) {
                    if (clientCache.state(24, syncY, 24) !=
                        interactionWorld.state(24, syncY, 24)) {
                        throw std::runtime_error(
                            "Smoke test: client cache diverged from the server world");
                    }
                }
                // Side-split memory: the server world, the client cache AND the
                // streamer worker world each own a chunk copy (three resident
                // copies — the P1-2 debt). All three are measured and the sum is
                // bounded — a gross leak on any side blows it.
                const auto serverChunkBytes = interactionWorld.residentBytes();
                const auto clientChunkBytes = clientCache.residentBytes();
                const auto workerChunkBytes = chunkStreamer.workerWorldResidentBytes();
                std::cout << "[memory] serverChunkResident=" << serverChunkBytes
                          << " clientChunkResident=" << clientChunkBytes
                          << " workerChunkResident=" << workerChunkBytes
                          << " total=" << (serverChunkBytes + clientChunkBytes + workerChunkBytes)
                          << "\n";
                if (serverChunkBytes + clientChunkBytes + workerChunkBytes >
                    512U * 1024U * 1024U) {
                    throw std::runtime_error(
                        "Smoke test: three-world resident exceeds the budget");
                }
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

    void refreshSaveList() {
        menuSystem.saveSummaries = saveRepository.list();
        if (menuSystem.saveSummaries.empty())
            menuSystem.selectedWorldIndex = 0U;
        else
            menuSystem.selectedWorldIndex =
                std::min(menuSystem.selectedWorldIndex, menuSystem.saveSummaries.size() - 1U);
        const std::size_t visibleRows = saveListVisibleRowCount();
        const std::size_t maximumFirst = menuSystem.saveSummaries.size() > visibleRows
                                             ? menuSystem.saveSummaries.size() - visibleRows
                                             : 0U;
        menuSystem.worldListFirstIndex = std::min(menuSystem.worldListFirstIndex, maximumFirst);
    }

    void scrollWorldList(int rows) {
        const std::size_t visibleRows = saveListVisibleRowCount();
        const std::size_t maximumFirst = menuSystem.saveSummaries.size() > visibleRows
                                             ? menuSystem.saveSummaries.size() - visibleRows
                                             : 0U;
        const auto requested = static_cast<long long>(menuSystem.worldListFirstIndex) + rows;
        menuSystem.worldListFirstIndex = static_cast<std::size_t>(
            std::clamp<long long>(requested, 0LL, static_cast<long long>(maximumFirst)));
    }

    void scrollLanguageList(int rows) {
        const std::size_t visibleRows = languageVisibleRowCount();
        const std::size_t maximumFirst = menuSystem.languageCodes.size() > visibleRows
                                             ? menuSystem.languageCodes.size() - visibleRows
                                             : 0U;
        const auto requested = static_cast<long long>(menuSystem.languageListFirstIndex) + rows;
        menuSystem.languageListFirstIndex = static_cast<std::size_t>(
            std::clamp<long long>(requested, 0LL, static_cast<long long>(maximumFirst)));
    }

    // PX-6 Bug1: scroll the Controls key-bind list by the wheel, clamped to the
    // last visible page (the same contract as the world/language lists).
    void scrollControlsList(int rows) {
        const std::size_t total = input::keyBindRows().size();
        const std::size_t visibleRows = ui::controlsVisibleRowCount(
            static_cast<float>(swapchainExtent.width),
            static_cast<float>(swapchainExtent.height), menuSystem.guiScaleSetting);
        const std::size_t maximumFirst = total > visibleRows ? total - visibleRows : 0U;
        const auto requested = static_cast<long long>(menuSystem.controlsListFirstIndex) + rows;
        menuSystem.controlsListFirstIndex = static_cast<std::size_t>(
            std::clamp<long long>(requested, 0LL, static_cast<long long>(maximumFirst)));
    }

    void updateLanguageScrollFromCursor() {
        if (menuSystem.pageStack.current() != ui::PageId::Language) {
            menuSystem.languageScrollbarDragging = false;
            return;
        }
        const auto cursor = currentFramebufferCursor();
        const ui::HudLayout layout{static_cast<float>(swapchainExtent.width),
                                   static_cast<float>(swapchainExtent.height),
                                   menuSystem.guiScaleSetting};
        const std::size_t visible = languageVisibleRowCount();
        menuSystem.languageListFirstIndex = ui::languageScrollIndexFromCursor(
            layout, static_cast<float>(swapchainExtent.width),
            menuSystem.languageCodes.size(), visible, cursor.y);
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

    void
    submitWorldEdit(int x, int y, int z, world::Block block, std::uint8_t fluidLevel = 0U,
                    std::optional<world::BlockOrientation> orientation = std::nullopt) override {
        const auto resolvedOrientation = orientation.value_or(world::defaultOrientation(block));
        rememberWorldEdit({x, y, z, world::BlockState{block, resolvedOrientation, fluidLevel}});
        chunkStreamer.setBlock(x, y, z, block, fluidLevel, resolvedOrientation);
        // M-Chunk B-5: the simulation already wrote interactionWorld; mirror the
        // edit into the client cache so the render mesh reflects it this frame.
        static_cast<void>(
            clientCache.setState(x, y, z, world::BlockState{block, resolvedOrientation, fluidLevel}));
    }

    void submitWorldStateEdit(int x, int y, int z, world::BlockState state) override {
        // setState, not setBlock: LIT is exactly what the loose block/fluid/
        // orientation triple cannot carry, so a furnace lighting would reach the
        // render streamer unlit. The saved edit now carries the whole state as
        // well — it used to decompose into the triple here, which silently
        // dropped LIT on the way to disk.
        rememberWorldEdit({x, y, z, state});
        chunkStreamer.setState(x, y, z, state);
        static_cast<void>(clientCache.setState(x, y, z, state));
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
        // The render-side light is maintained on the client cache, so an edit
        // previews against what will actually be drawn.
        interactionLightEngine.updateBlock(clientCache, worldX, y, worldZ);

        std::vector<world::SectionPosition> sections;
        const auto mark = [&](world::SectionPosition position) {
            if (position.sectionY < 0 || position.sectionY >= world::kSectionCount)
                return;
            if (!clientCache.hasChunk({position.chunkX, position.chunkZ}))
                return;
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
                    if (!world::isWorldYInRange(sampleY))
                        continue;
                    const auto chunk = world::chunkPositionFromWorld(
                        static_cast<float>(worldX + dx), static_cast<float>(worldZ + dz));
                    mark({chunk.x, world::sectionIndexFromWorldY(sampleY), chunk.z});
                }
            }
        }
        // Light changes can reach well beyond the edited voxel (a torch spreads
        // up to 14 blocks). Remesh every section whose light actually changed,
        // skipping empty ones that hold no vertices to relight.
        for (const auto position : interactionLightEngine.takeDirtySections()) {
            const world::Chunk* chunk = clientCache.chunk({position.chunkX, position.chunkZ});
            if (chunk != nullptr && !chunk->section(position.sectionY).empty()) {
                mark({position.chunkX, position.sectionY, position.chunkZ});
            }
        }

        if (sections.empty())
            return;
        // Cheap sampler *view*: O(1) reads of the light we just propagated into
        // the client cache above. (The per-chunk constructor re-propagates a
        // ~48x384x48 region with two BFS passes and must not run per edit here.)
        const world::ChunkLightSampler lighting{clientCache};
        for (const auto position : sections) {
            world_.remeshSectionImmediate(position, lighting);
        }
    }

    void clearRenderedWorld() {
        simulationActive.store(false, std::memory_order_release);
        checkVk(vkDeviceWaitIdle(device), "vkDeviceWaitIdle(world reset)");
        // The device is idle, so every pooled buffer — including those still in
        // the deferred slots — is safe to hand back to the free lists, letting
        // the next world reuse the same buffers instead of reallocating.
        for (auto& slot : deviceBufferPool_.deferred) {
            for (auto& buffer : slot) {
                world_.releaseStreamBufferNow(deviceBufferPool_, buffer);
            }
            slot.clear();
        }
        for (auto& slot : stagingBufferPool_.deferred) {
            for (auto& buffer : slot) {
                world_.releaseStreamBufferNow(stagingBufferPool_, buffer);
            }
            slot.clear();
        }
        for (auto& [position, mesh] : gpuMeshes) {
            static_cast<void>(position);
            world_.releaseStreamBufferNow(deviceBufferPool_, mesh.vertexBuffer);
            world_.releaseStreamBufferNow(deviceBufferPool_, mesh.indexBuffer);
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
        clientCache = {};
        // Drop the mirror so the next world does not briefly show the previous
        // one's player/world state before the first tick republishes.
        clientMirror_.clear();
        gameSession.resetWorldState();
        particleSystem = {};
        savedEditIndices.clear();
        completedStreamBatchCount = 0U;
        completedBlockEditCount = 0U;
        loadedCpuChunkCount = 0U;
        peakPendingSectionCount = 0U;
        spawnPositionInitialized = false;
        worldReady = false;
    }

    void startWorld(persistence::SaveGame save) {
        simulationActive.store(false, std::memory_order_release);
        clearRenderedWorld();
        // A new world starts a fresh chat: commands and their results from the
        // previous session must not leak into the bottom-left of the next map.
        chatHistory.clear();
        lastSessionPeakPendingSectionCount = 0U;
        // The authoritative restore — the session state, the chunk streamer and
        // the seeds — lives in the runtime so a headless server loads the same
        // world. The renderer's presentation (camera, textures, menus) follows.
        runtime.loadWorld(std::move(save), viewDistanceChunks);
        savedEditIndices.reserve(currentSave->edits.size());
        for (std::size_t index = 0; index < currentSave->edits.size(); ++index) {
            const auto& edit = currentSave->edits[index];
            savedEditIndices.insert_or_assign(PersistentEditPosition{edit.x, edit.y, edit.z},
                                              index);
        }
        // Game rules travel with the world too; GameRuntime re-attaches the
        // session's own change handler when it loads the save's rules.
        camera.setPosition(snapshotCameraEye());
        spawnPositionInitialized = currentSave->hasPlayerPosition;
        textures_.updateBiomeColorTextures(currentSave->summary.seed);
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
            auto save = runtime.createWorld(menuSystem.createWorldName, seed,
                                            menuSystem.createWorldGameMode);
            refreshSaveList();
            startWorld(std::move(save));
        } catch (const std::exception& exception) {
            menuSystem.saveStatus = "Create failed: " + std::string{exception.what()};
        }
    }

    // Saving reads the world and the live entity list, so it needs a section.
    // Split in two because /setworldspawn saves from *inside* the command write
    // section and the mutex is not recursive: callers already holding a section
    // use the Locked form, everyone else uses this one. The save itself is built
    // and persisted by the runtime; this wrapper only adds the presentation.
    void saveCurrentWorld() {
        const auto saveRead = worldLock.read();
        saveCurrentWorldLocked();
    }

    void saveCurrentWorldLocked() {
        try {
            // The save is built and persisted by the runtime; false means there
            // is no save open, matching the old disabled-for-this-session path.
            if (!runtime.saveLocked()) {
                menuSystem.saveStatus = "World saving is disabled for this session";
                return;
            }
            menuSystem.saveStatus = "World saved";
            refreshSaveList();
        } catch (const std::exception& exception) {
            menuSystem.saveStatus = "Save failed: " + std::string{exception.what()};
        }
    }

    void returnToTitle(bool saveFirst) {
        simulationActive.store(false, std::memory_order_release);
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
        runtime.unloadWorld();
        menuSystem.pageStack.reset(ui::PageId::Title);
        refreshSaveList();
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }

    void processInput() {
        // D0: sample the keyboard/look once a frame and ship it as a MovementInput
        // over the channel — the server stages it on the authoritative player and
        // derives the gated fields (flightAllowed/sprintAllowed) itself. This is
        // the same once-a-frame cadence commitInput() had; the client no longer
        // writes gameSession.input() (it has no session across a real connection).
        // PX-1: sample the window into a device-agnostic frame, then let the
        // InputSystem derive the whole movement intent (WASD/space/shift/ctrl,
        // gamepad left stick, jump/forward edges) from the rebindable table. When
        // a screen is up we still poll (so edge history stays consistent) but with
        // gameplay actions gated, which yields a zeroed intent for the server.
        input::RawInputFrame frame;
        input::sampleGlfwWindow(window, camera.direction(), frame);
        const bool gameplayEnabled = worldReady && !inventoryOpen && !paused && !chatOpen;
        const input::MovementIntent intent =
            inputSystem_.poll(frame, inputEvents_, gameplayEnabled);
        // The action edges are dispatched every frame, NOT only when gameplay is
        // enabled: E must close the inventory (a screen that itself disables the
        // gameplay poll), and F3/F5 toggle debug/perspective regardless of any
        // screen — the pre-PX-1 key callback fired those unconditionally. Only the
        // strictly in-play actions (hotbar swap, drop) stay gameplay-gated; the
        // dispatcher applies the right gate per action.
        dispatchInputEvents(gameplayEnabled);
        if (!gameplayEnabled) {
            // A screen is up: send a zeroed intent so the server stops the player.
            runtime.sendClientMovement(gameplay::MovementInput{});
            clearPendingInputEdges();
            return;
        }
        gameplay::MovementInput movement;
        movement.forward = intent.forward;
        movement.strafe = intent.strafe;
        movement.lookDirection = intent.lookDirection;
        movement.jumpHeld = intent.jumpHeld;
        movement.descendHeld = intent.descendHeld;
        movement.sneakHeld = intent.sneakHeld;
        movement.sprintHeld = intent.sprintHeld;
        if (stressFrames > 0U) {
            pendingForwardPressed_ = true;
        }
        // The jump edge toggles creative flight; the forward edge feeds the sprint
        // double-tap window. The InputSystem derives both from the level bitmap;
        // the key callback's pending flags are ORed in so a press that arrived as
        // a GLFW event between polls (and the stress harness above) is not lost.
        movement.jumpPressed = intent.jumpPressed || pendingJumpPressed_;
        movement.forwardPressed = intent.forwardPressed || pendingForwardPressed_;
        // Bedrock-style auto-jump is a client option; the physics decides when the
        // obstacle is actually jumpable. flightAllowed/sprintAllowed are NOT sent —
        // the server derives them from the authoritative game mode and food level.
        movement.autoJump = options.autoJump;
        runtime.sendClientMovement(movement);
        clearPendingInputEdges();
    }

    // PX-1: translate the InputSystem's discrete action edges into the renderer's
    // existing side effects (inventory toggle, hotbar swap, drop, debug,
    // perspective). Replaces the per-key GLFW_PRESS branches the key callback used
    // to carry. Each action carries its own gate, mirroring the pre-PX-1 guards:
    //
    //   Debug / Perspective  — fired unconditionally (the old callback had no
    //                          guard); toggling debug/perspective works from any
    //                          screen state, matching vanilla F3/F5.
    //   Inventory (E)        — fired while not paused and not typing in chat, so
    //                          E BOTH opens play->inventory AND closes it again
    //                          (the inventory screen disables the gameplay poll,
    //                          so gating this on gameplayEnabled would strand it
    //                          open — the reported regression). Chat swallows E as
    //                          a character instead.
    //   DropItem / Hotbar    — strictly in-play; gated on gameplayEnabled.
    void dispatchInputEvents(bool gameplayEnabled) {
        const input::InputDispatchGate gate{
            gameplayEnabled,
            /*inventoryToggleEnabled=*/worldReady && !paused && !chatOpen,
        };
        for (std::size_t i = 0; i < inputEvents_.size(); ++i) {
            const auto& event = inputEvents_[i];
            if (event.phase != input::EventPhase::Pressed) {
                continue;
            }
            if (!input::shouldDispatchAction(event.action, gate)) {
                continue;
            }
            switch (event.action) {
                case input::InputAction::Inventory:
                    setInventoryOpen(!inventoryOpen);
                    break;
                case input::InputAction::Debug:
                    debugOverlayOpen = !debugOverlayOpen;
                    break;
                case input::InputAction::Perspective:
                    cameraPerspective = nextPerspective(cameraPerspective);
                    break;
                case input::InputAction::DropItem:
                    dropRequested = true;
                    dropWholeStack =
                        glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS;
                    break;
                default:
                    if (input::isHotbarAction(event.action)) {
                        gameplay::SwapSlot swap;
                        swap.index = input::hotbarSlot(event.action);
                        runtime.enqueueClientCommand(std::move(swap));
                    }
                    break;
            }
        }
    }

    // Clears the client-side input edges, consumed once they have been folded into
    // a sent MovementInput or discarded when a screen comes up. Replaces the old
    // gameSession.clearInputEdges(), which reached into the session's accumulators.
    void clearPendingInputEdges() {
        pendingJumpPressed_ = false;
        pendingForwardPressed_ = false;
    }

    // The gameSession.player()'s external damage entry: any hit the world deals to the
    // gameSession.player() routes through the shared gameSession.vitals() pipeline, then raises the
    // death screen on the tick it kills — the gameSession.player()'s own onDeath handler.

    // Entity#kill / LivingEntity#kill for the gameSession.player(): OutOfWorld damage at
    // infinite magnitude, the same path /kill <entity> routes a creature through.

    // The mouse callback returns early while a screen is up, so a release that
    // happens behind one never reaches the commands. Every transition into a
    // screen therefore has to end the dig/use explicitly — the interaction state
    // lives in gameplay now, so the abort/stop edges are queued for it.
    void releaseInteractionButtons() {
        destroyButtonHeld = false;
        lastDestroyAimBlock.reset();
        enqueueDestroyAbort();
        enqueueUseStop();
    }

    void respawnPlayer() {
        // D0: the respawn intent travels the channel; the runtime applies it now
        // (the simulation is paused on the death screen, so no tick would drain it)
        // and republishes the fresh spawn snapshot. Then pump it into the mirror
        // so the camera and reads below see the spawn this frame instead of the
        // stale death position a tick later. The client no longer calls
        // GameSession::respawn directly — a cross-process client has no session.
        runtime.sendClientSessionCommand(gameplay::Respawn{});
        runtime.applyClientCommandsNow();
        static_cast<void>(clientMirror_.pump(runtime.clientChannel(), *this));
        camera.setPosition(snapshotCameraEye());
        // PlayerManager#respawnPlayer snaps the new body to the spawn's stored
        // angle (vanilla yaw 0) instead of carrying the death look over. The
        // /tp conversion applies here: vanilla yaw 0 faces +Z.
        camera.setRotation(clientMirror_.world().playerSpawnYaw + 90.0F, 0.0F);
        const auto& worldSnap = clientMirror_.world();
        chunkStreamer.request(world::chunkPositionFromWorld(worldSnap.worldSpawnPosition.x,
                                                            worldSnap.worldSpawnPosition.z));
        setPaused(false);
    }

    void setGameMode(gameplay::GameMode mode) {
        if (uiFrameData_.gameMode == mode) {
            return;
        }
        // D0: the game-mode switch travels the channel and the runtime applies it
        // authoritatively at once (working whether the sim is paused or ticking),
        // republishing so the next mirror pump updates uiFrameData_.gameMode. The
        // client no longer calls GameSession::setGameMode directly.
        runtime.sendClientSessionCommand(gameplay::SetGameMode{mode});
        runtime.applyClientCommandsNow();
        std::cout << "Game mode: " << gameplay::gameModeName(mode) << '\n';
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
        clearPendingInputEdges();
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
            // The command runs server-side on the runtime's dispatcher inside
            // the next tick (it owns the world write section); the result is
            // read back in the frame loop and appended to the history.
            runtime.enqueueChat(chatInputText);
        } else {
            chatHistory.push("<Player> " + chatInputText, true, uiTimeSeconds);
        }
        chatInputText.clear();
        setChatOpen(false);
    }

    // Recomputes the completion list for the token under the (end-of-input)
    // cursor. Called whenever the input changes; Tab cycles without recomputing.
    // Only command lines (a leading '/') are completed: plain chat is not a
    // command, so it gets no suggestions. 1.16.1's ChatScreen builds its
    // CommandSuggestor only for input beginning with '/'. The dispatcher keeps
    // the '/' optional for programmatic callers; the gate lives here at the UI
    // entry so a normal chat message never pops the command completion box.
    void refreshChatSuggestions() {
        if (chatInputText.empty() || chatInputText.front() != '/') {
            chatSuggestions_.clear();
            chatSuggestionIndex_ = 0;
            return;
        }
        chatSuggestions_ =
            runtime.commandDispatcher().suggestions(chatInputText, chatInputText.size());
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

    // The two /tp forms share destination resolution: a Position3 resolves
    // relative axes against the gameSession.player()'s feet and teleports there (applying the
    // optional rotation); a std::string destination is an entity id to teleport
    // onto. `withRotation` marks the `/tp <x> <y> <z> <yaw> <pitch>` form.
    gameplay::CommandResult teleportWithContext(const gameplay::command::CommandContext& context,
                                                bool withRotation) {
        if (const auto position = context.find<gameplay::command::Position3>("destination");
            position.has_value()) {
            // The relative /tp base is the authoritative feet (this teleports the
            // server player), not the lagging client mirror. Resolve `~` axes
            // through the one shared resolve() — the same function the
            // authoritative commands use, so a coordinate never means two things.
            gameplay::command::CommandSource base;
            base.position = gameSession.playerTickSnapshot().physicsCurrent;
            const glm::vec3 target = gameplay::command::resolve(*position, base);
            teleportPlayerTo(target);
            if (withRotation) {
                const auto rotation = context.find<gameplay::command::Rotation2>("rotation");
                if (!rotation.has_value()) {
                    return gameplay::CommandResult{false, "Usage: /tp <x> <y> <z> [<yaw> <pitch>]"};
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
            return gameplay::CommandResult{false,
                                           "Usage: /tp <entity> | <x> <y> <z> [<yaw> <pitch>]"};
        }
        if (withRotation) {
            return gameplay::CommandResult{false, "A rotation can only follow a position"};
        }
        const auto foundEntityId = entityIdById(*entityId);
        if (!foundEntityId.has_value()) {
            return gameplay::CommandResult{false, "No entity found: " + *entityId};
        }
        const auto target = snapshotEntityPosition(*foundEntityId);
        if (!target.has_value()) {
            return gameplay::CommandResult{false, "No entity found: " + *entityId};
        }
        teleportPlayerTo(*target);
        return gameplay::CommandResult{true, "Teleported to the " + *entityId};
    }

    // Entity#teleport without the render interpolation glitch: the gameSession.player(), the
    // physics interpolation endpoints and the camera all snap together, and the
    // chunk streamer recentres so the destination is loaded.
    void teleportPlayerTo(glm::vec3 target) {
        gameSession.teleportPlayer(gameplay::kPrimaryPlayerId, target);
        camera.setPosition(snapshotCameraEye());
        chunkStreamer.request(world::chunkPositionFromWorld(target.x, target.z));
    }

    // Vanilla's /tp yaw is 0 facing +Z; the camera's yaw uses atan2(z, x) with 0
    // facing +X, and vanilla pitch is positive looking down while the camera's
    // is positive looking up. Both convert here.
    void setPlayerLook(const gameplay::command::Rotation2& rotation) {
        camera.setRotation(static_cast<float>(rotation.yaw) + 90.0F,
                           static_cast<float>(-rotation.pitch));
    }

    // The first creature in the published entity snapshot whose registered id
    // (either namespace) matches, as its stable entity id. The render side names
    // entities for commands from what it draws, never from the live vector.
    [[nodiscard]] std::optional<std::uint64_t> entityIdById(std::string_view id) const {
        const auto& snapshot = clientMirror_.entities();
        for (const auto& entity : snapshot.entities()) {
            if (entity.type == nullptr) {
                continue;
            }
            if (entity.type->id().matches(id) || entity.type->vanillaId().matches(id)) {
                return entity.id;
            }
        }
        return std::nullopt;
    }

    // The creature's render position from the published snapshot, by stable id.
    [[nodiscard]] std::optional<glm::vec3> snapshotEntityPosition(std::uint64_t entityId) const {
        const auto& snapshot = clientMirror_.entities();
        for (const auto& entity : snapshot.entities()) {
            if (entity.id == entityId) {
                return entity.position;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::span<const gameplay::ItemStack> activeCreativeCatalog() const {
        if (menuSystem.creativeTab == ui::CreativeTab::Inventory)
            return {};
        return gameplay::creativeCatalog(
            static_cast<gameplay::CreativeCategory>(menuSystem.creativeTab));
    }

    [[nodiscard]] std::size_t creativeMaximumScrollRow() const {
        const std::size_t rowCount = (activeCreativeCatalog().size() + 8U) / 9U;
        return rowCount > 5U ? rowCount - 5U : 0U;
    }

    [[nodiscard]] float creativeScrollPosition() const {
        const std::size_t maximum = creativeMaximumScrollRow();
        return maximum == 0U
                   ? 0.0F
                   : static_cast<float>(menuSystem.creativeScrollRow) / static_cast<float>(maximum);
    }

    void scrollCreative(int rows) {
        const int maximum = static_cast<int>(creativeMaximumScrollRow());
        const int next =
            std::clamp(static_cast<int>(menuSystem.creativeScrollRow) + rows, 0, maximum);
        menuSystem.creativeScrollRow = static_cast<std::size_t>(next);
    }

    void setCreativeTab(ui::CreativeTab tab) {
        menuSystem.creativeTab = tab;
        menuSystem.creativeScrollRow = 0U;
        creativeScrollbarDragging = false;
    }

    void updateCreativeScrollFromCursor() {
        if (uiFrameData_.gameMode != gameplay::GameMode::Creative) {
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
                                   static_cast<float>(framebufferHeight),
                                   menuSystem.guiScaleSetting};
        const auto cursor = currentFramebufferCursor();
        const auto track = layout.creativeScrollbarTrack();
        const float travel = std::max(track.height - 15.0F * layout.scale(), 1.0F);
        const float normalized =
            std::clamp((cursor.y - track.y - 7.5F * layout.scale()) / travel, 0.0F, 1.0F);
        menuSystem.creativeScrollRow =
            static_cast<std::size_t>(std::lround(normalized * static_cast<float>(maximum)));
    }

    void setInventoryOpenLocked(bool open) {
        if (!open) {
            // The full menu close — stow the cursor and crafting grid, close
            // the chest entity and the container — lives on the session.
            gameSession.closeContainerMenu();
        }
        inventoryOpen = open;
        creativeScrollbarDragging = false;
        firstMouseSample = true;
        clearPendingInputEdges();
        releaseInteractionButtons();
        if (open) {
            unlockCursor();
        } else if (!paused && !chatOpen) {
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        }
    }

    void setInventoryOpen(bool open) {
        const auto inventoryWrite = worldLock.write();
        setInventoryOpenLocked(open);
    }

    // Everything the ScreenHandler needs to know about the open screen. The
    // renderer owns which screen that is and where the creative view sits; the
    // slot layout and the click routing that follow from it do not belong here.
    [[nodiscard]] gameplay::ScreenContext screenContext() const {
        const auto snapshot = clientMirror_.world();
        const auto furnace = snapshot.openFurnace.has_value()
                                 ? gameplay::FurnacePosition{snapshot.openFurnace->x,
                                                             snapshot.openFurnace->y,
                                                             snapshot.openFurnace->z}
                                 : gameplay::FurnacePosition{};
        return {snapshot.openContainerScreen, snapshot.openChest, furnace,
                uiFrameData_.gameMode, menuSystem.creativeTab == ui::CreativeTab::Inventory};
    }

    // The eye height the published snapshot implies (the same sneaking-derived
    // rule the per-frame camera uses), so a caller that needs just the height —
    // a stress teleport's feet target, say — reads the snapshot too.
    [[nodiscard]] float snapshotEyeHeight() const {
        return clientMirror_.player().sneaking
                   ? gameplay::PlayerController::kSneakingEyeHeight
                   : gameplay::PlayerController::kEyeHeight;
    }
    // The camera's follow point from the published player snapshot: the physics
    // endpoint plus the eye height. teleportPlayer and respawn mirror their
    // snapped endpoints into the snapshot, so a synchronous teleport is visible
    // the same frame — the camera never reads the live controller.
    [[nodiscard]] glm::vec3 snapshotCameraEye() const {
        const auto& snap = clientMirror_.player();
        return snap.physicsCurrent + glm::vec3{0.0F, snapshotEyeHeight(), 0.0F};
    }

    void setPaused(bool pause) {
        if (!worldSessionActive)
            return;
        if (pause) {
            simulationActive.store(false, std::memory_order_release);
        }
        // Publishing inactive prevents another tick from starting; the write
        // section also waits for a tick already in flight before session state
        // is snapped or menu transitions mutate it.
        const auto pauseWrite = worldLock.write();
        if (pause && inventoryOpen) {
            setInventoryOpenLocked(false);
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
        pressedMenuButton = ui::WidgetId::None;
        menuSystem.viewDistanceSliderDragging = false;
        menuSystem.simulationDistanceSliderDragging = false;
        menuSystem.masterVolumeSliderDragging = false;
        firstMouseSample = true;
        clearPendingInputEdges();
        if (!pause && worldReady) {
            simulationActive.store(true, std::memory_order_release);
        }
        releaseInteractionButtons();
        dropRequested = false;
        // Re-anchor to the *authoritative* position, not the client mirror: this
        // writes the server player, so it must read the session snapshot (which
        // respawn/teleport update synchronously) — the channel mirror lags a
        // tick, and after a respawn it still holds the death position, which would
        // teleport the freshly respawned player back onto its corpse.
        gameSession.teleportPlayer(gameplay::kPrimaryPlayerId,
                                   gameSession.playerTickSnapshot().physicsCurrent);
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

    [[nodiscard]] std::size_t menuButtonCount() const {
        return ui::menuButtonCount(menuSystem.pageStack.current(), currentSave.has_value());
    }

    // Save-screen list rows live in the band between the title and the
    // bottom-anchored function buttons; saveListVisibleRowCount decides how
    // many fit so the rows never collide with the buttons at any resolution.
    [[nodiscard]] ui::UiRect worldListRow(std::size_t index, const ui::HudLayout& layout) const {
        return ui::worldListRow(index, layout, static_cast<float>(swapchainExtent.width));
    }

    // How many world rows fit in the list band at the current canvas size. The
    // list fills the space between the title and the bottom buttons instead of
    // a fixed three rows, mirroring 1.16.1's three-layer save screen.
    [[nodiscard]] std::size_t saveListVisibleRowCount() const {
        return ui::saveListVisibleRowCount(static_cast<float>(swapchainExtent.width),
                                           static_cast<float>(swapchainExtent.height),
                                           menuSystem.guiScaleSetting);
    }

    // The language screen's selection list: a full-width dark box that runs from
    // the left to the right screen edge (the save-selection screen's band look).
    // It is sized to its rows and centred vertically in the space between the
    // title and the grey warning line, so the languages sit absolutely centred
    // rather than packed against the top of a tall box.
    [[nodiscard]] ui::UiRect languageListBox(const ui::HudLayout& layout) const {
        return ui::languageListBox(layout, static_cast<float>(swapchainExtent.width));
    }

    // The grey "(" + options.languageWarning + ")" line below the list, exactly
    // where 1.16.1's LanguageOptionsScreen draws it (between list and buttons).
    [[nodiscard]] float languageWarningY(const ui::HudLayout& layout) const {
        return ui::languageWarningY(layout);
    }

    // One row of the language list inside the full-width dark box.
    [[nodiscard]] ui::UiRect languageRow(std::size_t index, const ui::HudLayout& layout) const {
        return ui::languageRow(index, layout, static_cast<float>(swapchainExtent.width));
    }

    // How many language rows fit inside the black box at the current canvas.
    [[nodiscard]] std::size_t languageVisibleRowCount() const {
        return ui::languageVisibleRowCount(static_cast<float>(swapchainExtent.width),
                                           static_cast<float>(swapchainExtent.height),
                                           menuSystem.guiScaleSetting);
    }

    // Shared button geometry for the frontend pages: the save screen and its
    // edit/delete pages anchor their buttons to the bottom (the world list in
    // two columns), while the title and create screens keep the centred menu.
    [[nodiscard]] ui::UiRect frontendButtonRect(const ui::HudLayout& layout, ui::PageId page,
                                                std::size_t index, std::size_t buttonCount) const {
        return ui::frontendButtonRect(layout, page, index, buttonCount);
    }

    // PX-6: push a system toast (top-right notification). Only used for triggers
    // that already exist (e.g. a setting change) — never a placeholder for an
    // absent system (achievements/recipes have none, so none is pushed).
    void pushSystemToast(std::string title, std::string subtitle) {
        ui::Toast toast;
        toast.kind = ui::ToastKind::System;
        toast.title = std::move(title);
        toast.subtitle = std::move(subtitle);
        toastQueue_.push(std::move(toast));
    }

    // PX-6: show a sound's accessibility caption (SoundRegistry.subtitle) when
    // subtitles are enabled. The client option gates it; until that toggle is
    // wired the feed stays inert (no fake-on captions).
    void showSoundSubtitle(std::string_view subtitle) {
        if (!options.showSubtitles || subtitle.empty()) {
            return;
        }
        subtitleFeed_.show(std::string{subtitle});
    }

    // PX-4: the callback factory — binds every menu action to a renderer method,
    // captured by `this`. buildPage() stamps these onto the page's widgets, so a
    // click runs the same effect the old switch(MenuButton) case did. This is the
    // single seam where Vulkan/save/audio state meets the Vulkan-free ui:: model.
    [[nodiscard]] ui::MenuCallbacks buildMenuCallbacks() {
        ui::MenuCallbacks cb;
        cb.openSingleplayer = [this] {
            refreshSaveList();
            menuSystem.pageStack.push(ui::PageId::WorldList);
        };
        cb.exitGame = [this] { glfwSetWindowShouldClose(window, GLFW_TRUE); };
        cb.playSelectedWorld = [this] {
            if (menuSystem.selectedWorldIndex < menuSystem.saveSummaries.size()) {
                try {
                    startWorld(saveRepository.load(
                        menuSystem.saveSummaries[menuSystem.selectedWorldIndex].identifier));
                } catch (const std::exception& exception) {
                    menuSystem.saveStatus = "Load failed: " + std::string{exception.what()};
                }
            }
        };
        cb.createWorld = [this] {
            menuSystem.createWorldName.clear();
            menuSystem.createWorldGameMode = gameplay::GameMode::Survival;
            menuSystem.pageStack.push(ui::PageId::CreateWorld);
        };
        cb.editWorld = [this] {
            if (menuSystem.selectedWorldIndex < menuSystem.saveSummaries.size()) {
                menuSystem.editWorldIdentifier =
                    menuSystem.saveSummaries[menuSystem.selectedWorldIndex].identifier;
                menuSystem.editWorldName =
                    menuSystem.saveSummaries[menuSystem.selectedWorldIndex].displayName;
                menuSystem.pageStack.push(ui::PageId::EditWorld);
            }
        };
        cb.confirmCreate = [this] { startNewWorld(); };
        cb.toggleCreateGameMode = [this] {
            menuSystem.createWorldGameMode =
                menuSystem.createWorldGameMode == gameplay::GameMode::Survival
                    ? gameplay::GameMode::Creative
                    : gameplay::GameMode::Survival;
        };
        cb.renameWorld = [this] { applyRename(); };
        cb.deleteWorld = [this] { menuSystem.pageStack.push(ui::PageId::ConfirmDelete); };
        cb.confirmDelete = [this] { deleteSelectedWorld(); };
        cb.cancelDelete = [this] { menuSystem.pageStack.pop(); };
        cb.selectWorldRow = [this](std::size_t row) { menuSystem.selectedWorldIndex = row; };

        cb.resume = [this] { setPaused(false); };
        cb.saveAndQuit = [this] { returnToTitle(true); };
        cb.respawn = [this] { respawnPlayer(); };
        cb.returnToTitle = [this] { returnToTitle(true); };

        cb.openOptions = [this] {
            menuSystem.optionsOpen = true;
            menuSystem.pageStack.push(ui::PageId::Options);
        };
        cb.openVideoSettings = [this] { menuSystem.pageStack.push(ui::PageId::VideoSettings); };
        cb.openControls = [this] { menuSystem.pageStack.push(ui::PageId::Controls); };
        cb.openLanguage = [this] {
            menuSystem.pendingLanguageCode = options.language;
            menuSystem.languageStatus.clear();
            menuSystem.pageStack.push(ui::PageId::Language);
        };
        cb.openExperimental = [this] { menuSystem.pageStack.push(ui::PageId::Experimental); };
        cb.doneOptions = [this] {
            if (menuSystem.pageStack.current() == ui::PageId::Language) {
                beginLanguageLoad(menuSystem.pendingLanguageCode);
            }
            menuSystem.pageStack.pop();
            menuSystem.optionsOpen = menuSystem.pageStack.current() == ui::PageId::Options;
        };
        cb.back = [this] { menuSystem.pageStack.pop(); };

        cb.cycleResolution = [this] { cycleResolution(); };
        cb.cycleGuiScale = [this] { cycleGuiScale(); };
        cb.cycleFrameRateLimit = [this] {
            constexpr std::array limits{30, 60, 120, 144, 240, 0};
            const auto found = std::ranges::find(limits, options.frameRateLimit);
            const std::size_t current =
                found == limits.end()
                    ? 0U
                    : static_cast<std::size_t>(std::distance(limits.begin(), found));
            options.frameRateLimit = limits[(current + 1U) % limits.size()];
            persistOptions();
        };
        cb.toggleAntiAliasing = [this] {
            options.antiAliasing = !options.antiAliasing;
            persistOptions();
            recreateSwapchain();
        };
        cb.cycleAnisotropy = [this] {
            options.anisotropy = options.anisotropy >= 16 ? 1 : options.anisotropy * 2;
            persistOptions();
            recreateTextureSampler();
        };
        cb.toggleVsync = [this] {
            options.vsync = !options.vsync;
            persistOptions();
            recreateSwapchain();
        };
        cb.toggleSmoothLighting = [this] {
            options.smoothLightingQuality =
                nextSmoothLightingQuality(options.smoothLightingQuality);
            persistOptions();
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
        };
        cb.toggleDynamicLight = [this] {
            options.dynamicLight = !options.dynamicLight;
            persistOptions();
        };
        cb.toggleViewBobbing = [this] {
            options.viewBobbing = !options.viewBobbing;
            persistOptions();
        };
        cb.toggleAutoJump = [this] {
            options.autoJump = !options.autoJump;
            persistOptions();
        };
        cb.cycleDifficulty = [this] {
            if (currentSave.has_value()) {
                currentSave->difficulty = gameplay::nextDifficulty(currentSave->difficulty);
                gameSession.setDifficulty(currentSave->difficulty);
                // PX-6 system toast: a setting change (a trigger that already
                // exists) confirms to the player via the top-right overlay.
                pushSystemToast("Difficulty",
                                std::string{gameplay::difficultyName(currentSave->difficulty)});
            }
        };
        cb.toggleForceUnicodeFont = [this] {
            options.forceUnicodeFont = !options.forceUnicodeFont;
            textFont.setForceUnicode(options.forceUnicodeFont);
            recreateFontTexture();
            persistOptions();
        };
        // PX-6 Bug3: the sound-subtitles accessibility toggle gates the subtitle
        // overlay feed. Persisted like every other option.
        cb.toggleSubtitles = [this] {
            options.showSubtitles = !options.showSubtitles;
            if (!options.showSubtitles) {
                subtitleFeed_.clear();
            }
            persistOptions();
        };

        cb.cycleRainMode = [this] {
            options.rainMode = (options.rainMode + 1) % 3;
            rainMode_ = static_cast<RainMode>(options.rainMode);
            persistOptions();
        };
        cb.cycleParticleLevel = [this] {
            options.particleLevel = (options.particleLevel + 1) % 4;
            applyParticleLevel();
            persistOptions();
        };
        cb.toggleSunShadows = [this] {
            options.sunShadows = !options.sunShadows;
            shadowDisabled = !options.sunShadows;
            persistOptions();
        };
        cb.toggleRainCollisionCache = [this] {
            options.rainCollisionCache = !options.rainCollisionCache;
            rainSystem.setCollisionCache(options.rainCollisionCache);
            persistOptions();
        };
        cb.selectLanguageRow = [this](std::size_t row) {
            if (row < menuSystem.languageCodes.size()) {
                menuSystem.pendingLanguageCode = menuSystem.languageCodes[row];
                playUiClick();
            }
        };

        // PX-5 Key Binds: clicking a row begins capturing the next key for that
        // action; Reset restores the vanilla defaults. Both act on the PX-1
        // InputSystem single source (via keyBindScreen_), never a private copy.
        cb.beginKeyCapture = [this](input::InputAction action) {
            keyBindScreen_.beginCapture(action);
            playUiClick();
        };
        cb.resetKeyBinds = [this] {
            keyBindScreen_.resetToDefaults();
            persistOptions();
            playUiClick();
        };

        cb.viewDistance.value = [this] {
            return std::clamp((static_cast<float>(viewDistanceChunks) - 2.0F) / 34.0F, 0.0F, 1.0F);
        };
        cb.viewDistance.onDrag = [this](float) { updateViewDistanceFromCursor(); };
        cb.viewDistance.onCommit = [this] {
            updateViewDistanceFromCursor();
            persistOptions();
        };
        cb.simulationDistance.value = [this] {
            return std::clamp((static_cast<float>(simulationDistanceChunks) - 2.0F) / 10.0F, 0.0F,
                              1.0F);
        };
        cb.simulationDistance.onDrag = [this](float) { updateSimulationDistanceFromCursor(); };
        cb.simulationDistance.onCommit = [this] {
            updateSimulationDistanceFromCursor();
            persistOptions();
        };
        cb.masterVolume.value = [this] { return options.masterVolume; };
        cb.masterVolume.onDrag = [this](float) { updateMasterVolumeFromCursor(); };
        cb.masterVolume.onCommit = [this] {
            updateMasterVolumeFromCursor();
            persistOptions();
            if (options.masterVolume > 0.0F) {
                audioSystem.playItemPickup(camera.position());
            }
        };
        return cb;
    }

    // PX-4: the current page's rects come from the existing HudLayout, indexed by
    // widget ordinal — the exact frontendButtonRect contract the old draw used.
    [[nodiscard]] ui::RectProvider menuRectProvider() const {
        const ui::PageId page = menuSystem.pageStack.current();
        const ui::HudLayout layout{static_cast<float>(swapchainExtent.width),
                                   static_cast<float>(swapchainExtent.height),
                                   menuSystem.guiScaleSetting};
        const std::size_t count = menuButtonCount();
        const float fbWidth = static_cast<float>(swapchainExtent.width);
        // PX-6 Bug1: on Controls, the first `keyRows` widget indices are scrolling
        // key-bind list rows (controlsRow); the trailing four are the bottom
        // button band. Elsewhere every widget is a frontend button.
        const std::size_t keyRows =
            page == ui::PageId::Controls ? controlsVisibleKeyBindRowCount() : 0U;
        return [layout, page, count, fbWidth, keyRows](std::size_t index) {
            if (page == ui::PageId::Controls && index < keyRows) {
                return ui::controlsRow(index, layout, fbWidth);
            }
            const std::size_t buttonIndex = page == ui::PageId::Controls ? index - keyRows : index;
            return ui::frontendButtonRect(layout, page, buttonIndex, count);
        };
    }

    // PX-6 Bug1: how many key-bind rows are visible on the Controls list this
    // frame — the visible window, clamped to the number of rebindable actions.
    [[nodiscard]] std::size_t controlsVisibleKeyBindRowCount() const {
        const std::size_t total = input::keyBindRows().size();
        const std::size_t window = ui::controlsVisibleRowCount(
            static_cast<float>(swapchainExtent.width),
            static_cast<float>(swapchainExtent.height), menuSystem.guiScaleSetting);
        const std::size_t first = std::min(menuSystem.controlsListFirstIndex, total);
        return std::min(window, total - first);
    }

    // PX-4: the widget index under the cursor on the given page (kNoWidget if
    // none) — the model's hitTest against the page's laid-out rects.
    [[nodiscard]] std::size_t hoveredMenuIndex(const ui::Page& page) const {
        const auto cursor = currentFramebufferCursor();
        return ui::hitTest(page, cursor.x, cursor.y);
    }

    // PX-4: assemble the current page as a ui::Page value — the single build point.
    [[nodiscard]] ui::Page buildCurrentPage() {
        ui::MenuBuildContext ctx;
        ctx.worldOpen = currentSave.has_value();
        ctx.worldSelectable = !menuSystem.saveSummaries.empty();
        ctx.worldRowCount = 0;       // list rows are drawn by the list path today
        ctx.languageRowCount = 0;
        // PX-6 Bug1: the Controls key-bind list is scrolling — build only the
        // visible window so the page never exceeds the layout button cap.
        if (menuSystem.pageStack.current() == ui::PageId::Controls) {
            ctx.keyBindFirstIndex = menuSystem.controlsListFirstIndex;
            ctx.keyBindRowCount = controlsVisibleKeyBindRowCount();
        }
        // PX-5 Key Binds: each row's label is "Action: Key" from the InputSystem
        // single source, or "Action: > ? <" while that row is capturing.
        ctx.keyBindLabelFor = [this](input::InputAction action) -> std::string {
            const std::string name{input::actionDisplayName(action)};
            if (keyBindScreen_.capturing() && keyBindScreen_.capturingAction() == action) {
                return name + ": > ? <";
            }
            return name + ": " + input::bindingDisplayName(inputSystem_.bindings().binding(action));
        };
        return ui::buildPage(menuSystem.pageStack.current(), ctx, buildMenuCallbacks(),
                             menuRectProvider());
    }

    void handleMenuButtonPress() {
        if (menuSystem.pageStack.current() == ui::PageId::WorldList) {
            const auto cursor = currentFramebufferCursor();
            const ui::HudLayout layout{static_cast<float>(swapchainExtent.width),
                                       static_cast<float>(swapchainExtent.height),
                                       menuSystem.guiScaleSetting};
            const std::size_t visibleRows = saveListVisibleRowCount();
            const std::size_t maximumFirst = menuSystem.saveSummaries.size() > visibleRows
                                                 ? menuSystem.saveSummaries.size() - visibleRows
                                                 : 0U;
            const std::size_t first = std::min(menuSystem.worldListFirstIndex, maximumFirst);
            const std::size_t visible = std::min<std::size_t>(
                menuSystem.saveSummaries.size() - std::min(first, menuSystem.saveSummaries.size()),
                visibleRows);
            for (std::size_t index = 0; index < visible; ++index) {
                if (worldListRow(index, layout).contains(cursor.x, cursor.y)) {
                    menuSystem.selectedWorldIndex = first + index;
                    break;
                }
            }
        }
        // PX-4: the pressed widget is resolved through the ui:: model (index into
        // the current page); pressedMenuButton is kept only as the draw/debug id.
        const ui::Page page = buildCurrentPage();
        pressedMenuIndex_ = hoveredMenuIndex(page);
        // The pressed widget's stable id drives the draw highlight (which widget
        // paints pressed); the index drives dispatch. None when the press missed.
        pressedMenuButton = pressedMenuIndex_ != ui::kNoWidget
                                ? static_cast<ui::WidgetId>(page[pressedMenuIndex_].debugId)
                                : ui::WidgetId::None;
        // A press on a Slider starts its drag; the drag effect runs through the
        // slider's onDrag callback (never a traversal side effect). The dragging
        // flags stay so the release path and draw highlight keep working.
        if (pressedMenuIndex_ != ui::kNoWidget &&
            page[pressedMenuIndex_].kind == ui::WidgetKind::Slider) {
            const auto& slider = page[pressedMenuIndex_].slider;
            switch (static_cast<ui::WidgetId>(page[pressedMenuIndex_].debugId)) {
                case ui::WidgetId::ViewDistance:
                    menuSystem.viewDistanceSliderDragging = true;
                    break;
                case ui::WidgetId::SimulationDistance:
                    menuSystem.simulationDistanceSliderDragging = true;
                    break;
                case ui::WidgetId::MasterVolume:
                    menuSystem.masterVolumeSliderDragging = true;
                    break;
                default:
                    break;
            }
            if (slider.onDrag) {
                slider.onDrag(0.0F);  // the appliers read the cursor themselves
            }
        }
        // 26.1 keeps a draft selection while the screen is open. Done commits
        // it through one resource reload, so browsing several rows does not
        // repeatedly parse translations and rebuild fonts.
        if (menuSystem.pageStack.current() == ui::PageId::Language) {
            const auto cursor = currentFramebufferCursor();
            const ui::HudLayout layout{static_cast<float>(swapchainExtent.width),
                                       static_cast<float>(swapchainExtent.height),
                                       menuSystem.guiScaleSetting};
            const std::size_t visible = languageVisibleRowCount();
            const std::size_t maximumFirst = menuSystem.languageCodes.size() > visible
                                                 ? menuSystem.languageCodes.size() - visible
                                                 : 0U;
            const std::size_t first = std::min(menuSystem.languageListFirstIndex, maximumFirst);
            const auto scrollTrack = ui::languageScrollbarTrack(
                layout, static_cast<float>(swapchainExtent.width));
            if (menuSystem.languageCodes.size() > visible &&
                scrollTrack.contains(cursor.x, cursor.y)) {
                menuSystem.languageScrollbarDragging = true;
                pressedMenuButton = ui::WidgetId::None;
                updateLanguageScrollFromCursor();
                return;
            }
            for (std::size_t row = 0; row < visible; ++row) {
                const std::size_t index = first + row;
                if (index >= menuSystem.languageCodes.size()) {
                    break;
                }
                if (languageRow(row, layout).contains(cursor.x, cursor.y)) {
                    menuSystem.pendingLanguageCode = menuSystem.languageCodes[index];
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
                                   static_cast<float>(swapchainExtent.height),
                                   menuSystem.guiScaleSetting};
        const auto slider =
            frontendButtonRect(layout, menuSystem.pageStack.current(), 2U, menuButtonCount());
        const float inset = 4.0F * layout.scale();
        const float travel = std::max(slider.width - inset * 2.0F, 1.0F);
        const float normalized = std::clamp((cursor.x - slider.x - inset) / travel, 0.0F, 1.0F);
        const int requested =
            std::clamp(2 + static_cast<int>(std::lround(normalized * 34.0F)), 2, 36);
        if (requested == viewDistanceChunks) {
            return;
        }
        viewDistanceChunks = requested;
        chunkStreamer.setRadii(viewDistanceChunks,
                               viewDistanceChunks + world::kUnloadHysteresisChunks);
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
                                   static_cast<float>(swapchainExtent.height),
                                   menuSystem.guiScaleSetting};
        const auto slider =
            frontendButtonRect(layout, menuSystem.pageStack.current(), 3U, menuButtonCount());
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
                                   static_cast<float>(swapchainExtent.height),
                                   menuSystem.guiScaleSetting};
        const auto slider = layout.menuButton(0U, 3U);
        const float inset = 4.0F * layout.scale();
        const float travel = std::max(slider.width - inset * 2.0F, 1.0F);
        options.masterVolume = std::clamp((cursor.x - slider.x - inset) / travel, 0.0F, 1.0F);
        audioSystem.setMasterVolume(options.masterVolume);
    }

    void markWindowPlacementDirty() noexcept {
        windowPlacementDirty = true;
        windowPlacementChangedAt = std::chrono::steady_clock::now();
    }

    void noteWindowSizeChanged(int width, int height) noexcept {
        // GLFW delivers maximize/restore before the accompanying size event on
        // both Cocoa and Win32. This therefore captures even a quick resize-then-
        // maximize sequence without ever recording the maximized client size.
        if (width > 0 && height > 0 &&
            glfwGetWindowAttrib(window, GLFW_MAXIMIZED) != GLFW_TRUE) {
            options.windowWidth = width;
            options.windowHeight = height;
        }
        markWindowPlacementDirty();
    }

    void noteWindowMaximizeChanged(bool maximized) noexcept {
        options.windowMaximized = maximized;
        markWindowPlacementDirty();
    }

    void captureWindowPlacement() noexcept {
        if (window == nullptr) {
            return;
        }
        const bool maximized = glfwGetWindowAttrib(window, GLFW_MAXIMIZED) == GLFW_TRUE;
        options.windowMaximized = maximized;
        // A maximized window reports the monitor-sized client area. Keep the
        // last ordinary window size so restoring after this or the next launch
        // returns to the user's actual windowed dimensions.
        if (!maximized && glfwGetWindowAttrib(window, GLFW_ICONIFIED) != GLFW_TRUE) {
            int width = 0;
            int height = 0;
            glfwGetWindowSize(window, &width, &height);
            if (width > 0 && height > 0) {
                options.windowWidth = width;
                options.windowHeight = height;
            }
        }
    }

    void persistWindowPlacementIfSettled() noexcept {
        constexpr auto kWindowPlacementSettleTime = std::chrono::milliseconds{250};
        if (!windowPlacementDirty ||
            std::chrono::steady_clock::now() - windowPlacementChangedAt <
                kWindowPlacementSettleTime) {
            return;
        }
        windowPlacementDirty = false;
        captureWindowPlacement();
        persistOptions();
    }

    void persistOptions() noexcept {
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
        menuSystem.resolutionIndex =
            (menuSystem.resolutionIndex + 1U) % ui::kDisplayResolutions.size();
        const auto resolution = ui::kDisplayResolutions[menuSystem.resolutionIndex];
        // On macOS a maximized window keeps its maximized flag when the client
        // size is set directly, so the requested size never lands. Restore the
        // window first, then apply the new resolution as a plain windowed size.
        if (glfwGetWindowAttrib(window, GLFW_MAXIMIZED) == GLFW_TRUE) {
            glfwRestoreWindow(window);
        }
        options.windowMaximized = false;
        options.windowWidth = resolution.width;
        options.windowHeight = resolution.height;
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
        if (menuSystem.languageScrollbarDragging) {
            updateLanguageScrollFromCursor();
        }
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
        // PX-4: dispatch through the ui:: model. The pressed widget index was
        // captured on press; if the release lands on the same enabled widget its
        // onActivate runs the effect the old switch(MenuButton) case did.
        const ui::Page page = buildCurrentPage();
        const std::size_t released = hoveredMenuIndex(page);
        const std::size_t pressed = pressedMenuIndex_;
        pressedMenuButton = ui::WidgetId::None;
        pressedMenuIndex_ = ui::kNoWidget;
        menuSystem.viewDistanceSliderDragging = false;
        menuSystem.simulationDistanceSliderDragging = false;
        menuSystem.masterVolumeSliderDragging = false;
        menuSystem.languageScrollbarDragging = false;
        // A slider release commits (persist + feedback) through its callback.
        if (pressed != ui::kNoWidget && page[pressed].kind == ui::WidgetKind::Slider &&
            page[pressed].slider.onCommit) {
            page[pressed].slider.onCommit();
            return;
        }
        // A button release over the same widget clicks and runs its onActivate.
        if (released != ui::kNoWidget && released == pressed) {
            playUiClick();
        }
        const auto cursor = currentFramebufferCursor();
        static_cast<void>(ui::dispatchActivate(page, pressed, cursor.x, cursor.y));
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
                                   static_cast<float>(framebufferHeight),
                                   menuSystem.guiScaleSetting};
        // Inventory and container slots are silent in vanilla: only actual
        // buttons (AbstractButtonWidget) play ui.button.click, so picking up
        // or moving an item never clicks. The menu buttons above are the only
        // sound in this screen family.
        //
        // One slot list, routed by what the slot is. This used to be a chain of
        // `containerScreen ==` branches per screen, repeated for every question
        // the screen was asked.
        const auto slots = gameplay::ScreenHandler::buildSlotLayout(screenContext(), layout);
        if (const auto* slot = gameplay::ScreenHandler::slotAt(slots, framebufferCursor);
            slot != nullptr) {
            // Command-ized: the click executes on the server tick through the
            // interaction, which resolves the storage and routes it by slot kind.
            gameplay::ClickSlot click;
            click.kind = slot->kind;
            click.slotIndex = slot->index;
            click.button = static_cast<int>(button);
            click.shiftHeld = shiftHeld;
            runtime.enqueueClientCommand(std::move(click));
            return;
        }
        if (clientMirror_.world().openContainerScreen !=
            ContainerScreen::PlayerInventory) {
            // Clicking outside every slot of an open container throws the held
            // stack on the floor, but only outside the panel itself.
            if (!layout.inventoryPanel().contains(framebufferCursor.x, framebufferCursor.y)) {
                gameplay::DropCursor drop;
                drop.lookDirection = camera.direction();
                runtime.enqueueClientCommand(std::move(drop));
            }
            return;
        }
        if (uiFrameData_.gameMode == gameplay::GameMode::Creative) {
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
                // The 36 inventory slots and the hotbar are real PlayerInventory
                // slots routed by the slot hit-test above; only the delete box
                // and the space outside the panel remain here.
                if (layout.creativeDeleteSlot().contains(framebufferCursor.x,
                                                         framebufferCursor.y)) {
                    runtime.enqueueClientCommand(gameplay::ClearCursor{});
                    return;
                }
                if (!layout.creativePanel().contains(framebufferCursor.x, framebufferCursor.y)) {
                    gameplay::DropCursor drop;
                    drop.lookDirection = camera.direction();
                    runtime.enqueueClientCommand(std::move(drop));
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
                        runtime.enqueueClientCommand(gameplay::ClearCursor{});
                        return;
                    }
                    gameplay::ClickCreativeItem click;
                    click.catalogStack = catalog[catalogIndex];
                    click.button = button;
                    click.shiftHeld = shiftHeld;
                    runtime.enqueueClientCommand(std::move(click));
                    return;
                }
            }
            // The hotbar is routed by the slot hit-test above; only the space
            // outside the panel throws the cursor stack.
            if (!layout.creativePanel().contains(framebufferCursor.x, framebufferCursor.y)) {
                gameplay::DropCursor drop;
                drop.lookDirection = camera.direction();
                runtime.enqueueClientCommand(std::move(drop));
            }
            return;
        }
        // Survival: the 36 inventory slots are routed by the slot hit-test; a
        // click outside the panel throws the cursor stack.
        if (!layout.inventoryPanel().contains(framebufferCursor.x, framebufferCursor.y)) {
            gameplay::DropCursor drop;
            drop.lookDirection = camera.direction();
            runtime.enqueueClientCommand(std::move(drop));
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
        std::optional<gameplay::SlotRef> clickedSlot = slotUnderCursor();
        isDoubleClicking = button == gameplay::InventoryMouseButton::Left &&
                           clickedSlot.has_value() && clickedSlot == lastClickedSlot &&
                           now - lastClickTime < 0.25;
        lastClickedSlot = clickedSlot;
        lastClickTime = now;
        if (clientMirror_.world().cursorStack.empty() ||
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
        if (uiFrameData_.gameMode != gameplay::GameMode::Creative ||
            clientMirror_.world().openContainerScreen !=
                ContainerScreen::PlayerInventory) {
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

    // The HUD layout for the current framebuffer. Slot geometry belongs to the
    // screen, so anything that asks the ScreenHandler for slots needs one.
    [[nodiscard]] ui::HudLayout currentHudLayout() const {
        int framebufferWidth = 0;
        int framebufferHeight = 0;
        glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
        return ui::HudLayout{static_cast<float>(framebufferWidth),
                             static_cast<float>(framebufferHeight), menuSystem.guiScaleSetting};
    }

    // The slot under the mouse as a (kind, index) value, for double-click
    // tracking — never a storage pointer into gameplay.
    [[nodiscard]] std::optional<gameplay::SlotRef> slotUnderCursor() {
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
            return std::nullopt;
        }
        const auto cursor = ui::windowToFramebuffer(cursorX, cursorY, windowWidth, windowHeight,
                                                    framebufferWidth, framebufferHeight);
        const ui::HudLayout layout{static_cast<float>(framebufferWidth),
                                   static_cast<float>(framebufferHeight),
                                   menuSystem.guiScaleSetting};
        return dragSlotAt(layout, cursor);
    }

    // Every slot the player can reach in the current screen, for the PICKUP_ALL
    // gather: the container's input slots plus the whole player inventory. The
    // output slots are excluded because they never hold storage of their own.
    [[nodiscard]] std::vector<gameplay::SlotRef> allScreenSlots() const {
        const auto layout = currentHudLayout();
        std::vector<gameplay::SlotRef> refs;
        for (const auto& slot : gameplay::ScreenHandler::buildSlotLayout(screenContext(),
                                                                         layout)) {
            if (slot.acceptsItems()) {
                refs.push_back({slot.kind, slot.index});
            }
        }
        return refs;
    }

    // The slot under the cursor as a (kind, index) value, or nullopt when there
    // is none (a blank area, an output slot that cannot take items, or a
    // creative tab). QUICK_CRAFT collects these.
    [[nodiscard]] std::optional<gameplay::SlotRef> dragSlotAt(const ui::HudLayout& layout,
                                                              const ui::UiPoint& cursor) const {
        const auto slots = gameplay::ScreenHandler::buildSlotLayout(screenContext(), layout);
        const auto* slot = gameplay::ScreenHandler::slotAt(slots, cursor);
        if (slot == nullptr || !slot->acceptsItems()) {
            return std::nullopt;
        }
        return gameplay::SlotRef{slot->kind, slot->index};
    }

    // The on-screen rectangle of a slot the cursor swept during a QUICK_CRAFT
    // drag, or nullopt when the pointer no longer belongs to the current screen
    // (a closed container, for example). The drag's (kind, index) identity is
    // resolved back to its geometry, so the preview always lands on the slot
    // the drag would write.
    [[nodiscard]] std::optional<ui::UiRect> dragSlotRectangle(const ui::HudLayout& layout,
                                                              const gameplay::SlotRef& ref) const {
        const auto slots = gameplay::ScreenHandler::buildSlotLayout(screenContext(), layout);
        for (const auto& slot : slots) {
            if (slot.kind == ref.kind && slot.index == ref.index) {
                return slot.rect;
            }
        }
        return std::nullopt;
    }

    // The snapshot's current stack for a slot, so the drag preview and the
    // placement counts read the same published display state the HUD draws.
    [[nodiscard]] gameplay::ItemStack
    snapshotStackAt(gameplay::SlotKind kind, std::uint16_t index) const {
        // The world snapshot is a by-value copy, so the stack is returned by
        // value rather than as a reference into the copy.
        const auto snap = clientMirror_.world();
        switch (kind) {
        case gameplay::SlotKind::PlayerInventory:
            return snap.inventorySlots[index];
        case gameplay::SlotKind::ChestStorage:
            return snap.chestItems[index];
        case gameplay::SlotKind::TableCraftingGrid:
            return snap.tableCraftingGrid[index];
        case gameplay::SlotKind::PlayerCraftingGrid:
            return snap.playerCraftingGrid[index];
        case gameplay::SlotKind::FurnaceInput:
            return snap.furnaceInput;
        case gameplay::SlotKind::FurnaceFuel:
            return snap.furnaceFuel;
        case gameplay::SlotKind::FurnaceOutput:
            return snap.furnaceOutput;
        case gameplay::SlotKind::PlayerCraftingOutput:
        case gameplay::SlotKind::TableCraftingOutput:
            // Output slots are not drag targets (acceptsItems is false), so the
            // preview never asks for one; return a shared empty anyway.
            break;
        }
        static const gameplay::ItemStack kEmptyPreviewStack;
        return kEmptyPreviewStack;
    }

    // The amount the ongoing drag would place in each collected slot, mirroring
    // Inventory::dragDistribute exactly: a left drag shares the cursor stack as
    // evenly as the accepting slots allow, a right drag drops one item per slot.
    // Zero marks a collected slot that cannot take the dragged item (its stack
    // is full or a different item), which the preview skips just like the real
    // distribution does. Reads the container display snapshot, never live slots.
    [[nodiscard]] std::vector<std::uint8_t> dragPlacementCounts() const {
        std::vector<std::uint8_t> counts(inventoryDragSlots.size(), 0U);
        const auto& cursor = clientMirror_.world().cursorStack;
        if (cursor.empty() || inventoryDragSlots.empty()) {
            return counts;
        }
        const auto accepts = [&](const gameplay::ItemStack& target) {
            return target.empty() ||
                   (gameplay::sameItem(target, cursor) &&
                    target.count < gameplay::itemMaximumStackSize(target));
        };
        if (inventoryDragButton == gameplay::InventoryMouseButton::Right) {
            for (std::size_t index = 0; index < inventoryDragSlots.size(); ++index) {
                if (accepts(snapshotStackAt(inventoryDragSlots[index].kind,
                                            inventoryDragSlots[index].index))) {
                    counts[index] = 1U;
                }
            }
            return counts;
        }
        std::size_t fillable = 0;
        for (const auto& ref : inventoryDragSlots) {
            if (accepts(snapshotStackAt(ref.kind, ref.index)))
                ++fillable;
        }
        if (fillable == 0U) {
            return counts;
        }
        const auto maximum = gameplay::itemMaximumStackSize(cursor);
        std::uint8_t perSlot = static_cast<std::uint8_t>(cursor.count / fillable);
        std::uint8_t extra = static_cast<std::uint8_t>(cursor.count % fillable);
        for (std::size_t index = 0; index < inventoryDragSlots.size(); ++index) {
            auto target = snapshotStackAt(inventoryDragSlots[index].kind,
                                          inventoryDragSlots[index].index);
            if (!accepts(target))
                continue;
            std::uint8_t amount = perSlot;
            if (extra > 0U) {
                ++amount;
                --extra;
            }
            amount = std::min(amount, static_cast<std::uint8_t>(maximum - target.count));
            counts[index] = amount;
        }
        return counts;
    }

    // Draws the would-be placement of an in-progress QUICK_CRAFT drag in the
    // slots the cursor has swept, so the destination is visible before the
    // button is released. Vanilla's HandledScreen.drawSlot tints the swept slot
    // dark and paints a copy of the cursor stack with the amount that would land
    // there; this reproduces that preview on top of the already-drawn slots.
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
            gameplay::PickupAll pickup;
            pickup.targets = allScreenSlots();
            runtime.enqueueClientCommand(std::move(pickup));
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
            // stack across them (left = evenly, right = one per slot), on the
            // server tick.
            gameplay::DragDistribute drag;
            drag.button = inventoryDragButton;
            drag.targets = std::move(inventoryDragSlots);
            runtime.enqueueClientCommand(std::move(drag));
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
        const auto cursor = ui::windowToFramebuffer(windowX, windowY, windowWidth, windowHeight,
                                                    framebufferWidth, framebufferHeight);
        const ui::HudLayout layout{static_cast<float>(framebufferWidth),
                                   static_cast<float>(framebufferHeight),
                                   menuSystem.guiScaleSetting};
        const auto slot = dragSlotAt(layout, cursor);
        if (!slot.has_value()) {
            return;
        }
        if (static_cast<std::size_t>(clientMirror_.world().cursorStack.count) <=
            inventoryDragSlots.size()) {
            return;
        }
        if (std::ranges::find(inventoryDragSlots, *slot) == inventoryDragSlots.end()) {
            inventoryDragSlots.push_back(*slot);
        }
    }

    void shutdown() noexcept {
        // Before anything the tick touches is torn down. jthread would join on
        // destruction anyway, but that happens after the Vulkan teardown below.
        runtime.stopSimulation();
        if (window != nullptr) {
            captureWindowPlacement();
            persistOptions();
        }
        if (device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device);
            cleanupSwapchain();
            if (descriptorPool != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(device, descriptorPool, nullptr);
            }
            textures_.destroy(allocator != VK_NULL_HANDLE);
            if (allocator != VK_NULL_HANDLE) {
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
            for (auto& queryPool : occlusionQueryPools) {
                if (queryPool != VK_NULL_HANDLE) {
                    vkDestroyQueryPool(device, queryPool, nullptr);
                    queryPool = VK_NULL_HANDLE;
                }
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
        }
        vulkanDevice_.destroy();
        if (window != nullptr) {
            glfwDestroyWindow(window);
            window = nullptr;
        }
        if (glfwInitialized) {
            glfwTerminate();
            glfwInitialized = false;
        }
    }

    [[nodiscard]] AllocatedBuffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                               bool hostVisible) const {
        return resources_.createBuffer(size, usage, hostVisible);
    }

    [[nodiscard]] AllocatedImage
    createImage(std::uint32_t width, std::uint32_t height, std::uint32_t layers, VkFormat format,
                VkImageUsageFlags usage,
                VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT) const {
        return resources_.createImage(width, height, layers, format, usage, samples);
    }

    void destroyBuffer(AllocatedBuffer& buffer) const noexcept { resources_.destroyBuffer(buffer); }

    void destroyImage(AllocatedImage& image) const noexcept { resources_.destroyImage(image); }

    // Forwarders to VulkanResources, which owns the command pool + graphics
    // queue these need; the call sites here and in the texture cluster stay
    // unchanged, matching the createBuffer/createImage forwarders above.
    [[nodiscard]] VkCommandBuffer beginSingleUseCommands() const {
        return resources_.beginSingleUseCommands();
    }

    void endSingleUseCommands(VkCommandBuffer commandBuffer) const {
        resources_.endSingleUseCommands(commandBuffer);
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
                    for (int y = world::kMaxY - 3; y >= world::kMinY + 1; --y) {
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
                        for (int above = y + 3; above < world::kMaxY; ++above) {
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
            if (!interactionWorld.hasChunk(world::chunkPositionFromWorld(24.0F, 24.0F))) {
                return;
            }
            for (int y = world::kMaxY - 3; y >= world::kMinY + 1; --y) {
                const auto ground = interactionWorld.block(24, y, 24);
                if (world::hasCollision(ground) || world::isFluid(ground)) {
                    best = glm::ivec3{24, y + 1, 24};
                    break;
                }
            }
            if (!best.has_value()) {
                best = glm::ivec3{24, world::kMinY + 1, 24};
            }
        }
        const glm::vec3 feet{static_cast<float>(best->x) + 0.5F,
                             static_cast<float>(best->y) + 0.001F,
                             static_cast<float>(best->z) + 0.5F};
        gameSession.teleportPlayer(gameplay::kPrimaryPlayerId, feet);
        gameSession.setWorldSpawn(feet);
        camera.setPosition(snapshotCameraEye());
        spawnPositionInitialized = true;
        // Vanilla keeps its spawn chunks loaded for the server's lifetime; mark
        // the world spawn's chunk neighbourhood so it never streams out under
        // the player (ServerChunkManager#updateChunks).
        chunkStreamer.protectChunks(world::chunkPositionFromWorld(feet.x, feet.z),
                                    kSpawnChunkRadius);
        std::cout << "Spawn position: " << feet.x << "," << feet.y << "," << feet.z << '\n';
    }

    void playUiClick() { audioSystem.playButtonClick(camera.position()); }

    // AU-2: feed one tick of the ambient/music scheduler. Builds the situational
    // context (menu vs overworld, creative vs game) and, when a world is loaded,
    // a cave-mood brightness sample at a random block near the eye — the input
    // BiomeAmbientSoundsHandler samples each tick. All scheduling/threshold logic
    // lives in the audio system; this only gathers render-side context. Biome
    // ambient loops are left empty until WG lands the nether/cave biomes that own
    // them (记账).
    void driveAmbientMusic(float deltaSeconds) {
        audio::AudioSystem::AmbientMusicContext context;
        context.listenerPosition = camera.position();
        // The scheduler counts in 20-tps game-ticks, not frames. Absorb this
        // frame's real time and pass the whole ticks it crossed (0 on most frames,
        // >1 only on a long frame), so song delays and the cave-mood rate stay
        // FPS-decoupled instead of racing at the render frame rate.
        context.ticks = ambientMusicTicks_.advance(deltaSeconds);
        if (!worldSessionActive) {
            context.situation = audio::MusicSituation::Menu;
        } else {
            context.situation = uiFrameData_.gameMode == gameplay::GameMode::Creative
                                    ? audio::MusicSituation::Creative
                                    : audio::MusicSituation::Game;
            // A random block within the mood search extent (8) of the eye. The
            // per-frame LCG keeps the sampling cheap and needs no world RNG.
            ambientMusicRandom_ = ambientMusicRandom_ * 1664525U + 1013904223U;
            const auto roll = [this](int span) {
                ambientMusicRandom_ = ambientMusicRandom_ * 1664525U + 1013904223U;
                return static_cast<int>((ambientMusicRandom_ >> 8) % static_cast<std::uint32_t>(span)) -
                       (span / 2);
            };
            constexpr int kExtent = 8;
            constexpr int kSpan = kExtent * 2 + 1;
            const glm::vec3 eye = camera.position();
            const int bx = static_cast<int>(std::floor(eye.x)) + roll(kSpan);
            const int by = static_cast<int>(std::floor(eye.y)) + roll(kSpan);
            const int bz = static_cast<int>(std::floor(eye.z)) + roll(kSpan);
            const auto light = world_.skyBlockLightAt(bx, by, bz);
            audio::MoodSample sample;
            sample.offsetX = (static_cast<double>(bx) + 0.5) - static_cast<double>(eye.x);
            sample.offsetY = (static_cast<double>(by) + 0.5) - static_cast<double>(eye.y);
            sample.offsetZ = (static_cast<double>(bz) + 0.5) - static_cast<double>(eye.z);
            sample.skyBrightness = light.sky;
            sample.blockBrightness = light.block;
            context.moodSample = sample;
        }
        audioSystem.tickAmbientMusic(context);
    }
    std::uint32_t ambientMusicRandom_ = 0x1F123BB5U;
    // Fixed-step accumulator that turns frame time into whole 20-tps ticks for the
    // ambient/music scheduler (FPS-decoupled), mirroring SimulationDriver.
    audio::TickAccumulator ambientMusicTicks_;

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
    // The render thread's half of the interaction: package the aim target into
    // value-type commands for the gameplay controller (which runs inside the
    // server tick). Nothing here mutates the world.
    void enqueueInteractionCommand(gameplay::GameCommand command) {
        // The abort/stop edges are always safe and always wanted (a release
        // behind a screen must end the dig/use); the start edges are guarded by
        // the caller so a press during a pause or a menu never queues stale.
        if (worldSessionActive) {
            runtime.enqueueClientCommand(std::move(command));
        }
    }

    // Minecraft#doAttack, packaged: press carries the aimed creature or block,
    // release ends the dig. The server decides and ticks it.
    void enqueueDestroyStart() {
        if (!(worldReady && !paused && !inventoryOpen && !chatOpen)) {
            return;
        }
        gameplay::PlayerAction action;
        action.kind = gameplay::PlayerAction::Kind::StartDestroy;
        if (creatureHit.has_value()) {
            action.entity = true;
            action.entityId = creatureHit->entityId;
            lastDestroyAimBlock.reset();
        } else if (targetedBlock.has_value()) {
            action.block = targetedBlock->block;
            lastDestroyAimBlock = targetedBlock->block;
        } else {
            lastDestroyAimBlock.reset();
        }
        enqueueInteractionCommand(std::move(action));
    }
    void enqueueDestroyAbort() {
        gameplay::PlayerAction action;
        action.kind = gameplay::PlayerAction::Kind::AbortDestroy;
        enqueueInteractionCommand(std::move(action));
    }

    // A held attack follows the live ray target. The press starts the first
    // block; after it disappears (or the player looks elsewhere), the next
    // frame sends one new StartDestroy for the newly reached cell. Without this
    // hand-off the gameplay controller kept digging the first cell's Air state,
    // so even instant blocks such as grass required another click.
    void refreshHeldDestroyTarget() {
        if (!destroyButtonHeld || paused || inventoryOpen || chatOpen || !worldReady) {
            return;
        }
        const std::optional<glm::ivec3> aimedBlock =
            creatureHit.has_value() || !targetedBlock.has_value()
                ? std::nullopt
                : std::optional<glm::ivec3>{targetedBlock->block};
        if (aimedBlock == lastDestroyAimBlock) {
            return;
        }
        if (aimedBlock.has_value()) {
            gameplay::PlayerAction action;
            action.kind = gameplay::PlayerAction::Kind::StartDestroy;
            action.block = *aimedBlock;
            enqueueInteractionCommand(std::move(action));
        } else {
            gameplay::PlayerAction action;
            action.kind = gameplay::PlayerAction::Kind::StopDestroy;
            enqueueInteractionCommand(std::move(action));
        }
        lastDestroyAimBlock = aimedBlock;
    }

    // Minecraft#startUseItem, packaged: the right-click carries the block target
    // (or none for an air use), and the release ends a held use.
    void enqueueUseStart() {
        if (!(worldReady && !paused && !inventoryOpen && !chatOpen)) {
            return;
        }
        if (targetedBlock.has_value()) {
            gameplay::UseItemOn use;
            use.block = targetedBlock->block;
            use.adjacent = targetedBlock->adjacent;
            use.face = world::orientationFromOffset(targetedBlock->adjacent -
                                                    targetedBlock->block);
            // The precise point on the block's shape the ray struck, so placement
            // can read the sub-cell hit height (SlabBlock#getStateForPlacement
            // rests a slab on the half the player aimed at). The cell centre it
            // used to carry threw that fraction away.
            use.hitPosition =
                camera.position() + camera.direction() * targetedBlock->distance;
            use.lookDirection = camera.direction();
            enqueueInteractionCommand(std::move(use));
        } else {
            gameplay::UseItem use;
            enqueueInteractionCommand(std::move(use));
        }
    }
    void enqueueUseStop() {
        gameplay::UseItemStop stop;
        enqueueInteractionCommand(std::move(stop));
    }

    // The per-frame aim target, a pure read: raycast the world and the creature
    // herd from the camera. The interaction controller receives the result
    // through the commands; the draw pass reads it for the selection outline.
    void updateInteractionTarget() {
        if (!worldSessionActive || !worldReady) {
            targetedBlock.reset();
            creatureHit.reset();
            return;
        }
        const auto& selectedStack = clientMirror_.player().heldStack;
        const bool collectingWater = selectedStack.item == &gameplay::items::Bucket;
        const float blockReach =
            uiFrameData_.gameMode == gameplay::GameMode::Creative ? 5.0F : 4.5F;
        targetedBlock = world::raycastVoxels(clientCache, camera.position(),
                                             camera.direction(), blockReach, collectingWater);
        // A creature's collision box blocks the ray exactly like a block's shape
        // (vanilla's HitResult is the nearest of block-or-entity). Tested against
        // the published entity snapshot, never the live vector.
        const auto hit = gameplay::raycastSnapshotEntities(
            clientMirror_.entities(), camera.position(), camera.direction(), blockReach);
        creatureHit = (hit.has_value() &&
                       (!targetedBlock.has_value() || hit->distance < targetedBlock->distance))
                          ? hit
                          : std::nullopt;
        if (creatureHit.has_value()) {
            targetedBlock.reset();
        }
        refreshHeldDestroyTarget();
    }

    void transitionTextureImage(const AllocatedImage& image, std::uint32_t layerCount,
                                VkImageLayout oldLayout, VkImageLayout newLayout,
                                VkAccessFlags sourceAccess, VkAccessFlags destinationAccess,
                                VkPipelineStageFlags sourceStage,
                                VkPipelineStageFlags destinationStage) const {
        resources_.transitionTextureImage(image, layerCount, oldLayout, newLayout, sourceAccess,
                                          destinationAccess, sourceStage, destinationStage);
    }

    void recreateTextureSampler() {
        checkVk(vkDeviceWaitIdle(device), "vkDeviceWaitIdle(anisotropy)");
        if (descriptorPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device, descriptorPool, nullptr);
            descriptorPool = VK_NULL_HANDLE;
        }
        textures_.recreateTextureSampler(options.anisotropy);
        createDescriptorPoolAndSets();
    }

    // Keep the complete BMP unihex set resident. 26.1 chooses and bakes glyphs
    // lazily; this renderer uses page-array layers, so preloading the bounded
    // 256-page equivalent gives the same language-switch property: switching
    // never reparses unifont.zip, waits for the device, or rebuilds descriptor
    // sets. Empty pages are omitted by TextureManager.
    [[nodiscard]] std::set<int> requiredUnicodePages() const {
        if (language.empty() && !options.forceUnicodeFont) {
            return {};
        }
        std::set<int> pages;
        for (int page = 0; page < 256; ++page) {
            pages.insert(page);
        }
        return pages;
    }

    // Rebuilds the font array only when the force-unicode provider changes.
    void recreateFontTexture() {
        checkVk(vkDeviceWaitIdle(device), "vkDeviceWaitIdle(font)");
        if (descriptorPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device, descriptorPool, nullptr);
            descriptorPool = VK_NULL_HANDLE;
        }
        textures_.recreateFontTexture(fontMetrics, textFont, requiredUnicodePages(),
                                      options.forceUnicodeFont);
        createDescriptorPoolAndSets();
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
        VkDescriptorSetLayoutBinding rainSamplerBinding = samplerBinding;
        rainSamplerBinding.binding = 9;
        rainSamplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        const std::array bindings{uniformBinding,           samplerBinding,
                                  fontSamplerBinding,       guiSamplerBinding,
                                  entitySamplerBinding,     panoramaSamplerBinding,
                                  biomeGrassSamplerBinding, biomeFoliageSamplerBinding,
                                  shadowSamplerBinding,     rainSamplerBinding};
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
             static_cast<std::uint32_t>(kFramesInFlight * 9U)},
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
            imageInfo.imageView = textures_.textureView;
            imageInfo.sampler = textures_.textureSampler;
            VkDescriptorImageInfo fontImageInfo{};
            fontImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            fontImageInfo.imageView = textures_.fontTextureView;
            fontImageInfo.sampler = textures_.textureSampler;
            VkDescriptorImageInfo guiImageInfo{};
            guiImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            guiImageInfo.imageView = textures_.guiTextureView;
            guiImageInfo.sampler = textures_.textureSampler;
            VkDescriptorImageInfo entityImageInfo{};
            entityImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            entityImageInfo.imageView = textures_.entityTextureView;
            entityImageInfo.sampler = textures_.textureSampler;
            VkDescriptorImageInfo panoramaImageInfo{};
            panoramaImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            panoramaImageInfo.imageView = textures_.panoramaTextureView;
            panoramaImageInfo.sampler = textures_.panoramaSampler;
            VkDescriptorImageInfo biomeGrassImageInfo{};
            biomeGrassImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            biomeGrassImageInfo.imageView = textures_.biomeGrassView;
            biomeGrassImageInfo.sampler = textures_.biomeSampler;
            VkDescriptorImageInfo biomeFoliageImageInfo{};
            biomeFoliageImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            biomeFoliageImageInfo.imageView = textures_.biomeFoliageView;
            biomeFoliageImageInfo.sampler = textures_.biomeSampler;
            VkDescriptorImageInfo rainImageInfo{};
            rainImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            rainImageInfo.imageView = textures_.rainTextureView;
            rainImageInfo.sampler = textures_.textureSampler;
            std::array<VkWriteDescriptorSet, 9> writes{};
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
            writes[8] = vkStructure<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
            writes[8].dstSet = sets[index];
            writes[8].dstBinding = 9;
            writes[8].descriptorCount = 1;
            writes[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[8].pImageInfo = &rainImageInfo;
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
        // 18,000 rain drops plus the enlarged 24,000 shared particle pool fit
        // together, including the gameplay reserve, in one per-frame buffer.
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
        checkVk(
            vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &sceneDescriptorSetLayout),
            "vkCreateDescriptorSetLayout(scene)");

        const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                            static_cast<std::uint32_t>(kFramesInFlight)};
        auto poolInfo =
            vkStructure<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
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
        // The descriptors below declare SHADER_READ_ONLY_OPTIMAL and three
        // terrain/entity fragment shaders sample binding 8 unconditionally as
        // far as Vulkan is concerned. With the sun shadows off — the default —
        // the pre-pass returns early and never transitions this image, so it
        // would sit in UNDEFINED while every draw claimed otherwise. That is
        // undefined behaviour, and the shape it takes on real hardware is a
        // silent GPU fault: a magenta flash and VK_ERROR_DEVICE_LOST out of the
        // next vkWaitForFences.
        shadowTarget.initializeAsShaderRead();

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
        auto poolInfo =
            vkStructure<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
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
        auto pushInfo =
            vkStructure<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
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

    [[nodiscard]] VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR>& modes,
                                                     bool vsync) const {
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
        const auto support = vulkanDevice_.querySwapchain(physicalDevice);
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
        if (renderSampleCount() == VK_SAMPLE_COUNT_1_BIT)
            return;
        colorTargets.resize(swapchainImages.size());
        for (auto& target : colorTargets) {
            target.image = createImage(
                swapchainExtent.width, swapchainExtent.height, 1, swapchainFormat,
                VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                renderSampleCount());
            target.view =
                createImageView(target.image.image, swapchainFormat, VK_IMAGE_ASPECT_COLOR_BIT);
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
            target.image =
                createImage(swapchainExtent.width, swapchainExtent.height, 1, depthFormat,
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
                            ? VK_ATTACHMENT_STORE_OP_STORE
                            : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        // Note: a resolved MSAA color attachment keeps COLOR_ATTACHMENT_OPTIMAL
        // here; UNDEFINED is rejected by the validation layers and this MoltenVK
        // build does not map transient attachments to on-tile memory anyway.
        color.finalLayout = renderSampleCount() == VK_SAMPLE_COUNT_1_BIT
                                ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
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
                                   ? 2U
                                   : static_cast<std::uint32_t>(attachments.size());
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
        const auto panoramaResult = vkCreateGraphicsPipelines(
            device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &panoramaPipeline);
        vkDestroyShaderModule(device, panoramaFragmentModule, nullptr);
        vkDestroyShaderModule(device, panoramaVertexModule, nullptr);
        checkVk(panoramaResult, "vkCreateGraphicsPipelines(panorama)");

        // The crosshair and vignette pipelines below reuse the shared
        // pipelineInfo, so put the hud layout and stage array back (the
        // panorama block swapped them for its own 16-byte push-constant layout
        // and a local stage array that is now out of scope).
        pipelineInfo.layout = hudPipelineLayout;
        pipelineInfo.pStages = hudStages.data();

        // Minecraft 26.1 draws the 15x15 hud/crosshair sprite with an
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
        if (occlusionDisabled) {
            return;
        }
        auto poolInfo =
            vkStructure<VkQueryPoolCreateInfo>(VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);
        poolInfo.queryType = VK_QUERY_TYPE_OCCLUSION;
        poolInfo.queryCount = static_cast<std::uint32_t>(kOcclusionQueryPoolSize);
        for (auto& queryPool : occlusionQueryPools) {
            checkVk(vkCreateQueryPool(device, &poolInfo, nullptr, &queryPool),
                    "vkCreateQueryPool(occlusion frame)");
        }

        constexpr VkPushConstantRange pushRange{VK_SHADER_STAGE_VERTEX_BIT, 0,
                                                sizeof(OcclusionQueryPushConstants)};
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
            {0.0F, 0.0F, 0.0F},
            {1.0F, 0.0F, 0.0F},
            {0.0F, 1.0F, 0.0F},
            {1.0F, 1.0F, 0.0F},
            {0.0F, 0.0F, 1.0F},
            {1.0F, 0.0F, 1.0F},
            {0.0F, 1.0F, 1.0F},
            {1.0F, 1.0F, 1.0F},
        }};
        constexpr std::array<std::uint32_t, 36> kUnitCubeIndices{{
            0, 2, 3, 0, 3, 1, // -y
            4, 6, 7, 4, 7, 5, // +y
            0, 4, 6, 0, 6, 2, // -x
            1, 3, 7, 1, 7, 5, // +x
            0, 1, 5, 0, 5, 4, // -z
            2, 3, 7, 2, 7, 6, // +z
        }};
        occlusionBoxVertexBuffer =
            createBuffer(sizeof(kUnitCubeCorners), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
        std::memcpy(occlusionBoxVertexBuffer.mapped, kUnitCubeCorners.data(),
                    sizeof(kUnitCubeCorners));
        checkVk(
            vmaFlushAllocation(allocator, occlusionBoxVertexBuffer.allocation, 0, VK_WHOLE_SIZE),
            "vmaFlushAllocation(occlusion box vertices)");
        occlusionBoxIndexBuffer =
            createBuffer(sizeof(kUnitCubeIndices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
        std::memcpy(occlusionBoxIndexBuffer.mapped, kUnitCubeIndices.data(),
                    sizeof(kUnitCubeIndices));
        checkVk(vmaFlushAllocation(allocator, occlusionBoxIndexBuffer.allocation, 0, VK_WHOLE_SIZE),
                "vmaFlushAllocation(occlusion box indices)");
    }

    // Swapchain-coupled: the query pipeline binds the current render pass, so
    // it is rebuilt alongside the other pipelines when the swapchain changes.
    void createOcclusionQueryPipeline() {
        if (occlusionQueryPools.front() == VK_NULL_HANDLE) {
            return;
        }
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

        VkVertexInputBindingDescription binding{0, sizeof(glm::vec3), VK_VERTEX_INPUT_RATE_VERTEX};
        const std::array<VkVertexInputAttributeDescription, 1> attributes{{
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
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
            if (allocator != VK_NULL_HANDLE)
                destroyImage(target.image);
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
        if (!world::isFluid(clientCache.block(x, y, z))) {
            return false;
        }
        float surfaceHeight = 1.0F;
        if (!world::isFluid(clientCache.block(x, y + 1, z))) {
            const std::uint8_t level = clientCache.fluidLevel(x, y, z);
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
        const auto& playerSnap = clientMirror_.player();
        if (!options.viewBobbing || playerSnap.flying)
            return glm::mat4{1.0F};
        const float alpha = renderInterpolationAlpha;
        const float phase = -std::lerp(playerSnap.previousSpeed, playerSnap.speed, alpha);
        const float stride = std::lerp(playerSnap.previousStride, playerSnap.stride, alpha);
        constexpr float pi = 3.14159265358979323846F;
        glm::mat4 transform{1.0F};
        transform = glm::translate(transform, {std::sin(phase * pi) * stride * 0.5F,
                                               -std::abs(std::cos(phase * pi) * stride), 0.0F});
        transform = glm::rotate(transform, glm::radians(std::sin(phase * pi) * stride * 3.0F),
                                {0.0F, 0.0F, 1.0F});
        transform = glm::rotate(transform,
                                glm::radians(std::abs(std::cos(phase * pi - 0.2F) * stride) * 5.0F),
                                {1.0F, 0.0F, 0.0F});
        return transform;
    }

    // The actual eye the scene is rendered from. The camera object always sits
    // at the gameSession.player()'s eye; third person pulls the render eye back (or pushes it
    // in front, looking back) along the look direction. Both the view matrix and
    // the culling frustum are derived from this so they always agree.
    // RenderEye now lives in render/vulkan/WorldRenderTypes.hpp (shared).
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
        const auto hit = world::raycastVoxels(clientCache, eyePivot, boomDirection,
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
        const glm::mat4 baseView =
            glm::lookAt(renderEye.position, renderEye.position + renderEye.forward,
                        glm::vec3{0.0F, 1.0F, 0.0F});
        uniform.view = viewBobbingMatrix() * baseView;
        uniform.projection = camera.projectionMatrix(static_cast<float>(swapchainExtent.width) /
                                                         static_cast<float>(swapchainExtent.height),
                                                     cameraFarPlane());
        uniform.cameraPosition = glm::vec4{renderEye.position, 1.0F};
        // The sun and moon read the Overworld clock, not the frame timer: the
        // sky advances with the world's ticks and freezes exactly when the clock
        // does (doDaylightCycle off, or the game paused), instead of drifting on
        // real frames. The 20 Hz tick is coarse enough only for fast-moving
        // things; the sun barely moves per tick, so no sub-tick interpolation is
        // needed here.
        const auto dayTick = clientMirror_.world().dayTimeTicks;
        const auto daylight = world::DayNightCycle::stateAtTick(dayTick);
        uniform.sunDirection = glm::vec4{daylight.sunDirection, daylight.skyBrightness};
        // horizonFog.w drives only the moon phase. Fluid animation uses the
        // server tick below, so disabling the daylight cycle no longer freezes
        // water and lava. Wrapping the lunar clock also avoids float precision
        // loss in long-running worlds.
        constexpr double kLunarCycleSeconds = 8.0 * world::DayNightCycle::kSecondsPerDay;
        const double dayTimeSeconds = dayTick / world::DayNightCycle::kTicksPerSecond;
        uniform.horizonFog =
            glm::vec4{daylight.horizonColor,
                      static_cast<float>(std::fmod(dayTimeSeconds, kLunarCycleSeconds))};
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
        // The sky's weather read comes from the per-tick snapshot, reproducing
        // the frame interpolation the live system's rainGradientAt(alpha) gave.
        const auto& weather = clientMirror_.world();
        const float alpha = renderInterpolationAlpha;
        const float rainGradient = weather.previousRainGradient +
                                   (weather.rainGradient - weather.previousRainGradient) * alpha;
        const float thunderGradient = weather.previousThunderGradient +
                                      (weather.thunderGradient - weather.previousThunderGradient) *
                                          alpha;
        constexpr float kWeatherSkyReduction = 5.0F / 16.0F;
        const float skyFactor =
            (1.0F - std::clamp(rainGradient, 0.0F, 1.0F) * kWeatherSkyReduction) *
            (1.0F - std::clamp(thunderGradient, 0.0F, 1.0F) * kWeatherSkyReduction);
        uniform.weatherSettings =
            glm::vec4{rainGradient, thunderGradient, skyFactor, 1.0F - rainGradient};
        uniform.fluidAnimationLayers = glm::vec4{
            static_cast<float>(kWaterStillLayer), static_cast<float>(kWaterFlowLayer),
            static_cast<float>(kLavaStillLayer), static_cast<float>(kLavaFlowLayer)};
        uniform.fluidAnimationFrameCounts = glm::vec4{
            static_cast<float>(kWaterAnimationFrameCount),
            static_cast<float>(kWaterAnimationFrameCount),
            static_cast<float>(kLavaStillFrameCount),
            static_cast<float>(kLavaFlowFrameCount)};
        uniform.fluidAnimationFrameTimes = glm::vec4{
            textures_.fluidAnimationFrameTimes[0], textures_.fluidAnimationFrameTimes[1],
            textures_.fluidAnimationFrameTimes[2], textures_.fluidAnimationFrameTimes[3]};
        uniform.fluidAnimationSettings.x =
            static_cast<float>(clientMirror_.world().serverTick) + renderInterpolationAlpha;
        std::size_t lightCount = 0U;
        const auto& heldStack = clientMirror_.player().heldStack;
        const bool holdingTorch = gameplay::emitsHeldLight(heldStack);
        if (options.dynamicLight && holdingTorch) {
            uniform.pointLights[lightCount] = glm::vec4{camera.position(), 7.7F};
            uniform.lightColors[lightCount] = {1.0F, 0.72F, 0.38F, 0.78F};
            ++lightCount;
        }
        uniform.lightingSettings.x = static_cast<float>(lightCount);
        uniform.lightingSettings.y =
            options.smoothLightingQuality != world::SmoothLightingQuality::Off ? 1.0F : 0.0F;
        uniform.lightingSettings.z =
            currentMeshQuality == world::SmoothLightingQuality::High ? 1.0F : 0.0F;
        // lightingSettings.w is the sun-shadow switch: 1.0 when the pre-pass ran
        // this frame, so the terrain shader only samples the map when it is live.
        uniform.lightingSettings.w = shadowDisabled ? 0.0F : 1.0F;
        uniform.lightViewProj = shadowLightViewProj;
        std::memcpy(frame.uniformBuffer.mapped, &uniform, sizeof(uniform));
        checkVk(vmaFlushAllocation(allocator, frame.uniformBuffer.allocation, 0, sizeof(uniform)),
                "vmaFlushAllocation(camera uniform)");
    }

    [[nodiscard]] std::string_view translate(std::string_view key,
                                             std::string_view fallback) const {
        return language.translate(key, fallback);
    }

    [[nodiscard]] std::string translated(std::string_view key, std::string_view fallback) const {
        return std::string{translate(key, fallback)};
    }

    // 26.1's LanguageManager builds this catalog exclusively from pack.mcmeta.
    // No translation JSON is opened here, regardless of how many languages a
    // pack contains.
    void loadLanguageCatalog() {
        const auto started = std::chrono::steady_clock::now();
        const auto catalog = ui::availableLanguages(*resourceProvider);
        menuSystem.languageCodes.clear();
        menuSystem.languageDisplayNames.clear();
        menuSystem.languageCodes.reserve(catalog.size());
        menuSystem.languageDisplayNames.reserve(catalog.size());
        for (const auto& entry : catalog) {
            menuSystem.languageCodes.push_back(entry.code);
            menuSystem.languageDisplayNames.push_back(entry.displayName());
        }
        menuSystem.languageListFirstIndex = 0U;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        std::cout << "Loaded language catalog: " << catalog.size() << " entries in "
                  << elapsed.count() << " ms\n";
    }

    void loadLanguage() {
        loadLanguageCatalog();
        if (std::ranges::find(menuSystem.languageCodes, options.language) ==
            menuSystem.languageCodes.end()) {
            options.language = ui::kDefaultLanguageCode;
        }
        try {
            language = ui::Language::fromProvider(*resourceProvider, options.language);
            std::cout << "Loaded language " << options.language << ": " << language.size()
                      << " entries\n";
        } catch (const std::exception& exception) {
            std::cout << "Language " << options.language
                      << " unavailable, falling back to en_us: " << exception.what() << '\n';
            options.language = ui::kDefaultLanguageCode;
            language = ui::Language::fromProvider(*resourceProvider, options.language);
        }
        textFont.setForceUnicode(options.forceUnicodeFont);
        menuSystem.pendingLanguageCode = options.language;
    }

    void beginLanguageLoad(const std::string& code) {
        if (languageLoader.busy()) {
            queuedLanguageCode = code;
            menuSystem.languageStatus = "Language reload queued";
            return;
        }
        if (options.language == code) {
            menuSystem.languageStatus.clear();
            return;
        }
        menuSystem.languageStatus = "Loading language...";
        if (!languageLoader.start(code)) {
            menuSystem.languageStatus = "Unable to start language reload";
        } else {
            languageLoadStarted = std::chrono::steady_clock::now();
        }
    }

    void pollLanguageLoad() {
        auto prepared = languageLoader.poll();
        if (!prepared.has_value()) {
            return;
        }
        if (prepared->error.empty()) {
            language = std::move(prepared->language);
            options.language = std::move(prepared->code);
            menuSystem.pendingLanguageCode = options.language;
            menuSystem.languageStatus.clear();
            persistOptions();
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - languageLoadStarted);
            std::cout << "Loaded language " << options.language << ": " << language.size()
                      << " entries in " << elapsed.count() << " ms\n";
        } else {
            menuSystem.languageStatus = "Language load failed: " + prepared->error;
            menuSystem.pendingLanguageCode = options.language;
            std::cerr << menuSystem.languageStatus << '\n';
        }
        if (!queuedLanguageCode.empty()) {
            std::string queued = std::move(queuedLanguageCode);
            queuedLanguageCode.clear();
            beginLanguageLoad(queued);
        }
    }

    // ItemRenderer#renderGuiItemOverlay's damage bar: a black 13x2 strip along
    // the bottom of the 16x16 icon, with a coloured strip on top whose length
    // is the remaining durability and whose hue runs green to red across the
    // tool's life. Undamaged items draw nothing, exactly like vanilla.
    [[nodiscard]] static float particleLevelMultiplier(int level) {
        switch (level) {
        case 0:
            return 0.5F;
        case 2:
            return 2.0F;
        case 3:
            return 3.0F;
        default:
            return 1.0F;
        }
    }

    void applyParticleLevel() {
        particleSystem.setLevelScale(particleLevelMultiplier(options.particleLevel));
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
    // Starts the simulation thread (unless MC_REBEDROCK_SYNC_TICK asks for the
    // synchronous loop) and owns the tick/world-lock discipline; the runtime
    // holds the driver, the world lock and the simulationActive gate.
    void startSimulationThread() {
        runtime.startSimulation();
    }

    void drawFrame() {
        // No world lock is held here. Everything the draw pass samples — the
        // render-owned client cache, the atomically published snapshots, the GPU
        // mesh state — is lock-free, and the GPU fence wait, submit and present
        // below must not block the simulation thread's write section.
        auto& frame = frames[currentFrame];
        const auto fenceWaitStart = std::chrono::steady_clock::now();
        checkVk(vkWaitForFences(device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX),
                "vkWaitForFences");
        if (diag::traceEnabled()) {
            diag::frameTrace().fenceWaitMs += diag::msSince(fenceWaitStart);
        }
        // Tell VMA which frame this is so it can reuse allocations released a
        // frame-index window ago instead of growing new blocks every burst.
        vmaSetCurrentFrameIndex(allocator, frameNumber_);
        world_.releaseFrameResources(frame);
        world_.readBackOcclusionQueries();
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
        world_.prepareStreamingUpdates(frame);
        if (worldSessionActive && !worldReady && completedStreamBatchCount > 0U &&
            spawnPositionInitialized && pendingSectionUpdates.empty()) {
            worldReady = true;
            paused = false;
            menuSystem.pageStack.reset(ui::PageId::Game);
            // Loading complete only starts the simulation. It must not re-teleport
            // the player from render state: loadWorld already restored the saved
            // coordinates into the live controller, the physics endpoints and the
            // published snapshot, so any "re-anchor" here would write stale render
            // state back over the authoritative position.
            simulationActive.store(true, std::memory_order_release);
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            std::cout << "Terrain loading complete\n";
            // The spawn area is in; widen to the full render distance. The
            // per-frame streaming loop keeps requesting, so the rest of the view
            // distance fills in progressively during play, the way vanilla
            // streams chunks past its initial entry area.
            chunkStreamer.setRadii(viewDistanceChunks,
                                   viewDistanceChunks + world::kUnloadHysteresisChunks);
        }
        world_.updateShadowMatrix();
        updateUniform(frame);
        checkVk(vkResetFences(device, 1, &frame.inFlight), "vkResetFences");
        checkVk(vkResetCommandBuffer(frame.commandBuffer, 0), "vkResetCommandBuffer");
        const std::size_t visibleCount = world_.recordCommandBuffer(frame, imageIndex);
        const auto& titleSnap = clientMirror_.player();
        const std::string movementMode =
            titleSnap.flying ? (titleSnap.sprinting ? "FLY SPRINT" : "FLY")
                             : (titleSnap.sprinting ? "SPRINT" : "WALK");
        const std::string playerMode =
            std::string{gameplay::gameModeName(titleSnap.gameMode)} + " " + movementMode;
        const gameplay::ItemStack selectedItem = titleSnap.heldStack;
        if (visibleCount != lastVisibleMeshCount || gpuMeshes.size() != lastGpuMeshCount ||
            pendingSectionUpdates.size() != lastPendingSectionCount ||
            playerMode != lastPlayerMode || selectedItem != lastSelectedItem) {
            lastVisibleMeshCount = visibleCount;
            lastGpuMeshCount = gpuMeshes.size();
            lastPendingSectionCount = pendingSectionUpdates.size();
            lastPlayerMode = playerMode;
            lastSelectedItem = selectedItem;
            const gameplay::DescriptionId selectedDescriptionId =
                gameplay::itemDescriptionId(selectedItem);
            const std::string selectedName = selectedDescriptionId.empty()
                ? std::string{}
                : std::string{language.translate(selectedDescriptionId.prefix(),
                                                 selectedDescriptionId.source.space,
                                                 selectedDescriptionId.source.path,
                                                 selectedDescriptionId.source.path)};
            const std::string title =
                "MC Rebedrock - " + playerMode + " | visible " + std::to_string(visibleCount) +
                "/" + std::to_string(gpuMeshes.size()) + " | pending " +
                std::to_string(pendingSectionUpdates.size()) + " | CPU chunks " +
                std::to_string(loadedCpuChunkCount) + " | " + selectedName;
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
    const assets::ResourceProvider* resourceProvider = nullptr;
    ui::AsyncLanguageLoader languageLoader;
    std::chrono::steady_clock::time_point languageLoadStarted{};
    std::filesystem::path optionsPath;
    // The authoritative runtime owns the world, the save repository, the game
    // session, the simulation driver and the world lock. The references below
    // are convenience aliases so the many call sites keep reading them by their
    // familiar names; the objects themselves live in the runtime, which is what
    // a dedicated server links. Declaration order matters: the runtime is built
    // first and the aliases point into it.
    runtime::GameRuntime runtime;
    persistence::SaveRepository& saveRepository;
    world::ChunkStreamer& chunkStreamer;
    world::World& interactionWorld;
    // The client-side chunk cache the renderer meshes and samples from (M-Chunk
    // B-5): a distinct world owned by the presentation side, fed by the same
    // streamer batches and simulation edits that write the server world. The
    // simulation keeps ticking interactionWorld; the renderer reads only this
    // cache, so the two sides own their own chunk data.
    world::World clientCache;
    // The client-side render mirror: filled each frame by pumping the loopback
    // channel. Player, world and entity presentation all read the decoded views
    // here instead of reaching into the authoritative session.
    client::ClientMirror clientMirror_;
    gameplay::GameSession& gameSession;
    gameplay::SimulationDriver& simulationDriver;
    std::atomic_bool& simulationActive;
    world::WorldLock& worldLock;
    std::optional<persistence::SaveGame>& currentSave;
    std::uint64_t& worldEpoch;
    config::GameOptions options;
    std::optional<TestSceneOptions> testScene;
    audio::AudioSystem audioSystem;
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
    std::array<VkQueryPool, kFramesInFlight> occlusionQueryPools{};
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
    ui::MenuSystem menuSystem;
    // The HUD-facing gameplay state, captured once per frame so the draw
    // passes read a consistent snapshot instead of live gameplay objects.
    mutable ui::UiFrameData uiFrameData_;
    PerspectiveCamera camera;
    // The unmodified FOV the camera was built with. Every frame multiplies it by
    // the gameSession.player()'s movement FOV multiplier, so the base has to be kept aside.
    float baseFieldOfViewDegrees = 65.0F;
    // Health, hunger and environmental damage. Only ticked in survival.
    // The world's game rules, owned here and mirrored into the systems that
    // consume them; persisted as a sparse self-describing block in world.dat.
    // Minimal free-roaming creatures and the pig skeleton they render with.
    // The first consumer of the box-UV entity pipeline.
    // The species the box-UV entity pipeline has bound: one entry per loaded
    // creature (pig, zombie), each carrying its geometry + animations and its
    // layer in the entity texture array. Filled by createEntityTextureArray.
    std::vector<gameplay::entities::SpeciesRenderModel> speciesModels;
    animation::ModelAnimationSystem heldItemAnimation;
    // The gameSession.inventory() preview gameSession.player() and the in-world third-person
    // gameSession.player() use separate animator instances driven by different inputs (cursor vs.
    // the gameSession.player()'s own look/movement).
    animation::PlayerModelAnimator playerModelAnimator;
    // ANIM task2: the third-person world player runs this PlayerModelAnimator
    // controller stack (shared with the inventory preview), fed the authoritative
    // WalkAnimationState — HumanoidPoseSolver is retired, no separate solved pose.
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
    bool windowPlacementDirty = false;
    std::chrono::steady_clock::time_point windowPlacementChangedAt{};
    bool validationEnabled = false;
    bool firstMouseSample = true;
    bool destroyButtonHeld = false;
    std::optional<glm::ivec3> lastDestroyAimBlock;
    bool inventoryOpen = false;
    bool spawnPositionInitialized = false;
    bool worldReady = false;
    // DR repro hook: MC_REBEDROCK_LOAD_SAVE auto-loads the first real save.
    bool loadSaveStarted = false;
    bool creativeScrollbarDragging = false;
    // SlotActionType.QUICK_CRAFT drag state: the button held and the (kind,
    // index) of every slot the cursor swept over — values, never storage
    // pointers into gameplay. A drag starts when a press leaves a stack on the
    // cursor, collects slots while the button is held, and ships the set to the
    // interaction on release.
    bool inventoryDragActive = false;
    gameplay::InventoryMouseButton inventoryDragButton = gameplay::InventoryMouseButton::Left;
    std::vector<gameplay::SlotRef> inventoryDragSlots;
    // Vanilla sets this on a press that already acted (a pickup or quick-move
    // from an empty cursor) so the release does not place or distribute again.
    bool cancelNextInventoryRelease = false;
    // SlotActionType.PICKUP_ALL double-click state: the last slot pressed (by
    // kind + index), when, and whether the press was the second within the
    // vanilla 250 ms window. On release the same-type stacks gather into the
    // cursor.
    std::optional<gameplay::SlotRef> lastClickedSlot;
    double lastClickTime = 0.0;
    bool isDoubleClicking = false;
    bool paused = true;
    bool debugOverlayOpen = false;
    // D0: the client-side jump and sprint-double-tap edges. The GLFW key callback
    // sets them; processInput folds them into the frame's MovementInput and clears
    // them on send. Before the client/server split these were GameSession's
    // jumpPressed_/forwardPressed_ accumulators, which a cross-process client (no
    // session) cannot touch — now the edge is the client's and travels the channel.
    bool pendingJumpPressed_ = false;
    bool pendingForwardPressed_ = false;
    // PX-1: the single input collection point. Holds the rebindable action ->
    // binding table and the previous-frame level bitmap for edge detection, so
    // processInput() no longer reads raw GLFW keys and the key callback compares
    // against bindings instead of hardcoded GLFW_KEY_* constants.
    input::InputSystem inputSystem_;
    input::InputSystem::EventQueue inputEvents_;
    // PX-5: the Key Binds screen's rebind state, over the InputSystem single
    // source. Non-null capture means the next key press is consumed as a rebind
    // rather than gameplay input.
    input::KeyBindingScreen keyBindScreen_{inputSystem_};
    // PX-6: the game-in HUD overlays. The toast queue (top-right notifications)
    // and the subtitle feed (bottom-right sound captions) are Vulkan-free client-
    // presentation state, advanced on frame delta and drawn by HudRenderer.
    ui::ToastQueue toastQueue_;
    ui::SubtitleFeed subtitleFeed_;
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

    ui::WidgetId pressedMenuButton = ui::WidgetId::None;
    // PX-4: the pressed widget's index into the current ui::Page, so the release
    // dispatches through the model (dispatchActivate) instead of a MenuButton
    // switch. kNoWidget when no widget is pressed.
    std::size_t pressedMenuIndex_ = ui::kNoWidget;
    std::optional<world::VoxelRaycastHit> targetedBlock;

    // The furnace block the gameSession.player() last opened; while the shared furnace state
    // is burning, that block swaps to the lit state (texture + light).

    // The aim ray's nearest creature, computed with the block target each frame;
    // the input handlers package it into the destroy command.
    std::optional<gameplay::EntityRayHit> creatureHit;
    // The swing sequence the held-item bridge sampled last frame, so a restart
    // (sequence change) snaps instead of interpolating the arm back.
    std::optional<std::uint64_t> lastSwingSequence_;
    // PX-2 Bug2: the third-person world player's own swing-sequence memory, so its
    // attack arc snaps on a restart independently of the first-person bridge.
    std::optional<std::uint64_t> lastWorldSwingSequence_;
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
    // Menus, the cursor blink and chat expiry run on wall time and must keep
    // running while the simulation is paused or the sun is frozen.
    double uiTimeSeconds = 0.0;
    // Animation interpolation. Separate from uiTimeSeconds because it stops
    // with the world: a paused game should not keep swinging arms. Both are
    // frame-local and neither is persisted.
    double renderTimeSeconds = 0.0;
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
    // Owns the device/instance/allocator/queues/command pool. The same-named
    // members below are non-owning copies the renderer reads directly.
    VulkanDevice vulkanDevice_;
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
    // Owns the rain/GUI/panorama/biome texture resources (see TextureManager);
    // block/entity/font arrays still live here as flat members for now.
    TextureManager textures_;
    // The single path every block change this renderer makes flows through, so
    // the block-entity, neighbour, section and drop consequences are dispatched
    // from one place instead of being re-assembled at each call site.
    world::WorldMutationService worldMutations;
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
    // Reused CPU staging for block-dust/splash particles plus async rain. It
    // stays host-cached while records sample the world, then bulk-copies once
    // into the current frame's sequential-write mapped storage buffer.
    std::vector<ParticleRecord> sceneParticleRecords_;
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
    // Native 64x256 environment/rain.png for the vanilla column rain pass.
    // It cannot share the square block array without aspect-ratio distortion.
    // The six 1.16.1 title-screen panorama faces, one array layer each, sampled
    // with a dedicated linear sampler because they are photographs, not pixel
    // art. Kept out of the 256px GUI array so they stay at native resolution.
    // The 1.16.1 biome colour lookup textures (grass + foliage), sampled by the
    // terrain fragment shader with a linear sampler so biome boundaries blend as
    // a smooth per-pixel gradient — the GPU-side equivalent of Java's per-vertex
    // BiomeColors, but robust because the colour comes from a texture fetch
    // rather than a per-vertex attribute. Generated from the world seed and the
    // vanilla colour maps when a world loads; the pixel data is regenerated per
    // seed, the images/sampler are created once.
    ui::BitmapFontMetrics fontMetrics;
    // ascii.png metrics plus the legacy unicode pages, and the translation
    // table the interface reads its strings from.
    ui::TextFont textFont;
    ui::Language language;
    std::string queuedLanguageCode;
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
    bool occlusionDisabled = disableOcclusionQueries();
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
    std::unordered_map<PersistentEditPosition, std::size_t, PersistentEditPositionHash>
        savedEditIndices;
    // The save being edited, captured when Edit is pressed so the edit/delete
    // flow keeps working even if the list refreshes in between.
    bool worldSessionActive = false;

    // Wires the extracted HudRenderer to this Impl's state: reference fields
    // bind directly to members (so resize-recreated pipelines and input-mutated
    // UI flags stay visible), and a few std::function hooks keep the world-render
    // couplings (held item, submerged test, model-preview descriptor) in Impl.
    [[nodiscard]] HudRenderer::Bindings makeHudBindings() {
        return HudRenderer::Bindings{
            .menuSystem = menuSystem,
            .uiFrameData_ = uiFrameData_,
            .gameSession = gameSession,
            .clientMirror = clientMirror_,
            .textFont = textFont,
            .fontMetrics = fontMetrics,
            .language = language,
            .lightWorld = clientCache,
            .window = window,
            .options = options,
            .camera = camera,
            .swapchainExtent = swapchainExtent,
            .hudPipeline = hudPipeline,
            .hudPipelineLayout = hudPipelineLayout,
            .vignettePipeline = vignettePipeline,
            .crosshairPipeline = crosshairPipeline,
            .panoramaPipeline = panoramaPipeline,
            .panoramaPipelineLayout = panoramaPipelineLayout,
            .heldItemPipeline = heldItemPipeline,
            .itemPipelineLayout = itemPipelineLayout,
            .inventoryOpen = inventoryOpen,
            .containerScreen = uiFrameData_.containerScreen,
            .activeChest = uiFrameData_.activeChest,
            .debugOverlayOpen = debugOverlayOpen,
            .inventoryDragActive = inventoryDragActive,
            .inventoryDragSlots = inventoryDragSlots,
            .chatOpen = chatOpen,
            .chatHistory = chatHistory,
            .chatInputText = chatInputText,
            .chatSuggestions_ = chatSuggestions_,
            .chatSuggestionIndex_ = chatSuggestionIndex_,
            .toastQueue = toastQueue_,
            .subtitleFeed = subtitleFeed_,
            .currentSave = currentSave,
            .displayedFps = displayedFps,
            .playerModelAnimator = playerModelAnimator,
            .pressedMenuButton = pressedMenuButton,
            .spawnPositionInitialized = spawnPositionInitialized,
            .worldReady = worldReady,
            .worldSessionActive = worldSessionActive,
            .simulationDistanceChunks = simulationDistanceChunks,
            .viewDistanceChunks = viewDistanceChunks,
            .peakPendingSectionCount = peakPendingSectionCount,
            .pendingSectionUpdates = pendingSectionUpdates,
            .testScene = testScene,
            .guiWidgetSprites = textures_.guiWidgetSprites,
            .paused = paused,
            .uiTimeSeconds = uiTimeSeconds,
            .cameraSubmergedInWater = [this] { return cameraSubmergedInWater(); },
            .keyBindLabel =
                [this](input::InputAction action) -> std::string {
                    const std::string name{input::actionDisplayName(action)};
                    if (keyBindScreen_.capturing() &&
                        keyBindScreen_.capturingAction() == action) {
                        return name + ": > ? <";
                    }
                    return name + ": " +
                           input::bindingDisplayName(inputSystem_.bindings().binding(action));
                },
            .drawHeldItem = [this](VkCommandBuffer c,
                                   VkDescriptorSet d) { world_.drawHeldItem(c, d); },
            .currentFrameDescriptorSet = [this] { return frames[currentFrame].descriptorSet; },
            .activeCreativeCatalog = [this] { return activeCreativeCatalog(); },
            .creativeScrollPosition = [this] { return creativeScrollPosition(); },
            .creativeMaximumScrollRow = [this] { return creativeMaximumScrollRow(); },
            .dragPlacementCounts = [this] { return dragPlacementCounts(); },
            .cameraFarPlane = [this] { return cameraFarPlane(); },
            .dragSlotRectangle =
                [this](const ui::HudLayout& l, const gameplay::SlotRef& s) {
                    return dragSlotRectangle(l, s);
                },
        };
    }

    // Declared last so its reference members bind to fully-constructed members
    // above (NSDMI runs in declaration order); the HUD draw pass lives here.
    HudRenderer hud_{makeHudBindings()};

    // Wires WorldRenderer to this Impl's state (references-only: all state stays
    // owned here so the GPU-resource teardown ordering is untouched), plus a few
    // std::function hooks for camera/gameplay callbacks that remain in Impl.
    [[nodiscard]] WorldRenderer::Bindings makeWorldBindings() {
        return WorldRenderer::Bindings{
            .testScene = testScene,
            .chunkStreamer = chunkStreamer,
            .interactionWorld = interactionWorld,
            .clientCache = clientCache,
            .interactionLightEngine = interactionLightEngine,
            .gpuMeshes = gpuMeshes,
            .deviceBufferPool_ = deviceBufferPool_,
            .stagingBufferPool_ = stagingBufferPool_,
            .occlusionQueryPools = occlusionQueryPools,
            .occlusionQueryPipeline = occlusionQueryPipeline,
            .occlusionQueryLayout = occlusionQueryLayout,
            .occlusionBoxVertexBuffer = occlusionBoxVertexBuffer,
            .occlusionBoxIndexBuffer = occlusionBoxIndexBuffer,
            .pendingSectionOrder = pendingSectionOrder,
            .currentMeshQuality = currentMeshQuality,
            .targetMeshQuality = targetMeshQuality,
            .qualityRemeshPending = qualityRemeshPending,
            .gameSession = gameSession,
            .clientMirror = clientMirror_,
            .enqueueClientCommand =
                [this](gameplay::GameCommand command) {
                    runtime.enqueueClientCommand(std::move(command));
                },
            .simulationHost = *this,
            .worldLock = worldLock,
            .uiFrameData_ = uiFrameData_,
            .camera = camera,
            .speciesModels = speciesModels,
            .heldItemAnimation = heldItemAnimation,
            .worldPlayerAnimator = worldPlayerAnimator,
            .chestLidAnimation = chestLidAnimation,
            .itemDisplayAnimation = itemDisplayAnimation,
            .cameraPerspective = cameraPerspective,
            .worldBodyYaw = worldBodyYaw,
            .particleSystem = particleSystem,
            .inventoryOpen = inventoryOpen,
            .spawnPositionInitialized = spawnPositionInitialized,
            .worldReady = worldReady,
            .paused = paused,
            .dropRequested = dropRequested,
            .dropWholeStack = dropWholeStack,
            .chatOpen = chatOpen,
            .targetedBlock = targetedBlock,
            .renderTimeSeconds = renderTimeSeconds,
            .renderInterpolationAlpha = renderInterpolationAlpha,
            .window = window,
            .instance = instance,
            .surface = surface,
            .device = device,
            .allocator = allocator,
            .resources_ = resources_,
            .textures_ = textures_,
            .sceneDescriptorSets = sceneDescriptorSets,
            .gpuSceneBuffer = gpuSceneBuffer,
            .particlePipeline = particlePipeline,
            .particlePipelineLayout = particlePipelineLayout,
            .legacyParticles = legacyParticles,
            .shadowTarget = shadowTarget,
            .shadowPipelineLayout = shadowPipelineLayout,
            .shadowPipeline = shadowPipeline,
            .shadowDebugSet = shadowDebugSet,
            .shadowDebugPipelineLayout = shadowDebugPipelineLayout,
            .shadowDebugPipeline = shadowDebugPipeline,
            .shadowLightViewProj = shadowLightViewProj,
            .shadowDisabled = shadowDisabled,
            .shadowDebugOverlay = shadowDebugOverlay,
            .rainSystem = rainSystem,
            .sceneParticleRecords_ = sceneParticleRecords_,
            .rainMode_ = rainMode_,
            .rainTime_ = rainTime_,
            .rainSheetPipeline = rainSheetPipeline,
            .rainSheetPipelineLayout = rainSheetPipelineLayout,
            .language = language,
            .swapchainExtent = swapchainExtent,
            .renderPass = renderPass,
            .pipelineLayout = pipelineLayout,
            .graphicsPipeline = graphicsPipeline,
            .translucentPipeline = translucentPipeline,
            .cutoutPipeline = cutoutPipeline,
            .skyPipeline = skyPipeline,
            .outlinePipelineLayout = outlinePipelineLayout,
            .outlinePipeline = outlinePipeline,
            .itemPipelineLayout = itemPipelineLayout,
            .itemPipeline = itemPipeline,
            .itemShadowPipeline = itemShadowPipeline,
            .heldItemPipeline = heldItemPipeline,
            .framebuffers = framebuffers,
            .frames = frames,
            .currentFrame = currentFrame,
            .occlusionDisabled = occlusionDisabled,
            .hasLastRenderEye = hasLastRenderEye,
            .lastRenderEye = lastRenderEye,
            .occlusionValidityInitialized = occlusionValidityInitialized,
            .occlusionRotationAccumulatorDegrees = occlusionRotationAccumulatorDegrees,
            .occlusionTranslationAccumulator = occlusionTranslationAccumulator,
            .peakPendingSectionCount = peakPendingSectionCount,
            .smoothedFrameSeconds_ = smoothedFrameSeconds_,
            .streamingUploadBudget_ = streamingUploadBudget_,
            .occlusionStates = occlusionStates,
            .occlusionMissCount = occlusionMissCount,
            .pendingSectionUpdates = pendingSectionUpdates,
            .latestSectionRevisions = latestSectionRevisions,
            .worldEpoch = worldEpoch,
            .loadedCpuChunkCount = loadedCpuChunkCount,
            .completedBlockEditCount = completedBlockEditCount,
            .completedStreamBatchCount = completedStreamBatchCount,
            .lastVisibleMeshCount = lastVisibleMeshCount,
            .worldSessionActive = worldSessionActive,
            .hasLastStreamingForward = hasLastStreamingForward,
            .lastStreamingForward = lastStreamingForward,
            .uploadedSectionsThisFrame = uploadedSectionsThisFrame,
            .uploadedBytesThisFrame = uploadedBytesThisFrame,
            .totalUploadedBytes = totalUploadedBytes,
            .hud_ = hud_,
            .rainTargetCount = [this] { return rainTargetCount(); },
            .renderViewMatrix = [this] { return renderViewMatrix(); },
            .viewBobbingMatrix = [this] { return viewBobbingMatrix(); },
            .renderEyeState = [this] { return renderEyeState(); },
            .cameraFarPlane = [this] { return cameraFarPlane(); },
            .renderDistanceBlocks = [this] { return renderDistanceBlocks(); },
            .initializeSpawnPosition = [this] { initializeSpawnPosition(); },
            .submitWorldEditFn =
                [this](int x, int y, int z, world::Block b, std::uint8_t f,
                       std::optional<world::BlockOrientation> o) {
                    submitWorldEdit(x, y, z, b, f, o);
                },
            .hasPersistentEditFn = [this](int x, int y, int z) {
                return savedEditIndices.contains(PersistentEditPosition{x, y, z});
            },
            .onChunkUnloaded = [this](world::ChunkPosition position) {
                // M-3 C5: a chunk left the simulation radius. The runtime persists
                // its edits and creatures to the chunk's region file and drops
                // the creatures from the simulation, so a herd outside the radius
                // survives on disk until its chunk streams back in.
                runtime.persistUnloadedChunk(position);
            },
            .onChunkLoaded = [this](world::ChunkPosition position) {
                // A chunk streamed back in — restore the creatures the unload
                // path wrote for it, if any.
                runtime.restoreLoadedChunk(position);
            },
        };
    }

    // Declared after hud_ so its Bindings can reference hud_; world render pass.
    WorldRenderer world_{makeWorldBindings()};
};

VulkanRenderer::VulkanRenderer(std::filesystem::path shaderRoot,
                               const assets::ResourceProvider& resourceProvider,
                               world::ChunkStreamer& chunkStreamer, config::GameOptions options,
                               std::filesystem::path optionsPath, std::filesystem::path saveRoot,
                               std::optional<TestSceneOptions> testScene)
    : impl_(std::make_unique<Impl>(std::move(shaderRoot),
                                   resourceProvider, chunkStreamer, std::move(options),
                                   std::move(optionsPath), std::move(saveRoot), testScene)) {
    impl_->initialize();
}

VulkanRenderer::~VulkanRenderer() = default;

void VulkanRenderer::run() { impl_->run(); }

} // namespace mc::render
