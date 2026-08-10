#pragma once
// WorldRenderer: the world-render / streaming / occlusion subsystem extracted
// verbatim from VulkanRenderer::Impl. To keep the crash-sensitive GPU-resource
// lifecycle (buffer pools / gpuMeshes / device teardown ordering) exactly where
// it is, all state stays owned by Impl and is reached here through same-named
// reference members (bound once via Bindings); a few std::function hooks cover
// camera/gameplay callbacks that stay in Impl. Header-only inline (as VulkanDevice).
#include "render/vulkan/HudRenderer.hpp"
#include "render/vulkan/HudTypes.hpp"
#include "render/vulkan/WorldRenderTypes.hpp"
#include "render/vulkan/VulkanResources.hpp"
#include "render/vulkan/GpuSceneBuffer.hpp"
#include "render/vulkan/OffscreenTarget.hpp"
#include "render/vulkan/TextureManager.hpp"

#include "animation/AnimationAssets.hpp"
#include "animation/DisplayEntityAnimation.hpp"
#include "animation/HingeAnimation.hpp"
#include "animation/ModelAnimationSystem.hpp"
#include "animation/PlayerModelAnimator.hpp"
#include "animation/SkeletalModel.hpp"
#include "config/GameOptions.hpp"
#include "gameplay/ChestSystem.hpp"
#include "gameplay/GameSession.hpp"
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
#include "render/StreamingBudget.hpp"
#include "ui/Language.hpp"
#include "ui/TextFont.hpp"
#include "ui/UiFrameData.hpp"
#include "world/ChunkMesher.hpp"
#include "world/ChunkStreamer.hpp"
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
    std::unordered_map<world::SectionPosition, GpuMesh, world::SectionPositionHash>& gpuMeshes;
    StreamBufferPool& deviceBufferPool_;
    StreamBufferPool& stagingBufferPool_;
    VkQueryPool& occlusionQueryPool;
    VkPipeline& occlusionQueryPipeline;
    VkPipelineLayout& occlusionQueryLayout;
    AllocatedBuffer& occlusionBoxVertexBuffer;
    AllocatedBuffer& occlusionBoxIndexBuffer;
    std::deque<world::SectionPosition>& pendingSectionOrder;
    world::SmoothLightingQuality& currentMeshQuality;
    world::SmoothLightingQuality& targetMeshQuality;
    std::unordered_set<world::SectionPosition, world::SectionPositionHash>& qualityRemeshPending;
    gameplay::GameSession& gameSession;
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
    bool& breakButtonHeld;
    bool& inventoryOpen;
    bool& spawnPositionInitialized;
    bool& worldReady;
    bool& paused;
    bool& dropRequested;
    bool& dropWholeStack;
    bool& chatOpen;
    std::optional<world::VoxelRaycastHit>& targetedBlock;
    std::optional<glm::ivec3>& miningTarget;
    double& miningStartedAt;
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
    std::function<void(gameplay::ItemStack)> spawnDroppedStack;
    std::function<void()> initializeSpawnPosition;
    std::function<void(int, int, int, world::Block, std::uint8_t, std::optional<world::BlockOrientation>)> submitWorldEditFn;
  };

  explicit WorldRenderer(const Bindings& b)
      : testScene(b.testScene), chunkStreamer(b.chunkStreamer), interactionWorld(b.interactionWorld), gpuMeshes(b.gpuMeshes), deviceBufferPool_(b.deviceBufferPool_), stagingBufferPool_(b.stagingBufferPool_), occlusionQueryPool(b.occlusionQueryPool), occlusionQueryPipeline(b.occlusionQueryPipeline), occlusionQueryLayout(b.occlusionQueryLayout), occlusionBoxVertexBuffer(b.occlusionBoxVertexBuffer), occlusionBoxIndexBuffer(b.occlusionBoxIndexBuffer), pendingSectionOrder(b.pendingSectionOrder), currentMeshQuality(b.currentMeshQuality), targetMeshQuality(b.targetMeshQuality), qualityRemeshPending(b.qualityRemeshPending), gameSession(b.gameSession), uiFrameData_(b.uiFrameData_), camera(b.camera), speciesModels(b.speciesModels), heldItemAnimation(b.heldItemAnimation), worldPlayerAnimator(b.worldPlayerAnimator), chestLidAnimation(b.chestLidAnimation), itemDisplayAnimation(b.itemDisplayAnimation), cameraPerspective(b.cameraPerspective), worldBodyYaw(b.worldBodyYaw), particleSystem(b.particleSystem), breakButtonHeld(b.breakButtonHeld), inventoryOpen(b.inventoryOpen), spawnPositionInitialized(b.spawnPositionInitialized), worldReady(b.worldReady), paused(b.paused), dropRequested(b.dropRequested), dropWholeStack(b.dropWholeStack), chatOpen(b.chatOpen), targetedBlock(b.targetedBlock), miningTarget(b.miningTarget), miningStartedAt(b.miningStartedAt), renderInterpolationAlpha(b.renderInterpolationAlpha), window(b.window), instance(b.instance), surface(b.surface), device(b.device), allocator(b.allocator), resources_(b.resources_), textures_(b.textures_), sceneDescriptorSets(b.sceneDescriptorSets), gpuSceneBuffer(b.gpuSceneBuffer), particlePipeline(b.particlePipeline), particlePipelineLayout(b.particlePipelineLayout), legacyParticles(b.legacyParticles), shadowTarget(b.shadowTarget), shadowPipelineLayout(b.shadowPipelineLayout), shadowPipeline(b.shadowPipeline), shadowDebugSet(b.shadowDebugSet), shadowDebugPipelineLayout(b.shadowDebugPipelineLayout), shadowDebugPipeline(b.shadowDebugPipeline), shadowLightViewProj(b.shadowLightViewProj), shadowDisabled(b.shadowDisabled), shadowDebugOverlay(b.shadowDebugOverlay), rainSystem(b.rainSystem), sceneParticleRecords_(b.sceneParticleRecords_), rainMode_(b.rainMode_), rainTime_(b.rainTime_), rainSheetPipeline(b.rainSheetPipeline), rainSheetPipelineLayout(b.rainSheetPipelineLayout), language(b.language), swapchainExtent(b.swapchainExtent), renderPass(b.renderPass), pipelineLayout(b.pipelineLayout), graphicsPipeline(b.graphicsPipeline), translucentPipeline(b.translucentPipeline), cutoutPipeline(b.cutoutPipeline), skyPipeline(b.skyPipeline), outlinePipelineLayout(b.outlinePipelineLayout), outlinePipeline(b.outlinePipeline), itemPipelineLayout(b.itemPipelineLayout), itemPipeline(b.itemPipeline), itemShadowPipeline(b.itemShadowPipeline), heldItemPipeline(b.heldItemPipeline), framebuffers(b.framebuffers), frames(b.frames), currentFrame(b.currentFrame), occlusionDisabled(b.occlusionDisabled), hasLastRenderEye(b.hasLastRenderEye), lastRenderEye(b.lastRenderEye), occlusionValidityInitialized(b.occlusionValidityInitialized), occlusionRotationAccumulatorDegrees(b.occlusionRotationAccumulatorDegrees), occlusionTranslationAccumulator(b.occlusionTranslationAccumulator), peakPendingSectionCount(b.peakPendingSectionCount), smoothedFrameSeconds_(b.smoothedFrameSeconds_), streamingUploadBudget_(b.streamingUploadBudget_), occlusionStates(b.occlusionStates), occlusionMissCount(b.occlusionMissCount), pendingSectionUpdates(b.pendingSectionUpdates), latestSectionRevisions(b.latestSectionRevisions), worldEpoch(b.worldEpoch), loadedCpuChunkCount(b.loadedCpuChunkCount), completedBlockEditCount(b.completedBlockEditCount), completedStreamBatchCount(b.completedStreamBatchCount), lastVisibleMeshCount(b.lastVisibleMeshCount), worldSessionActive(b.worldSessionActive), hasLastStreamingForward(b.hasLastStreamingForward), lastStreamingForward(b.lastStreamingForward), uploadedSectionsThisFrame(b.uploadedSectionsThisFrame), uploadedBytesThisFrame(b.uploadedBytesThisFrame), totalUploadedBytes(b.totalUploadedBytes), hud_(b.hud_), rainTargetCount(b.rainTargetCount), renderViewMatrix(b.renderViewMatrix), viewBobbingMatrix(b.viewBobbingMatrix), renderEyeState(b.renderEyeState), cameraFarPlane(b.cameraFarPlane), renderDistanceBlocks(b.renderDistanceBlocks), spawnDroppedStack(b.spawnDroppedStack), initializeSpawnPosition(b.initializeSpawnPosition), submitWorldEditFn(b.submitWorldEditFn) {}

  WorldRenderer(const WorldRenderer&) = delete;
  WorldRenderer& operator=(const WorldRenderer&) = delete;


    // ---- helpers duplicated from the renderer core (pure forwards over the
    // bound references) so the moved bodies resolve them; Impl keeps its own. ----
    [[nodiscard]] std::string_view translate(std::string_view key,
                                             std::string_view fallback) const {
        return language.translate(key, fallback);
    }
    [[nodiscard]] gameplay::Inventory& activeInventory() const { return gameSession.inventory(); }
    [[nodiscard]] AllocatedBuffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                               bool hostVisible) const {
        return resources_.createBuffer(size, usage, hostVisible);
    }
    void destroyBuffer(AllocatedBuffer& buffer) const noexcept { resources_.destroyBuffer(buffer); }
    // submitWorldEdit carries default args at its Impl call sites; forward through
    // the bound hook so the moved test-scene setup keeps compiling unchanged.
    void submitWorldEdit(int x, int y, int z, world::Block block, std::uint8_t fluidLevel = 0U,
                         std::optional<world::BlockOrientation> orientation = std::nullopt) {
        submitWorldEditFn(x, y, z, block, fluidLevel, orientation);
    }

    void remeshSectionImmediate(world::SectionPosition position,
                                const world::ChunkLightSampler& lighting) {
        world::SectionMeshUpdate update;
        update.position = position;
        update.mesh = chunkStreamer.acquireMeshData();
        static_cast<void>(
            world::ChunkMesher::buildSection(interactionWorld, {position.chunkX, position.chunkZ},
                                             position.sectionY, lighting, update.mesh));
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


    void processChunkStreaming() {
        if (!worldSessionActive)
            return;
        const auto position = camera.position();
        // Look ahead along the movement direction so a fast-flying gameSession.player() never
        // reaches the boundary of the generated world: the request centre leads
        // the gameSession.player() by roughly a second of travel, and the worker generates
        // nearest-first around that leading position. Vanilla never stalls a
        // moving gameSession.player() on terrain because its gameSession.player() tickets already
        // keep the chunks in front of the gameSession.player() generated; this is the client-side
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
            constexpr float kSpinGuardRotation = 0.01F; // ~8°/frame
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
                0.0F, static_cast<float>(chunkStreamer.loadRadius() * world::kChunkWidth) - 8.0F);
            const float leadBlocks = std::min(speed * 20.0F, maxLead);
            const glm::vec2 direction = velocity / speed;
            requestPosition += glm::vec3{direction.x, 0.0F, direction.y} * leadBlocks;
        }
        chunkStreamer.request(world::chunkPositionFromWorld(requestPosition.x, requestPosition.z));
        while (auto batch = chunkStreamer.poll()) {
            queueStreamBatch(std::move(*batch));
        }
    }

    // The gameSession.player()'s movement is never blocked on terrain generation:
    // processChunkStreaming already leads the request centre in the direction of travel, and if the
    // worker ever falls behind, PlayerController's unloaded-column wall simply stops the
    // gameSession.player() in place (a normal collision, not a stall). Blocking the render thread
    // here is what caused the visible hitch at the boundary, so there is deliberately no
    // synchronous wait anymore.

    // Every button click plays the vanilla ui.button.click sound at the
    // listener, so the master-category click is always fully audible. Menu
    // buttons, creative tabs and every gameSession.inventory()/container slot go through this
    // one helper; drags (the two sliders, the creative scrollbar) do not.

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
            gpuMesh.sectionOrigin = {static_cast<float>(position.chunkX) * world::kChunkWidth,
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


    void updateShadowMatrix() {
        if (shadowDisabled) {
            return;
        }
        const auto daylight = world::DayNightCycle::state(gameSession.gameTimeSeconds());
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
        if (gameSession.itemEntities().entities().empty() &&
            gameSession.worldSimulation().fallingBlocks().empty()) {
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
                vkCmdPushConstants(commandBuffer, itemPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                                   sizeof(push), &push);
                vkCmdDraw(commandBuffer, 36U, 1, 0, 0);
            } else {
                // Non-block items share the held item's single-layer 3D model
                // instead of a flat camera-facing billboard: the item icon as a
                // thin slab with extruded edges, spinning about Y — the way
                // vanilla's ItemEntityRenderer draws the same ItemRenderer model
                // in GROUND transform.
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

    std::size_t drawParticles(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet,
                              bool deferSceneBufferFlush) {
        const auto& particles = particleSystem.particles();
        sceneParticleRecords_.clear();
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
        // Reuse one host-cached staging vector instead of allocating a temporary
        // one every frame. Keep world/light reads out of VMA's sequential-write
        // mapping; a single bulk copy into that mapping performs better on
        // Windows heaps that expose it as write-combined memory.
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
        // Write the records into this frame's storage-buffer slot and flush. The
        // per-frame fence waited at the top of drawFrame orders these host writes
        // against the prior submission's read of the same slot, so no barrier is
        // needed; vmaFlushAllocation covers non-coherent heaps (a no-op on
        // unified Apple memory).
        const std::size_t bytes = count * sizeof(ParticleRecord);
        // Async rain appends to this allocation immediately below. Defer the
        // flush in that case so a discrete Windows heap pays one flush for the
        // combined particle+rain range instead of two.
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

    // Draws the weather rain through one of three paths. Particle and async use
    // the same CPU drops for a fair draw-backend comparison; texture follows
    // vanilla 1.16.1's independent precipitation-column pass:
    //   texture   -> narrow vertical columns using environment/rain.png
    //   particles -> the old per-particle item-pipeline billboards
    //   async     -> one instanced draw from the scene storage buffer, with
    //                baseInstance pointing past the block-dust records

    void drawRain(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet,
                  std::size_t baseRecordCount) {
        const auto& drops = rainSystem.drops();
        static bool reported = false;
        if (rainMode_ == RainMode::Texture) {
            const float rainGradient = gameSession.weatherSystem().rainGradient();
            if (rainGradient <= 0.02F) {
                return;
            }
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, rainSheetPipeline);
            const std::array<VkDescriptorSet, 2> sets{descriptorSet,
                                                      sceneDescriptorSets[currentFrame]};
            vkCmdBindDescriptorSets(
                commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, rainSheetPipelineLayout, 0,
                static_cast<std::uint32_t>(sets.size()), sets.data(), 0, nullptr);

            // WorldRenderer#renderRainSnow uses a 10-block fancy radius and
            // emits one camera-oriented vertical strip per x/z column. The
            // strip begins at the MOTION_BLOCKING surface, spans the camera's
            // local vertical window, and fades toward the circular boundary.
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
                    // field_20794/field_20795 in the original are a stable
                    // 32x32 lookup indexed by integer offsets, not by the
                    // camera's fractional position. Its centre divides 0/0
                    // and therefore contributes no usable quad.
                    const float integerDistance = std::sqrt(static_cast<float>(dx * dx + dz * dz));
                    if (integerDistance <= 1.0e-4F) {
                        continue;
                    }
                    glm::vec2 tangent{1.0F, 0.0F};
                    tangent = {-static_cast<float>(dz) / integerDistance,
                               static_cast<float>(dx) / integerDistance};

                    float bottom = static_cast<float>(cameraY - kRainRadius);
                    float top = static_cast<float>(cameraY + kRainRadius);
                    // Probe to the same +32 ceiling used by the drop cache so a
                    // tall nearby roof also collapses the strip completely.
                    const float surface = rainSystem.precipitationSurfaceY(
                        interactionWorld, blockX, blockZ, cameraPosition.y + 32.0F);
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
                    // Vanilla seeds every column from its world coordinates,
                    // giving neighbouring strips stable but different scroll
                    // phases/speeds instead of one synchronized rain curtain.
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
            // Original precipitation is one tessellated batch. The storage
            // records retain that property here: one instanced draw, not one
            // Vulkan draw call per column.
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
        // Async: append the rain records after the block-dust records in the
        // same scene buffer and draw once with baseInstance past them.
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
        // The particle pass may have filled the buffer completely. Its deferred
        // records were flushed above even when no rain instance fits.
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
            const auto orientation =
                interactionWorld.orientation(chest.position.x, chest.position.y, chest.position.z);
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
            const glm::mat4 lidMatrix = glm::translate(glm::mat4{1.0F}, blockCenter) * yawMatrix *
                                        glm::translate(glm::mat4{1.0F}, hingeLocal) *
                                        glm::rotate(glm::mat4{1.0F}, -pitch, {1.0F, 0.0F, 0.0F}) *
                                        glm::translate(glm::mat4{1.0F}, closedCentreFromHinge);
            drawWorldCuboid(lidMatrix, {0.875F, 0.3125F, 0.875F}, kChestLidFirstLayer, packedLight);
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
        const glm::vec3 feet =
            camera.position() - glm::vec3{0.0F, gameSession.player().eyeHeight(), 0.0F};
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
    [[nodiscard]] const gameplay::entities::SpeciesRenderModel*
    speciesFor(const gameplay::entities::EntityType* type) const {
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
                // One leg cycle per block travelled reads as a natural gait.
                animator.addLayer(*walk, walk->localTime(walkDistance), 1.0F);
            }
            if (idle != nullptr) {
                animator.addLayer(
                    *idle, idle->localTime(static_cast<float>(gameSession.gameTimeSeconds())),
                    1.0F);
            }
            const animation::SkeletonPose pose = animator.evaluate();

            // LivingEntityRenderer#getLyingAngle: a dying body tips ninety
            // degrees over the twenty ticks of deathTime, easing as it lands.
            float deathRoll = 0.0F;
            if (entity.damage.deathTicks > 0) {
                const float progress = std::min((static_cast<float>(entity.damage.deathTicks) +
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
                glm::rotate(glm::mat4{1.0F}, yaw + kEntityFacingOffset,
                            glm::vec3{0.0F, 1.0F, 0.0F}) *
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
                model,
                {static_cast<float>(textures_.entityTextureWidth), static_cast<float>(textures_.entityTextureHeight)});
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
                        cube.hasRotation ? animation::rotationAboutPivot(cube.rotation, cube.pivot)
                                         : glm::mat4{1.0F};
                    const glm::mat4 cubeWorld = modelRoot * boneWorld * cubeRotation *
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
                                                       gameSession.player().inWater(),
                                                       !gameSession.player().onGround());
        if (!std::isfinite(duration) || duration <= 0.0F)
            return;
        const float progress = std::clamp(
            static_cast<float>((gameSession.gameTimeSeconds() - miningStartedAt) / duration), 0.0F,
            0.999F);
        // ClientPlayerInteractionManager reports (progress * 10) - 1, so the first
        // tenth of the dig carries no crack overlay at all.
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
        const glm::mat4 heldTransform =
            viewBobbingMatrix() * (emptyHand ? animation::firstPersonArmTransform(pose)
                                   : uiFrameData_.eating
                                       ? animation::firstPersonEatTransform(pose, cubeModel)
                                       : animation::firstPersonItemTransform(pose, cubeModel));
        const float heldFrontLayer =
            !emptyHand && gameplay::isBlockStack(stack)
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
            {(!emptyHand && !cubeModel) ? 7.0F : 6.0F, 0.0F, emptyHand ? 1.0F : 0.0F,
             emptyHand ? 1.0F : 0.0F},
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
        if (occlusionQueryPool != VK_NULL_HANDLE) {
            // Clear this frame's slot range before it is reused. Results for
            // the previous submission were already read back this drawFrame.
            vkCmdResetQueryPool(
                frame.commandBuffer, occlusionQueryPool,
                static_cast<std::uint32_t>(currentFrame * kOcclusionQueriesPerFrame),
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
        std::ranges::sort(frustumEntries,
                          [](const FrustumEntry& first, const FrustumEntry& second) {
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
                std::cerr << "  entry(" << entry.position.chunkX << ',' << entry.position.sectionY
                          << ',' << entry.position.chunkZ
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
            if (!occlusionDisabled && occlusionQueryPool != VK_NULL_HANDLE && withinQueryBudget) {
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
            // The occlusion pass can leave the query pipeline's descriptor set
            // bound, so the cutout pipeline re-binds its own before drawing.
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
                vkCmdPushConstants(frame.commandBuffer, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                                   0, sizeof(glm::vec4), &mesh->sectionOrigin);
                vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &mesh->vertexBuffer.buffer,
                                       &mesh->translucent.vertexOffset);
                vkCmdBindIndexBuffer(frame.commandBuffer, mesh->indexBuffer.buffer,
                                     mesh->translucent.indexOffset, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(frame.commandBuffer, mesh->translucent.indexCount, 1, 0, 0, 0);
            }
        }
        // Particles stay behind the translucent terrain pass, which is where
        // vanilla draws them too.
        const bool appendAsyncRain = rainMode_ == RainMode::Async && !rainSystem.drops().empty();
        const std::size_t particleRecordCount =
            drawParticles(frame.commandBuffer, frame.descriptorSet, appendAsyncRain);
        drawRain(frame.commandBuffer, frame.descriptorSet, particleRecordCount);
        drawMiningProgress(frame.commandBuffer, frame.descriptorSet);
        if (!inventoryOpen && !paused && !chatOpen && targetedBlock.has_value()) {
            const world::Block targeted = interactionWorld.block(
                targetedBlock->block.x, targetedBlock->block.y, targetedBlock->block.z);
            // The outline now traces the block's actual shape, so sub-block
            // blocks (torch, plants, chest) no longer show a full-cube marker.
            // Crops and farmland read their shape from the cell's state.
            const world::BlockBounds bounds =
                world::blockSelectionBounds(interactionWorld, targetedBlock->block, targeted);
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
        hud_.drawHud(frame.commandBuffer, frame.descriptorSet);
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
                          << ',' << position.chunkZ
                          << ") count=" << frame.occlusionQueryResults[index] << '\n';
            }
        }
    }



  // ---- bound references to renderer-core state (owned by Impl) ----
  std::optional<TestSceneOptions>& testScene;
  world::ChunkStreamer& chunkStreamer;
  world::World& interactionWorld;
  std::unordered_map<world::SectionPosition, GpuMesh, world::SectionPositionHash>& gpuMeshes;
  StreamBufferPool& deviceBufferPool_;
  StreamBufferPool& stagingBufferPool_;
  VkQueryPool& occlusionQueryPool;
  VkPipeline& occlusionQueryPipeline;
  VkPipelineLayout& occlusionQueryLayout;
  AllocatedBuffer& occlusionBoxVertexBuffer;
  AllocatedBuffer& occlusionBoxIndexBuffer;
  std::deque<world::SectionPosition>& pendingSectionOrder;
  world::SmoothLightingQuality& currentMeshQuality;
  world::SmoothLightingQuality& targetMeshQuality;
  std::unordered_set<world::SectionPosition, world::SectionPositionHash>& qualityRemeshPending;
  gameplay::GameSession& gameSession;
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
  bool& breakButtonHeld;
  bool& inventoryOpen;
  bool& spawnPositionInitialized;
  bool& worldReady;
  bool& paused;
  bool& dropRequested;
  bool& dropWholeStack;
  bool& chatOpen;
  std::optional<world::VoxelRaycastHit>& targetedBlock;
  std::optional<glm::ivec3>& miningTarget;
  double& miningStartedAt;
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

  // ---- camera / gameplay callbacks kept single-source in Impl ----
  std::function<std::size_t()> rainTargetCount;
  std::function<glm::mat4()> renderViewMatrix;
  std::function<glm::mat4()> viewBobbingMatrix;
  std::function<RenderEye()> renderEyeState;
  std::function<float()> cameraFarPlane;
  std::function<float()> renderDistanceBlocks;
  std::function<void(gameplay::ItemStack)> spawnDroppedStack;
  std::function<void()> initializeSpawnPosition;
  std::function<void(int, int, int, world::Block, std::uint8_t, std::optional<world::BlockOrientation>)> submitWorldEditFn;
};

} // namespace mc::render
