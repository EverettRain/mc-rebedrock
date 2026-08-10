#include "render/vulkan/VulkanRenderer.hpp"
#include "render/vulkan/GpuSceneBuffer.hpp"
#include "render/vulkan/BlockAtlasLayout.hpp"
#include "render/vulkan/HudRenderer.hpp"
#include "render/vulkan/WorldRenderer.hpp"
#include "render/vulkan/HudTypes.hpp"
#include "render/vulkan/WorldRenderTypes.hpp"
#include "render/vulkan/OffscreenTarget.hpp"
#include "render/vulkan/VulkanDevice.hpp"
#include "render/vulkan/TextureManager.hpp"
#include "render/vulkan/VulkanResources.hpp"

#include "animation/AnimationAssets.hpp"
#include "animation/DisplayEntityAnimation.hpp"
#include "animation/HingeAnimation.hpp"
#include "animation/ModelAnimationSystem.hpp"
#include "animation/PlayerModelAnimator.hpp"
#include "animation/SkeletalModel.hpp"
#include "assets/ImageData.hpp"
#include "audio/AudioSystem.hpp"
#include "gameplay/ChestSystem.hpp"
#include "gameplay/ContentRegistry.hpp"
#include "gameplay/CraftingSystem.hpp"
#include "gameplay/EntitySystem.hpp"
#include "gameplay/GameMode.hpp"
#include "gameplay/GameRules.hpp"
#include "gameplay/GameSession.hpp"
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
#include "gameplay/entities/CowEntity.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "gameplay/entities/PigEntity.hpp"
#include "gameplay/entities/SpeciesRenderData.hpp"
#include "persistence/SaveRepository.hpp"
#include "render/Frustum.hpp"
#include "render/ParticleSystem.hpp"
#include "render/PerspectiveCamera.hpp"
#include "render/RainSystem.hpp"
#include "render/StreamingBudget.hpp"
#include "ui/BitmapFontMetrics.hpp"
#include "ui/ButtonControl.hpp"
#include "ui/ChatHistory.hpp"
#include "ui/HudLayout.hpp"
#include "ui/Language.hpp"
#include "ui/MenuGeometry.hpp"
#include "ui/MenuSystem.hpp"
#include "ui/PageStack.hpp"
#include "ui/TextFont.hpp"
#include "ui/UiFrameData.hpp"
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

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
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

// kFramesInFlight now lives in render/vulkan/WorldRenderTypes.hpp (shared).
// Occlusion queries gate a section's opaque draw behind the depth the closer
// terrain wrote earlier in the same frame. Each in-flight frame owns a
// contiguous slot range; results are read back after the frame's fence. The
// pool stays well under Metal's 64 KiB visibility-result buffer: at 8 bytes a
// precise query, 4096 queries would sit exactly on that edge.
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
// kMenuBackgroundTint now lives in render/vulkan/HudTypes.hpp.

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
    Impl(std::filesystem::path shaderDirectory, std::filesystem::path textureDirectory,
         std::filesystem::path soundDirectory, world::ChunkStreamer& streamer,
         config::GameOptions initialOptions, std::filesystem::path initialOptionsPath,
         std::filesystem::path saveRoot, std::optional<TestSceneOptions> initialTestScene)
        : shaderRoot(std::move(shaderDirectory)), blockTextureRoot(std::move(textureDirectory)),
          optionsPath(std::move(initialOptionsPath)), saveRepository(std::move(saveRoot)),
          options(std::move(initialOptions)), testScene(initialTestScene),
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
            std::cerr << "Animation assets unavailable, using built-in clips: " << exception.what()
                      << '\n';
        }
    }

    void registerGameCommands() {
        commandDispatcher.literal("gamemode")
            .argument("mode", gameplay::command::kGameModeArgument)
            .executes([this](const gameplay::command::CommandContext& context) {
                const auto mode = context.find<gameplay::GameMode>("mode");
                if (!mode.has_value()) {
                    return gameplay::CommandResult{false, "Usage: /gamemode <survival|creative>"};
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
                gameSession.gameTimeSeconds() =
                    elapsedTicks / world::DayNightCycle::kTicksPerSecond;
                return gameplay::CommandResult{true, "Set the time to " +
                                                         std::to_string(static_cast<int>(*ticks))};
            });
        commandDispatcher.literal("give")
            .argument("item", gameplay::command::kGiveItemArgument)
            .argument("count", gameplay::command::kIntArgument)
            .executes([this](const gameplay::command::CommandContext& context) {
                const auto itemToken = context.find<std::string>("item");
                const auto count = context.find<std::int64_t>("count");
                if (!itemToken.has_value() || !count.has_value()) {
                    return gameplay::CommandResult{false, "Usage: /give <item|index> [count]"};
                }
                // GiveItemArgument guarantees the token is a catalog index or a
                // known item/block identifier. A bare number is an index into the
                // creative catalog; anything else is a block or item identifier
                // (the `rebedrock:` key, the vanilla alias, or the bare path).
                const bool numeric = std::all_of(itemToken->begin(), itemToken->end(),
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
                requested.count = static_cast<std::uint8_t>(std::min<std::int64_t>(*count, 255));
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
                        gameSession.itemEntities().spawn(gameSession.player().position(), stack,
                                                         {0.0F, 0.2F, 0.0F});
                    }
                    requested.count = static_cast<std::uint8_t>(requested.count - intended);
                }
                return gameplay::CommandResult{true,
                                               "Gave " + std::to_string(given) + "x " + identifier};
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
                    if (entity.type != nullptr && (entity.type->id().matches(*target) ||
                                                   entity.type->vanillaId().matches(*target))) {
                        gameSession.worldEntities().kill(entity.id);
                        ++killed;
                    }
                }
                if (killed == 0U) {
                    return gameplay::CommandResult{false,
                                                   "No entities of that species are spawned"};
                }
                return gameplay::CommandResult{true,
                                               "Killed " + std::to_string(killed) + "x " + *target};
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
    void playCreatureDeath(const gameplay::entities::EntityType& type,
                           glm::vec3 position) override {
        audioSystem.playCreatureDeath(type.soundProfile(), position);
    }
    void playCreatureAmbient(const gameplay::entities::EntityType& type,
                             glm::vec3 position) override {
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
            } else if (renderer->inventoryOpen &&
                       renderer->gameSession.gameMode() == gameplay::GameMode::Creative &&
                       yOffset != 0.0) {
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
            if (renderer->paused && (renderer->menuSystem.viewDistanceSliderDragging ||
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
        textures_ = TextureManager{&resources_,      device, allocator, blockTextureRoot,
                                   samplerAnisotropySupported, maximumSamplerAnisotropy};
        createDescriptorSetLayout();
        textures_.createTextureArray(options.anisotropy);
        textures_.createRainTexture();
        textures_.createBiomeTextureResources();
        loadLanguage();
        textures_.createFontTexture(fontMetrics, textFont, requiredUnicodePages(),
                                    options.forceUnicodeFont);
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
    }

    // The CPU drop target: texture mode keeps a small population only for
    // landing splashes/audio, while particle and async render the same full
    // population. MC_REBEDROCK_RAIN_COUNT overrides all.
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
        const int top = std::min(highestY, world::kWorldHeight - 1);
        const int bottom = std::max(lowestY, 0);
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
        const float rainGradient = gameSession.weatherSystem().rainGradient();
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
            interactionWorld.setChunk({0, 0}, std::move(chunk));
            world::WorldLightEngine lighting;
            const std::array positions{world::ChunkPosition{0, 0}};
            lighting.initializeChunks(interactionWorld, positions);
            for (const int sectionY : {1, 2}) {
                world::SectionMeshUpdate update;
                update.position = {0, sectionY, 0};
                update.mesh = world::ChunkMesher::buildSection(interactionWorld, {0, 0}, sectionY);
                update.revision = static_cast<std::uint64_t>(sectionY);
                pendingSectionOrder.push_back(update.position);
                latestSectionRevisions.insert_or_assign(update.position, update.revision);
                pendingSectionUpdates.insert_or_assign(update.position, std::move(update));
            }
            loadedCpuChunkCount = 1U;
            // The camera follows the gameSession.player()'s eye, so pin the gameSession.player()
            // just above the platform surface (y=47), looking along +Z at the scene.
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
            world::BlockOrientation::Up,    world::BlockOrientation::Down};
        world::Chunk chunk;
        chunk.setBlock(blockPosition.x, blockPosition.y, blockPosition.z, testScene->block);
        chunk.setOrientation(
            blockPosition.x, blockPosition.y, blockPosition.z,
            orientations[static_cast<std::size_t>(testScene->stage) % orientations.size()]);
        interactionWorld.setChunk({0, 0}, std::move(chunk));
        world::WorldLightEngine lighting;
        const std::array positions{world::ChunkPosition{0, 0}};
        lighting.initializeChunks(interactionWorld, positions);
        world::SectionMeshUpdate update;
        update.position = {0, blockPosition.y / world::kSectionSize, 0};
        update.mesh =
            world::ChunkMesher::buildSection(interactionWorld, {0, 0}, update.position.sectionY);
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
                    static_cast<float>(std::max(swapchainExtent.height, 1U)),
                    menuSystem.guiScaleSetting};
                const auto cursor = currentFramebufferCursor();
                const auto preview = animationLayout.playerPreview(gameSession.gameMode() ==
                                                                   gameplay::GameMode::Creative);
                playerModelAnimator.setCursorLook(
                    (cursor.x - preview.lookOrigin.x) / (40.0F * animationLayout.scale()),
                    (cursor.y - preview.lookOrigin.y) / (40.0F * animationLayout.scale()));
            }
            const glm::vec2 horizontalVelocity{gameSession.player().velocity().x,
                                               gameSession.player().velocity().z};
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
                if (angle < 0.0F)
                    angle += 2.0F * pi;
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
            worldPlayerAnimator.update(deltaSeconds, playerWalking,
                                       gameSession.player().sneaking());
            if (!paused && worldReady) {
                if (gameSession.gameRules().get<bool>(gameplay::GameRuleId::DoDaylightCycle)) {
                    gameSession.gameTimeSeconds() += static_cast<double>(deltaSeconds);
                }
                heldItemAnimation.update(deltaSeconds);
                hud_.updateVignetteDarkness(deltaSeconds);
                particleSystem.update(deltaSeconds, interactionWorld);
                // CPU rain drops follow the smoothed weather gradient and drive
                // landing splashes/audio in every mode. Particle and async also
                // render these exact drops; texture mode independently draws the
                // vanilla precipitation-column field.
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
                                  gameSession.weatherSystem().rainGradient(), rainTargetCount(),
                                  interactionWorld, wind);
                if (rainMode_ == RainMode::Texture) {
                    rainSystem.emitTextureImpacts(
                        deltaSeconds, camera.position(),
                        gameSession.weatherSystem().rainGradient(), interactionWorld);
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
                (gameSession.physicsCurrentPosition() - gameSession.physicsPreviousPosition()) *
                    physicsAlpha;
            // Capture the HUD snapshot from the settled session state.
            uiFrameData_.health = gameSession.vitals().health();
            uiFrameData_.foodLevel = gameSession.vitals().foodLevel();
            uiFrameData_.airTicks = gameSession.vitals().airTicks();
            uiFrameData_.ticksSinceDamage = gameSession.vitals().ticksSinceDamage();
            uiFrameData_.gameMode = gameSession.gameMode();
            uiFrameData_.eating = gameSession.eating();
            uiFrameData_.selectedStack = gameSession.inventory().selectedStack();
            uiFrameData_.selectedHotbarSlot = gameSession.inventory().selectedHotbarSlot();
            camera.setPosition(renderedFeetPosition +
                               glm::vec3{0.0F, gameSession.player().eyeHeight(), 0.0F});
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
                gameSession.player().setPosition(
                    stressPos - glm::vec3{0.0F, gameSession.player().eyeHeight(), 0.0F});
            }
            // GameRenderer#getFov: the base FOV times the gameSession.player()'s movement
            // multiplier, interpolated across the physics tick the same way the
            // eye position is. Sprinting widens it to 1.15x, creative flight to
            // 1.1x, and both ease in over a few ticks.
            const float fovMultiplier = gameSession.player().previousFieldOfViewMultiplier() +
                                        (gameSession.player().fieldOfViewMultiplier() -
                                         gameSession.player().previousFieldOfViewMultiplier()) *
                                            physicsAlpha;
            camera.setFieldOfViewDegrees(baseFieldOfViewDegrees * fovMultiplier);
            audioSystem.updateListener(camera.position(), camera.direction(), {0.0F, 1.0F, 0.0F});
            audioSystem.update();
            if (worldSessionActive)
                world_.processChunkStreaming();
            updateBlockInteraction();
            world_.updateItemDrop();
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
            if (smokeTest && smokeGameplayFrames == 16U) {
                setInventoryOpen(true);
            } else if (smokeTest && smokeGameplayFrames == 20U) {
                gameSession.inventory().clickCreativeItem(
                    {world::Block::Air, 1U, &gameplay::items::Diamond},
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
                gameSession.itemEntities().spawn(
                    gameSession.player().position() + glm::vec3{1.8F, 1.0F, 0.0F},
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
            if (position.sectionY < 0 || position.sectionY >= world::kSectionCount)
                return;
            if (!interactionWorld.hasChunk({position.chunkX, position.chunkZ}))
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
                    if (sampleY < 0 || sampleY >= world::kWorldHeight)
                        continue;
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
            const world::Chunk* chunk = interactionWorld.chunk({position.chunkX, position.chunkZ});
            if (chunk != nullptr && !chunk->section(position.sectionY).empty()) {
                mark({position.chunkX, position.sectionY, position.chunkZ});
            }
        }

        if (sections.empty())
            return;
        // Cheap sampler *view*: O(1) reads of the light we just propagated into
        // interactionWorld above. (The per-chunk constructor re-propagates a
        // ~48x256x48 region with two BFS passes and must not run per edit here.)
        const world::ChunkLightSampler lighting{interactionWorld};
        for (const auto position : sections) {
            world_.remeshSectionImmediate(position, lighting);
        }
    }

    void clearRenderedWorld() {
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
        gameSession.gameRules().setChangeHandler(
            [this](gameplay::GameRuleId id, const gameplay::GameRuleValueData& value) {
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
            gameSession.worldEntities().restore({record.x, record.y, record.z}, *type, record.yaw,
                                                {record.vx, record.vy, record.vz}, record.health,
                                                record.angerTicks, record.ageTicks,
                                                record.rngState);
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
        chunkStreamer.protectChunks(world::chunkPositionFromWorld(initialFeet.x, initialFeet.z),
                                    kSpawnChunkRadius);
        worldEpoch = chunkStreamer.resetWorld(currentSave->summary.seed, currentSave->edits);
        textures_.updateBiomeColorTextures(currentSave->summary.seed);
        // Natural spawning reads the biome map from the same seed that drives
        // the terrain, so spawns follow the biome being generated.
        gameSession.setWorldSeed(currentSave->summary.seed);
        gameSession.lootRandomState() =
            static_cast<std::uint32_t>(currentSave->summary.seed) ^
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
        currentSave->chests.assign(gameSession.chestSystem().entities().begin(),
                                   gameSession.chestSystem().entities().end());
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
            gameSession.input().flightAllowed =
                gameSession.gameMode() == gameplay::GameMode::Creative;
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
        gameSession.input().sprintAllowed =
            gameSession.input().flightAllowed || gameSession.vitals().foodLevel() > 6;
        // Bedrock-style auto-jump: walking forward into a one-block rise jumps
        // on its own; the gameSession.player() physics decides when the obstacle is jumpable.
        gameSession.input().autoJump = options.autoJump;
    }

    // The gameSession.player()'s external damage entry: any hit the world deals to the
    // gameSession.player() routes through the shared gameSession.vitals() pipeline, then raises the
    // death screen on the tick it kills — the gameSession.player()'s own onDeath handler.

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
        chunkStreamer.request(world::chunkPositionFromWorld(gameSession.worldSpawnPosition().x,
                                                            gameSession.worldSpawnPosition().z));
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
        return gameplay::CommandResult{true, "Set the spawn point to " +
                                                 std::to_string(static_cast<int>(spawn.x)) + " " +
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

    [[nodiscard]] MenuButton hoveredMenuButton() const {
        const auto cursor = currentFramebufferCursor();
        const ui::HudLayout layout{static_cast<float>(swapchainExtent.width),
                                   static_cast<float>(swapchainExtent.height),
                                   menuSystem.guiScaleSetting};
        const std::size_t buttonCount = menuButtonCount();
        for (std::size_t index = 0; index < buttonCount; ++index) {
            const auto rectangle =
                frontendButtonRect(layout, menuSystem.pageStack.current(), index, buttonCount);
            if (rectangle.contains(cursor.x, cursor.y)) {
                return hud_.menuButtonForIndex(index);
            }
        }
        return MenuButton::None;
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
                                       static_cast<float>(swapchainExtent.height),
                                       menuSystem.guiScaleSetting};
            const std::size_t visible = languageVisibleRowCount();
            const std::size_t maximumFirst = menuSystem.languageCodes.size() > visible
                                                 ? menuSystem.languageCodes.size() - visible
                                                 : 0U;
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
        menuSystem.resolutionIndex =
            (menuSystem.resolutionIndex + 1U) % ui::kDisplayResolutions.size();
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
        if (activatedButton != MenuButton::None && activatedButton != MenuButton::ViewDistance &&
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
            const std::size_t current =
                found == limits.end()
                    ? 0U
                    : static_cast<std::size_t>(std::distance(limits.begin(), found));
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
                currentSave->difficulty = gameplay::nextDifficulty(currentSave->difficulty);
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
                    startWorld(saveRepository.load(
                        menuSystem.saveSummaries[menuSystem.selectedWorldIndex].identifier));
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
                menuSystem.editWorldIdentifier =
                    menuSystem.saveSummaries[menuSystem.selectedWorldIndex].identifier;
                menuSystem.editWorldName =
                    menuSystem.saveSummaries[menuSystem.selectedWorldIndex].displayName;
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
            menuSystem.createWorldGameMode =
                menuSystem.createWorldGameMode == gameplay::GameMode::Survival
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
                                   static_cast<float>(framebufferHeight),
                                   menuSystem.guiScaleSetting};
        // Inventory and container slots are silent in vanilla: only actual
        // buttons (AbstractButtonWidget) play ui.button.click, so picking up
        // or moving an item never clicks. The menu buttons above are the only
        // sound in this screen family.
        if (containerScreen == ContainerScreen::Chest && activeChest.has_value()) {
            for (std::size_t index = 0; index < gameplay::ChestBlockEntity::kSlotCount; ++index) {
                if (layout.chestSlot(index).contains(framebufferCursor.x, framebufferCursor.y)) {
                    gameSession.chestSystem().clickSlot(*activeChest, index,
                                                        gameSession.inventory(), button, shiftHeld);
                    return;
                }
            }
        } else if (containerScreen == ContainerScreen::CraftingTable) {
            for (std::size_t index = 0; index < 9U; ++index) {
                if (layout.tableCraftingSlot(index).contains(framebufferCursor.x,
                                                             framebufferCursor.y)) {
                    gameSession.craftingSystem().clickTableSlot(gameSession.inventory(), index,
                                                                button, shiftHeld);
                    return;
                }
            }
            if (layout.tableCraftingOutput().contains(framebufferCursor.x, framebufferCursor.y)) {
                // Shift-click is QUICK_MOVE: the whole result goes to the gameSession.player()
                // gameSession.inventory() instead of the cursor, like vanilla's result slot.
                static_cast<void>(
                    gameSession.craftingSystem().craftTable(gameSession.inventory(), shiftHeld));
                return;
            }
        } else if (containerScreen == ContainerScreen::Furnace) {
            if (layout.furnaceInputSlot().contains(framebufferCursor.x, framebufferCursor.y)) {
                gameSession.craftingSystem().clickFurnaceInput(gameSession.inventory(), button,
                                                               shiftHeld);
                return;
            }
            if (layout.furnaceFuelSlot().contains(framebufferCursor.x, framebufferCursor.y)) {
                gameSession.craftingSystem().clickFurnaceFuel(gameSession.inventory(), button,
                                                              shiftHeld);
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
                    gameSession.craftingSystem().clickPlayerSlot(gameSession.inventory(), index,
                                                                 button, shiftHeld);
                    return;
                }
            }
            if (layout.playerCraftingOutput().contains(framebufferCursor.x, framebufferCursor.y)) {
                // Shift-click is QUICK_MOVE: the whole result goes to the gameSession.player()
                // gameSession.inventory() instead of the cursor, like vanilla's result slot.
                static_cast<void>(
                    gameSession.craftingSystem().craftPlayer(gameSession.inventory(), shiftHeld));
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
                        // QUICK_MOVE from the gameSession.player() gameSession.inventory() into the
                        // open container, the reverse of the container-slot shift.
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
                    gameSession.inventory().clickCreativeItem(catalog[catalogIndex], button,
                                                              shiftHeld);
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
        const auto cursor = ui::windowToFramebuffer(cursorX, cursorY, windowWidth, windowHeight,
                                                    framebufferWidth, framebufferHeight);
        const ui::HudLayout layout{static_cast<float>(framebufferWidth),
                                   static_cast<float>(framebufferHeight),
                                   menuSystem.guiScaleSetting};
        return dragSlotAt(layout, cursor);
    }

    // Every slot the gameSession.player() can reach in the current screen — the container's
    // input slots plus the full gameSession.player() gameSession.inventory() — for the PICKUP_ALL
    // gather.
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

    // QUICK_MOVE's gameSession.inventory() direction for a gameSession.player() slot while a
    // container is open: hand the stack to the open container, which decides where it goes.
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
    [[nodiscard]] gameplay::ItemStack* dragSlotAt(const ui::HudLayout& layout,
                                                  const ui::UiPoint& cursor) {
        if (containerScreen == ContainerScreen::Chest && activeChest.has_value()) {
            auto* chest = gameSession.chestSystem().find(*activeChest);
            if (chest != nullptr) {
                for (std::size_t index = 0; index < gameplay::ChestBlockEntity::kSlotCount;
                     ++index) {
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
        const bool creativePlayerScreen = containerScreen == ContainerScreen::PlayerInventory &&
                                          gameSession.gameMode() == gameplay::GameMode::Creative;
        for (std::size_t index = 0; index < gameplay::Inventory::kSlotCount; ++index) {
            if (creativePlayerScreen && menuSystem.creativeTab != ui::CreativeTab::Inventory &&
                index >= gameplay::Inventory::kHotbarSize) {
                continue;
            }
            const auto slot =
                containerScreen == ContainerScreen::Chest ? layout.chestInventorySlot(index)
                : creativePlayerScreen ? (menuSystem.creativeTab == ui::CreativeTab::Inventory
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
    [[nodiscard]] std::optional<ui::UiRect>
    dragSlotRectangle(const ui::HudLayout& layout, const gameplay::ItemStack* slot) const {
        if (slot == nullptr) {
            return std::nullopt;
        }
        if (containerScreen == ContainerScreen::Chest && activeChest.has_value()) {
            if (const auto* chest = gameSession.chestSystem().find(*activeChest);
                chest != nullptr) {
                for (std::size_t index = 0; index < gameplay::ChestBlockEntity::kSlotCount;
                     ++index) {
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
        const bool creativePlayerScreen = containerScreen == ContainerScreen::PlayerInventory &&
                                          gameSession.gameMode() == gameplay::GameMode::Creative;
        for (std::size_t index = 0; index < gameplay::Inventory::kSlotCount; ++index) {
            if (creativePlayerScreen && menuSystem.creativeTab != ui::CreativeTab::Inventory &&
                index >= gameplay::Inventory::kHotbarSize) {
                continue;
            }
            const auto rect =
                containerScreen == ContainerScreen::Chest ? layout.chestInventorySlot(index)
                : creativePlayerScreen ? (menuSystem.creativeTab == ui::CreativeTab::Inventory
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
            return target->empty() || (gameplay::sameItem(*target, cursor) &&
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
            if (accepts(target))
                ++fillable;
        }
        if (fillable == 0U) {
            return counts;
        }
        const auto maximum = gameplay::itemMaximumStackSize(cursor);
        std::uint8_t perSlot = static_cast<std::uint8_t>(cursor.count / fillable);
        std::uint8_t extra = static_cast<std::uint8_t>(cursor.count % fillable);
        for (std::size_t index = 0; index < inventoryDragSlots.size(); ++index) {
            gameplay::ItemStack* target = inventoryDragSlots[index];
            if (!accepts(target))
                continue;
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
        const auto cursor = ui::windowToFramebuffer(windowX, windowY, windowWidth, windowHeight,
                                                    framebufferWidth, framebufferHeight);
        const ui::HudLayout layout{static_cast<float>(framebufferWidth),
                                   static_cast<float>(framebufferHeight),
                                   menuSystem.guiScaleSetting};
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
        }
        vulkanDevice_.destroy();
        if (window != nullptr) {
            glfwDestroyWindow(window);
        }
        if (glfwInitialized) {
            glfwTerminate();
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
            if (!interactionWorld.hasChunk(world::chunkPositionFromWorld(24.0F, 24.0F))) {
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
        chunkStreamer.protectChunks(world::chunkPositionFromWorld(feet.x, feet.z),
                                    kSpawnChunkRadius);
        std::cout << "Spawn position: " << feet.x << "," << feet.y << "," << feet.z << '\n';
    }

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
    [[nodiscard]] bool attackTargetedEntity(const gameplay::EntityRayHit& hit) {
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
        if (gameSession.worldEntities().hurt(hit.entityId, damage, camera.position())) {
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
        const float blockReach =
            gameSession.gameMode() == gameplay::GameMode::Creative ? 5.0F : 4.5F;
        targetedBlock = world::raycastVoxels(interactionWorld, camera.position(),
                                             camera.direction(), blockReach, collectingWater);
        // A creature's collision box blocks the ray exactly like a block's shape
        // (vanilla's HitResult is the nearest of block-or-entity): looking past a
        // mob never reveals — let alone digs or places through — the block behind
        // it. Same reach as the block pick, so a mob just inside the pick range
        // still shields its backdrop. Runs every frame, not just on the click
        // edge, so a held dig cannot keep carving through a creature.
        const auto creatureHit =
            gameSession.worldEntities().raycast(camera.position(), camera.direction(), blockReach);
        const bool creatureIsNearest =
            creatureHit.has_value() &&
            (!targetedBlock.has_value() || creatureHit->distance < targetedBlock->distance);
        if (creatureIsNearest) {
            targetedBlock.reset();
        }
        // A container the click would open wins over a meal; otherwise holding
        // food starts (or keeps) the vanilla 32-tick eat, independently of the
        // 4-tick rightClickDelay. Attacking during a meal cancels it.
        const bool targetedContainer = targetedBlock.has_value() && [&] {
            const auto block = interactionWorld.block(
                targetedBlock->block.x, targetedBlock->block.y, targetedBlock->block.z);
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
            const auto interacted = interactionWorld.block(
                targetedBlock->block.x, targetedBlock->block.y, targetedBlock->block.z);
            const glm::ivec3 placeTarget =
                world::isReplaceable(interacted) ? targetedBlock->block : targetedBlock->adjacent;
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
        // Reuse the exact entity/block winner computed above. Re-running a
        // second spatial query here could disagree with the target that already
        // suppressed block mining, allowing one click to hurt an entity and
        // continue into the floor behind it.
        const bool struckEntity =
            breakBlockRequested && creatureIsNearest && creatureHit.has_value() &&
            creatureHit->distance <= gameplay::EntitySystem::kAttackReach &&
            attackTargetedEntity(*creatureHit);
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
                performBreak =
                    attackActive && gameSession.gameTimeSeconds() >= nextCreativeBreakSeconds;
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
                const float duration =
                    gameplay::miningSeconds(target, selectedStack, gameSession.player().inWater(),
                                            !gameSession.player().onGround());
                performBreak = gameSession.gameTimeSeconds() - miningStartedAt >= duration;
                if (!performBreak && miningTarget.has_value() &&
                    (lastMiningSoundAt < 0.0 ||
                     gameSession.gameTimeSeconds() - lastMiningSoundAt >= 0.20)) {
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
            const auto brokenOrientation = interactionWorld.orientation(block.x, block.y, block.z);
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
                    const auto removed =
                        gameSession.chestSystem().remove({block.x, block.y, block.z});
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
                    gameSession.spawnBlockDrops({block.x, block.y, block.z}, brokenBlock,
                                                selectedStack, brokenOrientation);
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
        const bool performUse =
            useActive && gameSession.gameTimeSeconds() >= nextUseSeconds && !gameSession.eating();
        if (performUse) {
            nextUseSeconds =
                gameSession.gameTimeSeconds() + 4.0 * gameplay::PlayerController::kTickSeconds;
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
                openChest({targetedBlock->block.x, targetedBlock->block.y, targetedBlock->block.z});
                break;
            default: {
                // Item#useOn: the held item decides what right-clicking does,
                // resolved by its own class instead of a switch in this loop.
                // The item answers with the outcome; the side effects (world
                // edit, audio, animation) are applied below.
                const world::PlacementContext placement{
                    targetedBlock->block,
                    placeTarget,
                    world::orientationFromOffset(targetedBlock->adjacent - targetedBlock->block),
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
                    if (interactionWorld.setBlock(block.x, block.y, block.z, world::Block::Air)) {
                        submitWorldEdit(block.x, block.y, block.z, world::Block::Air);
                        previewBlockEdit(block.x, block.y, block.z);
                        gameSession.worldSimulation().notifyNeighborChanged(
                            interactionWorld, {block.x, block.y, block.z});
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
                    if (interactionWorld.setBlock(block.x, block.y, block.z, world::Block::Water)) {
                        interactionWorld.setFluidLevel(block.x, block.y, block.z, 0U);
                        submitWorldEdit(block.x, block.y, block.z, world::Block::Water, 0U);
                        previewBlockEdit(block.x, block.y, block.z);
                        gameSession.worldSimulation().notifyPlaced({block.x, block.y, block.z},
                                                                   world::Block::Water);
                        gameSession.worldSimulation().notifyNeighborChanged(
                            interactionWorld, {block.x, block.y, block.z});
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
                        const glm::vec3 spawnPosition{
                            static_cast<float>(block.x) + 0.5F,
                            static_cast<float>(block.y) + 0.02F,
                            static_cast<float>(block.z) + 0.5F};
                        if (entityModelReady(&eggType) &&
                            gameplay::EntitySystem::canOccupy(
                                interactionWorld, spawnPosition, eggType.dimensions())) {
                            gameSession.worldEntities().spawn(spawnPosition, eggType);
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
                         (!gameSession.player().intersectsBlock(block.x, block.y, block.z) &&
                          !gameSession.worldEntities().intersectsBlock(
                              block.x, block.y, block.z))) &&
                        interactionWorld.setBlock(block.x, block.y, block.z, placedBlock)) {
                        interactionWorld.setOrientation(block.x, block.y, block.z, use.orientation);
                        submitWorldEdit(block.x, block.y, block.z, placedBlock, 0U,
                                        use.orientation);
                        previewBlockEdit(block.x, block.y, block.z);
                        gameSession.worldSimulation().notifyPlaced({block.x, block.y, block.z},
                                                                   placedBlock);
                        audioSystem.playBlockPlace(placedBlock,
                                                   {static_cast<float>(block.x) + 0.5F,
                                                    static_cast<float>(block.y) + 0.5F,
                                                    static_cast<float>(block.z) + 0.5F});
                        heldItemAnimation.trigger(animation::ModelAction::Use);
                        if (placedBlock == world::Block::Chest) {
                            static_cast<void>(
                                gameSession.chestSystem().place({block.x, block.y, block.z}));
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
                        interactionWorld.setOrientation(block.x, block.y, block.z,
                                                        world::farmlandOrientation(0));
                        submitWorldEdit(block.x, block.y, block.z, tilled, 0U,
                                        world::farmlandOrientation(0));
                        previewBlockEdit(block.x, block.y, block.z);
                        audioSystem.playBlockPlace(tilled, {static_cast<float>(block.x) + 0.5F,
                                                            static_cast<float>(block.y) + 0.5F,
                                                            static_cast<float>(block.z) + 0.5F});
                        heldItemAnimation.trigger(animation::ModelAction::Use);
                        gameSession.worldSimulation().notifyPlaced({block.x, block.y, block.z},
                                                                   tilled);
                        if (gameSession.gameMode() == gameplay::GameMode::Survival) {
                            if (gameSession.damageHeldTool(
                                    gameplay::ToolUse::Till,
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
        gameSession.itemEntities().spawn(gameSession.player().eyePosition() + direction * 0.45F,
                                         stack, direction * 0.28F + glm::vec3{0.0F, 0.12F, 0.0F});
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
        if (!options.viewBobbing || gameSession.player().flying())
            return glm::mat4{1.0F};
        const float alpha = renderInterpolationAlpha;
        const float phase = -std::lerp(gameSession.player().previousHorizontalSpeed(),
                                       gameSession.player().horizontalSpeed(), alpha);
        const float stride = std::lerp(gameSession.player().previousStrideDistance(),
                                       gameSession.player().strideDistance(), alpha);
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
        const glm::mat4 baseView =
            glm::lookAt(renderEye.position, renderEye.position + renderEye.forward,
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
        uniform.horizonFog = glm::vec4{
            daylight.horizonColor,
            static_cast<float>(std::fmod(gameSession.gameTimeSeconds(), kLunarCycleSeconds))};
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

    // Block names still localize through vanilla's translation keys; item names
    // come straight from the English/Chinese strings each item was registered
    // with, picked by the active language.
    [[nodiscard]] std::filesystem::path localizationRoot() const {
        return blockTextureRoot.parent_path().parent_path().parent_path() / "localization" /
               "minecraft";
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

    [[nodiscard]] bool entityModelReady(const gameplay::entities::EntityType* type) const {
        const auto* species = world_.speciesFor(type);
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
    void drawFrame() {
        auto& frame = frames[currentFrame];
        checkVk(vkWaitForFences(device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX),
                "vkWaitForFences");
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
        world_.updateShadowMatrix();
        updateUniform(frame);
        checkVk(vkResetFences(device, 1, &frame.inFlight), "vkResetFences");
        checkVk(vkResetCommandBuffer(frame.commandBuffer, 0), "vkResetCommandBuffer");
        const std::size_t visibleCount = world_.recordCommandBuffer(frame, imageIndex);
        const std::string movementMode =
            gameSession.player().flying()
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
    // The gameSession.inventory() preview gameSession.player() and the in-world third-person
    // gameSession.player() use separate animator instances driven by different inputs (cursor vs.
    // the gameSession.player()'s own look/movement).
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
    // The language screen's entries: each language's name in its own language,
    // e.g. "English (United States)" / "简体中文 (中国)", plus the list's scroll
    // offset. Built lazily when the screen opens.
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

    // Wires the extracted HudRenderer to this Impl's state: reference fields
    // bind directly to members (so resize-recreated pipelines and input-mutated
    // UI flags stay visible), and a few std::function hooks keep the world-render
    // couplings (held item, submerged test, model-preview descriptor) in Impl.
    [[nodiscard]] HudRenderer::Bindings makeHudBindings() {
        return HudRenderer::Bindings{
            .menuSystem = menuSystem,
            .uiFrameData_ = uiFrameData_,
            .gameSession = gameSession,
            .textFont = textFont,
            .fontMetrics = fontMetrics,
            .language = language,
            .interactionWorld = interactionWorld,
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
            .containerScreen = containerScreen,
            .activeChest = activeChest,
            .debugOverlayOpen = debugOverlayOpen,
            .inventoryDragActive = inventoryDragActive,
            .inventoryDragSlots = inventoryDragSlots,
            .chatOpen = chatOpen,
            .chatHistory = chatHistory,
            .chatInputText = chatInputText,
            .chatSuggestions_ = chatSuggestions_,
            .chatSuggestionIndex_ = chatSuggestionIndex_,
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
            .paused = paused,
            .uiTimeSeconds = uiTimeSeconds,
            .cameraSubmergedInWater = [this] { return cameraSubmergedInWater(); },
            .drawHeldItem = [this](VkCommandBuffer c, VkDescriptorSet d) { world_.drawHeldItem(c, d); },
            .currentFrameDescriptorSet = [this] { return frames[currentFrame].descriptorSet; },
            .activeCreativeCatalog = [this] { return activeCreativeCatalog(); },
            .creativeScrollPosition = [this] { return creativeScrollPosition(); },
            .creativeMaximumScrollRow = [this] { return creativeMaximumScrollRow(); },
            .dragPlacementCounts = [this] { return dragPlacementCounts(); },
            .cameraFarPlane = [this] { return cameraFarPlane(); },
            .dragSlotRectangle = [this](const ui::HudLayout& l, const gameplay::ItemStack* s) { return dragSlotRectangle(l, s); },
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
            .gpuMeshes = gpuMeshes,
            .deviceBufferPool_ = deviceBufferPool_,
            .stagingBufferPool_ = stagingBufferPool_,
            .occlusionQueryPool = occlusionQueryPool,
            .occlusionQueryPipeline = occlusionQueryPipeline,
            .occlusionQueryLayout = occlusionQueryLayout,
            .occlusionBoxVertexBuffer = occlusionBoxVertexBuffer,
            .occlusionBoxIndexBuffer = occlusionBoxIndexBuffer,
            .pendingSectionOrder = pendingSectionOrder,
            .currentMeshQuality = currentMeshQuality,
            .targetMeshQuality = targetMeshQuality,
            .qualityRemeshPending = qualityRemeshPending,
            .gameSession = gameSession,
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
            .breakButtonHeld = breakButtonHeld,
            .inventoryOpen = inventoryOpen,
            .spawnPositionInitialized = spawnPositionInitialized,
            .worldReady = worldReady,
            .paused = paused,
            .dropRequested = dropRequested,
            .dropWholeStack = dropWholeStack,
            .chatOpen = chatOpen,
            .targetedBlock = targetedBlock,
            .miningTarget = miningTarget,
            .miningStartedAt = miningStartedAt,
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
            .spawnDroppedStack = [this](gameplay::ItemStack s) { spawnDroppedStack(std::move(s)); },
            .initializeSpawnPosition = [this] { initializeSpawnPosition(); },
            .submitWorldEditFn = [this](int x, int y, int z, world::Block b, std::uint8_t f, std::optional<world::BlockOrientation> o) { submitWorldEdit(x, y, z, b, f, o); },
        };
    }

    // Declared after hud_ so its Bindings can reference hud_; world render pass.
    WorldRenderer world_{makeWorldBindings()};
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
