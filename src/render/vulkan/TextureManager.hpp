#pragma once

// GUI 一族自足纹理资源的所有者
// 含雨幕贴图、GUI 精灵数组、标题全景、群系配色查找表，以及把它们建起来的暂存上传
//
// 方块、实体、字体三个数组仍留在渲染器里
// 方块图集牵着一整套按名解析的资源烘焙器
// 实体数组要写世界通道读取的共享 speciesModels 列表，字体数组则与当前语言耦合

#include "render/vulkan/BlockAtlasBaker.hpp"
#include "render/vulkan/GuiSpriteAtlas.hpp"
#include "render/vulkan/HudTypes.hpp"
#include "render/vulkan/VulkanResources.hpp"

#include "assets/ResourceProvider.hpp"
#include "gameplay/entities/SpeciesRenderData.hpp"
#include "assets/ImageData.hpp"
#include "ui/BitmapFontMetrics.hpp"
#include "ui/TextFont.hpp"

#include <glm/vec4.hpp>

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <span>

#include <array>
#include <cstdint>
#include <filesystem>
#include <set>
#include <utility>
#include <vector>

namespace mc::render {

class TextureManager final {
  public:
    TextureManager() = default;
    TextureManager(const VulkanResources* resources, VkDevice device, VmaAllocator allocator,
                   const assets::ResourceProvider* resourceProvider, bool anisotropySupported,
                   float maxAnisotropy)
        : resources_(resources), device_(device), allocator_(allocator),
          resourceProvider_(resourceProvider),
          anisotropySupported_(anisotropySupported), maxAnisotropy_(maxAnisotropy) {}

    // 经 BlockAtlasBaker 烘出方块/实体/特效数组、上传，并建好共享的最近邻采样器
    // `anisotropy` 取自当前 GameOptions；设备上限在构造时已记下
    void createTextureArray(int anisotropy);
    void createTextureSampler(int anisotropy);
    // 各向异性设置变更后只重建采样器；描述符池的拆建仍由渲染器在外面负责
    void recreateTextureSampler(int anisotropy);

    void createRainTexture();
    void createGuiTexture();
    void createPanoramaTexture();
    void createPanoramaSampler();
    // UI-2：主菜单的 logo 与 edition 副标题
    //
    // 它们进不了 256px 的 GUI 图集：26.1 的 gui/title/minecraft.png 是 1024x256，
    // edition.png 是 512x64（逻辑尺寸仍是 256x64 与 128x16，也就是四倍分辨率的真资源）。
    // 缩到 256 再画会在 GUI 缩放 ≥2 时明显糊掉，那就不是 1:1 了。所以照 createPanoramaTexture
    // 的先例单独用一个原生分辨率的数组，而不是挤进 GUI 数组。
    //
    // 三张图竖着叠进同一层，各自的归一化 UV 在下面给出——于是只多一张图、一个绑定点。
    void createTitleTexture();

    // 建实体皮肤数组，并按玩法实体注册表填充 `speciesModels`
    // 该列表归渲染器所有，因为世界通道要读它
    void
    createEntityTextureArray(std::vector<gameplay::entities::SpeciesRenderModel>& speciesModels);
    // 建字体数组
    // `fontMetrics`/`textFont` 归渲染器所有（HUD 文本通道要读），在这里填充
    // 渲染器传入常驻的 BMP 页集合，因此切换语言不必重建整个数组
    // UI-11 / A6：把存档缩略图刷进 GUI 图集最后一层的 16 个槽位（多的丢弃）。
    void uploadWorldIcons(std::span<const assets::ImageData> icons);

    void createFontTexture(ui::BitmapFontMetrics& fontMetrics, ui::TextFont& textFont,
                           const std::set<int>& requiredPages, bool forceUnicode);
    // 强制 Unicode 字体开关变化时，释放旧字体数组并重建；描述符池的拆建仍由渲染器编排
    void recreateFontTexture(ui::BitmapFontMetrics& fontMetrics, ui::TextFont& textFont,
                             const std::set<int>& requiredPages, bool forceUnicode);

    // 释放全部自有 view 与 sampler；若 VMA 分配器尚在，连图像一起释放
    // 由渲染器在设备空闲后的关闭流程里调用一次
    void destroy(bool allocatorAlive) noexcept;

    // 群系配色纹理按 1:4 的群系图取样，这两个常量是每纹素覆盖的方块数与边长纹素数

    // 公开：渲染器的描述符装配与各绘制通道直接平铺读取这些句柄
    AllocatedImage rainTextureImage;
    VkImageView rainTextureView = VK_NULL_HANDLE;
    AllocatedImage guiTextureImage;
    VkImageView guiTextureView = VK_NULL_HANDLE;
    // 由 createGuiTexture() 为前端会拉伸的控件填充；HUD 绑一个引用，据此做九宫格
    GuiWidgetSpriteTable guiWidgetSprites{};
    AllocatedImage panoramaTextureImage;
    VkImageView panoramaTextureView = VK_NULL_HANDLE;
    VkSampler panoramaSampler = VK_NULL_HANDLE;
    // UI-2：标题美术（binding 6），一层，三张图竖着叠放
    // 采样器共用 textureSampler——那是最近邻的那个，和 26.1 给 GUI 纹理的过滤方式一致；
    // 全景那个采样器是线性的（放大的实拍图需要），拿它画 logo 会把像素画糊掉
    AllocatedImage titleTextureImage;
    VkImageView titleTextureView = VK_NULL_HANDLE;
    // 三张图各自在这一层里的归一化 UV；HUD 绑一个 const 引用，据此画 logo 与副标题
    TitleArtUv titleArtUv{};
    AllocatedImage fontTextureImage;
    VkImageView fontTextureView = VK_NULL_HANDLE;
    AllocatedImage entityTextureImage;
    VkImageView entityTextureView = VK_NULL_HANDLE;
    std::uint32_t entityTextureWidth = 0U;
    std::uint32_t entityTextureHeight = 0U;
    AllocatedImage textureImage;
    VkImageView textureView = VK_NULL_HANDLE;
    VkSampler textureSampler = VK_NULL_HANDLE;
    std::array<float, 4> fluidAnimationFrameTimes{1.0F, 1.0F, 1.0F, 1.0F};
    // 非流体动画方块纹理（首层/帧数/每帧 tick），按连续帧烘入
    // 渲染器把它们转给地形着色器，轮播方式与流体一致
    std::vector<BlockTextureAnimation> blockAnimations;

    // 本管理器持有的全部纹理与图集图像在 VMA 中的常驻字节总量
    // 含方块图集、实体、GUI、字体、全景、雨幕和两张群系查找表
    // 逐个分配用 vmaGetAllocationInfo 查得
    [[nodiscard]] std::size_t residentImageBytes() const;

  private:
    const VulkanResources* resources_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    const assets::ResourceProvider* resourceProvider_ = nullptr;
    bool anisotropySupported_ = false;
    float maxAnisotropy_ = 1.0F;
};

} // namespace mc::render
