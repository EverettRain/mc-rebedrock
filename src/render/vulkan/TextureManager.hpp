#pragma once

// Owns the self-contained GUI-family texture resources — the vanilla rain
// sheet, the GUI sprite array, the title panorama and the biome colour lookup
// textures — plus the mechanical staging uploads that build them. Extracted
// from VulkanRenderer's Impl so those images/samplers no longer sit in the
// renderer's flat member soup.
//
// The block, entity and font arrays deliberately stay in the renderer for now:
// the block atlas drags a large name-driven asset baker, the entity array
// writes the shared speciesModels list read by the world pass, and the font
// array is coupled to the active language. Those are follow-on extractions.

#include "render/vulkan/GuiSpriteAtlas.hpp"
#include "render/vulkan/VulkanResources.hpp"

#include "assets/ResourceProvider.hpp"
#include "gameplay/entities/SpeciesRenderData.hpp"
#include "ui/BitmapFontMetrics.hpp"
#include "ui/TextFont.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

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

    // Bakes the block/entity/effect array (via BlockAtlasBaker), uploads it, and
    // builds the shared nearest-neighbour sampler. `anisotropy` is the current
    // GameOptions setting; the device caps were captured at construction.
    void createTextureArray(int anisotropy);
    void createTextureSampler(int anisotropy);
    // Rebuilds only the sampler after an anisotropy setting change; the renderer
    // still tears down and rebuilds the descriptor pool around this.
    void recreateTextureSampler(int anisotropy);

    void createRainTexture();
    void createGuiTexture();
    void createPanoramaTexture();
    void createPanoramaSampler();
    void createBiomeTextureResources();
    void updateBiomeColorTextures(std::uint64_t seed);

    // Builds the entity skin array and fills `speciesModels` (owned by the
    // renderer because the world pass reads it) from the gameplay entity
    // registry.
    void
    createEntityTextureArray(std::vector<gameplay::entities::SpeciesRenderModel>& speciesModels);
    // Builds the font array. `fontMetrics`/`textFont` stay renderer-owned (the
    // HUD text pass reads them) and are filled here. The renderer passes the
    // persistent BMP page set so changing language does not rebuild this array.
    void createFontTexture(ui::BitmapFontMetrics& fontMetrics, ui::TextFont& textFont,
                           const std::set<int>& requiredPages, bool forceUnicode);
    // Releases the old font array and rebuilds it for a force-unicode provider
    // change. The renderer still orchestrates the descriptor-pool teardown.
    void recreateFontTexture(ui::BitmapFontMetrics& fontMetrics, ui::TextFont& textFont,
                             const std::set<int>& requiredPages, bool forceUnicode);

    // Releases every owned view and sampler, and — when the VMA allocator is
    // still alive — the images too. Called once from the renderer's shutdown
    // after the device is idle.
    void destroy(bool allocatorAlive) noexcept;

    // Blocks per texel of the biome colour textures (the 1:4 biome map) and the
    // texels per side; kBiomeTextureBlockSpan * kBiomeTextureSize blocks are
    // covered.
    static constexpr int kBiomeTextureBlockSpan = 4;
    static constexpr int kBiomeTextureSize = 512;

    // Public so the renderer's descriptor setup and draw passes read these the
    // same flat way they did when the fields lived in the Impl.
    AllocatedImage rainTextureImage;
    VkImageView rainTextureView = VK_NULL_HANDLE;
    AllocatedImage guiTextureImage;
    VkImageView guiTextureView = VK_NULL_HANDLE;
    // Filled by createGuiTexture() for the widgets the front-end stretches; the
    // HUD binds a reference to this and nine-slices from it.
    GuiWidgetSpriteTable guiWidgetSprites{};
    AllocatedImage panoramaTextureImage;
    VkImageView panoramaTextureView = VK_NULL_HANDLE;
    VkSampler panoramaSampler = VK_NULL_HANDLE;
    AllocatedImage biomeGrassImage;
    VkImageView biomeGrassView = VK_NULL_HANDLE;
    AllocatedImage biomeFoliageImage;
    VkImageView biomeFoliageView = VK_NULL_HANDLE;
    VkSampler biomeSampler = VK_NULL_HANDLE;
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

  private:
    const VulkanResources* resources_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    const assets::ResourceProvider* resourceProvider_ = nullptr;
    bool anisotropySupported_ = false;
    float maxAnisotropy_ = 1.0F;
};

} // namespace mc::render
