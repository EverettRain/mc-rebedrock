#include "render/graph/FrameGraph.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace mc::render::graph {
namespace {

[[noreturn]] void fail(std::string_view what, std::string_view name) {
    throw std::runtime_error("frame graph: " + std::string{what} + " (" + std::string{name} + ")");
}

[[nodiscard]] bool writes(Access access) noexcept {
    return access == Access::ColorWrite || access == Access::DepthWrite;
}

[[nodiscard]] bool reads(Access access) noexcept {
    return access == Access::Sample || access == Access::TransferRead;
}

[[nodiscard]] std::uint32_t sizeAsU32(std::size_t value) {
    return static_cast<std::uint32_t>(value);
}

} // namespace

void BakedGraph::reset() noexcept {
    steps_.clear();
    bodies_.clear();
    barrierPool_.clear();
    framebufferPool_.clear();
    clearPool_.clear();
}

void BakedGraph::compile(const GraphDesc& desc) {
    reset();

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
                         const PassContext& context) const {
    // 三张池的基址在循环外各取一次；循环内全是下标寻址，没有查找、没有分配
    const VkImageMemoryBarrier* const barriers = barrierPool_.data();
    const VkFramebuffer* const framebuffers = framebufferPool_.data();
    const VkClearValue* const clears = clearPool_.data();
    for (const BakedStep& step : steps_) {
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
}

} // namespace mc::render::graph
