#pragma once

// 渲染器内核（VulkanRenderer.cpp）与世界渲染子系统（WorldRenderer.hpp）共用的世界渲染类型与常量
// 放在 mc::render 而不是某个 .cpp 的匿名命名空间里，两边才能指同一份定义

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

// 并发渲染的帧数（双缓冲）：决定逐帧上下文数组和流式缓冲延迟归还环的长度
constexpr std::size_t kFramesInFlight = 2;

// 遮挡查询：一个 section 的不透明绘制要等同帧更近的地形写完深度才放行
// 每个在飞帧各持一个独立查询池
// MoltenVK 用一个 Metal visibility-result buffer 支撑一个池
// 逐帧分池因此消掉了同一查询对象上的跨帧 reset 与回读流量
// macOS 上默认关闭活动精确查询：当前 MoltenVK 在持续负载下仍会丢设备
// 池本身保留，供原生 Vulkan 和 macOS 显式诊断使用
inline constexpr std::size_t kOcclusionQueriesPerFrame = 2048;
inline constexpr std::size_t kOcclusionQueryPoolSize = kOcclusionQueriesPerFrame;
inline constexpr std::uint32_t kOcclusionHysteresisFrames = 2;
static_assert(kOcclusionQueryPoolSize * sizeof(std::uint64_t) <= 16U * 1024U);
#if defined(__APPLE__)
// 渲染器只看可见/不可见，不看具体像素数，MoltenVK 的 Boolean 模式正好是这个契约
// 注意 Boolean 模式一样会复现 MoltenVK 丢设备，这个标志是语义收敛而非稳定性绕过
inline constexpr VkQueryControlFlags kOcclusionQueryControlFlags = 0U;
#else
inline constexpr VkQueryControlFlags kOcclusionQueryControlFlags =
    VK_QUERY_CONTROL_PRECISE_BIT;
#endif

// 流式网格缓冲按 2 的幂尺寸分级池化，在各 section 上传之间复用，而不是每个网格建了又销
// MoltenVK 不会把释放掉的 MTLBuffer 内存还给系统，所以复用严格优于释放
// 池把显存高水位钉在工作集大小上
// 超过 kMaxStreamBufferPoolBytes 的常驻总量后，多余的空闲缓冲才还给驱动
constexpr std::array<VkDeviceSize, 8> kStreamBufferClassSizes{
    16U * 1024U,  32U * 1024U,  64U * 1024U,   128U * 1024U,
    256U * 1024U, 512U * 1024U, 1024U * 1024U, 2U * 1024U * 1024U};

// 逐帧流送上传预算的硬上限（自适应的字节预算见 StreamingBudget.hpp，这里是天花板）
//
// 一批生成好的区块可能一次性甩给渲染线程几百个 section 网格
// 每个新区块还会连带重网格化最多八个邻居，同帧全传会把 GPU 打出尖峰
// 因此预算是自适应铺开的：GPU 空闲就抬高，吃紧就压低
// 放置破坏、流体与沙的连锁这类玩法编辑走另一条免预算的优先桶，保证方块当帧可见
// 那条桶自身也有逐帧上限，免得一次大范围流体连锁把帧卡死
//
// kMaxPendingSectionUpdates 是"已排队但尚未上传"的网格数上限
// 工作线程产出远快于逐帧上传预算的消化速度，队列不设限时 CPU 侧网格数据会堆积
// 实测峰值曾超过 4500 个 section
// 超限时淘汰最老的低优先级条目
inline constexpr VkDeviceSize kMaxUploadBytesPerFrame = 8U * 1024U * 1024U;
inline constexpr std::size_t kMaxPrioritySectionUploadsPerFrame = 24;
inline constexpr std::size_t kMaxPendingSectionUpdates = 2048;
inline constexpr VkDeviceSize kMaxStreamBufferPoolBytes = 256U * 1024U * 1024U;
inline constexpr VkBufferUsageFlags kStreamBufferDeviceUsage =
    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
    VK_BUFFER_USAGE_TRANSFER_DST_BIT;

// F5 循环的视角模式：第一人称 → 玩家背后第三人称 → 玩家前方回看第三人称
enum class CameraPerspective : std::uint8_t {
    FirstPerson,
    ThirdPersonBack,
    ThirdPersonFront,
};

// 三条降雨渲染路径（MC_REBEDROCK_RAIN_MODE）
// Particles 与 Async 消费同一批 CPU 模拟的雨滴，好让两个后端可比
// Texture 走 vanilla 的逐列降水渲染，此时 CPU 雨滴只用于落地水花和天气音效
enum class RainMode { Texture, Particles, Async };

// Particles 与 Async 是同一视觉效果的两个渲染后端，因此同一粒子等级下的数量必须一致
constexpr std::size_t kParticleRainBaseCount = 2000U;
[[nodiscard]] constexpr std::size_t rainBaseCount(RainMode mode) {
    return mode == RainMode::Texture ? 30U : kParticleRainBaseCount;
}
static_assert(rainBaseCount(RainMode::Particles) == rainBaseCount(RainMode::Async));
static_assert(kParticleRainBaseCount * 3U == 6000U);  // medium: 1.5x, thunder: 2x
static_assert(kParticleRainBaseCount * 6U == 12000U); // high: 2x medium
static_assert(kParticleRainBaseCount * 9U == 18000U); // crazy: 3x medium

// 太阳空间阴影预通道的推送常量：光源视图投影矩阵加逐 section 原点，压在 Vulkan 保证的 128 字节以内
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
    // 打包顶点坐标所相对的 section 原点，逐次绘制推给地形着色器
    // 由 SectionPosition 算出——稀疏 section 的 bounds.minimum 并不是它的原点
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
    // 本帧记录的遮挡查询：数量、把结果贴回 section 所需的槽位映射，以及回读暂存区
    // 该暂存区在这个帧序号的下一次提交复用此范围之前被填满
    std::uint32_t occlusionQueryCount = 0U;
    std::vector<world::SectionPosition> occlusionQuerySections;
    std::vector<std::uint64_t> occlusionQueryResults;
};

// 可复用的流式网格缓冲：按尺寸档分的空闲表，加上逐帧的延迟归还队列
// 一个缓冲要延迟 kFramesInFlight 帧才回到空闲表，即直到 drawFrame 等到同槽围栏
// 这样才能保证 GPU 已经读完
struct StreamBufferPool final {
    std::array<std::vector<AllocatedBuffer>, kStreamBufferClassSizes.size()> freeByClass;
    std::array<std::vector<AllocatedBuffer>, kFramesInFlight> deferred;
    VkDeviceSize totalBytes = 0;
};

// 遮挡意义上的 section 绘制门控：Unknown 照画并发查询
// Visible 继续画同时复查
// Occluded 跳过，直到某次查询重新证明它可见
enum class OcclusionState : std::uint8_t { Unknown, Visible, Occluded };

struct OcclusionQueryPushConstants final {
    alignas(16) glm::vec4 aabbMinimum;
    alignas(16) glm::vec4 aabbMaximum;
};

// 沿视线方向算出的相机眼点，视图矩阵和剔除视锥都用它，两者因此永远一致
// F5 在第一人称、背后第三人称、前方回看第三人称之间循环
struct RenderEye {
    glm::vec3 position{0.0F};
    glm::vec3 forward{0.0F, 0.0F, 1.0F};
};

} // namespace mc::render
