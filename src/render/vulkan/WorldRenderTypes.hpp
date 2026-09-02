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

// 雨的两条绘制路径（MC_REBEDROCK_RAIN_MODE）
// Texture 走 vanilla 的逐列降水渲染，此时 CPU 雨滴只用于落地水花和天气音效
// Async 把 CPU 雨滴实例化成公告板，整片雨一次 vkCmdDraw 画完
//
// 这里曾有第三档 Particles，它与 Async 用的是同一批雨滴并产出同一份视觉
// 区别只在 Particles 逐雨滴发一次 draw call 而 Async 走实例化
// 那是一条为了和 Async 做直接对照而临时留下的绘制方式，却被接进实验性内容子菜单成了玩家可选项
// 疯狂档满雨时它意味着每帧一万八千次 draw call
// 对照数据已经拿到并记在 CHANGELOG 里，因此把它整条移除，生产路径 Async 保留原名
enum class RainMode { Texture, Async };

constexpr std::size_t kParticleRainBaseCount = 2000U;
[[nodiscard]] constexpr std::size_t rainBaseCount(RainMode mode) {
    return mode == RainMode::Texture ? 30U : kParticleRainBaseCount;
}
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

// 遮挡查询用到的 GPU 资源与它的总开关
//
// 与 WorldPipelines 同理，所有权在 VulkanRenderer::Impl
// 查询池与包围盒缓冲由它创建，查询管线随交换链销毁重建，WorldRenderer 只持有引用
// 逐 section 的查询结果不在这里，指 occlusionStates 与 occlusionMissCount
// 那是纯 CPU 状态，只有 WorldRenderer 读写，已经是它的自有成员
struct OcclusionResources final {
    // 每帧一个查询池，因此槽位区间总是从零开始，不必跨帧对账
    std::array<VkQueryPool, kFramesInFlight> queryPools{};
    VkPipeline queryPipeline = VK_NULL_HANDLE;
    VkPipelineLayout queryLayout = VK_NULL_HANDLE;
    // 查询绘制用的单位立方体，逐 section 通过推常量拉伸到它的包围盒
    AllocatedBuffer boxVertexBuffer;
    AllocatedBuffer boxIndexBuffer;
    // 整条遮挡通道的开关（平台判定与 MC_REBEDROCK_DISABLE_OCCLUSION）
    bool disabled = false;
};

// 世界通道用到的全部管线、管线布局与渲染通道，集中在一处
//
// 它们的所有权仍在 VulkanRenderer::Impl，由它创建
// 并在 cleanupSwapchain 与 createSwapchainResources 里随交换链销毁重建
// 并在 cleanupSwapchain 与 createSwapchainResources 里随交换链销毁重建
// WorldRenderer 持有的是对这个结构体的引用而不是拷贝
// 重建之后它必须看到新句柄，拿到一份旧值的拷贝就是一堆悬垂句柄
//
// 打包的理由是 Bindings：这 20 个句柄原先每个都是一条 VkPipeline& 绑定
// 于是每加一条管线就要在 Bindings 定义、成员声明与构造初始化列表三处各写一遍
// 收成一个结构体之后只剩 1 条绑定，加管线只动这里一处
struct WorldPipelines final {
    VkRenderPass renderPass = VK_NULL_HANDLE;
    // GUI 单独一趟。vanilla 在**未经伽马转换**的帧缓冲上合成界面，混合因此发生在
    // sRGB 编码值上；世界这趟仍写线性值、由 sRGB 附件编码。两者不能共用一个视图，
    // 于是 GUI 这趟绑同一张场景图的 UNORM 视图，全程直接读写编码值。
    VkRenderPass guiRenderPass = VK_NULL_HANDLE;
    // 地形三条渲染层共用的布局与它们各自的管线
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline graphicsPipeline = VK_NULL_HANDLE;
    VkPipeline translucentPipeline = VK_NULL_HANDLE;
    VkPipeline cutoutPipeline = VK_NULL_HANDLE;
    VkPipeline skyPipeline = VK_NULL_HANDLE;
    // 选择框轮廓
    VkPipelineLayout outlinePipelineLayout = VK_NULL_HANDLE;
    VkPipeline outlinePipeline = VK_NULL_HANDLE;
    // 掉落物、下落方块、经验球与第一人称手持物共用一套推常量布局
    VkPipelineLayout itemPipelineLayout = VK_NULL_HANDLE;
    VkPipeline itemPipeline = VK_NULL_HANDLE;
    VkPipeline itemShadowPipeline = VK_NULL_HANDLE;
    VkPipeline heldItemPipeline = VK_NULL_HANDLE;
    // 粒子（实例化）
    VkPipeline particlePipeline = VK_NULL_HANDLE;
    VkPipelineLayout particlePipelineLayout = VK_NULL_HANDLE;
    // 太阳空间阴影预通道，以及它的调试叠加层
    VkPipeline shadowPipeline = VK_NULL_HANDLE;
    VkPipelineLayout shadowPipelineLayout = VK_NULL_HANDLE;
    VkPipeline shadowDebugPipeline = VK_NULL_HANDLE;
    VkPipelineLayout shadowDebugPipelineLayout = VK_NULL_HANDLE;
    // 贴图雨的逐列雨幕
    VkPipeline rainSheetPipeline = VK_NULL_HANDLE;
    VkPipelineLayout rainSheetPipelineLayout = VK_NULL_HANDLE;
};

// 沿视线方向算出的相机眼点，视图矩阵和剔除视锥都用它，两者因此永远一致
// F5 在第一人称、背后第三人称、前方回看第三人称之间循环
struct RenderEye {
    glm::vec3 position{0.0F};
    glm::vec3 forward{0.0F, 0.0F, 1.0F};
};

} // namespace mc::render
