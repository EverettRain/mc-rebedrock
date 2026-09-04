#include "render/vulkan/VulkanRenderer.hpp"
#include "render/vulkan/BlockAtlasLayout.hpp"
#include "render/vulkan/GpuSceneBuffer.hpp"
#include "render/vulkan/HudRenderer.hpp"
#include "render/vulkan/HudTypes.hpp"
#include "render/vulkan/OffscreenTarget.hpp"
#include "render/vulkan/SceneReadback.hpp"
#include "render/vulkan/TextureManager.hpp"
#include "render/vulkan/VulkanDevice.hpp"
#include "render/vulkan/VulkanResources.hpp"
#include "render/vulkan/WorldRenderTypes.hpp"
#include "render/vulkan/WorldRenderer.hpp"

#include "render/BlockPreviewCamera.hpp"

#include "core/EnvFlags.hpp"
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
#include "input/ScreenMode.hpp"
#include "input/InputNaming.hpp"
#include "input/InputSystem.hpp"
#include "input/KeyBindingScreen.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "gameplay/entities/SpeciesRenderData.hpp"
#include "persistence/SaveRepository.hpp"
#include "render/Frustum.hpp"
#include "render/SmokeScript.hpp"
#include "render/vulkan/SmokeScriptSteps.hpp"
#include "runtime/GameRuntime.hpp"
#include "render/BlockAnimateTick.hpp"
#include "render/ParticleSystem.hpp"
#include "render/PerspectiveCamera.hpp"
#include "render/player/PlayerRenderState.hpp"
#include "render/RainSystem.hpp"
#include "render/SkyLight.hpp"
#include "render/StreamingBudget.hpp"
#include "ui/BitmapFontMetrics.hpp"
#include "ui/ButtonControl.hpp"
#include "ui/ChatHistory.hpp"
#include "ui/HudLayout.hpp"
#include "ui/Language.hpp"
#include "ui/MenuGeometry.hpp"
#include "ui/MenuInteraction.hpp"
#include "ui/MenuSystem.hpp"
#include "ui/OptionCycle.hpp"
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

// 生物音效走哪条音量总线由它的刷怪分类决定
// 怪物走 Hostile，Ambient 分类的蝙蝠走 Ambient，其余动物走 Neutral
// 这对应 26.1 里 Entity#getSoundSource 默认 NEUTRAL、Monster 覆写为 HOSTILE 的划分
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
    // macOS 27 加 MoltenVK 1.4.2 在持续的遮挡查询流量下会丢掉 Apple GPU
    // Boolean 和精确模式都一样，几秒到几分钟内就会发生
    // Vulkan 校验层和 Metal API 校验都是干净的；改成同步提交能掩盖，只关掉活动查询则能彻底消除
    // 这条路径保持 opt-in，只给受控的驱动回归测试用，不给玩家
    return std::getenv("MC_REBEDROCK_FORCE_OCCLUSION") == nullptr;
#else
    return false;
#endif
}

// 打开世界时先只加载出生点附近一小片区块，vanilla 同样是先进小片再边玩边流送视距
// 这样大视距不会让加载画面卡在整个 (2·radius+1)² 片区上，玩家迟迟不能动
constexpr int kSpawnChunkRadius = 4;

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

// F5 循环视角：第一人称 → 玩家背后第三人称 → 玩家前方回看第三人称
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

// 地形/实体着色器一帧内能轮播的非流体动画方块纹理数上限
// 当前只用到一个（岩浆块），16 给后续的海晶石、海晶灯等留足余量
inline constexpr std::size_t kMaxBlockAnimations = 16;

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
    // x = 太阳所在图集层，y = 月相的首层
    // 特殊区的布局在启动时算出，天空着色器从 uniform 读真实层号而不是写死数字
    // 图集一改，写死的数字就过期了
    alignas(16) glm::vec4 celestialLayers{0.0F};
    // x/y = 逐帧插值后的雨/雷强度，z = vanilla 的视觉天光系数，w = 天体可见度 (1 - 雨)
    // 纯表现量：世界与网格里的光照等级不受影响
    alignas(16) glm::vec4 weatherSettings{0.0F};
    // 流体动画的图集契约
    // 起始层与帧数由 CPU 侧布局这一唯一事实源提供，避免图集改动后 GLSL 里的字面量悄悄失配
    alignas(16) glm::vec4 fluidAnimationLayers{0.0F};
    alignas(16) glm::vec4 fluidAnimationFrameCounts{0.0F};
    alignas(16) glm::vec4 fluidAnimationFrameTimes{1.0F};
    // x = 含渲染插值小数部分的模拟动画 tick
    alignas(16) glm::vec4 fluidAnimationSettings{0.0F};
    // 阴影预通道写深度图所用的太阳空间视图投影矩阵；地形着色器把每个片元投进去采样
    // 它比预通道自己的矩阵晚一帧，这是阴影贴图的常规延迟
    alignas(16) glm::mat4 lightViewProj{1.0F};
    // 地形/实体着色器轮播的非流体动画方块纹理
    // blockAnimationSettings.x 是生效条数
    // 每个 blockAnimations[i] 依次是首层、帧数、每帧 tick 和一个未用分量
    // 追加在 lightViewProj 之后，已有的 UBO 偏移因此不受影响
    // 只读取更早字段的阴影与 cutout 着色器同样不受影响
    alignas(16) glm::vec4 blockAnimationSettings{0.0F};
    alignas(16) std::array<glm::vec4, kMaxBlockAnimations> blockAnimations{};
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

} // namespace

struct VulkanRenderer::Impl final : public gameplay::SimulationHost {
    Impl(std::filesystem::path shaderDirectory, const assets::ResourceProvider& provider, world::ChunkStreamer& streamer,
         config::GameOptions initialOptions, std::filesystem::path initialOptionsPath,
         std::filesystem::path saveRoot, std::optional<TestSceneOptions> initialTestScene)
        : shaderRoot(std::move(shaderDirectory)),
          resourceProvider(&provider), languageLoader(provider),
          optionsPath(std::move(initialOptionsPath)),
          // `provider` 是 Application 在 `bundled` 之上叠好的资源栈
          // 它同时充当集成式运行时的数据包底座
          // provider 按 PackType 把查询映射到 `assets/` 或 `data/`，一个 provider 同时服务两半
          // 纯 assets 的资源包对数据半边毫无贡献，那一半落到内置默认值
          // 而同时带 `assets/` 与 `data/` 的**合并包**，其结构、战利品、配方、标签也从这里加载
          // 这是有意为之，单人玩家只需往 resourcepacks/ 放一个包就同时拿到纹理和服务端数据
          // 各存档的 <save>/datapacks/ 仍叠在其上（逐存档覆盖）
          // 这条路径仅限集成式运行，专用服务器改用纯 DirectoryResourceProvider 作底座
          // 客户端资源包因此绝无可能把服务端数据注入服务器
          // **不要**把集成式的数据底座与资源栈硬拆开，否则合并包就失效了
          runtime(*this, streamer, std::move(saveRoot), &provider),
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
        // 把持久化的音频设置推给引擎：各分类子音量（主音量已在构造时传入）和方向性音频开关
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
        // 所有命令都归命令树所有
        // 经它注册，每条命令自带类型化参数、参数校验和补全
        // 物品、方块、游戏模式、规则这些参数背后的表以同一份来源同时供给补全与校验
        // 往任何一张表加一项，命令里就自动出现
        registerGameCommands();

        // 让手持物与玩家预览的动画器加载 resources/animation 下作者提供的剪辑
        // 文件缺失时两者都沿用内置剪辑，因此这里是尽力而为，绝不致命
        try {
            const auto animationRoot = resourceProvider->resourceRoot() / "animation";
            heldItemAnimation.load(animationRoot);
            playerModelAnimator.load(animationRoot);
            // 背包/创造界面的预览会整个身体朝光标转（看向剪辑让身体以头部偏航的一半跟随）
            // 世界中的玩家关掉身体跟随：它的身体已由渲染器另一套身体偏航逻辑处理
            playerModelAnimator.setBodyFollowsLook(true);
            worldPlayerAnimator.load(animationRoot);
        } catch (const std::exception& exception) {
            std::cerr << "Animation assets unavailable, using built-in clips: " << exception.what()
                      << '\n';
        }
    }

    void registerGameCommands() {
        // 物种注册**不在**这里做
        // Application 的 PerSaveDataStack::rebuildBuiltinOnly 在本渲染器构造之前就已执行
        // 每次逐存档数据包重建也会再执行一次，以实体为目标的命令从第一个世界起就能解析
        //
        // /tp 注册在这里而不是运行时侧，因为它的旋转要设置相机，而相机只有渲染器有
        // gamemode、time、give、gamerule、kill、spawnpoint、weather 都是权威命令
        // 它们挂在运行时的派发器上
        // headless 服务器才能同样执行它们
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

    // SimulationHost：游戏会话的 tick 所驱动的渲染侧反应
    // submitWorldEdit 与 previewBlockEdit 是 Impl 自己的方法，在各自定义处标了 override
    // 其余宿主方法在这里
    // 每次播放音效后，若开启了字幕就把它的无障碍字幕（如果有）送进字幕叠加层
    // 字幕为空或选项关闭时该调用是空操作，没有字幕的音效自然不显示
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
    // 玩法控制器驱动的交互副作用：这些是宿主在渲染侧承担的那一半
    void playBlockHit(world::Block block, glm::vec3 position) override {
        audioSystem.playBlockHit(block, position);
        emitLastSubtitle();
    }
    void playBlockPlace(world::Block block, glm::vec3 position) override {
        audioSystem.playBlockPlace(block, position);
        emitLastSubtitle();
    }
    void playBlockOpen(world::Block block, glm::vec3 position) override {
        audioSystem.playBlockOpen(block, position);
        emitLastSubtitle();
    }
    void playBlockClose(world::Block block, glm::vec3 position) override {
        audioSystem.playBlockClose(block, position);
        emitLastSubtitle();
    }
    void playBlockClick(world::Block block, glm::vec3 position, bool on) override {
        audioSystem.playBlockClick(block, position, on);
        emitLastSubtitle();
    }
    void playFlintAndSteelUse(glm::vec3 position) override {
        audioSystem.playFlintAndSteelUse(position);
        emitLastSubtitle();
    }
    void playShear(glm::vec3 position) override {
        audioSystem.playShear(position);
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
        // 权威的界面已由模拟侧打开并绑定
        // 本回调在主线程的事件排空中执行，只负责把表现层立起来
        // 它绝不能写玩法状态，也绝不能从模拟线程调用 GLFW
        static_cast<void>(screen);
        static_cast<void>(position);
        setInventoryOpenLocked(true);
    }
    void onPlayerDied() override {
        std::cout << "Player died\n";
        if (inventoryOpen) {
            // 玩法侧在死亡处理里已经关闭并收起权威界面，本回调只管表现层
            inventoryOpen = false;
            creativeScrollbarDragging = false;
            firstMouseSample = true;
        }
        if (chatOpen) {
            chatInput = {};
            chatOpen = false;
        }
        simulationActive.store(false, std::memory_order_release);
        paused = true;
        menuSystem.pageStack.reset(ui::PageId::Death);
        // 有界面打开时，processInput 会发一份清零的 MovementInput 让玩家停下
        // 这里只清掉客户端侧的边沿
        clearPendingInputEdges();
        releaseInteractionButtons();
        dropRequested = false;
        pressedMenuButton = ui::WidgetId::None;
        firstMouseSample = true;
        unlockCursor();
    }
    void onFurnaceStateChanged() override {}
    void onEatingStarted() override {
        // 进食挂在物品使用时间线上：beginEating 已经调用过 startUsing，本帧的桥接会采样它
        // 咀嚼音效由 GameSession::tickEating 的循环驱动
    }
    void onEatingCancelled() override {}

    // RN-15: the block-preview export's fixed scene constants. The block sits in
    // the middle of an otherwise empty chunk — no ground, so nothing but sky
    // behind it — and the camera's field of view is pinned here rather than taken
    // from the options file, because the options file is a user setting and this
    // is a measurement.
    static constexpr glm::ivec3 kPreviewBlockPosition{8, 64, 8};
    static constexpr float kPreviewFieldOfViewDegrees = 60.0F;
    // Frames to render before capturing. The first frames of a scene upload the
    // section mesh and settle the streaming state; capturing frame one would
    // photograph an empty chunk.
    static constexpr int kPreviewWarmupFrames = 8;

    void initialize() {
        if (glfwInit() != GLFW_TRUE) {
            throw std::runtime_error("GLFW initialization failed");
        }
        glfwInitialized = true;
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        // RN-15d：导出模式的窗口是固定尺寸的隐藏窗口
        // 尺寸必须固定：一张随显示器大小变化的导出图没法和另一台机器上的比对，
        // 而"可比"正是这个工具的全部价值。当前架构下 surface 仍要 GLFW，
        // 所以窗口还是要建，只是不显示、不许改大小。
        const bool exportingPreview = testScene.has_value() && testScene->exportPreview;
        const int windowWidth = exportingPreview ? static_cast<int>(testScene->previewSize)
                                                 : options.windowWidth;
        const int windowHeight = exportingPreview ? static_cast<int>(testScene->previewSize)
                                                  : options.windowHeight;
        glfwWindowHint(GLFW_RESIZABLE, exportingPreview ? GLFW_FALSE : GLFW_TRUE);
        glfwWindowHint(GLFW_VISIBLE, exportingPreview ? GLFW_FALSE : GLFW_TRUE);
        glfwWindowHint(GLFW_MAXIMIZED,
                       !exportingPreview && options.windowMaximized ? GLFW_TRUE : GLFW_FALSE);
        window = glfwCreateWindow(windowWidth, windowHeight,
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
        // 每个窗口事件先问 screenMode() 谁拥有输入，再交给该模式的处理函数
        // 模态优先级只在 input/ScreenMode.hpp 里作为纯函数写一次
        // 它不由各回调各自的标志判断链重新推导
        glfwSetKeyCallback(window, [](GLFWwindow* callbackWindow, int key, int, int action, int) {
            auto* renderer = static_cast<Impl*>(glfwGetWindowUserPointer(callbackWindow));
            switch (renderer->screenMode()) {
            case input::ScreenMode::KeyCapture:
                renderer->handleKeyCaptureKey(key, action);
                return;
            case input::ScreenMode::TextField:
                renderer->handleWorldNameKey(key, action);
                return;
            case input::ScreenMode::Chat:
                renderer->handleChatKey(key, action);
                return;
            case input::ScreenMode::Inventory:
                // I-3: the anvil's rename box takes the keyboard while that
                // screen is open and something is in the left slot. Anything it
                // does not claim (Escape, E) falls through to the screen.
                if (renderer->handleAnvilNameKey(key, action)) {
                    return;
                }
                renderer->handleScreenKey(key, action);
                return;
            case input::ScreenMode::Menu:
            case input::ScreenMode::Play:
                renderer->handleScreenKey(key, action);
                return;
            }
        });
        glfwSetCharCallback(window, [](GLFWwindow* callbackWindow, unsigned int codepoint) {
            auto* renderer = static_cast<Impl*>(glfwGetWindowUserPointer(callbackWindow));
            switch (renderer->screenMode()) {
            case input::ScreenMode::TextField:
                renderer->appendWorldNameCodepoint(codepoint);
                return;
            case input::ScreenMode::Chat:
                renderer->appendChatCodepoint(codepoint);
                return;
            // 其余模式都不接受输入文本
            // 正在捕获按键的那一行等的是**按键**，不是字符
            case input::ScreenMode::Inventory:
                renderer->appendAnvilNameCodepoint(codepoint);
                return;
            // 其余模式都不接受输入文本
            case input::ScreenMode::KeyCapture:
            case input::ScreenMode::Menu:
            case input::ScreenMode::Play:
                return;
            }
        });
        glfwSetScrollCallback(window, [](GLFWwindow* callbackWindow, double, double yOffset) {
            auto* renderer = static_cast<Impl*>(glfwGetWindowUserPointer(callbackWindow));
            if (yOffset == 0.0) {
                return;
            }
            const int direction = yOffset > 0.0 ? -1 : 1;
            const auto mode = renderer->screenMode();
            // 滚轮属于指针输入，菜单页即便某行正在捕获按键、或名称输入框占着键盘
            // 它的滚动列表依然可用，isMenuScreen 涵盖这三种情况
            if (input::isMenuScreen(mode)) {
                renderer->scrollMenuList(direction);
                return;
            }
            switch (mode) {
            case input::ScreenMode::Inventory:
                if (renderer->uiFrameData_.gameMode == gameplay::GameMode::Creative) {
                    renderer->scrollCreative(direction);
                }
                return;
            case input::ScreenMode::Play:
                renderer->scrollHotbar(direction);
                return;
            case input::ScreenMode::Chat:
            default:
                return;
            }
        });
        glfwSetMouseButtonCallback(
            window, [](GLFWwindow* callbackWindow, int button, int action, int modifiers) {
                auto* renderer = static_cast<Impl*>(glfwGetWindowUserPointer(callbackWindow));
                const auto mode = renderer->screenMode();
                if (input::isMenuScreen(mode)) {
                    renderer->handleMenuMouseButton(button, action);
                    return;
                }
                switch (mode) {
                case input::ScreenMode::Inventory:
                    renderer->handleInventoryMouseButton(button, action, modifiers);
                    return;
                case input::ScreenMode::Play:
                    renderer->handlePlayMouseButton(button, action);
                    return;
                // 聊天栏吞掉点击：本 build 渲染的聊天界面没有可点击内容
                case input::ScreenMode::Chat:
                default:
                    return;
                }
            });
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        if (glfwRawMouseMotionSupported() == GLFW_TRUE) {
            glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        }
        glfwSetCursorPosCallback(window, [](GLFWwindow* callbackWindow, double x, double y) {
            auto* renderer = static_cast<Impl*>(glfwGetWindowUserPointer(callbackWindow));
            const auto mode = renderer->screenMode();
            if (input::isMenuScreen(mode)) {
                renderer->dragMenuControl();
                return;
            }
            switch (mode) {
            case input::ScreenMode::Inventory:
                renderer->dragInventory(x, y);
                return;
            case input::ScreenMode::Play:
                renderer->applyLookDelta(x, y);
                return;
            case input::ScreenMode::Chat:
            default:
                return;
            }
        });

        vulkanDevice_.initialize(window);
        // 把句柄复制出来，渲染器里已有的引用照常可用；所有权与销毁责任仍在 vulkanDevice_
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
        loadLanguage();
        textures_.createFontTexture(fontMetrics, textFont, requiredUnicodePages(),
                                    options.forceUnicodeFont);
        // 绑定一次事件宿主，让 tick 循环之外产生的世界编辑也能进入渲染与持久化流水线
        // 这类编辑来自交互路径的变更汇
        // tick() 会重复绑定同一个宿主，无副作用
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
        // MC_REBEDROCK_RAIN_MODE 取 texture 或 async，选择降雨绘制路径
        // MC_REBEDROCK_RAIN_COUNT 覆盖雨滴目标数量
        // 实验性内容子菜单才是权威控制，环境变量只是开发与性能测试时的覆盖手段
        if (const char* modeValue = std::getenv("MC_REBEDROCK_RAIN_MODE")) {
            rainMode_ = std::string_view{modeValue} == "texture" ? RainMode::Texture
                                                                 : RainMode::Async;
        } else {
            rainMode_ = static_cast<RainMode>(std::clamp(options.rainMode, 0, 1));
        }
        if (const char* countValue = std::getenv("MC_REBEDROCK_RAIN_COUNT")) {
            rainCountOverride_ = std::strtoul(countValue, nullptr, 10);
        }
        // MC_REBEDROCK_PARTICLE_LEVEL 取 0 到 3 选择粒子效果等级，覆盖方式与降雨的环境变量相同
        // 菜单选项才是权威来源
        if (const char* levelValue = std::getenv("MC_REBEDROCK_PARTICLE_LEVEL")) {
            options.particleLevel =
                std::clamp(static_cast<int>(std::strtol(levelValue, nullptr, 10)), 0, 3);
        }
        applyParticleLevel();
        // MC_REBEDROCK_RAIN_COLLISION_CACHE=0 可在无界面情况下强制走逐雨滴直接碰撞路径
        // 菜单选项才是权威控制
        rainSystem.setCollisionCache(options.rainCollisionCache);
        if (const char* cacheValue = std::getenv("MC_REBEDROCK_RAIN_COLLISION_CACHE")) {
            rainSystem.setCollisionCache(std::strcmp(cacheValue, "0") != 0);
        }
        // 烟测始终走一遍太阳阴影路径，使预通道与地形采样每次运行都被验证
        if (diag::smokeTestEnabled()) {
            options.sunShadows = true;
        }
        shadowDisabled =
            !options.sunShadows || std::getenv("MC_REBEDROCK_SHADOW_DISABLE") != nullptr;
        if (occlusion_.disabled) {
#if defined(__APPLE__)
            std::cout << "GPU occlusion queries: disabled on macOS (set "
                         "MC_REBEDROCK_FORCE_OCCLUSION=1 for driver diagnostics)\n";
#else
            std::cout << "GPU occlusion queries: disabled\n";
#endif
        }
    }

    // CPU 雨滴目标数
    // texture 模式只留很少的量用于落地水花和音效，particles 与 async 渲染同一份完整数量
    // MC_REBEDROCK_RAIN_COUNT 覆盖以上全部
    [[nodiscard]] std::size_t rainTargetCount() const {
        // 雷暴要把世界浇透：雨量随雷暴强度上浮，最多到普通降雨的两倍
        // async 路径的容量使得多出来的数千雨滴几乎不增加绘制成本
        const float thunderBoost = 1.0F + clientMirror_.world().thunderGradient;
        // 面向玩家的雨量提升跟随粒子效果等级，普通雨与雷雨都取基线的 1.5 倍
        // 中档给 1.5 倍预算，高档翻倍，疯狂三倍，低档减半
        const float rainScale = 1.5F * particleLevelMultiplier(options.particleLevel);
        // ±24 格这样更宽的雨区需要更密的雨量才能整片看起来像下雨
        // 基数因此比 ±16 的旧值提高四分之一，逐模式的取值在 rainBaseCount 里
        // 这里曾套一层 switch 而三个 case 落到同一行，那是基数曾按模式分开留下的化石
        // 它零信息量，却让人以为几条路径在此处有分别
        const std::size_t base =
            rainCountOverride_ > 0U ? rainCountOverride_ : rainBaseCount(rainMode_);
        return static_cast<std::size_t>(static_cast<float>(base) * rainScale * thunderBoost);
    }

    // 一列中最高的完整碰撞面，对应 vanilla 的"阻挡运动"顶部高度
    // 限定在相机附近的 y 窗口内，使降雨搜索和屋顶探测都不必遍历整列
    // 返回雨滴落在该面上的静止位置；窗口内没有碰撞时返回空
    // 扫描自上而下、命中即停，那就是雨滴落地的面
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

    // 降雨音效
    // 下雨期间客户端每一两帧就在雨滴落点处播一小段 weather.rain，风雨声因此环绕相机
    // 当那个落点是相机上方的屋顶时，也就是玩家在遮蔽物下
    // 改用闷响的 weather.rain.above，音量 0.1、音高 0.5，即 vanilla 的"室内"雨声
    // 音量跟随平滑后的降雨强度，毛毛雨很轻，大雨很响
    // 雷暴强度再额外加半档，雷雨天因此比普通雨更猛
    // vanilla 把该片段固定在 0.2，这里的渐变是本项目的调整
    void updateWeatherSound(world::World& world) {
        const float rainGradient = clientMirror_.world().rainGradient;
        if (rainGradient <= 0.0F) {
            return;
        }
        const glm::ivec3 cameraBlock{static_cast<int>(std::floor(camera.position().x)),
                                     static_cast<int>(std::floor(camera.position().y)),
                                     static_cast<int>(std::floor(camera.position().z))};
        // vanilla 在 ±10 半径内随机采样至多十列，并保留最后一个不高于相机 +10 格的面
        // 屋顶遮蔽的情形正好落在这个范围里，因此这里的 y 窗口同样贴着相机取
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
        // vanilla 用"随机数小于计数器"来放行，计数器每次判定后自增
        // 它从零开始必不触发，随后放行概率依次为 1/3 和 2/3，最后每帧都放行
        // 播放一次就把它重置，平均下来一两帧一段
        weatherSoundRng_ = weatherSoundRng_ * 1664525U + 1013904223U;
        if (static_cast<int>(weatherSoundRng_ % 3U) >= weatherSoundCadence_++) {
            return;
        }
        weatherSoundCadence_ = 0;
        // 处于遮蔽下的判据是相机所在列上方有碰撞，且找到的面就是那个屋顶
        // 这说明雨落在头顶，于是在屋顶处播放闷响的室内雨声
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
        // 一次性诊断，证明天气音效循环确实以按强度缩放的音量抵达音频系统
        // 同时显示屋顶规则为这场雨的第一段选中了哪个片段
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
        // vanilla 在半透明地形之后绘制降水，并关闭深度写入
        // 雨列仍会与屋顶和地形做深度测试，但近处的一条半透明雨带不能在它后面所有雨带上打洞
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
        checkVk(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &worldPipelines_.rainSheetPipelineLayout),
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
        pipelineInfo.layout = worldPipelines_.rainSheetPipelineLayout;
        pipelineInfo.renderPass = worldPipelines_.renderPass;
        checkVk(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                          &worldPipelines_.rainSheetPipeline),
                "vkCreateGraphicsPipelines(rain sheet)");
        vkDestroyShaderModule(device, vertexModule, nullptr);
        vkDestroyShaderModule(device, fragmentModule, nullptr);
    }

    void initializeTestScene() {
        if (testScene->occlusionScene) {
            // 受控遮挡场景是一块表面在 y=47 的平整石台
            // section 1 占 y 16-31，被掏成埋在地下的洞穴
            // section 2 占 y 32-47，留一个 2x2 的地表开口
            // 相机位于 y=51 刚好在地表之上，沿 +Z 望向石台，查询结果因此是可预期的：
            //   section 2 是地表，必须保持可见，结果大于 0
            //   section 1 是地下洞穴，必须被剔除，结果为 0
            world::Chunk chunk;
            for (int y = 0; y < 48; ++y) {
                for (int z = 0; z < world::kChunkDepth; ++z) {
                    for (int x = 0; x < world::kChunkWidth; ++x) {
                        chunk.setBlock(x, y, z, world::Block::Stone);
                    }
                }
            }
            // section 1 内部一个 4x4x4 的地下洞穴
            // 洞壁也在同一 section 内，网格因此非空，该 section 仍可被测试
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
                pendingSectionOrder.push(update.position, 0, false);
                latestSectionRevisions.insert_or_assign(update.position, update.revision);
                pendingSectionUpdates.insert_or_assign(update.position, std::move(update));
            }
            loadedCpuChunkCount = 1U;
            // 相机跟随玩家眼点，所以把玩家钉在石台表面（y=47）之上一点，沿 +Z 望向场景
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
        constexpr std::array orientations{
            world::BlockOrientation::North, world::BlockOrientation::East,
            world::BlockOrientation::South, world::BlockOrientation::West,
            world::BlockOrientation::Up,    world::BlockOrientation::Down};
        world::Chunk chunk;
        // RN-15c: the scene spec carries the whole state now, not just the block.
        // `--stage` still spins the six orientations for the callers that have
        // always used it — but only when the spec did not name `facing` itself,
        // because two things driving one property is how a picture ends up not
        // being the state that was asked for.
        world::BlockState blockState = testScene->state;
        if (!testScene->stateSetsFacing) {
            blockState = blockState.with(
                orientations[static_cast<std::size_t>(testScene->stage) % orientations.size()]);
        }
        chunk.setState(kPreviewBlockPosition.x, kPreviewBlockPosition.y, kPreviewBlockPosition.z,
                       blockState);
        interactionWorld.setChunk({0, 0}, chunk);
        clientCache.setChunk({0, 0}, std::move(chunk));
        world::WorldLightEngine lighting;
        const std::array positions{world::ChunkPosition{0, 0}};
        lighting.initializeChunks(interactionWorld, positions);
        lighting.initializeChunks(clientCache, positions);
        world::SectionMeshUpdate update;
        update.position = {0, world::sectionIndexFromWorldY(kPreviewBlockPosition.y), 0};
        update.mesh =
            world::ChunkMesher::buildSection(clientCache, {0, 0}, update.position.sectionY);
        update.revision = 1U;
        pendingSectionOrder.push(update.position, 0, false);
        latestSectionRevisions.insert_or_assign(update.position, update.revision);
        pendingSectionUpdates.insert_or_assign(update.position, std::move(update));
        if (testScene->block == world::Block::Chest) {
            gameSession.createChestBlockEntity(
                {kPreviewBlockPosition.x, kPreviewBlockPosition.y, kPreviewBlockPosition.z});
        }
        loadedCpuChunkCount = 1U;
        worldReady = true;
        paused = true;
        menuSystem.pageStack.reset(ui::PageId::Game);
        // RN-15a: the day time is FIXED, and `--stage` no longer touches it.
        // It used to be `stage * kTicksPerDay / 10`, so changing the viewpoint
        // also changed the lighting and no two stages were comparable — which for
        // a tool whose whole value is comparison is fatal. kNewWorldTick is the
        // same midday the occlusion scene pins itself to.
        gameSession.clocks().setTotalTicks(
            world::ClockId::Overworld,
            static_cast<std::uint64_t>(world::DayNightCycle::kNewWorldTick));
        previewState_ = blockState;
        if (testScene->exportPreview) {
            // The rest of the determinism knobs, set explicitly rather than
            // inherited from whatever the options file happens to hold: an export
            // that depends on the user's video settings cannot be diffed against
            // one taken on another machine, and comparison is the whole point.
            // Only the export sets these — an interactive test scene stays the
            // interactive test scene it has always been.
            gameSession.weatherSystem().setWeather(/*clearTicks=*/1'000'000,
                                                   /*rainTicks=*/0, /*raining=*/false,
                                                   /*thundering=*/false);
            options.viewBobbing = false;
            options.sunShadows = false;
            baseFieldOfViewDegrees = kPreviewFieldOfViewDegrees;
            camera.setFieldOfViewDegrees(kPreviewFieldOfViewDegrees);
            // The one thing that would otherwise still vary frame to frame: the
            // world is static, but the interpolation weight is not, and it feeds
            // the view matrix. Pin it. The export loop never touches it again.
            renderInterpolationAlpha = 0.0F;
        }
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        std::cout << "Test scene: "
                  << world::blockDefinition(testScene->block).identifier.toString() << " stage "
                  << testScene->stage << '\n';
    }

    // RN-15d: the eight-corner export, written as its own loop rather than as a
    // branch inside run().
    //
    // Two reasons, and the first is a rule for this node: no preview branch lands
    // on the render hot path. The second is that it could not work as a branch
    // anyway — run() rewrites the camera from the player snapshot every single
    // frame, so a pose set before the frame would be overwritten during it.
    //
    // Nothing here starts the simulation thread. The scene is one block that does
    // not move, nothing in it ticks, and a thread ticking nothing is one more
    // source of the frame-to-frame variation this whole node exists to remove.
    [[nodiscard]] int runPreviewExport() {
        const auto directory = testScene->previewRoot / previewDirectoryName(*testScene);
        const float aspectRatio =
            swapchainExtent.height == 0U
                ? 1.0F
                : static_cast<float>(swapchainExtent.width) /
                      static_cast<float>(swapchainExtent.height);
        // The cell the block was placed in, in world coordinates. The camera math
        // works in cell-local units and is handed the origin, so the two never
        // disagree about where the block is.
        const glm::vec3 cellOrigin{static_cast<float>(kPreviewBlockPosition.x),
                                   static_cast<float>(kPreviewBlockPosition.y),
                                   static_cast<float>(kPreviewBlockPosition.z)};
        std::size_t failures = 0;
        for (std::size_t index = 0; index < kPreviewCornerCount; ++index) {
            const auto corner = static_cast<PreviewCorner>(index);
            const auto pose = previewCameraPose(previewState_, cellOrigin, corner,
                                                kPreviewFieldOfViewDegrees, aspectRatio);
            camera.setPosition(pose.eye);
            camera.setRotation(pose.yawDegrees, pose.pitchDegrees);
            // The first frames of a scene upload the section mesh and settle the
            // frames-in-flight ring; capturing frame one would photograph an
            // empty chunk. Poll events too, or a compositor waiting on the window
            // can stall the queue.
            std::optional<std::uint32_t> imageIndex;
            for (int warmup = 0; warmup < kPreviewWarmupFrames; ++warmup) {
                glfwPollEvents();
                imageIndex = drawFrame();
            }
            // The readback issues its own one-shot submit and does not
            // synchronise against in-flight work, so the wait belongs here.
            checkVk(vkDeviceWaitIdle(device), "vkDeviceWaitIdle(preview export)");
            const auto file =
                directory / (std::string{kPreviewCornerNames[index]} + ".png");
            if (!imageIndex.has_value()) {
                std::cerr << "Block preview: the swapchain was recreated instead of drawing "
                          << file.string() << "\n";
                ++failures;
                continue;
            }
            // The GUI pass's finalLayout leaves the scene image in
            // TRANSFER_SRC_OPTIMAL — that is where copySceneToSwapchain picks it
            // up, and where this picks it up too.
            if (!writeSceneImagePng(resources_, device, sceneTargets[*imageIndex].image.image,
                                    sceneUnormFormat(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    swapchainExtent.width, swapchainExtent.height, file)) {
                ++failures;
                continue;
            }
            std::cout << "Wrote " << file.string() << "\n";
        }
        // Seven images out of eight, silently, is the worst outcome for something
        // an automation diffs: it looks like a successful run whose baseline just
        // happens to be short. Say which, and exit non-zero.
        if (failures != 0U) {
            std::cerr << "Block preview export: " << failures << " of " << kPreviewCornerCount
                      << " images failed\n";
            return 1;
        }
        std::cout << "Block preview export: " << kPreviewCornerCount << " images in "
                  << directory.string() << "\n";
        return 0;
    }

    int run() {
        if (testScene.has_value() && testScene->exportPreview) {
            return runPreviewExport();
        }
        // 压测模式由 MC_REBEDROCK_STRESS_FRAMES 打开，它覆盖烟测的 704 游戏帧上限
        // 玩家会持续前进，不断搅动区块流送与遮挡查询
        // 长时间运行才会暴露的内存与 GPU 故障因此能在脚本化运行中出现，不必在键盘前守几分钟
        const char* stressFramesValue = std::getenv("MC_REBEDROCK_STRESS_FRAMES");
        stressFrames =
            stressFramesValue != nullptr ? std::strtoull(stressFramesValue, nullptr, 10) : 0U;
        // 比最后一个脚本步骤（706）多留一格，用于返回标题的判定
        const std::size_t smokeFrameLimit = stressFrames > 0U ? stressFrames : 710U;
        // MC_REBEDROCK_SMOKE_TEST 驱动一次脚本化会话
        // 依次走菜单、开世界、创造界面、聊天命令、进食、天气，最后返回标题
        // 它是测试脚手架而非玩法，因此做成由主循环推进的脚本，而不是拼进循环体的帧号判断链
        // 调度器在 render/SmokeScript.hpp，步骤在 render/vulkan/SmokeScriptSteps.hpp
        // 正常运行根本不构造脚本
        std::optional<SmokeScript> smokeScript;
        if (diag::smokeTestEnabled()) {
            smokeScript.emplace();
            installSmokeScript(*this, *smokeScript, stressFrames,
                               static_cast<std::uint64_t>(smokeFrameLimit));
        }
        // 模拟在这里离开渲染线程
        // 烟测与压测同样要走这条路径
        // MC_REBEDROCK_SYNC_TICK 保留了确定性的单线程回退，供排查故障时使用
        startSimulationThread();
        std::size_t renderedFrames = 0;
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
            // 保持一段帧时间的短期指数平均，并据此调整流送上传预算
            // GPU 被一批稠密 section 拖住时减少每帧上传量让负载回落
            // 帧时间恢复后再加速把区域填满
            // StreamingBudget.hpp 里的迟滞避免预算在单一阈值附近来回振荡
            smoothedFrameSeconds_ = smoothedFrameSeconds_ * 0.7F + deltaSeconds * 0.3F;
            streamingUploadBudget_ = mc::render::streamingUploadBudgetForFrameMs(
                smoothedFrameSeconds_ * 1000.0F, streamingUploadBudget_);
            uiTimeSeconds += static_cast<double>(deltaSeconds);
            // HUD 叠加层走渲染时钟，它属于客户端表现，绝不用世界 tick
            // 吐司的滑入与过期、字幕的淡出因此都按真实时间进行，与模拟速率无关
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
                // 输入准备写的是暂存的玩家输入，由 GameSession 自己的输入互斥量保护
                // 读的是已发布的快照与渲染器本地状态，两者都不需要世界锁
                {
                    const auto inputStart = std::chrono::steady_clock::now();
                    processInput();
                    if (diag::traceEnabled()) {
                        diag::frameTrace().inputMs += diag::msSince(inputStart);
                    }
                }
                // 在一帧最开头排空通道
                // 快照帧刷新客户端镜像，下面所有读取因此看到本帧的玩家与世界
                // 事件帧把该 tick 的副作用作用到作为宿主的本渲染器上
                // 副作用含对客户端缓存的世界编辑、音效、粒子、容器与进食反应
                // 每帧都排空，通道才不会积压
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
                // 动画器的步态输入取自逐 tick 玩家快照，而不是实时玩法对象
                // 快照是原子发布的，这次拷贝无需加锁
                const auto playerSnap = clientMirror_.player();
                // `speed` 是累积的步态相位，因此永远不会回零；真正的位移量是缓动后的步幅
                playerWalking = playerSnap.stride > 0.002F ||
                                playerSnap.previousStride > 0.002F;
            }
            playerModelAnimator.update(deltaSeconds, playerWalking);
            // 头先动、身体跟：头在限度内自由转动，到达限度才拖着身体转
            // 行走时身体缓慢转向视线方向，使移动保持面朝前方——与 vanilla 一致
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
            // 身体跟随头部，带死区和硬钳位
            // 头部相对偏航在 ±50° 的死区内自由转动
            // 超出后身体以约每 tick 30% 的速度转向视线，越过 50° 边界再额外加每 tick 20%
            // 头部相对角硬钳在 ±75°，头永远不会拧过头
            // dt*20 把逐 tick 速率换算到当前帧
            constexpr float kHeadYawDeadZone = 0.8727F;  // 50 deg
            constexpr float kHeadYawClamp = 1.3090F;     // 75 deg hard clamp
            const float tickAlpha = std::min(1.0F, deltaSeconds * 20.0F);
            float lagDiff = wrapAngle(lookYaw - worldBodyYaw);
            const float absLag = std::fabs(lagDiff);
            if (absLag > kHeadYawDeadZone || playerWalking) {
                // 基础每 tick 跟随 30%，越过死区后再加 20%
                const float followRate = 0.30F + (absLag > kHeadYawDeadZone ? 0.20F : 0.0F);
                worldBodyYaw += lagDiff * followRate * tickAlpha;
                lagDiff = wrapAngle(lookYaw - worldBodyYaw);
            }
            // 硬钳位：头部相对偏航不得超过 75°
            if (lagDiff > kHeadYawClamp) {
                worldBodyYaw = lookYaw - kHeadYawClamp;
            } else if (lagDiff < -kHeadYawClamp) {
                worldBodyYaw = lookYaw + kHeadYawClamp;
            }
            constexpr float kMaxHeadYaw = kHeadYawClamp;
            const float headRelative = wrapAngle(lookYaw - worldBodyYaw);
            worldPlayerAnimator.setCursorLook(headRelative / kMaxHeadYaw, -lookDir.y);
            // 第三人称下世界中的玩家与背包预览跑**同一套** PlayerModelAnimator 控制器栈
            // 输入是快照携带的权威行走动画状态，加上潜行标志和渲染时间
            // 状态含步态幅度 walkAmount 与相位 walkPosition
            // walkAmount 饱和到 1.0，停下时衰减到 0
            // 因此静止不摆臂、停下时四肢回到静息
            // body_look_amount 保持 0：世界中的身体偏航施加在模型根节点上，不在剪辑里
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
                // 物品/方块的手臂姿态会抬起一只手，但此时还没有绘制手持物层，所以普通持物保持静息
                // 正在使用则照常表现
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
                // 太阳不再跟着真实帧走，主世界时钟在固定的模拟 tick 内推进
                // 它在那里受 doDaylightCycle 规则门控，这里只剩下帧局部的动画时钟
                renderTimeSeconds += static_cast<double>(deltaSeconds);
                // 手持物姿态取自原子发布的逐 tick 玩家快照
                // 插值用**本帧**的 tick 小数，绝不用上一帧的系数
                // 挥动重新开始（序号变化）时提取器直接跳变，手臂因此不会从顶点倒放一遍
                {
                    // 先拷一份自洽的快照，之后完全从这份拷贝渲染
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
                // 一个读区间覆盖本帧各种特效对世界的全部采样
                // 粒子、雨滴碰撞和天气氛围音都会向世界发射线
                {
                    // 这里读的都是渲染侧自有的客户端缓存和原子发布的世界快照，无需加锁
                    hud_.updateVignetteDarkness(deltaSeconds);
                    const auto particleSimStart = std::chrono::steady_clock::now();
                    // 先派发环境 tick 再推进粒子：这一帧新生成的粒子当帧就参与模拟，
                    // 与服务端 ParticleEvent 灌进来的那批同一待遇
                    blockAnimateTicker.update(deltaSeconds, camera.position(), clientCache,
                                              particleSystem);
                    particleSystem.update(deltaSeconds, clientCache);
                    if (diag::traceEnabled()) {
                        diag::frameTrace().particleSimMs += diag::msSince(particleSimStart);
                        diag::frameTrace().particleCount =
                            static_cast<std::uint32_t>(particleSystem.particles().size());
                    }
                // CPU 雨滴跟随平滑后的天气强度，在所有模式下都负责落地水花与音效
                // particles 与 async 直接渲染这批雨滴；texture 模式则另行绘制 vanilla 的逐列降水
                const float thunderGradient = clientMirror_.world().thunderGradient;
                // 风向保持 10 到 20 秒，再用两三秒转到新方向
                // 这是偶尔的缓慢改向，而不是持续旋转让整片雨域一直打转、定不下一个斜度
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
                const auto rainSimStart = std::chrono::steady_clock::now();
                rainSystem.update(deltaSeconds, camera.position(),
                                  clientMirror_.world().rainGradient, rainTargetCount(),
                                  clientCache, wind);
                if (rainMode_ == RainMode::Texture) {
                    rainSystem.emitTextureImpacts(deltaSeconds, camera.position(),
                                                  clientMirror_.world().rainGradient,
                                                  clientCache);
                }
                if (diag::traceEnabled()) {
                    diag::frameTrace().rainSimMs += diag::msSince(rainSimStart);
                    diag::frameTrace().rainDropCount =
                        static_cast<std::uint32_t>(rainSystem.drops().size());
                    diag::frameTrace().rainLookups =
                        static_cast<std::uint32_t>(rainSystem.lastUpdateLookups());
                }
                for (const auto& splash : rainSystem.splashes()) {
                    if (splash.sampledImpact) {
                        particleSystem.spawnRainImpact(splash.position, splash.onWater);
                    } else {
                        particleSystem.spawnRainSplash(splash.position, splash.direction);
                    }
                }
                // 同一处也驱动雨**声**，在雨滴落点播 weather.rain
                // 玩家在屋顶下时改用闷响版本，音量统一按平滑后的降雨强度缩放
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
                // 一次性诊断，报出列表面缓存把碰撞降到了每帧多少次世界查询
                // 旧的直接路径是每滴每帧一次
                // 等雨量填满且缓存预热完成后才采样，预热后的一帧世界查询次数远少于雨滴数
                // 这个数字因此是稳态值，不是最初几帧的探测预热
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
                // processInput() 已经把本帧的移动经通道送出
                // 服务端在 tick 读取之前把它暂存好，因此不再需要单独的提交发布步骤
                if (!simulationDriver.threaded()) {
                    // 同步回退（MC_REBEDROCK_SYNC_TICK=1）
                    // 保留它，因为这是把线程问题与已知正确行为做二分对照的唯一手段
                    static_cast<void>(
                        simulationDriver.advance(deltaSeconds, [this] { runtime.tick(); }));
                }
            } else if (!simulationDriver.threaded()) {
                simulationDriver.reset();
            }
            // 玩家提交的聊天命令是在 tick 内由运行时派发器执行的；结果落地后追加进历史
            if (const auto chatResult = runtime.takeChatResult(); chatResult.has_value()) {
                chatHistory.push(chatResult->message, chatResult->success, uiTimeSeconds);
            }
            // 插值系数取自已发布快照自带的时间戳，而不是 SimulationDriver 另行计时的累加器
            // 它因此与本帧读到的端点同步，不会在 tick 边界上比端点超前一拍
            // 正是那个相位竞争让移动中的掉落物和挥动的手抖动
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
                // HUD 快照取自逐 tick 玩家快照（在模拟的写锁下发布），不取实时玩法状态
                uiFrameData_.health = playerSnap.health;
                uiFrameData_.foodLevel = playerSnap.foodLevel;
                uiFrameData_.airTicks = playerSnap.airTicks;
                uiFrameData_.ticksSinceDamage = playerSnap.ticksSinceDamage;
                uiFrameData_.experienceLevel = playerSnap.experienceLevel;
                uiFrameData_.experienceProgress = playerSnap.experienceProgress;
                uiFrameData_.gameMode = playerSnap.gameMode;
                uiFrameData_.eating = playerSnap.eating;
                uiFrameData_.selectedStack = playerSnap.heldStack;
                uiFrameData_.selectedHotbarSlot = playerSnap.selectedHotbarSlot;
                const auto worldSnap = clientMirror_.world();
                uiFrameData_.containerScreen = worldSnap.openContainerScreen;
                uiFrameData_.activeChest = worldSnap.openChest;
                // I-3 / AnvilScreen#slotChanged: whenever the left slot changes,
                // the box is reset to whatever that item is currently called —
                // its custom name if it has one, otherwise empty. Without this
                // the box would keep the previous item's text and silently
                // rename the new one.
                if (!(worldSnap.anvilLeft == previousAnvilLeft_)) {
                    previousAnvilLeft_ = worldSnap.anvilLeft;
                    // `getHoverName()`: the CUSTOM name if it has one, otherwise
                    // the item's ordinary translated name. Seeding with only the
                    // custom name left the box blank for an unnamed item, which
                    // is not what vanilla shows.
                    hud_.anvilName() = ui::textFieldWithValue(
                        hud_.itemHoverName(worldSnap.anvilLeft), ui::kAnvilNameFieldRules,
                        anvilNameMetrics());
                }
                playerEyeHeight =
                    playerSnap.sneaking ? gameplay::PlayerController::kSneakingEyeHeight
                                        : gameplay::PlayerController::kEyeHeight;
                fovMultiplier = playerSnap.previousFieldOfViewMultiplier +
                                (playerSnap.fieldOfViewMultiplier -
                                 playerSnap.previousFieldOfViewMultiplier) *
                                    physicsAlpha;
            }
            camera.setPosition(renderedFeetPosition + glm::vec3{0.0F, playerEyeHeight, 0.0F});
            // 遮挡测试场景把相机钉在石台表面 y=47 之上俯视
            // 地表与地下洞穴两个 section 因此都落在视锥内
            if (testScene.has_value() && testScene->occlusionScene) {
                camera.setPosition({8.0F, 60.0F, -8.0F});
            }
            // 压测模式每帧转动相机，随着 section 进出视锥不断搅动遮挡查询
            if (stressFrames > 0U) {
                // 偏航旋转加缓慢俯视，让视野像飞行中四处张望的玩家那样扫向地面
                // 同时沿向外扩张的螺旋移动，使流送窗口全程都在加载新区块
                camera.rotate(2.0F, -0.05F);
                const std::size_t stressClock =
                    std::getenv("MC_REBEDROCK_LOAD_SAVE") != nullptr || !smokeScript.has_value()
                        ? renderedFrames
                        : static_cast<std::size_t>(smokeScript->gameplayFrame());
                const float flightAngle = static_cast<float>(stressClock) * 0.06F;
                const float radius = 40.0F + static_cast<float>(stressClock) * 0.4F;
                const glm::vec3 stressPos{
                    std::cos(flightAngle) * radius,
                    120.0F + std::sin(static_cast<float>(stressClock) * 0.012F) * 80.0F,
                    std::sin(flightAngle) * radius,
                };
                camera.setPosition(stressPos);
                // 让玩家沿螺旋飞行，区块流送因此像真实游玩那样跟随移动
                // 存档以创造模式载入，不会摔伤
                const auto stressWrite = worldLock.write();
                gameSession.teleportPlayer(
                    gameplay::kPrimaryPlayerId,
                    stressPos - glm::vec3{0.0F, snapshotEyeHeight(), 0.0F});
            }
            // 视场角 = 基础 FOV 乘以玩家的移动系数，并像眼点一样在物理 tick 之间插值
            // 疾跑放大到 1.15 倍，创造飞行 1.1 倍，两者都在几个 tick 内缓入
            camera.setFieldOfViewDegrees(baseFieldOfViewDegrees * fovMultiplier);
            audioSystem.updateListener(camera.position(), camera.direction(), {0.0F, 1.0F, 0.0F});
            audioSystem.update();
            driveAmbientMusic(deltaSeconds);
            if (worldSessionActive)
                world_.processChunkStreaming();
            {
                // 权威交互跑在模拟 tick 内（那里持有世界写区间）
                // 本帧只做瞄准目标的射线检测，供输入处理封装成命令，另加独立的 Q 丢弃
                // 射线测的是渲染侧自有的客户端缓存，无需加锁
                updateInteractionTarget();
            }
            {
                const auto dropWrite = worldLock.write();
                world_.updateItemDrop();
            }
            // 对客户端缓存的世界编辑、音效、粒子、容器与进食反应都是模拟的副作用
            // 它们由上面帧开头那次通道排空施加，来源是服务端逐 tick 事件的解码结果
            {
                const auto drawStart = std::chrono::steady_clock::now();
                static_cast<void>(drawFrame());
                if (diag::traceEnabled()) {
                    diag::frameTrace().drawFrameMs += diag::msSince(drawStart);
                }
            }
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
                              << " uploadMs=" << t.uploadMs
                              << " recordMs=" << t.recordMs
                              << " drawFrameMs=" << t.drawFrameMs
                              << " inputMs=" << t.inputMs
                              << " acquireMs=" << t.acquireMs
                              << " presentMs=" << t.presentMs
                              << " occReadMs=" << t.occlusionReadbackMs
                              << " uniformMs=" << t.uniformMs
                              << " imageWaitMs=" << t.imageWaitMs
                              << " particleSimMs=" << t.particleSimMs
                              << " rainSimMs=" << t.rainSimMs
                              << " particleLightMs=" << t.particleLightMs
                              << " particles=" << t.particleCount
                              << " drops=" << t.rainDropCount
                              << " rainLookups=" << t.rainLookups
                              << " visible=" << t.visibleSections
                              << " unloaded=" << t.unloadedChunks
                              << " saveChunkCalls=" << t.saveChunkCalls
                              << " batches=" << t.queueBatchCount
                              << " editScan=" << t.editScan
                              << " center=(" << t.newCenterX << ',' << t.newCenterZ << ')'
                              << " centerChanged=" << (t.centerChanged ? 1 : 0)
                              << '\n';
                }
            }
            // 三份世界常驻内存的周期性报告
            // 烟测只加载出生点区域，数字偏小
            // 因此这里在真实游玩与压测中每约 2 秒采样一次三份区块副本
            // 默认关闭
            // 服务端世界在短读锁下读取，因为模拟线程会改它
            // 客户端缓存归渲染侧所有，工作线程的世界是原子采样
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
                        // total 是三个逻辑视图之和，共享的区块在每个持有者处各计一次
                        // uniqueTotal 是独占部分
                        // 两者之差即写时复制共享的物理副本，也就是合并三份世界后可以回收的量
                        std::cout << "[memory] server=" << serverBytes << "(u" << serverUnique
                                  << ") client=" << clientBytes << "(u" << clientUnique
                                  << ") worker=" << workerBytes << "(u" << workerUnique
                                  << ") total=" << total << " unique=" << uniqueTotal << " ("
                                  << (total / (1024U * 1024U)) << "MB/" << (uniqueTotal / (1024U * 1024U))
                                  << "MB)\n";
                        // GPU 侧归属用 VMA 总分配量对比几个大户
                        // 大户指世界网格顶点与索引、暂存和纹理
                        // 离屏、阴影、uniform、粒子、雨归入 other
                        // cpuMeshPool 是 CPU 侧 RenderMeshData 的复用池
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
                        // 把 other 拆开：MSAA 的深度/颜色瞬态目标是主要嫌疑
                        // 同时检测它们是否真的落进了 lazily-allocated 也就是 memoryless 内存
                        // VMA 的 allocationBytes 两种情况都按逻辑大小计
                        // 只有这样才能确认那项优化真正生效
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
            // 遮挡测试场景多渲染几帧，等两帧的查询延迟走完，然后退出并导出诊断数据
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
                // macOS 上 sleep_until 会因调度器的定时精度晚醒 1 到 2ms
                // 每帧都多出一个唤醒延迟，仅此一项就会把 120fps 的上限压到约 100fps
                // 因此先睡到接近目标，最后两毫秒忙等，让节奏落在目标值上而不是操作系统的唤醒粒度上
                const auto spinStart = deadline - std::chrono::milliseconds(2);
                if (std::chrono::steady_clock::now() < spinStart) {
                    std::this_thread::sleep_until(spinStart);
                }
                while (std::chrono::steady_clock::now() < deadline) {
                    // 尾段忙等，换取精确的节奏
                }
            }
            // 复现用钩子 MC_REBEDROCK_LOAD_SAVE 跳过菜单直接载入第一个真实存档
            // 随后由压测相机带着飞
            if (std::getenv("MC_REBEDROCK_LOAD_SAVE") != nullptr && !loadSaveStarted) {
                loadSaveStarted = true;
                const auto summaries = saveRepository.list();
                if (summaries.empty()) {
                    throw std::runtime_error("MC_REBEDROCK_LOAD_SAVE: no saves found");
                }
                startWorld(saveRepository.load(summaries.front().identifier));
            }
            if (smokeScript.has_value()) {
                smokeScript->advance(renderedFrames, worldReady);
            }
            // LOAD_SAVE 运行在渲染满 stressFrames 帧后结束
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
        return 0;
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

    // 用滚轮滚动按键设置列表，钳制到最后一页可见位置（与世界列表、语言列表同一约定）
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
            saveRepository.rename(menuSystem.editWorldIdentifier, menuSystem.editWorldName.value);
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
        // 同时退出确认页和编辑页，回到列表
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
        // 模拟侧已经写过 interactionWorld；把这次编辑镜像进客户端缓存，渲染网格本帧就能反映出来
        static_cast<void>(
            clientCache.setState(x, y, z, world::BlockState{block, resolvedOrientation, fluidLevel}));
    }

    void submitWorldStateEdit(int x, int y, int z, world::BlockState state) override {
        // 用 setState 而不是 setBlock
        // 点燃状态正是"方块加流体加朝向"这个松散三元组装不下的东西
        // 用后者会让点着的熔炉以未点亮的样子送到渲染流送侧
        // 存档记录的编辑同样带完整状态——它一旦在这里被拆成三元组，点燃状态就会在写盘途中悄悄丢失
        rememberWorldEdit({x, y, z, state});
        chunkStreamer.setState(x, y, z, state);
        static_cast<void>(clientCache.setState(x, y, z, state));
    }

    // 直接在渲染线程上用已更新的世界重建被玩法编辑影响到的 section
    // 放置或破坏的方块因此当帧可见，不必等后台工作线程往返一趟
    // 权威重建仍由工作线程完成
    // 它严格更高的修订号会经 queueStreamBatch 里常规的修订号守卫替换掉这层临时预览
    //
    // 这与区块流送侧应用方块编辑的做法一致
    // 工作线程的编辑批次只带网格不带光照，否则世界里存的光照会冻结在最后一次生成的快照上
    // 在这里跑同样的增量光照传播，预览的光照才正确，火把会立刻发光而不是先黑一下
    // 连续编辑之间也因此保持自洽
    // 这里用的是有界的增量广度优先传播，不是先前会卡住渲染线程的整块传播
    void previewBlockEdit(int worldX, int y, int worldZ) override {
        // 渲染侧的光照维护在客户端缓存上，编辑预览因此对着真正要画的那份数据
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

        // 几何与顶点 AO 会越过 section 边界采样一个体素
        // 被编辑的体素因此最多能弄脏它 26 个邻居所在的 section
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
        // 光照变化能远超被编辑的体素（火把可扩散 14 格）
        // 凡是光照真的变了的 section 都重新网格化，空的跳过——里面没有顶点需要重新照亮
        for (const auto position : interactionLightEngine.takeDirtySections()) {
            const world::Chunk* chunk = clientCache.chunk({position.chunkX, position.chunkZ});
            if (chunk != nullptr && !chunk->section(position.sectionY).empty()) {
                mark({position.chunkX, position.sectionY, position.chunkZ});
            }
        }

        if (sections.empty())
            return;
        // 轻量的采样**视图**，对上面刚传播进客户端缓存的光照做 O(1) 读取
        // 逐区块的那个构造函数会用两趟广度优先重新传播约 48x384x48 的区域
        // 那种做法绝不能在每次编辑时跑
        const world::ChunkLightSampler lighting{clientCache};
        for (const auto position : sections) {
            world_.remeshSectionImmediate(position, lighting);
        }
    }

    void clearRenderedWorld() {
        simulationActive.store(false, std::memory_order_release);
        checkVk(vkDeviceWaitIdle(device), "vkDeviceWaitIdle(world reset)");
        // 设备已空闲，池里所有缓冲都能安全放回空闲表，包括仍在延迟归还槽里的
        // 下一个世界因此复用同一批缓冲，不必重新分配
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
        world_.onWorldReset();
        pendingSectionOrder.clear();
        pendingSectionUpdates.clear();
        latestSectionRevisions.clear();

        // 把烘焙画质重新锚定到已保存的选项，新世界按存下来的画质开始网格化
        // 选 Off 时仍按 Standard 烘焙，反正着色器会忽略平滑光照通道
        qualityRemeshPending.clear();
        currentMeshQuality = options.smoothLightingQuality != world::SmoothLightingQuality::Off
                                 ? options.smoothLightingQuality
                                 : world::SmoothLightingQuality::Standard;
        targetMeshQuality = currentMeshQuality;
        chunkStreamer.setSmoothLightingQuality(currentMeshQuality);
        interactionWorld = {};
        clientCache = {};
        // 丢弃镜像，免得下一个世界在首个 tick 重新发布之前，短暂显示上一个世界的玩家/世界状态
        clientMirror_.clear();
        gameSession.resetWorldState();
        particleSystem = {};
        blockAnimateTicker.reset();
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
        // 新世界从空聊天开始：上一场的命令与结果不能漏进下一张地图的左下角
        chatHistory.clear();
        lastSessionPeakPendingSectionCount = 0U;
        // 会话状态、区块流送器和各类种子这些权威恢复都在运行时侧
        // headless 服务器因此加载的是同一个世界
        // 渲染器只负责随后的表现层（相机、纹理、菜单）
        runtime.loadWorld(std::move(save), viewDistanceChunks);
        savedEditIndices.reserve(currentSave->edits.size());
        for (std::size_t index = 0; index < currentSave->edits.size(); ++index) {
            const auto& edit = currentSave->edits[index];
            savedEditIndices.insert_or_assign(PersistentEditPosition{edit.x, edit.y, edit.z},
                                              index);
        }
        // 游戏规则也随世界一起走；GameRuntime 加载存档规则时会重新挂上会话自己的变更处理器
        camera.setPosition(snapshotCameraEye());
        spawnPositionInitialized = currentSave->hasPlayerPosition;
        // Warm the stream buffer pools before the first chunk batch arrives, so the
        // load burst pops pooled buffers instead of allocating on the render thread
        // per uploaded section (idempotent — a no-op once the pools are warm).
        world_.prewarmStreamBufferPools();
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
            auto save = runtime.createWorld(menuSystem.createWorldName.value, seed,
                                            menuSystem.createWorldGameMode,
                                            menuSystem.createWorldAllowCommands);
            refreshSaveList();
            startWorld(std::move(save));
        } catch (const std::exception& exception) {
            menuSystem.saveStatus = "Create failed: " + std::string{exception.what()};
        }
    }

    // 保存要读世界和实时实体列表，因此需要一个临界区
    // 拆成两个版本，是因为 /setworldspawn 在命令的写区间**内部**触发保存，而互斥量不可重入
    // 已经持有临界区的调用方用 Locked 版本，其余用这个
    // 存档的构建与落盘由运行时完成，这层包装只补上表现层
    void saveCurrentWorld() {
        const auto saveRead = worldLock.read();
        saveCurrentWorldLocked();
    }

    void saveCurrentWorldLocked() {
        try {
            // 存档由运行时构建并落盘；返回 false 表示当前没有打开的存档
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

    // ---- 输入归属 ----------------------------------------------------------
    //
    // 当前由谁拥有输入
    // 每个窗口回调都先问它，再把事件交给对应模式的处理函数
    // 优先级本身是 input/ScreenMode.hpp 里的纯函数，只写一次，不由各回调各自（略有出入地）重新推导
    [[nodiscard]] input::ScreenMode screenMode() const {
        const auto page = menuSystem.pageStack.current();
        return input::screenModeOf(input::ScreenState{
            /*keyCapturing=*/keyBindScreen_.capturing(),
            /*textFieldOpen=*/page == ui::PageId::CreateWorld || page == ui::PageId::EditWorld,
            /*chatOpen=*/chatOpen,
            /*inventoryOpen=*/inventoryOpen,
            /*paused=*/paused,
        });
    }

    // 按键设置的某一行正在捕获时，下一次按键**就是**这次重绑
    // 它在这里被消费并写进 InputSystem 这一唯一来源，不再充当菜单键或游戏键
    // Escape 表示取消捕获，而不是把 Escape 绑上去
    void handleKeyCaptureKey(int key, int action) {
        if (action != GLFW_PRESS) {
            return;
        }
        if (key == GLFW_KEY_ESCAPE) {
            keyBindScreen_.cancelCapture();
            return;
        }
        const input::Key captured = input::keyFromGlfw(key);
        if (captured != input::Key::Unknown) {
            keyBindScreen_.applyKey(captured);
            playUiClick();
        }
    }

    // UI-1: the two hand-rolled input handlers that used to live here — one per
    // field, each with its own backspace, its own append and its own
    // `codepoint >= 32 && <= 126` filter — are gone. What is left is a driver:
    // it translates GLFW into ui::TextField's vocabulary, reaches the clipboard
    // (the one thing the pure layer cannot), and owns the per-screen keys
    // (Enter, Escape, Tab) that are the SCREEN's business, not the widget's.
    //
    // Editing itself happens in src/ui/TextField.cpp, where it is asserted.

    // The measure/width pair for a field, built once per event so the editing
    // side scrolls the window exactly where the painter will draw it.
    [[nodiscard]] ui::TextFieldMetrics textFieldMetricsFor(const ui::UiRect& field, float scale,
                                                           bool bordered) const {
        return ui::TextFieldMetrics{
            [this, scale](std::string_view piece) { return textFont.textWidth(piece, scale); },
            ui::textFieldInnerWidth(field.width, scale, bordered)};
    }

    [[nodiscard]] ui::TextFieldMetrics worldNameMetrics() const {
        const auto layout = currentHudLayout();
        return textFieldMetricsFor(layout.worldNameField(), layout.scale(), true);
    }

    [[nodiscard]] ui::TextFieldMetrics chatMetrics() const {
        const auto layout = currentHudLayout();
        return textFieldMetricsFor(layout.chatInput(), layout.scale(), false);
    }

    [[nodiscard]] ui::TextFieldState& focusedWorldName() {
        return menuSystem.pageStack.current() == ui::PageId::CreateWorld
                   ? menuSystem.createWorldName
                   : menuSystem.editWorldName;
    }

    // Clipboard round-trip. GLFW owns the system clipboard, so it stops here.
    void copyToClipboard(const std::string& text) const {
        if (!text.empty()) {
            glfwSetClipboardString(window, text.c_str());
        }
    }

    [[nodiscard]] std::string clipboardText() const {
        const char* text = glfwGetClipboardString(window);
        return text != nullptr ? std::string{text} : std::string{};
    }

    // The editing half of a key press, shared by every field. Returns true when
    // the key belonged to the widget, so the caller only has to handle the keys
    // that are its screen's.
    bool editTextField(ui::TextFieldState& state, const ui::TextFieldRules& rules,
                       const ui::TextFieldMetrics& metrics, int key, int action) {
        if (action != GLFW_PRESS && action != GLFW_REPEAT) {
            return false;
        }
        const ui::TextFieldModifiers modifiers{
            (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
             glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS),
            (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
             glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS)};

        // Ctrl+A/C/X/V first: they are chords on letter keys, not editing keys.
        if (modifiers.control) {
            switch (key) {
            case GLFW_KEY_A:
                state = ui::textFieldApplyKey(std::move(state), rules, metrics,
                                              ui::TextFieldKey::SelectAll, modifiers);
                return true;
            case GLFW_KEY_C:
                copyToClipboard(ui::textFieldSelectedText(state));
                return true;
            case GLFW_KEY_X:
                copyToClipboard(ui::textFieldSelectedText(state));
                state = ui::textFieldApplyText(std::move(state), rules, metrics, "");
                return true;
            case GLFW_KEY_V:
                state = ui::textFieldApplyText(std::move(state), rules, metrics, clipboardText());
                return true;
            default:
                break;
            }
        }

        ui::TextFieldKey editKey{};
        switch (key) {
        case GLFW_KEY_BACKSPACE: editKey = ui::TextFieldKey::Backspace; break;
        case GLFW_KEY_DELETE:    editKey = ui::TextFieldKey::Delete;    break;
        case GLFW_KEY_LEFT:      editKey = ui::TextFieldKey::Left;      break;
        case GLFW_KEY_RIGHT:     editKey = ui::TextFieldKey::Right;     break;
        case GLFW_KEY_HOME:      editKey = ui::TextFieldKey::Home;      break;
        case GLFW_KEY_END:       editKey = ui::TextFieldKey::End;       break;
        default:                 return false;
        }
        state = ui::textFieldApplyKey(std::move(state), rules, metrics, editKey, modifiers);
        return true;
    }

    // 创建/编辑世界的名称输入框：编辑键与 vanilla 的文本框一致，回车提交该页，Escape 退出
    void handleWorldNameKey(int key, int action) {
        if (editTextField(focusedWorldName(), ui::kWorldNameFieldRules, worldNameMetrics(), key,
                          action)) {
            return;
        }
        if (action != GLFW_PRESS) {
            return;
        }
        const auto page = menuSystem.pageStack.current();
        if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) {
            playUiClick();
            if (page == ui::PageId::CreateWorld) {
                startNewWorld();
            } else {
                applyRename();
            }
        } else if (key == GLFW_KEY_ESCAPE) {
            menuSystem.pageStack.pop();
        }
    }

    // I-3: the anvil rename box. Live only while the anvil screen is open with
    // something in the left slot — vanilla's own `setEditable(!empty)` rule.
    [[nodiscard]] bool anvilRenameActive() const {
        const auto snapshot = clientMirror_.world();
        return snapshot.openContainerScreen == ContainerScreen::Anvil &&
               !snapshot.anvilLeft.empty();
    }

    [[nodiscard]] ui::TextFieldMetrics anvilNameMetrics() const {
        const auto layout = currentHudLayout();
        const auto panel = layout.inventoryPanel();
        return textFieldMetricsFor({panel.x + 62.0F * layout.scale(),
                                    panel.y + 24.0F * layout.scale(),
                                    103.0F * layout.scale(), 12.0F * layout.scale()},
                                   layout.scale(), false);
    }

    // The server owns what a rename costs and whether it applies, so every edit
    // is shipped — the price has to move as you type, exactly as vanilla's
    // screen does.
    // The server decides what a rename costs, but it cannot decide whether the
    // box still holds the item's DEFAULT name: that name is translated, and
    // translation is client-side in this build (a dedicated server ships no
    // assets). So the client blanks the text in exactly the case where the
    // server could not tell — an unnamed item whose box still reads as its
    // ordinary name — and sends it verbatim otherwise. The four cases then all
    // land on vanilla's own server-side rule, which compares against the
    // custom name:
    //
    //   unnamed + untouched box  -> "" -> no rename, no cost
    //   unnamed + edited box     -> text -> rename, one level
    //   named   + untouched box  -> the custom name -> equal, no cost
    //   named   + cleared box    -> "" -> strip the name, one level
    void publishAnvilName() {
        const auto snapshot = clientMirror_.world();
        const std::string& typed = hud_.anvilName().value;
        const bool unnamed =
            gameplay::customNameOf(snapshot.anvilLeft.customNameId).empty();
        gameplay::SetAnvilName rename;
        rename.name = (unnamed && typed == hud_.itemHoverName(snapshot.anvilLeft))
                          ? std::string{}
                          : typed;
        runtime.enqueueClientCommand(std::move(rename));
    }

    [[nodiscard]] bool handleAnvilNameKey(int key, int action) {
        if (!anvilRenameActive()) {
            return false;
        }
        const std::string before = hud_.anvilName().value;
        if (!editTextField(hud_.anvilName(), ui::kAnvilNameFieldRules, anvilNameMetrics(), key,
                           action)) {
            return false;
        }
        if (hud_.anvilName().value != before) {
            publishAnvilName();
        }
        return true;
    }

    void appendAnvilNameCodepoint(unsigned int codepoint) {
        if (!anvilRenameActive()) {
            return;
        }
        const std::string before = hud_.anvilName().value;
        hud_.anvilName() = ui::textFieldApplyChar(std::move(hud_.anvilName()),
                                                 ui::kAnvilNameFieldRules, anvilNameMetrics(),
                                                 static_cast<char32_t>(codepoint));
        if (hud_.anvilName().value != before) {
            publishAnvilName();
        }
    }

    void appendWorldNameCodepoint(unsigned int codepoint) {
        auto& name = focusedWorldName();
        name = ui::textFieldApplyChar(std::move(name), ui::kWorldNameFieldRules, worldNameMetrics(),
                                      static_cast<char32_t>(codepoint));
    }

    void handleChatKey(int key, int action) {
        // Tab completion and the screen's own keys stay here; the editing keys
        // are the widget's. The suggestion list is rebuilt after any edit that
        // changed the line, exactly as the old handler did on backspace.
        if (action == GLFW_PRESS && (key == GLFW_KEY_ESCAPE || key == GLFW_KEY_ENTER ||
                                     key == GLFW_KEY_KP_ENTER || key == GLFW_KEY_TAB)) {
            if (key == GLFW_KEY_ESCAPE) {
                setChatOpen(false);
            } else if (key == GLFW_KEY_TAB) {
                cycleChatSuggestion();
            } else {
                submitChatInput();
            }
            return;
        }
        const std::string before = chatInput.value;
        if (editTextField(chatInput, ui::kChatFieldRules, chatMetrics(), key, action) &&
            chatInput.value != before) {
            refreshChatSuggestions();
        }
    }

    void appendChatCodepoint(unsigned int codepoint) {
        // 打开聊天栏的那个按键同时也会产生一个字符；吞掉它，"t" 才不会变成输入的第一个字母
        const bool suppressedT = suppressedOpeningChatCodepoint == static_cast<unsigned int>('t') &&
                                 (codepoint == static_cast<unsigned int>('t') ||
                                  codepoint == static_cast<unsigned int>('T'));
        if (codepoint == suppressedOpeningChatCodepoint || suppressedT) {
            suppressedOpeningChatCodepoint = 0U;
            return;
        }
        suppressedOpeningChatCodepoint = 0U;
        const std::string before = chatInput.value;
        chatInput = ui::textFieldApplyChar(std::move(chatInput), ui::kChatFieldRules, chatMetrics(),
                                           static_cast<char32_t>(codepoint));
        if (chatInput.value != before) {
            refreshChatSuggestions();
        }
    }

    // 三种不占键盘的模式下的按键处理：打开聊天（仅游戏中）与返回/Escape 导航
    // 移动、快捷栏、F3、F5、E、Q 这些按键作用都由 InputSystem 在 processInput() 里按电平采样
    // 再由 dispatchInputEvents() 施加
    // 回调这条路径只承载"有界面打开时仍必须生效"的部分，因为那时上述轮询是被门控关掉的
    void handleScreenKey(int key, int action) {
        if (action != GLFW_PRESS) {
            return;
        }
        if ((key == GLFW_KEY_T || key == GLFW_KEY_SLASH) &&
            screenMode() == input::ScreenMode::Play) {
            setChatOpen(true);
            if (key == GLFW_KEY_SLASH) {
                chatInput = ui::textFieldWithValue("/", ui::kChatFieldRules, chatMetrics());
                suppressedOpeningChatCodepoint = static_cast<unsigned int>('/');
            } else {
                suppressedOpeningChatCodepoint = static_cast<unsigned int>('t');
            }
            return;
        }
        if (key == GLFW_KEY_ESCAPE) {
            handleBackKey();
        }
    }

    // 返回/Escape
    // 背包是叠在游戏页上的覆盖层，因此它先于页栈消费返回键
    // 让页面切换先跑会错误地打开暂停菜单，而不是关掉背包直接回到游戏
    void handleBackKey() {
        if (inventoryOpen) {
            setInventoryOpen(false);
            return;
        }
        switch (menuSystem.pageStack.current()) {
        case ui::PageId::VideoSettings:
            menuSystem.pageStack.pop();
            pressedMenuButton = ui::WidgetId::None;
            menuSystem.viewDistanceSliderDragging = false;
            menuSystem.simulationDistanceSliderDragging = false;
            break;
        case ui::PageId::Experimental:
            menuSystem.pageStack.pop();
            pressedMenuButton = ui::WidgetId::None;
            break;
        case ui::PageId::Language:
            // Escape 取消草稿选择；只有 Done 才启动异步的语言重载
            menuSystem.pendingLanguageCode = options.language;
            menuSystem.languageScrollbarDragging = false;
            menuSystem.pageStack.pop();
            pressedMenuButton = ui::WidgetId::None;
            break;
        case ui::PageId::Options:
            menuSystem.pageStack.pop();
            menuSystem.optionsOpen = false;
            pressedMenuButton = ui::WidgetId::None;
            menuSystem.viewDistanceSliderDragging = false;
            menuSystem.simulationDistanceSliderDragging = false;
            menuSystem.masterVolumeSliderDragging = false;
            break;
        case ui::PageId::Pause:
            setPaused(false);
            break;
        case ui::PageId::ConfirmDelete:
        case ui::PageId::WorldList:
            menuSystem.pageStack.pop();
            break;
        case ui::PageId::Game:
            setPaused(true);
            break;
        default:
            break;
        }
    }

    // 菜单页上的滚轮：滚该页自己的那个列表
    void scrollMenuList(int direction) {
        switch (menuSystem.pageStack.current()) {
        case ui::PageId::WorldList:
            scrollWorldList(direction);
            break;
        case ui::PageId::Language:
            scrollLanguageList(direction);
            break;
        case ui::PageId::Controls:
            scrollControlsList(direction);
            break;
        default:
            break;
        }
    }

    // 游戏中滚轮选择快捷栏格位，和其它槽位变更一样走命令队列
    void scrollHotbar(int direction) {
        const std::size_t current = clientMirror_.player().selectedHotbarSlot;
        const std::size_t count = gameplay::Inventory::kHotbarSize;
        gameplay::SwapSlot swap;
        swap.index = direction < 0 ? (current + count - 1U) % count : (current + 1U) % count;
        runtime.enqueueClientCommand(std::move(swap));
    }

    void handleMenuMouseButton(int button, int action) {
        if (button != GLFW_MOUSE_BUTTON_LEFT) {
            return;
        }
        if (action == GLFW_PRESS) {
            handleMenuButtonPress();
        } else if (action == GLFW_RELEASE) {
            handleMenuButtonRelease();
        }
    }

    void handleInventoryMouseButton(int button, int action, int modifiers) {
        if (action == GLFW_RELEASE) {
            // 两个键都能结束拖拽，松开同时也停掉创造页的滚动条拖拽
            handleInventoryButtonRelease();
            return;
        }
        if (action != GLFW_PRESS) {
            return;
        }
        if (button == GLFW_MOUSE_BUTTON_LEFT || button == GLFW_MOUSE_BUTTON_RIGHT) {
            handleInventoryClick(button == GLFW_MOUSE_BUTTON_RIGHT
                                     ? gameplay::InventoryMouseButton::Right
                                     : gameplay::InventoryMouseButton::Left,
                                 (modifiers & GLFW_MOD_SHIFT) != 0);
        }
    }

    void handlePlayMouseButton(int button, int action) {
        if (button == GLFW_MOUSE_BUTTON_LEFT) {
            // 破坏是一条命令，按下时带着瞄准目标发 Start，松开发 Abort
            // 实际推进由服务端在 tick 里完成
            if (action == GLFW_PRESS) {
                destroyButtonHeld = true;
                enqueueDestroyStart();
            } else if (action == GLFW_RELEASE) {
                destroyButtonHeld = false;
                lastDestroyAimBlock.reset();
                enqueueDestroyAbort();
            }
        } else if (button == GLFW_MOUSE_BUTTON_RIGHT) {
            // 使用同理，按下时带着目标发 UseItemOn，松开发 UseItemStop
            // 这样长按进食或连续使用才会结束
            if (std::getenv("MC_REBEDROCK_INTERACT_DEBUG") != nullptr) {
                std::cout << "[interact] mouse RIGHT "
                          << (action == GLFW_PRESS ? "press" : "release")
                          << " (inventoryOpen=" << inventoryOpen << ")" << std::endl;
            }
            if (action == GLFW_PRESS) {
                enqueueUseStart();
            } else if (action == GLFW_RELEASE) {
                enqueueUseStop();
            }
        }
    }

    // 菜单页上的光标移动只对已经在拖拽的控件有意义，各个拖拽标志互斥
    void dragMenuControl() {
        if (menuSystem.languageScrollbarDragging) {
            updateLanguageScrollFromCursor();
        } else if (menuSystem.viewDistanceSliderDragging) {
            updateViewDistanceFromCursor();
        } else if (menuSystem.simulationDistanceSliderDragging) {
            updateSimulationDistanceFromCursor();
        } else if (menuSystem.masterVolumeSliderDragging) {
            updateMasterVolumeFromCursor();
        }
    }

    void dragInventory(double x, double y) {
        if (creativeScrollbarDragging) {
            updateCreativeScrollFromCursor();
        }
        if (inventoryDragActive) {
            collectInventoryDragSlot(x, y);
        }
    }

    // 游戏中光标转动相机
    // 捕获光标后的第一次采样只用来记下参考位置
    // 这样进入游戏时视角不会按指针此前移动的距离猛地跳一下
    void applyLookDelta(double x, double y) {
        if (firstMouseSample) {
            lastMouseX = x;
            lastMouseY = y;
            firstMouseSample = false;
            return;
        }
        const float deltaX = static_cast<float>(x - lastMouseX);
        const float deltaY = static_cast<float>(lastMouseY - y);
        lastMouseX = x;
        lastMouseY = y;
        camera.rotate(deltaX * 0.10F, deltaY * 0.10F);
    }

    void processInput() {
        // 每帧采样一次键盘与视线，作为 MovementInput 经通道送出
        // 服务端把它暂存到权威玩家上，并自行派生 flightAllowed、sprintAllowed 这些受控字段
        // 客户端不再写 gameSession.input()，跨真实连接时它根本没有会话
        //
        // 采样先落到与设备无关的原始帧，再由 InputSystem 按可重绑定的表派生出完整移动意图
        // 意图涵盖 WASD、空格、Shift、Ctrl、手柄左摇杆，以及跳跃与前进的边沿
        // 有界面打开时仍然轮询，好让边沿历史保持连续，但玩法动作被门控掉
        // 于是送给服务端的是一份清零的意图
        input::RawInputFrame frame;
        input::sampleGlfwWindow(window, camera.direction(), frame);
        // 世界能否拿到本帧输入，用的是窗口回调那条同样的输入归属规则
        // 轮询与回调因此不可能对"有界面打开"这件事产生分歧
        const input::InputDispatchGate gate = input::dispatchGateFor(screenMode(), worldReady);
        const bool gameplayEnabled = gate.gameplayEnabled;
        const input::MovementIntent intent =
            inputSystem_.poll(frame, inputEvents_, gameplayEnabled);
        // 动作边沿**每帧**都派发，而不是只在玩法启用时派发
        // E 必须能关掉背包，而背包本身就会关掉玩法轮询
        // F3 与 F5 切换调试与视角，不受任何界面影响
        // 只有严格意义上的游戏中动作，比如切快捷栏和丢弃，才继续受玩法门控
        // 具体哪条动作用哪个门，由派发器逐条决定
        dispatchInputEvents(gate);
        if (!gameplayEnabled) {
            // 有界面打开时送一份清零的意图，让服务端把玩家停下
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
        // 跳跃边沿切换创造飞行，前进边沿喂给疾跑的双击窗口
        // 两者都由 InputSystem 从电平位图派生
        // 键回调攒下的待处理标志会或进来，免得两次轮询之间到达的按键事件丢失
        // 上面的压测装置产生的边沿同样靠这一步保住
        movement.jumpPressed = intent.jumpPressed || pendingJumpPressed_;
        movement.forwardPressed = intent.forwardPressed || pendingForwardPressed_;
        // 基岩风格的自动跳跃是客户端选项，障碍到底跳不跳得上由物理决定
        // flightAllowed 与 sprintAllowed **不**发送，服务端按权威的游戏模式和饱食度自行派生
        movement.autoJump = options.autoJump;
        runtime.sendClientMovement(movement);
        clearPendingInputEdges();
    }

    // 把 InputSystem 的离散动作边沿翻译成渲染器的既有副作用
    // 涵盖开关背包、切快捷栏、丢弃、调试叠加层和视角切换
    // 每条动作各带各的门：
    //
    //   调试 / 视角     无条件触发，任何界面状态下都能切，与 vanilla 的 F3、F5 一致
    //   背包（E）       未暂停且不在聊天输入时触发，因此 E 既能开背包也能关背包
    //                   背包界面自身会关掉玩法轮询，若按 gameplayEnabled 门控就会关不掉
    //                   聊天状态下 E 被当作字符吞掉
    //   丢弃 / 快捷栏   严格限定在游戏中，受 gameplayEnabled 门控
    void dispatchInputEvents(const input::InputDispatchGate& gate) {
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

    // 清掉客户端侧的输入边沿
    // 它们要么已经折进送出的 MovementInput，要么因为界面打开而被丢弃
    void clearPendingInputEdges() {
        pendingJumpPressed_ = false;
        pendingForwardPressed_ = false;
    }

    // 有界面打开时鼠标回调直接返回，躲在界面后面发生的松开永远到不了命令层
    // 因此每次进入界面都必须显式结束挖掘与使用
    // 交互状态现在归玩法侧所有，所以这里排的是 abort 与 stop 两个边沿
    void releaseInteractionButtons() {
        destroyButtonHeld = false;
        lastDestroyAimBlock.reset();
        enqueueDestroyAbort();
        enqueueUseStop();
    }

    void respawnPlayer() {
        // 重生意图经通道送出，运行时立刻应用并重新发布出生点快照
        // 之所以立刻应用，是因为死亡界面下模拟已暂停，没有 tick 会去排空它
        // 随后把快照泵进镜像，相机和下面的读取本帧就能看到出生点，而不是一个 tick 后才更新
        // 客户端不再直接调用 GameSession::respawn，跨进程的客户端根本没有会话
        runtime.sendClientSessionCommand(gameplay::Respawn{});
        runtime.applyClientCommandsNow();
        static_cast<void>(clientMirror_.pump(runtime.clientChannel(), *this));
        camera.setPosition(snapshotCameraEye());
        // 重生把新身体对齐到出生点存下的角度，而不是沿用死亡时的视线，vanilla 的偏航为 0
        // 这里同样适用 /tp 的换算：vanilla 偏航 0 面朝 +Z
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
        // 游戏模式切换经通道送出，运行时立刻权威地应用，模拟暂停或运行中都成立
        // 应用后重新发布，下一次镜像泵送就会更新 uiFrameData_.gameMode
        // 客户端不再直接调用 GameSession::setGameMode
        runtime.sendClientSessionCommand(gameplay::SetGameMode{mode});
        runtime.applyClientCommandsNow();
        std::cout << "Game mode: " << gameplay::gameModeName(mode) << '\n';
        menuSystem.creativeScrollRow = 0U;
        creativeScrollbarDragging = false;
        lastPlayerMode.clear();
    }

    // 菜单从被捕获的游戏光标手里接管时，vanilla 会把光标重新居中到屏幕上
    // 而不是把它留在虚拟捕获位置漂到的地方
    // 调用本函数前会置上 firstMouseSample，居中产生的那次移动事件因此被鼠标回调吞掉
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
        const std::string line = chatInput.value;
        if (line.empty()) {
            setChatOpen(false);
            return;
        }
        if (line.front() == '/') {
            // 命令在下一个 tick 里由运行时的派发器在服务端执行，那里持有世界写区间
            // 结果由帧循环读回并追加进历史
            runtime.enqueueChat(line);
        } else {
            chatHistory.push("<Player> " + line, true, uiTimeSeconds);
        }
        chatInput = {};
        setChatOpen(false);
    }

    // 为光标处的词重算补全列表
    // UI-1 之前光标只可能在末尾，于是这里传的是整行长度；现在光标能停在行中间，
    // 传的就是光标的**字节**偏移——派发器的 suggestion.start 与它同一坐标系
    // 输入一变就调用一次，Tab 只在已算好的列表里循环，不重算
    // 只有以 '/' 开头的命令行才补全，普通聊天不是命令，不给建议
    // vanilla 的聊天界面同样只为 '/' 开头的输入建补全器
    // 派发器对程序化调用方允许省略 '/'，这道门设在 UI 入口，普通聊天因此不会弹出补全框
    void refreshChatSuggestions() {
        if (chatInput.value.empty() || chatInput.value.front() != '/') {
            chatSuggestions_.clear();
            chatSuggestionIndex_ = 0;
            return;
        }
        chatSuggestions_ = runtime.commandDispatcher().suggestions(
            chatInput.value, ui::textFieldByteOffset(chatInput.value, chatInput.cursor));
        chatSuggestionIndex_ = 0;
    }

    // Tab 把高亮移到下一个候选并应用它
    // 应用时替换从 suggestion.start 到**光标**的那半个词，替换完把光标放在插入内容之后
    // 已存的列表保留各自的偏移，因此再按一次 Tab 就换成下一个候选
    void cycleChatSuggestion() {
        if (chatSuggestions_.empty()) {
            refreshChatSuggestions();
        }
        if (chatSuggestions_.empty()) {
            return;
        }
        chatSuggestionIndex_ = (chatSuggestionIndex_ + 1U) % chatSuggestions_.size();
        const auto& suggestion = chatSuggestions_[chatSuggestionIndex_];
        const std::size_t cursorByte =
            ui::textFieldByteOffset(chatInput.value, chatInput.cursor);
        if (suggestion.start > cursorByte) {
            return;
        }
        std::string line = chatInput.value;
        line.replace(suggestion.start, cursorByte - suggestion.start, suggestion.text);
        const std::size_t cursorChars = ui::textFieldCharCount(
            std::string_view{line}.substr(0, suggestion.start + suggestion.text.size()));
        const auto metrics = chatMetrics();
        chatInput = ui::textFieldWithValue(line, ui::kChatFieldRules, metrics);
        chatInput = ui::textFieldMoveCursorTo(std::move(chatInput), ui::kChatFieldRules, metrics,
                                              cursorChars, false);
    }

    // /tp 的两种形式共用同一套目标解析
    // Position3 形式把相对坐标轴按玩家脚底解析后传送过去，并应用可选的旋转
    // std::string 形式是要传送到的实体 id
    // `withRotation` 标记的是 `/tp <x> <y> <z> <yaw> <pitch>` 这一种
    gameplay::CommandResult teleportWithContext(const gameplay::command::CommandContext& context,
                                                bool withRotation) {
        if (const auto position = context.find<gameplay::command::Position3>("destination");
            position.has_value()) {
            // 相对坐标的基准取权威的脚底位置，因为这条命令传送的是服务端玩家
            // 不能取滞后一拍的客户端镜像
            // `~` 轴一律经共享的 resolve() 解析，权威命令用的也是它，坐标因此不会有两种含义
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

    // 传送时不留渲染插值的瑕疵，玩家、物理插值的两个端点和相机一起跳到位
    // 区块流送同时重新居中，目标位置因此已经加载好
    void teleportPlayerTo(glm::vec3 target) {
        gameSession.teleportPlayer(gameplay::kPrimaryPlayerId, target);
        camera.setPosition(snapshotCameraEye());
        chunkStreamer.request(world::chunkPositionFromWorld(target.x, target.z));
    }

    // vanilla 的 /tp 偏航以 0 面朝 +Z，而相机的偏航用 atan2(z, x)，0 面朝 +X
    // vanilla 的俯仰向下为正，相机的向上为正，两者都在这里换算
    void setPlayerLook(const gameplay::command::Rotation2& rotation) {
        camera.setRotation(static_cast<float>(rotation.yaw) + 90.0F,
                           static_cast<float>(-rotation.pitch));
    }

    // 在已发布的实体快照里找第一个注册 id 匹配的生物，返回它的稳定实体 id，两种命名空间都接受
    // 渲染侧为命令指名实体时只看自己画出来的东西，绝不读实时容器
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

    // 按稳定 id 从已发布快照取该生物的渲染位置
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
            // 完整的关界面流程归会话所有，含收起光标物品与合成栏、关闭箱子实体和容器
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

    // ScreenHandler 需要知道的关于当前界面的全部信息
    // 渲染器只拥有"开的是哪个界面"和"创造视图滚到哪里"
    // 由此推导出的槽位排版与点击路由不属于这里
    [[nodiscard]] gameplay::ScreenContext screenContext() const {
        const auto snapshot = clientMirror_.world();
        const auto furnace = snapshot.openFurnace.has_value()
                                 ? gameplay::FurnacePosition{snapshot.openFurnace->x,
                                                             snapshot.openFurnace->y,
                                                             snapshot.openFurnace->z}
                                 : gameplay::FurnacePosition{};
        gameplay::ScreenContext context;
        context.screen = snapshot.openContainerScreen;
        context.chest = snapshot.openChest;
        context.furnace = furnace;
        context.gameMode = uiFrameData_.gameMode;
        context.creativeInventoryTab = menuSystem.creativeTab == ui::CreativeTab::Inventory;
        return context;
    }

    // 已发布快照所隐含的眼高，规则与逐帧相机用的那套潜行判定相同
    // 只需要高度的调用方，比如压测传送要算脚底目标，也从快照读
    [[nodiscard]] float snapshotEyeHeight() const {
        return clientMirror_.player().sneaking
                   ? gameplay::PlayerController::kSneakingEyeHeight
                   : gameplay::PlayerController::kEyeHeight;
    }
    // 相机的跟随点取自已发布的玩家快照，即物理端点加上眼高
    // 传送与重生都会把跳到位的端点同步镜像进快照，同步传送因此当帧可见
    // 相机永远不读实时的控制器
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
        // 先发布"未激活"可以阻止新的 tick 开始
        // 写区间还会等待已经在飞的那个 tick 结束，之后才快照会话状态或让界面切换改动它
        const auto pauseWrite = worldLock.write();
        if (pause && inventoryOpen) {
            setInventoryOpenLocked(false);
        }
        if (pause && chatOpen) {
            chatInput = {};
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
        // 重新锚定到**权威**位置而不是客户端镜像，因为这里写的是服务端玩家
        // 因此必须读会话快照，重生与传送会同步更新它
        // 通道镜像滞后一个 tick，重生之后它还停在死亡位置
        // 用它会把刚重生的玩家又传回自己的尸体上
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

    // 存档界面的列表行位于标题与底部功能按钮之间的那条带内
    // saveListVisibleRowCount 决定能放几行，任何分辨率下行都不会撞上按钮
    [[nodiscard]] ui::UiRect worldListRow(std::size_t index, const ui::HudLayout& layout) const {
        return ui::worldListRow(index, layout, static_cast<float>(swapchainExtent.width));
    }

    // 当前画布尺寸下列表带里能放几行世界
    // 列表填满标题与底部按钮之间的空间，而不是固定三行，形态对齐 vanilla 的存档界面
    [[nodiscard]] std::size_t saveListVisibleRowCount() const {
        return ui::saveListVisibleRowCount(static_cast<float>(swapchainExtent.width),
                                           static_cast<float>(swapchainExtent.height),
                                           menuSystem.guiScaleSetting);
    }

    // 语言界面的选择列表是一个从屏幕左缘拉到右缘的深色框，取存档选择界面那种通栏样式
    // 框高按行数决定，并在标题与灰色提示行之间垂直居中
    // 语言项因此是绝对居中的，而不是挤在一个高框的顶部
    [[nodiscard]] ui::UiRect languageListBox(const ui::HudLayout& layout) const {
        return ui::languageListBox(layout, static_cast<float>(swapchainExtent.width));
    }

    // 列表下方那行灰色的 "(" + options.languageWarning + ")"
    // 位置与 vanilla 的语言界面一致，落在列表与按钮之间
    [[nodiscard]] float languageWarningY(const ui::HudLayout& layout) const {
        return ui::languageWarningY(layout);
    }

    // 通栏深色框内语言列表的一行
    [[nodiscard]] ui::UiRect languageRow(std::size_t index, const ui::HudLayout& layout) const {
        return ui::languageRow(index, layout, static_cast<float>(swapchainExtent.width));
    }

    // 当前画布尺寸下黑框里能放几行语言
    [[nodiscard]] std::size_t languageVisibleRowCount() const {
        return ui::languageVisibleRowCount(static_cast<float>(swapchainExtent.width),
                                           static_cast<float>(swapchainExtent.height),
                                           menuSystem.guiScaleSetting);
    }

    // 前端各页共用的按钮几何
    // 存档界面及其编辑与删除页把按钮锚在底部，世界列表页排成两列
    // 标题页与创建页则保持居中的菜单排布
    [[nodiscard]] ui::UiRect frontendButtonRect(const ui::HudLayout& layout, ui::PageId page,
                                                std::size_t index, std::size_t buttonCount) const {
        return ui::frontendButtonRect(layout, page, index, buttonCount);
    }

    // 推一条系统吐司提示到右上角
    // 只用于已经存在的触发点，比如设置变更
    // 不为尚未实现的系统占位，成就与配方目前没有，因此一条也不推
    void pushSystemToast(std::string title, std::string subtitle) {
        ui::Toast toast;
        toast.kind = ui::ToastKind::System;
        toast.title = std::move(title);
        toast.subtitle = std::move(subtitle);
        toastQueue_.push(std::move(toast));
    }

    // 开启字幕时显示某个音效的无障碍字幕，取自 SoundRegistry.subtitle
    // 由客户端选项门控，选项未接通之前整条字幕流保持静默，不伪造字幕
    void showSoundSubtitle(std::string_view subtitle) {
        if (!options.showSubtitles || subtitle.empty()) {
            return;
        }
        subtitleFeed_.show(std::string{subtitle});
    }

    // 回调工厂，把每个菜单动作绑到渲染器的方法上，捕获 `this`
    // buildPage() 把它们盖到页面各 widget 上，点击因此直接跑到对应效果
    // 这是 Vulkan、存档、音频状态与不含 Vulkan 的 ui:: 模型之间唯一的接缝
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
            menuSystem.createWorldName = {};
            menuSystem.createWorldGameMode = gameplay::GameMode::Survival;
            menuSystem.pageStack.push(ui::PageId::CreateWorld);
        };
        cb.editWorld = [this] {
            if (menuSystem.selectedWorldIndex < menuSystem.saveSummaries.size()) {
                menuSystem.editWorldIdentifier =
                    menuSystem.saveSummaries[menuSystem.selectedWorldIndex].identifier;
                menuSystem.editWorldName = ui::textFieldWithValue(
                    menuSystem.saveSummaries[menuSystem.selectedWorldIndex].displayName,
                    ui::kWorldNameFieldRules, worldNameMetrics());
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
        cb.toggleCreateAllowCommands = [this] {
            menuSystem.createWorldAllowCommands = !menuSystem.createWorldAllowCommands;
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
        // 所有循环选项共用这一个回调
        // 取值、字段和标签都归 ui::OptionCycle 的表所有，这道接缝不含任何逐选项知识
        // 它只做三件事：步进取值、持久化、交给 applyOptionChanged 去反应
        cb.cycleOption = [this](ui::WidgetId id, int direction) {
            const ui::OptionDesc* option = ui::findCyclingOption(id);
            if (option == nullptr) {
                return;
            }
            ui::cycleOptionValue(*option, options, direction);
            persistOptions();
            applyOptionChanged(id);
        };
        cb.cycleDifficulty = [this] {
            if (currentSave.has_value()) {
                currentSave->difficulty = gameplay::nextDifficulty(currentSave->difficulty);
                gameSession.setDifficulty(currentSave->difficulty);
                // 设置变更是一个已经存在的触发点，用右上角的吐司提示向玩家确认
                pushSystemToast("Difficulty",
                                std::string{gameplay::difficultyName(currentSave->difficulty)});
            }
        };
        cb.selectLanguageRow = [this](std::size_t row) {
            if (row < menuSystem.languageCodes.size()) {
                menuSystem.pendingLanguageCode = menuSystem.languageCodes[row];
                playUiClick();
            }
        };

        // 点击某一行开始为该动作捕获下一次按键，Reset 恢复 vanilla 默认值
        // 两者都经 keyBindScreen_ 作用在 InputSystem 这一唯一来源上，绝不改私有副本
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

    // 当前页面的矩形来自 HudLayout，按 widget 序号索引，遵循 frontendButtonRect 的约定
    [[nodiscard]] ui::RectProvider menuRectProvider() const {
        const ui::PageId page = menuSystem.pageStack.current();
        const ui::HudLayout layout{static_cast<float>(swapchainExtent.width),
                                   static_cast<float>(swapchainExtent.height),
                                   menuSystem.guiScaleSetting};
        const std::size_t count = menuButtonCount();
        const float fbWidth = static_cast<float>(swapchainExtent.width);
        // 按键设置页里前 `keyRows` 个 widget 是可滚动的按键列表行，走 controlsRow 排版
        // 末尾四个是底部按钮带，其余页面的每个 widget 都是普通前端按钮
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

    // 本帧按键设置列表上可见的行数，即可见窗口，钳制到可重绑定动作的总数
    [[nodiscard]] std::size_t controlsVisibleKeyBindRowCount() const {
        const std::size_t total = input::keyBindRows().size();
        const std::size_t window = ui::controlsVisibleRowCount(
            static_cast<float>(swapchainExtent.width),
            static_cast<float>(swapchainExtent.height), menuSystem.guiScaleSetting);
        const std::size_t first = std::min(menuSystem.controlsListFirstIndex, total);
        return std::min(window, total - first);
    }

    // 给定页面上光标所在的 widget 下标，没有则返回 kNoWidget
    // 它就是模型对该页已排版矩形做的命中测试
    [[nodiscard]] std::size_t hoveredMenuIndex(const ui::Page& page) const {
        const auto cursor = currentFramebufferCursor();
        return ui::hitTest(page, cursor.x, cursor.y);
    }

    // 把当前页面装配成一个 ui::Page 值，这是唯一的构建点
    [[nodiscard]] ui::Page buildCurrentPage() {
        ui::MenuBuildContext ctx;
        ctx.worldOpen = currentSave.has_value();
        ctx.worldSelectable = !menuSystem.saveSummaries.empty();
        ctx.worldRowCount = 0;       // list rows are drawn by the list path today
        ctx.languageRowCount = 0;
        // 按键设置列表是可滚动的，只构建可见窗口，页面因此不会超出排版的按钮数上限
        if (menuSystem.pageStack.current() == ui::PageId::Controls) {
            ctx.keyBindFirstIndex = menuSystem.controlsListFirstIndex;
            ctx.keyBindRowCount = controlsVisibleKeyBindRowCount();
        }
        // 每行的标签形如"动作: 按键"，取自 InputSystem 这一唯一来源
        // 该行正在捕获时改显示为"动作: > ? <"
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

    // 选项变更之后做出反应的**唯一**地方
    // 选项有哪些取值、标签怎么读，归 ui::OptionCycle 的表所有
    // 渲染器只管这次变更要付什么代价：重建交换链或采样器、重网格化世界、重调某个在跑的子系统
    // 在这里没有条目的选项，下一次有人读那个字段时自然生效，大多数选项都是这样
    void applyOptionChanged(ui::WidgetId id) {
        switch (id) {
        case ui::WidgetId::AntiAliasing:
        case ui::WidgetId::Vsync:
            recreateSwapchain();
            break;
        case ui::WidgetId::Anisotropy:
            recreateTextureSampler();
            break;
        case ui::WidgetId::SmoothLighting:
            applySmoothLightingQuality();
            break;
        case ui::WidgetId::ForceUnicodeFont:
            textFont.setForceUnicode(options.forceUnicodeFont);
            recreateFontTexture();
            break;
        case ui::WidgetId::Subtitles:
            if (!options.showSubtitles) {
                subtitleFeed_.clear();
            }
            break;
        case ui::WidgetId::RainMode:
            rainMode_ = static_cast<RainMode>(std::clamp(options.rainMode, 0, 1));
            break;
        case ui::WidgetId::ParticleLevel:
            applyParticleLevel();
            break;
        case ui::WidgetId::SunShadows:
            shadowDisabled = !options.sunShadows;
            break;
        case ui::WidgetId::RainCollisionCache:
            rainSystem.setCollisionCache(options.rainCollisionCache);
            break;
        default:
            break;
        }
    }

    // 网格是按当前的平滑光照画质烘出来的，打包顶点只带一套 AO 数据
    // 改画质因此要重网格化所有常驻 section
    // 选 Off 时沿用已有的烘焙结果，由着色器丢掉 AO，所以 Off 不触发重网格化
    void applySmoothLightingQuality() {
        const auto baked = options.smoothLightingQuality == world::SmoothLightingQuality::Off
                               ? currentMeshQuality
                               : options.smoothLightingQuality;
        if (baked == currentMeshQuality) {
            return;
        }
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
        // 按下的 widget 经 ui:: 模型解析成当前页面里的下标
        // pressedMenuButton 只作为绘制与调试用的 id 保留
        const ui::Page page = buildCurrentPage();
        pressedMenuIndex_ = hoveredMenuIndex(page);
        // 稳定 id 驱动绘制高亮，决定哪个 widget 画成按下态，下标驱动派发
        // 按空了则两者都为空
        pressedMenuButton = pressedMenuIndex_ != ui::kNoWidget
                                ? static_cast<ui::WidgetId>(page[pressedMenuIndex_].debugId)
                                : ui::WidgetId::None;
        // 按在滑块上即开始拖拽，拖拽效果一律经该滑块的 onDrag 回调，绝不是遍历的副作用
        // 拖拽标志保留下来，松开路径与绘制高亮才继续可用
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
        // 26.1 在界面打开期间只维护一个草稿选择，按 Done 才用一次资源重载提交
        // 于是连着浏览好几行不会反复解析翻译、反复重建字体
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

    // vanilla 的模拟距离滑块，单位为区块，表示实体被冻结之外的半径
    // 它作用在会话的 tick 门控上，因此与渲染距离相互独立
    // 取值范围 2 到 12 个区块，单位与视距滑块相同
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
        // Cocoa 与 Win32 上 GLFW 都会先送最大化或还原事件，再送随之而来的尺寸事件
        // 因此即便是"先拖动改大小、紧接着最大化"这种快速序列也能记对，不会把最大化后的尺寸存下来
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
        // 最大化的窗口报出的是整块显示器大小的客户区
        // 这里保留最后一次普通窗口尺寸，本次还原或下次启动才能回到玩家真正的窗口大小
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
        // macOS 上直接设置客户区尺寸不会清掉最大化标志，请求的尺寸因此永远落不下去
        // 所以先还原窗口，再把新分辨率当作普通窗口尺寸应用
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
            // vanilla 的滑块在松开时给一声反馈
            // 试听放在监听者位置，避免距离衰减把"音频到底通不通"给盖住
            if (options.masterVolume > 0.0F) {
                audioSystem.playItemPickup(camera.position());
            }
        }
        // 经 ui:: 模型派发，按下时已记下 widget 下标
        // 松开落在同一个且启用的 widget 上时，就跑它的 onActivate
        const ui::Page page = buildCurrentPage();
        const std::size_t released = hoveredMenuIndex(page);
        const std::size_t pressed = pressedMenuIndex_;
        pressedMenuButton = ui::WidgetId::None;
        pressedMenuIndex_ = ui::kNoWidget;
        menuSystem.viewDistanceSliderDragging = false;
        menuSystem.simulationDistanceSliderDragging = false;
        menuSystem.masterVolumeSliderDragging = false;
        menuSystem.languageScrollbarDragging = false;
        // 滑块松开时经它自己的回调提交，含持久化与反馈音
        if (pressed != ui::kNoWidget && page[pressed].kind == ui::WidgetKind::Slider &&
            page[pressed].slider.onCommit) {
            page[pressed].slider.onCommit();
            return;
        }
        // 按钮在同一个 widget 上松开即视为点击，执行它的 onActivate
        if (released != ui::kNoWidget && released == pressed) {
            playUiClick();
        }
        const auto cursor = currentFramebufferCursor();
        static_cast<void>(ui::dispatchActivate(page, pressed, cursor.x, cursor.y));
    }

    // 按下与"结束拖拽的那次松开"共用的槽位与创造页命中测试
    // 先解析光标位置，再对它下面的东西动作
    // 那可能是容器槽、玩家背包槽、创造页签，或者是空白处——那就把光标上的物品堆丢出去
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
        // vanilla 里背包与容器的槽位是静音的，只有真正的按钮控件才播 ui.button.click
        // 拿起或移动物品因此没有点击声，这一族界面里只有上面那些菜单按钮出声
        //
        // ENCH-2：附魔台的三条选项条是真按钮而不是槽位，所以先于槽位表命中测试
        // 客户端只报"按了第几条"，能不能买、扣多少级和青金石、上什么附魔全在服务端判
        if (clientMirror_.world().openContainerScreen == ContainerScreen::EnchantingTable) {
            for (std::size_t option = 0; option < 3U; ++option) {
                if (!layout.enchantingOption(option).contains(framebufferCursor.x,
                                                              framebufferCursor.y)) {
                    continue;
                }
                gameplay::ClickEnchantOption click;
                click.optionIndex = static_cast<int>(option);
                runtime.enqueueClientCommand(std::move(click));
                return;
            }
        }
        // 只有一张槽位表，按槽位自身的类型路由
        const auto slots = gameplay::ScreenHandler::buildSlotLayout(screenContext(), layout);
        if (const auto* slot = gameplay::ScreenHandler::slotAt(slots, framebufferCursor);
            slot != nullptr) {
            // 点击被命令化，在服务端 tick 上经交互执行
            // 由交互解析出实际存储并按槽位类型路由
            gameplay::ClickSlot click;
            click.kind = slot->kind;
            click.slotIndex = slot->index;
            click.button = static_cast<int>(button);
            click.shiftHeld = shiftHeld;
            // 服务端看不到客户端当前选的创造页签，因此由点击自己带上
            // 在物品分类页签上 Shift 点击等于丢进无限目录里销毁
            // 在背包页签上则是普通的移动，行为与 screenContext() 里的 creativeInventoryTab 一致
            click.creativeInventoryTab = menuSystem.creativeTab == ui::CreativeTab::Inventory;
            runtime.enqueueClientCommand(std::move(click));
            return;
        }
        if (clientMirror_.world().openContainerScreen !=
            ContainerScreen::PlayerInventory) {
            // 容器打开时点在所有槽位之外会把手上的物品堆扔到地上
            // 但必须点在面板本身之外才算
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
                // 36 个背包格与快捷栏都是真实的玩家背包槽，由上面的槽位命中测试路由
                // 留到这里的只剩删除框和面板之外的空白
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
                        // 创造目录里的空格子是删除目标
                        // 只有点在面板之外才会生成掉落物实体，与 vanilla 的容器一致
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
            // 快捷栏由上面的槽位命中测试路由，只有面板之外的空白才会把光标物品堆扔出去
            if (!layout.creativePanel().contains(framebufferCursor.x, framebufferCursor.y)) {
                gameplay::DropCursor drop;
                drop.lookDirection = camera.direction();
                runtime.enqueueClientCommand(std::move(drop));
            }
            return;
        }
        // 生存模式下 36 个背包格由槽位命中测试路由，点在面板之外则扔出光标物品堆
        if (!layout.inventoryPanel().contains(framebufferCursor.x, framebufferCursor.y)) {
            gameplay::DropCursor drop;
            drop.lookDirection = camera.direction();
            runtime.enqueueClientCommand(std::move(drop));
        }
    }

    // 快速合成拖拽的按下阶段
    // vanilla 把一次点击拆成两段：光标为空时按下立即生效，走拿起、快速移动或创造点击
    // 光标上有东西时按下只是开始拖拽，物品堆留在光标上
    // 直到松开才把它分配到划过的各个槽位，若指针没动过就整堆放进松开处的那个槽
    void handleInventoryClick(gameplay::InventoryMouseButton button, bool shiftHeld) {
        // 记下上一次的槽位、按键和时间，250 毫秒内再次按下同一槽位即算双击
        // 松开时把双击变成"收拢全部同类物品"的动作
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

    // 创造目录的格子与控件在按下那一刻就生效
    // 目录点击创建或调整光标物品堆，页签立即切换，删除槽立即清空，滚动条开始自己的拖拽
    // 真实物品槽被有意排除在外
    // 这样快速合成拖拽与双击收拢在两种游戏模式下、以及创造模式打开的容器里都走同一套状态机
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

    // 当前帧缓冲对应的 HUD 排版
    // 槽位几何属于界面，凡是要向 ScreenHandler 要槽位的地方都得先有它
    [[nodiscard]] ui::HudLayout currentHudLayout() const {
        int framebufferWidth = 0;
        int framebufferHeight = 0;
        glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
        return ui::HudLayout{static_cast<float>(framebufferWidth),
                             static_cast<float>(framebufferHeight), menuSystem.guiScaleSetting};
    }

    // 鼠标下的槽位，表示为"类型加下标"的值，用于双击判定
    // 它绝不是指向玩法侧存储的指针
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

    // 当前界面下玩家能够到的全部槽位，供双击收拢使用
    // 含容器的输入槽和整个玩家背包
    // 输出槽被排除，因为它们从不持有自己的存储
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

    // 光标下的槽位，表示为"类型加下标"的值，没有则返回 nullopt
    // 空白区域、放不进物品的输出槽、创造页签都算没有
    // 快速合成拖拽收集的就是这些值
    [[nodiscard]] std::optional<gameplay::SlotRef> dragSlotAt(const ui::HudLayout& layout,
                                                              const ui::UiPoint& cursor) const {
        const auto slots = gameplay::ScreenHandler::buildSlotLayout(screenContext(), layout);
        const auto* slot = gameplay::ScreenHandler::slotAt(slots, cursor);
        if (slot == nullptr || !slot->acceptsItems()) {
            return std::nullopt;
        }
        return gameplay::SlotRef{slot->kind, slot->index};
    }

    // 快速合成拖拽期间光标划过的某个槽位在屏幕上的矩形
    // 若该指针已不属于当前界面，比如容器已关闭，则返回 nullopt
    // 拖拽记下的"类型加下标"会被解析回几何，预览因此总是落在拖拽真正会写入的那个槽上
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

    // 从快照取某个槽位当前的物品堆
    // 拖拽预览与落位数量因此读的是 HUD 正在画的同一份已发布显示状态
    [[nodiscard]] gameplay::ItemStack
    snapshotStackAt(gameplay::SlotKind kind, std::uint16_t index) const {
        // 世界快照是按值拷贝，因此这里按值返回物品堆，而不是返回指向那份拷贝的引用
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
        case gameplay::SlotKind::EnchantingItem:
            return snap.enchantingItem;
        case gameplay::SlotKind::EnchantingLapis:
            return snap.enchantingLapis;
        case gameplay::SlotKind::AnvilLeft:
            return snap.anvilLeft;
        case gameplay::SlotKind::AnvilRight:
            return snap.anvilRight;
        case gameplay::SlotKind::AnvilOutput:
            return snap.anvilResult;
        case gameplay::SlotKind::Equipment:
            // 存储侧每 tick 发布的四个护甲槽加副手，即 WorldSnapshot::equipmentSlots
            // 该数组按 EquipmentSlot 的底层值索引
            // 而这里的 `index` 是界面自己的绘制顺序，头、胸、腿、脚、副手对应枚举值 4、3、2、1、0
            // 因此要经 equipmentSlotAt() 转换，而不是拿绘制序号直接索引按枚举排列的数组
            // 点击路由 ScreenHandler::resolveSlotStorage 用的也是这个转换
            return snap.equipmentSlots[static_cast<std::size_t>(
                gameplay::equipmentSlotAt(index))];
        case gameplay::SlotKind::PlayerCraftingOutput:
        case gameplay::SlotKind::TableCraftingOutput:
            // 输出槽不是拖拽目标，acceptsItems 为假，预览不会来问它
            // 这里仍返回一个共享的空物品堆兜底
            break;
        }
        static const gameplay::ItemStack kEmptyPreviewStack;
        return kEmptyPreviewStack;
    }

    // 当前拖拽会往每个已收集槽位放多少，与 Inventory::dragDistribute 完全一致
    // 左键拖拽在能接收的槽位之间尽量均分光标物品堆，右键拖拽每格放一个
    // 数量为 0 表示该槽位收不下拖拽中的物品，可能是堆满了，也可能是不同物品
    // 预览会跳过这些槽，真正的分配同样如此
    // 读的是容器显示快照，绝不读实时槽位
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

    // 在光标划过的槽位上画出拖拽松手后的落位，松开之前就能看到会放到哪
    // vanilla 的做法是把划过的槽位压暗，再画一份光标物品堆并标上该格会落的数量
    // 这里在已经画好的槽位之上复现同样的预览
    void handleInventoryButtonRelease() {
        creativeScrollbarDragging = false;
        if (cancelNextInventoryRelease) {
            // 按下时就已经生效的操作，比如空光标下的拿起或快速移动，松开时无事可做
            // 顺便丢掉双击标志，后续的松开因此不会拿它做文章
            cancelNextInventoryRelease = false;
            isDoubleClicking = false;
            return;
        }
        if (isDoubleClicking) {
            // 双击某个槽位会把界面里所有同类物品堆收拢到光标上，与 vanilla 一致
            // 第二次按下已经开始了一次拖拽，因此把那份状态一并清掉
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
        if (inventoryDragSlots.size() > 1U) {
            // 拖拽划过了两个以上真实槽位，于是在服务端 tick 上把光标物品堆分配下去
            // 左键均分，右键每格一个
            gameplay::DragDistribute drag;
            drag.button = inventoryDragButton;
            drag.targets = std::move(inventoryDragSlots);
            runtime.enqueueClientCommand(std::move(drag));
        } else {
            // 划过 0 个或 1 个槽位算普通点击，不算快速合成
            // vanilla 也只在跨多个槽位时才快速合成，单槽退回普通的拿起放下
            // 这一点很重要：分配逻辑只填空槽或同类槽，见 dragPlacementCounts 的接收判定
            // 把单槽"拖拽"送进去，会静默地拒绝覆盖一个已经放着**别的**物品的槽
            // 那正是创造模式里"选中物品再点快捷栏，有时要点好几下"的成因
            // dispatchInventoryClick 会重新命中测试光标下的槽，并按真实点击的方式放置
            dispatchInventoryClick(inventoryDragButton, false);
        }
        inventoryDragActive = false;
        inventoryDragSlots.clear();
    }

    // 快速合成拖拽的收集阶段，解析光标下的槽位并加进拖拽集合，按住期间每个槽只加一次
    // vanilla 只在光标上的物品数多于已收集槽数时才继续收集
    // 一次拖拽因此不会索要超过它所携带的数量
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
        // 必须赶在 tick 会碰到的任何东西被销毁之前停下
        // jthread 析构时本来也会 join，但那发生在下面的 Vulkan 销毁之后
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
            if (worldPipelines_.shadowDebugPipelineLayout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(device, worldPipelines_.shadowDebugPipelineLayout, nullptr);
            }
            if (worldPipelines_.shadowPipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(device, worldPipelines_.shadowPipeline, nullptr);
            }
            if (worldPipelines_.shadowPipelineLayout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(device, worldPipelines_.shadowPipelineLayout, nullptr);
            }
            for (auto& queryPool : occlusion_.queryPools) {
                if (queryPool != VK_NULL_HANDLE) {
                    vkDestroyQueryPool(device, queryPool, nullptr);
                    queryPool = VK_NULL_HANDLE;
                }
            }
            if (occlusion_.queryLayout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(device, occlusion_.queryLayout, nullptr);
            }
            if (occlusion_.boxVertexBuffer.buffer != VK_NULL_HANDLE) {
                destroyBuffer(occlusion_.boxVertexBuffer);
            }
            if (occlusion_.boxIndexBuffer.buffer != VK_NULL_HANDLE) {
                destroyBuffer(occlusion_.boxIndexBuffer);
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

    // 转发到 VulkanResources，命令池与图形队列归它所有
    // 这里和纹理一族的调用点因此写法不变，与上面 createBuffer、createImage 的转发同一形态
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
                        // 水不阻挡任何东西，所以玩家要站的那两格必须单独检查有没有水
                        // 否则搜索会心安理得地选中一片海床
                        const bool submerged =
                            world::isFluid(interactionWorld.block(x, y + 1, z)) ||
                            world::isFluid(interactionWorld.block(x, y + 2, z));
                        if (!naturalSurface || submerged ||
                            world::hasCollision(interactionWorld.block(x, y + 1, z)) ||
                            world::hasCollision(interactionWorld.block(x, y + 2, z))) {
                            continue;
                        }
                        // 洞穴地板同样满足上面每一条检查，石头上面是空气
                        // 因此候选点还必须对天空可见
                        // 出生格与世界顶之间只要有实心方块，就判定为洞穴并跳过该列
                        // vanilla 出生在高度图的顶层方块上，那正是这个表面
                        // y+1 与 y+2 是两格高的玩家所占的格子，扫描因此从它们之上开始
                        // 树叶不算遮挡，探出的树冠仍属户外，不像洞顶，不能让森林地面被否掉
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
            // 严格扫描一无所获，可能是首选中心附近全是大海或被完全覆盖
            // vanilla 的出生点搜索从不放弃，这里也不能放弃
            // 搜索若无位置返回，spawnPositionInitialized 会一直为假，加载画面就永远停着
            // 本次运行如此，同一种子每次重新加载也如此
            // 于是回落到中心处最高的实心表面，海床也没关系，玩家能游上来
            // 若连这个也失败，再回落到默认脚底位置
            // 回落只在中心区块加载完成后进行，这样读到的是真实地形而不是空气
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
        // vanilla 在服务端整个生命周期内都保持出生区块常驻
        // 这里同样标记世界出生点周围的区块，它们不会在玩家脚下被流送出去
        chunkStreamer.protectChunks(world::chunkPositionFromWorld(feet.x, feet.z),
                                    kSpawnChunkRadius);
        std::cout << "Spawn position: " << feet.x << "," << feet.y << "," << feet.z << '\n';
    }

    void playUiClick() { audioSystem.playButtonClick(camera.position()); }

    // 给环境音与音乐调度器喂一个 tick
    // 这里构建情境上下文，区分菜单与主世界、创造与游戏中
    // 世界已加载时还会在眼点附近随机取一格采样洞穴氛围亮度，那是 vanilla 每 tick 采的输入
    // 调度与阈值逻辑全在音频系统里，这里只收集渲染侧的上下文
    // 群系环境音循环暂时留空，等下界与洞穴群系落地后再补
    // them (记账).
    void driveAmbientMusic(float deltaSeconds) {
        audio::AudioSystem::AmbientMusicContext context;
        context.listenerPosition = camera.position();
        // 调度器按 20 tps 的游戏 tick 计数，不按帧
        // 这里吸收本帧的真实时间，并把它跨过的整 tick 数传下去
        // 多数帧是 0，只有长帧才大于 1
        // 于是音乐间隔与洞穴氛围的速率与帧率解耦，不会跟着渲染帧率狂奔
        context.ticks = ambientMusicTicks_.advance(deltaSeconds);
        if (!worldSessionActive) {
            context.situation = audio::MusicSituation::Menu;
        } else {
            context.situation = uiFrameData_.gameMode == gameplay::GameMode::Creative
                                    ? audio::MusicSituation::Creative
                                    : audio::MusicSituation::Game;
            // 在眼点周围氛围搜索范围 8 格之内随机取一格
            // 逐帧的线性同余发生器让采样很廉价，也不必动用世界的随机数
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
    // 定步长累加器，把帧时间换成 20 tps 的整 tick 供环境音与音乐调度器使用
    // 它与帧率解耦，做法与 SimulationDriver 一致
    audio::TickAccumulator ambientMusicTicks_;

    // 掷被破坏方块的战利品表，把掉出来的东西放在它原先那一格上
    // 多个物品堆按黄金角散开，免得互相叠在一起
    // 消耗手持工具的耐久，耐久耗尽时播放 vanilla 的损坏音效
    // 创造模式不磨损工具，因此只有生存模式的调用方会走到这里
    // 攻击时忽略 `blockHardness`，其余情况传入被挖方块的硬度
    // 因为 vanilla 对瞬间就碎的方块不收耐久

    // 一次挥击，若射线先碰到生物再碰到方块，则由该生物挨这一下
    // 击中生物时返回 true，调用方因此在这次点击里跳过挖掘路径
    // 交互中属于渲染线程的那一半，把瞄准目标打包成值类型的命令交给玩法控制器
    // 控制器跑在服务端 tick 内，这里不改动世界的任何状态
    void enqueueInteractionCommand(gameplay::GameCommand command) {
        // abort 与 stop 两个边沿永远安全也永远需要，躲在界面后面的松开必须结束挖掘与使用
        // start 边沿由调用方门控，暂停或菜单期间的按下因此不会排进陈旧命令
        if (worldSessionActive) {
            runtime.enqueueClientCommand(std::move(command));
        }
    }

    // 打包好的攻击动作，按下时带上瞄中的生物或方块，松开时结束挖掘
    // 具体怎么判定、怎么推进由服务端在 tick 里决定
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

    // 按住攻击时目标跟随实时射线
    // 按下先挖第一个方块，等它消失或玩家看向别处，下一帧就为新够到的那格再发一次 StartDestroy
    // 没有这次交接，玩法控制器会一直挖第一格已经变成空气的状态
    // 于是连草这种一碰就碎的方块也得再点一下
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

    // 打包好的使用动作，右键按下时带上方块目标，对空使用则不带
    // 松开时结束长按使用
    void enqueueUseStart() {
        const bool interactDebug = std::getenv("MC_REBEDROCK_INTERACT_DEBUG") != nullptr;
        if (!(worldReady && !paused && !inventoryOpen && !chatOpen)) {
            // 这道门会静默吞掉右键，一旦触发就根本不会发出使用命令
            // 追踪因此止步于此，而不是止步于服务端处理函数
            if (interactDebug) {
                std::cout << "[interact] use-start BLOCKED (worldReady=" << worldReady
                          << " paused=" << paused << " inventoryOpen=" << inventoryOpen
                          << " chatOpen=" << chatOpen << ")" << std::endl;
            }
            return;
        }
        // MC_REBEDROCK_INTERACT_DEBUG 让每次右键打一行
        // 内容是准星解析到实体、方块、空这三种使用目标中的哪一种，以及客户端认为手上拿的是什么
        // 这是追踪"剪毛、染色、喂食这类生物交互没生效"时属于客户端的那一半
        if (interactDebug) {
            const auto& held = clientMirror_.player().heldStack;
            const std::string_view heldName =
                held.item != nullptr ? std::string_view{held.item->identifier.path}
                : gameplay::isBlockStack(held) ? std::string_view{"<block>"}
                                               : std::string_view{"<empty>"};
            if (creatureHit.has_value()) {
                std::cout << "[interact] use-start -> ENTITY id=" << creatureHit->entityId
                          << " held=" << heldName << std::endl;
            } else if (targetedBlock.has_value()) {
                std::cout << "[interact] use-start -> BLOCK held=" << heldName << std::endl;
            } else {
                std::cout << "[interact] use-start -> EMPTY (no entity/block hit) held=" << heldName
                          << std::endl;
            }
        }
        // 准星下的生物同样接管使用键，比如剪毛和喂食，优先级与攻击键给它的一致
        // creatureHit 只有在它是两个命中里更近的那个时才会被置上
        if (creatureHit.has_value()) {
            gameplay::UseItemOn use;
            use.entity = true;
            use.entityId = creatureHit->entityId;
            use.lookDirection = camera.direction();
            enqueueInteractionCommand(std::move(use));
        } else if (targetedBlock.has_value()) {
            gameplay::UseItemOn use;
            use.block = targetedBlock->block;
            use.adjacent = targetedBlock->adjacent;
            use.face = world::orientationFromOffset(targetedBlock->adjacent -
                                                    targetedBlock->block);
            // 射线打在方块形状上的精确交点，放置逻辑因此能读到格内的命中高度
            // 台阶就靠它决定放在玩家瞄准的那一半上，只带格心会把这个小数丢掉
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

    // 逐帧的瞄准目标，纯读取，从相机向世界和生物群发射线
    // 交互控制器经命令拿到结果，绘制通道则用它画选择框
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
        // 生物的碰撞盒像方块形状一样挡住射线，vanilla 的命中结果取方块与实体里更近的那个
        // 测试对象是已发布的实体快照，绝不是实时容器
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

    // 让整套 BMP unihex 常驻
    // 26.1 是惰性挑选并烘焙字形的，而本渲染器用的是页数组层
    // 预加载有界的 256 页等价物能得到同样的切换语言性质
    // 切换时不会重新解析 unifont.zip，不必等设备，也不重建描述符集
    // 空页由 TextureManager 略去
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

    // 只有强制 Unicode 字体的 provider 变化时才重建字体数组
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
        // 太阳阴影深度图，由地形片元着色器采样
        // 单独一个绑定点，只有 grass_block.frag 以及将来的受光通道看得到它
        // 其它管线不写它也不会出问题
        VkDescriptorSetLayoutBinding shadowSamplerBinding{};
        shadowSamplerBinding.binding = 8;
        shadowSamplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        shadowSamplerBinding.descriptorCount = 1;
        shadowSamplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutBinding rainSamplerBinding = samplerBinding;
        rainSamplerBinding.binding = 9;
        rainSamplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        // 绑定点 6/7 曾是按世界位置烘好的群系草色/叶色查找纹理
        // 群系配色改成了顶点上的 tint（BM-1），那两张纹理与它们的绑定点一并撤掉
        // 绑定号不必连续：阴影仍是 8，雨仍是 9，着色器不用改号
        const std::array bindings{uniformBinding,       samplerBinding,
                                  fontSamplerBinding,   guiSamplerBinding,
                                  entitySamplerBinding, panoramaSamplerBinding,
                                  shadowSamplerBinding, rainSamplerBinding};
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
            VkDescriptorImageInfo rainImageInfo{};
            rainImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            rainImageInfo.imageView = textures_.rainTextureView;
            rainImageInfo.sampler = textures_.textureSampler;
            std::array<VkWriteDescriptorSet, 7> writes{};
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
            writes[6].dstBinding = 9;
            writes[6].descriptorCount = 1;
            writes[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[6].pImageInfo = &rainImageInfo;
            vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()), writes.data(),
                                   0, nullptr);
        }
    }

    // 场景描述符集，即 set 1，持有实例化粒子管线要读的逐帧存储缓冲
    // 单独一套布局能让共享的相机与纹理 set 0 保持原样
    // 现有管线都只绑一个 set，而存储缓冲的阶段标志将来可以扩到 COMPUTE，互不干扰
    void createSceneDescriptorResources() {
        // 3 MiB holds ~65,536 ParticleRecords: the 疯狂 particle level's
        // 18000 个雨滴加上扩大后 24000 的共享粒子池，连同玩法预留量，一起装进单个逐帧缓冲
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

    // 实例化粒子管线从场景存储缓冲 set 1 读逐粒子记录
    // 面向相机的四边形在顶点着色器里展开
    // 整批粒子只发一次绘制调用，取代过去逐粒子的 vkCmdDraw
    // 顶点输入为空，粒子数据全部经 SSBO 加 gl_InstanceIndex 送达
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
        // 面向相机的公告板没有有意义的背面
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
        checkVk(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &worldPipelines_.particlePipelineLayout),
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
        pipelineInfo.layout = worldPipelines_.particlePipelineLayout;
        pipelineInfo.renderPass = worldPipelines_.renderPass;
        checkVk(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                          &worldPipelines_.particlePipeline),
                "vkCreateGraphicsPipelines(particle)");
        vkDestroyShaderModule(device, vertexModule, nullptr);
        vkDestroyShaderModule(device, fragmentModule, nullptr);
    }

    // 太阳空间的阴影预通道把视锥内的地形渲染进一张离屏深度图
    // 该通道不需要描述符集，只用 80 字节推送常量，即光源视图投影矩阵加 section 原点
    // 顶点数据与主通道共用同一批 VoxelVertex 缓冲
    // 目标、管线与布局都与交换链无关，只创建一次
    void createShadowResources() {
        shadowTarget.init({&resources_, device, 2048U, 2048U});
        // 下面的描述符声明布局为 SHADER_READ_ONLY_OPTIMAL
        // 在 Vulkan 看来，三个地形与实体片元着色器都无条件采样 binding 8
        // 而太阳阴影默认是关的，预通道会提前返回，从不转换这张图像的布局
        // 于是它停在 UNDEFINED，却有一堆绘制声称并非如此，这是未定义行为
        // 它在真实硬件上的表现是一次静默的 GPU 故障
        // 画面闪一下品红，随后下一次 vkWaitForFences 返回 VK_ERROR_DEVICE_LOST
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
        checkVk(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &worldPipelines_.shadowPipelineLayout),
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
        // 没有颜色附件，纯深度通道不需要混合状态
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
        pipelineInfo.layout = worldPipelines_.shadowPipelineLayout;
        pipelineInfo.renderPass = shadowTarget.renderPass();
        checkVk(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                          &worldPipelines_.shadowPipeline),
                "vkCreateGraphicsPipelines(shadow)");
        vkDestroyShaderModule(device, vertexModule, nullptr);
        vkDestroyShaderModule(device, fragmentModule, nullptr);

        createShadowDebugResources();

        // 把每一帧的 set 0 的 binding 8 指向阴影深度图，地形着色器才能采样它
        // 图像视图与采样器此刻已存在
        // 图的内容由预通道每帧重写，这与描述符声明的 SHADER_READ_ONLY 布局本就吻合
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

    // 阴影图调试叠加层在屏幕一角用一个四边形采样离屏深度纹理，开发期因此能看到预通道的输出
    // 集布局、采样器、描述符集和管线布局都只创建一次
    // 只有管线与交换链绑定，因为它按当前 MSAA 采样数渲染进主通道
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
        checkVk(vkCreatePipelineLayout(device, &pushInfo, nullptr, &worldPipelines_.shadowDebugPipelineLayout),
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
        pipelineInfo.layout = worldPipelines_.shadowDebugPipelineLayout;
        // 阴影调试叠加层跟 HUD 一起画，因此归 GUI 那趟（单采样、编码值）
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        pipelineInfo.renderPass = worldPipelines_.guiRenderPass;
        checkVk(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                          &worldPipelines_.shadowDebugPipeline),
                "vkCreateGraphicsPipelines(shadow debug)");
        vkDestroyShaderModule(device, vertexModule, nullptr);
        vkDestroyShaderModule(device, fragmentModule, nullptr);
    }

    // 在主渲染通道之前录制太阳空间的深度预通道
    // 先用光锥剔除已加载的 section，再用阴影管线画每个投射者的不透明层
    // 最后把深度图像转成可采样布局，供主通道和调试叠加层读取
    //
    // 同时重算预通道写入、地形着色器采样时所用的太阳空间视图投影矩阵
    // 它每帧调用一次，且排在把矩阵拷进 UBO 的 updateUniform **之前**
    // 于是着色器投影用的矩阵与预通道渲染深度图用的矩阵来自同一帧，两者之间没有相机移动的滞后
    // 帧的最后一步是把场景图逐字节 copy 进交换链图像，因此只接受 8 位四通道的表面格式
    // ——它与场景图必须字节兼容，通道序也必须一致（见 sceneUnormFormat）
    [[nodiscard]] static bool isSupportedSurfaceFormat(VkFormat format) {
        return format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_B8G8R8A8_UNORM ||
               format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_R8G8B8A8_UNORM;
    }

    [[nodiscard]] VkSurfaceFormatKHR
    chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const {
        for (const auto& format : formats) {
            if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
                format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                return format;
            }
        }
        for (const auto& format : formats) {
            if (isSupportedSurfaceFormat(format.format) &&
                format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                return format;
            }
        }
        if (!formats.empty() && isSupportedSurfaceFormat(formats.front().format)) {
            return formats.front();
        }
        throw std::runtime_error(
            "No 8-bit RGBA/BGRA surface format: the GUI pass composites into a byte-compatible "
            "scene image and the frame is copied out verbatim");
    }

    [[nodiscard]] VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR>& modes,
                                                     bool vsync) const {
        // FIFO 是真正的垂直同步，呈现引擎等显示器刷新，帧率因此被钉在屏幕刷新率且不撕裂
        // 它不吃 CPU，代价是交换本身会阻塞
        // 不开它则走 MAILBOX，应用提交多快就呈现多快，中间的帧被丢掉，也就是"不限帧"那条路径
        // MAILBOX 并非所有设备都支持，因此无论如何 FIFO 都是兜底
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
                swapchainExtent.width, swapchainExtent.height, 1, sceneUnormFormat(),
                VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                renderSampleCount());
            target.view =
                createImageView(target.image.image, sceneUnormFormat(), VK_IMAGE_ASPECT_COLOR_BIT);
        }
    }

    void createDepthTargets() {
        depthFormat = chooseDepthFormat();
        depthTargets.resize(swapchainImages.size());
        for (auto& target : depthTargets) {
            // 深度附件在通道内被清空、写入，且从不回读
            // 标成瞬态之后，Apple 这类片上式 GPU 能把它留在片上内存里
            // 否则就是一块约 250 MB 的多重采样渲染目标分配
            // 渲染通道本来就用 loadOp CLEAR 加 storeOp DONT_CARE，正是 memoryless 的形态
            // 不支持该特性的驱动退化成普通分配，行为不变
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

    // 场景图：世界与 GUI 的共同画布，两个视图各自解释同一批字节
    // 格式取 UNORM 为基，SRGB 只是它的另一个视图；copy 到交换链图像是逐字节的，
    // 因此交换链是 UNORM 还是 SRGB 都不影响最终呈现
    void createSceneTargets() {
        sceneTargets.resize(swapchainImages.size());
        for (auto& target : sceneTargets) {
            target.image = createImage(swapchainExtent.width, swapchainExtent.height, 1,
                                       sceneUnormFormat(),
                                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
            target.view =
                createImageView(target.image.image, sceneUnormFormat(), VK_IMAGE_ASPECT_COLOR_BIT);
        }
    }

    void createGuiDepthTargets() {
        guiDepthTargets.resize(swapchainImages.size());
        for (auto& target : guiDepthTargets) {
            // 与世界的深度同理：通道内清空、写入、不回读，标成瞬态就能留在片上内存
            target.image =
                createImage(swapchainExtent.width, swapchainExtent.height, 1, depthFormat,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                            VK_SAMPLE_COUNT_1_BIT);
            VkImageAspectFlags aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
            if (depthFormatHasStencil(depthFormat)) {
                aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
            }
            target.view = createImageView(target.image.image, depthFormat, aspect);
        }
    }

    // GUI 那趟：单采样、载入世界那趟的结果、画完留给 copy
    // 颜色附件的格式是 UNORM，于是固定功能混合读写的是 sRGB 编码值——这正是 vanilla
    // 的合成空间，也是"提示框比原版更透""界面文字比原版亮一档"两个缺陷的收口
    void createGuiRenderPass() {
        VkAttachmentDescription color{};
        color.format = sceneUnormFormat();
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAttachmentDescription depth{};
        depth.format = depthFormat;
        depth.samples = VK_SAMPLE_COUNT_1_BIT;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        const std::array attachments{color, depth};
        VkAttachmentReference colorReference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depthReference{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorReference;
        subpass.pDepthStencilAttachment = &depthReference;
        // 世界那趟写完颜色，这趟才能载入它
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        // 这一趟画完立刻被 copy 读走。隐式的尾部依赖只到 BOTTOM_OF_PIPE、不带访问掩码，
        // 保证不了颜色写入对 transfer 读可见，所以显式写一条
        VkSubpassDependency presentDependency{};
        presentDependency.srcSubpass = 0;
        presentDependency.dstSubpass = VK_SUBPASS_EXTERNAL;
        presentDependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        presentDependency.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        presentDependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        presentDependency.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        const std::array dependencies{dependency, presentDependency};
        auto info = vkStructure<VkRenderPassCreateInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);
        info.attachmentCount = static_cast<std::uint32_t>(attachments.size());
        info.pAttachments = attachments.data();
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        info.dependencyCount = static_cast<std::uint32_t>(dependencies.size());
        info.pDependencies = dependencies.data();
        checkVk(vkCreateRenderPass(device, &info, nullptr, &worldPipelines_.guiRenderPass),
                "vkCreateRenderPass(gui)");
    }

    void createRenderPass() {
        VkAttachmentDescription color{};
        color.format = sceneUnormFormat();
        color.samples = renderSampleCount();
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = renderSampleCount() == VK_SAMPLE_COUNT_1_BIT
                            ? VK_ATTACHMENT_STORE_OP_STORE
                            : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        // 注意：解析过的 MSAA 颜色附件在这里保持 COLOR_ATTACHMENT_OPTIMAL
        // 填 UNDEFINED 会被校验层拒绝，何况当前这版 MoltenVK 本来也不把瞬态附件放到片上内存
        // 世界这趟的结果不再直接呈现：GUI 那趟要按 UNORM 视图把它载入并继续画
        color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
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
        resolve.format = sceneUnormFormat();
        resolve.samples = VK_SAMPLE_COUNT_1_BIT;
        resolve.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        resolve.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        resolve.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        resolve.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        resolve.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        resolve.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
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
        checkVk(vkCreateRenderPass(device, &info, nullptr, &worldPipelines_.renderPass), "vkCreateRenderPass");
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
        // PackedVoxelVertex 有六个按 4 字节对齐的整型属性
        // location 1 里那个填充字节携带片元的群系掩码，决定用哪张群系配色查找表
        // location 5 是逐顶点 RGB 着色，掩码选中字面着色路径时由片元乘上去
        // 红石粉按信号强度算出的红色走的就是这条路径
        const std::array<VkVertexInputAttributeDescription, 6> attributes{{
            {0, 0, VK_FORMAT_R16G16_UINT, offsetof(VoxelVertex, positionX)},
            {1, 0, VK_FORMAT_R16G16_UINT, offsetof(VoxelVertex, positionZ)},
            {2, 0, VK_FORMAT_R16G16_UINT, offsetof(VoxelVertex, uvX)},
            {3, 0, VK_FORMAT_R32_UINT, offsetof(VoxelVertex, textureLayer)},
            {4, 0, VK_FORMAT_R8G8B8A8_UINT, offsetof(VoxelVertex, skyLight)},
            {5, 0, VK_FORMAT_R8G8B8A8_UINT, offsetof(VoxelVertex, tintR)},
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
        // 地形网格存的是相对 section 原点的坐标，原点逐次绘制推送，顶点着色器据此还原世界坐标
        // 天空通道共用这套布局，只是忽略那段推送范围
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
        checkVk(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &worldPipelines_.pipelineLayout),
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
        pipelineInfo.layout = worldPipelines_.pipelineLayout;
        pipelineInfo.renderPass = worldPipelines_.renderPass;
        const auto result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                                      nullptr, &worldPipelines_.graphicsPipeline);
        checkVk(result, "vkCreateGraphicsPipelines");

        depthStencil.depthWriteEnable = VK_FALSE;
        // 半透明通道是双面的，不做背面剔除
        // 染色玻璃、冰、水这类可透视方块必须显示远端的内壁，尽管那些面背对相机
        // 透过近处的半透明面看过去，理应看到后墙的颜色
        // 沿用不透明通道的 VK_CULL_MODE_BACK_BIT 会把远端面剔掉，那里就直接看穿了
        // 普通玻璃掩盖了这个问题，因为它的贴图几乎全是 alpha 0，本来就没什么可看
        // 但染色玻璃是实心填充，破洞一目了然
        // 不透明与 cutout 通道保留背面剔除，cutout 的十字模型自己就输出了正反两种绕序
        // 因此这里的 NONE 只作用于半透明管线，建完立刻恢复
        rasterization.cullMode = VK_CULL_MODE_NONE;
        colorAttachment.blendEnable = VK_TRUE;
        colorAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        colorAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        colorAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        colorAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        const auto translucentResult = vkCreateGraphicsPipelines(
            device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &worldPipelines_.translucentPipeline);
        checkVk(translucentResult, "vkCreateGraphicsPipelines(translucent)");
        rasterization.cullMode = VK_CULL_MODE_BACK_BIT; // restore for the cutout pipeline
        depthStencil.depthWriteEnable = VK_TRUE;
        colorAttachment.blendEnable = VK_FALSE;

        const auto cutoutFragmentCode = readSpirv(shaderRoot / "block_cutout.frag.spv");
        const auto cutoutFragmentModule = createShaderModule(cutoutFragmentCode);
        auto cutoutFragmentStage = fragmentStage;
        cutoutFragmentStage.module = cutoutFragmentModule;
        const std::array cutoutStages{vertexStage, cutoutFragmentStage};
        // Java 里带 mipmap 的 cutout 树叶用的是普通背面剔除
        // 植物自带反向绕序的三角形，因此保持双面
        rasterization.cullMode = VK_CULL_MODE_BACK_BIT;
        // 草方块侧面的 overlay 与它下面的不透明底面**完全共面**（BM-1 的第二个
        // element）。共面的后画者必须能通过深度测试，所以 cutout 用
        // LESS_OR_EQUAL 而不是 LESS。
        // 别改回 LESS，也别退回"沿法线推出去一点"：可解析的深度差随距离按 d^2
        // 衰减，任何固定的世界空间偏移在远处都会掉到一个 ULP 以下，于是草边在
        // 远景里随镜头移动闪回泥土贴图烘死的那抹绿。共面 + LESS_OR_EQUAL 与距离无关。
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        pipelineInfo.pStages = cutoutStages.data();
        const auto cutoutResult = vkCreateGraphicsPipelines(
            device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &worldPipelines_.cutoutPipeline);
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS; // 恢复给后面的管线
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
        checkVk(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &worldPipelines_.outlinePipelineLayout),
                "vkCreatePipelineLayout(outline)");
        pipelineInfo.pStages = outlineStages.data();
        pipelineInfo.pVertexInputState = &outlineVertexInput;
        pipelineInfo.layout = worldPipelines_.outlinePipelineLayout;
        const auto outlineResult = vkCreateGraphicsPipelines(
            device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &worldPipelines_.outlinePipeline);
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
        pipelineInfo.layout = worldPipelines_.pipelineLayout;
        const auto skyResult = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                                         nullptr, &worldPipelines_.skyPipeline);
        vkDestroyShaderModule(device, skyFragmentModule, nullptr);
        vkDestroyShaderModule(device, skyVertexModule, nullptr);
        checkVk(skyResult, "vkCreateGraphicsPipelines(sky)");

        // 从这里往下的 hud / 全景 / 准星 / 暗角 / 手持物都画在 **GUI 那趟**：
        // 单采样（vanilla 的界面本来也不做 MSAA），且颜色附件是 UNORM 视图，
        // 于是这些着色器输出的是 sRGB 编码值、混合也发生在编码值上
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        pipelineInfo.renderPass = worldPipelines_.guiRenderPass;
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

        // RN-14：方块物品图标专用的一条，和上面那条共用着色器与布局，只把深度测试与
        // 写入打开。物品模型是一组盒子，墙的中柱与横臂互相穿插，任何「整盒从后往前画」
        // 的排序都合成不对——它需要真正的深度缓冲。GUI 通道本来就带深度附件且每帧清空，
        // 界面里其它元素一律不做深度测试，所以这条只影响图标自己。
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
        const auto hudIconResult = vkCreateGraphicsPipelines(
            device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &hudBlockIconPipeline);
        checkVk(hudIconResult, "vkCreateGraphicsPipelines(hudBlockIcon)");
        depthStencil.depthTestEnable = VK_FALSE;
        depthStencil.depthWriteEnable = VK_FALSE;

        // 标题全景立方体用一个全屏三角形，片元着色器对六张全景面做光线步进
        // 它只需要共享描述符集里的全景采样器，加上偏航、俯仰、视场角这组推送常量
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

        // 下面的准星与暗角管线复用同一份 pipelineInfo，所以要把 HUD 的布局和着色器阶段数组放回去
        // 全景那段把它们换成了自己 16 字节推送常量的布局和一个已经离开作用域的局部数组
        pipelineInfo.layout = hudPipelineLayout;
        pipelineInfo.pStages = hudStages.data();

        // 26.1 用反色混合绘制 15x15 的准星贴图，明暗地形上都看得见
        // 这里特意为它单开一条混合状态不同的管线
        colorAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        colorAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        colorAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        colorAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        const auto crosshairResult = vkCreateGraphicsPipelines(
            device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &crosshairPipeline);
        checkVk(crosshairResult, "vkCreateGraphicsPipelines(crosshair)");

        // vanilla 的 HUD 用乘性混合绘制暗角贴图，四角压暗画面而中心不受影响
        // 这同样是一条专用的混合状态管线
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

        // 掉落物与下落方块回到世界那趟
        multisampling.rasterizationSamples = renderSampleCount();
        pipelineInfo.renderPass = worldPipelines_.renderPass;
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
        checkVk(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &worldPipelines_.itemPipelineLayout),
                "vkCreatePipelineLayout(item)");
        pipelineInfo.pStages = itemStages.data();
        pipelineInfo.layout = worldPipelines_.itemPipelineLayout;
        const auto itemResult = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                                          nullptr, &worldPipelines_.itemPipeline);
        checkVk(itemResult, "vkCreateGraphicsPipelines(item)");
        depthStencil.depthWriteEnable = VK_FALSE;
        const auto itemShadowResult = vkCreateGraphicsPipelines(
            device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &worldPipelines_.itemShadowPipeline);
        checkVk(itemShadowResult, "vkCreateGraphicsPipelines(item shadow)");
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
        // 第一人称手持物与背包里的玩家预览画在 GUI 那趟（单采样、自己的深度），
        // 但用的是与世界里同一份着色器——整帧都在编码值上工作，不再需要两个变体
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        pipelineInfo.renderPass = worldPipelines_.guiRenderPass;
        const auto heldItemResult = vkCreateGraphicsPipelines(
            device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &worldPipelines_.heldItemPipeline);
        multisampling.rasterizationSamples = renderSampleCount();
        pipelineInfo.renderPass = worldPipelines_.renderPass;
        checkVk(heldItemResult, "vkCreateGraphicsPipelines(held item)");
        vkDestroyShaderModule(device, itemFragmentModule, nullptr);
        vkDestroyShaderModule(device, itemVertexModule, nullptr);
    }

    // 设备级的遮挡查询资源
    // 含查询池、携带逐次绘制包围盒推送常量的管线布局
    // 还有查询通道为每个 section 绘制的单位立方体顶点与索引缓冲
    void createOcclusionQueryResources() {
        if (occlusion_.disabled) {
            return;
        }
        auto poolInfo =
            vkStructure<VkQueryPoolCreateInfo>(VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);
        poolInfo.queryType = VK_QUERY_TYPE_OCCLUSION;
        poolInfo.queryCount = static_cast<std::uint32_t>(kOcclusionQueryPoolSize);
        for (auto& queryPool : occlusion_.queryPools) {
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
        checkVk(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &occlusion_.queryLayout),
                "vkCreatePipelineLayout(occlusion query)");

        // 一个位于 [0,1]³ 的单位立方体
        // 查询顶点着色器用逐次绘制的推送常量把它撑成各 section 的包围盒
        // 剔除关闭且绕序无关，因此从任何视点都能测试这个盒子
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
        occlusion_.boxVertexBuffer =
            createBuffer(sizeof(kUnitCubeCorners), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
        std::memcpy(occlusion_.boxVertexBuffer.mapped, kUnitCubeCorners.data(),
                    sizeof(kUnitCubeCorners));
        checkVk(
            vmaFlushAllocation(allocator, occlusion_.boxVertexBuffer.allocation, 0, VK_WHOLE_SIZE),
            "vmaFlushAllocation(occlusion box vertices)");
        occlusion_.boxIndexBuffer =
            createBuffer(sizeof(kUnitCubeIndices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
        std::memcpy(occlusion_.boxIndexBuffer.mapped, kUnitCubeIndices.data(),
                    sizeof(kUnitCubeIndices));
        checkVk(vmaFlushAllocation(allocator, occlusion_.boxIndexBuffer.allocation, 0, VK_WHOLE_SIZE),
                "vmaFlushAllocation(occlusion box indices)");
    }

    // 与交换链耦合，查询管线绑定当前渲染通道，交换链一变就和其它管线一起重建
    void createOcclusionQueryPipeline() {
        if (occlusion_.queryPools.front() == VK_NULL_HANDLE) {
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
        pipelineInfo.layout = occlusion_.queryLayout;
        pipelineInfo.renderPass = worldPipelines_.renderPass;
        const auto result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                                      nullptr, &occlusion_.queryPipeline);
        checkVk(result, "vkCreateGraphicsPipelines(occlusion query)");
        vkDestroyShaderModule(device, vertexModule, nullptr);
        vkDestroyShaderModule(device, fragmentModule, nullptr);
    }

    // 场景图 → 交换链图像。两者同属 B8G8R8A8 的尺寸类，vkCmdCopyImage 因此是逐字节
    // 搬运、不做任何色彩转换——场景图里存的已经是最终要呈现的 sRGB 编码值。
    // GUI 那趟的 finalLayout 已经把场景图交在 TRANSFER_SRC_OPTIMAL 上，这里只需要
    // 把交换链图像从 UNDEFINED 带到 TRANSFER_DST，再带到 PRESENT_SRC。
    void copySceneToSwapchain(VkCommandBuffer commandBuffer, std::uint32_t imageIndex) const {
        auto barrier = vkStructure<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = swapchainImages[imageIndex];
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &barrier);
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = region.srcSubresource;
        region.extent = {swapchainExtent.width, swapchainExtent.height, 1};
        vkCmdCopyImage(commandBuffer, sceneTargets[imageIndex].image.image,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapchainImages[imageIndex],
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = 0;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &barrier);
    }

    void createGuiFramebuffers() {
        guiFramebuffers.resize(sceneTargets.size());
        for (std::size_t index = 0; index < guiFramebuffers.size(); ++index) {
            const std::array attachments{sceneTargets[index].view,
                                         guiDepthTargets[index].view};
            auto info =
                vkStructure<VkFramebufferCreateInfo>(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
            info.renderPass = worldPipelines_.guiRenderPass;
            info.attachmentCount = static_cast<std::uint32_t>(attachments.size());
            info.pAttachments = attachments.data();
            info.width = swapchainExtent.width;
            info.height = swapchainExtent.height;
            info.layers = 1;
            checkVk(vkCreateFramebuffer(device, &info, nullptr, &guiFramebuffers[index]),
                    "vkCreateFramebuffer(gui)");
        }
    }

    void createFramebuffers() {
        framebuffers.resize(sceneTargets.size());
        for (std::size_t index = 0; index < framebuffers.size(); ++index) {
            std::array<VkImageView, 3> attachments{};
            std::uint32_t attachmentCount = 2U;
            if (renderSampleCount() == VK_SAMPLE_COUNT_1_BIT) {
                attachments[0] = sceneTargets[index].view;
                attachments[1] = depthTargets[index].view;
            } else {
                attachments[0] = colorTargets[index].view;
                attachments[1] = depthTargets[index].view;
                attachments[2] = sceneTargets[index].view;
                attachmentCount = 3U;
            }
            auto info =
                vkStructure<VkFramebufferCreateInfo>(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
            info.renderPass = worldPipelines_.renderPass;
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
        createColorTargets();
        createDepthTargets();
        createSceneTargets();
        createGuiDepthTargets();
        createRenderPass();
        createGuiRenderPass();
        createGraphicsPipeline();
        createOcclusionQueryPipeline();
        createParticlePipeline();
        createShadowDebugPipeline();
        createRainSheetPipeline();
        createFramebuffers();
        createGuiFramebuffers();
    }

    void cleanupSwapchain() noexcept {
        for (const auto framebuffer : guiFramebuffers) {
            vkDestroyFramebuffer(device, framebuffer, nullptr);
        }
        guiFramebuffers.clear();
        for (auto& target : sceneTargets) {
            if (target.view != VK_NULL_HANDLE) {
                vkDestroyImageView(device, target.view, nullptr);
            }
            if (allocator != VK_NULL_HANDLE) {
                destroyImage(target.image);
            }
        }
        sceneTargets.clear();
        for (auto& target : guiDepthTargets) {
            if (target.view != VK_NULL_HANDLE) {
                vkDestroyImageView(device, target.view, nullptr);
            }
            if (allocator != VK_NULL_HANDLE) {
                destroyImage(target.image);
            }
        }
        guiDepthTargets.clear();
        if (worldPipelines_.guiRenderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device, worldPipelines_.guiRenderPass, nullptr);
            worldPipelines_.guiRenderPass = VK_NULL_HANDLE;
        }
        for (const auto framebuffer : framebuffers) {
            vkDestroyFramebuffer(device, framebuffer, nullptr);
        }
        framebuffers.clear();
        if (worldPipelines_.graphicsPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, worldPipelines_.graphicsPipeline, nullptr);
            worldPipelines_.graphicsPipeline = VK_NULL_HANDLE;
        }
        if (worldPipelines_.translucentPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, worldPipelines_.translucentPipeline, nullptr);
            worldPipelines_.translucentPipeline = VK_NULL_HANDLE;
        }
        if (worldPipelines_.cutoutPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, worldPipelines_.cutoutPipeline, nullptr);
            worldPipelines_.cutoutPipeline = VK_NULL_HANDLE;
        }
        if (worldPipelines_.skyPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, worldPipelines_.skyPipeline, nullptr);
            worldPipelines_.skyPipeline = VK_NULL_HANDLE;
        }
        if (worldPipelines_.outlinePipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, worldPipelines_.outlinePipeline, nullptr);
            worldPipelines_.outlinePipeline = VK_NULL_HANDLE;
        }
        if (occlusion_.queryPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, occlusion_.queryPipeline, nullptr);
            occlusion_.queryPipeline = VK_NULL_HANDLE;
        }
        if (crosshairPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, crosshairPipeline, nullptr);
            crosshairPipeline = VK_NULL_HANDLE;
        }
        if (vignettePipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, vignettePipeline, nullptr);
            vignettePipeline = VK_NULL_HANDLE;
        }
        if (hudBlockIconPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, hudBlockIconPipeline, nullptr);
            hudBlockIconPipeline = VK_NULL_HANDLE;
        }
        if (hudPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, hudPipeline, nullptr);
            hudPipeline = VK_NULL_HANDLE;
        }
        if (panoramaPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, panoramaPipeline, nullptr);
            panoramaPipeline = VK_NULL_HANDLE;
        }
        if (worldPipelines_.itemPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, worldPipelines_.itemPipeline, nullptr);
            worldPipelines_.itemPipeline = VK_NULL_HANDLE;
        }
        if (worldPipelines_.itemShadowPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, worldPipelines_.itemShadowPipeline, nullptr);
            worldPipelines_.itemShadowPipeline = VK_NULL_HANDLE;
        }
        if (worldPipelines_.heldItemPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, worldPipelines_.heldItemPipeline, nullptr);
            worldPipelines_.heldItemPipeline = VK_NULL_HANDLE;
        }
        if (worldPipelines_.particlePipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, worldPipelines_.particlePipeline, nullptr);
            worldPipelines_.particlePipeline = VK_NULL_HANDLE;
        }
        if (worldPipelines_.particlePipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, worldPipelines_.particlePipelineLayout, nullptr);
            worldPipelines_.particlePipelineLayout = VK_NULL_HANDLE;
        }
        if (worldPipelines_.shadowDebugPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, worldPipelines_.shadowDebugPipeline, nullptr);
            worldPipelines_.shadowDebugPipeline = VK_NULL_HANDLE;
        }
        if (worldPipelines_.rainSheetPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, worldPipelines_.rainSheetPipeline, nullptr);
            worldPipelines_.rainSheetPipeline = VK_NULL_HANDLE;
        }
        if (worldPipelines_.rainSheetPipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, worldPipelines_.rainSheetPipelineLayout, nullptr);
            worldPipelines_.rainSheetPipelineLayout = VK_NULL_HANDLE;
        }
        if (worldPipelines_.pipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, worldPipelines_.pipelineLayout, nullptr);
            worldPipelines_.pipelineLayout = VK_NULL_HANDLE;
        }
        if (worldPipelines_.outlinePipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, worldPipelines_.outlinePipelineLayout, nullptr);
            worldPipelines_.outlinePipelineLayout = VK_NULL_HANDLE;
        }
        if (hudPipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, hudPipelineLayout, nullptr);
            hudPipelineLayout = VK_NULL_HANDLE;
        }
        if (panoramaPipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, panoramaPipelineLayout, nullptr);
            panoramaPipelineLayout = VK_NULL_HANDLE;
        }
        if (worldPipelines_.itemPipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, worldPipelines_.itemPipelineLayout, nullptr);
            worldPipelines_.itemPipelineLayout = VK_NULL_HANDLE;
        }
        if (worldPipelines_.renderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device, worldPipelines_.renderPass, nullptr);
            worldPipelines_.renderPass = VK_NULL_HANDLE;
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

    // 该点是否位于它所在那一格的水面之下
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

    // 场景实际渲染所用的眼点
    // 相机对象始终位于玩家眼睛处，第三人称沿视线把渲染眼点往后拉，或推到前方回看
    // 视图矩阵与剔除视锥都由它推导，两者因此永远一致
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
        // 相机与玩家之间有实心方块时把相机拉近，它因此不会穿墙
        // 留一点余量让它别贴在面上
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

    // 场景的视图矩阵，尚未叠加视角摇晃
    // uniform 与剔除视锥都用它，地形因此是按真正渲染出来的那个视角剔除的
    // 这在第三人称下尤其关键，前视视角的朝向甚至与第一人称相机相反
    [[nodiscard]] glm::mat4 renderViewMatrix() const {
        const RenderEye eye = renderEyeState();
        return glm::lookAt(eye.position, eye.position + eye.forward, glm::vec3{0.0F, 1.0F, 0.0F});
    }

    void updateUniform(FrameContext& frame) const {
        CameraUniform uniform;
        uniform.model = glm::mat4{1.0F};
        const RenderEye renderEye = renderEyeState();
        // 视角摇晃在所有视角下都作用于整个场景，与 vanilla 第三人称的相机抖动一致
        const glm::mat4 baseView =
            glm::lookAt(renderEye.position, renderEye.position + renderEye.forward,
                        glm::vec3{0.0F, 1.0F, 0.0F});
        uniform.view = viewBobbingMatrix() * baseView;
        uniform.projection = camera.projectionMatrix(static_cast<float>(swapchainExtent.width) /
                                                         static_cast<float>(swapchainExtent.height),
                                                     cameraFarPlane());
        uniform.cameraPosition = glm::vec4{renderEye.position, 1.0F};
        // 日月读主世界时钟而不是帧计时器
        // 天空随世界 tick 推进，时钟一停它就停，比如关掉昼夜规则或游戏暂停，不会跟着真实帧漂移
        // 20 Hz 的 tick 只对快速运动的东西才显得粗，太阳每 tick 几乎不动，这里不需要 tick 内插值
        const auto dayTick = clientMirror_.world().dayTimeTicks;
        const auto daylight = world::DayNightCycle::stateAtTick(dayTick);
        // sunDirection.w is the lightmap's SkyFactor. 26.1 drives it from the
        // SKY_LIGHT_FACTOR track (1.0 through the day, 0.24 at night), not from
        // the sun-elevation cosine `skyBrightness` — that curve is for sky and
        // fog COLOUR, and using it here left every surface reading a fractional
        // sky factor for most of the day.
        uniform.sunDirection =
            glm::vec4{daylight.sunDirection, SkyLight::skyLightFactor(dayTick)};
        // horizonFog.w 只驱动月相
        // 流体动画改用下面的服务端 tick，关掉昼夜规则因此不再冻住水和岩浆
        // 月相时钟取模回绕，长时间运行的世界也不会损失浮点精度
        constexpr double kLunarCycleSeconds = 8.0 * world::DayNightCycle::kSecondsPerDay;
        const double dayTimeSeconds = dayTick / world::DayNightCycle::kTicksPerSecond;
        uniform.horizonFog =
            glm::vec4{daylight.horizonColor,
                      static_cast<float>(std::fmod(dayTimeSeconds, kLunarCycleSeconds))};
        // renderSettings.z 是水下 EXP2 雾的浓度，vanilla 取 0.05
        // 这里略浓，取 0.08，在去掉旧的硬距离截断之后重新还原那种浑浊感
        // 约 22 格处几乎已是满雾
        uniform.renderSettings =
            glm::vec4{renderDistanceBlocks(), cameraSubmergedInWater() ? 1.0F : 0.0F, 0.08F, 24.0F};
        // 天空着色器从图集里取太阳贴图和月相瓦片
        // 层号来自推导出来的特殊区起始值而不是写死的数字
        // 图集一变，它们也不会悄悄指到某张方块纹理上
        uniform.celestialLayers = glm::vec4{kSunLayer, kMoonPhaseFirstLayer, 0.0F, 0.0F};
        // 天空的天气读数取自逐 tick 快照，复现实时系统按插值系数取降雨强度时的帧插值
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
        // 把烘好的方块纹理动画转给着色器，它从各自的首层开始轮播
        // 目前是岩浆块，将来还有海晶石与海晶灯，条数上限为 kMaxBlockAnimations
        const auto& bakedAnimations = textures_.blockAnimations;
        const std::size_t animationCount =
            std::min(bakedAnimations.size(), kMaxBlockAnimations);
        uniform.blockAnimationSettings.x = static_cast<float>(animationCount);
        for (std::size_t i = 0; i < animationCount; ++i) {
            uniform.blockAnimations[i] = glm::vec4{bakedAnimations[i].baseLayer,
                                                   static_cast<float>(bakedAnimations[i].frameCount),
                                                   bakedAnimations[i].frameTime, 0.0F};
        }
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
        // lightingSettings.w 是太阳阴影开关，本帧预通道跑过时为 1.0
        // 地形着色器因此只在阴影图确实有效时才采样它
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

    // 26.1 的语言管理器只从 pack.mcmeta 构建这份目录
    // 无论资源包里有多少种语言，这里都不会打开任何翻译 JSON
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

    // 粒子效果等级到密度倍率的换算，低档 0.5、中档 1.0、高档 2.0、疯狂档 3.0
    // 生成数量与存活上限都乘这个系数，见 ParticleSystem::setLevelScale
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

    // 整只实体只采样一次世界光照，做法与 vanilla 一致
    // 在实体躯干所占的那格取一个天光等级和一个方块光等级，不逐骨骼也不逐片元采样
    // 天光与方块光这一对被打包进一个 float，因为 ItemPush 已经正好用满 Vulkan 保证的 128 字节
    // 长方体各模式里只剩 dimensions.w 这一个空位
    // 0 保留给"没有场景光照"，因此编码整体偏移一，由 item_entity.vert 解回
    // `samplePoint` 是要读取其方块的那个点，调用方据此说明自己实体的躯干到底在哪
    // 贴地的实体必须往上抬半格采样，否则脚下的采样会取整落进实心方块，读出来一片漆黑
    // 启动模拟线程，除非 MC_REBEDROCK_SYNC_TICK 要求走同步循环
    // tick 与世界锁的纪律归它管，驱动器、世界锁和激活开关都在运行时里
    void startSimulationThread() {
        runtime.startSimulation();
    }

    // Returns the swapchain image index the frame was drawn into, so a caller
    // that wants the pixels knows which scene target holds them (RN-15d). A
    // nullopt means the swapchain was recreated instead of a frame being drawn.
    std::optional<std::uint32_t> drawFrame() {
        // 这里不持有世界锁
        // 绘制通道采样的东西都是无锁的：渲染侧自有的客户端缓存、原子发布的快照、GPU 网格状态
        // 而下面的围栏等待、提交和呈现绝不能阻塞模拟线程的写区间
        auto& frame = frames[currentFrame];
        const auto fenceWaitStart = std::chrono::steady_clock::now();
        checkVk(vkWaitForFences(device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX),
                "vkWaitForFences");
        if (diag::traceEnabled()) {
            diag::frameTrace().fenceWaitMs += diag::msSince(fenceWaitStart);
        }
        // 告诉 VMA 当前是第几帧，它才能复用一个帧窗口之前释放的分配
        // 否则每来一波突发就得新开内存块
        vmaSetCurrentFrameIndex(allocator, frameNumber_);
        const auto occReadStart = std::chrono::steady_clock::now();
        world_.releaseFrameResources(frame);
        world_.readBackOcclusionQueries();
        if (diag::traceEnabled()) {
            diag::frameTrace().occlusionReadbackMs += diag::msSince(occReadStart);
        }
        std::uint32_t imageIndex = 0;
        const auto acquireStart = std::chrono::steady_clock::now();
        const auto acquire = vkAcquireNextImageKHR(
            device, swapchain, UINT64_MAX, frame.imageAvailable, VK_NULL_HANDLE, &imageIndex);
        if (diag::traceEnabled()) {
            diag::frameTrace().acquireMs += diag::msSince(acquireStart);
        }
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
            recreateSwapchain();
            return std::nullopt;
        }
        if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
            checkVk(acquire, "vkAcquireNextImageKHR");
        }
        const auto imageWaitStart = std::chrono::steady_clock::now();
        if (imagesInFlight[imageIndex] != VK_NULL_HANDLE) {
            checkVk(vkWaitForFences(device, 1, &imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX),
                    "vkWaitForFences(swapchain image)");
        }
        if (diag::traceEnabled()) {
            diag::frameTrace().imageWaitMs += diag::msSince(imageWaitStart);
        }
        imagesInFlight[imageIndex] = frame.inFlight;
        {
            const auto uploadStart = std::chrono::steady_clock::now();
            world_.prepareStreamingUpdates(frame);
            if (diag::traceEnabled()) {
                diag::frameTrace().uploadMs += diag::msSince(uploadStart);
            }
        }
        if (worldSessionActive && !worldReady && completedStreamBatchCount > 0U &&
            spawnPositionInitialized && pendingSectionUpdates.empty()) {
            worldReady = true;
            paused = false;
            menuSystem.pageStack.reset(ui::PageId::Game);
            // 加载完成只负责启动模拟，绝不能拿渲染状态再传送一次玩家
            // loadWorld 已经把存档坐标恢复进实时控制器、物理端点和已发布快照
            // 这里再"重新锚定"会用陈旧的渲染状态盖掉权威位置
            simulationActive.store(true, std::memory_order_release);
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            std::cout << "Terrain loading complete\n";
            // 出生点区域已就位，把半径放宽到完整渲染距离
            // 逐帧的流送循环会持续请求，其余视距在游玩过程中逐步填满
            // 这与 vanilla 进入世界后继续流送初始区域之外的区块是同一做法
            chunkStreamer.setRadii(viewDistanceChunks,
                                   viewDistanceChunks + world::kUnloadHysteresisChunks);
        }
        const auto uniformStart = std::chrono::steady_clock::now();
        world_.updateShadowMatrix();
        updateUniform(frame);
        if (diag::traceEnabled()) {
            diag::frameTrace().uniformMs += diag::msSince(uniformStart);
        }
        checkVk(vkResetFences(device, 1, &frame.inFlight), "vkResetFences");
        checkVk(vkResetCommandBuffer(frame.commandBuffer, 0), "vkResetCommandBuffer");
        const auto recordStart = std::chrono::steady_clock::now();
        const std::size_t visibleCount = world_.recordCommandBuffer(frame, imageIndex);
        if (diag::traceEnabled()) {
            diag::frameTrace().recordMs += diag::msSince(recordStart);
            diag::frameTrace().visibleSections = static_cast<std::uint32_t>(visibleCount);
        }
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

        // 交换链图像现在最早是被 copy 写（GUI 那趟画在场景图上），因此等待阶段要含 TRANSFER
        const VkPipelineStageFlags waitStage =
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
        const auto presentSemaphore = presentSemaphores[imageIndex];
        auto submit = vkStructure<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &frame.imageAvailable;
        submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &frame.commandBuffer;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &presentSemaphore;
        const auto presentStart = std::chrono::steady_clock::now();
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
        if (diag::traceEnabled()) {
            diag::frameTrace().presentMs += diag::msSince(presentStart);
        }
        currentFrame = (currentFrame + 1U) % kFramesInFlight;
        ++frameNumber_;
        return imageIndex;
    }

    std::filesystem::path shaderRoot;
    const assets::ResourceProvider* resourceProvider = nullptr;
    ui::AsyncLanguageLoader languageLoader;
    std::chrono::steady_clock::time_point languageLoadStarted{};
    std::filesystem::path optionsPath;
    // 权威运行时拥有世界、存档仓库、游戏会话、模拟驱动器和世界锁
    // 下面这些引用只是便利别名，让众多调用点仍以熟悉的名字读它们
    // 对象本身住在运行时里，而运行时正是专用服务器所链接的那部分
    // 声明顺序有意义：先构造运行时，别名再指进去
    runtime::GameRuntime runtime;
    persistence::SaveRepository& saveRepository;
    world::ChunkStreamer& chunkStreamer;
    world::World& interactionWorld;
    // 渲染器做网格化和采样所用的客户端区块缓存
    // 它是表现侧自己拥有的一个独立世界，数据来源与写服务端世界的流送批次和模拟编辑相同
    // 模拟持续 tick 的是 interactionWorld，渲染器只读这份缓存，两边各自拥有自己的区块数据
    world::World clientCache;
    // 客户端渲染镜像，每帧靠泵送回环通道填充
    // 玩家、世界与实体的表现都读这里解码出来的视图，不去碰权威会话
    client::ClientMirror clientMirror_;
    gameplay::GameSession& gameSession;
    gameplay::SimulationDriver& simulationDriver;
    std::atomic_bool& simulationActive;
    world::WorldLock& worldLock;
    std::optional<persistence::SaveGame>& currentSave;
    std::uint64_t& worldEpoch;
    config::GameOptions options;
    std::optional<TestSceneOptions> testScene;
    // The state initializeTestScene actually placed, after `--stage` had its
    // say. The exporter sizes the camera off this rather than off
    // testScene->state, so the picture and the box around it are the same block.
    world::BlockState previewState_{world::Block::Stone};
    audio::AudioSystem audioSystem;
    // 在渲染线程上复刻工作线程的增量光照，即时编辑预览因此用的是正确光照而不是陈旧的存值
    world::WorldLightEngine interactionLightEngine;
    std::unordered_map<world::SectionPosition, GpuMesh, world::SectionPositionHash> gpuMeshes;
    StreamBufferPool deviceBufferPool_;
    StreamBufferPool stagingBufferPool_;
    render::SectionDeliveryQueue<world::SectionPosition, world::SectionPositionHash>
        pendingSectionOrder;
    std::unordered_map<world::SectionPosition, world::SectionMeshUpdate, world::SectionPositionHash>
        pendingSectionUpdates;
    std::unordered_map<world::SectionPosition, std::uint64_t, world::SectionPositionHash>
        latestSectionRevisions;
    // 前者是 GPU 上的网格当初烘焙时用的平滑光照画质，后者是工作线程正在重烘的目标画质
    // uniform.lightingSettings.z 跟随 currentMeshQuality
    // 着色器因此绝不会把 High 的 AO 曲线用在 Standard 网格上
    // 这次切换要等 qualityRemeshPending 排空才放行
    world::SmoothLightingQuality currentMeshQuality = world::SmoothLightingQuality::Standard;
    world::SmoothLightingQuality targetMeshQuality = world::SmoothLightingQuality::Standard;
    std::unordered_set<world::SectionPosition, world::SectionPositionHash> qualityRemeshPending;
    ui::MenuSystem menuSystem;
    // I-3: the left slot as of the last frame, so the rename box knows when to
    // reseed itself (AnvilScreen#slotChanged's trigger).
    gameplay::ItemStack previousAnvilLeft_{};
    // 面向 HUD 的玩法状态，每帧捕获一次
    // 各绘制通道因此读的是一份自洽的快照，而不是实时玩法对象
    mutable ui::UiFrameData uiFrameData_;
    PerspectiveCamera camera;
    // 相机构造时的原始视场角
    // 每帧都要乘上玩家的移动视场系数，因此基准值必须单独留一份
    float baseFieldOfViewDegrees = 65.0F;
    // 生命、饥饿与环境伤害，只在生存模式下 tick
    // 世界的游戏规则，所有权在这里，并镜像给消费它们的各系统
    // 以稀疏的自描述块形式持久化在 world.dat 里
    // 自由活动的生物，以及它们渲染所用的骨架
    // box-UV 实体管线已绑定的物种，每个已加载生物一条
    // 每条携带它的几何、动画，以及它在实体纹理数组里的层号
    // 由 createEntityTextureArray 填充
    std::vector<gameplay::entities::SpeciesRenderModel> speciesModels;
    animation::ModelAnimationSystem heldItemAnimation;
    // 背包预览里的玩家与世界中第三人称的玩家各用一个独立的动画器实例
    // 前者由光标驱动，后者由玩家自己的视线与移动驱动
    animation::PlayerModelAnimator playerModelAnimator;
    // 世界中第三人称的玩家跑的就是这套 PlayerModelAnimator 控制器栈，与背包预览共用
    // 输入是权威的行走动画状态，不再另算一份姿态
    animation::PlayerModelAnimator worldPlayerAnimator;
    CameraPerspective cameraPerspective = CameraPerspective::FirstPerson;
    // 第三人称的身体偏航，它滞后于视线方向
    // 头先转，转到限度才拖着身体一起转
    float worldBodyYaw = 0.0F;
    bool worldBodyYawInitialized = false;
    ParticleSystem particleSystem;
    // RN-9d：客户端环境 tick。vanilla 的 ClientLevel.animateTick——方块自己冒的
    // 粒子（附魔台的银河字母，后续的火把火焰/岩浆冒泡）全部由它派发，
    // 与服务端 ParticleEvent 那条来路互不相干
    render::BlockAnimateTicker blockAnimateTicker;
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
    // 复现用钩子，MC_REBEDROCK_LOAD_SAVE 会自动载入第一个真实存档
    bool loadSaveStarted = false;
    bool creativeScrollbarDragging = false;
    // 快速合成拖拽的状态：按住的是哪个键，以及光标划过的每个槽位的类型与下标
    // 全都是值，绝不是指向玩法侧存储的指针
    // 按下之后光标上还留着物品堆就算拖拽开始，按住期间持续收集槽位，松开时把整个集合交给交互
    bool inventoryDragActive = false;
    gameplay::InventoryMouseButton inventoryDragButton = gameplay::InventoryMouseButton::Left;
    std::vector<gameplay::SlotRef> inventoryDragSlots;
    // vanilla 在"按下时就已经生效"的操作上置这个标志，比如空光标下的拿起或快速移动
    // 松开时因此不会再放置或分配一次
    bool cancelNextInventoryRelease = false;
    // 双击收拢的状态：上一次按下的槽位类型与下标、按下的时刻
    // 以及这次按下是否落在 vanilla 那个 250 毫秒窗口内、算作第二次
    // 松开时同类物品堆会收拢到光标上
    std::optional<gameplay::SlotRef> lastClickedSlot;
    double lastClickTime = 0.0;
    bool isDoubleClicking = false;
    bool paused = true;
    bool debugOverlayOpen = false;
    // 客户端侧的跳跃边沿与疾跑双击边沿
    // 由 GLFW 键回调置上，processInput 把它们折进本帧的 MovementInput 并在发送后清掉
    // 边沿归客户端所有并经通道送出，跨进程的客户端因此不必去碰会话的累加器
    bool pendingJumpPressed_ = false;
    bool pendingForwardPressed_ = false;
    // 唯一的输入收集点
    // 它持有可重绑定的"动作到按键"表，以及上一帧的电平位图用于边沿检测
    // processInput() 因此不再直接读 GLFW 原始按键，键回调也改为比对绑定而不是写死的键值常量
    input::InputSystem inputSystem_;
    input::InputSystem::EventQueue inputEvents_;
    // 按键设置界面的重绑状态，建立在 InputSystem 这一唯一来源之上
    // 捕获目标非空时，下一次按键会被当作重绑消费掉，而不是游戏输入
    input::KeyBindingScreen keyBindScreen_{inputSystem_};
    // 游戏内的 HUD 叠加层
    // 右上角的吐司队列与右下角的音效字幕流都是不含 Vulkan 的客户端表现状态
    // 它们按帧间隔推进，由 HudRenderer 绘制
    ui::ToastQueue toastQueue_;
    ui::SubtitleFeed subtitleFeed_;
    bool dropRequested = false;
    bool dropWholeStack = false;
    bool chatOpen = false;
    // HUD 暗角，从全黑起步，每 tick 以 1% 的速度趋近 1 减去眼部亮度
    // 见 updateVignetteDarkness
    float vignetteDarkness_ = 1.0F;
    unsigned int suppressedOpeningChatCodepoint = 0U;
    int viewDistanceChunks = 4;
    // vanilla 的模拟距离，表示玩家周围多少个区块保持被模拟
    // 超出范围的实体会被冻结但仍然渲染
    // 单位是区块，读法与旁边的视距滑块一致
    int simulationDistanceChunks = 4;

    ui::WidgetId pressedMenuButton = ui::WidgetId::None;
    // 按下的 widget 在当前 ui::Page 中的下标，松开时据此经模型派发
    // 没有按下任何 widget 时为 kNoWidget
    std::size_t pressedMenuIndex_ = ui::kNoWidget;
    std::optional<world::VoxelRaycastHit> targetedBlock;

    // 玩家最后打开的那个熔炉方块
    // 共享的熔炉状态处于燃烧中时，该方块换成点亮态，纹理与发光一起变

    // 瞄准射线上最近的生物，每帧与方块目标一同算出
    // 输入处理把它打包进破坏命令
    std::optional<gameplay::EntityRayHit> creatureHit;
    // 手持物桥接上一帧采样到的挥动序号
    // 序号变化即表示重新开始，此时直接跳变，而不是把手臂插值倒回去
    std::optional<std::uint64_t> lastSwingSequence_;
    // 世界中第三人称玩家自己的挥动序号记忆
    // 它的攻击弧线因此能独立于第一人称桥接，在重新开始时跳变
    std::optional<std::uint64_t> lastWorldSwingSequence_;
    ui::TextFieldState chatInput;
    ui::ChatHistory chatHistory;
    // 打开着的聊天行的 Tab 补全状态
    // 含光标处那个词的候选列表，输入一变就重建，以及当前高亮的行
    // Tab 只在已存列表里循环，不重算
    std::vector<gameplay::command::Suggestion> chatSuggestions_;
    std::size_t chatSuggestionIndex_ = 0;
    // 手持物名称高亮的状态：屏幕上那个名字属于哪个快捷栏格与哪个物品堆，以及它何时开始显示
    // 名字在两秒后淡出，空手时整个清掉
    // 它由常量的 HUD 通道写入，因此和其它 UI 动画状态一样标为 mutable
    mutable std::size_t selectedNameSlot_ = static_cast<std::size_t>(-1);
    mutable gameplay::ItemStack selectedNameStack_;
    mutable double selectedNameShownAt_ = -1.0;
    // 菜单、光标闪烁和聊天过期都按墙钟走
    // 模拟暂停或太阳冻结期间它们必须继续运行
    double uiTimeSeconds = 0.0;
    // 动画插值用的时钟，与 uiTimeSeconds 分开，因为它随世界一起停
    // 暂停的游戏不该继续挥手臂
    // 两者都是帧局部的，也都不持久化
    double renderTimeSeconds = 0.0;
    // 进食状态，右键食物开始 vanilla 那段 32 tick 即 1.6 秒的进食
    // 期间手持物抬到嘴边，计时结束才真正吃下
    // 松开按键或换物品即取消
    double lastMouseX = 0.0;
    double lastMouseY = 0.0;
    float renderInterpolationAlpha = 0.0F;
    float fpsSampleSeconds = 0.0F;
    std::size_t fpsSampleFrames = 0U;
    int displayedFps = 0;
    GLFWwindow* window = nullptr;
    // 拥有设备、实例、分配器、队列和命令池
    // 下面那些同名成员是渲染器直接读取的非拥有副本
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
    // 拥有雨幕、GUI、全景、群系这几类纹理资源，详见 TextureManager
    // 方块、实体、字体三个数组仍以平铺成员的形式留在这里
    TextureManager textures_;
    // 本渲染器所做的每一次方块变更都走这一条路径
    // 方块实体、邻居更新、section 脏标记和掉落物这些后果因此从一处派发，不必在每个调用点重新拼一遍
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
    OffscreenTarget shadowTarget;
    VkDescriptorSetLayout shadowDebugSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool shadowDebugPool = VK_NULL_HANDLE;
    VkDescriptorSet shadowDebugSet = VK_NULL_HANDLE;
    VkSampler shadowDebugSampler = VK_NULL_HANDLE;
    glm::mat4 shadowLightViewProj{1.0F};
    bool shadowDisabled = std::getenv("MC_REBEDROCK_SHADOW_DISABLE") != nullptr;
    render::RainSystem rainSystem;
    RainMode rainMode_ = RainMode::Async;
    std::size_t rainCountOverride_ = 0U;
    float rainTime_ = 0.0F;
    // 风向及其偶尔的改向
    // `rainWindAngle_` 缓慢趋近 `windTargetAngle_`，`windShiftTimer_` 倒计时到下一次改向
    float rainWindAngle_ = 0.0F;
    float windTargetAngle_ = 0.0F;
    float windShiftTimer_ = 0.0F;
    // 天气音效的调度状态
    // `weatherSoundCadence_` 是那个放行计数器，它让雨声大约每一两帧响一次
    // 线性同余发生器供选列和放行两处掷点使用，不去动音频系统自己的随机状态
    int weatherSoundCadence_ = 0;
    std::uint32_t weatherSoundRng_ = 0x5EED11U;
    // 原生 64x256 的 environment/rain.png，供 vanilla 的逐列降雨通道使用
    // 它不能挤进方形的方块数组，否则宽高比会被压坏
    // 标题界面的六张全景面，各占一层，用专门的线性采样器采样
    // 它们是实拍图而不是像素画，也因此不放进 256px 的 GUI 数组，好保持原生分辨率
    // 群系配色查找表，含草色与叶色两张，由地形片元着色器用线性采样器采样
    // 群系边界因此融成逐像素的平滑渐变，这是 Java 逐顶点着色在 GPU 侧的等价物
    // 颜色来自一次纹理取样而不是逐顶点属性，因此更稳健
    // 世界加载时按世界种子和 vanilla 配色图生成，像素数据逐种子重建，图像与采样器只创建一次
    ui::BitmapFontMetrics fontMetrics;
    // ascii.png 的字形度量、传统 unicode 页，以及界面读取文案所用的翻译表
    ui::TextFont textFont;
    ui::Language language;
    std::string queuedLanguageCode;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> swapchainImages;
    VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
    // 整帧都画在**未经伽马转换**的目标上，与 vanilla 一致：着色器写的就是最终字节，
    // 混合也发生在这些 sRGB 编码值上。所有被采样的颜色纹理同样是 UNORM，因此纹素
    // 一路不经过任何传输函数——世界着色器里逐条转写自 vanilla 的算式于是成立。
    // 通道序必须跟交换链一致——最后那步是 vkCmdCopyImage，逐字节搬运不做任何转换，
    // BGRA 的场景图 copy 进 RGBA 的交换链会把红蓝对调。
    [[nodiscard]] VkFormat sceneUnormFormat() const {
        return swapchainFormat == VK_FORMAT_R8G8B8A8_UNORM ||
                       swapchainFormat == VK_FORMAT_R8G8B8A8_SRGB
                   ? VK_FORMAT_R8G8B8A8_UNORM
                   : VK_FORMAT_B8G8R8A8_UNORM;
    }
    VkExtent2D swapchainExtent{};
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    std::vector<DepthTarget> depthTargets;
    std::vector<ColorTarget> colorTargets;
    // 世界与 GUI 都画进这张场景图，最后整屏 copy 进交换链图像
    // 它带 MUTABLE_FORMAT，因此能有两个视图：世界那趟用 SRGB 视图（着色器写线性值、
    // 硬件编码、混合在线性空间），GUI 那趟用 UNORM 视图（着色器直接写编码值、
    // 混合也在编码值上，与 vanilla 同一套）
    // 每个交换链图像一份，帧间不会互相踩
    struct SceneTarget final {
        AllocatedImage image;
        VkImageView view = VK_NULL_HANDLE;
    };
    std::vector<SceneTarget> sceneTargets;
    // GUI 那趟自己的深度：背包里的 3D 玩家预览和第一人称手持物要深度测试，
    // 而世界的深度可能是多重采样的。顺带对齐 vanilla——它在画手之前清一次深度，
    // 所以手不会被贴脸的方块切掉
    std::vector<DepthTarget> guiDepthTargets;
    std::vector<VkFramebuffer> guiFramebuffers;
    VkPipeline crosshairPipeline = VK_NULL_HANDLE;
    VkPipelineLayout hudPipelineLayout = VK_NULL_HANDLE;
    VkPipeline hudPipeline = VK_NULL_HANDLE;
    VkPipeline hudBlockIconPipeline = VK_NULL_HANDLE;
    VkPipelineLayout panoramaPipelineLayout = VK_NULL_HANDLE;
    VkPipeline panoramaPipeline = VK_NULL_HANDLE;
    VkPipeline vignettePipeline = VK_NULL_HANDLE;
    // 世界通道的管线族，定义见 render/vulkan/WorldRenderTypes.hpp
    // 所有权仍在这里（本类创建，并随交换链销毁重建），WorldRenderer 只持有它的引用
    WorldPipelines worldPipelines_;
    // 遮挡查询的 GPU 资源与开关，定义见 WorldRenderTypes.hpp
    // 逐 section 的查询结果是纯 CPU 状态，已经归 WorldRenderer 自有
    OcclusionResources occlusion_{.disabled = disableOcclusionQueries()};
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkFence> imagesInFlight;
    std::vector<VkSemaphore> presentSemaphores;
    std::array<FrameContext, kFramesInFlight> frames{};
    std::size_t currentFrame = 0;
    std::uint32_t frameNumber_ = 0;
    // 压测的帧数上限，取自 MC_REBEDROCK_STRESS_FRAMES，为 0 表示不启用
    std::size_t stressFrames = 0;
    // MC_REBEDROCK_DISABLE_OCCLUSION 关掉遮挡通道
    // 设上它之后崩溃就消失的话，就能把问题归到查询上
    std::size_t completedStreamBatchCount = 0;
    std::size_t completedBlockEditCount = 0;
    std::size_t loadedCpuChunkCount = 0;
    std::size_t peakPendingSectionCount = 0;
    std::size_t lastSessionPeakPendingSectionCount = 0;
    VkDeviceSize totalUploadedBytes = 0;
    // 流送上传预算，每帧按平滑后的帧时间调整
    // GPU 有余量时抬高，帧时间往上走时压低，迟滞规则见 render/StreamingBudget.hpp
    float smoothedFrameSeconds_ = 0.0F;
    std::size_t streamingUploadBudget_ = mc::render::kMaxStreamingBudgetHigh;
    std::size_t lastVisibleMeshCount = std::numeric_limits<std::size_t>::max();
    std::size_t lastGpuMeshCount = std::numeric_limits<std::size_t>::max();
    std::size_t lastPendingSectionCount = std::numeric_limits<std::size_t>::max();
    std::string lastPlayerMode;
    gameplay::ItemStack lastSelectedItem{};
    std::unordered_map<PersistentEditPosition, std::size_t, PersistentEditPositionHash>
        savedEditIndices;
    // 正在编辑的存档，在按下编辑时记下
    // 即使中途列表刷新，编辑与删除流程也照常可用
    bool worldSessionActive = false;

    // 把 HudRenderer 接到本 Impl 的状态上
    // 引用字段直接绑到成员，缩放后重建的管线和被输入改动的 UI 标志因此始终可见
    // 另有几个 std::function 钩子把世界渲染侧的耦合留在 Impl 里
    // 那几处是手持物、水下判定和模型预览的描述符
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
            .hudBlockIconPipeline = hudBlockIconPipeline,
            .hudPipelineLayout = hudPipelineLayout,
            .vignettePipeline = vignettePipeline,
            .crosshairPipeline = crosshairPipeline,
            .panoramaPipeline = panoramaPipeline,
            .panoramaPipelineLayout = panoramaPipelineLayout,
            .heldItemPipeline = worldPipelines_.heldItemPipeline,
            .itemPipelineLayout = worldPipelines_.itemPipelineLayout,
            .inventoryOpen = inventoryOpen,
            .containerScreen = uiFrameData_.containerScreen,
            .activeChest = uiFrameData_.activeChest,
            .debugOverlayOpen = debugOverlayOpen,
            .inventoryDragActive = inventoryDragActive,
            .inventoryDragSlots = inventoryDragSlots,
            .chatOpen = chatOpen,
            .chatHistory = chatHistory,
            .chatInput = chatInput,
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

    // 放在最后声明，它的引用成员才能绑到上面已完全构造好的成员，成员初始化按声明顺序执行
    // HUD 的绘制通道就在这里
    HudRenderer hud_{makeHudBindings()};

    // 把 WorldRenderer 接到本 Impl 的状态上，只用引用
    // 所有状态仍归这里所有，GPU 资源的销毁次序因此不受影响
    // 另有几个 std::function 钩子承接留在 Impl 里的相机与玩法回调
    [[nodiscard]] WorldRenderer::Bindings makeWorldBindings() {
        return WorldRenderer::Bindings{
            .testScene = testScene,
            .pipelines = worldPipelines_,
            .occlusion = occlusion_,
            .chunkStreamer = chunkStreamer,
            .interactionWorld = interactionWorld,
            .clientCache = clientCache,
            .interactionLightEngine = interactionLightEngine,
            .gpuMeshes = gpuMeshes,
            .deviceBufferPool_ = deviceBufferPool_,
            .stagingBufferPool_ = stagingBufferPool_,
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
            .shadowTarget = shadowTarget,
            .shadowDebugSet = shadowDebugSet,
            .shadowLightViewProj = shadowLightViewProj,
            .shadowDisabled = shadowDisabled,
            .rainSystem = rainSystem,
            .rainMode_ = rainMode_,
            .rainTime_ = rainTime_,
            .language = language,
            .swapchainExtent = swapchainExtent,
            .framebuffers = framebuffers,
            .guiFramebuffers = guiFramebuffers,
            .copySceneToSwapchain =
                [this](VkCommandBuffer c, std::uint32_t index) { copySceneToSwapchain(c, index); },
            .frames = frames,
            .currentFrame = currentFrame,
            .peakPendingSectionCount = peakPendingSectionCount,
            .smoothedFrameSeconds_ = smoothedFrameSeconds_,
            .streamingUploadBudget_ = streamingUploadBudget_,
            .pendingSectionUpdates = pendingSectionUpdates,
            .latestSectionRevisions = latestSectionRevisions,
            .worldEpoch = worldEpoch,
            .loadedCpuChunkCount = loadedCpuChunkCount,
            .completedBlockEditCount = completedBlockEditCount,
            .completedStreamBatchCount = completedStreamBatchCount,
            .lastVisibleMeshCount = lastVisibleMeshCount,
            .worldSessionActive = worldSessionActive,
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
                // 有区块离开了模拟半径
                // 运行时把它的编辑与生物写进该区块的 region 文件，并从模拟里移除这些生物
                // 半径之外的兽群因此在磁盘上存活，直到它的区块被重新流送进来
                runtime.persistUnloadedChunk(position);
            },
            .onChunkLoaded = [this](world::ChunkPosition position) {
                // 有区块重新流送进来，把卸载时为它写下的生物恢复回去，如果有的话
                runtime.restoreLoadedChunk(position);
            },
        };
    }

    // 声明在 hud_ 之后，它的 Bindings 才能引用 hud_，世界渲染通道在这里
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

int VulkanRenderer::run() { return impl_->run(); }

} // namespace mc::render
