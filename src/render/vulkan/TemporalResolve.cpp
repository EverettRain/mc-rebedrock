#include "render/vulkan/TemporalResolve.hpp"

#include <array>
#include <stdexcept>

namespace mc::render {

void TemporalResolve::init(const Config& config) {
    destroy();
    if (config.resources == nullptr || config.device == VK_NULL_HANDLE ||
        config.sceneColorViews.empty()) {
        throw std::runtime_error("TemporalResolve::init needs a device and the scene colour views");
    }
    if (config.inputViews.size() != config.sceneColorViews.size() ||
        config.inputImages.size() != config.sceneColorViews.size() ||
        config.depthImages.size() != config.sceneColorViews.size()) {
        throw std::runtime_error(
            "TemporalResolve::init needs one input, input image and depth image per scene view");
    }
    resources_ = config.resources;
    device_ = config.device;
    extent_ = config.extent;
    depthAspect_ = config.depthAspect;
    inputImages_.assign(config.inputImages.begin(), config.inputImages.end());
    depthImages_.assign(config.depthImages.begin(), config.depthImages.end());
    createHistoryTargets();
    createRenderPass(config);
    createFramebuffers(config);
    createDescriptors(config);
    createPipeline(config.shaderRoot);
    initializeHistory();
}

void TemporalResolve::createHistoryTargets() {
    // 两张，与交换链图像数无关。见头文件里的第 2 条
    constexpr auto count = kHistorySlotCount;
    historyImages_.resize(count);
    historyViews_.resize(count);
    for (std::size_t index = 0; index < count; ++index) {
        historyImages_[index] = resources_->createImage(
            extent_.width, extent_.height, 1U, kHistoryFormat,
            // TRANSFER_DST 是为了 initializeHistory 那一次清零。它不在热路径上，
            // 但没有这一位，第一帧采样的就是一张从未被写过的图像
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        historyViews_[index] = resources_->createImageView(historyImages_[index].image,
                                                           kHistoryFormat,
                                                           VK_IMAGE_ASPECT_COLOR_BIT);
    }
}

void TemporalResolve::createRenderPass(const Config& config) {
    // 0 号：scene_color。四个操作全部照抄帧图对这一步的推导结果
    VkAttachmentDescription scene{};
    scene.format = config.sceneColorFormat;
    scene.samples = VK_SAMPLE_COUNT_1_BIT;
    scene.loadOp = config.sceneLoadOp;
    scene.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    scene.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    scene.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    scene.initialLayout = config.sceneInitialLayout;
    scene.finalLayout = config.sceneFinalLayout;

    // 1 号：历史。整屏覆盖所以不载入；画完停在 SHADER_READ_ONLY，下一帧直接采样它——
    // 布局转换因此由渲染通道自己完成，一条手写屏障都不需要（跨提交的**可见性**
    // 仍然要一条，那是 recordPrepare 的事，不是布局的事）。
    VkAttachmentDescription history{};
    history.format = kHistoryFormat;
    history.samples = VK_SAMPLE_COUNT_1_BIT;
    history.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    history.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    history.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    history.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    history.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    history.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    const std::array attachments{scene, history};
    const std::array references{
        VkAttachmentReference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        VkAttachmentReference{1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = static_cast<std::uint32_t>(references.size());
    subpass.pColorAttachments = references.data();

    // 前置：世界那趟的写、以及上一帧对这张历史的写，都要在本趟采样之前完成。
    // recordPrepare 的屏障已经把这件事说清楚了，这条依赖是渲染通道内部那一半
    VkSubpassDependency before{};
    before.srcSubpass = VK_SUBPASS_EXTERNAL;
    before.dstSubpass = 0;
    before.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                          VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    before.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    before.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    before.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    // 后置：写完的 scene_color 归界面那趟，写完的历史归下一帧
    VkSubpassDependency after{};
    after.srcSubpass = 0;
    after.dstSubpass = VK_SUBPASS_EXTERNAL;
    after.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    after.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    after.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    after.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    const std::array dependencies{before, after};

    auto info = vkStructure<VkRenderPassCreateInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);
    info.attachmentCount = static_cast<std::uint32_t>(attachments.size());
    info.pAttachments = attachments.data();
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = static_cast<std::uint32_t>(dependencies.size());
    info.pDependencies = dependencies.data();
    checkVk(vkCreateRenderPass(device_, &info, nullptr, &renderPass_),
            "vkCreateRenderPass(temporal resolve)");
}

void TemporalResolve::createFramebuffers(const Config& config) {
    // 两维的叉积：交换链图像 × 历史槽。这张表是这个类自己保管的，正因为帧图的
    // BakedStep 只认 imageIndex 一维（见头文件第 3 条）
    framebuffers_.resize(config.sceneColorViews.size() * kHistorySlotCount);
    for (std::size_t index = 0; index < config.sceneColorViews.size(); ++index) {
        for (std::size_t slot = 0; slot < kHistorySlotCount; ++slot) {
            const std::array attachments{config.sceneColorViews[index], historyViews_[slot]};
            auto info =
                vkStructure<VkFramebufferCreateInfo>(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
            info.renderPass = renderPass_;
            info.attachmentCount = static_cast<std::uint32_t>(attachments.size());
            info.pAttachments = attachments.data();
            info.width = extent_.width;
            info.height = extent_.height;
            info.layers = 1;
            checkVk(vkCreateFramebuffer(device_, &info, nullptr,
                                        &framebuffers_[index * kHistorySlotCount + slot]),
                    "vkCreateFramebuffer(temporal resolve)");
        }
    }
}

void TemporalResolve::createDescriptors(const Config& config) {
    // 深度的采样视图必须只有一个 aspect。带 stencil 的深度格式建出来的那份视图
    // （createDepthTargets 用的是推导出的 aspect）有两位，拿它当 sampled image 是非法的
    depthViews_.resize(depthImages_.size());
    for (std::size_t index = 0; index < depthImages_.size(); ++index) {
        depthViews_[index] = resources_->createImageView(depthImages_[index], config.depthFormat,
                                                          VK_IMAGE_ASPECT_DEPTH_BIT);
    }

    // NEAREST 给本帧的颜色与深度：着色器用 texelFetch 读它们，邻域盒要的是整像素。
    // LINEAR 给历史：重投影落在像素之间，最近邻会把亚像素信息量化回整像素，
    // TAA 攒了半天的那点位置精度当场没了
    auto samplerInfo = vkStructure<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    checkVk(vkCreateSampler(device_, &samplerInfo, nullptr, &nearestSampler_),
            "vkCreateSampler(temporal resolve nearest)");
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    checkVk(vkCreateSampler(device_, &samplerInfo, nullptr, &linearSampler_),
            "vkCreateSampler(temporal resolve linear)");

    // 两个集合而不是一个 N×N 的表：本帧那一半按 imageIndex 选，历史那一半按
    // **上一帧**的 imageIndex 选，两个下标是独立的
    const std::array<VkDescriptorSetLayoutBinding, 2> currentBindings{
        VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    auto layoutInfo = vkStructure<VkDescriptorSetLayoutCreateInfo>(
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    layoutInfo.bindingCount = static_cast<std::uint32_t>(currentBindings.size());
    layoutInfo.pBindings = currentBindings.data();
    checkVk(vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &currentLayout_),
            "vkCreateDescriptorSetLayout(temporal resolve current)");
    const VkDescriptorSetLayoutBinding historyBinding{
        0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &historyBinding;
    checkVk(vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &historyLayout_),
            "vkCreateDescriptorSetLayout(temporal resolve history)");

    const auto count = static_cast<std::uint32_t>(config.sceneColorViews.size());
    const auto historyCount = static_cast<std::uint32_t>(historyViews_.size());
    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                        count * 2U + historyCount};
    auto poolInfo =
        vkStructure<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
    poolInfo.maxSets = count + historyCount;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    checkVk(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_),
            "vkCreateDescriptorPool(temporal resolve)");

    const std::vector<VkDescriptorSetLayout> currentLayouts(count, currentLayout_);
    currentSets_.assign(count, VK_NULL_HANDLE);
    auto allocateInfo =
        vkStructure<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
    allocateInfo.descriptorPool = descriptorPool_;
    allocateInfo.descriptorSetCount = count;
    allocateInfo.pSetLayouts = currentLayouts.data();
    checkVk(vkAllocateDescriptorSets(device_, &allocateInfo, currentSets_.data()),
            "vkAllocateDescriptorSets(temporal resolve current)");
    const std::vector<VkDescriptorSetLayout> historyLayouts(historyCount, historyLayout_);
    historySets_.assign(historyCount, VK_NULL_HANDLE);
    allocateInfo.descriptorSetCount = historyCount;
    allocateInfo.pSetLayouts = historyLayouts.data();
    checkVk(vkAllocateDescriptorSets(device_, &allocateInfo, historySets_.data()),
            "vkAllocateDescriptorSets(temporal resolve history)");

    // 每个描述符都要在这里写一次。RN-29 的那条教训：资源建得比集合晚、于是某个
    // 绑定重建之后再也没被写过——采样一个未写入的描述符是未定义行为，而它在真机上
    // 时有时无。三种绑定一次写完，没有"晚点补上"的那一档
    std::vector<VkDescriptorImageInfo> images;
    std::vector<VkWriteDescriptorSet> writes;
    images.reserve(static_cast<std::size_t>(count) * 2U + historyViews_.size());
    const auto pushWrite = [&](VkDescriptorSet set, std::uint32_t binding, VkSampler sampler,
                               VkImageView view, VkImageLayout layout) {
        images.push_back({sampler, view, layout});
        auto write = vkStructure<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        write.dstSet = set;
        write.dstBinding = binding;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes.push_back(write);
    };
    for (std::uint32_t index = 0; index < count; ++index) {
        pushWrite(currentSets_[index], 0U, nearestSampler_, config.inputViews[index],
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        pushWrite(currentSets_[index], 1U, nearestSampler_, depthViews_[index],
                  VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    }
    for (std::size_t index = 0; index < historyViews_.size(); ++index) {
        pushWrite(historySets_[index], 0U, linearSampler_, historyViews_[index],
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    // pImageInfo 要等 images 不再增长才能取地址：vector 一 push 就可能整体搬家，
    // 边填边取地址是一串悬垂指针，而 vkUpdateDescriptorSets 读的正是它们
    for (std::size_t index = 0; index < writes.size(); ++index) {
        writes[index].pImageInfo = &images[index];
    }
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0,
                           nullptr);
}

void TemporalResolve::createPipeline(const std::filesystem::path& shaderRoot) {
    const auto vertexCode = readSpirvFile(shaderRoot / "taa_resolve.vert.spv");
    const auto fragmentCode = readSpirvFile(shaderRoot / "taa_resolve.frag.spv");
    const auto makeModule = [&](const std::vector<std::uint32_t>& code) {
        auto info =
            vkStructure<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
        info.codeSize = code.size() * sizeof(std::uint32_t);
        info.pCode = code.data();
        VkShaderModule module = VK_NULL_HANDLE;
        checkVk(vkCreateShaderModule(device_, &info, nullptr, &module),
                "vkCreateShaderModule(temporal resolve)");
        return module;
    };
    const auto vertexModule = makeModule(vertexCode);
    const auto fragmentModule = makeModule(fragmentCode);

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push.size = sizeof(TemporalPush);
    const std::array setLayouts{currentLayout_, historyLayout_};
    auto layoutInfo =
        vkStructure<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    layoutInfo.setLayoutCount = static_cast<std::uint32_t>(setLayouts.size());
    layoutInfo.pSetLayouts = setLayouts.data();
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
    checkVk(vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &pipelineLayout_),
            "vkCreatePipelineLayout(temporal resolve)");

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
    // TAA 与 MSAA 是同一档设置的两个取值，永远不会同时开——resolve 因此恒为单采样
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    auto depthStencil = vkStructure<VkPipelineDepthStencilStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO);
    // resolve 是**替换**：混合开着会让本帧的结果与附件里的旧内容叠起来，
    // 那看着仍像"有点糊"，而每一帧都在变亮
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.blendEnable = VK_FALSE;
    attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    // 两个颜色附件各要一条混合状态：给一条而声明两个附件是 VUID 违规，
    // 而校验层关掉时它在不同驱动上表现不同
    const std::array blendAttachments{attachment, attachment};
    auto colorBlend = vkStructure<VkPipelineColorBlendStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
    colorBlend.attachmentCount = static_cast<std::uint32_t>(blendAttachments.size());
    colorBlend.pAttachments = blendAttachments.data();
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
    checkVk(result, "vkCreateGraphicsPipelines(temporal resolve)");
}

void TemporalResolve::initializeHistory() const {
    VkCommandBuffer commandBuffer = resources_->beginSingleUseCommands();
    std::vector<VkImageMemoryBarrier> toTransfer(historyImages_.size());
    for (std::size_t index = 0; index < historyImages_.size(); ++index) {
        auto& barrier = toTransfer[index];
        barrier = vkStructure<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = historyImages_[index].image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0U, 1U, 0U, 1U};
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                         static_cast<std::uint32_t>(toTransfer.size()), toTransfer.data());
    const VkClearColorValue black{{0.0F, 0.0F, 0.0F, 1.0F}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0U, 1U, 0U, 1U};
    for (const auto& image : historyImages_) {
        vkCmdClearColorImage(commandBuffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &black, 1, &range);
    }
    std::vector<VkImageMemoryBarrier> toRead = toTransfer;
    for (auto& barrier : toRead) {
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                         static_cast<std::uint32_t>(toRead.size()), toRead.data());
    resources_->endSingleUseCommands(commandBuffer);
}

void TemporalResolve::recordBarriers(VkCommandBuffer commandBuffer, std::uint32_t imageIndex,
                                     std::uint32_t readSlot) const {
    const auto index = static_cast<std::size_t>(imageIndex);
    std::array<VkImageMemoryBarrier, 3> barriers{};
    for (auto& barrier : barriers) {
        barrier = vkStructure<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }
    // 世界那趟画完的颜色。它的 finalLayout 是角色布局（帧图的推导对每一个附件都
    // 给角色布局），转成可采样的那一步在这里——与 shadow_depth 的做法同源
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[0].image = inputImages_[index];
    barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barriers[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    // 深度。重投影读它
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    barriers[1].image = depthImages_[index];
    barriers[1].subresourceRange.aspectMask = depthAspect_;
    barriers[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    // ★ 上一帧写的那张历史。布局两边相同，这条屏障要的不是转换而是**可见性**：
    // 同一队列上的两次提交只保证按顺序开始，不保证上一帧的颜色写在本帧采样之前
    // 已经落到内存里。少了它，画面在高帧率下会随机闪一帧陈旧历史，而且只在真机上
    barriers[2].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[2].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[2].image = historyImages_[readSlot].image;
    barriers[2].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barriers[2].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                         static_cast<std::uint32_t>(barriers.size()), barriers.data());
}

void TemporalResolve::record(VkCommandBuffer commandBuffer, std::uint32_t imageIndex,
                             std::uint32_t writeSlot, const glm::mat4& reprojection,
                             float historyWeight) const {
    if (pipeline_ == VK_NULL_HANDLE) {
        return;
    }
    const std::uint32_t readSlot = writeSlot ^ 1U;
    recordBarriers(commandBuffer, imageIndex, readSlot);

    auto beginInfo = vkStructure<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
    beginInfo.renderPass = renderPass_;
    beginInfo.framebuffer =
        framebuffers_[static_cast<std::size_t>(imageIndex) * kHistorySlotCount + writeSlot];
    beginInfo.renderArea.extent = extent_;
    // pClearValues 按**附件号**索引：0 号的 loadOp 是推导给的（今天是 CLEAR），
    // 1 号是 DONT_CARE 但仍要占位
    const std::array<VkClearValue, 2> clears{};
    beginInfo.clearValueCount = static_cast<std::uint32_t>(clears.size());
    beginInfo.pClearValues = clears.data();
    vkCmdBeginRenderPass(commandBuffer, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.width = static_cast<float>(extent_.width);
    viewport.height = static_cast<float>(extent_.height);
    viewport.maxDepth = 1.0F;
    const VkRect2D scissor{{0, 0}, extent_};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    const std::array sets{currentSets_[imageIndex], historySets_[readSlot]};
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0,
                            static_cast<std::uint32_t>(sets.size()), sets.data(), 0, nullptr);
    const TemporalPush push{reprojection, historyWeight, 0.0F, 0.0F, 0.0F};
    vkCmdPushConstants(commandBuffer, pipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    vkCmdEndRenderPass(commandBuffer);
}

void TemporalResolve::destroy() {
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
    currentSets_.clear();
    historySets_.clear();
    for (auto* layout : {&currentLayout_, &historyLayout_}) {
        if (*layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_, *layout, nullptr);
            *layout = VK_NULL_HANDLE;
        }
    }
    for (auto* sampler : {&nearestSampler_, &linearSampler_}) {
        if (*sampler != VK_NULL_HANDLE) {
            vkDestroySampler(device_, *sampler, nullptr);
            *sampler = VK_NULL_HANDLE;
        }
    }
    for (auto framebuffer : framebuffers_) {
        vkDestroyFramebuffer(device_, framebuffer, nullptr);
    }
    framebuffers_.clear();
    for (auto view : depthViews_) {
        vkDestroyImageView(device_, view, nullptr);
    }
    depthViews_.clear();
    for (auto view : historyViews_) {
        vkDestroyImageView(device_, view, nullptr);
    }
    historyViews_.clear();
    if (resources_ != nullptr) {
        for (auto& image : historyImages_) {
            resources_->destroyImage(image);
        }
    }
    historyImages_.clear();
    if (renderPass_ != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device_, renderPass_, nullptr);
        renderPass_ = VK_NULL_HANDLE;
    }
    inputImages_.clear();
    depthImages_.clear();
    resources_ = nullptr;
    device_ = VK_NULL_HANDLE;
}

} // namespace mc::render
