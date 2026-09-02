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
// 由 particle_instanced.vert 解读
// 方块粉尘与 async 雨滴都走这条：雨滴的 uvOrigin 取 (0,0)、uvScale 取 1，即整张水纹
// RN-9b：layerLight 的后两槽是粒子的外观通道
//   z = 打包的 RGB tint（r<<16|g<<8|b，各 0..255；0 表示不着色）
//       fp32 精确到 2^24，0xFFFFFF 正好落在边界内，往返无损
//   w = 自发光 0..1，着色器把它当作方块光的下限（vanilla
//       FlyTowardsPositionParticle#getLightCoords 的 addSmoothBlockEmission）
// 两槽从前恒为 0，现有粒子照样填 0，逐像素结果不变
struct ParticleRecord final {
    alignas(16) glm::vec4 positionSize;   // xyz world position, w quad size
    alignas(16) glm::vec4 uvOriginScale;  // xy uvOrigin, z uvScale, w opacity
    alignas(16) glm::vec4 layerLight;     // x textureLayer, y packed scene light,
                                          // z packed tint, w emission
};
static_assert(sizeof(ParticleRecord) == 48);

// 贴图雨的逐列记录，与 ParticleRecord 共用同一块场景存储缓冲和同一个 48 字节槽位
// 但两者字段含义完全不同，它由 rain_sheet.vert 里的 RainColumn 解读而不是 particle_instanced.vert
//
// 这三个 vec4 曾直接以 ParticleRecord 的名义写入
// 于是 uvOriginScale 里装的其实是列顶、不透明度、滚动相位与光照，与它自己的字段注释完全对不上
// 改 ParticleRecord 注释的人不会知道还有第二个读者，GPU 侧的 RainColumn 又是独立声明的
// 两边只能靠人肉同步，因此给它一个自己的名字和字段名，布局用 static_assert 钉死
struct RainColumnRecord final {
    alignas(16) glm::vec4 positionBottomWidth;   // xz 列心, y 列底, w 半宽
    alignas(16) glm::vec4 topOpacityPhaseLight;  // x 列顶, y 不透明度, z 滚动相位, w 打包光照
    alignas(16) glm::vec4 tangent;               // xy 面向相机的水平切向
};
static_assert(sizeof(RainColumnRecord) == sizeof(ParticleRecord));
static_assert(alignof(RainColumnRecord) == alignof(ParticleRecord));

// 两种记录共享一条暂存队列，因此写入端显式做一次转换
// 这个转换点就是写入者声明自己清楚该槽位要交给哪个着色器读
[[nodiscard]] inline ParticleRecord asParticleRecord(const RainColumnRecord& column) {
    return {column.positionBottomWidth, column.topOpacityPhaseLight, column.tangent};
}

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
