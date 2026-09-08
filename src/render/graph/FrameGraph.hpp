#pragma once

// 烘焙式 frame graph（RN-20a）
//
// 声明一次 → 编译一次 → 每帧只执行一张扁平的指令表。
// 通用 render graph 每帧重建 DAG、拓扑排序、哈希查表、分配 barrier 数组，那是为
// 「pass 集合逐帧变化」的引擎设计的；本作的 pass 集合在**一次配置内是常量**
// （配置 = 画质选项 + 光影包 + 交换链尺寸），那部分开销在这里是纯浪费，而且落在
// 渲染线程的关键路径上。
//
// 分层与依赖方向
// --------------
// 本层只认 **Vulkan 句柄与描述**，不认 WorldRenderer / VulkanRenderer::Impl。
// pass body 通过 function_ref 注入，per-frame 状态经 PassContext::user 透传，
// graph 不解释那个指针。放在 render/graph/ 而不是 render/vulkan/，因为它是编排层，
// 后面光影包的两个前端要落在同一层。
//
// 那两个前端是「内置默认 pass 表」与「本作自研格式的外部包」。**不做 Iris / OptiFine
// 格式的导入**（用户裁定 2026-09-08）：那些包是为 OpenGL 写的，程序模型建立在固定的
// pass 名、隐式绑定全部 colortex、`/* RENDERTARGETS: */` 注释与 FBO 附件切换之上，
// 还普遍依赖 gl_FragData[] 这类固定管线残留。兼容它等于在 Vulkan 上重建一层 OpenGL
// 语义外加运行时 GLSL→SPIR-V 编译，换来的还是别人为另一套硬件模型写的包。
// 自研格式反过来可以与本层的 GraphDesc 一一对应（资源表 + 视图表 + pass 表），
// 着色器直接交 SPIR-V，运行时零编译器依赖。
//
// 本轮（RN-20a）**不创建任何 Vulkan 对象**。renderPass / framebuffer / clear 值都由
// 调用方在 PassDesc 里给出，编译只做校验、剪枝与扁平化。资源与视图两张表在这一轮
// 同样只被校验——它们存在是为了给 20b（管线注册表）/ 20c（usage 与 storeOp 按消费者
// 推导、资源别名）留下挂点，本轮不据此分配一字节显存。
//
// 「一个资源，两个视图」
// ----------------------
// 资源（ResourceDesc）是**一张图像**的身份；视图（ViewDesc）是对它的一种解释，多对一。
// pass 引用的是**视图槽**。layout / barrier / 生命周期一律按 ViewDesc::resource 归并，
// 于是「同一张图像的两个视图共享一条 layout 时间线」是结构性的，而不是靠约定——
// 这正是「不要把两个视图拆成两个资源」那条约束的实质。
//
// 注意：截至本轮，scene_color 在实现里**只有一个 UNORM 视图**，世界与 GUI 两趟都绑它，
// 整帧的混合都发生在 sRGB 编码值上。那是**正确的**，与 vanilla 一致（OpenGL 默认不开
// GL_FRAMEBUFFER_SRGB）；两趟拆分的真实理由是界面不做 MSAA、且界面自带每帧清空的深度。
// 不要把「补一个 sRGB 视图」当成欠账去做，那是回归。

#include "core/FunctionRef.hpp"
#include "render/graph/GpuTimestamps.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace mc::render::graph {

enum class ResourceKind : std::uint8_t { Color, Depth, Swapchain };

// 一个 pass 对一个视图的用法。本轮只用于校验与顺序检查；20c 的 usage/storeOp 推导
// 会把它当成推导输入（「最后一次写之后是否还有 Sample 读者」）。
// TransferRead 与 Sample 并列而不是被它吞掉：帧末把场景图 copy 进交换链的那一步
// 是 vkCmdCopyImage 而不是采样，但它同样是「最后一次写之后的读者」。
// 20c 的 usage/storeOp 推导按「有没有读者」判 TRANSIENT，漏掉这一类会把 scene_color
// 判成瞬态——那是整帧变黑，不是性能问题。
// ColorResolve 是**第三种**关系，不是 ColorWrite 的一个标志位。MSAA 开时世界那趟把
// 多采样的 scene_color_msaa resolve 进单采样的 scene_color：后者的内容确实由这一步产生
// （所以对 usage / TRANSIENT 推导它算写者），但它**不被载入**——`resolve.loadOp` 在
// createRenderPass() 里是 DONT_CARE，不是 ColorWrite 那条「前面没有写者就 CLEAR」。
// 用枚举值而不是 `bool resolveTarget`：后者让 `ColorWrite + resolveTarget` 这种无意义
// 组合可表达，且每个消费点要判两个字段。枚举天然互斥。
enum class Access : std::uint8_t {
    ColorWrite,
    DepthWrite,
    ColorResolve,
    DepthReadOnly,
    Sample,
    TransferRead
};

// 资源 = 一张图像的身份与分配参数。视图不在这里。
struct ResourceDesc final {
    std::string_view name;
    ResourceKind kind = ResourceKind::Color;
    VkFormat format = VK_FORMAT_UNDEFINED;
    // 0 = 跟随交换链尺寸；否则固定（阴影图自带尺寸）
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    // scene_color / scene_depth / gui_depth 是逐交换链图像一份；shadow_depth 是单份
    bool perSwapchainImage = true;
};

// 视图 = 对某个资源的一种解释，多对一
struct ViewDesc final {
    std::uint16_t resource = 0;  // 下标，指回 ResourceDesc 表
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
};

struct PassAttachment final {
    std::uint16_t view = 0;
    Access access = Access::ColorWrite;
};

// 每帧透传给 pass body 的上下文。graph 不解释 user，只原样转交——
// 于是 body 可以是**自由函数**而不是捕获 this 的 lambda，没有生命周期可踩。
struct PassContext final {
    void* user = nullptr;
    std::uint32_t imageIndex = 0;
};

using PassBody = core::function_ref<void(VkCommandBuffer, const PassContext&)>;

struct PassDesc final {
    // 编译期用于报错；`execute()` 不读它。RN-19d0 之后编译会把它抄进
    // `BakedGraph::stepNames()`，那份只给计时报告用——一个只会说「第 3 步」的
    // profiler 没有用处。热路径的 `BakedStep` 仍然不含名字。
    std::string_view name;
    std::span<const PassAttachment> attachments{};
    PassBody record;

    // VK_NULL_HANDLE = 非渲染步（纯屏障 / 拷贝 / 查询池 reset）。
    // 非渲染步不得带 framebuffer 与 clear：vkCmdResetQueryPool 之类在 renderpass 内非法，
    // 「非渲染步」这个身份就是它们的保护。
    VkRenderPass renderPass = VK_NULL_HANDLE;
    // 1 个 = 单份（shadow）；N 个 = 逐交换链图像一份，索引就是 imageIndex
    std::span<const VkFramebuffer> framebuffers{};
    std::span<const VkClearValue> clears{};
    VkExtent2D extent{};

    // 本步**开始前**要下的 image barrier，由调用方按边界给全，编译期合成一段连续区间，
    // 执行期一次 vkCmdPipelineBarrier 下完。反面案例是每个资源一次 barrier——
    // 那在 MoltenVK 上会被翻译成多次 Metal fence，代价远高于原生 Vulkan。
    // buffer / memory barrier 不进这里（它们没有 layout，也不参与合批），留在 body 内。
    std::span<const VkImageMemoryBarrier> barriers{};
    VkPipelineStageFlags barrierSrcStage = 0;
    VkPipelineStageFlags barrierDstStage = 0;

    // 编译期剪枝，不是运行期 if。剪掉的步连同它的 barrier 一起消失。
    bool enabled = true;
    // 「必须是最后一个渲染步」的标记。GUI 那趟带它：界面合成永远在最后，
    // 光影包不得插到它后面，也不得改写它。
    bool locked = false;
};

struct GraphDesc final {
    std::span<const ResourceDesc> resources;
    std::span<const ViewDesc> views;
    std::span<const PassDesc> passes;
};

// ---------------------------------------------------------------------------
// 阶段 1 的产物：资源计划（RN-20c）
// ---------------------------------------------------------------------------
//
// 先有鸡还是先有蛋：`compile()` 需要 renderPass / framebuffer 句柄，而句柄要在资源
// 创建之后才有；推导的产物（usage / storeOp / TRANSIENT）恰恰是**创建资源时的参数**。
// 拆法是把编译分成两阶段——
//
//     planResources(GraphDesc)  只吃「谁读谁写」，不碰任何句柄  → ResourcePlan
//     调用方按计划创建 image / view / renderpass / framebuffer
//     compile(GraphDesc, ResourcePlan)                        → BakedStep[]
//
// ResourcePlan 是**纯值对象**，不持有任何 Vulkan 句柄，因此可以在 headless 测试里
// 完整构造与断言——那是 RN-20c 验收的主力，不是靠真机跑一遍看画面。
//
// 推导规则的完整定义见 FrameGraph.cpp 的「推导规则」一节。

// 一个「(pass, 资源)」对上的附件操作。读者（Sample / TransferRead）不产生条目——
// 它们是描述符采样与 vkCmdCopyImage，不是附件。
struct ResourceOps final {
    std::string_view pass;
    std::string_view resource;
    Access access = Access::ColorWrite;
    VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    VkAttachmentStoreOp storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout finalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
};

// 一张图像的创建参数。`usage` / `aspect` 是推出来的，其余照抄 ResourceDesc——
// 把两者放进同一条记录，创建函数才有一个**单一**的参数来源。
struct PlannedResource final {
    std::string_view name;
    ResourceKind kind = ResourceKind::Color;
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    bool perSwapchainImage = true;
    VkImageUsageFlags usage = 0;
    VkImageAspectFlags aspect = 0;

    // 「这两张图像的创建参数一样吗」——用于 SunShadows 翻转后核对推导没有改变
    // 任何 image 参数（RN-20c 的结论：不改，所以翻转只重编译、不重建 image）。
    // usage / aspect 也在内，因为它们才是推导会动的那两个。
    [[nodiscard]] bool sameImageParameters(const PlannedResource& other) const noexcept {
        return name == other.name && kind == other.kind && format == other.format &&
               width == other.width && height == other.height && samples == other.samples &&
               perSwapchainImage == other.perSwapchainImage && usage == other.usage &&
               aspect == other.aspect;
    }
};

class ResourcePlan final {
  public:
    [[nodiscard]] std::span<const PlannedResource> resources() const noexcept {
        return resources_;
    }
    [[nodiscard]] std::span<const ResourceOps> ops() const noexcept { return ops_; }

    // 按名字取。找不到抛——创建函数拿错名字要在这里炸，不是拿到一份默认值继续跑。
    [[nodiscard]] const PlannedResource& resource(std::string_view name) const;
    [[nodiscard]] const ResourceOps& ops(std::string_view pass, std::string_view resource) const;
    // 该资源在这一步里有附件操作吗（MSAA 关时 scene_color_msaa 整个不存在）
    [[nodiscard]] bool has(std::string_view name) const noexcept;

    void add(const PlannedResource& resource) { resources_.push_back(resource); }
    void add(const ResourceOps& ops) { ops_.push_back(ops); }

  private:
    std::vector<PlannedResource> resources_;
    std::vector<ResourceOps> ops_;
};

// 阶段 1。只吃 resources / views / passes 三张表的「谁读谁写」，不碰任何句柄，
// 因此可以在 image 存在之前调用——这正是两阶段拆分要解决的那个循环。
// 校验失败抛 std::runtime_error。
[[nodiscard]] ResourcePlan planResources(const GraphDesc& desc);

// 编译产物。一步 40 字节，五步落在同一条 cache line 邻域内。
//
// framebuffer 不内联成定长数组：交换链图像数**没有上界**（createSwapchain 取
// minImageCount + 1，而 vkGetSwapchainImagesKHR 还允许驱动返回多于请求的数量），
// 任何定长常数都是一个需要运行期校验的猜测。改走 BakedGraph 上的一张扁平池，
// 基址在 execute 的循环外加载一次，循环内是一次加法一次访存——与内联数组同价，
// 且没有 per-step 的 vector 解引用。
struct BakedStep final {
    std::uint32_t barrierFirst = 0;
    std::uint32_t barrierCount = 0;
    VkPipelineStageFlags barrierSrcStage = 0;
    VkPipelineStageFlags barrierDstStage = 0;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    // framebufferPool_[framebufferFirst + imageIndex * framebufferStride]
    // stride = 0 是单份（shadow），stride = 1 是逐交换链图像一份
    std::uint32_t framebufferFirst = 0;
    std::uint32_t framebufferStride = 0;
    std::uint32_t clearFirst = 0;
    std::uint32_t clearCount = 0;
    VkExtent2D extent{};
    std::uint16_t passIndex = 0;
};

class BakedGraph final {
  public:
    // 阶段 2。校验失败抛 std::runtime_error，消息带 pass / 资源名——
    // 拓扑写错要在加载时报错，不是每帧在渲染线程上报。
    // 编译**不重排步序**：本轮声明序即拓扑序，编译只校验「写者在读者之前」。
    // 重排留给 20f 的光影包前端，那时才有会乱序的声明来源。
    //
    // plan 必须是**同一张 desc** 的阶段 1 产物：编译会逐字段核对两者的资源表。
    // 那是「计划说 A、创建写 B 而没有人比对」这条失效模式的收口——资源是按计划
    // 创建的，desc 与计划一致就等于创建参数与计划一致。
    void compile(const GraphDesc& desc, const ResourcePlan& plan);
    void reset() noexcept;

    // 热路径。零堆分配、零容器查找、每个边界一次 vkCmdPipelineBarrier。
    // 这三条写在 tests/frame_graph_test.cpp 里，不是写在这段注释里。
    //
    // `timestamps` 默认是空句柄 = 不计时，此时一个 vk 入口都不多调（RN-19d0）。
    void execute(VkCommandBuffer commandBuffer, std::uint32_t imageIndex,
                 const PassContext& context,
                 const GpuTimestampWriter& timestamps = {}) const;

    [[nodiscard]] bool empty() const noexcept { return steps_.empty(); }
    [[nodiscard]] std::span<const BakedStep> steps() const noexcept { return steps_; }
    [[nodiscard]] std::span<const VkImageMemoryBarrier> barriers() const noexcept {
        return barrierPool_;
    }
    [[nodiscard]] std::span<const VkFramebuffer> framebuffers() const noexcept {
        return framebufferPool_;
    }
    [[nodiscard]] std::span<const VkClearValue> clears() const noexcept { return clearPool_; }
    // 步序与 `steps()` 一一对应。只给 RN-19d0 的计时报告用，执行期不碰。
    [[nodiscard]] std::span<const std::string_view> stepNames() const noexcept {
        return stepNames_;
    }

  private:
    std::vector<BakedStep> steps_;
    std::vector<std::string_view> stepNames_;
    std::vector<PassBody> bodies_;
    std::vector<VkImageMemoryBarrier> barrierPool_;
    std::vector<VkFramebuffer> framebufferPool_;
    std::vector<VkClearValue> clearPool_;
};

} // namespace mc::render::graph
