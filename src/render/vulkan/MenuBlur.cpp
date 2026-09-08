#include "render/vulkan/MenuBlur.hpp"

#include <array>
#include <fstream>
#include <stdexcept>

namespace mc::render {
namespace {

[[nodiscard]] std::vector<std::uint32_t> readSpirvFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file) {
        throw std::runtime_error("Unable to open shader: " + path.string());
    }
    const auto end = file.tellg();
    if (end <= 0 || static_cast<std::uint64_t>(end) % sizeof(std::uint32_t) != 0U) {
        throw std::runtime_error("Invalid SPIR-V file: " + path.string());
    }
    const auto byteCount = static_cast<std::size_t>(end);
    std::vector<std::uint32_t> code(byteCount / sizeof(std::uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(byteCount));
    if (!file) {
        throw std::runtime_error("Unable to read shader: " + path.string());
    }
    return code;
}

} // namespace

void MenuBlur::init(const Config& config) {
    destroy();
    if (config.resources == nullptr || config.device == VK_NULL_HANDLE ||
        config.sceneColorViews.empty()) {
        throw std::runtime_error("MenuBlur::init needs a device and the scene colour views");
    }
    resources_ = config.resources;
    device_ = config.device;
    extent_ = config.extent;
    colorFormat_ = config.colorFormat;
    sceneColorPassLayout_ = config.sceneColorPassLayout;
    sceneColorViews_.assign(config.sceneColorViews.begin(), config.sceneColorViews.end());
    sceneColorImages_.assign(config.sceneColorImages.begin(), config.sceneColorImages.end());
    backgroundRenderPass_ = config.backgroundRenderPass;
    backgroundFramebuffers_.assign(config.backgroundFramebuffers.begin(),
                                   config.backgroundFramebuffers.end());
    if (sceneColorImages_.size() != sceneColorViews_.size()) {
        throw std::runtime_error("MenuBlur::init needs one scene colour image per view");
    }
    createRenderPass();
    createTargets();
    createDescriptors();
    createPipeline(config.shaderRoot);
}

void MenuBlur::createRenderPass() {
    // 只有颜色附件——模糊不读也不写深度。这一趟**不必**与界面那趟兼容：
    // 它跑在自己的管线上，而管线是按这个渲染通道建的。
    VkAttachmentDescription color{};
    color.format = colorFormat_;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    // 输出被整屏覆盖，载入旧内容纯属浪费带宽；也正因如此 initialLayout 可以是
    // UNDEFINED——ping-pong 的两张靶每一趟都是全量覆盖。
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // 画完立刻被下一趟采样，所以就停在 SHADER_READ_ONLY——六趟之间因此一条
    // 手写屏障都不需要，布局转换由渲染通道自己完成。
    color.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference colorReference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorReference;
    // 前一趟正在采样这张靶（写后读的反向：读后写），要等它读完再覆盖
    VkSubpassDependency before{};
    before.srcSubpass = VK_SUBPASS_EXTERNAL;
    before.dstSubpass = 0;
    before.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    before.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    before.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    before.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    // 本趟写完要被下一趟采样
    VkSubpassDependency after{};
    after.srcSubpass = 0;
    after.dstSubpass = VK_SUBPASS_EXTERNAL;
    after.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    after.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    after.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    after.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    const std::array dependencies{before, after};
    auto info = vkStructure<VkRenderPassCreateInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);
    info.attachmentCount = 1;
    info.pAttachments = &color;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = static_cast<std::uint32_t>(dependencies.size());
    info.pDependencies = dependencies.data();
    checkVk(vkCreateRenderPass(device_, &info, nullptr, &renderPass_),
            "vkCreateRenderPass(menu blur)");
}

void MenuBlur::createTargets() {
    const auto count = sceneColorViews_.size();
    swapImages_.resize(count);
    swapViews_.resize(count);
    sceneFramebuffers_.resize(count);
    swapFramebuffers_.resize(count);
    for (std::size_t index = 0; index < count; ++index) {
        // 与 scene_color 同格式同尺寸：ping-pong 的两张靶必须可以互换角色，
        // 否则横向那趟与纵向那趟就不是同一个卷积。
        swapImages_[index] = resources_->createImage(
            extent_.width, extent_.height, 1U, colorFormat_,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        swapViews_[index] = resources_->createImageView(swapImages_[index].image, colorFormat_,
                                                        VK_IMAGE_ASPECT_COLOR_BIT);
        const auto makeFramebuffer = [&](VkImageView view) {
            auto info =
                vkStructure<VkFramebufferCreateInfo>(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
            info.renderPass = renderPass_;
            info.attachmentCount = 1;
            info.pAttachments = &view;
            info.width = extent_.width;
            info.height = extent_.height;
            info.layers = 1;
            VkFramebuffer framebuffer = VK_NULL_HANDLE;
            checkVk(vkCreateFramebuffer(device_, &info, nullptr, &framebuffer),
                    "vkCreateFramebuffer(menu blur)");
            return framebuffer;
        };
        sceneFramebuffers_[index] = makeFramebuffer(sceneColorViews_[index]);
        swapFramebuffers_[index] = makeFramebuffer(swapViews_[index]);
    }
}

void MenuBlur::createDescriptors() {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    auto layoutInfo =
        vkStructure<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    checkVk(vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &setLayout_),
            "vkCreateDescriptorSetLayout(menu blur)");

    // ★ LINEAR。box_blur.fsh 以 2 为步长在**像素之间**采样，靠双线性一次拿两个像素的
    // 平均把采样次数减半（它自己的注释就是这么写的）。换成 NEAREST，同样的循环会
    // 每隔一个像素漏掉一个，结果是竖条纹而不是模糊。
    auto samplerInfo = vkStructure<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    // 采样点会越过边缘（±半径），CLAMP_TO_EDGE 让边缘像素向外延伸，
    // 这与 vanilla 的行为一致；REPEAT 会把对侧的画面卷进来，屏幕四边出现鬼影。
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    checkVk(vkCreateSampler(device_, &samplerInfo, nullptr, &sampler_),
            "vkCreateSampler(menu blur)");

    const auto count = static_cast<std::uint32_t>(sceneColorViews_.size());
    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, count * 2U};
    auto poolInfo =
        vkStructure<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
    poolInfo.maxSets = count * 2U;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    checkVk(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_),
            "vkCreateDescriptorPool(menu blur)");

    const std::vector<VkDescriptorSetLayout> layouts(count * 2U, setLayout_);
    std::vector<VkDescriptorSet> sets(count * 2U, VK_NULL_HANDLE);
    auto allocateInfo =
        vkStructure<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
    allocateInfo.descriptorPool = descriptorPool_;
    allocateInfo.descriptorSetCount = count * 2U;
    allocateInfo.pSetLayouts = layouts.data();
    checkVk(vkAllocateDescriptorSets(device_, &allocateInfo, sets.data()),
            "vkAllocateDescriptorSets(menu blur)");
    sceneSets_.assign(sets.begin(), sets.begin() + count);
    swapSets_.assign(sets.begin() + count, sets.end());

    std::vector<VkDescriptorImageInfo> images(sets.size());
    std::vector<VkWriteDescriptorSet> writes(sets.size());
    for (std::size_t index = 0; index < sets.size(); ++index) {
        const bool scene = index < sceneColorViews_.size();
        images[index].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        images[index].imageView =
            scene ? sceneColorViews_[index] : swapViews_[index - sceneColorViews_.size()];
        images[index].sampler = sampler_;
        writes[index] = vkStructure<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        writes[index].dstSet = sets[index];
        writes[index].dstBinding = 0;
        writes[index].descriptorCount = 1;
        writes[index].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[index].pImageInfo = &images[index];
    }
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0,
                           nullptr);
}

void MenuBlur::createPipeline(const std::filesystem::path& shaderRoot) {
    const auto vertexCode = readSpirvFile(shaderRoot / "menu_blur.vert.spv");
    const auto fragmentCode = readSpirvFile(shaderRoot / "menu_blur.frag.spv");
    const auto makeModule = [&](const std::vector<std::uint32_t>& code) {
        auto info =
            vkStructure<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
        info.codeSize = code.size() * sizeof(std::uint32_t);
        info.pCode = code.data();
        VkShaderModule module = VK_NULL_HANDLE;
        checkVk(vkCreateShaderModule(device_, &info, nullptr, &module),
                "vkCreateShaderModule(menu blur)");
        return module;
    };
    const auto vertexModule = makeModule(vertexCode);
    const auto fragmentModule = makeModule(fragmentCode);

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push.size = sizeof(BlurPush);
    auto layoutInfo =
        vkStructure<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
    checkVk(vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &pipelineLayout_),
            "vkCreatePipelineLayout(menu blur)");

    auto vertexStage = vkStructure<VkPipelineShaderStageCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
    vertexStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertexStage.module = vertexModule;
    vertexStage.pName = "main";
    auto fragmentStage = vertexStage;
    fragmentStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragmentStage.module = fragmentModule;
    const std::array stages{vertexStage, fragmentStage};

    auto vertexInput = vkStructure<VkPipelineVertexInputStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
    auto inputAssembly = vkStructure<VkPipelineInputAssemblyStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    auto viewportState = vkStructure<VkPipelineViewportStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;
    auto rasterization = vkStructure<VkPipelineRasterizationStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0F;
    auto multisampling = vkStructure<VkPipelineMultisampleStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    auto depthStencil = vkStructure<VkPipelineDepthStencilStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO);
    VkPipelineColorBlendAttachmentState colorAttachment{};
    // 模糊是**替换**，不是叠加：混合开着会把上一趟的结果与这一趟叠在一起，
    // 那看起来仍然像"糊了"，只是每一趟都在变亮。
    colorAttachment.blendEnable = VK_FALSE;
    colorAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    auto colorBlend = vkStructure<VkPipelineColorBlendStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &colorAttachment;
    const std::array dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    auto dynamicState = vkStructure<VkPipelineDynamicStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
    dynamicState.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    auto info =
        vkStructure<VkGraphicsPipelineCreateInfo>(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
    info.stageCount = static_cast<std::uint32_t>(stages.size());
    info.pStages = stages.data();
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &inputAssembly;
    info.pViewportState = &viewportState;
    info.pRasterizationState = &rasterization;
    info.pMultisampleState = &multisampling;
    info.pDepthStencilState = &depthStencil;
    info.pColorBlendState = &colorBlend;
    info.pDynamicState = &dynamicState;
    info.layout = pipelineLayout_;
    info.renderPass = renderPass_;
    const auto result =
        vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_);
    // 新管线必须销毁着色器模块——已登记的坑，忘了就是每次交换链重建泄一对
    vkDestroyShaderModule(device_, fragmentModule, nullptr);
    vkDestroyShaderModule(device_, vertexModule, nullptr);
    checkVk(result, "vkCreateGraphicsPipelines(menu blur)");
}

void MenuBlur::destroy() {
    if (device_ == VK_NULL_HANDLE) {
        resources_ = nullptr;
        return;
    }
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (pipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        pipelineLayout_ = VK_NULL_HANDLE;
    }
    if (descriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        descriptorPool_ = VK_NULL_HANDLE;
    }
    sceneSets_.clear();
    swapSets_.clear();
    if (setLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
        setLayout_ = VK_NULL_HANDLE;
    }
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, sampler_, nullptr);
        sampler_ = VK_NULL_HANDLE;
    }
    for (auto framebuffer : sceneFramebuffers_) {
        vkDestroyFramebuffer(device_, framebuffer, nullptr);
    }
    sceneFramebuffers_.clear();
    for (auto framebuffer : swapFramebuffers_) {
        vkDestroyFramebuffer(device_, framebuffer, nullptr);
    }
    swapFramebuffers_.clear();
    for (auto view : swapViews_) {
        vkDestroyImageView(device_, view, nullptr);
    }
    swapViews_.clear();
    if (resources_ != nullptr) {
        for (auto& image : swapImages_) {
            resources_->destroyImage(image);
        }
    }
    swapImages_.clear();
    if (renderPass_ != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device_, renderPass_, nullptr);
        renderPass_ = VK_NULL_HANDLE;
    }
    sceneColorViews_.clear();
    sceneColorImages_.clear();
    backgroundFramebuffers_.clear();
    backgroundRenderPass_ = VK_NULL_HANDLE;
    resources_ = nullptr;
    device_ = VK_NULL_HANDLE;
}

void MenuBlur::beginBackgroundPass(VkCommandBuffer commandBuffer,
                                   std::uint32_t imageIndex) const {
    auto info = vkStructure<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
    info.renderPass = backgroundRenderPass_;
    info.framebuffer = backgroundFramebuffers_[imageIndex];
    info.renderArea.extent = extent_;
    // 这一趟与界面那趟共用同一份渲染通道描述，深度附件因此也是 LOAD_OP_CLEAR：
    // pClearValues 按**附件号**索引，所以即使颜色那格不清也必须占位，给够两项。
    // 少给一项是 VUID-VkRenderPassBeginInfo-clearValueCount-00902。
    std::array<VkClearValue, 2> clears{};
    clears[1].depthStencil = {1.0F, 0};
    info.clearValueCount = static_cast<std::uint32_t>(clears.size());
    info.pClearValues = clears.data();
    vkCmdBeginRenderPass(commandBuffer, &info, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport{};
    viewport.width = static_cast<float>(extent_.width);
    viewport.height = static_cast<float>(extent_.height);
    viewport.maxDepth = 1.0F;
    const VkRect2D scissor{{0, 0}, extent_};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
}

void MenuBlur::endBackgroundPass(VkCommandBuffer commandBuffer) const {
    vkCmdEndRenderPass(commandBuffer);
}

void MenuBlur::transition(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout from,
                          VkImageLayout to, VkPipelineStageFlags sourceStage,
                          VkPipelineStageFlags destinationStage, VkAccessFlags sourceAccess,
                          VkAccessFlags destinationAccess) const {
    auto barrier = vkStructure<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = sourceAccess;
    barrier.dstAccessMask = destinationAccess;
    vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);
}

void MenuBlur::runPass(VkCommandBuffer commandBuffer, VkFramebuffer target, VkDescriptorSet source,
                       float dirX, float dirY, float radius) const {
    auto info = vkStructure<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
    info.renderPass = renderPass_;
    info.framebuffer = target;
    info.renderArea.extent = extent_;
    vkCmdBeginRenderPass(commandBuffer, &info, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport{};
    viewport.width = static_cast<float>(extent_.width);
    viewport.height = static_cast<float>(extent_.height);
    viewport.maxDepth = 1.0F;
    const VkRect2D scissor{{0, 0}, extent_};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                            &source, 0, nullptr);
    const BlurPush push{dirX, dirY, radius, 0.0F};
    vkCmdPushConstants(commandBuffer, pipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    vkCmdEndRenderPass(commandBuffer);
}

void MenuBlur::record(VkCommandBuffer commandBuffer, std::uint32_t imageIndex, int radius) const {
    if (radius <= 0 || pipeline_ == VK_NULL_HANDLE) {
        return;
    }
    const auto index = static_cast<std::size_t>(imageIndex);
    const auto sceneImage = sceneColorImages_[index];
    // 世界那趟（或全景那趟）把 scene_color 留在界面那趟期望的布局上。第一趟要采样它，
    // 所以先转成 SHADER_READ_ONLY；六趟之后再原样还回去——不还回去，界面那趟的
    // initialLayout 就对不上，而那是只有校验层会说话的错误。
    transition(commandBuffer, sceneImage, sceneColorPassLayout_,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    const auto amount = static_cast<float>(radius);
    // 趟数与方向排布来自 ui::kMenuBlurPassCount / menuBlurPassDirection（blur.json 的
    // 三对横竖）。写在那边而不是这里，是因为"六趟"和"横竖交替"退回去都不会崩，
    // 在这个翻译单元里没有任何测试看得见——那正是 screen_background_test 钉住它们的理由。
    //
    // 偶数趟从 scene_color 出发回到 scene_color：偶数趟读 scene 写 swap，奇数趟反过来。
    for (int pass = 0; pass < ui::kMenuBlurPassCount; ++pass) {
        const auto direction = ui::menuBlurPassDirection(pass);
        const bool readScene = pass % 2 == 0;
        runPass(commandBuffer, readScene ? swapFramebuffers_[index] : sceneFramebuffers_[index],
                readScene ? sceneSets_[index] : swapSets_[index], direction.x, direction.y, amount);
    }
    transition(commandBuffer, sceneImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               sceneColorPassLayout_, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_SHADER_READ_BIT,
               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
}

} // namespace mc::render
