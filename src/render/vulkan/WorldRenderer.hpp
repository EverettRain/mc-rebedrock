#pragma once
// 世界渲染 / 区块流送 / 遮挡剔除子系统
// 缓冲池、gpuMeshes、设备销毁次序这些 GPU 资源生命周期对崩溃极其敏感
// 因此全部状态仍归 Impl 所有，这里只通过同名引用成员访问，引用在 Bindings 里一次性绑定
// 另有几个 std::function 钩子接留在 Impl 的相机与玩法回调
// 全部内联在头文件里，与 VulkanDevice 同一形态
#include "render/vulkan/BlockAtlasLayout.hpp"
#include "render/vulkan/HudRenderer.hpp"
#include "render/vulkan/HudTypes.hpp"
#include "render/vulkan/WorldRenderTypes.hpp"
#include "render/vulkan/VulkanResources.hpp"
#include "render/vulkan/GpuSceneBuffer.hpp"
#include "render/vulkan/OffscreenTarget.hpp"
#include "render/vulkan/TextureManager.hpp"

#include "core/FrameTrace.hpp"

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
#include "gameplay/GameplayMutationSink.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/ItemEntitySystem.hpp"
#include "gameplay/MiningSystem.hpp"
#include "gameplay/SpawnEggItems.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "gameplay/entities/SpeciesRenderData.hpp"
#include "render/Frustum.hpp"
#include "render/MeshData.hpp"
#include "render/ParticleSystem.hpp"
#include "render/PerspectiveCamera.hpp"
#include "render/RainSystem.hpp"
#include "render/SectionDeliveryQueue.hpp"
#include "render/StreamingBudget.hpp"
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
#include <cmath>
#include <cstddef>
#include <cstdint>
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
    world::ChunkStreamer& chunkStreamer;
    world::World& interactionWorld;
    // 客户端区块缓存：渲染器做网格化和采样所用的世界
    // 模拟侧写 interactionWorld，渲染侧读这份缓存，两边各自拥有自己的区块数据
    world::World& clientCache;
    world::WorldLightEngine& interactionLightEngine;
    std::unordered_map<world::SectionPosition, GpuMesh, world::SectionPositionHash>& gpuMeshes;
    StreamBufferPool& deviceBufferPool_;
    StreamBufferPool& stagingBufferPool_;
    std::array<VkQueryPool, kFramesInFlight>& occlusionQueryPools;
    VkPipeline& occlusionQueryPipeline;
    VkPipelineLayout& occlusionQueryLayout;
    AllocatedBuffer& occlusionBoxVertexBuffer;
    AllocatedBuffer& occlusionBoxIndexBuffer;
    render::SectionDeliveryQueue<world::SectionPosition, world::SectionPositionHash>& pendingSectionOrder;
    world::SmoothLightingQuality& currentMeshQuality;
    world::SmoothLightingQuality& targetMeshQuality;
    std::unordered_set<world::SectionPosition, world::SectionPositionHash>& qualityRemeshPending;
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
    animation::HingeAnimation& chestLidAnimation;
    animation::DisplayEntityAnimation& itemDisplayAnimation;
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
    VkPipeline& particlePipeline;
    VkPipelineLayout& particlePipelineLayout;
    bool& legacyParticles;
    OffscreenTarget& shadowTarget;
    VkPipelineLayout& shadowPipelineLayout;
    VkPipeline& shadowPipeline;
    VkDescriptorSet& shadowDebugSet;
    VkPipelineLayout& shadowDebugPipelineLayout;
    VkPipeline& shadowDebugPipeline;
    glm::mat4& shadowLightViewProj;
    bool& shadowDisabled;
    bool& shadowDebugOverlay;
    render::RainSystem& rainSystem;
    std::vector<ParticleRecord>& sceneParticleRecords_;
    RainMode& rainMode_;
    float& rainTime_;
    VkPipeline& rainSheetPipeline;
    VkPipelineLayout& rainSheetPipelineLayout;
    ui::Language& language;
    VkExtent2D& swapchainExtent;
    VkRenderPass& renderPass;
    VkPipelineLayout& pipelineLayout;
    VkPipeline& graphicsPipeline;
    VkPipeline& translucentPipeline;
    VkPipeline& cutoutPipeline;
    VkPipeline& skyPipeline;
    VkPipelineLayout& outlinePipelineLayout;
    VkPipeline& outlinePipeline;
    VkPipelineLayout& itemPipelineLayout;
    VkPipeline& itemPipeline;
    VkPipeline& itemShadowPipeline;
    VkPipeline& heldItemPipeline;
    std::vector<VkFramebuffer>& framebuffers;
    std::array<FrameContext, kFramesInFlight>& frames;
    std::size_t& currentFrame;
    bool& occlusionDisabled;
    bool& hasLastRenderEye;
    RenderEye& lastRenderEye;
    bool& occlusionValidityInitialized;
    float& occlusionRotationAccumulatorDegrees;
    float& occlusionTranslationAccumulator;
    std::size_t& peakPendingSectionCount;
    float& smoothedFrameSeconds_;
    std::size_t& streamingUploadBudget_;
    std::unordered_map<world::SectionPosition, OcclusionState, world::SectionPositionHash>& occlusionStates;
    std::unordered_map<world::SectionPosition, std::uint32_t, world::SectionPositionHash>& occlusionMissCount;
    std::unordered_map<world::SectionPosition, world::SectionMeshUpdate, world::SectionPositionHash>& pendingSectionUpdates;
    std::unordered_map<world::SectionPosition, std::uint64_t, world::SectionPositionHash>& latestSectionRevisions;
    std::uint64_t& worldEpoch;
    std::size_t& loadedCpuChunkCount;
    std::size_t& completedBlockEditCount;
    std::size_t& completedStreamBatchCount;
    std::size_t& lastVisibleMeshCount;
    bool& worldSessionActive;
    bool& hasLastStreamingForward;
    glm::vec3& lastStreamingForward;
    std::size_t& uploadedSectionsThisFrame;
    VkDeviceSize& uploadedBytesThisFrame;
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
  };

  explicit WorldRenderer(const Bindings& b)
      : testScene(b.testScene), chunkStreamer(b.chunkStreamer),
        interactionWorld(b.interactionWorld), clientCache(b.clientCache),
        interactionLightEngine(b.interactionLightEngine),
        gpuMeshes(b.gpuMeshes), deviceBufferPool_(b.deviceBufferPool_),
        stagingBufferPool_(b.stagingBufferPool_), occlusionQueryPools(b.occlusionQueryPools),
        occlusionQueryPipeline(b.occlusionQueryPipeline),
        occlusionQueryLayout(b.occlusionQueryLayout),
        occlusionBoxVertexBuffer(b.occlusionBoxVertexBuffer),
        occlusionBoxIndexBuffer(b.occlusionBoxIndexBuffer),
        pendingSectionOrder(b.pendingSectionOrder), currentMeshQuality(b.currentMeshQuality),
        targetMeshQuality(b.targetMeshQuality), qualityRemeshPending(b.qualityRemeshPending),
        gameSession(b.gameSession), clientMirror(b.clientMirror),
        enqueueClientCommand(b.enqueueClientCommand),
        simulationHost(b.simulationHost), worldLock(b.worldLock), uiFrameData_(b.uiFrameData_),
        camera(b.camera), speciesModels(b.speciesModels), heldItemAnimation(b.heldItemAnimation),
        worldPlayerAnimator(b.worldPlayerAnimator),
        chestLidAnimation(b.chestLidAnimation),
        itemDisplayAnimation(b.itemDisplayAnimation), cameraPerspective(b.cameraPerspective),
        worldBodyYaw(b.worldBodyYaw), particleSystem(b.particleSystem),
        inventoryOpen(b.inventoryOpen),
        spawnPositionInitialized(b.spawnPositionInitialized), worldReady(b.worldReady),
        paused(b.paused), dropRequested(b.dropRequested), dropWholeStack(b.dropWholeStack),
        chatOpen(b.chatOpen), targetedBlock(b.targetedBlock),
        renderTimeSeconds(b.renderTimeSeconds),
        renderInterpolationAlpha(b.renderInterpolationAlpha), window(b.window),
        instance(b.instance), surface(b.surface), device(b.device), allocator(b.allocator),
        resources_(b.resources_), textures_(b.textures_),
        sceneDescriptorSets(b.sceneDescriptorSets), gpuSceneBuffer(b.gpuSceneBuffer),
        particlePipeline(b.particlePipeline), particlePipelineLayout(b.particlePipelineLayout),
        legacyParticles(b.legacyParticles), shadowTarget(b.shadowTarget),
        shadowPipelineLayout(b.shadowPipelineLayout), shadowPipeline(b.shadowPipeline),
        shadowDebugSet(b.shadowDebugSet), shadowDebugPipelineLayout(b.shadowDebugPipelineLayout),
        shadowDebugPipeline(b.shadowDebugPipeline), shadowLightViewProj(b.shadowLightViewProj),
        shadowDisabled(b.shadowDisabled), shadowDebugOverlay(b.shadowDebugOverlay),
        rainSystem(b.rainSystem), sceneParticleRecords_(b.sceneParticleRecords_),
        rainMode_(b.rainMode_), rainTime_(b.rainTime_), rainSheetPipeline(b.rainSheetPipeline),
        rainSheetPipelineLayout(b.rainSheetPipelineLayout), language(b.language),
        swapchainExtent(b.swapchainExtent), renderPass(b.renderPass),
        pipelineLayout(b.pipelineLayout), graphicsPipeline(b.graphicsPipeline),
        translucentPipeline(b.translucentPipeline), cutoutPipeline(b.cutoutPipeline),
        skyPipeline(b.skyPipeline), outlinePipelineLayout(b.outlinePipelineLayout),
        outlinePipeline(b.outlinePipeline), itemPipelineLayout(b.itemPipelineLayout),
        itemPipeline(b.itemPipeline), itemShadowPipeline(b.itemShadowPipeline),
        heldItemPipeline(b.heldItemPipeline), framebuffers(b.framebuffers), frames(b.frames),
        currentFrame(b.currentFrame), occlusionDisabled(b.occlusionDisabled),
        hasLastRenderEye(b.hasLastRenderEye), lastRenderEye(b.lastRenderEye),
        occlusionValidityInitialized(b.occlusionValidityInitialized),
        occlusionRotationAccumulatorDegrees(b.occlusionRotationAccumulatorDegrees),
        occlusionTranslationAccumulator(b.occlusionTranslationAccumulator),
        peakPendingSectionCount(b.peakPendingSectionCount),
        smoothedFrameSeconds_(b.smoothedFrameSeconds_),
        streamingUploadBudget_(b.streamingUploadBudget_), occlusionStates(b.occlusionStates),
        occlusionMissCount(b.occlusionMissCount), pendingSectionUpdates(b.pendingSectionUpdates),
        latestSectionRevisions(b.latestSectionRevisions), worldEpoch(b.worldEpoch),
        loadedCpuChunkCount(b.loadedCpuChunkCount),
        completedBlockEditCount(b.completedBlockEditCount),
        completedStreamBatchCount(b.completedStreamBatchCount),
        lastVisibleMeshCount(b.lastVisibleMeshCount), worldSessionActive(b.worldSessionActive),
        hasLastStreamingForward(b.hasLastStreamingForward),
        lastStreamingForward(b.lastStreamingForward),
        uploadedSectionsThisFrame(b.uploadedSectionsThisFrame),
        uploadedBytesThisFrame(b.uploadedBytesThisFrame),
        totalUploadedBytes(b.totalUploadedBytes), hud_(b.hud_), rainTargetCount(b.rainTargetCount),
        renderViewMatrix(b.renderViewMatrix), viewBobbingMatrix(b.viewBobbingMatrix),
        renderEyeState(b.renderEyeState), cameraFarPlane(b.cameraFarPlane),
        renderDistanceBlocks(b.renderDistanceBlocks),
        initializeSpawnPosition(b.initializeSpawnPosition), submitWorldEditFn(b.submitWorldEditFn),
        hasPersistentEditFn(b.hasPersistentEditFn), onChunkUnloaded(b.onChunkUnloaded),
        onChunkLoaded(b.onChunkLoaded) {
  }

  WorldRenderer(const WorldRenderer&) = delete;
  WorldRenderer& operator=(const WorldRenderer&) = delete;

  // 每个待上传 section 入队时的身份信息，供上传侧的投递顺序诊断使用
  // 它必须按**真正请求它的那个中心**评分，而不是最新的请求中心
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
            // 首次网格延迟在 processChunkStreaming 里、区块**进入请求半径**那一刻开始计时
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
                std::getenv("MC_REBEDROCK_SMOKE_TEST") != nullptr) {
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
                std::getenv("MC_REBEDROCK_SMOKE_TEST") != nullptr) {
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
            // 该 section 相对**本批次自己的**请求中心的切比雪夫环号
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
                            // 补救请求用**普通**优先级，因为被淘汰的是排队环里**最远**的那个
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
        auto vertexStaging = acquireStreamBuffer(stagingBufferPool_, vertexBytes,
                                                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        auto indexStaging = acquireStreamBuffer(stagingBufferPool_, indexBytes,
                                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
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
            // 画质重网格所等待的某个 section 已被上传或退役
            // 它不再阻塞底部那次 uniform 切换
            qualityRemeshPending.erase(position);

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
            uploadRenderMesh(frame, update.mesh, gpuMesh);
            // 工作线程把这个网格建在池化的 RenderMeshData 上；归还它，容量供下一个 section 构建复用
            chunkStreamer.releaseMeshData(std::move(update.mesh));
            gpuMeshes.insert_or_assign(position, gpuMesh);
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
        // 等按 targetMeshQuality 重烘的 section 全部落地后，再切换着色器 High 分支期望的画质
        // 否则它会用新的 AO 曲线去读一个仍是 Standard 的旧网格，反之亦然
        if (qualityRemeshPending.empty() && currentMeshQuality != targetMeshQuality) {
            currentMeshQuality = targetMeshQuality;
        }
    }


    void updateShadowMatrix() {
        if (shadowDisabled) {
            return;
        }
        const auto daylight = world::DayNightCycle::stateAtTick(
            clientMirror.world().dayTimeTicks);
        const glm::vec3 sun = glm::normalize(daylight.sunDirection);
        const glm::vec3 eye = camera.position();
        const glm::mat4 lightView =
            glm::lookAt(eye + sun * 96.0F, eye - sun * 96.0F, glm::vec3{0.0F, 1.0F, 0.0F});
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
        // 限制预通道的绘制量，视点飞高或光锥覆盖密集区域时投射者列表能涨到数千
        // 每帧全部重画正是那种可能把设备推向丢失的重负载帧
        // 只保留最近的 512 个
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
        // 即使一个投射者都没有也照样开始并结束该通道
        // 这样深度图像每帧结束时都处于 SHADER_READ_ONLY_OPTIMAL
        // 调试叠加层无条件采样它，跳过转换的那一帧会让它停在 UNDEFINED 并触发校验层报错
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
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowDebugPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                shadowDebugPipelineLayout, 0, 1, &shadowDebugSet, 0, nullptr);
        vkCmdPushConstants(commandBuffer, shadowDebugPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(rect), &rect);
        vkCmdDraw(commandBuffer, 6U, 1, 0, 0);
    }

    void drawItemEntities(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet) const {
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
        if (!snapshotItems.empty()) {
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, itemShadowPipeline);
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    itemPipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
            for (const auto& entity : snapshotItems) {
                const glm::vec3 renderedPosition =
                    entity.previousPosition +
                    (entity.position - entity.previousPosition) * itemAlpha;
                const int startY = static_cast<int>(std::floor(renderedPosition.y));
                std::optional<float> groundY;
                for (int y = startY; y >= std::max(0, startY - 12); --y) {
                    if (world::hasCollision(clientCache.block(
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
        // 手持物生成的 2.5D 薄片顶点数，对应 item_entity.vert 的 data.x 落在 (6.5,7.5) 模式
        // 计为正反面 12 个顶点，加上 16x16 的边缘四边形
        constexpr std::uint32_t kGeneratedItemVertexCount = 12U + 16U * 16U * 4U * 6U;
        // 该模式把视图矩阵烘进变换后再投影，即 gl_Position = projection * viewModelTransform
        // 掉落物因此和世界用同一套视图，包括视角摇晃
        const glm::mat4 cameraView = viewBobbingMatrix() * renderViewMatrix();
        for (const auto& entity : snapshotItems) {
            const glm::vec3 renderedPosition =
                entity.previousPosition +
                (entity.position - entity.previousPosition) * itemAlpha;
            // 台阶是方块而不是扁平的 2.5D 图标
            // vanilla 画的是它的方块模型，一个平躺的半高盒子
            // 而不是 else 分支那种立着旋转的挤出贴图
            // 它走立方体路径，Y 向尺寸减半（见下面的 slabDrop）
            const bool slabDrop =
                gameplay::isBlockStack(entity.stack) && world::isSlab(entity.stack.block);
            const bool cubeModel =
                gameplay::isBlockStack(entity.stack) &&
                (world::blockDefinition(entity.stack.block).model == world::BlockModel::Cube ||
                 world::blockDefinition(entity.stack.block).model == world::BlockModel::Chest ||
                 slabDrop);
            const auto layers =
                cubeModel ? world::textureLayers(entity.stack.block)
                          : world::BlockTextureLayers{gameplay::itemTextureLayer(entity.stack),
                                                      gameplay::itemTextureLayer(entity.stack),
                                                      gameplay::itemTextureLayer(entity.stack)};
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
            if (cubeModel) {
                // 台阶把盒子的 Y 向尺寸减半，使它像 vanilla 的物品模型那样平躺
                // 其它立方体沿用标量尺寸（xyz 为零时着色器回落到 positionSize.w）
                const glm::vec4 dimensions =
                    slabDrop ? glm::vec4{0.30F, 0.15F, 0.30F, packedLight}
                             : glm::vec4{0.0F, 0.0F, 0.0F, packedLight};
                const ItemPush push{
                    {renderedPosition.x, renderedPosition.y + 0.18F + bob, renderedPosition.z,
                     0.30F},
                    {layers.top, layers.side, layers.bottom, rotation},
                    // data.z 是滚转角，在立方体路径上没用，这里借来标记台阶
                    // 着色器据此在侧面显示纹理的下半条
                    {1.0F, 0.0F, slabDrop ? 1.0F : 0.0F, 0.0F},
                    dimensions,
                };
                vkCmdPushConstants(commandBuffer, itemPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                                   sizeof(push), &push);
                vkCmdDraw(commandBuffer, 36U, 1, 0, 0);
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
                const ItemPush push{
                    {0.0F, 0.0F, 0.0F, 0.30F},  {layers.top, layers.side, layers.bottom, 0.0F},
                    {7.0F, 0.0F, 0.0F, 0.0F},   {1.0F, 1.0F, 0.0625F, packedLight},
                    cameraView * dropTransform,
                };
                vkCmdPushConstants(commandBuffer, itemPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                                   sizeof(push), &push);
                vkCmdDraw(commandBuffer, kGeneratedItemVertexCount, 1, 0, 0);
            }
        }
        for (const auto& entity : snapshotFallingBlocks) {
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
                {1.0F, 0.0F, 0.0F, 2.0F},
                {0.0F, 0.0F, 0.0F, packedSceneLight(renderedPosition)},
            };
            vkCmdPushConstants(commandBuffer, itemPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                               sizeof(push), &push);
            vkCmdDraw(commandBuffer, 36, 1, 0, 0);
        }
        // 经验球是一小块面向相机的球体贴图公告板
        // 它走粒子同一条平面公告板路径，即 item_entity.vert 的 data.x == -1
        // 整个预留图集层就是这张贴图，因此 uvOrigin 取 (0,0)、uvScale 取 1
        // vanilla 的经验球还会上下浮动并循环变色，这里先做静态版本
        for (const auto& orb : snapshotOrbs) {
            const glm::vec3 renderedPosition =
                orb.previousPosition + (orb.position - orb.previousPosition) * itemAlpha;
            const glm::vec3 billboardCentre = renderedPosition + glm::vec3{0.0F, 0.25F, 0.0F};
            const ItemPush push{
                {billboardCentre.x, billboardCentre.y, billboardCentre.z, 0.3F},
                {kExperienceOrbLayer, 0.0F, 0.0F, 1.0F},
                {-1.0F, 0.0F, 0.0F, 1.0F},
                {0.0F, 0.0F, 0.0F, packedSceneLight(billboardCentre)},
            };
            vkCmdPushConstants(commandBuffer, itemPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                               sizeof(push), &push);
            vkCmdDraw(commandBuffer, 6U, 1, 0, 0);
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
        // MC_REBEDROCK_LEGACY_PARTICLES 保留逐粒子推送常量的旧绘制方式，便于与实例化路径直接对比
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
        // 复用一个常驻的暂存向量，而不是每帧临时分配
        // 世界与光照的读取都留在 VMA 的顺序写映射之外，最后一次性整体拷进去
        // 在把该映射暴露为 write-combined 内存的 Windows 堆上，这样明显更快
        auto& buffer = gpuSceneBuffer.frame(currentFrame);
        sceneParticleRecords_.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            const auto& particle = particles[index];
            sceneParticleRecords_.push_back(ParticleRecord{
                {particle.position.x, particle.position.y, particle.position.z, particle.size},
                {particle.uvOrigin.x, particle.uvOrigin.y, particle.uvScale, particle.opacity},
                {particle.textureLayer, packedSceneLight(particle.position), 0.0F, 0.0F},
            });
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
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, particlePipeline);
        const std::array<VkDescriptorSet, 2> sets{descriptorSet, sceneDescriptorSets[currentFrame]};
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                particlePipelineLayout, 0, 2, sets.data(), 0, nullptr);
        vkCmdDraw(commandBuffer, 6U, static_cast<std::uint32_t>(count), 0, 0);
        static bool reported = false;
        if (!reported) {
            reported = true;
            std::cout << "[particles] instanced 1 draw for " << count
                      << " records (legacy = " << particles.size() << " draws)\n";
        }
        return count;
    }

    // 三条路径之一绘制降雨
    // Particles 与 Async 用同一批 CPU 雨滴，好让两个绘制后端可比
    // Texture 走 vanilla 独立的逐列降水通道：
    //   texture   -> 用 environment/rain.png 画窄长的竖直雨列
    //   particles -> 旧的逐粒子物品管线公告板
    //   async     -> 从场景存储缓冲发起一次实例化绘制，baseInstance 指到方块粉尘记录之后

    void drawRain(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet,
                  std::size_t baseRecordCount) {
        const auto& drops = rainSystem.drops();
        static bool reported = false;
        if (rainMode_ == RainMode::Texture) {
            const float rainGradient = clientMirror.world().rainGradient;
            if (rainGradient <= 0.02F) {
                return;
            }
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, rainSheetPipeline);
            const std::array<VkDescriptorSet, 2> sets{descriptorSet,
                                                      sceneDescriptorSets[currentFrame]};
            vkCmdBindDescriptorSets(
                commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, rainSheetPipelineLayout, 0,
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
                    constexpr std::uint64_t kJavaRandomMultiplier = 0x5DEECE66DULL;
                    constexpr std::uint64_t kJavaRandomAddend = 0xBULL;
                    constexpr std::uint64_t kJavaRandomMask = (1ULL << 48U) - 1ULL;
                    std::uint64_t randomState =
                        (static_cast<std::uint64_t>(static_cast<std::int64_t>(randomSeed)) ^
                         kJavaRandomMultiplier) &
                        kJavaRandomMask;
                    randomState =
                        (randomState * kJavaRandomMultiplier + kJavaRandomAddend) & kJavaRandomMask;
                    const float randomFloat = static_cast<float>(randomState >> 24U) / 16777216.0F;
                    const float tickTime = rainTime_ * 20.0F;
                    const std::uint32_t phaseTick =
                        (static_cast<std::uint32_t>(std::floor(tickTime)) + xSeed + zSeed) & 31U;
                    const float partialTick = tickTime - std::floor(tickTime);
                    const float scroll = -(static_cast<float>(phaseTick) + partialTick) / 32.0F *
                                         (3.0F + randomFloat);
                    const float packedLight = packedSceneLight(
                        {columnX, std::max(surface, static_cast<float>(cameraY)) + 0.1F, columnZ});
                    sceneParticleRecords_.push_back(ParticleRecord{
                        {columnX, bottom, columnZ, 0.5F},
                        {top, opacity, scroll, packedLight},
                        {tangent.x, tangent.y, 0.0F, 0.0F},
                    });
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
            if (!reported) {
                reported = true;
                std::cout << "[rain] mode=texture vanilla-columns=" << columnCount
                          << " texture=environment/rain.png draws=1\n";
            }
            return;
        }
        if (drops.empty()) {
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
        // 异步路径把雨的记录接在方块粉尘记录之后，写进同一个场景缓冲
        // 再用越过它们的 baseInstance 一次画完
        const std::size_t capacity = gpuSceneBuffer.capacityBytes() / sizeof(ParticleRecord);
        const std::size_t count = std::min(drops.size(), capacity - baseRecordCount);
        auto& buffer = gpuSceneBuffer.frame(currentFrame);
        sceneParticleRecords_.reserve(baseRecordCount + count);
        for (std::size_t index = 0; index < count; ++index) {
            const auto& drop = drops[index];
            sceneParticleRecords_.push_back(ParticleRecord{
                {drop.position.x, drop.position.y, drop.position.z, drop.size},
                {0.0F, 0.0F, 1.0F, 0.6F},
                {static_cast<float>(kWaterStillLayer), packedSceneLight(drop.position), 0.0F, 0.0F},
            });
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
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, particlePipeline);
        const std::array<VkDescriptorSet, 2> sets{descriptorSet, sceneDescriptorSets[currentFrame]};
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                particlePipelineLayout, 0, 2, sets.data(), 0, nullptr);
        vkCmdDraw(commandBuffer, 6U, static_cast<std::uint32_t>(count), 0,
                  static_cast<std::uint32_t>(baseRecordCount));
        if (!reported && count >= rainTargetCount() * 9U / 10U) {
            reported = true;
            std::cout << "[rain] mode=async drops=" << count << " draws=1\n";
        }
    }

    // 用一个完整世界矩阵变换、经物品着色器的世界空间蒙皮长方体模式（data.x = 8）画一个轴对齐长方体
    // 矩阵携带平移与朝向；`dimensions` 是世界单位下的盒子尺寸；六个面采样纹理层 [layer, layer+5]
    // 箱子与第三人称玩家共用它，于是部件的旋转绑定在自己的局部坐标系上，而不是某个固定世界轴
    // `packedLight` 是该实体的场景光照采样（见 packedSceneLight），传 0 则使用固定光照
    // 今后任何经此绘制的方块实体，只要传入它就能获得场景光照

    void pushWorldCuboid(VkCommandBuffer commandBuffer, const glm::mat4& worldMatrix,
                         glm::vec3 dimensions, float textureLayer, float packedLight = 0.0F) const {
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
        const auto& chests = clientMirror.world().chests;
        if (chests.empty())
            return;
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, itemPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, itemPipelineLayout,
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

    void drawWorldPlayer(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet) const {
        if (cameraPerspective == CameraPerspective::FirstPerson || !worldReady) {
            return;
        }
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, itemPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, itemPipelineLayout,
                                0, 1, &descriptorSet, 0, nullptr);

        // 相机位于插值后的眼点；模型以脚为锚点
        const glm::vec3 feet =
            camera.position() -
            glm::vec3{0.0F, clientMirror.player().sneaking
                          ? gameplay::PlayerController::kSneakingEyeHeight
                          : gameplay::PlayerController::kEyeHeight,
                      0.0F};
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
                pushWorldCuboid(commandBuffer, cubeWorld, cube.renderSize(), layer, packedLight);
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

    void pushBoxUvCuboid(VkCommandBuffer commandBuffer, const glm::mat4& worldMatrix,
                         glm::vec3 renderSize, glm::vec3 uvSize, glm::vec2 uv, bool mirror,
                         glm::vec2 textureSize, std::uint32_t faceOverride, float layer,
                         std::uint32_t woolTint = 0xFFFFFFU, float packedLight = 0.0F,
                         float hurtFlash = 0.0F) const {
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
            {9.0F, uv.x, uv.y, mirror ? 1.0F : 0.0F},
            {renderSize.x, renderSize.y, renderSize.z,
             packedLight + (hurtFlash > 0.5F ? 512.0F : 0.0F)},
            worldMatrix,
        };
        vkCmdPushConstants(commandBuffer, itemPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(push), &push);
        vkCmdDraw(commandBuffer, 36U, 1, 0, 0);
    }

    // 把自由活动的生物渲染成 box-UV 蒙皮模型，动画库与掉落物相同，同样在物理 tick 之间插值

    void drawWorldEntities(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet) const {
        // 从逐 tick 快照绘制，绝不读实时实体容器
        // 模拟跑在自己线程上，本通道遍历期间那个容器正在被重排和扩缩
        // 快照是按值拷贝，先绑到局部变量，其 entities() 引用才有效
        const auto& snapshot = clientMirror.entities();
        const auto& snapshotEntities = snapshot.entities();
        if (!worldReady || snapshotEntities.empty()) {
            return;
        }
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, itemPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, itemPipelineLayout,
                                0, 1, &descriptorSet, 0, nullptr);
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
                    pushBoxUvCuboid(commandBuffer, cubeWorld, cube.renderSize(), cube.size, cube.uv,
                                    cube.mirror, textureSize, cube.faceOverride,
                                    boneLayer, woolTint, packedLight, hurtFlash);
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
        // 打开背包或聊天时手持物依然可见，与原版一致；只有暂停或非第一人称视角才隐藏手
        if (!worldReady || paused || cameraPerspective != CameraPerspective::FirstPerson) {
            return;
        }
        const auto& stack = uiFrameData_.selectedStack;
        const bool emptyHand = stack.empty();
        const auto& pose = heldItemAnimation.pose();
        // 手持台阶用的是它的方块模型——平持的半高盒子，而不是立着的挤出贴图
        // 它走立方体路径，Y 向尺寸减半（见下面的 heldDimensions）
        const bool heldSlab =
            !emptyHand && gameplay::isBlockStack(stack) && world::isSlab(stack.block);
        const bool cubeModel =
            !emptyHand && gameplay::isBlockStack(stack) &&
            (world::blockDefinition(stack.block).model == world::BlockModel::Cube ||
             world::blockDefinition(stack.block).model == world::BlockModel::Chest ||
             world::blockDefinition(stack.block).model == world::BlockModel::DirectionalCube ||
             heldSlab);
        const auto layers =
            emptyHand
                ? world::BlockTextureLayers{kPlayerRightArmFirstLayer, kPlayerRightArmFirstLayer,
                                            kPlayerRightArmFirstLayer}
            : cubeModel ? world::textureLayers(stack.block)
                        : world::BlockTextureLayers{gameplay::itemTextureLayer(stack),
                                                    gameplay::itemTextureLayer(stack),
                                                    gameplay::itemTextureLayer(stack)};
        const glm::mat4 heldTransform =
            viewBobbingMatrix() * (emptyHand ? animation::firstPersonArmTransform(pose)
                                   : uiFrameData_.eating
                                       ? animation::firstPersonEatTransform(pose, cubeModel)
                                       : animation::firstPersonItemTransform(pose, cubeModel));
        const float heldFrontLayer =
            !emptyHand && gameplay::isBlockStack(stack)
                ? (stack.block == world::Block::Chest
                       ? kChestItemFrontLayer
                       : (world::blockDefinition(stack.block).model ==
                                  world::BlockModel::DirectionalCube
                              ? world::directionalLayers(stack.block).front
                              : 0.0F))
                : 0.0F;
        // 手与手持方块跟随玩家眼部的环境光，夜里会变暗，而不是永远处在固定光照下
        const float heldLight = packedSceneLight(camera.position());
        const ItemPush push{
            {0.0F, 0.0F, 0.0F, 1.0F},
            {layers.top, layers.side, layers.bottom, heldFrontLayer},
            {(!emptyHand && !cubeModel) ? 7.0F : 6.0F, 0.0F,
             emptyHand ? 1.0F : (heldSlab ? 1.0F : 0.0F), emptyHand ? 1.0F : 0.0F},
            emptyHand ? glm::vec4{0.25F, 0.75F, 0.25F, heldLight}
                      : (cubeModel ? glm::vec4{1.0F, heldSlab ? 0.5F : 1.0F, 1.0F, heldLight}
                                   : glm::vec4{1.0F, 1.0F, 0.0625F, heldLight}),
            heldTransform,
        };
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, heldItemPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, itemPipelineLayout,
                                0, 1, &descriptorSet, 0, nullptr);
        vkCmdPushConstants(commandBuffer, itemPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(push), &push);
        constexpr std::uint32_t generatedItemVertexCount = 12U + 16U * 16U * 4U * 6U;
        vkCmdDraw(commandBuffer, !emptyHand && !cubeModel ? generatedItemVertexCount : 36U, 1, 0,
                  0);
    }


    [[nodiscard]] std::size_t recordCommandBuffer(FrameContext& frame, std::uint32_t imageIndex) {
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
        const VkQueryPool frameQueryPool = occlusionQueryPools[currentFrame];
        if (frameQueryPool != VK_NULL_HANDLE) {
            // 复用前先清空本帧的槽位区间
            // 上一次提交的结果已在本次 drawFrame 里读回
            // 每帧独占自己的查询池，槽位因此总是从零开始
            vkCmdResetQueryPool(frame.commandBuffer, frameQueryPool, 0U,
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
        // 太阳空间阴影预通道写出一张离屏深度图，供主通道（和调试叠加层）采样
        // 它必须排在上面的网格上传之后、主渲染通道之前
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
            if (!occlusionDisabled && frameQueryPool != VK_NULL_HANDLE && withinQueryBudget) {
                vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  occlusionQueryPipeline);
                vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        occlusionQueryLayout, 0, 1, &frame.descriptorSet, 0,
                                        nullptr);
                const VkDeviceSize boxOffset = 0;
                vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &occlusionBoxVertexBuffer.buffer,
                                       &boxOffset);
                vkCmdBindIndexBuffer(frame.commandBuffer, occlusionBoxIndexBuffer.buffer, 0,
                                     VK_INDEX_TYPE_UINT32);
                const OcclusionQueryPushConstants push{
                    glm::vec4{mesh.bounds.minimum, 1.0F},
                    glm::vec4{mesh.bounds.maximum, 1.0F},
                };
                vkCmdPushConstants(frame.commandBuffer, occlusionQueryLayout,
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
            if (!occlusionDisabled && withinQueryBudget && !cameraMovingFast &&
                state == OcclusionState::Occluded) {
                continue;
            }
            if (mesh.opaque.indexCount > 0U) {
                vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  graphicsPipeline);
                vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        pipelineLayout, 0, 1, &frame.descriptorSet, 0, nullptr);
                vkCmdPushConstants(frame.commandBuffer, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
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
            vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, cutoutPipeline);
            // 遮挡通道可能把查询管线的描述符集留在绑定状态，因此 cutout 管线在绘制前重新绑定自己的
            vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    pipelineLayout, 0, 1, &frame.descriptorSet, 0, nullptr);
            for (const auto* mesh : visibleCutoutMeshes) {
                vkCmdPushConstants(frame.commandBuffer, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
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
                vkCmdPushConstants(frame.commandBuffer, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                                   0, sizeof(glm::vec4), &mesh->sectionOrigin);
                vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &mesh->vertexBuffer.buffer,
                                       &mesh->translucent.vertexOffset);
                vkCmdBindIndexBuffer(frame.commandBuffer, mesh->indexBuffer.buffer,
                                     mesh->translucent.indexOffset, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(frame.commandBuffer, mesh->translucent.indexCount, 1, 0, 0, 0);
            }
        }
        // 粒子排在半透明地形通道之后，与 vanilla 的绘制位置一致
        const bool appendAsyncRain = rainMode_ == RainMode::Async && !rainSystem.drops().empty();
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
        if (!inventoryOpen && !paused && !chatOpen && targetedBlock.has_value() && !targetIsFire) {
            // 选择框描的是方块的真实形状，火把、植物、箱子、台阶这类非满方块不再显示成整格框
            // 形状取自该格的状态（台阶上下半、作物生长阶段等）
            const world::BlockBounds bounds =
                world::blockSelectionBounds(clientCache, targetedBlock->block);
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
        // 游戏内 HUD 层以及叠在它上面的各种界面，都由 drawHud 按 vanilla 的层序绘制
        // HUD 层含手持物、水下叠加、暗角、快捷栏、状态条、准星和手持物名称
        hud_.drawHud(frame.commandBuffer, frame.descriptorSet);
        drawShadowDebugOverlay(frame.commandBuffer);
        vkCmdEndRenderPass(frame.commandBuffer);
        checkVk(vkEndCommandBuffer(frame.commandBuffer), "vkEndCommandBuffer");
        return visibleCount;
    }

    // 应用两次提交之前记录的遮挡查询结果
    // 本帧的围栏刚等待完毕，那批查询必然已完成
    // 在本帧槽位区间被重置复用之前于此读取，逐 section 的绘制门控就恰好保持两帧延迟

    void readBackOcclusionQueries() {
        const VkQueryPool frameQueryPool = occlusionQueryPools[currentFrame];
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
  world::ChunkStreamer& chunkStreamer;
  world::World& interactionWorld;
  world::World& clientCache;
  world::WorldLightEngine& interactionLightEngine;
  std::unordered_map<world::SectionPosition, GpuMesh, world::SectionPositionHash>& gpuMeshes;
  StreamBufferPool& deviceBufferPool_;
  StreamBufferPool& stagingBufferPool_;
  std::array<VkQueryPool, kFramesInFlight>& occlusionQueryPools;
  VkPipeline& occlusionQueryPipeline;
  VkPipelineLayout& occlusionQueryLayout;
  AllocatedBuffer& occlusionBoxVertexBuffer;
  AllocatedBuffer& occlusionBoxIndexBuffer;
  render::SectionDeliveryQueue<world::SectionPosition, world::SectionPositionHash>& pendingSectionOrder;
  world::SmoothLightingQuality& currentMeshQuality;
  world::SmoothLightingQuality& targetMeshQuality;
  std::unordered_set<world::SectionPosition, world::SectionPositionHash>& qualityRemeshPending;
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
  animation::HingeAnimation& chestLidAnimation;
  animation::DisplayEntityAnimation& itemDisplayAnimation;
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
  VkPipeline& particlePipeline;
  VkPipelineLayout& particlePipelineLayout;
  bool& legacyParticles;
  OffscreenTarget& shadowTarget;
  VkPipelineLayout& shadowPipelineLayout;
  VkPipeline& shadowPipeline;
  VkDescriptorSet& shadowDebugSet;
  VkPipelineLayout& shadowDebugPipelineLayout;
  VkPipeline& shadowDebugPipeline;
  glm::mat4& shadowLightViewProj;
  bool& shadowDisabled;
  bool& shadowDebugOverlay;
  render::RainSystem& rainSystem;
  std::vector<ParticleRecord>& sceneParticleRecords_;
  RainMode& rainMode_;
  float& rainTime_;
  VkPipeline& rainSheetPipeline;
  VkPipelineLayout& rainSheetPipelineLayout;
  ui::Language& language;
  VkExtent2D& swapchainExtent;
  VkRenderPass& renderPass;
  VkPipelineLayout& pipelineLayout;
  VkPipeline& graphicsPipeline;
  VkPipeline& translucentPipeline;
  VkPipeline& cutoutPipeline;
  VkPipeline& skyPipeline;
  VkPipelineLayout& outlinePipelineLayout;
  VkPipeline& outlinePipeline;
  VkPipelineLayout& itemPipelineLayout;
  VkPipeline& itemPipeline;
  VkPipeline& itemShadowPipeline;
  VkPipeline& heldItemPipeline;
  std::vector<VkFramebuffer>& framebuffers;
  std::array<FrameContext, kFramesInFlight>& frames;
  std::size_t& currentFrame;
  bool& occlusionDisabled;
  bool& hasLastRenderEye;
  RenderEye& lastRenderEye;
  bool& occlusionValidityInitialized;
  float& occlusionRotationAccumulatorDegrees;
  float& occlusionTranslationAccumulator;
  std::size_t& peakPendingSectionCount;
  float& smoothedFrameSeconds_;
  std::size_t& streamingUploadBudget_;
  std::unordered_map<world::SectionPosition, OcclusionState, world::SectionPositionHash>& occlusionStates;
  std::unordered_map<world::SectionPosition, std::uint32_t, world::SectionPositionHash>& occlusionMissCount;
  std::unordered_map<world::SectionPosition, world::SectionMeshUpdate, world::SectionPositionHash>& pendingSectionUpdates;
  std::unordered_map<world::SectionPosition, std::uint64_t, world::SectionPositionHash>& latestSectionRevisions;
  std::uint64_t& worldEpoch;
  std::size_t& loadedCpuChunkCount;
  std::size_t& completedBlockEditCount;
  std::size_t& completedStreamBatchCount;
  std::size_t& lastVisibleMeshCount;
  bool& worldSessionActive;
  bool& hasLastStreamingForward;
  glm::vec3& lastStreamingForward;
  std::size_t& uploadedSectionsThisFrame;
  VkDeviceSize& uploadedBytesThisFrame;
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
};

} // namespace mc::render
