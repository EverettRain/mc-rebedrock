#pragma once

#include "render/vulkan/VulkanResources.hpp"

#include <glm/vec4.hpp>

#include <cstddef>
#include <vector>

namespace mc::render {

// Per-instance record for GPU particle draws. The layout must match the std430
// buffer in particle_instanced.vert. Three 16-byte vec4s: the seven payload
// floats (position3, size, uvOrigin2, uvScale, opacity, textureLayer, packed
// light) padded to std430's vec4 alignment — the AsyncParticles raw-record
// pattern, where the CPU writes one compact record per particle and the vertex
// shader expands the camera-facing quad on the GPU.
struct ParticleRecord final {
    alignas(16) glm::vec4 positionSize;   // xyz world position, w quad size
    alignas(16) glm::vec4 uvOriginScale;  // xy uvOrigin, z uvScale, w opacity
    alignas(16) glm::vec4 layerLight;     // x textureLayer, y packed scene light
};
static_assert(sizeof(ParticleRecord) == 48);

// Ring of host-visible storage buffers, one per in-flight frame. Each frame
// owns a buffer large enough for the particle cap, so a frame writes its
// records into its own slot without racing the previous frame's GPU read — the
// fence wait at the top of drawFrame orders host writes against the prior
// submission of the same slot. A persistent host-mapped buffer needs no
// transfer pass or barrier: the host writes are ordered by vkQueueSubmit and
// the per-frame fence.
class GpuSceneBuffer final {
  public:
    struct Config final {
        const VulkanResources* resources = nullptr;
        std::size_t frameCount = 0;
        std::size_t capacityBytes = 0;
    };

    void init(const Config& config);
    void destroy();

    [[nodiscard]] AllocatedBuffer& frame(std::size_t index) { return buffers_[index]; }
    [[nodiscard]] const AllocatedBuffer& frame(std::size_t index) const {
        return buffers_[index];
    }
    [[nodiscard]] std::size_t capacityBytes() const { return capacityBytes_; }
    [[nodiscard]] std::size_t frameCount() const { return buffers_.size(); }

  private:
    const VulkanResources* resources_ = nullptr;
    std::vector<AllocatedBuffer> buffers_;
    std::size_t capacityBytes_ = 0;
};

} // namespace mc::render
