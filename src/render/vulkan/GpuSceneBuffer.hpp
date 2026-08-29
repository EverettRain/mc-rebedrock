#pragma once

#include "render/vulkan/VulkanResources.hpp"

#include <glm/vec4.hpp>

#include <cstddef>
#include <vector>

namespace mc::render {

// GPU 场景精灵与雨条的逐实例记录
// 布局与 particle_instanced.vert、rain_sheet.vert 里的 std430 缓冲一致
// 三个 16 字节 vec4 装下位置、尺寸、UV 原点与缩放、不透明度、纹理层和打包光照
// 各字段按 std430 的 vec4 对齐补齐
// CPU 每个粒子只写一条紧凑记录，面向相机的四边形由顶点着色器在 GPU 上展开
struct ParticleRecord final {
    alignas(16) glm::vec4 positionSize;   // xyz world position, w quad size
    alignas(16) glm::vec4 uvOriginScale;  // xy uvOrigin, z uvScale, w opacity
    alignas(16) glm::vec4 layerLight;     // x textureLayer, y packed scene light
};
static_assert(sizeof(ParticleRecord) == 48);

// 主机可见存储缓冲的环，每个在飞帧一个，各自容量按粒子上限给足
// 于是一帧只写自己的槽，不会和上一帧的 GPU 读竞争
// drawFrame 开头对同槽围栏的等待，把主机写排在上一次提交之后
// 常驻映射的缓冲不需要传输通道也不需要屏障：提交和逐帧围栏已经定序
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
