#pragma once
// 世界渲染 / 区块流送 / 遮挡剔除子系统
// 缓冲池、gpuMeshes、设备销毁次序这些 GPU 资源生命周期对崩溃极其敏感
// 因此全部状态仍归 Impl 所有，这里只通过同名引用成员访问，引用在 Bindings 里一次性绑定
// 另有几个 std::function 钩子接留在 Impl 的相机与玩法回调
// 全部内联在头文件里，与 VulkanDevice 同一形态
#include "render/vulkan/BlockAtlasLayout.hpp"
#include "render/vulkan/HudRenderer.hpp"
#include "render/vulkan/HudTypes.hpp"
#include "render/TemporalAntiAliasing.hpp"
#include "render/vulkan/MenuBlur.hpp"
#include "render/vulkan/TemporalResolve.hpp"
#include "render/vulkan/WorldRenderTypes.hpp"
#include "render/vulkan/VulkanResources.hpp"
#include "render/vulkan/GpuSceneBuffer.hpp"
#include "render/vulkan/OffscreenTarget.hpp"
#include "render/vulkan/TextureManager.hpp"

#include "core/EnvFlags.hpp"
#include "core/FrameTrace.hpp"

#include "render/graph/FrameGraph.hpp"

#include "animation/AnimationAssets.hpp"
#include "animation/DisplayEntityAnimation.hpp"
#include "animation/HingeAnimation.hpp"
#include "animation/ModelAnimationSystem.hpp"
#include "animation/PlayerModelAnimator.hpp"
#include "animation/SkeletalModel.hpp"
#include "client/ClientMirror.hpp"
#include "config/GameOptions.hpp"
#include "gameplay/ChestSystem.hpp"
#include "gameplay/DyeColor.hpp"
#include "gameplay/GameSession.hpp"
#include "gameplay/Random.hpp"
#include "gameplay/GameplayMutationSink.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/ItemEntitySystem.hpp"
#include "gameplay/MiningSystem.hpp"
#include "gameplay/SpawnEggItems.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "gameplay/entities/SpeciesRenderData.hpp"
#include "render/BlockOutlineGeometry.hpp"
#include "render/Frustum.hpp"
#include "render/MeshData.hpp"
#include "render/ParticleSystem.hpp"
#include "render/PerspectiveCamera.hpp"
#include "render/RainSystem.hpp"
#include "render/SectionDeliveryQueue.hpp"
#include "render/StreamingBudget.hpp"
#include "render/SunShadowMap.hpp"
#include "render/EntityRenderDraws.hpp"
#include "render/EntityShadowDecal.hpp"
#include "render/SkyLight.hpp"
#include "ui/Language.hpp"
#include "ui/TextFont.hpp"
#include "ui/UiFrameData.hpp"
#include "world/ChunkMesher.hpp"
#include "world/ChunkStreamer.hpp"
#include "world/ChunkStreamingTrace.hpp"
#include "world/WorldLock.hpp"
#include "world/DayNightCycle.hpp"
#include "world/World.hpp"
#include "world/VoxelRaycast.hpp"
#include "world/WorldConstants.hpp"
#include "world/WorldLightEngine.hpp"

#include <vulkan/vulkan.h>
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
#include <span>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
namespace mc::render {

class WorldRenderer final {
 public:
  struct Bindings final {
    std::optional<TestSceneOptions>& testScene;
    // 世界通道的全部管线与管线布局，所有权在 Impl（随交换链销毁重建）
    // 这里必须是引用：重建之后读到的得是新句柄，一份拷贝就是一堆悬垂句柄
    WorldPipelines& pipelines;
    // 遮挡查询的 GPU 资源与开关，所有权同样在 Impl
    OcclusionResources& occlusion;
    world::ChunkStreamer& chunkStreamer;
    world::World& interactionWorld;
    // 客户端区块缓存：渲染器做网格化和采样所用的世界
    // 模拟侧写 interactionWorld，渲染侧读这份缓存，两边各自拥有自己的区块数据
    world::World& clientCache;
    world::WorldLightEngine& interactionLightEngine;
    std::unordered_map<world::SectionPosition, GpuMesh, world::SectionPositionHash>& gpuMeshes;
    StreamBufferPool& deviceBufferPool_;
    StreamBufferPool& stagingBufferPool_;
    render::SectionDeliveryQueue<world::SectionPosition, world::SectionPositionHash>& pendingSectionOrder;
    gameplay::GameSession& gameSession;
    // 客户端的玩家/世界/实体镜像
    // 渲染侧的读取在解码通道之后都来自这里；会话只保留给测试与交互路径上少数几个显式的权威操作
    const client::ClientMirror& clientMirror;
    // 回环通道的客户端一端，只暴露发命令的钩子
    // Q 丢弃因此和其它命令一样把意图走消息路径送出，而不是直接伸手进会话的队列
    std::function<void(gameplay::GameCommand)> enqueueClientCommand;
    gameplay::SimulationHost& simulationHost;
    world::WorldLock& worldLock;
    ui::UiFrameData& uiFrameData_;
    PerspectiveCamera& camera;
    std::vector<gameplay::entities::SpeciesRenderModel>& speciesModels;
    animation::ModelAnimationSystem& heldItemAnimation;
    animation::PlayerModelAnimator& worldPlayerAnimator;
    CameraPerspective& cameraPerspective;
    float& worldBodyYaw;
    ParticleSystem& particleSystem;
    bool& inventoryOpen;
    bool& spawnPositionInitialized;
    bool& worldReady;
    bool& paused;
    bool& dropRequested;
    bool& dropWholeStack;
    bool& chatOpen;
    std::optional<world::VoxelRaycastHit>& targetedBlock;
    // RN-39：离屏导出的 `--outline`。生产运行里恒为 false
    bool& previewOutline;
    double& renderTimeSeconds;
    float& renderInterpolationAlpha;
    GLFWwindow*& window;
    VkInstance& instance;
    VkSurfaceKHR& surface;
    VkDevice& device;
    VmaAllocator& allocator;
    VulkanResources& resources_;
    TextureManager& textures_;
    std::array<VkDescriptorSet, kFramesInFlight>& sceneDescriptorSets;
    GpuSceneBuffer& gpuSceneBuffer;
    OffscreenTarget& shadowTarget;
    VkDescriptorSet& shadowDebugSet;
    // RN-35：逐级的光源矩阵。所有权在 VulkanRenderer::Impl
    std::array<glm::mat4, kSunShadowCascadeCount>& shadowLightViewProj;
    bool& shadowDisabled;
    // RN-23：玩家的图形设置。贴花只读 entityShadows，但绑整份而不是再镜像一个 bool，
    // 因为一个手工同步的镜像就是一处会被忘记更新的地方（shadowDisabled 之所以是自己
    // 的 bool，是因为它还叠着环境变量与烟测，不等于 !options.sunShadows）。
    const config::GameOptions& options;
    render::RainSystem& rainSystem;
    RainMode& rainMode_;
    float& rainTime_;
    ui::Language& language;
    VkExtent2D& swapchainExtent;
    std::vector<VkFramebuffer>& framebuffers;
    // GUI 那趟的帧缓冲（场景图的 UNORM 视图 + 它自己的深度），以及把画完的场景图
    // 逐字节 copy 进交换链图像的那一步——图像的所有权在 VulkanRenderer，这里只调用
    std::vector<VkFramebuffer>& guiFramebuffers;
    // UI-5：全景那一趟 + 六趟整帧模糊，跑在世界与界面之间
    MenuBlur& menuBlur;
    // TAA-1：时间性 resolve 那一趟与它的逐帧状态。两者的所有权都在 VulkanRenderer
    // （随交换链销毁重建），这里只调用
    TemporalResolve& temporalResolve;
    TemporalFrameState& temporalState;
    std::function<void(VkCommandBuffer, std::uint32_t)> copySceneToSwapchain;
    std::array<FrameContext, kFramesInFlight>& frames;
    std::size_t& currentFrame;
    std::size_t& peakPendingSectionCount;
    float& smoothedFrameSeconds_;
    std::size_t& streamingUploadBudget_;
    std::unordered_map<world::SectionPosition, world::SectionMeshUpdate, world::SectionPositionHash>& pendingSectionUpdates;
    std::unordered_map<world::SectionPosition, std::uint64_t, world::SectionPositionHash>& latestSectionRevisions;
    std::uint64_t& worldEpoch;
    std::size_t& loadedCpuChunkCount;
    std::size_t& completedBlockEditCount;
    std::size_t& completedStreamBatchCount;
    std::size_t& lastVisibleMeshCount;
    bool& worldSessionActive;
    VkDeviceSize& totalUploadedBytes;
    HudRenderer& hud_;
    std::function<std::size_t()> rainTargetCount;
    std::function<glm::mat4()> renderViewMatrix;
    std::function<glm::mat4()> viewBobbingMatrix;
    std::function<RenderEye()> renderEyeState;
    std::function<float()> cameraFarPlane;
    std::function<float()> renderDistanceBlocks;
    std::function<void()> initializeSpawnPosition;
    std::function<void(int, int, int, world::Block, std::uint8_t, std::optional<world::BlockOrientation>)> submitWorldEditFn;
    std::function<bool(int, int, int)> hasPersistentEditFn;
    // 区块生命周期回调，接到运行时的持久化上
    // onChunkUnloaded 在区块被移除（离开模拟半径）时触发，onChunkLoaded 在区块生成进来时触发
    // 两者都在该批次的世界写区间内调用，因此处理函数可以安全触碰模拟状态与存档
    std::function<void(world::ChunkPosition)> onChunkUnloaded;
    std::function<void(world::ChunkPosition)> onChunkLoaded;
    // 烘焙式 frame graph。所有权在 Impl（与 WorldPipelines 同一条边界：随交换链销毁重建），
    // 这里必须是引用——重建之后读到的得是新编译的那张表，一份拷贝就是一堆悬垂句柄
    graph::BakedGraph& frameGraph;
  };

  explicit WorldRenderer(const Bindings& b)
      : testScene(b.testScene), pipelines(b.pipelines), occlusion(b.occlusion), chunkStreamer(b.chunkStreamer),
        interactionWorld(b.interactionWorld), clientCache(b.clientCache),
        interactionLightEngine(b.interactionLightEngine), gpuMeshes(b.gpuMeshes),
        deviceBufferPool_(b.deviceBufferPool_), stagingBufferPool_(b.stagingBufferPool_),
        pendingSectionOrder(b.pendingSectionOrder),
        gameSession(b.gameSession), clientMirror(b.clientMirror),
        enqueueClientCommand(b.enqueueClientCommand), simulationHost(b.simulationHost),
        worldLock(b.worldLock), uiFrameData_(b.uiFrameData_), camera(b.camera),
        speciesModels(b.speciesModels), heldItemAnimation(b.heldItemAnimation),
        worldPlayerAnimator(b.worldPlayerAnimator), cameraPerspective(b.cameraPerspective),
        worldBodyYaw(b.worldBodyYaw), particleSystem(b.particleSystem),
        inventoryOpen(b.inventoryOpen), spawnPositionInitialized(b.spawnPositionInitialized),
        worldReady(b.worldReady), paused(b.paused), dropRequested(b.dropRequested),
        dropWholeStack(b.dropWholeStack), chatOpen(b.chatOpen), targetedBlock(b.targetedBlock),
        previewOutline(b.previewOutline),
        renderTimeSeconds(b.renderTimeSeconds),
        renderInterpolationAlpha(b.renderInterpolationAlpha), window(b.window),
        instance(b.instance), surface(b.surface), device(b.device), allocator(b.allocator),
        resources_(b.resources_), textures_(b.textures_),
        sceneDescriptorSets(b.sceneDescriptorSets), gpuSceneBuffer(b.gpuSceneBuffer),
        shadowTarget(b.shadowTarget), shadowDebugSet(b.shadowDebugSet),
        shadowLightViewProj(b.shadowLightViewProj),
        shadowDisabled(b.shadowDisabled), options(b.options), rainSystem(b.rainSystem),
        rainMode_(b.rainMode_),
        rainTime_(b.rainTime_), language(b.language),
        swapchainExtent(b.swapchainExtent), framebuffers(b.framebuffers),
          guiFramebuffers(b.guiFramebuffers), menuBlur(b.menuBlur),
          temporalResolve(b.temporalResolve), temporalState(b.temporalState),
          copySceneToSwapchain(b.copySceneToSwapchain),
          frames(b.frames),
        currentFrame(b.currentFrame), peakPendingSectionCount(b.peakPendingSectionCount),
        smoothedFrameSeconds_(b.smoothedFrameSeconds_),
        streamingUploadBudget_(b.streamingUploadBudget_), pendingSectionUpdates(b.pendingSectionUpdates),
        latestSectionRevisions(b.latestSectionRevisions), worldEpoch(b.worldEpoch),
        loadedCpuChunkCount(b.loadedCpuChunkCount),
        completedBlockEditCount(b.completedBlockEditCount),
        completedStreamBatchCount(b.completedStreamBatchCount),
        lastVisibleMeshCount(b.lastVisibleMeshCount), worldSessionActive(b.worldSessionActive),
        totalUploadedBytes(b.totalUploadedBytes), hud_(b.hud_), rainTargetCount(b.rainTargetCount),
        renderViewMatrix(b.renderViewMatrix), viewBobbingMatrix(b.viewBobbingMatrix),
        renderEyeState(b.renderEyeState), cameraFarPlane(b.cameraFarPlane),
        renderDistanceBlocks(b.renderDistanceBlocks),
        initializeSpawnPosition(b.initializeSpawnPosition), submitWorldEditFn(b.submitWorldEditFn),
        hasPersistentEditFn(b.hasPersistentEditFn), onChunkUnloaded(b.onChunkUnloaded),
        onChunkLoaded(b.onChunkLoaded), frameGraph(b.frameGraph) {
  }

  WorldRenderer(const WorldRenderer&) = delete;
  WorldRenderer& operator=(const WorldRenderer&) = delete;

  // 每个待上传 section 入队时的身份信息，供上传侧的投递顺序诊断使用
  // 它必须按真正请求它的那个中心评分，而不是按最新的请求中心
  // 否则中心移动之后才上传的事件会报出落在 [0, loadRadius] 之外的环号
  // 同时记下入队时的中心、纪元和事件类型，类型分流送、优先、重网格三种
  // 顺序分析因此能排除优先与重网格事件，并在中心或纪元变化时重置环号基准
  // 纯诊断数据，归本类所有
  // 它既不属于 Impl 那套对崩溃敏感的 GPU 资源生命周期，也不属于玩法侧的 pendingSectionUpdates
  // 仅在 chunkTraceEnabled() 时写入和消费
  struct PendingSectionTrace final {
      int ring = 0;
      int centerX = 0;
      int centerZ = 0;
      std::uint64_t epoch = 0;
      diag::DeliveryEventType type = diag::DeliveryEventType::Streaming;
  };
  std::unordered_map<world::SectionPosition, PendingSectionTrace, world::SectionPositionHash>
      pendingSectionEnqueueRing_{};

  // 当前生效的"首次网格延迟"计时窗口
  // processChunkStreaming 只为新进入请求半径的区块起计时，为刚离开的撤销计时
  // 纪元切换时则全部撤销，而不是每次中心移动就把整个窗口重新起计
  // 重新起计会把已经可见的区块也算进去，它们下一次普通重网格就会伪造出假的首次网格样本
  // 纯诊断，仅在 chunkTraceEnabled() 时触碰
  bool traceWindowValid_ = false;
  int traceArmedCenterX_ = 0;
  int traceArmedCenterZ_ = 0;
  int traceArmedRadius_ = 0;
  std::uint64_t traceArmedEpoch_ = 0;


    // ---- 与渲染器内核重复的一组助手，都是对已绑定引用的纯转发 ----
    // 它们供搬过来的函数体使用，Impl 自己另外保留一份
    [[nodiscard]] std::string_view translate(std::string_view key,
                                             std::string_view fallback) const {
        return language.translate(key, fallback);
    }
    [[nodiscard]] AllocatedBuffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                               bool hostVisible) const {
        return resources_.createBuffer(size, usage, hostVisible);
    }
    void destroyBuffer(AllocatedBuffer& buffer) const noexcept { resources_.destroyBuffer(buffer); }
    // submitWorldEdit 在 Impl 的调用点带默认实参
    // 这里经绑定的钩子转发，让搬过来的测试场景装配代码原样编译
    void submitWorldEdit(int x, int y, int z, world::Block block, std::uint8_t fluidLevel = 0U,
                         std::optional<world::BlockOrientation> orientation = std::nullopt) {
        submitWorldEditFn(x, y, z, block, fluidLevel, orientation);
    }

    void remeshSectionImmediate(world::SectionPosition position,
                                const world::ChunkLightSampler& lighting) {
        // 读世界来构建几何
        // 调用点已经处于临界区内，因此这里不再自取锁——见各调用方
        world::SectionMeshUpdate update;
        update.position = position;
        update.mesh = chunkStreamer.acquireMeshData();
        static_cast<void>(
            world::ChunkMesher::buildSection(clientCache, {position.chunkX, position.chunkZ},
                                             position.sectionY, lighting, update.mesh));
        update.remove = update.mesh.empty();
        update.highPriority = true;
        // 故意不动 latestSectionRevisions：这只是一层临时预览
        // 工作线程那次权威重建带着严格更高的修订号，其批次被轮询到时会覆盖它
        if (!pendingSectionUpdates.contains(position)) {
            // highPriority 为 true 时进入队列的优先通道（排在所有环桶之前），编辑预览因此即时可见
            // 优先条目不使用环号参数
            pendingSectionOrder.push(position, 0, true);
        }
        pendingSectionUpdates.insert_or_assign(position, std::move(update));
    }


    void queueStreamBatch(world::ChunkStreamBatch batch) {
        if (batch.worldEpoch != worldEpoch)
            return;
        // 逐帧流送耗时的前一半，后一半在 prepareStreamingUpdates 的上传侧
        // 两半记入同一个样本，调用方因此能分辨批次落地开销与 GPU 上传准备开销
        // 本函数每投递一个批次只跑一次，poll() 排的是一个很小的队列而不是逐 section
        // 下面这次 steady_clock::now() 因此即便关闭追踪也无条件执行
        // 相比本函数每次都要做的世界加锁与区块表写入，它可以忽略
        const bool chunkTrace = diag::chunkTraceEnabled();
        const auto queueBatchStart = diag::ChunkStreamingMetrics::Clock::now();
        if (chunkTrace) {
            // 首次网格延迟从区块进入请求半径那一刻起计，计时点在 processChunkStreaming 里
            // 对应的语义是"从进入请求半径到 GPU 可见"
            // 在这里 CPU 批次抵达时才起计会漏掉"进半径到排入生成队列"这一段，把长尾算小
            // 这里只推进半径填充进度
            diag::chunkStreamingMetrics().noteRadiusProgress(
                batch.center.x, batch.center.z, batch.loadedChunkCount, queueBatchStart);
        }
        loadedCpuChunkCount = batch.loadedChunkCount;
        completedBlockEditCount += batch.appliedBlockEditCount;
        const bool generatedOrUnloadedChunks = batch.appliedBlockEditCount == 0U;
        ++completedStreamBatchCount;
        if (diag::traceEnabled()) {
            ++diag::frameTrace().queueBatchCount;
        }
        std::vector<std::size_t> appliedStateUpdates;
        appliedStateUpdates.reserve(batch.stateUpdates.size());

        // 第一阶段是唯一的服务端世界临界区
        // 它负责装入与移除权威区块、应用带保护的跨区块地物写入、执行持久化与实体回调
        // 客户端缓存、光照和网格簿记归渲染侧所有，特意留在临界区之外
        {
            const auto batchWrite = worldLock.write();
            const auto lockHoldStart = std::chrono::steady_clock::now();
            for (auto& update : batch.chunkUpdates) {
                if (update.remove) {
                    if (diag::traceEnabled()) {
                        ++diag::frameTrace().unloadedChunks;
                    }
                    if (chunkTrace) {
                        // 这是主动卸载，停止追踪，免得被当成"丢失的区块"报出来
                        // 诊断关心的是本该常驻却不在的区块，不是正常离开半径的区块
                        diag::missingChunkDetector().noteChunkRemoved(update.position.x,
                                                                      update.position.z);
                        // 卸载时清掉该区块的首次网格计时与已记录标记
                        // 日后重新加载才能干净地重新计一次
                        // 也不会留下陈旧计时，被后来某次不同驻留期的网格误记
                        diag::chunkStreamingMetrics().disarmFirstMesh(
                            {update.position.x, 0, update.position.z});
                    }
                    interactionWorld.removeChunk(update.position);
                    if (onChunkUnloaded) {
                        onChunkUnloaded(update.position);
                    }
                } else if (generatedOrUnloadedChunks) {
                    // 只有生成批次会引入 CPU 区块；编辑批次只贡献网格，绝不覆盖更新的玩法状态
                    if (chunkTrace) {
                        diag::missingChunkDetector().noteChunkDelivered(
                            update.position.x, update.position.z, queueBatchStart);
                    }
                    interactionWorld.setChunk(update.position, update.chunk);
                    if (onChunkLoaded) {
                        onChunkLoaded(update.position);
                    }
                }
            }
            // 生成可能把树冠伸进一个已加载的邻居区块
            // 该格若有更新的本地玩法编辑，保留它
            for (std::size_t index = 0; index < batch.stateUpdates.size(); ++index) {
                const auto& update = batch.stateUpdates[index];
                if (interactionWorld.state(update.worldX, update.y, update.worldZ) ==
                        update.expected &&
                    !hasPersistentEditFn(update.worldX, update.y, update.worldZ)) {
                    static_cast<void>(interactionWorld.setState(
                        update.worldX, update.y, update.worldZ, update.state));
                    appliedStateUpdates.push_back(index);
                }
            }
            if (!spawnPositionInitialized) {
                initializeSpawnPosition();
            }
            if (completedStreamBatchCount == 1U &&
                diag::smokeTestEnabled()) {
                const auto snap = clientMirror.player();
                const glm::vec3 oldPosition = snap.physicsCurrent;
                gameSession.teleportPlayer(gameplay::kPrimaryPlayerId,
                                           glm::vec3{52.284F, oldPosition.y, -4.284F});
                const float eyeHeight = snap.sneaking
                                            ? gameplay::PlayerController::kSneakingEyeHeight
                                            : gameplay::PlayerController::kEyeHeight;
                camera.setPosition(snap.physicsCurrent + glm::vec3{0.0F, eyeHeight, 0.0F});
            }
            if (completedStreamBatchCount == 2U &&
                diag::smokeTestEnabled()) {
                gameplay::GameplayMutationSink sink{interactionWorld, gameSession};
                const auto place = [&](int x, int y, int z, world::Block block) {
                    static_cast<void>(gameSession.worldMutations().setBlock(
                        interactionWorld, {x, y, z}, world::BlockState{block},
                        world::MutationFlags::All, world::MutationCause::Command, sink));
                };
                place(52, 70, -4, world::Block::Glass);
                place(54, 72, -4, world::Block::Sand);
                place(50, 70, -4, world::Block::Water);
            }
            if (diag::traceEnabled()) {
                diag::frameTrace().lockHoldMs += diag::msSince(lockHoldStart);
            }
        }

        // 第二阶段完全归渲染侧
        // 把工作线程的区块搬进客户端缓存，只发生在服务端已经复制走其权威值之后
        // 20 TPS 的 tick 不会为客户端重新光照或网格队列等待
        for (auto& update : batch.chunkUpdates) {
            if (update.remove) {
                clientCache.removeChunk(update.position);
            } else if (generatedOrUnloadedChunks) {
                clientCache.setChunk(update.position, std::move(update.chunk));
            }
        }
        for (const auto index : appliedStateUpdates) {
            const auto& update = batch.stateUpdates[index];
            static_cast<void>(
                clientCache.setState(update.worldX, update.y, update.worldZ, update.state));
            interactionLightEngine.updateBlock(clientCache, update.worldX, update.y,
                                               update.worldZ);
        }
        if (!appliedStateUpdates.empty()) {
            static_cast<void>(interactionLightEngine.takeDirtySections());
        }
        for (auto& update : batch.sectionUpdates) {
            const auto latest = latestSectionRevisions.find(update.position);
            if (latest != latestSectionRevisions.end() && update.revision < latest->second) {
                continue;
            }
            latestSectionRevisions.insert_or_assign(update.position, update.revision);
            update.highPriority = batch.highPriority;
            // 该 section 相对本批次自己那个请求中心的切比雪夫环号
            // 它是承重数据而非仅供诊断，投递队列以它为桶键
            // 上传顺序因此保持严格的由中心向外扩环，而不是单纯的到达顺序
            // 同样遵循"按本批次的中心记录，而不是最新的中心"
            // 中心之后移动了，先前请求的 section 仍必须按真正请求它的那个中心评分
            const int enqueueRing =
                std::max(std::abs(update.position.chunkX - batch.center.x),
                         std::abs(update.position.chunkZ - batch.center.z));
            if (chunkTrace) {
                // 按进入的通道给事件打类型标记
                // 玩法编辑、同步、画质重网格这类高优先级批次会跳过环桶
                // 它们不属于由中心向外的扩环序列，标记为 Priority，顺序分析会排除
                // 其余一切都按自身环号回到环桶，属于普通 Streaming
                // 普通优先级的淘汰补救重网格也在其中
                pendingSectionEnqueueRing_[update.position] = {
                    enqueueRing, batch.center.x, batch.center.z, batch.worldEpoch,
                    batch.highPriority ? diag::DeliveryEventType::Priority
                                       : diag::DeliveryEventType::Streaming};
            }
            if (!pendingSectionUpdates.contains(update.position)) {
                // 限制网格积压
                // 高优先级条目豁免（它们只会进优先通道，且淘汰逻辑从不碰优先通道）
                // 淘汰取自投递队列中最远的非空环桶，而不是 FIFO 队尾
                // 队尾常常是一个靠近中心、只是在排了大量远环工作的批次里到得晚的 section
                // 把它淘汰掉正是中心区块被饿死的直接原因
                // 改成淘汰最远环，可以保证被丢掉的工作永远不比留在队列里的更近
                if (!update.highPriority &&
                    pendingSectionUpdates.size() >= kMaxPendingSectionUpdates) {
                    if (const auto victim = pendingSectionOrder.evictFarthest()) {
                        const auto victimFound = pendingSectionUpdates.find(*victim);
                        if (victimFound != pendingSectionUpdates.end()) {
                            latestSectionRevisions.erase(*victim);
                            pendingSectionUpdates.erase(victimFound);
                            pendingSectionEnqueueRing_.erase(*victim);
                            // 丢掉一个已排队但从未上传的 section，会在流送积压消化完后留下永久空洞
                            // 症状是区块缺失，直到放置方块强制重网格才补回来
                            // 因此重新请求一次重网格，让它在积压清空后再次投递
                            // 已经在 GPU 上的 section 只是少了一次重网格，无需重新请求
                            //
                            // 补救请求用普通优先级，因为被淘汰的本来就是排队环里最远的那个
                            // 走优先通道重投会让远环的补救工作抢在中心环桶前面
                            // 那正是破坏中心保护、让半径填充慢约 25% 的那次回归
                            // 普通优先级把它按自身环距重新排在中心之后
                            // 积压清空后它照样会落地，不留空洞
                            if (!gpuMeshes.contains(*victim)) {
                                chunkStreamer.requestSectionRemesh(*victim, /*highPriority=*/false);
                            }
                        }
                    }
                }
                // 玩法编辑批次插到流送之前，免得刚发生的世界变化卡在一堆远处区块网格后面
                // 其余一律按环号入队
                // 无论本批次的 section 以什么顺序完成网格化，投递顺序都保持由中心向外扩环
                pendingSectionOrder.push(update.position, enqueueRing, batch.highPriority);
            }
            pendingSectionUpdates.insert_or_assign(update.position, std::move(update));
        }
        peakPendingSectionCount = std::max(peakPendingSectionCount, pendingSectionUpdates.size());
        lastVisibleMeshCount = std::numeric_limits<std::size_t>::max();
        if (chunkTrace) {
            // 逐帧流送耗时的前一半，即本批次落地的开销
            // 含服务端世界写入、客户端缓存镜像和 section 队列簿记
            // GPU 上传那一半单独记在 prepareStreamingUpdates 里
            // 这里的对应字段填 0，好让按帧汇总的调用方把同一帧的两行相加
            diag::chunkStreamingMetrics().recordFrameCost(
                diag::msSince(queueBatchStart), 0.0, 0U);
        }
    }


    void processChunkStreaming() {
        if (!worldSessionActive)
            return;
        const auto position = camera.position();
        // 沿移动方向前探，使高速飞行的玩家永远碰不到已生成世界的边界
        // 请求中心比玩家超前约一秒的行程，工作线程围绕这个前探位置由近及远地生成
        // vanilla 靠玩家票据提前生成前方区块，从不因地形生成而卡住移动中的玩家
        // 这里是客户端侧的等价做法
        // 前探距离设了上限，保证玩家自己所在的区块仍在卸载半径之内
        const auto& playerSnap = clientMirror.player();
        const glm::vec2 velocity{
            playerSnap.physicsCurrent.x - playerSnap.physicsPrevious.x,
            playerSnap.physicsCurrent.z - playerSnap.physicsPrevious.z,
        };
        glm::vec3 requestPosition = position;
        // 只有移动才让请求中心前探
        // 原先按视线方向前探时，玩家原地转视角就能把中心挪出最多 0.4 倍视距
        // 整个流送窗口跟着绕圈，每转一次就卸载并重新加载一整圈
        // 那正是区块卸载卡顿的主因，FRAME_TRACE 实测站着转视角会产生 25–188ms 的同步卸载帧
        // 现在站着不动时中心就钉在玩家身上，转视角不触发任何流送
        // 加载半径内的四周本来就常驻，转过去也不会有缺口
        const float speed = glm::length(velocity);
        if (speed > 0.001F) {
            const float maxLead = std::max(
                0.0F, static_cast<float>(chunkStreamer.loadRadius() * world::kChunkWidth) - 8.0F);
            const float leadBlocks = std::min(speed * 20.0F, maxLead);
            const glm::vec2 direction = velocity / speed;
            requestPosition += glm::vec3{direction.x, 0.0F, direction.y} * leadBlocks;
        }
        const auto requestCenter =
            world::chunkPositionFromWorld(requestPosition.x, requestPosition.z);
        if (diag::traceEnabled()) {
            static int lastCenterX = std::numeric_limits<int>::min();
            static int lastCenterZ = std::numeric_limits<int>::min();
            diag::frameTrace().newCenterX = requestCenter.x;
            diag::frameTrace().newCenterZ = requestCenter.z;
            diag::frameTrace().centerChanged =
                requestCenter.x != lastCenterX || requestCenter.z != lastCenterZ;
            lastCenterX = requestCenter.x;
            lastCenterZ = requestCenter.z;
        }
        if (diag::chunkTraceEnabled()) {
            // 半径填充计时：请求中心一移动就（重新）起计
            // 中心没变则让已有的计时继续跑——它要么已经完成，要么上一次移动引发的填充还在追赶
            // 每帧都重新起计的话，慢速填充就永远观测不到
            const int radius = chunkStreamer.loadRadius();
            const bool epochChanged = traceArmedEpoch_ != worldEpoch;
            const bool centerMoved = !traceWindowValid_ ||
                                     requestCenter.x != traceArmedCenterX_ ||
                                     requestCenter.z != traceArmedCenterZ_;
            if (epochChanged || centerMoved) {
                const std::size_t expected =
                    static_cast<std::size_t>(2 * radius + 1) * static_cast<std::size_t>(2 * radius + 1);
                const auto enteredRadiusAt = diag::ChunkStreamingMetrics::Clock::now();
                diag::chunkStreamingMetrics().beginRadiusFill(
                    requestCenter.x, requestCenter.z, radius, expected, enteredRadiusAt);
                // 只为刚进入窗口的位置起计，为刚离开的撤销计时
                // 而不是每次移动都把整个 (2r+1)² 重新起计
                // 给已经可见、首次网格早已记录的区块重新起计会出问题
                // 它之后一次普通重网格会被误记成延迟长达数秒的"首次网格"，假长尾就是这么来的
                // 撤销计时同时清掉已记录标记，因此真正离开又重新进入的区块能干净地重新计一次
                //
                // 纪元切换也就是世界重置时，上一个窗口的计时与已记录标记都不再对应当前世界
                // 先全部撤销避免陈旧计时跨重置存活，再为新窗口起计
                const auto inNewWindow = [&](int x, int z) {
                    return std::abs(x - requestCenter.x) <= radius &&
                           std::abs(z - requestCenter.z) <= radius;
                };
                if (traceWindowValid_) {
                    for (int dz = -traceArmedRadius_; dz <= traceArmedRadius_; ++dz) {
                        for (int dx = -traceArmedRadius_; dx <= traceArmedRadius_; ++dx) {
                            const int x = traceArmedCenterX_ + dx;
                            const int z = traceArmedCenterZ_ + dz;
                            if (epochChanged || !inNewWindow(x, z)) {
                                diag::chunkStreamingMetrics().disarmFirstMesh({x, 0, z});
                            }
                        }
                    }
                }
                const auto inOldWindow = [&](int x, int z) {
                    return traceWindowValid_ && !epochChanged &&
                           std::abs(x - traceArmedCenterX_) <= traceArmedRadius_ &&
                           std::abs(z - traceArmedCenterZ_) <= traceArmedRadius_;
                };
                for (int dz = -radius; dz <= radius; ++dz) {
                    for (int dx = -radius; dx <= radius; ++dx) {
                        const int x = requestCenter.x + dx;
                        const int z = requestCenter.z + dz;
                        if (!inOldWindow(x, z)) {
                            diag::chunkStreamingMetrics().armFirstMesh({x, 0, z}, enteredRadiusAt);
                        }
                    }
                }
                traceWindowValid_ = true;
                traceArmedCenterX_ = requestCenter.x;
                traceArmedCenterZ_ = requestCenter.z;
                traceArmedRadius_ = radius;
                traceArmedEpoch_ = worldEpoch;
            }
        }
        chunkStreamer.request(requestCenter);
        while (auto batch = chunkStreamer.poll()) {
            queueStreamBatch(std::move(*batch));
        }
    }

    // 玩家移动绝不为地形生成阻塞，processChunkStreaming 已经让请求中心沿行进方向前探
    // 万一工作线程落后，PlayerController 的未加载列墙会把玩家原地挡住
    // 那是一次普通碰撞，不是卡顿
    // 在这里阻塞渲染线程正是边界处那次可见顿挫的成因，因此特意不再同步等待

    // 每次按钮点击都在监听者位置播放 vanilla 的 ui.button.click
    // 主音量分类下的点击声因此始终清晰可闻
    // 菜单按钮、创造页签以及所有背包/容器槽位都走这一个助手；拖拽（两个滑块和创造滚动条）不走

    void updateItemDrop() {
        if (!dropRequested) {
            return;
        }
        dropRequested = false;
        // Q 丢弃是一条命令：交互在服务端 tick 上取出选中物品堆并抛出，渲染器在这里不碰背包
        gameplay::DropSelected drop;
        drop.wholeStack = dropWholeStack;
        drop.lookDirection = camera.direction();
        enqueueClientCommand(std::move(drop));
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


    [[nodiscard]] AllocatedBuffer acquireStreamBuffer(StreamBufferPool& pool, VkDeviceSize bytes,
                                                      VkBufferUsageFlags usage, bool hostVisible) {
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

    // 给流式缓冲池做冷启动，第一波密集加载因此复用池中缓冲
    // 否则它每上传一个 section 就要在渲染线程上调一次 createBuffer
    // 那是一次 VMA 分配，在 MoltenVK 上还额外是一次 Metal 缓冲分配
    // 那是一次 VMA 分配，在 MoltenVK 上还额外是一次 Metal 缓冲分配
    // 首波的这场分配风暴正是区域流入时 MTL HUD 上那些小尖峰的来源
    // 池热起来之后同样的上传只是从空闲链表里弹一个，这也是加载结束后帧时间会稳下来的原因
    // 预热把这份一次性开销挪到会话启动时
    //
    // 只播种中间几档尺寸：典型 section 的顶点与索引缓冲落在 32 KiB 到 256 KiB
    // 每次上传要从暂存池与设备池各取一个顶点缓冲和一个索引缓冲
    // 暂存池是主机可见、用途为 TRANSFER_SRC 的，两个池因此都按各自消费者要求的用途与可见性预热
    // 空闲链表在复用时不再校验用途，所以这一点必须在预热时就对
    // 这个操作幂等，它只把每一档补到目标数量，池已经热了之后再进世界就是空操作
    // 常驻开销有界，远低于池的裁剪上限 kMaxStreamBufferPoolBytes
    // 约为 (32+64+128+256) KiB 乘 24 再乘 2 个池，合计约 23 MiB
    void prewarmStreamBufferPools() {
        static constexpr std::array<std::size_t, 4> kWarmClasses{1U, 2U, 3U, 4U}; // 32K..256K
        static constexpr std::size_t kPerClass = 24U;
        const auto warm = [&](StreamBufferPool& pool, VkBufferUsageFlags usage, bool hostVisible) {
            for (const std::size_t classIndex : kWarmClasses) {
                auto& freeList = pool.freeByClass[classIndex];
                const VkDeviceSize classBytes = kStreamBufferClassSizes[classIndex];
                while (freeList.size() < kPerClass) {
                    AllocatedBuffer buffer = createBuffer(classBytes, usage, hostVisible);
                    buffer.pooledSizeClass = static_cast<std::uint8_t>(classIndex + 1U);
                    pool.totalBytes += classBytes;
                    freeList.push_back(buffer);
                }
            }
        };
        warm(deviceBufferPool_, kStreamBufferDeviceUsage, false);
        warm(stagingBufferPool_, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    }

    // 立即归还空闲表；只有在设备空闲（世界重置）或该缓冲从未被提交时才安全

    void releaseStreamBufferNow(StreamBufferPool& pool, AllocatedBuffer& buffer) {
        if (buffer.pooledSizeClass == 0U) {
            destroyBuffer(buffer);
            return;
        }
        const std::size_t classIndex = buffer.pooledSizeClass - 1U;
        pool.freeByClass[classIndex].push_back(buffer);
        buffer = {};
    }

    // 保留 kFramesInFlight 帧；tickStreamBufferPool 在同槽围栏确认 GPU 用完之后归还它

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
        // 池溢出后把多余的空闲缓冲还给驱动，免得一次大爆发（传送、世界重置）把显存永久占住
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
        for (AllocatedBuffer* buffer : ownedBuffers(mesh)) {
            deferStreamBufferRelease(deviceBufferPool_, *buffer);
        }
        mesh = {};
    }


    [[nodiscard]] static VkDeviceSize meshByteSize(const MeshData& mesh) {
        return static_cast<VkDeviceSize>(mesh.vertices.size() * sizeof(VoxelVertex)) +
               static_cast<VkDeviceSize>(mesh.indices.size() * sizeof(std::uint32_t));
    }


    // RN-22：半透明层建好排序状态，并按当前视点排一次序。
    //
    // 排在渲染线程而不是网格化的工作线程里，与 vanilla（`SectionCompiler.compile` 收
    // 一个 `VertexSorting`）不同，理由是**单一调用点**：重排本来就只能在渲染线程做
    // （它要动 GPU 缓冲），把首次排序也放这里，"相机在哪"这件事就只有一处需要知道，
    // 不必把相机位置一路穿进 world/ChunkStreamer。代价是首次排序落在渲染线程上，但
    // 它与紧挨着的那次整块网格 memcpy 同量级，且共用同一个逐帧上传预算。
    void sortTranslucentIndices(const render::MeshData& source, GpuMesh& destination,
                                std::vector<std::uint32_t>& sorted) {
        destination.translucentSort = render::buildTranslucentSortState(source);
        if (destination.translucentSort.empty()) {
            // 前提不成立（或本来就没有半透明面）：原样上传，绘制顺序即发射顺序。
            sorted = source.indices;
            destination.translucentPointOfView = {};
            return;
        }
        const glm::vec3 eye = renderEyeState().position;
        destination.translucentPointOfView =
            render::translucencyPointOfViewOf(eye, destination.sectionCoordinates);
        render::writeSortedTranslucentIndices(destination.translucentSort,
                                              eye - destination.sectionOrigin, sorted);
    }


    void uploadRenderMesh(FrameContext& frame, const render::RenderMeshData& source,
                          GpuMesh& destination) {
        // 半透明的索引在这里就已经是排好序的那一份，长度与源相同。
        sortTranslucentIndices(source.translucentMesh, destination, translucentIndexScratch_);

        const std::array layers{&source.mesh, &source.cutoutMesh};
        VkDeviceSize vertexBytes = 0;
        VkDeviceSize indexBytes = 0;
        for (const auto* layer : layers) {
            vertexBytes += static_cast<VkDeviceSize>(layer->vertices.size() * sizeof(VoxelVertex));
            indexBytes += static_cast<VkDeviceSize>(layer->indices.size() * sizeof(std::uint32_t));
        }
        vertexBytes += static_cast<VkDeviceSize>(source.translucentMesh.vertices.size() *
                                                 sizeof(VoxelVertex));
        // RN-37：玻璃的阴影索引跟着 opaque/cutout 待在主索引缓冲里。它们指的是
        // 半透明层的顶点，所以**只有索引**要算进这条预算
        const VkDeviceSize translucentShadowIndexBytes = static_cast<VkDeviceSize>(
            source.translucentShadowIndices.size() * sizeof(std::uint32_t));
        indexBytes += translucentShadowIndexBytes;
        const VkDeviceSize translucentIndexBytes =
            static_cast<VkDeviceSize>(translucentIndexScratch_.size() * sizeof(std::uint32_t));
        auto vertexStaging = acquireStreamBuffer(stagingBufferPool_, vertexBytes,
                                                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        auto indexStaging = acquireStreamBuffer(stagingBufferPool_, indexBytes,
                                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        std::array<GpuMeshLayer*, 2> destinations{&destination.opaque, &destination.cutout};
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
        // 半透明的顶点仍与另外两层共用一条顶点缓冲（绘制时按 translucent.vertexOffset
        // 绑定），只有索引分了家；`indexOffset` 因此对它恒为 0。
        const VkDeviceSize translucentVertexBytes = static_cast<VkDeviceSize>(
            source.translucentMesh.vertices.size() * sizeof(VoxelVertex));
        destination.translucent.vertexOffset = vertexOffset;
        destination.translucent.indexOffset = 0;
        destination.translucent.indexCount =
            static_cast<std::uint32_t>(translucentIndexScratch_.size());
        // RN-37：同一批顶点，另一段索引。vertexOffset 与上面那一行**必须**相同，
        // 两者写在一起正是为了让它一眼可查——写错的症状是玻璃的影子长在别的方块上
        destination.translucentShadow.vertexOffset = vertexOffset;
        destination.translucentShadow.indexOffset = indexOffset;
        destination.translucentShadow.indexCount =
            static_cast<std::uint32_t>(source.translucentShadowIndices.size());
        if (translucentShadowIndexBytes > 0U) {
            std::memcpy(static_cast<std::byte*>(indexStaging.mapped) + indexOffset,
                        source.translucentShadowIndices.data(),
                        static_cast<std::size_t>(translucentShadowIndexBytes));
        }
        if (translucentVertexBytes > 0U) {
            std::memcpy(static_cast<std::byte*>(vertexStaging.mapped) + vertexOffset,
                        source.translucentMesh.vertices.data(),
                        static_cast<std::size_t>(translucentVertexBytes));
        }
        checkVk(vmaFlushAllocation(allocator, vertexStaging.allocation, 0, VK_WHOLE_SIZE),
                "vmaFlushAllocation(streaming vertices)");
        checkVk(vmaFlushAllocation(allocator, indexStaging.allocation, 0, VK_WHOLE_SIZE),
                "vmaFlushAllocation(streaming indices)");

        destination.vertexBuffer =
            acquireStreamBuffer(deviceBufferPool_, vertexBytes, kStreamBufferDeviceUsage, false);
        frame.uploadCopies.push_back(
            {vertexStaging.buffer, destination.vertexBuffer.buffer, vertexBytes});
        deferStreamBufferRelease(stagingBufferPool_, vertexStaging);
        if (indexBytes > 0U) {
            destination.indexBuffer = acquireStreamBuffer(deviceBufferPool_, indexBytes,
                                                          kStreamBufferDeviceUsage, false);
            frame.uploadCopies.push_back(
                {indexStaging.buffer, destination.indexBuffer.buffer, indexBytes});
        }
        deferStreamBufferRelease(stagingBufferPool_, indexStaging);
        if (translucentIndexBytes > 0U) {
            uploadTranslucentIndices(frame, destination, translucentIndexScratch_);
        }
    }


    // RN-22：把一份（重新）排好序的半透明索引送上 GPU，换掉这个 section 原来那条。
    //
    // 旧缓冲走延迟归还队列而不是就地覆写：见 `GpuMesh::translucentIndexBuffer` 的注释，
    // 就地覆写会与仍在读它的上一帧撞 WAR，而延迟归还本来就保证了 kFramesInFlight 帧。
    void uploadTranslucentIndices(FrameContext& frame, GpuMesh& mesh,
                                  const std::vector<std::uint32_t>& indices) {
        const auto bytes = static_cast<VkDeviceSize>(indices.size() * sizeof(std::uint32_t));
        auto staging =
            acquireStreamBuffer(stagingBufferPool_, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        std::memcpy(staging.mapped, indices.data(), static_cast<std::size_t>(bytes));
        checkVk(vmaFlushAllocation(allocator, staging.allocation, 0, VK_WHOLE_SIZE),
                "vmaFlushAllocation(translucent indices)");
        deferStreamBufferRelease(deviceBufferPool_, mesh.translucentIndexBuffer);
        mesh.translucentIndexBuffer =
            acquireStreamBuffer(deviceBufferPool_, bytes, kStreamBufferDeviceUsage, false);
        frame.uploadCopies.push_back({staging.buffer, mesh.translucentIndexBuffer.buffer, bytes});
        deferStreamBufferRelease(stagingBufferPool_, staging);
    }


    void prepareStreamingUpdates(FrameContext& frame) {
        uploadedSectionsThisFrame = 0;
        uploadedBytesThisFrame = 0;
        std::size_t processedUpdates = 0;
        std::size_t priorityUploads = 0;
        constexpr std::size_t kMaxRemovalsPerFrame = 256;
        const bool chunkTrace = diag::chunkTraceEnabled();
        // 只在开启追踪时取时钟，因为本函数每帧都跑
        // queueStreamBatch 每批次才跑一次，为何不必门控见那里的说明
        const auto uploadPrepStart =
            chunkTrace ? diag::ChunkStreamingMetrics::Clock::now() : diag::ChunkStreamingMetrics::Clock::time_point{};
        std::size_t tracedUploads = 0;

        while (!pendingSectionOrder.empty()) {
            const world::SectionPosition position = pendingSectionOrder.front();
            const auto found = pendingSectionUpdates.find(position);
            if (found == pendingSectionUpdates.end()) {
                pendingSectionOrder.popFront();
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
                // 编辑走另一个带上限的桶，不占流送的 section/字节预算，因此当帧就能落地
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
            pendingSectionOrder.popFront();
            ++processedUpdates;
            // 取出入队时记录的环号、中心、纪元与类型，对应真正请求该 section 的那个批次中心
            // 无论这个 section 最终是否上传网格都把它从旁表里删掉
            // 诊断表因此不会比它对应的待处理条目活得更久
            PendingSectionTrace enqueueTrace{};
            if (chunkTrace) {
                const auto ringFound = pendingSectionEnqueueRing_.find(position);
                if (ringFound != pendingSectionEnqueueRing_.end()) {
                    enqueueTrace = ringFound->second;
                    pendingSectionEnqueueRing_.erase(ringFound);
                }
            }
            const auto existing = gpuMeshes.find(position);
            if (existing != gpuMeshes.end()) {
                retireMesh(frame, existing->second);
                gpuMeshes.erase(existing);
            }
            occlusionStates.erase(position);
            occlusionMissCount.erase(position);
            if (!uploadsMesh) {
                if (chunkTrace && !update.remove) {
                    // 确认为空的 section 是一种合法结果而非缺口
                    // 跳过空 section 时仍会为它投递一条 remove 更新
                    // 因积压上限被淘汰、从未到达 GPU 的 section 会另行重新请求重网格
                    // 它在这里保持未决状态
                    diag::missingChunkDetector().noteChunkResolved(position.chunkX,
                                                                    position.chunkZ);
                }
                chunkStreamer.releaseMeshData(std::move(update.mesh));
                continue;
            }

            GpuMesh gpuMesh;
            gpuMesh.bounds = update.mesh.bounds;
            gpuMesh.sectionOrigin = {static_cast<float>(position.chunkX) * world::kChunkWidth,
                                     static_cast<float>(world::sectionOriginY(position.sectionY)),
                                     static_cast<float>(position.chunkZ) * world::kChunkDepth};
            // RN-22：section 号，用于半透明重排的象限量化。sectionOrigin 恒是 16 的
            // 倍数，但 y 可以是负的（kMinY < 0），所以是 floor 除不是截断除。
            gpuMesh.sectionCoordinates = {
                static_cast<int>(std::floor(gpuMesh.sectionOrigin.x / 16.0F)),
                static_cast<int>(std::floor(gpuMesh.sectionOrigin.y / 16.0F)),
                static_cast<int>(std::floor(gpuMesh.sectionOrigin.z / 16.0F))};
            uploadRenderMesh(frame, update.mesh, gpuMesh);
            // 工作线程把这个网格建在池化的 RenderMeshData 上；归还它，容量供下一个 section 构建复用
            chunkStreamer.releaseMeshData(std::move(update.mesh));
            gpuMeshes.insert_or_assign(position, std::move(gpuMesh));
            // 新网格必须先画一次并查询过，遮挡结果才可信，因此它从 Unknown 起步，不继承陈旧结果
            occlusionStates[position] = OcclusionState::Unknown;
            if (chunkTrace) {
                const auto uploadedAt = diag::ChunkStreamingMetrics::Clock::now();
                diag::chunkStreamingMetrics().recordFirstMesh(
                    {position.chunkX, 0, position.chunkZ}, uploadedAt);
                diag::missingChunkDetector().noteChunkResolved(position.chunkX, position.chunkZ);
                diag::deliveryOrderTrace().record(
                    position.chunkX, position.chunkZ, enqueueTrace.ring, enqueueTrace.centerX,
                    enqueueTrace.centerZ, enqueueTrace.epoch, enqueueTrace.type);
                ++tracedUploads;
            }
            if (priority) {
                ++priorityUploads;
            } else {
                ++uploadedSectionsThisFrame;
                uploadedBytesThisFrame += updateBytes;
            }
            totalUploadedBytes += updateBytes;
        }
        if (chunkTrace && tracedUploads > 0U) {
            // 逐帧流送耗时的后一半，即本帧从待处理积压中分担的 GPU 上传准备开销
            // 含暂存拷贝与缓冲获取，与 queueStreamBatch 记录的批次落地那一半配对
            diag::chunkStreamingMetrics().recordFrameCost(
                0.0, diag::msSince(uploadPrepStart), tracedUploads);
        }
        scheduleTranslucentResorts(frame);
    }


    // RN-22：半透明层的逐 quad 重排调度，26.1 `LevelRenderer.scheduleTranslucentSectionResort`
    // （LevelRenderer.java:997）的形状。
    //
    // 它必须跑在 renderpass 之外 —— 重排要发 `vkCmdCopyBuffer`，而拷贝是在
    // `recordUpload` 里落到指令缓冲的，那一步在任何 renderpass 开始之前。所以调度挂在
    // `prepareStreamingUpdates` 尾巴上，而不是挂在 drawWorld 里那次 section 排序旁边。
    //
    // 视点取 `renderEyeState().position` 而不是 `camera.position()`：第三人称下渲染眼
    // 点被沿视线拉后 4 格，混合顺序该按真正出图的那个眼点算。视锥与遮挡已经统一用它
    // （见 drawWorld 里建 Frustum 那段）。vanilla 传的 `camera.position()` 本身就是
    // 渲染眼点，所以这也是对齐而不是分歧。
    void scheduleTranslucentResorts(FrameContext& frame) {
        const bool trace = diag::traceEnabled();
        const auto resortStart =
            trace ? diag::FrameTrace::Clock::now() : diag::FrameTrace::Clock::time_point{};
        const glm::vec3 eye = renderEyeState().position;
        const glm::ivec3 eyeBlock{static_cast<int>(std::floor(eye.x)),
                                  static_cast<int>(std::floor(eye.y)),
                                  static_cast<int>(std::floor(eye.z))};
        const bool cameraBlockChanged =
            !translucentResortInitialized_ || eyeBlock != lastTranslucentResortBlock_;
        translucentResortInitialized_ = true;
        lastTranslucentResortBlock_ = eyeBlock;

        // 有半透明几何的 section 名单。逐帧重建，因为 gpuMeshes 每帧都在增删。
        // 名单与调度输入分成两个数组：调度是纯函数，它只认 section 号与上次的象限，
        // 不认 GpuMesh —— 那正是它能被 benchmark 与测试拿同一份代码驱动的原因。
        translucentSections_.clear();
        translucentCandidates_.clear();
        for (auto& [position, mesh] : gpuMeshes) {
            if (mesh.translucentSort.empty()) {
                continue;
            }
            translucentSections_.push_back(&mesh);
            translucentCandidates_.push_back({mesh.sectionCoordinates, mesh.translucentPointOfView});
        }
        if (trace) {
            diag::frameTrace().translucentSections =
                static_cast<std::uint32_t>(translucentSections_.size());
        }
        if (translucentSections_.empty()) {
            translucentResortCursor_ = 0;
            return;
        }

        render::selectTranslucentResorts(
            std::span<const render::TranslucentResortCandidate>{translucentCandidates_}, eye,
            cameraBlockChanged, translucentResortCursor_, translucentResortSelection_);

        for (const std::size_t index : translucentResortSelection_) {
            GpuMesh& mesh = *translucentSections_[index];
            render::writeSortedTranslucentIndices(
                mesh.translucentSort, eye - mesh.sectionOrigin, translucentIndexScratch_);
            uploadTranslucentIndices(frame, mesh, translucentIndexScratch_);
            mesh.translucentPointOfView =
                render::translucencyPointOfViewOf(eye, mesh.sectionCoordinates);
        }
        const std::size_t resorted = translucentResortSelection_.size();
        translucentResortsThisFrame_ = resorted;
        if (trace) {
            diag::frameTrace().translucentResorts = static_cast<std::uint32_t>(resorted);
            diag::frameTrace().translucentResortMs += diag::msSince(resortStart);
        }
    }


    // 矩阵的构造整体搬进 render/SunShadowMap.hpp——深度约定、texel snapping 与正交框
    // 尺寸都在那里，投射者的排序键也在那里读同一批常量。这里只剩「用哪个太阳、哪个视点」。
    //
    // 视点用 renderEyeState() 而不是 camera.position()：相机对象始终在玩家眼睛处，
    // 第三人称把渲染眼点沿视线拉后 4 格。用相机位置会让 128 格的光锥中心停在玩家身上
    // 而不是画面中心，第三人称下光锥因此偏心，画面前方约 4 格宽的一条带子落在框外、
    // 完全没有阴影。视图矩阵与剔除视锥都已经统一用渲染眼点（renderViewMatrix），
    // 阴影是最后一个还在用相机位置的消费者。
    void updateShadowMatrix() {
        if (shadowDisabled) {
            return;
        }
        // 阴影用的是**量化到角度步长**的 tick，不是真实 tick（RN-24，量化规则与步长的
        // 依据都在 render/SunShadowMap.hpp）。着色用的太阳仍取真实 tick，见那里对这条
        // 有意不一致的说明。
        const auto daylight = world::DayNightCycle::stateAtTick(
            sunShadowSunTick(clientMirror.world().dayTimeTicks));
        // 排序键要用同一帧的同一个太阳，因此在这里存下来给 recordShadow 用，
        // 而不是让它自己再取一次 dayTimeTicks——两次取之间跨了 tick 就会错开一帧
        shadowSunDirection_ = glm::normalize(daylight.sunDirection);
        // RN-35：逐级各算一份。两级共用同一个旋转与同一个深度范围，只有横向半边长
        // 与吸附步长不同——「两级是同一个太阳投的」因此是结构性的
        for (std::size_t cascade = 0; cascade < kSunShadowCascadeCount; ++cascade) {
            shadowLightViewProj[cascade] =
                sunShadowLightViewProj(shadowSunDirection_, renderEyeState().position, cascade,
                                       // RN-47：玩家可调的那一档。越界的值在
                                       // sunShadowOrthoHalfExtent 里收口到默认档
                                       options.shadowNearDistance);
        }
    }


    // 阴影预通道的 body（RN-20a）。renderpass 的 begin/end、清空值与渲染区域由 frame
    // graph 给（见 VulkanRenderer 的 rebuildFrameGraph），这里只剩「挑投射者、画它们」。
    //
    // 「即使一个投射者都没有也照样 begin/end」那条契约仍在，只是换了承载者：图里这一步
    // 整步存在，深度图因此每帧都被清空、并由图在世界那步前的边界屏障转成
    // SHADER_READ_ONLY_OPTIMAL。关掉太阳阴影时整步在**编译期**被剪掉（连同那条屏障），
    // 不是在这里 return；那张图靠 OffscreenTarget::initializeAsShaderRead 留下的布局保持合法。
    // RN-35：一趟画一级。两级各有自己的正交框、自己的帧缓冲层、自己的投射者集合——
    // 「哪些 section 挡得住光」在 16 格框和 128 格框下不是同一批。
    void recordShadow(FrameContext& frame, std::size_t cascade) {
        // 候选只收**能挡光**的 section。从前这个判断在下面的绘制循环里，于是
        // 空 opaque 的 section 白占 512 个名额里的位置：选进来、排了序、然后 continue。
        //
        // 「能挡光」是 opaque **或** cutout：Cutout 桶在本作装的不只是草和树叶，还有
        // 楼梯、墙、栅栏、门、活板门这些实心材质的异形方块。只画 opaque 的时候，
        // 一段楼梯在太阳底下不投任何影子，而挨着它的台阶（Opaque 桶）投。
        shadowCasterMeshes_.clear();
        shadowCasterBounds_.clear();
        for (const auto& [position, mesh] : gpuMeshes) {
            static_cast<void>(position);
            // RN-37：玻璃只在 translucentShadow 那一层里投影，纯玻璃的 section
            // 三层里只有它非空——漏掉这一条，那些 section 连候选都进不来
            if (mesh.opaque.indexCount == 0U && mesh.cutout.indexCount == 0U &&
                mesh.translucentShadow.indexCount == 0U) {
                continue;
            }
            shadowCasterMeshes_.push_back(&mesh);
            shadowCasterBounds_.push_back(mesh.bounds);
        }
        // 光锥剔除 + 按光源空间深度截断到 kMaxSunShadowCasters（见 SunShadowMap.hpp）。
        // 三个 vector 都是成员，clear() 保留容量，因此稳态下逐帧零分配。
        selectSunShadowSceneCasters(shadowLightViewProj[cascade], shadowSunDirection_,
            shadowCasterBounds_, entityDraws_.casters, shadowCasterSelection_,
            shadowEntitySelection_);
        VkViewport viewport{};
        viewport.width = static_cast<float>(shadowTarget.width());
        viewport.height = static_cast<float>(shadowTarget.height());
        viewport.maxDepth = 1.0F;
        vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
        VkRect2D scissor{{0, 0}, {shadowTarget.width(), shadowTarget.height()}};
        vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
        // 一层几何一次管线切换，而不是逐 section 在两条管线之间来回跳。
        // RN-37：一次绑定可以画多层。镂空地形与玻璃的阴影几何走的是**同一条**管线
        // （同一个顶点程序、同一次 alpha 测试），分两次调用就是白切一次管线
        const auto recordShadowLayer = [&](VkPipeline pipeline, VkPipelineLayout layout,
                                           std::initializer_list<GpuMeshLayer GpuMesh::*> layers) {
            vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            if (layout == pipelines.shadowCutoutPipelineLayout) {
                vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        layout, 0, 1, &frame.descriptorSet, 0, nullptr);
            }
            for (const std::size_t index : shadowCasterSelection_) {
                const GpuMesh* mesh = shadowCasterMeshes_[index];
                for (const auto layer : layers) {
                const GpuMeshLayer& draw = mesh->*layer;
                // 选进来的 section 只保证**至少一层**非空，每一层都要各自再问一次。
                if (draw.indexCount == 0U) {
                    continue;
                }
                // RN-51：`.w` 标记「这一趟画的是薄投射者」。着色器只读 `.xyz` 当位置，
                // 这个分量一直是未赋义的 1.0——现在它是玻璃那一层的旗子，
                // 侧对光的玻璃面因此不再往阴影图里渲那条断续的发丝影
                const float thinCaster = layer == &GpuMesh::translucentShadow ? 1.0F : 0.0F;
                const ShadowPush push{shadowLightViewProj[cascade],
                                      glm::vec4{mesh->sectionOrigin, thinCaster}};
                vkCmdPushConstants(frame.commandBuffer, layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                                   sizeof(push), &push);
                vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &mesh->vertexBuffer.buffer,
                                       &draw.vertexOffset);
                vkCmdBindIndexBuffer(frame.commandBuffer, mesh->indexBuffer.buffer,
                                     draw.indexOffset, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(frame.commandBuffer, draw.indexCount, 1, 0, 0, 0);
                }
            }
        };
        recordShadowLayer(pipelines.shadowPipeline, pipelines.shadowPipelineLayout,
                          {&GpuMesh::opaque});
        // 玻璃与镂空地形一起画：同一条管线、同一次 alpha 测试，玻璃的阴影因此恰好
        // 只剩纹理里不透明的那一圈边框。
        //
        // ★ RN-50：**只进近段**。玻璃边框宽 1/16 格，而远段一个纹素正好也是 1/16 格——
        // 一个恰好等于采样间距的投射者，光栅化出来的是噪声不是信号：它随纹素中心落在
        // 边框内外而通断，实机上就是「阴影线条上的光斑」（用户 2026-09-09 两次报到）。
        // 离屏实测：同一堵玻璃墙，远段只画得出 68 个影子像素，近段是 405 个。
        //
        // 近段的纹素在三档设置下分别是 1/128、1/64、1/42.7 格，也就是边框宽度的
        // 8 / 4 / 2.7 倍——都撑得住。所以规则是「撑得住的那一级才画」，
        // 而不是「哪一级都画一遍」。代价是近段框之外玻璃不投影：**没有影子**比
        // 一串闪烁的光斑好，而且远段那一趟还省了这一层。
        recordShadowLayer(pipelines.shadowCutoutPipeline, pipelines.shadowCutoutPipelineLayout,
                          cascade == 0 ? std::initializer_list<GpuMeshLayer GpuMesh::*>{
                                             &GpuMesh::cutout, &GpuMesh::translucentShadow}
                                       : std::initializer_list<GpuMeshLayer GpuMesh::*>{
                                             &GpuMesh::cutout});
        // 实体的矩阵走 UBO 而不是 push constant（ItemPush 正好满 128 字节），级别因此
        // 是一个特化常量——每级一条管线，见 item_entity.vert 的 sunShadowCascade
        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pipelines.entityShadowPipelines[cascade]);
        vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipelines.entityShadowPipelineLayout, 0, 1, &frame.descriptorSet, 0, nullptr);
        for (const std::size_t index : shadowEntitySelection_) {
            const auto& caster = entityDraws_.casters[index];
            for (std::size_t i = caster.firstDraw; i < caster.firstDraw + caster.drawCount; ++i) {
                const auto& draw = entityDraws_.draws[i];
                vkCmdPushConstants(frame.commandBuffer, pipelines.entityShadowPipelineLayout,
                    VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(draw.push), &draw.push);
                vkCmdDraw(frame.commandBuffer, draw.vertexCount, 1, draw.firstVertex, 0);
            }
        }
        if (!diagnosticsOnce_.shadowCasters && !shadowCasterSelection_.empty()) {
            diagnosticsOnce_.shadowCasters = true;
            std::cout << "[shadow] cascade " << cascade << " pre-pass "
                      << shadowCasterSelection_.size() << " casters\n";
        }
    }

    void drawCollectedEntities(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet) const {
        const glm::mat4 cameraView = viewBobbingMatrix() * renderViewMatrix();
        VkPipeline bound = VK_NULL_HANDLE;
        for (const auto& caster : entityDraws_.casters) {
            if (caster.kind == ShadowEntityKind::Player &&
                cameraPerspective == CameraPerspective::FirstPerson) continue;
            const VkPipeline pipeline = caster.kind == ShadowEntityKind::Decal
                ? pipelines.itemShadowPipeline : pipelines.itemPipeline;
            if (bound != pipeline) {
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pipelines.itemPipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
                bound = pipeline;
            }
            for (std::size_t i = caster.firstDraw; i < caster.firstDraw + caster.drawCount; ++i) {
                const auto& draw = entityDraws_.draws[i];
                ItemPush push = draw.push;
                if (push.data.x == kItemModeGeneratedItem)
                    push.viewModelTransform = cameraView * push.viewModelTransform;
                vkCmdPushConstants(commandBuffer, pipelines.itemPipelineLayout,
                    VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
                vkCmdDraw(commandBuffer, draw.vertexCount, 1, draw.firstVertex, 0);
            }
        }
    }

    // 仅调试用的叠加层，由 MC_REBEDROCK_SHADOW_DEBUG=1 打开
    // 它把阴影深度纹理采样到右上角的一个四边形里，好让预通道的输出可见

    void drawShadowDebugOverlay(VkCommandBuffer commandBuffer) const {
        if (!shadowDebugOverlay || shadowDisabled) {
            return;
        }
        const float width = static_cast<float>(swapchainExtent.width);
        const float height = static_cast<float>(swapchainExtent.height);
        constexpr float kSize = 256.0F;
        const glm::vec4 rect{
            1.0F - 2.0F * kSize / width,
            1.0F - 2.0F * kSize / height,
            2.0F * kSize / width,
            2.0F * kSize / height,
        };
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.shadowDebugPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelines.shadowDebugPipelineLayout, 0, 1, &shadowDebugSet, 0, nullptr);
        vkCmdPushConstants(commandBuffer, pipelines.shadowDebugPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(rect), &rect);
        vkCmdDraw(commandBuffer, 6U, 1, 0, 0);
    }

    // RN-23：每一类实体的圆形阴影贴花。
    //
    // 它是一个独立的收集器，不是挂在各个实体收集器里的一段，有两个理由。
    // 一是玩家：collectWorldPlayer 在第一人称且太阳阴影关闭时整个早退（那种情况下它
    // 没有颜色几何要交），贴花挂在那里就会跟着一起消失，而那正是绝大多数玩家的常态。
    // 二是这五类实体在这里只需要同样的三样东西——一个位置、一个半径、一个强度——放在
    // 一起，「谁有影子、半径多少、出处在哪」就是一张能一眼读完的表，而不是散在四个
    // 函数里的四段相同代码。
    //
    // 第一人称的本机玩家**没有**贴花，这与 26.1 一致：LevelRenderer.java:797 的
    // `entity != camera.entity() || camera.isDetached() || 睡着` 把第一人称下自己
    // 的实体整个挡在渲染列表之外，连 extractShadow 都不会跑。注意这与太阳阴影开启时
    // 的行为**不对称**：那条路径（RN-11b）有意让第一人称仍然提交完整的身体投影，只
    // 跳过颜色绘制。两条路径各自对齐了不同的参照，这个不对称是两次有意判断叠加的
    // 结果，不是漏了一处。
    void collectEntityShadowDecals() {
        // RN-11b 的判断保留并扩展到全部实体：太阳阴影开着时统一不画贴花，而不是按
        // 单只实体是否入选真实投影临时恢复——那会让一只实体在预算边缘反复闪变。
        if (!worldReady || !options.entityShadows || !drawEntityShadowDecal(!shadowDisabled)) {
            return;
        }
        const glm::vec3 eye = renderEyeState().position;
        // 天光衰减取 SkyLight，本仓这条公式的单一源（RN-12 为暗角建的）。夜里露天
        // 地面因此是亮度 4，影子淡到几乎看不见，而不是和正午一样浓。
        const int skyDarken = SkyLight::skyDarken(clientMirror.world().dayTimeTicks);
        // 一格问一次世界：这一格自己的光照，加上它**下面**那一格的方块状态。
        const auto sample = [this](int x, int y, int z) {
            return render::EntityShadowCell{
                clientCache.state(x, y - 1, z),
                static_cast<int>(clientCache.skyLight(x, y, z)),
                static_cast<int>(clientCache.blockLight(x, y, z)),
            };
        };
        // 本帧全部贴花共用一个 Decal 投射者。Decal 不参与太阳阴影（castsSunShadow
        // 把它排除在外），所以这里的"一个投射者"不是"一只实体"，只是一段连续的、
        // 用同一条管线画的绘制——合成一段就少一次管线切换。
        bool begun = false;
        const auto emitDecal = [&](const glm::vec3& position, float radius, float strength) {
            const render::EntityShadowInput input{
                position, radius, strength, glm::dot(position - eye, position - eye), skyDarken};
            render::collectEntityShadowPieces(input, sample, shadowDecalPieces_);
            const float discRadius = std::min(radius, render::kMaxEntityShadowRadius);
            for (const auto& piece : shadowDecalPieces_) {
                // 全透明的一片只是填充率：足迹的四个角落几乎总是落在圆盘之外。
                if (piece.alpha <= 0.004F) {
                    continue;
                }
                const auto uv = render::entityShadowPieceUv(piece, discRadius);
                if (!begun) {
                    entityDraws_.begin(ShadowEntityKind::Decal);
                    begun = true;
                }
                entityDraws_.append(
                    ItemPush{
                        // 抬起千分之三格：贴花与方块顶面共面就会 z-fighting。
                        // vanilla 靠 RenderType 的 polygon offset，本作这条管线不写
                        // 深度、也没有 offset 状态，所以抬升是它的等价物。
                        {position.x + piece.relativeX,
                         position.y + piece.relativeY + 0.003F,
                         position.z + piece.relativeZ, piece.sizeX},
                        {uv.u0, uv.v0, uv.u1, uv.v1},
                        {kItemModeEntityShadow, piece.alpha, 0.0F, 0.0F},
                        {piece.sizeZ, 0.0F, 0.0F, 0.0F},
                    },
                    6U, 0U);
            }
        };

        // 本机玩家：AvatarRenderer.java:50 的 0.5。第三人称才画，理由见上。
        if (cameraPerspective != CameraPerspective::FirstPerson) {
            const auto& player = clientMirror.player();
            emitDecal(player.physicsPrevious +
                          (player.physicsCurrent - player.physicsPrevious) * renderInterpolationAlpha,
                      0.5F, 1.0F);
        }

        // 生物：半径逐物种声明在它自己的 EntityRenderDescriptor 上，不是这里的一张
        // switch。没声明的物种半径为 0，一片也不出——那是 EntityRenderer 自己的默认。
        {
            const auto& snapshot = clientMirror.entities();
            for (const auto& entity : snapshot.entities()) {
                if (entity.type == nullptr) {
                    continue;
                }
                emitDecal(entity.previousPosition +
                              (entity.position - entity.previousPosition) * renderInterpolationAlpha,
                          entity.type->render().shadowRadius, 1.0F);
            }
        }

        // 掉落物、经验球、下落方块。
        // 这里自己取一次 entityRenderFrame，而 collectItemEntities 取的是另一次：两次
        // 相隔几微秒，最坏情况下贴花与它的掉落物差一个插值步。掉落物一 tick 走不了几
        // 厘米，所以这点错位看不见；把两个收集器捆在同一次读取上要改 collectItemEntities
        // 的签名和它的源码护栏，代价大于收益，记账在此。
        {
            const auto frame = clientMirror.entityRenderFrame();
            const float alpha = frame.alpha;
            const auto interpolated = [alpha](const auto& entity) {
                return entity.previousPosition +
                       (entity.position - entity.previousPosition) * alpha;
            };
            // ItemEntityRenderer.java:27-28 和 ExperienceOrbRenderer.java:22-23 都是
            // 0.15 半径、0.75 强度：小东西的影子既小又淡。
            for (const auto& entity : frame.snapshot.items()) {
                emitDecal(interpolated(entity), 0.15F, 0.75F);
            }
            for (const auto& orb : frame.snapshot.experienceOrbs()) {
                emitDecal(interpolated(orb), 0.15F, 0.75F);
            }
            // FallingBlockRenderer.java:17
            for (const auto& falling : frame.snapshot.fallingBlocks()) {
                emitDecal(interpolated(falling), 0.5F, 1.0F);
            }
        }
    }

    void collectItemEntities() {
        // 两者都读逐 tick 快照，理由和生物一样：实时容器归模拟侧所有
        // 先把按值返回的快照绑到局部变量——绑定到按值返回对象的成员引用并不会延长其生命周期
        //
        // 快照与插值系数必须取自同一次 entityRenderFrame 打包读取
        // 掉落物的两个端点和混合它们的系数若来自不同发布就会出问题
        // 一次恰好落在两次读取之间的 tick 会让系数比端点晚一拍，掉落物随之抖动
        // 这里不使用全帧通用的 renderInterpolationAlpha
        const auto entityFrame = clientMirror.entityRenderFrame();
        const auto& snapshot = entityFrame.snapshot;
        const float itemAlpha = entityFrame.alpha;
        const auto& snapshotItems = snapshot.items();
        const auto& snapshotFallingBlocks = snapshot.fallingBlocks();
        const auto& snapshotOrbs = snapshot.experienceOrbs();
        if (snapshotItems.empty() && snapshotFallingBlocks.empty() && snapshotOrbs.empty()) {
            return;
        }
        // 贴花从前只有掉落物有，而且是「沿着中心那一列往下找地面、画一个固定 0.15 的
        // 圆盘」——生物和玩家一片影子也没有。它已整条移到 collectEntityShadowDecals()，
        // 那里按 26.1 的逐格采集给**每一类**实体出片。
        // 手持物生成的 2.5D 薄片顶点数，对应 item_entity.vert 的 data.x 落在 (6.5,7.5) 模式
        // 计为正反面 12 个顶点，加上 16x16 的边缘四边形
        constexpr std::uint32_t kGeneratedItemVertexCount = 12U + 16U * 16U * 4U * 6U;
        // 普通掉落物存世界矩阵；颜色提交时才乘 view，深度通道直接使用世界矩阵。
        for (const auto& entity : snapshotItems) {
            entityDraws_.begin(ShadowEntityKind::Item);
            const glm::vec3 renderedPosition =
                entity.previousPosition +
                (entity.position - entity.previousPosition) * itemAlpha;
            // 画方块模型还是扁平的 2.5D 图标，由 world::rendersAsModelItem 单点回答
            // 掉落物、手持物、背包图标三条物品渲染面共用它
            // 这里曾各自手写 `model == Cube || ...`，三处口径还不一样：
            // 侦测器（DirectionalCube）只补进了手持与图标，掉在地上就退化成扁平贴图
            //
            // RN-14：那个单点此前只有「立方体 / 扁平贴图」两个答案，台阶还得靠一个
            // 把盒子 Y 向压半的特例。现在它给出的是一份**盒子表**，台阶只是其中
            // 一个 (0,0,0)-(16,8,16) 的盒子，楼梯是两个，栅栏门是八个。
            const bool modelItem = gameplay::isBlockStack(entity.stack) &&
                                   world::rendersAsModelItem(entity.stack.block);
            // RN-8c-D: every layer of a block item comes from
            // world::cubeItemLayers, NOT from the flat top/side/bottom triple.
            // The atlas baker deliberately puts a DirectionalCube's FRONT in that
            // triple's `side` slot, so that a three-slot item cube would at least
            // be recognisable as a furnace; reading it here as a real side put the
            // front on three faces at once.
            const world::CubeItemLayers itemFaces =
                modelItem ? cubeItemCubeLayers(entity.stack.block)
                          : world::CubeItemLayers{};
            const float previousAge =
                entity.ageTicks == 0U ? 0.0F : static_cast<float>(entity.ageTicks - 1U);
            const float age = previousAge + itemAlpha;
            // 漂浮与旋转由动画库的展示实体预设驱动（Molang 编写的曲线）
            const auto motion = itemDisplayAnimation.at(age, entity.visualPhase);
            const float bob = motion.bobHeight;
            const float rotation = motion.yawRadians;
            // 上抬半格：掉落物贴地放置，它自己的 y 会取整落进脚下那个方块里
            const float packedLight =
                packedSceneLight(renderedPosition + glm::vec3{0.0F, 0.5F, 0.0F});
            if (modelItem) {
                // RN-14：物品模型的每个盒子的每个已声明的面各一次绘制（着色器模式 10）
                // 盒中心在模型空间里的偏移由 CPU 先按同一个 yaw 旋好再加到世界位置上，
                // 着色器那边一个字没改；面的 uv rect 与 `rotation` 象限逐次推过去，
                // 于是图标、掉落物、手持物三面读的是同一张 world::ItemModel 表。
                //
                // 只画模型声明了的面：楼梯上层台阶没有 down 面（vanilla 也没有），
                // 硬画出来会与下层盒子的顶面共面而闪烁。
                constexpr float kDropScale = 0.30F;
                const glm::vec3 centre{renderedPosition.x, renderedPosition.y + 0.18F + bob,
                                       renderedPosition.z};
                const float yawCosine = std::cos(rotation);
                const float yawSine = std::sin(rotation);
                const auto range = world::itemModelRange(entity.stack.block);
                for (std::size_t b = 0; b < range.count; ++b) {
                    const world::ItemModelBox& box =
                        world::kItemModelBoxes[static_cast<std::size_t>(range.first) + b];
                    const glm::vec3 size = (box.to16 - box.from16) * (kDropScale / 16.0F);
                    const glm::vec3 offset =
                        ((box.from16 + box.to16) * 0.5F / 16.0F - glm::vec3{0.5F}) * kDropScale;
                    // 与着色器里的 yawRotation 同一个约定（axisMatrix('y')）
                    const glm::vec3 turned{yawCosine * offset.x + yawSine * offset.z, offset.y,
                                           -yawSine * offset.x + yawCosine * offset.z};
                    const glm::vec3 boxCentre = centre + turned;
                    for (std::uint32_t f = 0; f < world::kFaces.size(); ++f) {
                        const auto facing =
                            static_cast<std::size_t>(world::bakeFacingOf(world::kFaces[f].face));
                        const world::ItemModelFace& face = box.face[facing];
                        if (!face.present) {
                            continue;
                        }
                        // Assembled by makeDroppedBlockItemFacePush, not here.
                        // The held path below assembles its own and filled the
                        // face's UV rect into different fields; every held block
                        // then stretched one column of texels over itself.
                        const ItemPush push = makeDroppedBlockItemFacePush(
                            face, world::itemFaceLayer(itemFaces, face.slot), boxCentre, size,
                            rotation, packedLight);
                        entityDraws_.append(push, 6U, f * 6U);
                    }
                }
            } else {
                // 非方块物品与手持物共用同一套单层 3D 模型，而不是面向相机的平面公告板
                // 物品图标做成带挤出边缘的薄片绕 Y 轴旋转
                // 这与 vanilla 在 GROUND 变换下绘制同一个物品模型的方式一致
                glm::mat4 dropTransform{1.0F};
                dropTransform = glm::translate(
                    dropTransform,
                    {renderedPosition.x, renderedPosition.y + 0.18F + bob, renderedPosition.z});
                dropTransform = glm::rotate(dropTransform, rotation, {0.0F, 1.0F, 0.0F});
                dropTransform = glm::scale(dropTransform, glm::vec3{0.30F});
                const float spriteLayer = gameplay::itemTextureLayer(entity.stack);
                const ItemPush push{
                    {0.0F, 0.0F, 0.0F, 0.30F},  {spriteLayer, spriteLayer, spriteLayer, 0.0F},
                    {kItemModeGeneratedItem, 0.0F, 0.0F, 0.0F},
                    {1.0F, 1.0F, 0.0625F, packedLight},
                    dropTransform,
                };
                entityDraws_.append(push, kGeneratedItemVertexCount, 0);
            }
        }
        for (const auto& entity : snapshotFallingBlocks) {
            entityDraws_.begin(ShadowEntityKind::FallingBlock);
            const glm::vec3 renderedPosition =
                entity.previousPosition +
                (entity.position - entity.previousPosition) * itemAlpha;
            const auto layers = world::textureLayers(entity.block);
            // 下落方块以自身位置为中心绘制，它穿过的那格是空气，直接采样即可
            const ItemPush push{
                {renderedPosition.x, renderedPosition.y, renderedPosition.z, 1.0F},
                {layers.top, layers.side, layers.bottom, 0.0F},
                // data.w 在 item_entity.frag 里选中"与地形等价"的下落方块光照
                // 普通掉落方块物品保持为零
                {kItemModeBlockCube, 0.0F, 0.0F, 2.0F},
                {0.0F, 0.0F, 0.0F, packedSceneLight(renderedPosition)},
            };
            entityDraws_.append(push, 36, 0);
        }
        // 经验球是一小块面向相机的球体贴图公告板
        // 它走粒子同一条平面公告板路径，即 item_entity.vert 的 data.x == -1
        // 整个预留图集层就是这张贴图，因此 uvOrigin 取 (0,0)、uvScale 取 1
        // vanilla 的经验球还会上下浮动并循环变色，这里先做静态版本
        for (const auto& orb : snapshotOrbs) {
            entityDraws_.begin(ShadowEntityKind::Orb);
            const glm::vec3 renderedPosition =
                orb.previousPosition + (orb.position - orb.previousPosition) * itemAlpha;
            const glm::vec3 billboardCentre = renderedPosition + glm::vec3{0.0F, 0.25F, 0.0F};
            const ItemPush push{
                {billboardCentre.x, billboardCentre.y, billboardCentre.z, 0.3F},
                {kExperienceOrbLayer, 0.0F, 0.0F, 1.0F},
                {kItemModeAtlasBillboard, 0.0F, 0.0F, 1.0F},
                {0.0F, 0.0F, 0.0F, packedSceneLight(billboardCentre)},
            };
            entityDraws_.append(push, 6U, 0);
        }
    }

    // 粒子单独成一个通道：vanilla 把它们画在半透明地形层之后，而上面那些实体属于更早的实体阶段

    std::size_t drawParticles(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet,
                              bool deferSceneBufferFlush) {
        const auto& particles = particleSystem.particles();
        sceneParticleRecords_.clear();
        if (particles.empty()) {
            return 0U;
        }
        const std::size_t capacity = gpuSceneBuffer.capacityBytes() / sizeof(ParticleRecord);
        const std::size_t count = std::min(particles.size(), capacity);
        if (count == 0U) {
            return 0U;
        }
        // 复用一个常驻的暂存向量，而不是每帧临时分配
        // 世界与光照的读取都留在 VMA 的顺序写映射之外，最后一次性整体拷进去
        // 在把该映射暴露为 write-combined 内存的 Windows 堆上，这样明显更快
        auto& buffer = gpuSceneBuffer.frame(currentFrame);
        sceneParticleRecords_.reserve(count);
        // 逐粒子的 packedSceneLight 是两次区块查找，这一段单独计时以便归因
        const auto particleLightStart = std::chrono::steady_clock::now();
        for (std::size_t index = 0; index < count; ++index) {
            const auto& particle = particles[index];
            sceneParticleRecords_.push_back(ParticleRecord{
                {particle.position.x, particle.position.y, particle.position.z, particle.size},
                {particle.uvOrigin.x, particle.uvOrigin.y, particle.uvScale, particle.opacity},
                // RN-9b：后两槽是粒子自己的颜色与自发光，见 GpuSceneBuffer.hpp
                {particle.textureLayer, packedSceneLight(particle.position),
                 static_cast<float>(particle.tint), particle.emission},
            });
        }
        if (diag::traceEnabled()) {
            diag::frameTrace().particleLightMs += diag::msSince(particleLightStart);
        }
        // 把记录写进本帧的存储缓冲槽并刷新
        // drawFrame 开头等待的逐帧围栏已经把这些主机写排在上一次提交对同槽的读之后，因此不需要屏障
        // vmaFlushAllocation 用于非一致性堆（在 Apple 的统一内存上是空操作）
        const std::size_t bytes = count * sizeof(ParticleRecord);
        // 异步雨紧接着往同一块分配里追加，此时把刷新推迟
        // 独立显卡的 Windows 堆因此只为粒子与雨的合并区间付一次刷新，而不是两次
        if (!deferSceneBufferFlush) {
            std::memcpy(buffer.mapped, sceneParticleRecords_.data(), bytes);
            checkVk(vmaFlushAllocation(allocator, buffer.allocation, 0, bytes),
                    "vmaFlushAllocation(particle scene buffer)");
        }
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.particlePipeline);
        const std::array<VkDescriptorSet, 2> sets{descriptorSet, sceneDescriptorSets[currentFrame]};
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelines.particlePipelineLayout, 0, 2, sets.data(), 0, nullptr);
        vkCmdDraw(commandBuffer, 6U, static_cast<std::uint32_t>(count), 0, 0);
        if (!diagnosticsOnce_.instancedParticles) {
            diagnosticsOnce_.instancedParticles = true;
            std::cout << "[particles] instanced 1 draw for " << count
                      << " records (legacy = " << particles.size() << " draws)\n";
        }
        return count;
    }

    // 降雨绘制的两条路径各自成函数
    // 它们除了函数名之外没有任何共享逻辑，贴图路径连雨滴都不读
    // 三条分支曾缝在一个一百八十行的函数里，还共用同一个 static bool reported
    // 那样运行时一旦切换雨模式，另一条路径就永远不再打诊断行
    void drawRain(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet,
                  std::size_t baseRecordCount) {
        if (rainMode_ == RainMode::Texture) {
            drawTextureRain(commandBuffer, descriptorSet, baseRecordCount);
        } else {
            drawAsyncRain(commandBuffer, descriptorSet, baseRecordCount);
        }
    }

    // vanilla 的逐列降水，用 environment/rain.png 画窄长的竖直雨列
    // 它不消费 CPU 雨滴，那批雨滴此时只负责落地水花与天气音效
    void drawTextureRain(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet,
                         std::size_t baseRecordCount) {
        const float rainGradient = clientMirror.world().rainGradient;
        if (rainGradient <= 0.02F) {
            return;
        }
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.rainSheetPipeline);
        const std::array<VkDescriptorSet, 2> sets{descriptorSet,
                                                  sceneDescriptorSets[currentFrame]};
        vkCmdBindDescriptorSets(
            commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.rainSheetPipelineLayout, 0,
            static_cast<std::uint32_t>(sets.size()), sets.data(), 0, nullptr);

        // vanilla 的降水渲染用 10 格的高画质半径，每个 x/z 列发出一条面向相机的竖直条带
        // 条带从阻挡运动的表面高度起算，覆盖相机的局部竖直窗口，并向圆形边界淡出
        constexpr int kRainRadius = 10;
        const glm::vec3 cameraPosition = camera.position();
        const int cameraX = static_cast<int>(std::floor(cameraPosition.x));
        const int cameraY = static_cast<int>(std::floor(cameraPosition.y));
        const int cameraZ = static_cast<int>(std::floor(cameraPosition.z));
        std::size_t columnCount = 0U;
        const std::size_t capacity = gpuSceneBuffer.capacityBytes() / sizeof(ParticleRecord);
        sceneParticleRecords_.reserve(std::min(capacity, baseRecordCount + 441U));
        for (int dz = -kRainRadius; dz <= kRainRadius; ++dz) {
            for (int dx = -kRainRadius; dx <= kRainRadius; ++dx) {
                const int blockX = cameraX + dx;
                const int blockZ = cameraZ + dz;
                const float columnX = static_cast<float>(blockX) + 0.5F;
                const float columnZ = static_cast<float>(blockZ) + 0.5F;
                const float relativeX = columnX - cameraPosition.x;
                const float relativeZ = columnZ - cameraPosition.z;
                const float distance = std::sqrt(relativeX * relativeX + relativeZ * relativeZ);
                // 原版那两张表是按整数偏移索引的稳定 32x32 查找表，与相机的小数位置无关
                // 表的中心处会算出 0/0，因此不产生可用的四边形
                const float integerDistance = std::sqrt(static_cast<float>(dx * dx + dz * dz));
                if (integerDistance <= 1.0e-4F) {
                    continue;
                }
                glm::vec2 tangent{1.0F, 0.0F};
                tangent = {-static_cast<float>(dz) / integerDistance,
                           static_cast<float>(dx) / integerDistance};

                float bottom = static_cast<float>(cameraY - kRainRadius);
                float top = static_cast<float>(cameraY + kRainRadius);
                // 探测高度用与雨滴缓存相同的 +32 上限，附近的高屋顶因此也能把条带完全压没
                const float surface = rainSystem.precipitationSurfaceY(
                    clientCache, blockX, blockZ, cameraPosition.y + 32.0F);
                if (surface >= 0.0F) {
                    bottom = std::max(bottom, surface);
                    top = std::max(top, surface);
                }
                if (top - bottom <= 1.0e-4F) {
                    continue;
                }

                const float normalizedDistance = distance / static_cast<float>(kRainRadius);
                const float opacity =
                    ((1.0F - normalizedDistance * normalizedDistance) * 0.5F + 0.5F) *
                    rainGradient;
                if (opacity <= 0.01F || baseRecordCount + columnCount >= capacity) {
                    continue;
                }
                // vanilla 用世界坐标给每一列播种
                // 相邻条带因此各有稳定但不同的滚动相位与速度，不会形成一整幅同步的雨帘
                const std::uint32_t xBits = static_cast<std::uint32_t>(blockX);
                const std::uint32_t zBits = static_cast<std::uint32_t>(blockZ);
                const std::uint32_t xSeed = xBits * xBits * 3121U + xBits * 45238971U;
                const std::uint32_t zSeed = zBits * zBits * 418711U + zBits * 13761U;
                const std::int32_t randomSeed = static_cast<std::int32_t>(xSeed ^ zSeed);
                // 用共享的 Java LCG（mc::rng），不再在这里内联手写第四份 LCG 步进
                // seedFromValue + nextFloat 与原来那两行按位展开完全等价
                std::uint64_t randomState = rng::seedFromValue(
                    static_cast<std::uint64_t>(static_cast<std::int64_t>(randomSeed)));
                const float randomFloat = rng::nextFloat(randomState);
                const float tickTime = rainTime_ * 20.0F;
                const std::uint32_t phaseTick =
                    (static_cast<std::uint32_t>(std::floor(tickTime)) + xSeed + zSeed) & 31U;
                const float partialTick = tickTime - std::floor(tickTime);
                const float scroll = -(static_cast<float>(phaseTick) + partialTick) / 32.0F *
                                     (3.0F + randomFloat);
                const float packedLight = packedSceneLight(
                    {columnX, std::max(surface, static_cast<float>(cameraY)) + 0.1F, columnZ});
                // 显式以 RainColumnRecord 的字段名写入，再转成共享槽位
                // 它由 rain_sheet.vert 解读，与方块粉尘的 ParticleRecord 语义无关
                sceneParticleRecords_.push_back(asParticleRecord(RainColumnRecord{
                    {columnX, bottom, columnZ, 0.5F},
                    {top, opacity, scroll, packedLight},
                    {tangent.x, tangent.y, 0.0F, 0.0F},
                }));
                ++columnCount;
            }
        }
        if (columnCount == 0U) {
            return;
        }
        const std::size_t totalRecordCount = baseRecordCount + columnCount;
        auto& buffer = gpuSceneBuffer.frame(currentFrame);
        std::memcpy(buffer.mapped, sceneParticleRecords_.data(),
                    totalRecordCount * sizeof(ParticleRecord));
        checkVk(vmaFlushAllocation(allocator, buffer.allocation, 0,
                                   totalRecordCount * sizeof(ParticleRecord)),
                "vmaFlushAllocation(particle/texture-rain scene buffer)");
        // 原版降水是一整批网格
        // 这里的存储记录保持同一性质：一次实例化绘制，而不是每列一次 Vulkan 绘制调用
        vkCmdDraw(commandBuffer, 6U, static_cast<std::uint32_t>(columnCount), 0,
                  static_cast<std::uint32_t>(baseRecordCount));
        if (!diagnosticsOnce_.textureRain) {
            diagnosticsOnce_.textureRain = true;
            std::cout << "[rain] mode=texture vanilla-columns=" << columnCount
                      << " texture=environment/rain.png draws=1\n";
        }
    }

    // 异步粒子雨，把 CPU 雨滴实例化绘制
    // 记录接在方块粉尘之后写进同一个场景存储缓冲，baseInstance 越过它们
    // 整片雨因此一次 vkCmdDraw 画完
    void drawAsyncRain(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet,
                       std::size_t baseRecordCount) {
        const auto& drops = rainSystem.drops();
        if (drops.empty()) {
            return;
        }
        const std::size_t capacity = gpuSceneBuffer.capacityBytes() / sizeof(ParticleRecord);
        const std::size_t count = std::min(drops.size(), capacity - baseRecordCount);
        auto& buffer = gpuSceneBuffer.frame(currentFrame);
        sceneParticleRecords_.reserve(baseRecordCount + count);
        // 雨丝采样的是水的图集层，BM-1 之后那是未 tint 的原图，第三槽再写 0（= 不着色）
        // 就是白乘灰白 = 灰白（RN-21）。第三槽本来就是 packed tint，与粒子那条路径同一个
        // 语义，所以这里走同一份颜色而不是再硬写一个常数。
        //
        // 逐帧取一次而不是逐雨滴取一次：整片雨都生成在相机附近 kSpawnHalfWidth 的方形里，
        // 而 biomeTintAt 是 25 次 biomeAt。逐雨滴取会在渲染线程上按雨滴数付这笔钱，
        // 换来的是同一片雨里肉眼分不出的色差。
        const glm::vec3 rainCamera = camera.position();
        const auto rainColor = world::biomeTintAt(clientCache, world::BiomeTintKind::Water,
                                                  static_cast<int>(std::floor(rainCamera.x)),
                                                  static_cast<int>(std::floor(rainCamera.z)));
        const auto rainTint = static_cast<float>(packParticleTint(
            {static_cast<float>(rainColor[0]) / 255.0F, static_cast<float>(rainColor[1]) / 255.0F,
             static_cast<float>(rainColor[2]) / 255.0F}));
        // 与粒子同一性质，逐雨滴两次区块查找，累加进同一个 particleLightMs
        const auto rainLightStart = std::chrono::steady_clock::now();
        for (std::size_t index = 0; index < count; ++index) {
            const auto& drop = drops[index];
            sceneParticleRecords_.push_back(ParticleRecord{
                {drop.position.x, drop.position.y, drop.position.z, drop.size},
                {0.0F, 0.0F, 1.0F, 0.6F},
                {static_cast<float>(kWaterStillLayer), packedSceneLight(drop.position), rainTint,
                 0.0F},
            });
        }
        if (diag::traceEnabled()) {
            diag::frameTrace().particleLightMs += diag::msSince(rainLightStart);
        }
        const std::size_t totalRecordCount = baseRecordCount + count;
        if (totalRecordCount > 0U) {
            std::memcpy(buffer.mapped, sceneParticleRecords_.data(),
                        totalRecordCount * sizeof(ParticleRecord));
            checkVk(vmaFlushAllocation(allocator, buffer.allocation, 0,
                                       totalRecordCount * sizeof(ParticleRecord)),
                    "vmaFlushAllocation(combined particle/rain scene buffer)");
        }
        // 粒子通道可能已经把缓冲写满
        // 即使一个雨实例都放不下，它推迟的记录也已在上面刷新过了
        if (count == 0U) {
            return;
        }
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.particlePipeline);
        const std::array<VkDescriptorSet, 2> sets{descriptorSet, sceneDescriptorSets[currentFrame]};
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelines.particlePipelineLayout, 0, 2, sets.data(), 0, nullptr);
        vkCmdDraw(commandBuffer, 6U, static_cast<std::uint32_t>(count), 0,
                  static_cast<std::uint32_t>(baseRecordCount));
        if (!diagnosticsOnce_.asyncRain && count >= rainTargetCount() * 9U / 10U) {
            diagnosticsOnce_.asyncRain = true;
            std::cout << "[rain] mode=async drops=" << count << " draws=1\n";
        }
    }

    // RN-8c-D: the two faces a block item's cube needs beyond the flat
    // top/side/bottom triple, packed into the one spare float the item push
    // constant has (see world::packItemFrontBackLayers). Front sits on the
    // model's NORTH face and back on its south one, because a block item is the
    // block's model with no blockstate rotation — the same faces the inventory
    // icon resolves. A block with no front of its own answers side for both, so
    // this is safe to ask of any cube.
    //
    // The chest is the one block drawn from its own atlas section rather than
    // from its texture triple, so its front comes from there; everything else
    // about its cube is left exactly as it was.
    [[nodiscard]] static world::CubeItemLayers cubeItemCubeLayers(world::Block block) {
        auto faces = world::cubeItemLayers(block);
        if (block == world::Block::Chest) {
            // The chest item is drawn from its own baked atlas faces rather than
            // from the block's textures, so its front comes from there. Its flat
            // triple already holds the chest item's top/side, so only the front
            // has to be named.
            faces.front = kChestItemFrontLayer;
        }
        return faces;
    }

    // 用一个完整世界矩阵变换、经物品着色器的世界空间蒙皮长方体模式（data.x = 8）画一个轴对齐长方体
    // 矩阵携带平移与朝向；`dimensions` 是世界单位下的盒子尺寸；六个面采样纹理层 [layer, layer+5]
    // 箱子与第三人称玩家共用它，于是部件的旋转绑定在自己的局部坐标系上，而不是某个固定世界轴
    // `packedLight` 是该实体的场景光照采样（见 packedSceneLight），传 0 则使用固定光照
    // 今后任何经此绘制的方块实体，只要传入它就能获得场景光照

    static ItemPush makeWorldCuboidPush(const glm::mat4& worldMatrix,
                         glm::vec3 dimensions, float textureLayer, float packedLight = 0.0F) {
        const ItemPush push{
            {0.0F, 0.0F, 0.0F, 1.0F},
            {textureLayer, 0.0F, 0.0F, 0.0F},
            {kItemModeWorldMatrixCuboid, 0.0F, 0.0F, 0.0F},
            {dimensions.x, dimensions.y, dimensions.z, packedLight},
            worldMatrix,
        };
        return push;
    }
    void pushWorldCuboid(VkCommandBuffer commandBuffer, const glm::mat4& worldMatrix,
                         glm::vec3 dimensions, float layer, float light = 0.0F) const {
        const auto push = makeWorldCuboidPush(worldMatrix, dimensions, layer, light);
        vkCmdPushConstants(commandBuffer, pipelines.itemPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(push), &push);
        vkCmdDraw(commandBuffer, 36U, 1, 0, 0);
    }


    void drawChestEntities(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet) const {
        const auto& chests = clientMirror.world().chests;
        if (chests.empty())
            return;
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.itemPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.itemPipelineLayout,
                                0, 1, &descriptorSet, 0, nullptr);
        const auto drawWorldCuboid = [&](const glm::mat4& worldMatrix, glm::vec3 dimensions,
                                         float textureLayer, float packedLight) {
            pushWorldCuboid(commandBuffer, worldMatrix, dimensions, textureLayer, packedLight);
        };
        for (const auto& chest : chests) {
            const glm::vec3 origin{static_cast<float>(chest.position.x),
                                   static_cast<float>(chest.position.y),
                                   static_cast<float>(chest.position.z)};
            const auto orientation =
                clientCache.orientation(chest.position.x, chest.position.y, chest.position.z);
            const float yaw =
                orientation == world::BlockOrientation::East
                    ? 1.57079632679F
                    : (orientation == world::BlockOrientation::North
                           ? 3.14159265359F
                           : (orientation == world::BlockOrientation::West ? -1.57079632679F
                                                                           : 0.0F));
            const auto rotateHorizontal = [yaw](glm::vec3 offset) {
                const float cosine = std::cos(yaw);
                const float sine = std::sin(yaw);
                return glm::vec3{cosine * offset.x + sine * offset.z, offset.y,
                                 -sine * offset.x + cosine * offset.z};
            };
            const glm::vec3 blockCenter = origin + glm::vec3{0.5F};
            const glm::mat4 yawMatrix = glm::rotate(glm::mat4{1.0F}, yaw, {0.0F, 1.0F, 0.0F});
            // 箱子是 cutout 方块，光会传进它自己那一格：vanilla 对方块实体正是在该格采样
            const float packedLight = packedSceneLight(blockCenter);

            const glm::vec3 baseCenter = blockCenter + rotateHorizontal({0.0F, -0.1875F, 0.0F});
            drawWorldCuboid(glm::translate(glm::mat4{1.0F}, baseCenter) * yawMatrix,
                            {0.875F, 0.625F, 0.875F}, kChestBaseFirstLayer, packedLight);

            const float interpolatedLid =
                chest.previousLidAngle +
                (chest.lidAngle - chest.previousLidAngle) * renderInterpolationAlpha;
            // 掀起角度取自数据驱动的合页动画（其贝塞尔切线精确复现原先的三次缓出曲线）
            const float pitch = chestLidAnimation.liftRadians(interpolatedLid);
            // 绕合页轴线做刚体旋转，先平移到合页，绕合页的 X 轴旋转
            // 再把盖子盒从合页平移回它闭合时的中心
            // 这样复合能把合页边钉住，盖子才是掀起而不是在箱口上滑动
            // 若改成先绕盒子自身中心旋转、再让中心沿合页弧线滑动就做不到
            // 俯仰角为负时前缘向上抬
            // 先算局部坐标系，再按摆放朝向偏航
            constexpr glm::vec3 hingeLocal{0.0F, 0.125F, -0.4375F};
            constexpr glm::vec3 closedCentreFromHinge{0.0F, 0.15625F, 0.4375F};
            const glm::mat4 lidMatrix = glm::translate(glm::mat4{1.0F}, blockCenter) * yawMatrix *
                                        glm::translate(glm::mat4{1.0F}, hingeLocal) *
                                        glm::rotate(glm::mat4{1.0F}, -pitch, {1.0F, 0.0F, 0.0F}) *
                                        glm::translate(glm::mat4{1.0F}, closedCentreFromHinge);
            drawWorldCuboid(lidMatrix, {0.875F, 0.3125F, 0.875F}, kChestLidFirstLayer, packedLight);
        }
    }

    // 第三人称下把玩家渲染成多骨骼蒙皮长方体，动画库与背包预览相同
    // 但驱动数据取自玩家自己的视线与移动

    void collectWorldPlayer() {
        if (!worldReady || (shadowDisabled && cameraPerspective == CameraPerspective::FirstPerson)) {
            return;
        }
        // 姿态未绑定 = 动画器一次都没算过 = 这一局还没有玩家可画。模型在初始化期就
        // 加载好了，所以 `model().boneCount()` 早就非零，光靠它判断会让下面的循环拿
        // 一个空姿态去取矩阵。开着太阳阴影的隐藏导出正好走到这个组合：它直接调
        // drawFrame，绕过了主循环每帧的 updateWorldPlayer，而阴影一开，
        // 上面那条第一人称早退就不再兜底了。
        if (!worldPlayerAnimator.skeletonPose().bound()) {
            return;
        }

        entityDraws_.begin(ShadowEntityKind::Player);
        // 模型锚在玩家快照的插值脚点。正常游戏与相机跟随的脚点相同；
        // 导出等独立观察机位不会把玩家模型拖到镜头下面。
        const auto& player = clientMirror.player();
        const glm::vec3 feet = player.physicsPrevious +
            (player.physicsCurrent - player.physicsPrevious) * renderInterpolationAlpha;
        // 身体朝向是带滞后的身体偏航，头部由动画器相对它转动
        // 若模型渲染出来是背朝前，把 kFacingOffset 改成 3.14159265F
        constexpr float kFacingOffset = 0.0F;
        const float facingYaw = worldBodyYaw + kFacingOffset;
        constexpr float kModelUnitsToBlocks = 1.0F / 16.0F;
        const glm::mat4 modelRoot =
            glm::translate(glm::mat4{1.0F}, feet) *
            glm::rotate(glm::mat4{1.0F}, facingYaw, glm::vec3{0.0F, 1.0F, 0.0F}) *
            glm::scale(glm::mat4{1.0F}, glm::vec3{kModelUnitsToBlocks});
        // 两种第三人称视角共用这条路径，玩家因此和周围生物一样随场景变暗
        const float packedLight = packedSceneLight(feet + glm::vec3{0.0F, 0.9F, 0.0F});

        const auto layerForBone = [](std::string_view name) -> float {
            if (name == "head")
                return kPlayerHeadFirstLayer;
            if (name == "body")
                return kPlayerBodyFirstLayer;
            if (name == "rightArm")
                return kPlayerRightArmFirstLayer;
            if (name == "leftArm")
                return kPlayerLeftArmFirstLayer;
            if (name == "rightLeg")
                return kPlayerRightLegFirstLayer;
            if (name == "leftLeg")
                return kPlayerLeftLegFirstLayer;
            return -1.0F; // e.g. the hat layer, skipped for now
        };

        const auto& model = worldPlayerAnimator.model();
        // 第三人称姿态来自 PlayerModelAnimator 的控制器栈，与背包预览共用
        // 输入是真实的行走动画状态：静止不摆臂，停下时衰减到静息，潜行时身体前倾
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
                entityDraws_.append(makeWorldCuboidPush(cubeWorld, cube.renderSize(), layer, packedLight), 36U);
            }
        }
    }

    // `type` 对应的已加载物种，未加载时返回 nullptr（模型解析失败，或它不在随包的物种集合里）
    [[nodiscard]] const gameplay::entities::SpeciesRenderModel*
    speciesFor(const gameplay::entities::EntityType* type) const {
        for (const auto& species : speciesModels) {
            if (species.type == type) {
                return &species;
            }
        }
        return nullptr;
    }

    // 该物种的模型是否已绑定并可渲染
    // 生成路径以此为门控，免得刷怪蛋召出一只显示为缺失网格的生物

    [[nodiscard]] float packedSceneLight(glm::vec3 samplePoint) const {
        const int blockX = static_cast<int>(std::floor(samplePoint.x));
        const int blockY = static_cast<int>(std::floor(samplePoint.y));
        const int blockZ = static_cast<int>(std::floor(samplePoint.z));
        const float sky = static_cast<float>(clientCache.skyLight(blockX, blockY, blockZ));
        const float block = static_cast<float>(clientCache.blockLight(blockX, blockY, blockZ));
        return 1.0F + sky + block * 16.0F;
    }

    // 某个世界方块处的原始天光/方块光等级（0..15），供洞穴氛围音的累积器采样
    // 读的是渲染侧自有的客户端缓存，与交互射线同一个无锁来源
    struct LightSample final {
        int sky = 0;
        int block = 0;
    };
    [[nodiscard]] LightSample skyBlockLightAt(int blockX, int blockY, int blockZ) const {
        return {static_cast<int>(clientCache.skyLight(blockX, blockY, blockZ)),
                static_cast<int>(clientCache.blockLight(blockX, blockY, blockZ))};
    }

    // 画一个 box-UV 蒙皮长方体，对应模式 9，世界矩阵携带骨骼与方块变换
    // `renderSize` 是以模型单位表示的绘制尺寸
    // `uvSize` 是采样 box-UV 展开图所用的未膨胀尺寸，`uv` 是该展开图的原点
    // `textureSize` 取模型声明的 texture_width 与 texture_height，着色器用它去除纹素坐标
    // 这与 Bedrock 一致，实际像素分辨率与声明不同的实体皮肤因此仍能逐面对上
    // 采样实体纹理数组（binding 4）

    static ItemPush makeBoxUvCuboidPush(const glm::mat4& worldMatrix,
                         glm::vec3 renderSize, glm::vec3 uvSize, glm::vec2 uv, bool mirror,
                         glm::vec2 textureSize, std::uint32_t faceOverride, float layer,
                         std::uint32_t woolTint = 0xFFFFFFU, float packedLight = 0.0F,
                         float hurtFlash = 0.0F) {
        // 推送常量已经用满 Vulkan 保证的 128 字节，因此 box-UV 路径把标量塞得很紧
        // textureLayersRotation.w 以原始位模式存放逐面的来源与旋转覆盖
        // 着色器里用 floatBitsToUint 取回
        // positionSize.w 携带打包成 0xRRGGBB 的羊毛着色（白色即不着色）
        // 受伤强度取 0 或 1，已无处安放，于是搭在 dimensions.w 里压在打包好的场景光照之上
        // 光照取值 [0, 256]，受伤再加 512，着色器负责把两者拆回来
        // dimensions.w 为 0 时仍表示"没有场景光照，沿用固定光照"
        const ItemPush push{
            {uvSize.x, uvSize.y, uvSize.z, std::bit_cast<float>(woolTint)},
            {layer, textureSize.x, textureSize.y, std::bit_cast<float>(faceOverride)},
            {kItemModeBoxUvEntity, uv.x, uv.y, mirror ? 1.0F : 0.0F},
            {renderSize.x, renderSize.y, renderSize.z,
             packedLight + (hurtFlash > 0.5F ? 512.0F : 0.0F)},
            worldMatrix,
        };
        return push;
    }

    // 把自由活动的生物渲染成 box-UV 蒙皮模型，动画库与掉落物相同，同样在物理 tick 之间插值

    void collectWorldEntities() {
        // 从逐 tick 快照绘制，绝不读实时实体容器
        // 模拟跑在自己线程上，本通道遍历期间那个容器正在被重排和扩缩
        // 快照是按值拷贝，先绑到局部变量，其 entities() 引用才有效
        const auto& snapshot = clientMirror.entities();
        const auto& snapshotEntities = snapshot.entities();
        if (!worldReady || snapshotEntities.empty()) {
            return;
        }
        constexpr float kPi = 3.14159265358979323846F;
        constexpr float kModelUnitsToBlocks = 1.0F / 16.0F;
        // 随包的生物模型一律面朝 -Z（Minecraft 的正面），因此转半圈即可让它的前端对准游荡朝向
        // 若某个模型面朝 +Z 需另行调整
        constexpr float kEntityFacingOffset = kPi;

        for (const auto& entity : snapshotEntities) {
            // 每只生物用它自己物种的模型、动画和纹理层
            // 物种加载失败的实体直接跳过，而不是画成另一种生物
            const gameplay::entities::SpeciesRenderModel* species = speciesFor(entity.type);
            if (species == nullptr || !species->loaded) {
                continue;
            }
            // 动画剪辑的标识符来自物种注册时的渲染描述，而不是写死在渲染器里的字面量
            // 新增生物因此只需在它的类型上声明
            const auto& render = entity.type->render();
            const animation::AnimationClip* walk =
                species->model.animations.find(render.walkAnimation);
            const animation::AnimationClip* idle =
                species->model.animations.find(render.idleAnimation);

            const glm::vec3 position =
                entity.previousPosition +
                (entity.position - entity.previousPosition) * renderInterpolationAlpha;
            // 偏航按最短弧插值，重新选定朝向时模型不会绕远路旋转
            float deltaYaw = entity.yaw - entity.previousYaw;
            while (deltaYaw > kPi)
                deltaYaw -= 2.0F * kPi;
            while (deltaYaw < -kPi)
                deltaYaw += 2.0F * kPi;
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
                // 每走过一格完成一个迈腿周期，看起来才像自然步态
                animator.addLayer(*walk, walk->localTime(walkDistance), 1.0F);
            }
            if (idle != nullptr) {
                // 静息摇摆是帧动画，走 renderTime：无论昼夜规则如何它都保持呼吸，只有游戏暂停时才停
                animator.addLayer(
                    *idle, idle->localTime(static_cast<float>(renderTimeSeconds)), 1.0F);
            }
            const animation::SkeletonPose pose = animator.evaluate();

            // 死亡倒地：尸体在 20 个 tick 的死亡计时内倾倒 90 度，落地时带缓动
            float deathRoll = 0.0F;
            if (entity.deathTicks > 0) {
                const float progress = std::min((static_cast<float>(entity.deathTicks) +
                                                 renderInterpolationAlpha - 1.0F) /
                                                    20.0F * 1.6F,
                                                1.0F);
                deathRoll = std::sqrt(std::max(progress, 0.0F)) * (kPi * 0.5F);
            }
            // 受伤染色在受伤计时的每个 tick 都开启，并在整段死亡动画期间保持
            const float hurtFlash =
                entity.hurtTicks > 0 || entity.deathTicks > 0 ? 1.0F : 0.0F;
            // 几何烘焙只把 vanilla 的 scale(-1,-1,1) 中 Y 那一半折进 Y-up 几何里
            // X 那一半缺失会让所有生物相对 vanilla 左右镜像
            // 这里在模型根节点补上 X 翻转——最内层、在生物自己的坐标系里，与 vanilla 的做法一致
            // 放在渲染期而不是几何里，枢轴、逐骨骼旋转和镜像标志才留在各自的自然坐标系中
            // 旋转过的躯干因此仍能接上，面覆盖也继续有效
            // cullMode 为 NONE，翻转后的绕序无妨；法线随世界矩阵一起变换，反射正确
            const glm::mat4 modelRoot =
                glm::translate(glm::mat4{1.0F}, position) *
                glm::rotate(glm::mat4{1.0F}, yaw + kEntityFacingOffset,
                            glm::vec3{0.0F, 1.0F, 0.0F}) *
                glm::rotate(glm::mat4{1.0F}, deathRoll, glm::vec3{0.0F, 0.0F, 1.0F}) *
                glm::scale(glm::mat4{1.0F},
                           glm::vec3{-kModelUnitsToBlocks, kModelUnitsToBlocks, kModelUnitsToBlocks});

            // 整只生物取一次光照采样：黄昏变暗、无光洞穴里全黑、靠近火把会被照亮，和周围方块一致
            // 采样点取插值后的渲染位置而不是 tick 位置，走动时变化才平滑
            // 上抬半格取到的是躯干高度，EntitySystem 判定撞墙用的也是这个高度
            entityDraws_.begin(ShadowEntityKind::Creature);
            const float packedLight = packedSceneLight(position + glm::vec3{0.0F, 0.5F, 0.0F});

            const auto& model = species->model.model;
            // 声明的纹理尺寸就是 box-UV 的坐标空间，图集像素只需是它的等比缩放副本
            // 各长方体采样实体纹理数组里该物种自己的那一层
            const glm::vec2 textureSize = gameplay::entities::entityTextureSize(
                model,
                {static_cast<float>(textures_.entityTextureWidth), static_cast<float>(textures_.entityTextureHeight)});
            for (std::size_t index = 0; index < model.boneCount(); ++index) {
                const auto& bone = model.bones()[index];
                if (bone.neverRender) {
                    continue;
                }
                const glm::mat4 boneWorld = pose.worldMatrix(static_cast<int>(index));
                // 逐骨骼的纹理层在加载时就算好
                // "wool" 前缀的骨骼采样该物种的羊毛层，其余骨骼采样身体皮肤
                // 热循环里不再比对骨骼名
                const float boneLayer = index < species->boneTextureLayer.size()
                                            ? species->boneTextureLayer[index]
                                            : species->textureLayer;
                // 羊毛骨骼（即采样羊毛层的那些）按生物的染色着色，剪毛后整体消失；其余骨骼不着色
                // 白色是默认色、着色近似恒等，因此非羊和未染色的羊只付一次比较的代价
                const bool isWoolBone = species->secondaryTextureLayer >= 0.0F &&
                                        boneLayer == species->secondaryTextureLayer;
                if (isWoolBone && entity.sheared) {
                    continue;
                }
                const std::uint32_t woolTint =
                    isWoolBone ? gameplay::dyeColorTexture(entity.color) : 0xFFFFFFU;
                for (const auto& cube : bone.cubes) {
                    // 逐方块的旋转在骨骼内部、绕方块自己的枢轴进行
                    // 随后 `inflate` 让盒子绕自身中心膨胀，不影响 UV 展开图
                    const glm::mat4 cubeRotation =
                        cube.hasRotation ? animation::rotationAboutPivot(cube.rotation, cube.pivot)
                                         : glm::mat4{1.0F};
                    const glm::mat4 cubeWorld = modelRoot * boneWorld * cubeRotation *
                                                glm::translate(glm::mat4{1.0F}, cube.center());
                    entityDraws_.append(makeBoxUvCuboidPush(cubeWorld, cube.renderSize(), cube.size, cube.uv,
                                    cube.mirror, textureSize, cube.faceOverride,
                                    boneLayer, woolTint, packedLight, hurtFlash), 36U);
                }
            }
        }
    }


    void drawMiningProgress(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet) const {
        // 玩家状态和交互通道正在进行的挖掘都读自已发布的逐 tick 快照
        // 叠加层因此不会在渲染线程上碰实时的 PlayerInteraction
        // 这里保留按值拷贝
        // 若把它放进 Bindings，它会在 WorldRenderer 构造时就定格，永远处于未激活状态
        const auto playerSnapshot = clientMirror.player();
        const auto& digSnapshot = playerSnapshot.digging;
        if (uiFrameData_.gameMode != gameplay::GameMode::Survival || !digSnapshot.active ||
            !targetedBlock.has_value() || digSnapshot.target != targetedBlock->block) {
            return;
        }
        const auto block = digSnapshot.target;
        const auto target = clientCache.block(block.x, block.y, block.z);
        const float duration = gameplay::miningSeconds(target, uiFrameData_.selectedStack,
                                                       playerSnapshot.inWater,
                                                       !playerSnapshot.onGround);
        if (!std::isfinite(duration) || duration <= 0.0F)
            return;
        // 挖掘按 tick 推进，而裂纹叠加层每帧都画
        // 用帧插值系数对已过 tick 数做插值，阶段推进才是平滑的，而不是 20 Hz 的跳变
        const auto durationTicks =
            static_cast<float>(duration) * static_cast<float>(world::DayNightCycle::kTicksPerSecond);
        const float elapsedTicks =
            static_cast<float>(playerSnapshot.serverTick - digSnapshot.startedTick) +
            renderInterpolationAlpha;
        const float progress = std::clamp(elapsedTicks / durationTicks, 0.0F, 0.999F);
        // 阶段号取 (进度 * 10) - 1，因此挖掘的头十分之一完全没有裂纹叠加
        const int stage = std::clamp(static_cast<int>(progress * 10.0F) - 1, -1, 9);
        if (stage < 0)
            return;
        const float layer = kDestroyStageFirstLayer + static_cast<float>(stage);
        const ItemPush push{
            {static_cast<float>(block.x) + 0.5F, static_cast<float>(block.y) + 0.5F,
             static_cast<float>(block.z) + 0.5F, 1.006F},
            {layer, layer, layer, 0.0F},
            {kItemModeBlockCube, 0.0F, 0.0F, 0.0F},
            {0.0F, 0.0F, 0.0F, 0.0F},
        };
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.itemPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.itemPipelineLayout,
                                0, 1, &descriptorSet, 0, nullptr);
        vkCmdPushConstants(commandBuffer, pipelines.itemPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(push), &push);
        vkCmdDraw(commandBuffer, 36U, 1U, 0U, 0U);
    }


    void drawHeldItem(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet) const {
        // 打开背包或聊天时手持物依然可见，与原版一致；只有暂停或非第一人称视角才隐藏手
        if (!worldReady || paused || cameraPerspective != CameraPerspective::FirstPerson) {
            return;
        }
        const auto& stack = uiFrameData_.selectedStack;
        const bool emptyHand = stack.empty();
        const auto& pose = heldItemAnimation.pose();
        // 与掉落物、背包图标共用 world::rendersAsModelItem，不再各自列举 BlockModel
        // RN-14：手持台阶不再是「立方体压半高」的特例，它就是物品模型里的一个盒子
        const bool cubeModel = !emptyHand && gameplay::isBlockStack(stack) &&
                               world::rendersAsModelItem(stack.block);
        // RN-8c-D: same as the dropped block — all five layers from
        // world::cubeItemLayers, never from the flat triple whose `side` slot
        // holds a DirectionalCube's front.
        const world::CubeItemLayers heldFaces =
            !emptyHand && cubeModel ? cubeItemCubeLayers(stack.block) : world::CubeItemLayers{};
        const auto layers =
            emptyHand
                ? world::BlockTextureLayers{kPlayerRightArmFirstLayer, kPlayerRightArmFirstLayer,
                                            kPlayerRightArmFirstLayer}
                : world::BlockTextureLayers{gameplay::itemTextureLayer(stack),
                                            gameplay::itemTextureLayer(stack),
                                            gameplay::itemTextureLayer(stack)};
        const glm::mat4 heldTransform =
            viewBobbingMatrix() * (emptyHand ? animation::firstPersonArmTransform(pose)
                                   : uiFrameData_.eating
                                       ? animation::firstPersonEatTransform(pose, cubeModel)
                                       : animation::firstPersonItemTransform(pose, cubeModel));

        // 手与手持方块跟随玩家眼部的环境光，夜里会变暗，而不是永远处在固定光照下
        const float heldLight = packedSceneLight(camera.position());
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.heldItemPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.itemPipelineLayout,
                                0, 1, &descriptorSet, 0, nullptr);
        constexpr std::uint32_t generatedItemVertexCount = 12U + 16U * 16U * 4U * 6U;
        if (cubeModel) {
            // RN-14：与掉落物同一份 world::ItemModel 表，逐盒逐面画（着色器模式 11）
            // 盒中心偏移在这里乘进手持变换矩阵，着色器那边只多认一个模式号
            const auto range = world::itemModelRange(stack.block);
            for (std::size_t b = 0; b < range.count; ++b) {
                const world::ItemModelBox& box =
                    world::kItemModelBoxes[static_cast<std::size_t>(range.first) + b];
                const glm::vec3 size = (box.to16 - box.from16) / 16.0F;
                const glm::vec3 offset =
                    (box.from16 + box.to16) * 0.5F / 16.0F - glm::vec3{0.5F};
                const glm::mat4 boxTransform = glm::translate(heldTransform, offset);
                for (std::uint32_t f = 0; f < world::kFaces.size(); ++f) {
                    const auto facing =
                        static_cast<std::size_t>(world::bakeFacingOf(world::kFaces[f].face));
                    const world::ItemModelFace& face = box.face[facing];
                    if (!face.present) {
                        continue;
                    }
                    const ItemPush push = makeHeldBlockItemFacePush(
                        face, world::itemFaceLayer(heldFaces, face.slot), size, heldLight,
                        boxTransform);
                    vkCmdPushConstants(commandBuffer, pipelines.itemPipelineLayout,
                                       VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
                    vkCmdDraw(commandBuffer, 6U, 1, f * 6U, 0);
                }
            }
            return;
        }
        const ItemPush push{
            {0.0F, 0.0F, 0.0F, 1.0F},
            {layers.top, layers.side, layers.bottom, 0.0F},
            {emptyHand ? kItemModeMatrixViewModel : kItemModeGeneratedItem, 0.0F,
             emptyHand ? 1.0F : 0.0F, emptyHand ? 1.0F : 0.0F},
            emptyHand ? glm::vec4{0.25F, 0.75F, 0.25F, heldLight}
                      : glm::vec4{1.0F, 1.0F, 0.0625F, heldLight},
            heldTransform,
        };
        vkCmdPushConstants(commandBuffer, pipelines.itemPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(push), &push);
        vkCmdDraw(commandBuffer, emptyHand ? 36U : generatedItemVertexCount, 1, 0, 0);
    }


    // frame graph 的 pass body 是**自由函数**，不是捕获 this 的 lambda：function_ref
    // 不延长目标寿命，绑一个临时 lambda 就是悬垂。要用的状态经 PassContext::user 透传，
    // graph 不解释那个指针。
    struct GraphPassArgs final {
        WorldRenderer* self = nullptr;
        FrameContext* frame = nullptr;
    };

    // 五个蹦床。graph 交出来的命令缓冲与 frame.commandBuffer 是同一个句柄
    // （recordCommandBuffer 就是拿它调的 execute），body 沿用 frame.commandBuffer——
    // 这样搬进图里的几百行主体一个字符都不用改，逐像素回归才二分得动。
    static void graphUploadStep(VkCommandBuffer commandBuffer, const graph::PassContext& context) {
        static_cast<void>(commandBuffer);
        auto& args = *static_cast<GraphPassArgs*>(context.user);
        const diag::ScopedAccumulate bodyTimer{args.self->graphBodyMs_};
        args.self->recordUpload(*args.frame);
    }

    // RN-35：一级一个蹦床。级别是编译期常量而不是 PassContext 上的一个字段——
    // 图里它们是两个**独立的步**（各自的帧缓冲、各自的清空），不是一个步跑两遍
    template <std::size_t Cascade>
    static void graphShadowStep(VkCommandBuffer commandBuffer, const graph::PassContext& context) {
        static_cast<void>(commandBuffer);
        auto& args = *static_cast<GraphPassArgs*>(context.user);
        const diag::ScopedAccumulate bodyTimer{args.self->graphBodyMs_};
        args.self->recordShadow(*args.frame, Cascade);
    }

    static void graphWorldStep(VkCommandBuffer commandBuffer, const graph::PassContext& context) {
        static_cast<void>(commandBuffer);
        auto& args = *static_cast<GraphPassArgs*>(context.user);
        const diag::ScopedAccumulate bodyTimer{args.self->graphBodyMs_};
        args.self->recordWorld(*args.frame);
    }

    static void graphTemporalResolveStep(VkCommandBuffer commandBuffer,
                                         const graph::PassContext& context) {
        static_cast<void>(commandBuffer);
        auto& args = *static_cast<GraphPassArgs*>(context.user);
        const diag::ScopedAccumulate bodyTimer{args.self->graphBodyMs_};
        args.self->recordTemporalResolve(*args.frame, context.imageIndex);
    }

    // RN-53 的判别仪器（diag::graphGapProbeEnabled）。它必须**真的什么都不做**：
    // 连 ScopedAccumulate 都不要——那会往 graphBodyMs_ 上加一笔，而这个探针要量的
    // 恰恰是「一个零成本的步在两个 renderpass 之间值多少 GPU 时间」。
    // 关着时这一步在编译期就被剪掉，这个函数根本不会被调用。
    static void graphGapProbeStep(VkCommandBuffer commandBuffer,
                                  const graph::PassContext& context) {
        static_cast<void>(commandBuffer);
        static_cast<void>(context);
    }

    static void graphMenuBackgroundStep(VkCommandBuffer commandBuffer,
                                        const graph::PassContext& context) {
        static_cast<void>(commandBuffer);
        auto& args = *static_cast<GraphPassArgs*>(context.user);
        const diag::ScopedAccumulate bodyTimer{args.self->graphBodyMs_};
        args.self->recordMenuBackground(*args.frame, context.imageIndex);
    }

    static void graphGuiStep(VkCommandBuffer commandBuffer, const graph::PassContext& context) {
        static_cast<void>(commandBuffer);
        auto& args = *static_cast<GraphPassArgs*>(context.user);
        const diag::ScopedAccumulate bodyTimer{args.self->graphBodyMs_};
        args.self->recordGui(*args.frame);
    }

    static void graphPresentBlitStep(VkCommandBuffer commandBuffer,
                                     const graph::PassContext& context) {
        static_cast<void>(commandBuffer);
        auto& args = *static_cast<GraphPassArgs*>(context.user);
        const diag::ScopedAccumulate bodyTimer{args.self->graphBodyMs_};
        // 画完的场景图逐字节搬进交换链图像。两者都是 B8G8R8A8，copy 不做任何转换，
        // 于是交换链取 UNORM 还是 SRGB 都不影响呈现结果
        args.self->copySceneToSwapchain(args.frame->commandBuffer, context.imageIndex);
    }

    // ---- frame graph 的五个 pass body（RN-20a）------------------------------
    //
    // 四段命令录制（上传 / 阴影 / 世界 / 界面）加帧末的 blit 不再是顺序调用，而是被
    // 烘焙式 frame graph 编排：顺序、renderpass 的 begin/end、清空值与边界屏障都在
    // 编译期定好，每帧只执行一张扁平的指令表。body 的**内容**没有搬动，只是换了调用者。

    // 上传步。本帧的暂存拷贝、一条 TRANSFER → VERTEX_INPUT 的 memory barrier，
    // 以及遮挡查询池的 reset。
    //
    // 它在图里是**非渲染步**（renderPass == VK_NULL_HANDLE），这个身份就是它的保护：
    // vkCmdResetQueryPool 在 renderpass 内是非法的。上传那条屏障是 memory barrier，
    // 没有 layout 可转，也就不参与图的 image barrier 合批，留在这里。
    void recordUpload(FrameContext& frame) {
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
        const VkQueryPool frameQueryPool = occlusion.queryPools[currentFrame];
        if (frameQueryPool != VK_NULL_HANDLE) {
            // 复用前先清空本帧的槽位区间
            // 上一次提交的结果已在本次 drawFrame 里读回
            // 每帧独占自己的查询池，槽位因此总是从零开始
            vkCmdResetQueryPool(frame.commandBuffer, frameQueryPool, 0U,
                                static_cast<std::uint32_t>(kOcclusionQueriesPerFrame));
        }
    }

    // 世界那趟的 body。sky → 不透明地形 → cutout → 实体 → 半透明 → 粒子 → 雨 → 描边，
    // 每一条顺序约束的理由都写在下面各自的注释里。renderpass 的 begin/end 归 graph；
    // 视口与裁剪是命令缓冲级动态状态，仍由本 body 自己设。
    void recordWorld(FrameContext& frame) {
        // 与上传步里 reset 的是同一个池：每帧独占一个，currentFrame 在一帧之内不变。
        // 两步各自取一次，是因为它们已经是两个独立的 body，不再共享一个函数作用域
        const VkQueryPool frameQueryPool = occlusion.queryPools[currentFrame];
        VkViewport viewport{};
        viewport.width = static_cast<float>(swapchainExtent.width);
        viewport.height = static_cast<float>(swapchainExtent.height);
        viewport.maxDepth = 1.0F;
        vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
        VkRect2D scissor{{0, 0}, swapchainExtent};
        vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.skyPipeline);
        vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelines.pipelineLayout, 0, 1, &frame.descriptorSet, 0, nullptr);
        vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);

        const float aspect =
            static_cast<float>(swapchainExtent.width) / static_cast<float>(swapchainExtent.height);
        // 剔除要用场景实际渲染所在的眼点与朝向，而不是第一人称相机
        // 第三人称下渲染眼点被拉到后方，前视视角还朝向相反
        // 这时用 camera.viewMatrix() 会把屏幕上的大部分地形剔掉
        const Frustum frustum(camera.projectionMatrix(aspect, cameraFarPlane()) *
                              viewBobbingMatrix() * renderViewMatrix());
        // 遮挡结果比当前帧晚两帧
        // 视角快速移动时刚扫进视锥的 section 仍带着上一个眼点的 Occluded 标记，会有几帧空成窟窿
        // 运动量按渲染眼点度量，视锥也是用它建的，第三人称下相机对象另在别处
        // 视角移动较快时本帧照画所有在视锥内的 section 并同时重新查询，转视角期间几何不会消失
        const RenderEye renderEye = renderEyeState();
        // 由于结果晚两帧，陈旧的 Occluded 状态只有在眼点静止时才可信
        // 累计自上次校验点以来的旋转与平移，超过阈值就整表作废
        // 作废后视锥内所有 section 照画并重新查询
        // 于是即使一次始终触发不了逐帧快速运动判定的平滑快扫，也会在转过几度之内失效重来
        if (!occlusionValidityInitialized) {
            occlusionValidityInitialized = true;
            occlusionRotationAccumulatorDegrees = 0.0F;
            occlusionTranslationAccumulator = 0.0F;
        }
        if (hasLastRenderEye) {
            occlusionRotationAccumulatorDegrees += glm::degrees(std::acos(
                std::clamp(glm::dot(renderEye.forward, lastRenderEye.forward), -1.0F, 1.0F)));
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
        // 遮挡通道由近及远处理 section，先画近处地形并写入深度
        // 再用累积的深度测试更远 section 的包围盒，通过后才画它自己的网格
        // 埋在地下的洞穴因此不再被上方地表挡着还反复着色
        // 查询管线与不透明管线逐 section 交替，每次绘制前管线和它的描述符集都要重新绑定
        // 只绑一次会让上一个管线的描述符集仍然生效，触发 VUID-vkCmdDrawIndexed-None-08600
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
        std::ranges::sort(frustumEntries,
                          [](const FrustumEntry& first, const FrustumEntry& second) {
                              return first.distanceSquared < second.distanceSquared;
                          });

        // 诊断用：把场景的网格/视锥状态报告一次
        if (!diagnosticsOnce_.occlusionScene && testScene.has_value() && testScene->occlusionScene) {
            diagnosticsOnce_.occlusionScene = true;
            const glm::vec3 camPos = camera.position();
            const glm::vec3 camDir = camera.direction();
            std::cerr << "[scene] gpuMeshes=" << gpuMeshes.size()
                      << " frustumEntries=" << frustumEntries.size() << " cam=" << camPos.x << ','
                      << camPos.y << ',' << camPos.z << " dir=" << camDir.x << ',' << camDir.y
                      << ',' << camDir.z << '\n';
            for (const auto& entry : frustumEntries) {
                std::cerr << "  entry(" << entry.position.chunkX << ',' << entry.position.sectionY
                          << ',' << entry.position.chunkZ
                          << ") opaque=" << entry.mesh->opaque.indexCount << '\n';
            }
        }

        frame.occlusionQueryCount = 0U;
        frame.occlusionQuerySections.clear();
        std::size_t visibleCount = 0;
        std::vector<const GpuMesh*> visibleCutoutMeshes;
        std::vector<const GpuMesh*> visibleTranslucentMeshes;
        for (const auto& entry : frustumEntries) {
            const auto& mesh = *entry.mesh;
            const auto stateIt = occlusionStates.find(entry.position);
            const OcclusionState state =
                stateIt == occlusionStates.end() ? OcclusionState::Unknown : stateIt->second;
            // 在本 section 记录查询之前先取一次预算余量
            // 这样填满最后一个槽位的那个 section 与超出预算的那些能得到一致处理
            // 超预算的一律无条件绘制，否则陈旧的 Occluded 状态会把它们永远藏起来
            const bool withinQueryBudget = frame.occlusionQueryCount < kOcclusionQueriesPerFrame;

            // 视锥内的每个 section 都会重新查询
            // 被挡住的洞穴一被看到就立刻显现，可见的一被遮住就立刻剔除
            // 查询结果在两帧后才用于门控绘制
            if (!occlusion.disabled && frameQueryPool != VK_NULL_HANDLE && withinQueryBudget) {
                vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  occlusion.queryPipeline);
                vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        occlusion.queryLayout, 0, 1, &frame.descriptorSet, 0,
                                        nullptr);
                const VkDeviceSize boxOffset = 0;
                vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &occlusion.boxVertexBuffer.buffer,
                                       &boxOffset);
                vkCmdBindIndexBuffer(frame.commandBuffer, occlusion.boxIndexBuffer.buffer, 0,
                                     VK_INDEX_TYPE_UINT32);
                const OcclusionQueryPushConstants push{
                    glm::vec4{mesh.bounds.minimum, 1.0F},
                    glm::vec4{mesh.bounds.maximum, 1.0F},
                };
                vkCmdPushConstants(frame.commandBuffer, occlusion.queryLayout,
                                   VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
                const std::uint32_t slot = frame.occlusionQueryCount;
                vkCmdBeginQuery(frame.commandBuffer, frameQueryPool, slot,
                                kOcclusionQueryControlFlags);
                vkCmdDrawIndexed(frame.commandBuffer, 36, 1, 0, 0, 0);
                vkCmdEndQuery(frame.commandBuffer, frameQueryPool, slot);
                frame.occlusionQuerySections.push_back(entry.position);
                ++frame.occlusionQueryCount;
            }

            // Unknown 与 Visible 的 section 当帧就画，几何因此不会突然弹出
            // Occluded 的要等一次通过的查询证明它可见
            // 例外是查询预算已用尽，那时陈旧状态会让它们永久隐藏
            // 另一个例外是视角正在快速移动，那时它们的状态来自旧眼点
            // 这两种情况下它们照画并同时重新查询
            if (!occlusion.disabled && withinQueryBudget && !cameraMovingFast &&
                state == OcclusionState::Occluded) {
                continue;
            }
            if (mesh.opaque.indexCount > 0U) {
                vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  pipelines.graphicsPipeline);
                vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        pipelines.pipelineLayout, 0, 1, &frame.descriptorSet, 0, nullptr);
                vkCmdPushConstants(frame.commandBuffer, pipelines.pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                                   0, sizeof(glm::vec4), &mesh.sectionOrigin);
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
            vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.cutoutPipeline);
            // 遮挡通道可能把查询管线的描述符集留在绑定状态，因此 cutout 管线在绘制前重新绑定自己的
            vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    pipelines.pipelineLayout, 0, 1, &frame.descriptorSet, 0, nullptr);
            for (const auto* mesh : visibleCutoutMeshes) {
                vkCmdPushConstants(frame.commandBuffer, pipelines.pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                                   0, sizeof(glm::vec4), &mesh->sectionOrigin);
                vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &mesh->vertexBuffer.buffer,
                                       &mesh->cutout.vertexOffset);
                vkCmdBindIndexBuffer(frame.commandBuffer, mesh->indexBuffer.buffer,
                                     mesh->cutout.indexOffset, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(frame.commandBuffer, mesh->cutout.indexCount, 1, 0, 0, 0);
            }
        }
        // 实体属于实体阶段，位于 cutout 地形与半透明地形之间，顺序与 vanilla 一致
        // 画在半透明通道之后会让所有生物、物品和玩家浮在水和玻璃前面
        // 因为半透明管线不写深度，它画过的东西无法拒绝后来的绘制
        // 放在这里，深度缓冲两个方向都成立
        // 生物前面的水会混合在它之上，生物后面的水会被深度测试拒掉
        drawChestEntities(frame.commandBuffer, frame.descriptorSet);
        drawCollectedEntities(frame.commandBuffer, frame.descriptorSet);
        std::ranges::sort(visibleTranslucentMeshes, [&cameraPosition](const GpuMesh* first,
                                                                      const GpuMesh* second) {
            const glm::vec3 firstCenter = (first->bounds.minimum + first->bounds.maximum) * 0.5F;
            const glm::vec3 secondCenter = (second->bounds.minimum + second->bounds.maximum) * 0.5F;
            return glm::dot(firstCenter - cameraPosition, firstCenter - cameraPosition) >
                   glm::dot(secondCenter - cameraPosition, secondCenter - cameraPosition);
        });
        if (!visibleTranslucentMeshes.empty()) {
            vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              pipelines.translucentPipeline);
            vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    pipelines.pipelineLayout, 0, 1, &frame.descriptorSet, 0, nullptr);
            for (const auto* mesh : visibleTranslucentMeshes) {
                vkCmdPushConstants(frame.commandBuffer, pipelines.pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                                   0, sizeof(glm::vec4), &mesh->sectionOrigin);
                vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &mesh->vertexBuffer.buffer,
                                       &mesh->translucent.vertexOffset);
                // RN-22：半透明的索引在自己的缓冲里（重排要整条换掉），偏移恒为 0。
                vkCmdBindIndexBuffer(frame.commandBuffer, mesh->translucentIndexBuffer.buffer,
                                     0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(frame.commandBuffer, mesh->translucent.indexCount, 1, 0, 0, 0);
            }
        }
        // 粒子排在半透明地形通道之后，与 vanilla 的绘制位置一致
        // 雨的记录会追加进同一块场景缓冲，因此粒子通道此时推迟刷新，两段合并成一次刷新
        // 贴图雨在无雨时会直接 return，绝不能对它推迟——那样粒子已经发了 draw 却没人写缓冲
        const bool appendAsyncRain =
            rainMode_ == RainMode::Async && !rainSystem.drops().empty();
        const std::size_t particleRecordCount =
            drawParticles(frame.commandBuffer, frame.descriptorSet, appendAsyncRain);
        drawRain(frame.commandBuffer, frame.descriptorSet, particleRecordCount);
        drawMiningProgress(frame.commandBuffer, frame.descriptorSet);
        // 火可以被瞄准，左键才能扑灭，但与 vanilla 一样不画选择框
        // 它虽可交互，轮廓形状却是空的，因此瞄得上却看不到框
        const bool targetIsFire =
            targetedBlock.has_value() &&
            clientCache.block(targetedBlock->block.x, targetedBlock->block.y,
                              targetedBlock->block.z) == world::Block::Fire;
        // RN-39：`previewOutline` 只有离屏导出会打开（`--outline`）。导出为了冻住世界
        // 把 paused 置真，而这一行把 paused 读成「有界面打开」——同一个混淆点已经让
        // RN-33 的每一张出图都糊了一遍。这里不拆那个语义（影响面在 UI 线），
        // 让导出显式说明它要画这一层。
        if (!inventoryOpen && (!paused || previewOutline) && !chatOpen &&
            targetedBlock.has_value() && !targetIsFire) {
            // 选择框描的是方块的真实形状，火把、植物、箱子、台阶这类非满方块不再显示成整格框
            // 形状取自该格的状态（台阶上下半、作物生长阶段等）
            // RN-10f：描边取自形状的盒集，而不是整个形状的包围盒——此前楼梯、墙、
            // 栅栏门都被一个大方框圈住，框住的是射线打不到、人也站不上去的空气。
            // RN-16：但也不是**逐盒**各描一圈。vanilla 的 ShapeRenderer.renderShape
            // 走 VoxelShape.forAllEdges，而那是 Shapes.or 合并后的离散网格：两个盒
            // 共面的内部接缝**不发边**。逐盒描边会把楼梯背面与两个侧面在 y=0.5 处各多
            // 画一条横线（正面那条是真边界，合并后仍在）。合并与发边在
            // render::outlineEdgesOf，一次 draw 画一条线段。
            const world::BlockSelectionBoxes outlineBoxes =
                world::blockSelectionBoxes(clientCache, targetedBlock->block);
            std::array<render::OutlineBox, world::kMaxSelectionBoxes> outlineShape{};
            for (std::size_t i = 0; i < outlineBoxes.count; ++i) {
                outlineShape[i] = {outlineBoxes.boxes[i].minimum, outlineBoxes.boxes[i].maximum};
            }
            const render::OutlineEdges outlineEdges = render::outlineEdgesOf(
                std::span<const render::OutlineBox>{outlineShape.data(), outlineBoxes.count});
            if (outlineEdges.count > 0) {
                vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  pipelines.outlinePipeline);
                vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        pipelines.outlinePipelineLayout, 0, 1,
                                        &frame.descriptorSet, 0, nullptr);
                for (std::size_t i = 0; i < outlineEdges.count; ++i) {
                    // 推送常量只有一个写点（HudTypes.hpp 的 makeOutlineSegmentPush）：
                    // 就地拼一个 vec4 数组正是「声明分散到每个调用点」的形状，
                    // hud_push_constant_test 拦的就是它
                    const render::OutlinePush outlinePush = render::makeOutlineSegmentPush(
                        targetedBlock->block, outlineEdges.segments[i],
                        static_cast<float>(swapchainExtent.width),
                        static_cast<float>(swapchainExtent.height));
                    vkCmdPushConstants(frame.commandBuffer, pipelines.outlinePipelineLayout,
                                       VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(outlinePush),
                                       &outlinePush);
                    // RN-45：一条棱是两个三角形、六个顶点；着色器按 gl_VertexIndex
                    // 查表取端点与推向哪一侧，没有顶点缓冲，所以这个数与
                    // BlockOutlineGeometry.hpp 必须同源
                    vkCmdDraw(frame.commandBuffer, render::kOutlineSegmentVertexCount, 1, 0, 0);
                }
            }
        }
        graphVisibleCount_ = visibleCount;
    }

    // 界面那趟的 body。
        // 界面单独一趟。世界那趟的颜色附件是场景图的 **sRGB 视图**（着色器写线性值、
        // 硬件编码、混合因此发生在线性空间）；这一趟绑的是同一张图的 **UNORM 视图**，
        // 着色器直接写 sRGB 编码值、固定功能混合也在编码值上做——正是 vanilla 合成
        // 界面的空间。分开的代价是一次 render pass 切换，换来的是提示框的半透明、
        // 准星的反色、暗角的乘性混合以及所有界面文字的颜色都与原版逐字节一致。
        //
        // 深度是这一趟自己的、每帧清空：第一人称手持物与背包里的 3D 玩家预览要深度
        // 测试，而世界的深度可能是多重采样的；顺带对齐 vanilla——它也在画手之前清一次
        // 深度，所以贴脸的方块不会把手切掉。
    //
    // ⚠ 上面这段注释里「世界那趟绑 sRGB 视图、这一趟绑 UNORM 视图」**与代码不符**，
    // 保留原文只是为了不在本轮（零视觉变更的重构）里混进无关改动。事实是：
    // createSceneTargets 只建一个 UNORM 视图，两套 framebuffer 绑的是同一个 view，
    // 两个 renderpass 的 color 格式都是 sceneUnormFormat()，全仓没有 MUTABLE_FORMAT，
    // 也没有任何 sRGB image view。
    //
    // 而且**现状是对的**：整帧都画在未经伽马转换的目标上、混合发生在 sRGB 编码值上，
    // 这正是 vanilla 的行为（OpenGL 默认不开 GL_FRAMEBUFFER_SRGB），也是「所有采样纹理
    // 必须 UNORM、着色器里不许出现传输函数」那条铁律的另一半。两趟拆分的真实理由是
    // 上面第二段说的那个：**界面不做 MSAA，且界面自带每帧清空的深度**。
    // 不要把「补一个 sRGB 视图」当成欠账去做——那是回归。详见 sceneUnormFormat 的注释。
    // UI-5：全景 + 整帧模糊，跑在世界那趟与界面那趟之间。
    //
    // 这正是 26.1 的 BEFORE_BLUR / AFTER_BLUR 两段（`GuiRenderer.java:182-184`）：
    // 全景属于模糊**之前**那一段，界面其余部分属于之后。它在图里是**非渲染步**——
    // 一次背景绘制加六趟模糊，各有各的 renderpass 与靶，塞不进一个附件表；
    // 步身自己 begin/end，图只负责它的位置与 scene_color 的 SAMPLED 用途位。
    //
    // 模糊关着（menuBackgroundBlurriness = 0，界面上显示 OFF）时这里只画全景；
    // 主菜单那一档更是连模糊都不该有——26.1 的 TitleScreen 是清晰的。
    void recordMenuBackground(FrameContext& frame, std::uint32_t imageIndex) {
        if (!menuBlur.valid() || !hud_.screenOpen()) {
            return;
        }
        const auto kind = hud_.currentBackgroundKind();
        if (ui::backgroundDrawsPanorama(kind)) {
            menuBlur.beginBackgroundPass(frame.commandBuffer, imageIndex);
            hud_.drawMenuPanorama(frame.commandBuffer, frame.descriptorSet);
            menuBlur.endBackgroundPass(frame.commandBuffer);
        }
        if (!ui::backgroundIsBlurred(kind)) {
            return;
        }
        const int blurriness = options.menuBackgroundBlurriness;
        if (!ui::menuBlurEnabled(blurriness)) {
            return;
        }
        menuBlur.record(frame.commandBuffer, imageIndex, ui::menuBlurRadius(blurriness));
    }

    // TAA-1：resolve。非渲染步——三条前置屏障、renderpass 的 begin/end 与那一个
    // 全屏三角形都在 TemporalResolve 里，理由见那个头文件第 3 条。
    void recordTemporalResolve(FrameContext& frame, std::uint32_t imageIndex) {
        temporalResolve.record(frame.commandBuffer, imageIndex, temporalState.historyWriteSlot,
                               temporalState.reprojection(), temporalState.historyWeight());
        // 本帧的相机成为下一帧的"上一帧"，历史靶翻面。推进放在这里而不是 drawFrame
        // 末尾：只有这一步真的录进了命令缓冲，历史图里才会有东西可读
        temporalState.advance();
    }

    void recordGui(FrameContext& frame) {
        // 视口与裁剪是命令缓冲级的动态状态，跨 pass 仍然有效；这里自己算一份再重设一次，
        // 是为了让这一趟自己成立，不依赖上一趟留下了什么
        VkViewport viewport{};
        viewport.width = static_cast<float>(swapchainExtent.width);
        viewport.height = static_cast<float>(swapchainExtent.height);
        viewport.maxDepth = 1.0F;
        VkRect2D scissor{{0, 0}, swapchainExtent};
        vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
        // 游戏内 HUD 层以及叠在它上面的各种界面，都由 drawHud 按 vanilla 的层序绘制
        // HUD 层含手持物、水下叠加、暗角、快捷栏、状态条、准星和手持物名称
        hud_.drawHud(frame.commandBuffer, frame.descriptorSet);
        drawShadowDebugOverlay(frame.commandBuffer);
    }

    // 一帧的命令录制。四段加 blit 全部由图执行，这里只剩命令缓冲的 begin/end。
    //
    // 没有「出问题就退回旧路径」的运行期开关：双路径会让逐像素回归无法二分定位，
    // 旧的顺序调用因此是删掉而不是留着当 fallback。
    [[nodiscard]] std::size_t recordCommandBuffer(FrameContext& frame, std::uint32_t imageIndex) {
        refreshDiagnosticsEpoch();
        entityDraws_.clear();
        collectEntityShadowDecals();
        collectItemEntities();
        collectWorldEntities();
        collectWorldPlayer();
        auto beginInfo =
            vkStructure<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
        checkVk(vkBeginCommandBuffer(frame.commandBuffer, &beginInfo), "vkBeginCommandBuffer");
        graphVisibleCount_ = 0;
        graphBodyMs_ = 0.0;
        GraphPassArgs args{this, &frame};
        const graph::PassContext context{&args, imageIndex};
        const auto executeStart = diag::FrameTrace::Clock::now();
        // RN-19d0：诊断关着时 frame.timestampPool 是空句柄，execute() 因此一个 vk 入口
        // 都不多调。槽位数写回帧上下文，回读按它读而不是按池容量读。
        const graph::GpuTimestampWriter timestamps{frame.timestampPool, 0U};
        frame.timestampSlots =
            timestamps.active() ? graph::gpuTimestampSlotCount(frameGraph.steps().size()) : 0U;
        frameGraph.execute(frame.commandBuffer, imageIndex, context, timestamps);
        if (diag::traceEnabled()) {
            // graphMs 量的是 execute() **自身**的编排开销：屏障合批与 begin/end。
            // body 的时间由各蹦床累进 graphBodyMs_ 后在这里扣掉——它已经被 recordMs 量着，
            // 两个阶段并列相加才有意义。
            diag::frameTrace().graphMs += diag::msSince(executeStart) - graphBodyMs_;
        }
        checkVk(vkEndCommandBuffer(frame.commandBuffer), "vkEndCommandBuffer");
        return graphVisibleCount_;
    }

    // 应用两次提交之前记录的遮挡查询结果
    // 本帧的围栏刚等待完毕，那批查询必然已完成
    // 在本帧槽位区间被重置复用之前于此读取，逐 section 的绘制门控就恰好保持两帧延迟

    void readBackOcclusionQueries() {
        const VkQueryPool frameQueryPool = occlusion.queryPools[currentFrame];
        if (frameQueryPool == VK_NULL_HANDLE) {
            return;
        }
        auto& frame = frames[currentFrame];
        const std::uint32_t count = frame.occlusionQueryCount;
        if (count == 0U) {
            return;
        }
        frame.occlusionQueryResults.resize(count);
        // 围栏刚等待完毕，这里的每个查询都已完成，WAIT_BIT 因此永远不会阻塞
        // 但它让主机读取显式地与 Metal 的可见性结果缓冲同步，而不是指望 MoltenVK 的延迟累积恰好做完
        const VkResult result = vkGetQueryPoolResults(
            device, frameQueryPool, 0U, count, count * sizeof(std::uint64_t),
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
        // 受控遮挡场景会把查询结果导出，便于对照已知几何验证包围盒测试
        if (testScene.has_value() && testScene->occlusionScene) {
            for (std::uint32_t index = 0; index < count; ++index) {
                const auto& position = frame.occlusionQuerySections[index];
                std::cerr << "[query] section(" << position.chunkX << ',' << position.sectionY
                          << ',' << position.chunkZ
                          << ") count=" << frame.occlusionQueryResults[index] << '\n';
            }
        }
    }



  // ---- 绑定到渲染器内核状态的引用（所有权在 Impl）----
  std::optional<TestSceneOptions>& testScene;
  WorldPipelines& pipelines;
  OcclusionResources& occlusion;
  world::ChunkStreamer& chunkStreamer;
  world::World& interactionWorld;
  world::World& clientCache;
  world::WorldLightEngine& interactionLightEngine;
  std::unordered_map<world::SectionPosition, GpuMesh, world::SectionPositionHash>& gpuMeshes;
  StreamBufferPool& deviceBufferPool_;
  StreamBufferPool& stagingBufferPool_;
  render::SectionDeliveryQueue<world::SectionPosition, world::SectionPositionHash>& pendingSectionOrder;
  gameplay::GameSession& gameSession;
  const client::ClientMirror& clientMirror;
  std::function<void(gameplay::GameCommand)> enqueueClientCommand;
  gameplay::SimulationHost& simulationHost;
  world::WorldLock& worldLock;
  ui::UiFrameData& uiFrameData_;
  PerspectiveCamera& camera;
  std::vector<gameplay::entities::SpeciesRenderModel>& speciesModels;
  animation::ModelAnimationSystem& heldItemAnimation;
  animation::PlayerModelAnimator& worldPlayerAnimator;
  CameraPerspective& cameraPerspective;
  float& worldBodyYaw;
  ParticleSystem& particleSystem;
  bool& inventoryOpen;
  bool& spawnPositionInitialized;
  bool& worldReady;
  bool& paused;
  bool& dropRequested;
  bool& dropWholeStack;
  bool& chatOpen;
  std::optional<world::VoxelRaycastHit>& targetedBlock;
  bool& previewOutline;
  double& renderTimeSeconds;
  float& renderInterpolationAlpha;
  GLFWwindow*& window;
  VkInstance& instance;
  VkSurfaceKHR& surface;
  VkDevice& device;
  VmaAllocator& allocator;
  VulkanResources& resources_;
  TextureManager& textures_;
  std::array<VkDescriptorSet, kFramesInFlight>& sceneDescriptorSets;
  GpuSceneBuffer& gpuSceneBuffer;
  OffscreenTarget& shadowTarget;
  VkDescriptorSet& shadowDebugSet;
  std::array<glm::mat4, kSunShadowCascadeCount>& shadowLightViewProj;
  bool& shadowDisabled;
  const config::GameOptions& options;
  render::RainSystem& rainSystem;
  RainMode& rainMode_;
  float& rainTime_;
  ui::Language& language;
  VkExtent2D& swapchainExtent;
  std::vector<VkFramebuffer>& framebuffers;
  std::vector<VkFramebuffer>& guiFramebuffers;
  MenuBlur& menuBlur;
  // TAA-1：resolve 那一趟与它的逐帧状态，所有权在 VulkanRenderer
  TemporalResolve& temporalResolve;
  TemporalFrameState& temporalState;
  std::function<void(VkCommandBuffer, std::uint32_t)> copySceneToSwapchain;
  std::array<FrameContext, kFramesInFlight>& frames;
  std::size_t& currentFrame;
  std::size_t& peakPendingSectionCount;
  float& smoothedFrameSeconds_;
  std::size_t& streamingUploadBudget_;
  std::unordered_map<world::SectionPosition, world::SectionMeshUpdate, world::SectionPositionHash>& pendingSectionUpdates;
  std::unordered_map<world::SectionPosition, std::uint64_t, world::SectionPositionHash>& latestSectionRevisions;
  std::uint64_t& worldEpoch;
  std::size_t& loadedCpuChunkCount;
  std::size_t& completedBlockEditCount;
  std::size_t& completedStreamBatchCount;
  std::size_t& lastVisibleMeshCount;
  bool& worldSessionActive;
  VkDeviceSize& totalUploadedBytes;
  HudRenderer& hud_;

  // ---- 保留在 Impl 里作为唯一来源的相机/玩法回调 ----
  std::function<std::size_t()> rainTargetCount;
  std::function<glm::mat4()> renderViewMatrix;
  std::function<glm::mat4()> viewBobbingMatrix;
  std::function<RenderEye()> renderEyeState;
  std::function<float()> cameraFarPlane;
  std::function<float()> renderDistanceBlocks;
  std::function<void()> initializeSpawnPosition;
  std::function<void(int, int, int, world::Block, std::uint8_t, std::optional<world::BlockOrientation>)> submitWorldEditFn;
  std::function<bool(int, int, int)> hasPersistentEditFn;
  std::function<void(world::ChunkPosition)> onChunkUnloaded;
  std::function<void(world::ChunkPosition)> onChunkLoaded;
  // 引用成员放末位：构造初始化列表的顺序就是声明顺序
  graph::BakedGraph& frameGraph;

  // ---- 本类自有的状态（不再绕经 Impl）----
  // 这些字段只有本类读写。它们曾以 T& 挂在 Bindings 上，而在 Impl 里除了「声明一次、
  // 绑定一次」之外没有任何使用者，是把 Impl 的成员表整体复印过来留下的透传
  // 透传的代价有两层：每加一个状态都要在 Bindings 定义、成员声明、构造初始化列表三处同写；
  // 更要紧的是它让「往 Impl 加成员再引用回来」看起来像是正常做法，债会自我复制
  // 判定标准是「Impl 自身是否读写它」，而不是「谁看起来该拥有它」：像 clientMirror 那样
  // 被 HudRenderer 一并消费的，仍然留在 Bindings 上

  // 箱盖与掉落物运动的数据驱动定义，经动画库求值
  // 箱盖是贝塞尔缓出的合页，掉落物是漂浮加旋转
  // 世界那步 body 数出的可见 section 数。body 经蹦床调用、不返回值，因此走这里回到
  // recordCommandBuffer 的返回值上
  std::size_t graphVisibleCount_ = 0;
  // 本帧五个 pass body 的墙钟合计。graphMs = execute 总时长 − 这个值，于是它量的是
  // 编排本身而不是 body（body 已经被 recordMs 量着）
  double graphBodyMs_ = 0.0;
  animation::HingeAnimation chestLidAnimation;
  animation::DisplayEntityAnimation itemDisplayAnimation;
  bool shadowDebugOverlay = std::getenv("MC_REBEDROCK_SHADOW_DEBUG") != nullptr;
  // 本帧的太阳方向，updateShadowMatrix 写、recordShadow 读。矩阵与投射者排序键必须
  // 出自同一个太阳，否则跨 tick 的那一帧里两者会错开
  glm::vec3 shadowSunDirection_{0.0F, 1.0F, 0.0F};
  // 阴影预通道的逐帧暂存：候选网格、它们的包围盒、以及选中的下标。
  // 成员而非局部变量，clear() 保留容量，稳态下逐帧零分配
  EntityRenderDraws entityDraws_;
  // RN-23：一只实体这一帧的贴花片。同样是成员，clear() 保留容量。
  std::vector<render::EntityShadowPiece> shadowDecalPieces_;
  std::vector<std::size_t> shadowEntitySelection_;
  std::vector<const GpuMesh*> shadowCasterMeshes_;
  std::vector<Aabb> shadowCasterBounds_;
  std::vector<std::size_t> shadowCasterSelection_;
  // RN-22：半透明逐 quad 重排的逐帧暂存与调度游标。同样是成员而非局部变量，
  // clear() 保留容量，稳态下逐帧零分配。
  std::vector<std::uint32_t> translucentIndexScratch_;
  std::vector<GpuMesh*> translucentSections_;
  std::vector<render::TranslucentResortCandidate> translucentCandidates_;
  std::vector<std::size_t> translucentResortSelection_;
  std::size_t translucentResortCursor_ = 0;
  std::size_t translucentResortsThisFrame_ = 0;
  glm::ivec3 lastTranslucentResortBlock_{};
  bool translucentResortInitialized_ = false;
  // 方块粉尘、水花粒子和异步雨共用的 CPU 暂存缓冲，可复用
  // 记录采样世界期间它一直留在主机缓存里，最后一次性整体拷进本帧顺序写映射的存储缓冲
  std::vector<ParticleRecord> sceneParticleRecords_;
  // 上一帧的渲染眼点
  // 遮挡通道据此判断视角是否快到让晚两帧的查询结果已经过期
  bool hasLastRenderEye = false;
  RenderEye lastRenderEye{};
  // 自上一次遮挡校验点以来累计的旋转与平移
  // 晚两帧的 Occluded 结果只在眼点接近静止时才可信
  // 累计运动超过一个小阈值就把整张 occlusionStates 表丢掉，陈旧的 section 因此照画并重新查询
  // 即使是一次始终触发不了逐帧快速运动判定的平滑快扫也照样失效重来
  bool occlusionValidityInitialized = false;
  float occlusionRotationAccumulatorDegrees = 0.0F;
  float occlusionTranslationAccumulator = 0.0F;
  // 流送请求中心同时朝视线方向和移动方向前探，转过身时那片区域已经在生成
  // 前探方向用独立的一份朝向而不是渲染眼点，好让它与第一或第三人称的眼点无关
  // 视角摆动期间由旋转保护取消前探，已加载的区域因此不会来回颠簸
  bool hasLastStreamingForward = false;
  glm::vec3 lastStreamingForward{0.0F, 0.0F, 1.0F};
  std::size_t uploadedSectionsThisFrame = 0;
  VkDeviceSize uploadedBytesThisFrame = 0;

  // 逐 section 的遮挡状态，由遮挡查询结果驱动
  // 某个包围盒不再通过深度测试时，它的不透明网格就被跳过
  std::unordered_map<world::SectionPosition, OcclusionState, world::SectionPositionHash>
      occlusionStates;
  // 逐 section 连续为零的查询次数，用来给 Visible 转 Occluded 这条边加迟滞
  // 需要连着好几次查询都不通过才切换，而不是凭一次擦边的结果
  std::unordered_map<world::SectionPosition, std::uint32_t, world::SectionPositionHash>
      occlusionMissCount;

  // 只打一次的诊断行，各自一个「已打印」标志
  //
  // 它们曾是各自函数体里的 `static bool reported`，有两个毛病：每次进入都要过一道
  // 线程安全初始化的 guard，而这些函数每帧都跑
  // 更实际的毛病是状态挂在进程上，退回标题界面换个世界重进，这批诊断就再也不打印了
  // 而那正是最想看它们的时候
  // 同一个坑在雨的绘制路径上真实发生过一次（见 drawRain 上方的注释：三条分支曾
  // 共用一个 static，运行时切换雨模式后另一条就永远不报了
  //
  // 现在它们随世界纪元复位，每个新世界重新武装一次
  struct OneShotDiagnostics final {
      bool shadowCasters = false;
      bool instancedParticles = false;
      bool textureRain = false;
      bool asyncRain = false;
      bool occlusionScene = false;
  };
  OneShotDiagnostics diagnosticsOnce_{};
  std::uint64_t diagnosticsEpoch_ = 0;

  // 世界重置时把只属于本类、且与旧世界绑定的那些表清空，重置指切换存档或重新生成
  // 这些表原先摊在 Impl 的清理函数里逐张 clear——包括那张纯诊断的环号旁表，
  // 那样 Impl 得知道本类内部有哪些表、哪些该清，正是引用透传留下的习惯
  // 一次性诊断标志不在这里：它们由 refreshDiagnosticsEpoch 按世界纪元复位，
  // 两套机制并存只会让「到底谁负责复位」变得含糊
  void onWorldReset() {
      occlusionStates.clear();
      occlusionMissCount.clear();
      pendingSectionEnqueueRing_.clear();
  }

  // 每帧记录命令前调一次：一次整数比较，纪元没变就什么都不做
  void refreshDiagnosticsEpoch() {
      if (diagnosticsEpoch_ != worldEpoch) {
          diagnosticsEpoch_ = worldEpoch;
          diagnosticsOnce_ = {};
      }
  }
};

} // namespace mc::render
