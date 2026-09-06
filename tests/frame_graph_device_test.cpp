// RN-20a：把编译出来的图真的跑一遍，让 Khronos 校验层判它的语义合法性
//
// frame_graph_test 用桩验的是**调用次数与参数**；这里验的是**语义**：
// 布局转换的 oldLayout 对不对得上、屏障有没有落在 renderpass 内部、
// vkCmdResetQueryPool 是不是留在了任何 renderpass 之外。
//
// 这条正是本轮设计里最容易踩的那个错：`shadowTarget.transitionToShaderRead()` 原先跑在
// `vkCmdEndRenderPass` 之后；begin/end 一归 graph，那个位置就落进 renderpass 内部了，
// 而对附件做 layout 转换在 renderpass 内非法。桩抓不到它，校验层一句话就报出来。
//
// 不需要 surface、不需要交换链、不需要显示器：离屏 image + lavapipe 就够。
// 拿不到设备或校验层时**跳过并返回成功**——它是加分项，不是门禁的替代品。

#include "render/graph/FrameGraph.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace mc::render::graph;

constexpr std::uint32_t kWidth = 64;
constexpr std::uint32_t kHeight = 64;
// 「逐交换链图像一份」那条路径要真的被走到，至少得有两份
constexpr std::uint32_t kImageCount = 2;
constexpr std::uint32_t kQueryCount = 4;

std::vector<std::string>& validationErrors() {
    static std::vector<std::string> errors;
    return errors;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                            VkDebugUtilsMessageTypeFlagsEXT,
                                            const VkDebugUtilsMessengerCallbackDataEXT* data,
                                            void*) {
    if ((severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)) != 0U &&
        data != nullptr && data->pMessage != nullptr) {
        validationErrors().emplace_back(data->pMessage);
    }
    return VK_FALSE;
}

[[nodiscard]] bool ok(VkResult result) { return result == VK_SUCCESS; }

void skip(const char* why) {
    std::cout << "frame_graph_device_test skipped: " << why << '\n';
}

// ---- 极简资源持有者：这个测试不碰 VMA，逐图像各分配一块 ---------------------

struct Device final {
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queueFamily = 0;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory{};

    [[nodiscard]] std::optional<std::uint32_t> memoryType(std::uint32_t bits,
                                                          VkMemoryPropertyFlags flags) const {
        for (std::uint32_t index = 0; index < memory.memoryTypeCount; ++index) {
            if ((bits & (1U << index)) != 0U &&
                (memory.memoryTypes[index].propertyFlags & flags) == flags) {
                return index;
            }
        }
        return std::nullopt;
    }
};

struct Image final {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

bool createImage(const Device& gpu, VkFormat format, VkImageUsageFlags usage,
                 VkImageAspectFlags aspect, Image& out) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {kWidth, kHeight, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!ok(vkCreateImage(gpu.device, &info, nullptr, &out.image))) {
        return false;
    }
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(gpu.device, out.image, &requirements);
    const auto type =
        gpu.memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!type.has_value()) {
        return false;
    }
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = *type;
    if (!ok(vkAllocateMemory(gpu.device, &allocation, nullptr, &out.memory))) {
        return false;
    }
    if (!ok(vkBindImageMemory(gpu.device, out.image, out.memory, 0))) {
        return false;
    }
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = out.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange = {aspect, 0, 1, 0, 1};
    return ok(vkCreateImageView(gpu.device, &viewInfo, nullptr, &out.view));
}

// 深度附件 + 可采样：阴影图的形态
VkRenderPass createShadowRenderPass(const Device& gpu, VkFormat depthFormat) {
    VkAttachmentDescription depth{};
    depth.format = depthFormat;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pDepthStencilAttachment = &reference;
    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments = &depth;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    VkRenderPass pass = VK_NULL_HANDLE;
    return ok(vkCreateRenderPass(gpu.device, &info, nullptr, &pass)) ? pass : VK_NULL_HANDLE;
}

// 世界那趟：清空、画完把 color 留在 COLOR_ATTACHMENT_OPTIMAL 交给界面那趟
VkRenderPass createWorldRenderPass(const Device& gpu, VkFormat colorFormat,
                                   VkFormat depthFormat) {
    std::array<VkAttachmentDescription, 2> attachments{};
    attachments[0].format = colorFormat;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[1].format = depthFormat;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color;
    subpass.pDepthStencilAttachment = &depth;
    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = static_cast<std::uint32_t>(attachments.size());
    info.pAttachments = attachments.data();
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    VkRenderPass pass = VK_NULL_HANDLE;
    return ok(vkCreateRenderPass(gpu.device, &info, nullptr, &pass)) ? pass : VK_NULL_HANDLE;
}

// 界面那趟：载入世界那趟的结果，画完留在 TRANSFER_SRC_OPTIMAL 交给帧末的 copy。
// 那个 finalLayout 就是护栏 3——方块预览导出正是从这个布局读回场景图的。
VkRenderPass createGuiRenderPass(const Device& gpu, VkFormat colorFormat, VkFormat depthFormat) {
    std::array<VkAttachmentDescription, 2> attachments{};
    attachments[0].format = colorFormat;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    attachments[1].format = depthFormat;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color;
    subpass.pDepthStencilAttachment = &depth;
    VkSubpassDependency dependency{};
    dependency.srcSubpass = 0;
    dependency.dstSubpass = VK_SUBPASS_EXTERNAL;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependency.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = static_cast<std::uint32_t>(attachments.size());
    info.pAttachments = attachments.data();
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dependency;
    VkRenderPass pass = VK_NULL_HANDLE;
    return ok(vkCreateRenderPass(gpu.device, &info, nullptr, &pass)) ? pass : VK_NULL_HANDLE;
}

VkFramebuffer createFramebuffer(const Device& gpu, VkRenderPass pass,
                                std::span<const VkImageView> attachments) {
    VkFramebufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass = pass;
    info.attachmentCount = static_cast<std::uint32_t>(attachments.size());
    info.pAttachments = attachments.data();
    info.width = kWidth;
    info.height = kHeight;
    info.layers = 1;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    return ok(vkCreateFramebuffer(gpu.device, &info, nullptr, &framebuffer)) ? framebuffer
                                                                            : VK_NULL_HANDLE;
}

// ---- 场景：生产拓扑在离屏设备上的复刻 --------------------------------------

struct Scene final {
    const Device* gpu = nullptr;
    Image shadowDepth;
    std::array<Image, kImageCount> sceneColor{};
    std::array<Image, kImageCount> sceneDepth{};
    std::array<Image, kImageCount> guiDepth{};
    Image presentTarget;  // 交换链图像的替身
    VkRenderPass shadowPass = VK_NULL_HANDLE;
    VkRenderPass worldPass = VK_NULL_HANDLE;
    VkRenderPass guiPass = VK_NULL_HANDLE;
    VkFramebuffer shadowFramebuffer = VK_NULL_HANDLE;
    std::array<VkFramebuffer, kImageCount> worldFramebuffers{};
    std::array<VkFramebuffer, kImageCount> guiFramebuffers{};
    VkQueryPool queryPool = VK_NULL_HANDLE;
};

// pass body。内容不是生产代码的搬运（这里没有管线也没有网格），但**位置**是真的：
// 上传步里那两样东西（查询池 reset、TRANSFER → VERTEX_INPUT 的 memory barrier）
// 一旦落进 renderpass 内部就是非法，校验层会报。
void bodyUpload(VkCommandBuffer commandBuffer, const PassContext& context) {
    const auto& scene = *static_cast<const Scene*>(context.user);
    vkCmdResetQueryPool(commandBuffer, scene.queryPool, 0, kQueryCount);
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &barrier, 0, nullptr, 0,
                         nullptr);
}

// 零投射者的阴影通道：契约就是「一个都没有也照样 begin/end」，body 因此可以是空的
void bodyEmpty(VkCommandBuffer, const PassContext&) {}

void bodyPresentBlit(VkCommandBuffer commandBuffer, const PassContext& context) {
    const auto& scene = *static_cast<const Scene*>(context.user);
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = scene.presentTarget.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    region.extent = {kWidth, kHeight, 1};
    // 源必须已经在 TRANSFER_SRC_OPTIMAL 上——那是 GUI renderpass 的 finalLayout（护栏 3）
    vkCmdCopyImage(commandBuffer, scene.sceneColor[context.imageIndex].image,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scene.presentTarget.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = 0;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);
}

void buildGraph(const Scene& scene, bool shadowEnabled, BakedGraph& graph,
                VkFormat colorFormat, VkFormat depthFormat) {
    static std::vector<ResourceDesc> resources;
    static std::vector<ViewDesc> views;
    resources = {
        {.name = "scene_color", .kind = ResourceKind::Color, .format = colorFormat,
         .width = kWidth, .height = kHeight, .samples = VK_SAMPLE_COUNT_1_BIT,
         .perSwapchainImage = true},
        {.name = "scene_depth", .kind = ResourceKind::Depth, .format = depthFormat,
         .width = kWidth, .height = kHeight, .samples = VK_SAMPLE_COUNT_1_BIT,
         .perSwapchainImage = true},
        {.name = "gui_depth", .kind = ResourceKind::Depth, .format = depthFormat,
         .width = kWidth, .height = kHeight, .samples = VK_SAMPLE_COUNT_1_BIT,
         .perSwapchainImage = true},
        {.name = "shadow_depth", .kind = ResourceKind::Depth, .format = depthFormat,
         .width = kWidth, .height = kHeight, .samples = VK_SAMPLE_COUNT_1_BIT,
         .perSwapchainImage = false},
    };
    views = {{0, colorFormat, VK_IMAGE_ASPECT_COLOR_BIT},
             {1, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT},
             {2, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT},
             {3, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT}};

    static const std::array<PassAttachment, 1> shadowAttachments{{{3, Access::DepthWrite}}};
    static const std::array<PassAttachment, 3> worldAttachments{
        {{0, Access::ColorWrite}, {1, Access::DepthWrite}, {3, Access::Sample}}};
    static const std::array<PassAttachment, 2> guiAttachments{
        {{0, Access::ColorWrite}, {2, Access::DepthWrite}}};
    static const std::array<PassAttachment, 1> presentAttachments{{{0, Access::TransferRead}}};

    static std::array<VkClearValue, 2> worldClears{};
    worldClears[0].color = {{0.055F, 0.080F, 0.110F, 1.0F}};
    worldClears[1].depthStencil = {1.0F, 0};
    static std::array<VkClearValue, 2> guiClears{};
    guiClears[1].depthStencil = {1.0F, 0};
    static std::array<VkClearValue, 1> shadowClears{};
    shadowClears[0].depthStencil = {1.0F, 0};
    static std::array<VkFramebuffer, 1> shadowFramebuffers{};
    shadowFramebuffers[0] = scene.shadowFramebuffer;

    static std::array<VkImageMemoryBarrier, 1> worldBarriers{};
    worldBarriers[0] = {};
    worldBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    worldBarriers[0].oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    worldBarriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    worldBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    worldBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    worldBarriers[0].image = scene.shadowDepth.image;
    worldBarriers[0].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    worldBarriers[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    worldBarriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    static std::vector<PassDesc> passes;
    passes = {
        {.name = "upload", .record = &bodyUpload},
        {.name = "shadow",
         .attachments = shadowAttachments,
         .record = &bodyEmpty,
         .renderPass = scene.shadowPass,
         .framebuffers = shadowFramebuffers,
         .clears = shadowClears,
         .extent = {kWidth, kHeight},
         .enabled = shadowEnabled},
        {.name = "world",
         .attachments = worldAttachments,
         .record = &bodyEmpty,
         .renderPass = scene.worldPass,
         .framebuffers = scene.worldFramebuffers,
         .clears = worldClears,
         .extent = {kWidth, kHeight},
         .barriers = shadowEnabled ? std::span<const VkImageMemoryBarrier>{worldBarriers}
                                   : std::span<const VkImageMemoryBarrier>{},
         .barrierSrcStage = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
         .barrierDstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT},
        {.name = "gui",
         .attachments = guiAttachments,
         .record = &bodyEmpty,
         .renderPass = scene.guiPass,
         .framebuffers = scene.guiFramebuffers,
         .clears = guiClears,
         .extent = {kWidth, kHeight},
         .locked = true},
        {.name = "present_blit", .attachments = presentAttachments, .record = &bodyPresentBlit},
    };
    graph.compile({.resources = resources, .views = views, .passes = passes});
}

bool submitOnce(const Device& gpu, const BakedGraph& graph, const Scene& scene,
                std::uint32_t imageIndex) {
    VkCommandBufferAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocation.commandPool = gpu.commandPool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    if (!ok(vkAllocateCommandBuffers(gpu.device, &allocation, &commandBuffer))) {
        return false;
    }
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!ok(vkBeginCommandBuffer(commandBuffer, &begin))) {
        return false;
    }
    PassContext context{};
    context.user = const_cast<Scene*>(&scene);
    context.imageIndex = imageIndex;
    graph.execute(commandBuffer, imageIndex, context);
    if (!ok(vkEndCommandBuffer(commandBuffer))) {
        return false;
    }
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commandBuffer;
    if (!ok(vkQueueSubmit(gpu.queue, 1, &submit, VK_NULL_HANDLE))) {
        return false;
    }
    const bool waited = ok(vkQueueWaitIdle(gpu.queue));
    vkFreeCommandBuffers(gpu.device, gpu.commandPool, 1, &commandBuffer);
    return waited;
}

// 建好之后把阴影图从 UNDEFINED 直接带到 SHADER_READ_ONLY，
// 与 OffscreenTarget::initializeAsShaderRead 同形——关掉阴影时全靠它保持布局合法
bool initializeShadowAsShaderRead(const Device& gpu, const Scene& scene) {
    VkCommandBufferAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocation.commandPool = gpu.commandPool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    if (!ok(vkAllocateCommandBuffers(gpu.device, &allocation, &commandBuffer))) {
        return false;
    }
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!ok(vkBeginCommandBuffer(commandBuffer, &begin))) {
        return false;
    }
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = scene.shadowDepth.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);
    if (!ok(vkEndCommandBuffer(commandBuffer))) {
        return false;
    }
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commandBuffer;
    if (!ok(vkQueueSubmit(gpu.queue, 1, &submit, VK_NULL_HANDLE))) {
        return false;
    }
    const bool waited = ok(vkQueueWaitIdle(gpu.queue));
    vkFreeCommandBuffers(gpu.device, gpu.commandPool, 1, &commandBuffer);
    return waited;
}

bool createDevice(Device& gpu) {
    std::uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> layers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
    bool hasValidation = false;
    for (const auto& layer : layers) {
        if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
            hasValidation = true;
        }
    }
    if (!hasValidation) {
        skip("VK_LAYER_KHRONOS_validation not installed");
        return false;
    }

    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "frame_graph_device_test";
    application.apiVersion = VK_API_VERSION_1_2;
    const char* const layerNames[]{"VK_LAYER_KHRONOS_validation"};
    const char* const extensionNames[]{VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    VkDebugUtilsMessengerCreateInfoEXT messenger{};
    messenger.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
    messenger.pfnUserCallback = &debugCallback;
    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pNext = &messenger;
    instanceInfo.pApplicationInfo = &application;
    instanceInfo.enabledLayerCount = 1;
    instanceInfo.ppEnabledLayerNames = layerNames;
    instanceInfo.enabledExtensionCount = 1;
    instanceInfo.ppEnabledExtensionNames = extensionNames;
    if (!ok(vkCreateInstance(&instanceInfo, nullptr, &gpu.instance))) {
        skip("vkCreateInstance failed");
        return false;
    }
    const auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(gpu.instance, "vkCreateDebugUtilsMessengerEXT"));
    if (createMessenger != nullptr) {
        createMessenger(gpu.instance, &messenger, nullptr, &gpu.messenger);
    }

    std::uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(gpu.instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        skip("no Vulkan physical device (install lavapipe or run on a GPU host)");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(gpu.instance, &deviceCount, devices.data());
    for (const auto candidate : devices) {
        std::uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
        for (std::uint32_t index = 0; index < familyCount; ++index) {
            if ((families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U) {
                gpu.physical = candidate;
                gpu.queueFamily = index;
                break;
            }
        }
        if (gpu.physical != VK_NULL_HANDLE) {
            break;
        }
    }
    if (gpu.physical == VK_NULL_HANDLE) {
        skip("no graphics queue family");
        return false;
    }
    vkGetPhysicalDeviceMemoryProperties(gpu.physical, &gpu.memory);

    const float priority = 1.0F;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = gpu.queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    if (!ok(vkCreateDevice(gpu.physical, &deviceInfo, nullptr, &gpu.device))) {
        skip("vkCreateDevice failed");
        return false;
    }
    vkGetDeviceQueue(gpu.device, gpu.queueFamily, 0, &gpu.queue);
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = gpu.queueFamily;
    return ok(vkCreateCommandPool(gpu.device, &poolInfo, nullptr, &gpu.commandPool));
}

[[nodiscard]] std::optional<VkFormat> pickDepthFormat(const Device& gpu) {
    for (const VkFormat candidate :
         {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT}) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(gpu.physical, candidate, &properties);
        if ((properties.optimalTilingFeatures &
             VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0U &&
            (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0U) {
            return candidate;
        }
    }
    return std::nullopt;
}

} // namespace

int main() {
    Device gpu;
    if (!createDevice(gpu)) {
        return 0;  // 跳过就是成功：本容器没有 GPU 时它不该变成门禁
    }
    const auto depthFormat = pickDepthFormat(gpu);
    if (!depthFormat.has_value()) {
        skip("no sampled depth format");
        return 0;
    }
    constexpr VkFormat kColorFormat = VK_FORMAT_B8G8R8A8_UNORM;

    Scene scene;
    scene.gpu = &gpu;
    bool built = createImage(gpu, *depthFormat,
                             VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                 VK_IMAGE_USAGE_SAMPLED_BIT,
                             VK_IMAGE_ASPECT_DEPTH_BIT, scene.shadowDepth);
    for (std::uint32_t index = 0; index < kImageCount; ++index) {
        built = built && createImage(gpu, kColorFormat,
                                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                     VK_IMAGE_ASPECT_COLOR_BIT, scene.sceneColor[index]);
        built = built && createImage(gpu, *depthFormat,
                                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                     VK_IMAGE_ASPECT_DEPTH_BIT, scene.sceneDepth[index]);
        built = built && createImage(gpu, *depthFormat,
                                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                     VK_IMAGE_ASPECT_DEPTH_BIT, scene.guiDepth[index]);
    }
    built = built && createImage(gpu, kColorFormat, VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT, scene.presentTarget);
    if (!built) {
        skip("offscreen images could not be created");
        return 0;
    }
    scene.shadowPass = createShadowRenderPass(gpu, *depthFormat);
    scene.worldPass = createWorldRenderPass(gpu, kColorFormat, *depthFormat);
    scene.guiPass = createGuiRenderPass(gpu, kColorFormat, *depthFormat);
    if (scene.shadowPass == VK_NULL_HANDLE || scene.worldPass == VK_NULL_HANDLE ||
        scene.guiPass == VK_NULL_HANDLE) {
        skip("render passes could not be created");
        return 0;
    }
    const std::array<VkImageView, 1> shadowAttachment{scene.shadowDepth.view};
    scene.shadowFramebuffer = createFramebuffer(gpu, scene.shadowPass, shadowAttachment);
    for (std::uint32_t index = 0; index < kImageCount; ++index) {
        const std::array<VkImageView, 2> world{scene.sceneColor[index].view,
                                               scene.sceneDepth[index].view};
        const std::array<VkImageView, 2> gui{scene.sceneColor[index].view,
                                             scene.guiDepth[index].view};
        scene.worldFramebuffers[index] = createFramebuffer(gpu, scene.worldPass, world);
        scene.guiFramebuffers[index] = createFramebuffer(gpu, scene.guiPass, gui);
    }
    VkQueryPoolCreateInfo queryInfo{};
    queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    queryInfo.queryType = VK_QUERY_TYPE_OCCLUSION;
    queryInfo.queryCount = kQueryCount;
    if (!ok(vkCreateQueryPool(gpu.device, &queryInfo, nullptr, &scene.queryPool))) {
        skip("query pool could not be created");
        return 0;
    }
    if (!initializeShadowAsShaderRead(gpu, scene)) {
        skip("initial shadow transition failed");
        return 0;
    }

    int failures = 0;
    validationErrors().clear();

    // 1) 阴影启用：世界那步前有一条边界屏障，把阴影图从 DEPTH_ATTACHMENT 带到 SHADER_READ。
    //    它必须落在 renderpass **之外**——这正是 begin/end 归 graph 之后最容易踩的地方。
    {
        BakedGraph graph;
        buildGraph(scene, true, graph, kColorFormat, *depthFormat);
        for (std::uint32_t index = 0; index < kImageCount; ++index) {
            if (!submitOnce(gpu, graph, scene, index)) {
                std::cerr << "FAIL: submit failed (shadow enabled, image " << index << ")\n";
                ++failures;
            }
        }
    }
    // 2) 阴影剪掉：那条屏障必须跟着消失。留着它就是 oldLayout 与实际布局不符，
    //    校验层会报 VUID-VkImageMemoryBarrier-oldLayout-01197。
    {
        BakedGraph graph;
        buildGraph(scene, false, graph, kColorFormat, *depthFormat);
        for (std::uint32_t index = 0; index < kImageCount; ++index) {
            if (!submitOnce(gpu, graph, scene, index)) {
                std::cerr << "FAIL: submit failed (shadow pruned, image " << index << ")\n";
                ++failures;
            }
        }
    }

    if (!validationErrors().empty()) {
        std::cerr << "FAIL: " << validationErrors().size() << " validation message(s):\n";
        for (const auto& message : validationErrors()) {
            std::cerr << "  " << message << '\n';
        }
        ++failures;
    }

    vkDeviceWaitIdle(gpu.device);
    if (failures != 0) {
        return 1;
    }
    std::cout << "frame_graph_device_test ok (validation clean)\n";
    return 0;
}
