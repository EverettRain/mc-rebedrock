#include "render/graph/FrameGraph.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace mc::render::graph {
namespace {

[[noreturn]] void fail(std::string_view what, std::string_view name) {
    throw std::runtime_error("frame graph: " + std::string{what} + " (" + std::string{name} + ")");
}

// 写者 = 内容由这一步产生。ColorResolve 也在内：resolve 目标的像素确实是这一步写的
// （它只是不被载入，见下面 loadOp 的规则）。
[[nodiscard]] bool writes(Access access) noexcept {
    return access == Access::ColorWrite || access == Access::DepthWrite ||
           access == Access::ColorResolve;
}

[[nodiscard]] bool reads(Access access) noexcept {
    return access == Access::Sample || access == Access::DepthReadOnly ||
           access == Access::TransferRead;
}

// 附件 = 进 renderpass 的 pAttachments。读者里只有 DepthReadOnly 是附件；
// Sample 是描述符采样、TransferRead 是 vkCmdCopyImage，两者都不在 renderpass 里。
[[nodiscard]] bool isAttachment(Access access) noexcept {
    return access != Access::Sample && access != Access::TransferRead;
}

// graph 层不认 render/vulkan，所以格式判断在这里重来一份，取值与
// VulkanResources::depthFormatHasStencil 相同。aspect 必须跟着格式走：
// 写死 DEPTH_BIT 在带 stencil 的深度格式上是错的。
[[nodiscard]] bool formatHasStencil(VkFormat format) noexcept {
    return format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT ||
           format == VK_FORMAT_D16_UNORM_S8_UINT || format == VK_FORMAT_S8_UINT;
}

// 附件在它自己那趟里的布局。Sample 读者要的 SHADER_READ_ONLY **不**从这里来——
// 那条转换走 PassDesc::barriers 的边界屏障（RN-20a 已确立），renderpass 的
// finalLayout 保持角色布局。阴影那张图就是这样：shadow renderpass 的 finalLayout 是
// DEPTH_STENCIL_ATTACHMENT_OPTIMAL，世界那步的边界屏障再把它转成 SHADER_READ_ONLY。
[[nodiscard]] VkImageLayout roleLayout(ResourceKind kind) noexcept {
    return kind == ResourceKind::Depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                       : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}

[[nodiscard]] std::uint32_t sizeAsU32(std::size_t value) {
    return static_cast<std::uint32_t>(value);
}

} // namespace

const PlannedResource& ResourcePlan::resource(std::string_view name) const {
    for (const PlannedResource& entry : resources_) {
        if (entry.name == name) {
            return entry;
        }
    }
    fail("resource plan has no such resource", name);
}

bool ResourcePlan::has(std::string_view name) const noexcept {
    return std::any_of(resources_.begin(), resources_.end(),
                       [name](const PlannedResource& entry) { return entry.name == name; });
}

const ResourceOps& ResourcePlan::ops(std::string_view pass, std::string_view resource) const {
    for (const ResourceOps& entry : ops_) {
        if (entry.pass == pass && entry.resource == resource) {
            return entry;
        }
    }
    fail("resource plan has no attachment ops for this pass", resource);
}

// ---------------------------------------------------------------------------
// 推导规则（RN-20c）
// ---------------------------------------------------------------------------
//
// 对每个资源，扫它在所有**启用的** pass 里的 Access：
//
//   写者 = ColorWrite | DepthWrite | ColorResolve
//   读者 = Sample | DepthReadOnly | TransferRead
//
//   usage = 基础位（Color→COLOR_ATTACHMENT，Depth→DEPTH_STENCIL_ATTACHMENT，
//                   Swapchain→COLOR_ATTACHMENT|TRANSFER_DST）
//         | 有 Sample / DepthReadOnly 读者 ? SAMPLED       : 0
//         | 有 TransferRead 读者          ? TRANSFER_SRC   : 0
//         | 有写者且最后一次写之后没有读者  ? TRANSIENT      : 0
//
// 「有写者」那一项不是修饰，是规则的一部分：**图内没有写者的资源永远不是瞬态**。
// 它的内容来自图外（`shadow_depth` 在 shadow 被剪掉时正是如此，见 compile() 里
// 「一个资源若在本图内没有任何启用的写者」那段——两处是同一条判断）。
// 少了这一条，关掉太阳阴影就会把一张仍被 binding 8 无条件采样的图判成瞬态。
//
// 逐 (pass, 资源) 的附件操作：
//
//   loadOp   = ColorResolve                ? DONT_CARE
//            : 该资源在本 pass 之前有写者   ? LOAD
//            :                              CLEAR
//   storeOp  = 本 pass 之后还有读者或写者   ? STORE : DONT_CARE
//   initial  = loadOp == LOAD ? 角色布局 : UNDEFINED
//   final    = 本 pass 之后的**第一个**消费者是 TransferRead ? TRANSFER_SRC_OPTIMAL
//                                                          : 角色布局
//
// storeOp 按「之后还有读者**或写者**」判，而不是任务书那条「最后一次写之后有读者」：
// 后者是逐资源的，推不出「world 那趟必须 STORE 好让 gui 那趟 LOAD 它」。今天两条在
// 五个资源上给出相同答案，但只有前者对多次写的资源成立。
ResourcePlan planResources(const GraphDesc& desc) {
    for (std::size_t index = 0; index < desc.views.size(); ++index) {
        if (desc.views[index].resource >= desc.resources.size()) {
            fail("view refers to a resource slot that does not exist", std::to_string(index));
        }
    }

    const std::size_t resourceCount = desc.resources.size();
    const std::size_t passCount = desc.passes.size();

    // 逐资源的访问时间线。用 pass 下标而不是指针：剪掉的步不进表，于是「启用的」
    // 这条限定是结构性的，不是每个消费点各判一次 enabled。
    struct Touch final {
        std::size_t pass = 0;
        Access access = Access::ColorWrite;
    };
    std::vector<std::vector<Touch>> timeline(resourceCount);
    for (std::size_t index = 0; index < passCount; ++index) {
        const PassDesc& pass = desc.passes[index];
        if (!pass.enabled) {
            continue;
        }
        bool resolveSeen = false;
        for (const PassAttachment& attachment : pass.attachments) {
            if (attachment.view >= desc.views.size()) {
                fail("pass refers to a view slot that does not exist", pass.name);
            }
            const std::uint16_t resource = desc.views[attachment.view].resource;
            // 同一 pass 里同一个 view 既是 ColorWrite 又是 ColorResolve 是无意义的：
            // 那意味着一张图像同时是多采样靶与它自己的 resolve 目标。MSAA 关时
            // sceneColorMsaa 塌回 scene_color，正是这条最容易被踩出来的形态。
            for (const Touch& seen : timeline[resource]) {
                if (seen.pass != index) {
                    continue;
                }
                const bool clash = (seen.access == Access::ColorWrite &&
                                    attachment.access == Access::ColorResolve) ||
                                   (seen.access == Access::ColorResolve &&
                                    attachment.access == Access::ColorWrite);
                if (clash) {
                    fail("a view is both the color attachment and its own resolve target",
                         pass.name);
                }
            }
            if (attachment.access == Access::ColorResolve) {
                if (resolveSeen) {
                    fail("a pass declares more than one resolve target", pass.name);
                }
                resolveSeen = true;
            }
            timeline[resource].push_back({index, attachment.access});
        }
    }

    ResourcePlan plan;
    for (std::size_t index = 0; index < resourceCount; ++index) {
        const ResourceDesc& resource = desc.resources[index];
        const std::vector<Touch>& touches = timeline[index];

        bool hasWriter = false;
        bool sampled = false;
        bool transferRead = false;
        std::size_t lastWrite = 0;
        bool readerAfterLastWrite = false;
        for (const Touch& touch : touches) {
            if (writes(touch.access)) {
                hasWriter = true;
                lastWrite = touch.pass;
            }
            sampled = sampled || touch.access == Access::Sample ||
                      touch.access == Access::DepthReadOnly;
            transferRead = transferRead || touch.access == Access::TransferRead;
        }
        for (const Touch& touch : touches) {
            if (reads(touch.access) && touch.pass >= lastWrite) {
                readerAfterLastWrite = true;
            }
        }

        VkImageUsageFlags usage = 0;
        switch (resource.kind) {
        case ResourceKind::Depth:
            usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            break;
        case ResourceKind::Swapchain:
            // 交换链图像不是我们创建的，推导只描述它被怎么用：帧末 vkCmdCopyImage 的
            // 目标。它永远不是瞬态——呈现引擎持有它。
            usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            break;
        case ResourceKind::Color:
            usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            break;
        }
        if (sampled) {
            usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
        }
        if (transferRead) {
            usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        }
        if (hasWriter && !readerAfterLastWrite && resource.kind != ResourceKind::Swapchain) {
            usage |= VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
        }
        // 瞬态附件在 Vulkan 里只能当附件用（Apple 上是 memoryless）。推导若同时给出
        // 瞬态与「可采样 / 可拷贝」，那是规则写错了，不是一次可以将就的分配。
        if ((usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) != 0U &&
            (usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) != 0U) {
            fail("a transient attachment cannot also be sampled or copied", resource.name);
        }

        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        if (resource.kind == ResourceKind::Depth) {
            aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
            if (formatHasStencil(resource.format)) {
                aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
            }
        }

        plan.add(PlannedResource{.name = resource.name,
                                 .kind = resource.kind,
                                 .format = resource.format,
                                 .width = resource.width,
                                 .height = resource.height,
                                 .samples = resource.samples,
                                 .perSwapchainImage = resource.perSwapchainImage,
                                 .usage = usage,
                                 .aspect = aspect});

        for (const Touch& touch : touches) {
            if (!isAttachment(touch.access)) {
                continue;
            }
            bool writerBefore = false;
            bool anyAfter = false;
            bool firstConsumerIsTransferRead = false;
            std::size_t firstConsumer = passCount;
            for (const Touch& other : touches) {
                if (other.pass < touch.pass && writes(other.access)) {
                    writerBefore = true;
                }
                if (other.pass > touch.pass) {
                    anyAfter = true;
                    if (other.pass < firstConsumer) {
                        firstConsumer = other.pass;
                        firstConsumerIsTransferRead = other.access == Access::TransferRead;
                    }
                }
            }

            ResourceOps ops{};
            ops.pass = desc.passes[touch.pass].name;
            ops.resource = resource.name;
            ops.access = touch.access;
            ops.loadOp = touch.access == Access::ColorResolve ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                         : writerBefore                       ? VK_ATTACHMENT_LOAD_OP_LOAD
                                                              : VK_ATTACHMENT_LOAD_OP_CLEAR;
            ops.storeOp =
                anyAfter ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
            ops.initialLayout = ops.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD
                                    ? roleLayout(resource.kind)
                                    : VK_IMAGE_LAYOUT_UNDEFINED;
            ops.finalLayout = firstConsumerIsTransferRead ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                                          : roleLayout(resource.kind);
            plan.add(ops);
        }
    }
    return plan;
}

void BakedGraph::reset() noexcept {
    steps_.clear();
    stepNames_.clear();
    bodies_.clear();
    barrierPool_.clear();
    framebufferPool_.clear();
    clearPool_.clear();
}

void BakedGraph::compile(const GraphDesc& desc, const ResourcePlan& plan) {
    reset();

    // ---- 计划与描述必须同源 --------------------------------------------------
    //
    // 资源是按 plan 创建的，所以「plan 与 desc 一致」等价于「创建参数与 desc 一致」。
    // 不核对就回到了 RN-20c 要消灭的那个形态：计划说 A、创建写 B，而没有任何东西
    // 比对这两者。逐字段比，因为格式或采样数对不上会在真机上表现成 framebuffer
    // 创建失败或静默的画面错误，不是一条能推迟的告警。
    if (plan.resources().size() != desc.resources.size()) {
        fail("the resource plan was made from a different graph", "resource count");
    }
    for (std::size_t index = 0; index < desc.resources.size(); ++index) {
        const ResourceDesc& described = desc.resources[index];
        const PlannedResource& planned = plan.resources()[index];
        if (planned.name != described.name || planned.kind != described.kind ||
            planned.format != described.format || planned.width != described.width ||
            planned.height != described.height || planned.samples != described.samples ||
            planned.perSwapchainImage != described.perSwapchainImage) {
            fail("the resource plan does not match the graph it is compiled with", described.name);
        }
    }

    // ---- 表内引用与命名 ------------------------------------------------------
    for (std::size_t index = 0; index < desc.views.size(); ++index) {
        if (desc.views[index].resource >= desc.resources.size()) {
            fail("view refers to a resource slot that does not exist",
                 std::to_string(index));
        }
    }
    for (std::size_t first = 0; first < desc.resources.size(); ++first) {
        for (std::size_t second = first + 1; second < desc.resources.size(); ++second) {
            if (desc.resources[first].name == desc.resources[second].name) {
                fail("duplicate resource name", desc.resources[first].name);
            }
        }
    }
    for (std::size_t first = 0; first < desc.passes.size(); ++first) {
        for (std::size_t second = first + 1; second < desc.passes.size(); ++second) {
            if (desc.passes[first].name == desc.passes[second].name) {
                fail("duplicate pass name", desc.passes[first].name);
            }
        }
    }

    // ---- 声明序即拓扑序：只校验「写者在读者之前」，不重排 --------------------
    //
    // 一个资源若在本图内**没有**任何启用的写者，它的读者不受约束——那是图外来源
    // （shadow_depth 在 shadow 被剪掉时就是这样：内容来自上一帧，布局由
    // OffscreenTarget::initializeAsShaderRead 保证）。有写者才要求读在写后。
    // ⚠ planResources() 的「无写者永不瞬态」是**同一条判断**：图外来源的内容必须
    // 活过整帧。改这里就要一起改那里。
    std::vector<std::size_t> firstWrite(desc.resources.size(), desc.passes.size());
    for (std::size_t index = 0; index < desc.passes.size(); ++index) {
        const PassDesc& pass = desc.passes[index];
        if (!pass.enabled) {
            continue;
        }
        for (const PassAttachment& attachment : pass.attachments) {
            if (attachment.view >= desc.views.size()) {
                fail("pass refers to a view slot that does not exist", pass.name);
            }
            if (!writes(attachment.access)) {
                continue;
            }
            const std::uint16_t resource = desc.views[attachment.view].resource;
            firstWrite[resource] = std::min(firstWrite[resource], index);
        }
    }
    for (std::size_t index = 0; index < desc.passes.size(); ++index) {
        const PassDesc& pass = desc.passes[index];
        if (!pass.enabled) {
            continue;
        }
        for (const PassAttachment& attachment : pass.attachments) {
            if (!reads(attachment.access)) {
                continue;
            }
            const std::uint16_t resource = desc.views[attachment.view].resource;
            if (firstWrite[resource] < desc.passes.size() && firstWrite[resource] > index) {
                fail("pass samples a resource declared before its writer", pass.name);
            }
        }
    }

    // ---- 剪枝 + 扁平化 -------------------------------------------------------
    std::size_t lastRenderStep = desc.passes.size();
    std::size_t lockedStep = desc.passes.size();
    for (std::size_t index = 0; index < desc.passes.size(); ++index) {
        const PassDesc& pass = desc.passes[index];
        if (!pass.enabled) {
            continue;
        }
        if (!pass.record) {
            fail("pass has no record body", pass.name);
        }
        const bool renderStep = pass.renderPass != VK_NULL_HANDLE;
        if (renderStep) {
            if (pass.framebuffers.empty()) {
                fail("render step has no framebuffer", pass.name);
            }
            if (pass.extent.width == 0U || pass.extent.height == 0U) {
                fail("render step has an empty render area", pass.name);
            }
        } else {
            // 非渲染步的身份就是它的保护：上传拷贝与遮挡查询池 reset 必须留在任何
            // renderpass 之外，vkCmdResetQueryPool 在 renderpass 内是非法的
            if (!pass.framebuffers.empty() || !pass.clears.empty()) {
                fail("non-render step carries a framebuffer or clear value", pass.name);
            }
        }
        if (!pass.barriers.empty() &&
            (pass.barrierSrcStage == 0U || pass.barrierDstStage == 0U)) {
            fail("barrier batch has no pipeline stages", pass.name);
        }

        // stride 从资源表推，不是从 span 长度猜：交换链只有一张图像时两者无法区分
        bool perSwapchainImage = false;
        for (const PassAttachment& attachment : pass.attachments) {
            if (desc.resources[desc.views[attachment.view].resource].perSwapchainImage) {
                perSwapchainImage = true;
            }
        }
        if (renderStep && !perSwapchainImage && pass.framebuffers.size() != 1U) {
            fail("single-instance render step has more than one framebuffer", pass.name);
        }

        BakedStep step{};
        step.barrierFirst = sizeAsU32(barrierPool_.size());
        step.barrierCount = sizeAsU32(pass.barriers.size());
        step.barrierSrcStage = pass.barrierSrcStage;
        step.barrierDstStage = pass.barrierDstStage;
        barrierPool_.insert(barrierPool_.end(), pass.barriers.begin(), pass.barriers.end());
        step.renderPass = pass.renderPass;
        step.framebufferFirst = sizeAsU32(framebufferPool_.size());
        step.framebufferStride = renderStep && perSwapchainImage ? 1U : 0U;
        framebufferPool_.insert(framebufferPool_.end(), pass.framebuffers.begin(),
                                pass.framebuffers.end());
        step.clearFirst = sizeAsU32(clearPool_.size());
        step.clearCount = sizeAsU32(pass.clears.size());
        clearPool_.insert(clearPool_.end(), pass.clears.begin(), pass.clears.end());
        step.extent = pass.extent;
        step.passIndex = static_cast<std::uint16_t>(bodies_.size());
        bodies_.push_back(pass.record);
        steps_.push_back(step);
        // RN-19d0：名字只随步表走一份，计时报告因此不可能与执行的步序错位。
        stepNames_.push_back(pass.name);

        if (renderStep) {
            lastRenderStep = index;
            if (pass.locked) {
                if (lockedStep != desc.passes.size()) {
                    fail("more than one locked render step", pass.name);
                }
                lockedStep = index;
            }
        }
    }

    // GUI 那趟合成界面，永远是最后一个渲染步。这条是「提示框透明度与界面文字亮度」
    // 那批收口的结构保证：任何前端都不得把自己的 pass 插到它后面。
    if (lockedStep != desc.passes.size() && lockedStep != lastRenderStep) {
        fail("the locked render step is not the last one", desc.passes[lockedStep].name);
    }
}

void BakedGraph::execute(VkCommandBuffer commandBuffer, std::uint32_t imageIndex,
                         const PassContext& context,
                         const GpuTimestampWriter& timestamps) const {
    // 三张池的基址在循环外各取一次；循环内全是下标寻址，没有查找、没有分配
    const VkImageMemoryBarrier* const barriers = barrierPool_.data();
    const VkFramebuffer* const framebuffers = framebufferPool_.data();
    const VkClearValue* const clears = clearPool_.data();
    // RN-19d0：不计时时这里一个 vk 入口都不多调
    const bool timing = timestamps.active();
    std::uint32_t timestampSlot = timestamps.firstQuery;
    if (timing) {
        // reset 必须在任何 renderpass 之外——`vkCmdResetQueryPool` 在 renderpass 内非法。
        // 这里是整张图的最前面，天然满足；frame_graph_device_test 的校验层盯着这一条。
        vkCmdResetQueryPool(commandBuffer, timestamps.pool, timestamps.firstQuery,
                            gpuTimestampSlotCount(steps_.size()));
    }
    for (const BakedStep& step : steps_) {
        if (timing) {
            // 打在**屏障之前**：一步的代价包含它自己那次布局转换，否则屏障的时间会
            // 掉进前一步的账上，而屏障恰恰是这张图存在的理由之一。
            // BOTTOM_OF_PIPE = 「在此之前提交的命令全部走完」，这正是边界的定义。
            vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                timestamps.pool, timestampSlot++);
        }
        if (step.barrierCount != 0U) {
            vkCmdPipelineBarrier(commandBuffer, step.barrierSrcStage, step.barrierDstStage, 0, 0,
                                 nullptr, 0, nullptr, step.barrierCount,
                                 barriers + step.barrierFirst);
        }
        if (step.renderPass != VK_NULL_HANDLE) {
            VkRenderPassBeginInfo info{};
            info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            info.renderPass = step.renderPass;
            info.framebuffer =
                framebuffers[step.framebufferFirst + imageIndex * step.framebufferStride];
            info.renderArea.extent = step.extent;
            info.clearValueCount = step.clearCount;
            info.pClearValues = clears + step.clearFirst;
            vkCmdBeginRenderPass(commandBuffer, &info, VK_SUBPASS_CONTENTS_INLINE);
        }
        bodies_[step.passIndex](commandBuffer, context);
        if (step.renderPass != VK_NULL_HANDLE) {
            vkCmdEndRenderPass(commandBuffer);
        }
    }
    if (timing) {
        // 收尾那一点：N 个步骤 N+1 个点，最后一段才有右端。
        vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps.pool,
                            timestampSlot);
    }
}

} // namespace mc::render::graph
