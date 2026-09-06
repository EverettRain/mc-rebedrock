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
    p.worldAttachments = {{sceneColorMsaa, Access::ColorWrite},
                          {1, Access::DepthWrite},
                          {0, Access::ColorWrite},
                          {3, Access::Sample}};
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
    graph.compile(describe(p));

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
    graph.compile(describe(enabled));
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
    prunedGraph.compile(describe(pruned));
    check(prunedGraph.steps().size() == 4, "关掉阴影时整步被剪，只剩四步");
    check(prunedGraph.barriers().empty(), "阴影被剪，那条边界屏障必须一起消失");
    check(prunedGraph.steps()[1].renderPass == worldRenderPass(),
          "剪掉之后世界那步顶上来，仍绑世界 renderpass");
}

// ---- 3. 执行期契约：顺序、零分配、每边界一次 barrier ------------------------

void testExecuteOrder() {
    const Production p = makeProduction(true, false);
    BakedGraph graph;
    graph.compile(describe(p));
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
    graph.compile({.resources = resources, .views = views, .passes = passes});
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
    graph.compile(describe(p));
    trace().clear();
    const std::size_t allocations = runExecuteCountingAllocations(graph, 1);
    check(allocations == 0, "execute() 期间的堆分配增量必须是 0");
}

// ---- 4. 编译期校验 ----------------------------------------------------------

bool compileThrows(const Production& p) {
    BakedGraph graph;
    try {
        graph.compile(describe(p));
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
    check(on.worldAttachments[0].view != on.worldAttachments[2].view,
          "多采样 color 与 resolve 目标是两个不同的资源槽");
    check(off.worldAttachments[0].view == off.worldAttachments[2].view,
          "关 MSAA 时两者是同一个 scene_color");
    BakedGraph graph;
    graph.compile(describe(on));
    check(graph.steps().size() == 5, "开 MSAA 不改变步数");
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
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "frame_graph_test ok\n";
    return 0;
}
