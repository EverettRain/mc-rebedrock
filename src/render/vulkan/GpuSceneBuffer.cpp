#include "render/vulkan/GpuSceneBuffer.hpp"

namespace mc::render {

void GpuSceneBuffer::init(const Config& config) {
    destroy();
    resources_ = config.resources;
    capacityBytes_ = config.capacityBytes;
    buffers_.resize(config.frameCount);
    for (auto& buffer : buffers_) {
        buffer = resources_->createBuffer(config.capacityBytes,
                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    }
}

void GpuSceneBuffer::destroy() {
    if (resources_ != nullptr) {
        for (auto& buffer : buffers_) {
            resources_->destroyBuffer(buffer);
        }
    }
    buffers_.clear();
    capacityBytes_ = 0;
}

} // namespace mc::render
