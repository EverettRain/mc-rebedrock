#pragma once

// World-render types and constants shared between the renderer core
// (VulkanRenderer.cpp) and the extracted world-render subsystem
// (WorldRenderer, in progress). These were previously defined in
// VulkanRenderer.cpp's anonymous namespace, whose internal linkage prevents
// sharing across translation units; hoisting them into mc::render here lets
// both sides name the same definitions without drift. (Prerequisite for the
// WorldRenderer extraction; see docs/wait-for-opus/vulkanrenderer-modular-split.md.)

#include "render/MeshData.hpp"                // Aabb
#include "render/vulkan/VulkanResources.hpp"  // AllocatedBuffer
#include "world/ChunkStreamer.hpp"            // world::SectionPosition

#include <vulkan/vulkan.h>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mc::render {

// Frames rendered concurrently (double buffering): sizes the per-frame context
// array and the stream-buffer deferred-return rings.
constexpr std::size_t kFramesInFlight = 2;

// Occlusion queries gate a section's opaque draw behind the depth the closer
// terrain wrote earlier in the same frame. Each in-flight frame owns a
// separate query pool: MoltenVK backs one pool with one Metal visibility-result
// buffer, so per-frame pools remove cross-frame reset/readback traffic from the
// same query object. Active precise queries are disabled by default on macOS
// because current MoltenVK can still lose the device under sustained load; the
// pools remain available for native Vulkan and explicit macOS diagnostics.
inline constexpr std::size_t kOcclusionQueriesPerFrame = 2048;
inline constexpr std::size_t kOcclusionQueryPoolSize = kOcclusionQueriesPerFrame;
inline constexpr std::uint32_t kOcclusionHysteresisFrames = 2;
static_assert(kOcclusionQueryPoolSize * sizeof(std::uint64_t) <= 16U * 1024U);
#if defined(__APPLE__)
// The renderer consumes only zero/non-zero visibility. MoltenVK's Boolean mode
// provides exactly that contract, so diagnostics do not request unused precise
// counts. Boolean mode still reproduces the current MoltenVK device loss; this
// flag is semantic cleanup, not the macOS stability workaround.
inline constexpr VkQueryControlFlags kOcclusionQueryControlFlags = 0U;
#else
inline constexpr VkQueryControlFlags kOcclusionQueryControlFlags =
    VK_QUERY_CONTROL_PRECISE_BIT;
#endif

// Stream-mesh buffers are pooled by power-of-two size class and reused across
// section uploads instead of created/destroyed per mesh.
constexpr std::array<VkDeviceSize, 8> kStreamBufferClassSizes{
    16U * 1024U,  32U * 1024U,  64U * 1024U,   128U * 1024U,
    256U * 1024U, 512U * 1024U, 1024U * 1024U, 2U * 1024U * 1024U};

// Per-frame streaming upload budget caps (see StreamingBudget.hpp for the
// adaptive byte budget; these are the hard ceilings).
inline constexpr VkDeviceSize kMaxUploadBytesPerFrame = 8U * 1024U * 1024U;
inline constexpr std::size_t kMaxPrioritySectionUploadsPerFrame = 24;
inline constexpr std::size_t kMaxPendingSectionUpdates = 2048;
inline constexpr VkDeviceSize kMaxStreamBufferPoolBytes = 256U * 1024U * 1024U;
inline constexpr VkBufferUsageFlags kStreamBufferDeviceUsage =
    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
    VK_BUFFER_USAGE_TRANSFER_DST_BIT;

// Camera view mode, cycled with F5 like Java Edition: first person, third
// person behind the player, then third person in front looking back.
enum class CameraPerspective : std::uint8_t {
    FirstPerson,
    ThirdPersonBack,
    ThirdPersonFront,
};

// The three rain render paths (MC_REBEDROCK_RAIN_MODE). Particles and Async
// consume the same CPU-simulated drops for a fair backend comparison. Texture
// follows 1.16.1's column precipitation renderer; its CPU drops remain active
// only for landing splashes and weather audio.
enum class RainMode { Texture, Particles, Async };

// Particle and async are two render backends for the same visual effect, so
// their population must be identical at a given particle level.
constexpr std::size_t kParticleRainBaseCount = 2000U;
[[nodiscard]] constexpr std::size_t rainBaseCount(RainMode mode) {
    return mode == RainMode::Texture ? 30U : kParticleRainBaseCount;
}
static_assert(rainBaseCount(RainMode::Particles) == rainBaseCount(RainMode::Async));
static_assert(kParticleRainBaseCount * 3U == 6000U);  // medium: 1.5x, thunder: 2x
static_assert(kParticleRainBaseCount * 6U == 12000U); // high: 2x medium
static_assert(kParticleRainBaseCount * 9U == 18000U); // crazy: 3x medium

// Push constants for the sun-space shadow pre-pass: the light view-projection
// and the per-section origin, packed under Vulkan's 128-byte guarantee.
struct ShadowPush final {
    alignas(16) glm::mat4 lightViewProj;
    alignas(16) glm::vec4 sectionOrigin;
};
static_assert(sizeof(ShadowPush) <= 128U,
              "Shadow push constants must fit Vulkan's guaranteed minimum");

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

// The camera eye used to build both the view matrix and the culling frustum
// (F5 cycles first person, third person behind, third person in front looking
// back) along the look direction, so the two always agree.
struct RenderEye {
    glm::vec3 position{0.0F};
    glm::vec3 forward{0.0F, 0.0F, 1.0F};
};

} // namespace mc::render
