// RN-20a：烘焙式 frame graph 的三条执行期契约 + 「照抄现状」的结构断言
//
// 这个测试**自己定义** execute() 调用的三个 Vulkan 入口，因此不链 Vulkan loader：
// 静态链接先解析已定义符号，libvulkan 从头到尾没被拉进来。代价是它只能验调用次数与
// 参数，验不了语义合法性——那归 frame_graph_device_test（lavapipe + 校验层）。
//
// ⚠ 一旦 FrameGraph.cpp 伸手去调第四个 Vulkan 入口，这个目标会**链接失败**。
// 那是特性不是缺陷：它把「execute() 只调 vkCmdPipelineBarrier / vkCmdBeginRenderPass /
// vkCmdEndRenderPass」这条契约钉在链接期。

#include "render/graph/FrameGraph.hpp"

#include <cstdlib>
#include <iostream>
#include <new>
#include <string>
#include <vector>

namespace {

// ---- 被记录的 Vulkan 调用 --------------------------------------------------

struct BarrierCall final {
    VkPipelineStageFlags srcStage = 0;
    VkPipelineStageFlags dstStage = 0;
    std::vector<VkImageMemoryBarrier> barriers;
};

struct BeginCall final {
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkExtent2D extent{};
    std::vector<VkClearValue> clears;
};

struct Trace final {
    // 记录本身会分配。零分配那条断言因此先把记录关掉再量——被量的必须是编排，
    // 不是测试自己的仪表
    bool recording = true;
    std::vector<std::string> order;
    std::vector<BarrierCall> barriers;
    std::vector<BeginCall> begins;
    int endCount = 0;

    void clear() {
        order.clear();
        barriers.clear();
        begins.clear();
        endCount = 0;
    }
};

Trace& trace() {
    static Trace instance;
    return instance;
}

// ---- 堆分配计数 ------------------------------------------------------------
// 全局钩子。execute() 期间的增量必须是 0——它跑在渲染线程上，每帧一次。

std::size_t& allocationCount() {
    static std::size_t count = 0;
    return count;
}

bool& allocationCountingEnabled() {
    static bool enabled = false;
    return enabled;
}

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

} // namespace

void* operator new(std::size_t size) {
    if (allocationCountingEnabled()) {
        ++allocationCount();
    }
    void* memory = std::malloc(size == 0 ? 1 : size);
    if (memory == nullptr) {
        throw std::bad_alloc{};
    }
    return memory;
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

// ---- execute() 调用的三个入口，就地实现 -------------------------------------

extern "C" {

void vkCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags srcStage,
                          VkPipelineStageFlags dstStage, VkDependencyFlags, std::uint32_t,
                          const VkMemoryBarrier*, std::uint32_t, const VkBufferMemoryBarrier*,
                          std::uint32_t imageBarrierCount,
                          const VkImageMemoryBarrier* imageBarriers) {
    if (!trace().recording) {
        return;
    }
    trace().order.emplace_back("barrier");
    BarrierCall call;
    call.srcStage = srcStage;
    call.dstStage = dstStage;
    call.barriers.assign(imageBarriers, imageBarriers + imageBarrierCount);
    trace().barriers.push_back(std::move(call));
}

void vkCmdBeginRenderPass(VkCommandBuffer, const VkRenderPassBeginInfo* info,
                          VkSubpassContents) {
    if (!trace().recording) {
        return;
    }
    trace().order.emplace_back("begin");
    BeginCall call;
    call.renderPass = info->renderPass;
    call.framebuffer = info->framebuffer;
    call.extent = info->renderArea.extent;
    call.clears.assign(info->pClearValues, info->pClearValues + info->clearValueCount);
    trace().begins.push_back(std::move(call));
}

void vkCmdEndRenderPass(VkCommandBuffer) {
    if (!trace().recording) {
        return;
    }
    trace().order.emplace_back("end");
    ++trace().endCount;
}

} // extern "C"

namespace {

using namespace mc::render::graph;

// 句柄是不透明的；测试只要求它们各不相同、且能被逐个认出来
template <typename Handle> Handle handle(std::uintptr_t value) {
    return reinterpret_cast<Handle>(value);
}

void recordNamed(const char* name) {
    if (!trace().recording) {
        return;
    }
    trace().order.emplace_back(name);
}

void bodyUpload(VkCommandBuffer, const PassContext&) { recordNamed("body:upload"); }
void bodyShadow(VkCommandBuffer, const PassContext&) { recordNamed("body:shadow"); }
void bodyWorld(VkCommandBuffer, const PassContext&) { recordNamed("body:world"); }
void bodyGui(VkCommandBuffer, const PassContext&) { recordNamed("body:gui"); }
void bodyPresent(VkCommandBuffer, const PassContext&) { recordNamed("body:present"); }

// ---- 生产拓扑的复刻 ---------------------------------------------------------
// 句柄与尺寸是假的，**结构**是真的：五步、剪枝规则、两套 framebuffer、
// 单份阴影目标、locked 的界面步、以及阴影那条边界屏障。

constexpr std::uint32_t kSwapchainImages = 3;

struct Production final {
    std::vector<ResourceDesc> resources;
    std::vector<ViewDesc> views;
    std::vector<PassAttachment> shadowAttachments;
    std::vector<PassAttachment> worldAttachments;
    std::vector<PassAttachment> guiAttachments;
    std::vector<PassAttachment> presentAttachments;
    std::vector<VkClearValue> worldClears;
    std::vector<VkClearValue> guiClears;
    std::vector<VkClearValue> shadowClears;
    std::vector<VkFramebuffer> worldFramebuffers;
    std::vector<VkFramebuffer> guiFramebuffers;
    std::vector<VkFramebuffer> shadowFramebuffers;
    std::vector<VkImageMemoryBarrier> worldBarriers;
    std::vector<PassDesc> passes;
};

// 生产路径上的三个句柄。测试按它们判断「哪一步绑了哪一套」。
VkRenderPass worldRenderPass() { return handle<VkRenderPass>(0x1000); }
VkRenderPass guiRenderPass() { return handle<VkRenderPass>(0x2000); }
VkRenderPass shadowRenderPass() { return handle<VkRenderPass>(0x3000); }
VkFramebuffer worldFramebuffer(std::uint32_t index) {
    return handle<VkFramebuffer>(0x4000 + index);
}
VkFramebuffer guiFramebuffer(std::uint32_t index) {
    return handle<VkFramebuffer>(0x5000 + index);
}
VkFramebuffer shadowFramebuffer() { return handle<VkFramebuffer>(0x6000); }
VkImage shadowImage() { return handle<VkImage>(0x7000); }

constexpr VkExtent2D kSwapchainExtent{1280, 720};
constexpr VkExtent2D kShadowExtent{2048, 2048};

Production makeProduction(bool shadowEnabled, bool multisampled) {
    Production p;
    p.resources = {
        {.name = "scene_color",
         .kind = ResourceKind::Color,
         .format = VK_FORMAT_B8G8R8A8_UNORM,
         .width = kSwapchainExtent.width,
         .height = kSwapchainExtent.height,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .perSwapchainImage = true},
        {.name = "scene_depth",
         .kind = ResourceKind::Depth,
         .format = VK_FORMAT_D32_SFLOAT,
         .width = kSwapchainExtent.width,
         .height = kSwapchainExtent.height,
         .samples = multisampled ? VK_SAMPLE_COUNT_2_BIT : VK_SAMPLE_COUNT_1_BIT,
         .perSwapchainImage = true},
        {.name = "gui_depth",
         .kind = ResourceKind::Depth,
         .format = VK_FORMAT_D32_SFLOAT,
         .width = kSwapchainExtent.width,
         .height = kSwapchainExtent.height,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .perSwapchainImage = true},
        {.name = "shadow_depth",
         .kind = ResourceKind::Depth,
         .format = VK_FORMAT_D32_SFLOAT,
         .width = kShadowExtent.width,
         .height = kShadowExtent.height,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .perSwapchainImage = false},
    };
    std::uint16_t sceneColorMsaa = 0;
    if (multisampled) {
        sceneColorMsaa = static_cast<std::uint16_t>(p.resources.size());
        p.resources.push_back({.name = "scene_color_msaa",
                               .kind = ResourceKind::Color,
                               .format = VK_FORMAT_B8G8R8A8_UNORM,
                               .width = kSwapchainExtent.width,
                               .height = kSwapchainExtent.height,
                               .samples = VK_SAMPLE_COUNT_2_BIT,
                               .perSwapchainImage = true});
    }
    for (std::size_t index = 0; index < p.resources.size(); ++index) {
        p.views.push_back({.resource = static_cast<std::uint16_t>(index),
                           .format = p.resources[index].format,
                           .aspect = p.resources[index].kind == ResourceKind::Depth
                                         ? VK_IMAGE_ASPECT_DEPTH_BIT
                                         : VK_IMAGE_ASPECT_COLOR_BIT});
    }
    p.shadowAttachments = {{3, Access::DepthWrite}};
    // 开 MSAA 时 scene_color 是 resolve 目标（第三种关系），关时它就是那个 color
    // 附件本身——此时**不能**再多一条 resolve，否则同一个 view 既是 color 又是自己的
    // resolve 目标，planResources() 会拒
    p.worldAttachments = {{sceneColorMsaa, Access::ColorWrite}, {1, Access::DepthWrite}};
    if (multisampled) {
        p.worldAttachments.push_back({0, Access::ColorResolve});
    }
    p.worldAttachments.push_back({3, Access::Sample});
    p.guiAttachments = {{0, Access::ColorWrite}, {2, Access::DepthWrite}};
    p.presentAttachments = {{0, Access::TransferRead}};

    p.worldClears.resize(2);
    p.worldClears[0].color = {{0.055F, 0.080F, 0.110F, 1.0F}};
    p.worldClears[1].depthStencil = {1.0F, 0};
    p.guiClears.resize(2);
    p.guiClears[1].depthStencil = {1.0F, 0};
    p.shadowClears.resize(1);
    p.shadowClears[0].depthStencil = {1.0F, 0};

    for (std::uint32_t index = 0; index < kSwapchainImages; ++index) {
        p.worldFramebuffers.push_back(worldFramebuffer(index));
        p.guiFramebuffers.push_back(guiFramebuffer(index));
    }
    p.shadowFramebuffers = {shadowFramebuffer()};

    VkImageMemoryBarrier shadowRead{};
    shadowRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    shadowRead.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    shadowRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    shadowRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    shadowRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    shadowRead.image = shadowImage();
    shadowRead.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    shadowRead.subresourceRange.levelCount = 1;
    shadowRead.subresourceRange.layerCount = 1;
    shadowRead.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    shadowRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    if (shadowEnabled) {
        p.worldBarriers = {shadowRead};
    }

    p.passes = {
        {.name = "upload", .record = &bodyUpload},
        {.name = "shadow",
         .attachments = p.shadowAttachments,
         .record = &bodyShadow,
         .renderPass = shadowRenderPass(),
         .framebuffers = p.shadowFramebuffers,
         .clears = p.shadowClears,
         .extent = kShadowExtent,
         .enabled = shadowEnabled},
        {.name = "world",
         .attachments = p.worldAttachments,
         .record = &bodyWorld,
         .renderPass = worldRenderPass(),
         .framebuffers = p.worldFramebuffers,
         .clears = p.worldClears,
         .extent = kSwapchainExtent,
         .barriers = p.worldBarriers,
         .barrierSrcStage = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
         .barrierDstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT},
        {.name = "gui",
         .attachments = p.guiAttachments,
         .record = &bodyGui,
         .renderPass = guiRenderPass(),
         .framebuffers = p.guiFramebuffers,
         .clears = p.guiClears,
         .extent = kSwapchainExtent,
         .locked = true},
        {.name = "present_blit", .attachments = p.presentAttachments, .record = &bodyPresent},
    };
    return p;
}

GraphDesc describe(const Production& p) {
    return {.resources = p.resources, .views = p.views, .passes = p.passes};
}

// 两阶段：先推导，再编译。测试里这两步永远成对出现——compile() 会核对计划与描述
// 同源，拿一张别的图的计划来编译是编译期错误
void planAndCompile(BakedGraph& graph, const GraphDesc& desc) {
    graph.compile(desc, planResources(desc));
}

// execute() 的分配计数窗口。测试自己的仪表（vkCmd* 蹦床里的记录）会分配，所以先把
// 记录关掉，量到的才是编排本身。
std::size_t runExecuteCountingAllocations(const BakedGraph& graph, std::uint32_t imageIndex) {
    PassContext context{};
    context.imageIndex = imageIndex;
    trace().recording = false;
    graph.execute(handle<VkCommandBuffer>(0xC0DE), imageIndex, context);  // 预热
    const std::size_t before = allocationCount();
    allocationCountingEnabled() = true;
    graph.execute(handle<VkCommandBuffer>(0xC0DE), imageIndex, context);
    allocationCountingEnabled() = false;
    trace().recording = true;
    return allocationCount() - before;
}

// ---- 1. 结构照抄：五步的顺序、句柄、清空值、渲染区域 ------------------------

void testProductionTopology() {
    const Production p = makeProduction(true, false);
    BakedGraph graph;
    planAndCompile(graph, describe(p));

    check(graph.steps().size() == 5, "启用阴影时应当烘出五步");
    const auto steps = graph.steps();
    check(steps[0].renderPass == VK_NULL_HANDLE, "upload 必须是非渲染步");
    check(steps[0].barrierCount == 0, "upload 不带 image barrier（它那条是 memory barrier）");
    check(steps[1].renderPass == shadowRenderPass(), "shadow 绑阴影 renderpass");
    check(steps[1].framebufferStride == 0, "阴影目标是单份，stride 必须是 0");
    check(steps[1].extent.width == kShadowExtent.width, "阴影渲染区域用阴影图尺寸");
    check(steps[1].clearCount == 1, "阴影只清深度，一个清空值");
    check(steps[2].renderPass == worldRenderPass(), "world 绑世界 renderpass");
    check(steps[2].framebufferStride == 1, "世界靶逐交换链图像一份");
    check(steps[2].clearCount == 2, "世界清 color + depth");
    check(steps[3].renderPass == guiRenderPass(), "gui 绑界面 renderpass");
    check(steps[3].clearCount == 2, "界面清 color（LOAD 时忽略）+ depth");
    check(steps[4].renderPass == VK_NULL_HANDLE, "present_blit 必须是非渲染步");

    // 护栏 1 在现有代码里唯一还踩得到的形态：两套 framebuffer 不能张冠李戴
    const auto framebuffers = graph.framebuffers();
    check(framebuffers[steps[2].framebufferFirst] == worldFramebuffer(0),
          "world 必须绑 framebuffers[]");
    check(framebuffers[steps[3].framebufferFirst] == guiFramebuffer(0),
          "gui 必须绑 guiFramebuffers[]");
    check(framebuffers[steps[1].framebufferFirst] == shadowFramebuffer(),
          "shadow 必须绑离屏目标的那一份");

    const auto clears = graph.clears();
    check(clears[steps[2].clearFirst].color.float32[0] == 0.055F, "世界的天空清空色照抄现状");
    check(clears[steps[2].clearFirst + 1].depthStencil.depth == 1.0F, "世界深度清成 1.0");
    check(clears[steps[3].clearFirst + 1].depthStencil.depth == 1.0F, "界面深度每帧清成 1.0");
    check(clears[steps[1].clearFirst].depthStencil.depth == 1.0F, "阴影深度清成 1.0");
}

// ---- 2. 阴影那条边界屏障：启用时恰好 1 条、逐字段照抄；剪掉时 0 条 -----------

void testShadowBoundaryBarrier() {
    const Production enabled = makeProduction(true, false);
    BakedGraph graph;
    planAndCompile(graph, describe(enabled));
    const auto steps = graph.steps();
    check(steps[2].barrierCount == 1, "启用阴影时世界那步恰好一条边界屏障");
    check(steps[2].barrierSrcStage == VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
          "srcStage 照抄 OffscreenTarget::transitionToShaderRead");
    check(steps[2].barrierDstStage == VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
          "dstStage 照抄 OffscreenTarget::transitionToShaderRead");
    const VkImageMemoryBarrier& barrier = graph.barriers()[steps[2].barrierFirst];
    check(barrier.oldLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, "oldLayout 照抄");
    check(barrier.newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, "newLayout 照抄");
    check(barrier.srcAccessMask == VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, "srcAccess 照抄");
    check(barrier.dstAccessMask == VK_ACCESS_SHADER_READ_BIT, "dstAccess 照抄");
    check(barrier.image == shadowImage(), "屏障作用在阴影深度图上");

    // 剪掉 shadow 时那条屏障必须跟着消失：那张图停在 initializeAsShaderRead 留下的
    // SHADER_READ_ONLY_OPTIMAL 上，再来一次 DEPTH_ATTACHMENT → SHADER_READ 的转换
    // oldLayout 对不上
    const Production pruned = makeProduction(false, false);
    BakedGraph prunedGraph;
    planAndCompile(prunedGraph, describe(pruned));
    check(prunedGraph.steps().size() == 4, "关掉阴影时整步被剪，只剩四步");
    check(prunedGraph.barriers().empty(), "阴影被剪，那条边界屏障必须一起消失");
    check(prunedGraph.steps()[1].renderPass == worldRenderPass(),
          "剪掉之后世界那步顶上来，仍绑世界 renderpass");
}

// ---- 3. 执行期契约：顺序、零分配、每边界一次 barrier ------------------------

void testExecuteOrder() {
    const Production p = makeProduction(true, false);
    BakedGraph graph;
    planAndCompile(graph, describe(p));
    trace().clear();
    PassContext context{};
    context.imageIndex = 2;
    graph.execute(handle<VkCommandBuffer>(0xC0DE), 2, context);

    const std::vector<std::string> expected{
        "body:upload", "begin",     "body:shadow", "end",         "barrier",
        "begin",       "body:world", "end",        "begin",       "body:gui",
        "end",         "body:present"};
    check(trace().order == expected, "执行顺序：上传 → 阴影 → 屏障 → 世界 → 界面 → blit");
    check(trace().begins.size() == 3, "三个渲染步，三次 begin");
    check(trace().endCount == 3, "begin 与 end 必须配对");
    check(trace().begins[1].framebuffer == worldFramebuffer(2),
          "imageIndex 直接用作 per-swapchain-image 的下标");
    check(trace().begins[0].framebuffer == shadowFramebuffer(),
          "单份目标不随 imageIndex 变");
    check(trace().begins[2].framebuffer == guiFramebuffer(2), "界面用 guiFramebuffers[]");
}

void testSingleBarrierCallPerBoundary() {
    // 人造图：一个边界上挂三条屏障。合批的判据是**一次** vkCmdPipelineBarrier 调用、
    // pImageMemoryBarriers 指向池里一段连续区间——反面案例是每个资源一次调用。
    std::vector<ResourceDesc> resources{
        {.name = "a", .format = VK_FORMAT_B8G8R8A8_UNORM, .width = 4, .height = 4},
        {.name = "b", .format = VK_FORMAT_B8G8R8A8_UNORM, .width = 4, .height = 4},
        {.name = "c", .format = VK_FORMAT_B8G8R8A8_UNORM, .width = 4, .height = 4},
    };
    std::vector<ViewDesc> views{{0, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT},
                                {1, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT},
                                {2, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT}};
    std::vector<VkImageMemoryBarrier> barriers(3);
    for (std::uint32_t index = 0; index < 3; ++index) {
        barriers[index].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[index].image = handle<VkImage>(0xA000 + index);
    }
    const std::vector<PassAttachment> attachments{{0, Access::ColorWrite}};
    std::vector<PassDesc> passes{
        {.name = "first", .attachments = attachments, .record = &bodyUpload},
        {.name = "second",
         .attachments = attachments,
         .record = &bodyWorld,
         .barriers = barriers,
         .barrierSrcStage = VK_PIPELINE_STAGE_TRANSFER_BIT,
         .barrierDstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT},
    };
    BakedGraph graph;
    planAndCompile(graph, {.resources = resources, .views = views, .passes = passes});
    trace().clear();
    PassContext context{};
    graph.execute(handle<VkCommandBuffer>(0xC0DE), 0, context);
    check(trace().barriers.size() == 1, "一个边界上的三条屏障必须合成一次调用");
    check(trace().barriers[0].barriers.size() == 3, "三条都在同一次调用里下完");
    check(trace().barriers[0].barriers[0].image == handle<VkImage>(0xA000) &&
              trace().barriers[0].barriers[2].image == handle<VkImage>(0xA002),
          "指向池里一段连续区间，顺序与声明一致");
}

void testZeroAllocation() {
    const Production p = makeProduction(true, false);
    BakedGraph graph;
    planAndCompile(graph, describe(p));
    trace().clear();
    const std::size_t allocations = runExecuteCountingAllocations(graph, 1);
    check(allocations == 0, "execute() 期间的堆分配增量必须是 0");
}

// ---- 4. 编译期校验 ----------------------------------------------------------

bool compileThrows(const Production& p) {
    BakedGraph graph;
    try {
        planAndCompile(graph, describe(p));
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

void testCompileRejections() {
    {
        // 界面那趟必须是最后一个渲染步
        Production p = makeProduction(true, false);
        std::swap(p.passes[2], p.passes[3]);
        check(compileThrows(p), "locked 的界面步不在最后必须被编译期拒掉");
    }
    {
        // 非渲染步不得带 framebuffer——vkCmdResetQueryPool 在 renderpass 内非法，
        // 「非渲染步」这个身份就是上传步的保护
        Production p = makeProduction(true, false);
        p.passes[0].framebuffers = p.worldFramebuffers;
        check(compileThrows(p), "非渲染步带 framebuffer 必须被拒");
    }
    {
        // 单份目标不能有多份 framebuffer
        Production p = makeProduction(true, false);
        p.passes[1].framebuffers = p.worldFramebuffers;
        check(compileThrows(p), "单份的阴影步给了多份 framebuffer 必须被拒");
    }
    {
        // 有屏障却没给 stage
        Production p = makeProduction(true, false);
        p.passes[2].barrierSrcStage = 0;
        check(compileThrows(p), "屏障批没有 stage 必须被拒");
    }
    {
        // 读在写之前
        Production p = makeProduction(true, false);
        std::swap(p.passes[1], p.passes[2]);
        check(compileThrows(p), "世界排在阴影之前（读在写前）必须被拒");
    }
    {
        // 视图指向不存在的资源
        Production p = makeProduction(true, false);
        p.views[0].resource = 99;
        check(compileThrows(p), "视图指向不存在的资源必须被拒");
    }
}

void testMultisampledAttachmentCount() {
    // 开 MSAA 时世界那趟是三个附件（多采样 color + depth + resolve），关时两个。
    // graph 侧的可见后果是资源表多一张 scene_color_msaa，而 scene_color 仍是同一个资源
    const Production off = makeProduction(true, false);
    const Production on = makeProduction(true, true);
    check(off.resources.size() == 4, "关 MSAA 时四个资源");
    check(on.resources.size() == 5, "开 MSAA 时多一张多采样 color");
    check(on.worldAttachments.size() == 4 && off.worldAttachments.size() == 3,
          "开 MSAA 时世界那趟多一条 resolve 声明");
    check(on.worldAttachments[0].view != on.worldAttachments[2].view &&
              on.worldAttachments[2].access == Access::ColorResolve,
          "多采样 color 与 resolve 目标是两个不同的资源槽，后者是 ColorResolve");
    check(off.worldAttachments[0].view == 0 &&
              off.worldAttachments[0].access == Access::ColorWrite,
          "关 MSAA 时 scene_color 就是那个 color 附件本身，没有 resolve 一说");
    BakedGraph graph;
    planAndCompile(graph, describe(on));
    check(graph.steps().size() == 5, "开 MSAA 不改变步数");
}

// ---- 5. 推导结果与手写现状逐位相同（RN-20c 的核心判据）----------------------
//
// 下面这张表是从五个创建函数里**逐个抄下来**的，不是从推导反推出来的：
//
//   createSceneTargets      scene_color        COLOR_ATTACHMENT | TRANSFER_SRC        1 采样
//   createDepthTargets      scene_depth        DEPTH_STENCIL_ATTACHMENT | TRANSIENT   N 采样
//   createGuiDepthTargets   gui_depth          DEPTH_STENCIL_ATTACHMENT | TRANSIENT   1 采样
//   createColorTargets      scene_color_msaa   TRANSIENT | COLOR_ATTACHMENT           N 采样
//   OffscreenTarget::init   shadow_depth       DEPTH_STENCIL_ATTACHMENT | SAMPLED     1 采样
//
// 加上三个 renderpass 里逐个附件的 loadOp / storeOp / initialLayout / finalLayout
// （createRenderPass 的 MSAA 两档、createGuiRenderPass、OffscreenTarget::init）。
// 「差不多」不算：这里比的是每一个 bit。

void expectResource(const ResourcePlan& plan, std::string_view name, VkImageUsageFlags usage,
                    VkSampleCountFlagBits samples, VkImageAspectFlags aspect, const char* what) {
    const PlannedResource& planned = plan.resource(name);
    check(planned.usage == usage && planned.samples == samples && planned.aspect == aspect, what);
}

void expectOps(const ResourcePlan& plan, std::string_view pass, std::string_view resource,
               VkAttachmentLoadOp loadOp, VkAttachmentStoreOp storeOp,
               VkImageLayout initialLayout, VkImageLayout finalLayout, const char* what) {
    const ResourceOps& ops = plan.ops(pass, resource);
    check(ops.loadOp == loadOp && ops.storeOp == storeOp && ops.initialLayout == initialLayout &&
              ops.finalLayout == finalLayout,
          what);
}

constexpr VkImageUsageFlags kColorAttachment = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
constexpr VkImageUsageFlags kDepthAttachment = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
constexpr VkImageUsageFlags kTransient = VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
constexpr VkImageLayout kColorOptimal = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
constexpr VkImageLayout kDepthOptimal = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
constexpr VkImageLayout kUndefined = VK_IMAGE_LAYOUT_UNDEFINED;
constexpr VkImageLayout kTransferSrc = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

void testDerivationMatchesHandWrittenSingleSampled() {
    const Production p = makeProduction(true, false);
    const ResourcePlan plan = planResources(describe(p));

    // createSceneTargets：COLOR_ATTACHMENT | TRANSFER_SRC，单采样，**不是**瞬态。
    // TRANSFER_SRC 来自 present_blit 那步的 TransferRead；漏掉那个读者，这里会变成
    // TRANSIENT + DONT_CARE，真机上是整帧变黑
    expectResource(plan, "scene_color", kColorAttachment | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                   VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                   "scene_color 的 usage 与 createSceneTargets 逐位相同");
    // createDepthTargets：今天没有消费者，所以是瞬态。RN-11 接上消费者时翻转的是它
    expectResource(plan, "scene_depth", kDepthAttachment | kTransient, VK_SAMPLE_COUNT_1_BIT,
                   VK_IMAGE_ASPECT_DEPTH_BIT,
                   "scene_depth 的 usage 与 createDepthTargets 逐位相同");
    // createGuiDepthTargets：每帧 CLEAR 且**永远**不会被读，与上一条同形不同因
    expectResource(plan, "gui_depth", kDepthAttachment | kTransient, VK_SAMPLE_COUNT_1_BIT,
                   VK_IMAGE_ASPECT_DEPTH_BIT,
                   "gui_depth 的 usage 与 createGuiDepthTargets 逐位相同");
    // OffscreenTarget::init：binding 8 的描述符在采样它，所以 SAMPLED、不是瞬态
    expectResource(plan, "shadow_depth", kDepthAttachment | VK_IMAGE_USAGE_SAMPLED_BIT,
                   VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
                   "shadow_depth 的 usage 与 OffscreenTarget::init 逐位相同");
    check(!plan.has("scene_color_msaa"), "关 MSAA 时多采样靶整个不存在，与 createColorTargets 的 early return 一致");

    // createRenderPass（关 MSAA 档）
    expectOps(plan, "world", "scene_color", VK_ATTACHMENT_LOAD_OP_CLEAR,
              VK_ATTACHMENT_STORE_OP_STORE, kUndefined, kColorOptimal,
              "world/scene_color 的四个操作与 createRenderPass 单采样档逐位相同");
    expectOps(plan, "world", "scene_depth", VK_ATTACHMENT_LOAD_OP_CLEAR,
              VK_ATTACHMENT_STORE_OP_DONT_CARE, kUndefined, kDepthOptimal,
              "world/scene_depth 的四个操作与 createRenderPass 逐位相同");
    // createGuiRenderPass：LOAD 世界那趟的结果，画完转 TRANSFER_SRC 给 copy
    expectOps(plan, "gui", "scene_color", VK_ATTACHMENT_LOAD_OP_LOAD,
              VK_ATTACHMENT_STORE_OP_STORE, kColorOptimal, kTransferSrc,
              "gui/scene_color 的四个操作与 createGuiRenderPass 逐位相同");
    expectOps(plan, "gui", "gui_depth", VK_ATTACHMENT_LOAD_OP_CLEAR,
              VK_ATTACHMENT_STORE_OP_DONT_CARE, kUndefined, kDepthOptimal,
              "gui/gui_depth 的四个操作与 createGuiRenderPass 逐位相同");
    // OffscreenTarget::init：CLEAR/STORE，finalLayout 停在 DEPTH_ATTACHMENT——
    // 转成 SHADER_READ_ONLY 的是世界那步的边界屏障，不是这个 renderpass
    expectOps(plan, "shadow", "shadow_depth", VK_ATTACHMENT_LOAD_OP_CLEAR,
              VK_ATTACHMENT_STORE_OP_STORE, kUndefined, kDepthOptimal,
              "shadow/shadow_depth 的四个操作与 OffscreenTarget::init 逐位相同");
    // Sample 与 TransferRead 是描述符采样与 vkCmdCopyImage，不是附件，不产生条目
    check(plan.ops().size() == 5, "五条附件操作，一条不多：读者不是附件");
}

void testDerivationMatchesHandWrittenMultisampled() {
    const Production p = makeProduction(true, true);
    const ResourcePlan plan = planResources(describe(p));

    // 开 MSAA 时 scene_color 是 resolve 目标：仍然是 COLOR_ATTACHMENT | TRANSFER_SRC、
    // 单采样、非瞬态——与关 MSAA 时**同一个答案**，因为读者集合没变
    expectResource(plan, "scene_color", kColorAttachment | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                   VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                   "MSAA 档 scene_color 的 usage 仍与 createSceneTargets 逐位相同");
    // createColorTargets：TRANSIENT | COLOR_ATTACHMENT，多采样。画完 resolve 就丢
    expectResource(plan, "scene_color_msaa", kTransient | kColorAttachment,
                   VK_SAMPLE_COUNT_2_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                   "scene_color_msaa 的 usage 与 createColorTargets 逐位相同");
    expectResource(plan, "scene_depth", kDepthAttachment | kTransient, VK_SAMPLE_COUNT_2_BIT,
                   VK_IMAGE_ASPECT_DEPTH_BIT,
                   "MSAA 档 scene_depth 跟着世界那趟的采样数走");

    // createRenderPass（开 MSAA 档）：0 号是多采样靶，2 号是 resolve
    expectOps(plan, "world", "scene_color_msaa", VK_ATTACHMENT_LOAD_OP_CLEAR,
              VK_ATTACHMENT_STORE_OP_DONT_CARE, kUndefined, kColorOptimal,
              "world/scene_color_msaa 的四个操作与 createRenderPass 多采样档逐位相同");
    // ⚠ 本轮最容易错的一处：resolve 目标的 loadOp 是 DONT_CARE，不是「前面没写者就
    // CLEAR」。它不是 ColorWrite 的一个变体，是第三种关系
    expectOps(plan, "world", "scene_color", VK_ATTACHMENT_LOAD_OP_DONT_CARE,
              VK_ATTACHMENT_STORE_OP_STORE, kUndefined, kColorOptimal,
              "world/scene_color 作为 resolve 目标：loadOp DONT_CARE、storeOp STORE");
    expectOps(plan, "world", "scene_depth", VK_ATTACHMENT_LOAD_OP_CLEAR,
              VK_ATTACHMENT_STORE_OP_DONT_CARE, kUndefined, kDepthOptimal,
              "MSAA 档 world/scene_depth 的四个操作不变");
    expectOps(plan, "gui", "scene_color", VK_ATTACHMENT_LOAD_OP_LOAD,
              VK_ATTACHMENT_STORE_OP_STORE, kColorOptimal, kTransferSrc,
              "MSAA 档界面那趟的四个操作不变");
}

// 深度格式带 stencil 时 aspect 要跟着走。写死 DEPTH_BIT 在 D32_SFLOAT_S8_UINT 上是错的
void testStencilAspectFollowsFormat() {
    Production p = makeProduction(true, false);
    for (auto& resource : p.resources) {
        if (resource.format == VK_FORMAT_D32_SFLOAT) {
            resource.format = VK_FORMAT_D32_SFLOAT_S8_UINT;
        }
    }
    for (auto& view : p.views) {
        view.format = p.resources[view.resource].format;
    }
    const ResourcePlan plan = planResources(describe(p));
    const VkImageAspectFlags expected =
        VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    check(plan.resource("scene_depth").aspect == expected &&
              plan.resource("gui_depth").aspect == expected &&
              plan.resource("shadow_depth").aspect == expected,
          "带 stencil 的深度格式必须推出 DEPTH|STENCIL 两个 aspect 位");
    check(plan.resource("scene_color").aspect == VK_IMAGE_ASPECT_COLOR_BIT,
          "颜色资源的 aspect 不受深度格式影响");
}

// ---- 6. 人造消费者：RN-11 与 20d 将来要走的那条路，先在这里钉住 --------------

void bodyFakeConsumer(VkCommandBuffer, const PassContext&) { recordNamed("body:fake"); }

void testSyntheticDepthConsumerFlipsDerivation() {
    // 基线：scene_depth 今天没有消费者 → TRANSIENT + DONT_CARE
    const Production baseline = makeProduction(true, false);
    const ResourcePlan before = planResources(describe(baseline));
    check(before.resource("scene_depth").usage == (kDepthAttachment | kTransient),
          "没有消费者时 scene_depth 是瞬态");
    check(before.ops("world", "scene_depth").storeOp == VK_ATTACHMENT_STORE_OP_DONT_CARE,
          "没有消费者时世界那趟不必存深度");

    // 给它接上一个声明 Access::Sample 的假 pass（放在 gui 之前——gui 是 locked 的）
    Production consumer = makeProduction(true, false);
    const std::vector<PassAttachment> fakeAttachments{{1, Access::Sample}};
    std::vector<PassDesc> passes(consumer.passes.begin(), consumer.passes.end());
    passes.insert(passes.begin() + 3, PassDesc{.name = "fake_ssao",
                                               .attachments = fakeAttachments,
                                               .record = &bodyFakeConsumer});
    consumer.passes = passes;
    const ResourcePlan after = planResources(describe(consumer));
    check(after.resource("scene_depth").usage ==
              (kDepthAttachment | VK_IMAGE_USAGE_SAMPLED_BIT),
          "有 Sample 消费者时 scene_depth 变成 SAMPLED 且**丢掉** TRANSIENT");
    check((after.resource("scene_depth").usage & kTransient) == 0U,
          "瞬态位必须消失，不是又加了一个位");
    check(after.ops("world", "scene_depth").storeOp == VK_ATTACHMENT_STORE_OP_STORE,
          "有消费者时世界那趟必须存深度");
    // gui_depth 与它同形不同因：接上 scene_depth 的消费者不该动到界面深度
    check(after.resource("gui_depth").usage == (kDepthAttachment | kTransient),
          "gui_depth 不跟着翻转——两条 TRANSIENT 的理由不同");

    // 去掉那个 pass，一切变回去。推导没有记忆
    const ResourcePlan back = planResources(describe(baseline));
    check(back.resource("scene_depth").usage == (kDepthAttachment | kTransient) &&
              back.ops("world", "scene_depth").storeOp == VK_ATTACHMENT_STORE_OP_DONT_CARE,
          "拿掉消费者后变回 TRANSIENT + DONT_CARE");
}

// ---- 7. SunShadows 翻转不改变任何 image 参数（坑 5 的结论）-------------------

void testShadowToggleDoesNotChangeImageParameters() {
    const Production on = makeProduction(true, false);
    const Production off = makeProduction(false, false);
    const ResourcePlan enabled = planResources(describe(on));
    const ResourcePlan disabled = planResources(describe(off));

    check(enabled.resources().size() == disabled.resources().size(),
          "翻转开关不改变资源集合");
    bool same = true;
    for (std::size_t index = 0; index < enabled.resources().size(); ++index) {
        same = same && enabled.resources()[index].sameImageParameters(disabled.resources()[index]);
    }
    check(same, "五个资源的 image 参数（含 usage/aspect）逐位相同，所以翻转开关只重编译");

    // 具体到那张图：读者集合与开关无关（binding 8 的描述符恒采样它），所以 SAMPLED
    // 在两档都在，TRANSIENT 在两档都不在——**关掉时它在图内没有写者，而无写者的资源
    // 永不瞬态**，那条规则就是为这一档写的
    check(disabled.resource("shadow_depth").usage ==
              (kDepthAttachment | VK_IMAGE_USAGE_SAMPLED_BIT),
          "剪掉 shadow 步之后 shadow_depth 的 usage 一位不变");
    check((disabled.resource("shadow_depth").usage & kTransient) == 0U,
          "图内没有写者的资源永远不是瞬态：它的内容来自图外");

    // 变的那个字段不是 image 参数：shadow 那步的 storeOp 随着步一起消失。
    // 谁把 usage 做成依赖开关的，上面那条断言会当场红
    check(enabled.ops("shadow", "shadow_depth").storeOp == VK_ATTACHMENT_STORE_OP_STORE,
          "开着的时候 shadow 那趟必须存下深度给世界那趟采样");
    bool threw = false;
    try {
        (void)disabled.ops("shadow", "shadow_depth");
    } catch (const std::exception&) {
        threw = true;
    }
    check(threw, "关掉之后根本没有消费 shadow_depth 的 renderpass，storeOp 落不到任何对象上");
}

// 「图内没有写者的资源永不瞬态」这条规则单独立一个测试，因为上面那个测试**抓不住它**：
// 关掉阴影时 shadow_depth 仍有 world 那步的 Sample 读者，光靠「最后一次写之后有读者」
// 就已经躲开了瞬态。这条规则真正兜住的是**图内一次都没被碰过**的资源——它的内容整个
// 来自图外（上一帧、或者 initializeAsShaderRead 留下的布局），判成瞬态就是让驱动
// 有权丢掉它。把 world 那步的 Sample 声明也去掉，就是那个形态。
void testUntouchedResourceIsNeverTransient() {
    Production p = makeProduction(false, false);
    // 去掉世界那步对 shadow_depth 的 Sample 声明（坑 3 那条），于是关掉阴影之后
    // 这张图在图内既没有写者也没有读者
    std::vector<PassAttachment> world;
    for (const PassAttachment& attachment : p.worldAttachments) {
        if (attachment.access != Access::Sample) {
            world.push_back(attachment);
        }
    }
    p.worldAttachments = world;
    p.passes[2].attachments = p.worldAttachments;
    const ResourcePlan plan = planResources(describe(p));
    check((plan.resource("shadow_depth").usage & kTransient) == 0U,
          "图内一次都没被碰过的资源不是瞬态：它的内容来自图外");
    check(plan.resource("shadow_depth").usage == kDepthAttachment,
          "没有任何消费者时它只剩基础位，既不 SAMPLED 也不瞬态");
}

// ---- 8. 计划与描述必须同源 --------------------------------------------------

void testCompileRejectsForeignPlan() {
    const Production on = makeProduction(true, true);
    const Production off = makeProduction(true, false);
    BakedGraph graph;
    bool threw = false;
    try {
        // 拿 MSAA 档的计划去编译单采样档的图：资源表对不上
        graph.compile(describe(off), planResources(describe(on)));
    } catch (const std::exception&) {
        threw = true;
    }
    check(threw, "计划与描述不同源必须被编译期拒掉");
}

void testPlanRejectsSelfResolve() {
    // 同一个 view 既是 color 附件又是自己的 resolve 目标——关 MSAA 时如果忘了把
    // resolve 那一条去掉，就是这个形态
    Production p = makeProduction(true, false);
    std::vector<PassAttachment> broken(p.worldAttachments.begin(), p.worldAttachments.end());
    broken.push_back({0, Access::ColorResolve});
    p.worldAttachments = broken;
    p.passes[2].attachments = p.worldAttachments;
    bool threw = false;
    try {
        (void)planResources(describe(p));
    } catch (const std::exception&) {
        threw = true;
    }
    check(threw, "一个 view 同时是 color 与自己的 resolve 目标必须被拒");
}

} // namespace

int main() {
    testProductionTopology();
    testShadowBoundaryBarrier();
    testExecuteOrder();
    testSingleBarrierCallPerBoundary();
    testZeroAllocation();
    testCompileRejections();
    testMultisampledAttachmentCount();
    testDerivationMatchesHandWrittenSingleSampled();
    testDerivationMatchesHandWrittenMultisampled();
    testStencilAspectFollowsFormat();
    testSyntheticDepthConsumerFlipsDerivation();
    testShadowToggleDoesNotChangeImageParameters();
    testUntouchedResourceIsNeverTransient();
    testCompileRejectsForeignPlan();
    testPlanRejectsSelfResolve();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "frame_graph_test ok\n";
    return 0;
}
