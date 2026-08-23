#pragma once
// HudRenderer: the HUD / front-end drawing subsystem extracted verbatim from
// VulkanRenderer::Impl. It owns the small UI animation state and reaches the
// renderer core through reference members (bound once in Bindings) plus a few
// std::function hooks for world-render couplings; the draw bodies are unchanged.
// Header-only inline, mirroring VulkanDevice.
#include "render/vulkan/GuiSpriteAtlas.hpp"
#include "render/vulkan/HudTypes.hpp"

#include "animation/PlayerModelAnimator.hpp"
#include "animation/SkeletalModel.hpp"
#include "config/GameOptions.hpp"
#include "gameplay/ChestSystem.hpp"
#include "gameplay/CraftingSystem.hpp"
#include "gameplay/GameMode.hpp"
#include "client/ClientMirror.hpp"
#include "core/VersionManifest.hpp"
#include "gameplay/GameSession.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/ItemEntitySystem.hpp"
#include "gameplay/SpawnEggItems.hpp"
#include "gameplay/command/ArgumentType.hpp"
#include "gameplay/command/CommandDispatcher.hpp"
#include "gameplay/entities/SpeciesRenderData.hpp"
#include "persistence/SaveRepository.hpp"
#include "render/PerspectiveCamera.hpp"
#include "render/TestScene.hpp"
#include "input/InputAction.hpp"
#include "input/InputNaming.hpp"
#include "ui/BitmapFontMetrics.hpp"
#include "ui/ButtonControl.hpp"
#include "ui/ChatHistory.hpp"
#include "ui/GuiNineSlice.hpp"
#include "ui/SubtitleFeed.hpp"
#include "ui/Toast.hpp"
#include "ui/HudLayout.hpp"
#include "ui/Language.hpp"
#include "ui/MenuGeometry.hpp"
#include "ui/MenuSystem.hpp"
#include "ui/PageBuilder.hpp"
#include "ui/PageStack.hpp"
#include "ui/TextFont.hpp"
#include "ui/TextWrap.hpp"
#include "ui/UiFrameData.hpp"
#include "world/ChunkStreamer.hpp"
#include "world/DayNightCycle.hpp"
#include "world/World.hpp"
#include "world/WorldConstants.hpp"

#include <unordered_map>

#include <GLFW/glfw3.h>
#include <vulkan/vulkan.h>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
namespace mc::render {
class HudRenderer final {
  public:
    struct Bindings final {
        ui::MenuSystem& menuSystem;
        ui::UiFrameData& uiFrameData_;
        gameplay::GameSession& gameSession;
        // Stage C slice 1b-2: HUD player/world reads come from the client mirror.
        const client::ClientMirror& clientMirror;
        ui::TextFont& textFont;
        ui::BitmapFontMetrics& fontMetrics;
        ui::Language& language;
        // The world the HUD samples light from. This is the render-owned client
        // chunk cache, never the server world — the HUD's light reads must not
        // take the server lock.
        world::World& lightWorld;
        GLFWwindow*& window;
        config::GameOptions& options;
        PerspectiveCamera& camera;
        VkExtent2D& swapchainExtent;
        VkPipeline& hudPipeline;
        VkPipelineLayout& hudPipelineLayout;
        VkPipeline& vignettePipeline;
        VkPipeline& crosshairPipeline;
        VkPipeline& panoramaPipeline;
        VkPipelineLayout& panoramaPipelineLayout;
        VkPipeline& heldItemPipeline;
        VkPipelineLayout& itemPipelineLayout;
        bool& inventoryOpen;
        const ContainerScreen& containerScreen;
        const std::optional<gameplay::ChestPosition>& activeChest;
        bool& debugOverlayOpen;
        bool& inventoryDragActive;
        std::vector<gameplay::SlotRef>& inventoryDragSlots;
        bool& chatOpen;
        ui::ChatHistory& chatHistory;
        std::string& chatInputText;
        std::vector<gameplay::command::Suggestion>& chatSuggestions_;
        std::size_t& chatSuggestionIndex_;
        ui::ToastQueue& toastQueue;
        ui::SubtitleFeed& subtitleFeed;
        std::optional<persistence::SaveGame>& currentSave;
        int& displayedFps;
        animation::PlayerModelAnimator& playerModelAnimator;
        ui::WidgetId& pressedMenuButton;
        bool& spawnPositionInitialized;
        bool& worldReady;
        bool& worldSessionActive;
        int& simulationDistanceChunks;
        int& viewDistanceChunks;
        std::size_t& peakPendingSectionCount;
        const std::unordered_map<world::SectionPosition, world::SectionMeshUpdate,
                                 world::SectionPositionHash>& pendingSectionUpdates;
        const std::optional<TestSceneOptions>& testScene;
        // Atlas rectangles + 26.1 gui.scaling for the stretchable widgets,
        // filled by TextureManager::createGuiTexture().
        const GuiWidgetSpriteTable& guiWidgetSprites;
        bool& paused;
        double& uiTimeSeconds;
        std::function<bool()> cameraSubmergedInWater;
        // PX-6 Bug1: the Controls key-bind row label ("Action: Key") from the
        // InputSystem single source, so the draw page shows live bindings.
        std::function<std::string(input::InputAction)> keyBindLabel;
        std::function<void(VkCommandBuffer, VkDescriptorSet)> drawHeldItem;
        std::function<VkDescriptorSet()> currentFrameDescriptorSet;
        std::function<std::span<const gameplay::ItemStack>()> activeCreativeCatalog;
        std::function<float()> creativeScrollPosition;
        std::function<std::size_t()> creativeMaximumScrollRow;
        std::function<std::vector<std::uint8_t>()> dragPlacementCounts;
        std::function<float()> cameraFarPlane;
        std::function<std::optional<ui::UiRect>(const ui::HudLayout&, const gameplay::SlotRef&)>
            dragSlotRectangle;
    };

    explicit HudRenderer(const Bindings& b)
        : menuSystem(b.menuSystem), uiFrameData_(b.uiFrameData_), gameSession(b.gameSession),
          clientMirror(b.clientMirror),
          textFont(b.textFont), fontMetrics(b.fontMetrics), language(b.language),
          lightWorld(b.lightWorld), window(b.window), options(b.options),
          camera(b.camera), swapchainExtent(b.swapchainExtent), hudPipeline(b.hudPipeline),
          hudPipelineLayout(b.hudPipelineLayout), vignettePipeline(b.vignettePipeline),
          crosshairPipeline(b.crosshairPipeline), panoramaPipeline(b.panoramaPipeline),
          panoramaPipelineLayout(b.panoramaPipelineLayout), heldItemPipeline(b.heldItemPipeline),
          itemPipelineLayout(b.itemPipelineLayout), inventoryOpen(b.inventoryOpen),
          containerScreen(b.containerScreen), activeChest(b.activeChest),
          debugOverlayOpen(b.debugOverlayOpen),
          inventoryDragActive(b.inventoryDragActive), inventoryDragSlots(b.inventoryDragSlots),
          chatOpen(b.chatOpen), chatHistory(b.chatHistory), chatInputText(b.chatInputText),
          chatSuggestions_(b.chatSuggestions_), chatSuggestionIndex_(b.chatSuggestionIndex_),
          toastQueue(b.toastQueue), subtitleFeed(b.subtitleFeed),
          currentSave(b.currentSave), displayedFps(b.displayedFps),
          playerModelAnimator(b.playerModelAnimator), pressedMenuButton(b.pressedMenuButton),
          spawnPositionInitialized(b.spawnPositionInitialized), worldReady(b.worldReady),
          worldSessionActive(b.worldSessionActive),
          simulationDistanceChunks(b.simulationDistanceChunks),
          viewDistanceChunks(b.viewDistanceChunks),
          peakPendingSectionCount(b.peakPendingSectionCount),
          pendingSectionUpdates(b.pendingSectionUpdates), testScene(b.testScene),
          guiWidgetSprites(b.guiWidgetSprites), paused(b.paused),
          uiTimeSeconds(b.uiTimeSeconds), cameraSubmergedInWater(b.cameraSubmergedInWater),
          keyBindLabel(b.keyBindLabel),
          drawHeldItem(b.drawHeldItem), currentFrameDescriptorSet(b.currentFrameDescriptorSet),
          activeCreativeCatalog(b.activeCreativeCatalog),
          creativeScrollPosition(b.creativeScrollPosition),
          creativeMaximumScrollRow(b.creativeMaximumScrollRow),
          dragPlacementCounts(b.dragPlacementCounts), cameraFarPlane(b.cameraFarPlane),
          dragSlotRectangle(b.dragSlotRectangle) {}

    HudRenderer(const HudRenderer&) = delete;
    HudRenderer& operator=(const HudRenderer&) = delete;

    // ---- helpers duplicated from the renderer core (pure reads over the bound
    // references), so the moved draw code resolves them without reaching back
    // into Impl. Impl keeps its own copies for the input path. ----
    [[nodiscard]] std::string_view translate(std::string_view key,
                                             std::string_view fallback) const {
        return language.translate(key, fallback);
    }
    [[nodiscard]] std::string translated(std::string_view key, std::string_view fallback) const {
        return std::string{translate(key, fallback)};
    }
    [[nodiscard]] ui::UiPoint currentFramebufferCursor() const {
        double cursorX = 0.0;
        double cursorY = 0.0;
        int windowWidth = 0;
        int windowHeight = 0;
        int framebufferWidth = 0;
        int framebufferHeight = 0;
        glfwGetCursorPos(window, &cursorX, &cursorY);
        glfwGetWindowSize(window, &windowWidth, &windowHeight);
        glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
        return ui::windowToFramebuffer(cursorX, cursorY, windowWidth, windowHeight,
                                       framebufferWidth, framebufferHeight);
    }
    [[nodiscard]] std::size_t menuButtonCount() const {
        return ui::menuButtonCount(menuSystem.pageStack.current(), currentSave.has_value());
    }
    [[nodiscard]] ui::UiRect worldListRow(std::size_t index, const ui::HudLayout& layout) const {
        return ui::worldListRow(index, layout, static_cast<float>(swapchainExtent.width));
    }
    [[nodiscard]] std::size_t saveListVisibleRowCount() const {
        return ui::saveListVisibleRowCount(static_cast<float>(swapchainExtent.width),
                                           static_cast<float>(swapchainExtent.height),
                                           menuSystem.guiScaleSetting);
    }
    [[nodiscard]] ui::UiRect languageListBox(const ui::HudLayout& layout) const {
        return ui::languageListBox(layout, static_cast<float>(swapchainExtent.width));
    }
    [[nodiscard]] float languageWarningY(const ui::HudLayout& layout) const {
        return ui::languageWarningY(layout);
    }
    [[nodiscard]] ui::UiRect languageRow(std::size_t index, const ui::HudLayout& layout) const {
        return ui::languageRow(index, layout, static_cast<float>(swapchainExtent.width));
    }
    [[nodiscard]] std::size_t languageVisibleRowCount() const {
        return ui::languageVisibleRowCount(static_cast<float>(swapchainExtent.width),
                                           static_cast<float>(swapchainExtent.height),
                                           menuSystem.guiScaleSetting);
    }
    [[nodiscard]] ui::UiRect frontendButtonRect(const ui::HudLayout& layout, ui::PageId page,
                                                std::size_t index, std::size_t buttonCount) const {
        return ui::frontendButtonRect(layout, page, index, buttonCount);
    }

    // PX-4: the current page as a ui::Page for DRAWING — the single source shared
    // with dispatch (ui::buildPage), so the old per-page MenuButton arrays are
    // gone. Wired with labels + slider display values (not action callbacks): the
    // draw backend reads label/rect/kind/enabled/slider.value off each widget.
    // Action callbacks are intentionally empty; drawing never fires them.
    [[nodiscard]] ui::Page buildDrawPage() const {
        const ui::PageId pageId = menuSystem.pageStack.current();
        const ui::HudLayout layout{static_cast<float>(swapchainExtent.width),
                                   static_cast<float>(swapchainExtent.height),
                                   menuSystem.guiScaleSetting};
        const std::size_t count = menuButtonCount();
        ui::MenuBuildContext ctx;
        ctx.worldOpen = currentSave.has_value();
        ctx.worldSelectable = !menuSystem.saveSummaries.empty();
        ctx.labelFor = [this](std::uint16_t id) {
            return widgetLabel(static_cast<ui::WidgetId>(id));
        };
        // PX-6 Bug1: on Controls, feed the scroll window + the live key-bind
        // labels, and lay the visible rows out as list rows (controlsRow). The
        // trailing four are the bottom button band. Everything else is unchanged.
        const float fbWidth = static_cast<float>(swapchainExtent.width);
        std::size_t keyRows = 0U;
        if (pageId == ui::PageId::Controls) {
            const std::size_t total = input::keyBindRows().size();
            const std::size_t window = ui::controlsVisibleRowCount(
                fbWidth, static_cast<float>(swapchainExtent.height), menuSystem.guiScaleSetting);
            const std::size_t first = std::min(menuSystem.controlsListFirstIndex, total);
            keyRows = std::min(window, total - first);
            ctx.keyBindFirstIndex = first;
            ctx.keyBindRowCount = keyRows;
            ctx.keyBindLabelFor = [this](input::InputAction action) {
                return keyBindLabel ? keyBindLabel(action)
                                    : std::string{input::actionDisplayName(action)};
            };
        }
        ui::MenuCallbacks callbacks;
        callbacks.viewDistance.value = [this] {
            return static_cast<float>(viewDistanceChunks - 2) / 34.0F;
        };
        callbacks.simulationDistance.value = [this] {
            return static_cast<float>(simulationDistanceChunks - 2) / 10.0F;
        };
        callbacks.masterVolume.value = [this] { return options.masterVolume; };
        return ui::buildPage(pageId, ctx, callbacks,
                             [layout, pageId, count, fbWidth, keyRows](std::size_t index) {
                                 if (pageId == ui::PageId::Controls && index < keyRows) {
                                     return ui::controlsRow(index, layout, fbWidth);
                                 }
                                 const std::size_t buttonIndex =
                                     pageId == ui::PageId::Controls ? index - keyRows : index;
                                 return ui::frontendButtonRect(layout, pageId, buttonIndex, count);
                             });
    }

    void drawDragPreview(VkCommandBuffer commandBuffer, const ui::HudLayout& layout) const {
        if (!inventoryDragActive || clientMirror.world().cursorStack.empty()) {
            return;
        }
        const auto counts = dragPlacementCounts();
        for (std::size_t index = 0; index < inventoryDragSlots.size(); ++index) {
            const auto rect = dragSlotRectangle(layout, inventoryDragSlots[index]);
            if (!rect.has_value() || counts[index] == 0U) {
                continue;
            }
            drawHudQuad(commandBuffer, *rect, {0.0F, 0.0F, 0.0F, 0.5F});
            drawHudItemIcon(commandBuffer, *rect, clientMirror.world().cursorStack);
            const std::string count = std::to_string(counts[index]);
            const float textScale = layout.scale();
            drawHudText(commandBuffer, count,
                        rect->x + 17.0F * textScale - hudTextWidth(count, textScale),
                        rect->y + 9.0F * textScale, textScale, {1.0F, 1.0F, 1.0F, 1.0F});
        }
    }

    void drawHudQuad(VkCommandBuffer commandBuffer, const ui::UiRect& rectangle,
                     const glm::vec4& color, float textureLayer = 0.0F, bool textured = false,
                     glm::vec4 uvRectangle = {0.0F, 0.0F, 1.0F, 1.0F}, bool fontGlyph = false,
                     bool guiSprite = false) const {
        const float width = static_cast<float>(swapchainExtent.width);
        const float height = static_cast<float>(swapchainExtent.height);
        const auto clipRectangle = ui::framebufferToClip(rectangle, width, height);
        const HudPush push{
            {
                clipRectangle.x,
                clipRectangle.y,
                clipRectangle.width,
                clipRectangle.height,
            },
            color,
            uvRectangle,
            {guiSprite ? 3.0F : (fontGlyph ? 2.0F : (textured ? 1.0F : 0.0F)), textureLayer, 0.0F,
             0.0F},
        };
        vkCmdPushConstants(commandBuffer, hudPipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);
        vkCmdDraw(commandBuffer, 6, 1, 0, 0);
    }

    // `portion` is the slab half the icon shows: 0 full cube, 1 bottom, 2 top;
    // the shader reads it from uvRect.x (the block branch ignores uvRect) and
    // folds the cube down to a half slab. It rides uvRect rather than data so the
    // three texture-layer slots in data stay free for the chest/furnace fronts.
    void drawHudBlockIcon(VkCommandBuffer commandBuffer, const ui::UiRect& rectangle,
                          world::Block block, float portion = 0.0F) const {
        const float width = static_cast<float>(swapchainExtent.width);
        const float height = static_cast<float>(swapchainExtent.height);
        const auto clipRectangle = ui::framebufferToClip(rectangle, width, height);
        const auto textures = world::textureLayers(block);
        const bool chest = block == world::Block::Chest;
        const bool furnace = block == world::Block::Furnace;
        const HudPush push{
            {clipRectangle.x, clipRectangle.y, clipRectangle.width, clipRectangle.height},
            {1.0F, 1.0F, 1.0F, 1.0F},
            {portion, 0.0F, 1.0F, 1.0F},
            {(chest || furnace) ? 4.25F : 4.0F, chest ? kChestItemTopLayer : textures.top,
             chest ? kChestItemFrontLayer : (furnace ? kFurnaceFrontLayer : textures.side),
             chest ? kChestItemSideLayer : textures.side},
        };
        vkCmdPushConstants(commandBuffer, hudPipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);
        vkCmdDraw(commandBuffer, 18, 1, 0, 0);
    }

    void drawHudItemIcon(VkCommandBuffer commandBuffer, const ui::UiRect& rectangle,
                         const gameplay::ItemStack& stack) const {
        if (gameplay::isBlockStack(stack)) {
            const auto model = world::blockDefinition(stack.block).model;
            if (model == world::BlockModel::Cube || model == world::BlockModel::Chest) {
                drawHudBlockIcon(commandBuffer, rectangle, stack.block);
                return;
            }
            if (model == world::BlockModel::Slab) {
                // A slab item is wielded as the bottom half, so the inventory icon
                // shows the bottom-half cube like vanilla's slab item render.
                drawHudBlockIcon(commandBuffer, rectangle, stack.block, 1.0F);
                return;
            }
        }
        drawHudQuad(commandBuffer, rectangle, {1.0F, 1.0F, 1.0F, 1.0F},
                    gameplay::itemTextureLayer(stack), true);
    }

    void drawGuiSprite(VkCommandBuffer commandBuffer, const ui::UiRect& destination, float layer,
                       const ui::UiRect& sourcePixels,
                       const glm::vec4& tint = {1.0F, 1.0F, 1.0F, 1.0F}) const {
        constexpr float atlasSize = 256.0F;
        drawHudQuad(commandBuffer, destination, tint, layer, false,
                    {sourcePixels.x / atlasSize, sourcePixels.y / atlasSize,
                     sourcePixels.width / atlasSize, sourcePixels.height / atlasSize},
                    false, true);
    }

    // Draws a widget sprite into `destination` honouring its 26.1 gui.scaling.
    // `scale` is the GUI scale — framebuffer pixels per GUI pixel — which is
    // what turns the sprite's declared pixel borders into destination lengths,
    // so a 3px button frame stays 3 GUI pixels at any button width instead of
    // smearing with the rest of the bitmap. The slicing itself lives in
    // ui::forEachGuiSpriteQuad, which is Vulkan-free and unit-tested.
    void drawScaledGuiSprite(VkCommandBuffer commandBuffer, const ui::UiRect& destination,
                             float layer, const GuiAtlasSprite& sprite, float scale,
                             const glm::vec4& tint = {1.0F, 1.0F, 1.0F, 1.0F}) const {
        ui::forEachGuiSpriteQuad(destination, sprite.region, sprite.scaling, scale,
                                 [&](const ui::GuiSpriteQuad& quad) {
                                     drawGuiSprite(commandBuffer, quad.destination, layer,
                                                   quad.source, tint);
                                 });
    }

    void drawMinecraftCrosshair(VkCommandBuffer commandBuffer, const ui::UiRect& rectangle) const {
        constexpr float atlasSize = 256.0F;
        const auto clipRectangle =
            ui::framebufferToClip(rectangle, static_cast<float>(swapchainExtent.width),
                                  static_cast<float>(swapchainExtent.height));
        const HudPush push{
            {clipRectangle.x, clipRectangle.y, clipRectangle.width, clipRectangle.height},
            {1.0F, 1.0F, 1.0F, 1.0F},
            {0.0F, 0.0F, 15.0F / atlasSize, 15.0F / atlasSize},
            {5.0F, 1.0F, 0.0F, 0.0F},
        };
        vkCmdPushConstants(commandBuffer, hudPipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);
        vkCmdDraw(commandBuffer, 6, 1, 0, 0);
    }

    void drawUnderwaterOverlay(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet) const {
        if (!cameraSubmergedInWater()) {
            return;
        }
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipelineLayout,
                                0, 1, &descriptorSet, 0, nullptr);
        drawGuiSprite(commandBuffer,
                      {0.0F, 0.0F, static_cast<float>(swapchainExtent.width),
                       static_cast<float>(swapchainExtent.height)},
                      6.0F, {0.0F, 0.0F, 256.0F, 256.0F}, {0.70F, 0.85F, 1.0F, 0.10F});
    }

    // 1.16.1's Screen.renderBackground darkens every open in-game screen with a
    // vertical gradient (top rgba(16,16,16,0xC0) -> bottom rgba(16,16,16,0xD0)),
    // baked into kScreenDimGuiLayer in createGuiTexture().
    void drawScreenDimOverlay(VkCommandBuffer commandBuffer) const {
        drawGuiSprite(commandBuffer,
                      {0.0F, 0.0F, static_cast<float>(swapchainExtent.width),
                       static_cast<float>(swapchainExtent.height)},
                      kScreenDimGuiLayer, {0.0F, 0.0F, 256.0F, 256.0F}, {1.0F, 1.0F, 1.0F, 1.0F});
    }

    // 1.16.1's InGameHud renders the vignette texture with a multiplicative
    // blend (dst * (1 - src)) so the dark corners darken the scene while the
    // centre is left untouched. It must run on the dedicated vignette pipeline;
    // the HUD pipeline is rebound afterwards so later HUD sprites keep their
    // normal alpha blending.
    void drawVignette(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet) const {
        if (vignetteDarkness_ <= 0.001F) {
            return;
        }
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vignettePipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipelineLayout,
                                0, 1, &descriptorSet, 0, nullptr);
        const float darkness = vignetteDarkness_;
        drawGuiSprite(commandBuffer,
                      {0.0F, 0.0F, static_cast<float>(swapchainExtent.width),
                       static_cast<float>(swapchainExtent.height)},
                      kVignetteGuiLayer, {0.0F, 0.0F, 256.0F, 256.0F},
                      {darkness, darkness, darkness, 1.0F});
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipeline);
    }

    void drawMinecraftButton(VkCommandBuffer commandBuffer, const ui::UiRect& rectangle,
                             std::string_view label, ui::ButtonVisualState state, float scale,
                             glm::vec4 tint = glm::vec4{1.0F}) const {
        // Snap to whole framebuffer pixels. On a maximised or odd-sized window a
        // fractional button origin shifts the nearest-neighbour sprite sampling
        // by a sub-texel, which can make the 1px border render unevenly or catch
        // a neighbouring texel at the top edge.
        const ui::UiRect snapped{std::floor(rectangle.x), std::floor(rectangle.y),
                                 std::floor(rectangle.width + 0.5F),
                                 std::floor(rectangle.height + 0.5F)};
        const GuiWidgetSprite face =
            state == ui::ButtonVisualState::Disabled
                ? GuiWidgetSprite::ButtonDisabled
                : (state == ui::ButtonVisualState::Normal ? GuiWidgetSprite::Button
                                                          : GuiWidgetSprite::ButtonHighlighted);
        // Pressed darkens the caller's tint instead of hard-coding grey, so the
        // red delete button keeps a coherent colour in every state.
        const glm::vec4 buttonTint =
            state == ui::ButtonVisualState::Pressed
                ? glm::vec4{tint.r * 0.78F, tint.g * 0.78F, tint.b * 0.78F, 1.0F}
                : tint;
        // Nine-sliced: the frame keeps its declared pixel width whatever the
        // layout does to the button, so a two-column settings page no longer
        // stretches a 200px bitmap into a soft-edged rectangle.
        drawScaledGuiSprite(commandBuffer, snapped, 0.0F,
                            guiWidgetSprite(guiWidgetSprites, face), scale, buttonTint);
        const glm::vec4 textColor = state == ui::ButtonVisualState::Disabled
                                        ? glm::vec4{0.63F, 0.63F, 0.63F, 1.0F}
                                        : (state == ui::ButtonVisualState::Hovered ||
                                                   state == ui::ButtonVisualState::Pressed
                                               ? glm::vec4{1.0F, 1.0F, 0.63F, 1.0F}
                                               : glm::vec4{1.0F});
        const float textY =
            snapped.y + (6.0F + (state == ui::ButtonVisualState::Pressed ? 1.0F : 0.0F)) * scale;
        drawHudText(commandBuffer, label,
                    snapped.x + (snapped.width - hudTextWidth(label, scale)) * 0.5F, textY, scale,
                    textColor);
    }

    void drawMinecraftSlider(VkCommandBuffer commandBuffer, const ui::UiRect& rectangle,
                             std::string_view label, ui::ButtonVisualState state, float value,
                             float scale) const {
        const ui::UiRect snapped{std::floor(rectangle.x), std::floor(rectangle.y),
                                 std::floor(rectangle.width + 0.5F),
                                 std::floor(rectangle.height + 0.5F)};
        const glm::vec4 tint = state == ui::ButtonVisualState::Pressed
                                   ? glm::vec4{0.78F, 0.78F, 0.78F, 1.0F}
                                   : glm::vec4{1.0F};
        // The track is nine-sliced like a button; the handle is drawn at its
        // native 8x20, where nine-slicing is the identity, so it keeps landing
        // exactly on its own art.
        drawScaledGuiSprite(commandBuffer, snapped, 0.0F,
                            guiWidgetSprite(guiWidgetSprites, GuiWidgetSprite::Slider), scale,
                            tint);
        const float clampedValue = std::clamp(value, 0.0F, 1.0F);
        const float knobX = snapped.x + clampedValue * std::max(snapped.width - 8.0F * scale, 0.0F);
        const auto& knob = guiWidgetSprite(guiWidgetSprites,
                                           state == ui::ButtonVisualState::Normal
                                               ? GuiWidgetSprite::SliderHandle
                                               : GuiWidgetSprite::SliderHandleHighlighted);
        drawScaledGuiSprite(commandBuffer, {knobX, snapped.y, 8.0F * scale, snapped.height}, 0.0F,
                            knob, scale);
        const glm::vec4 textColor = state == ui::ButtonVisualState::Disabled
                                        ? glm::vec4{0.63F, 0.63F, 0.63F, 1.0F}
                                        : (state == ui::ButtonVisualState::Hovered ||
                                                   state == ui::ButtonVisualState::Pressed
                                               ? glm::vec4{1.0F, 1.0F, 0.63F, 1.0F}
                                               : glm::vec4{1.0F});
        const float textY =
            snapped.y + (6.0F + (state == ui::ButtonVisualState::Pressed ? 1.0F : 0.0F)) * scale;
        drawHudText(commandBuffer, label,
                    snapped.x + (snapped.width - hudTextWidth(label, scale)) * 0.5F, textY, scale,
                    textColor);
    }

    [[nodiscard]] float hudTextWidth(std::string_view text, float scale) const {
        return textFont.textWidth(text, scale);
    }

    void drawHudText(VkCommandBuffer commandBuffer, std::string_view text, float x, float y,
                     float scale, const glm::vec4& color, bool shadow = true) const {
        float cursorX = x;
        for (const char32_t codepoint : ui::decodeUtf8(text)) {
            const auto metrics = textFont.glyph(codepoint);
            const glm::vec4 uv{
                metrics.u,
                metrics.v,
                metrics.uvWidth,
                metrics.uvHeight,
            };
            const ui::UiRect glyph{
                cursorX + metrics.offsetX * scale,
                y + metrics.offsetY * scale,
                metrics.pixelWidth * scale,
                metrics.pixelHeight * scale,
            };
            if (metrics.visible && shadow) {
                auto shadowColor = color;
                shadowColor.r *= 0.18F;
                shadowColor.g *= 0.18F;
                shadowColor.b *= 0.18F;
                drawHudQuad(commandBuffer,
                            {glyph.x + scale, glyph.y + scale, glyph.width, glyph.height},
                            shadowColor, metrics.layer, false, uv, true);
            }
            if (metrics.visible) {
                drawHudQuad(commandBuffer, glyph, color, metrics.layer, false, uv, true);
            }
            cursorX += metrics.advance * scale;
        }
    }

    [[nodiscard]] std::string_view itemDisplayName(const gameplay::ItemStack& stack) const {
        const gameplay::DescriptionId descriptionId = gameplay::itemDescriptionId(stack);
        if (descriptionId.empty()) return {};
        return language.translate(descriptionId.prefix(), descriptionId.source.space,
                                  descriptionId.source.path, descriptionId.source.path);
    }

    // blockTextureRoot is <vanilla>/1.16.1/textures/minecraft/block; the
    // localization files live three parents up under localization/minecraft.

    void drawDurabilityBar(VkCommandBuffer commandBuffer, const ui::UiRect& icon,
                           const gameplay::ItemStack& stack) const {
        const std::uint16_t maximumDamage = gameplay::itemMaximumDamage(stack);
        if (maximumDamage == 0U || stack.damage == 0U) {
            return;
        }
        const float spent = static_cast<float>(stack.damage) / static_cast<float>(maximumDamage);
        const float unit = icon.width / 16.0F;
        const float remainingWidth = std::round(13.0F * (1.0F - spent));
        drawHudQuad(commandBuffer,
                    {icon.x + 2.0F * unit, icon.y + 13.0F * unit, 13.0F * unit, 2.0F * unit},
                    {0.0F, 0.0F, 0.0F, 1.0F});
        // MathHelper.hsvToRgb((1 - spent) / 3, 1, 1) with full saturation and
        // value, which only ever mixes red and green.
        const float hue = (1.0F - spent) / 3.0F * 6.0F;
        const glm::vec4 color = hue < 1.0F
                                    ? glm::vec4{1.0F, hue, 0.0F, 1.0F}
                                    : glm::vec4{std::max(2.0F - hue, 0.0F), 1.0F, 0.0F, 1.0F};
        drawHudQuad(commandBuffer,
                    {icon.x + 2.0F * unit, icon.y + 13.0F * unit, remainingWidth * unit, unit},
                    color);
    }

    void drawHudSlot(VkCommandBuffer commandBuffer, const ui::UiRect& rectangle,
                     const gameplay::ItemStack& stack, bool selected, bool hovered = false,
                     bool minecraftStyle = false) const {
        if (!minecraftStyle) {
            const float border = selected ? 3.0F : 2.0F;
            drawHudQuad(commandBuffer, rectangle,
                        selected ? glm::vec4{0.96F, 0.82F, 0.28F, 0.96F}
                                 : glm::vec4{0.08F, 0.08F, 0.10F, 0.88F});
            const ui::UiRect inner{
                rectangle.x + border,
                rectangle.y + border,
                rectangle.width - border * 2.0F,
                rectangle.height - border * 2.0F,
            };
            drawHudQuad(commandBuffer, inner, {0.24F, 0.24F, 0.27F, 0.90F});
        }
        if (hovered) {
            drawHudQuad(commandBuffer, rectangle, {1.0F, 1.0F, 1.0F, 0.34F});
        }
        if (!stack.empty()) {
            const float iconInset = minecraftStyle ? 0.0F : rectangle.width * 0.16F;
            const ui::UiRect icon{
                rectangle.x + iconInset,
                rectangle.y + iconInset,
                rectangle.width - iconInset * 2.0F,
                rectangle.height - iconInset * 2.0F,
            };
            drawHudItemIcon(commandBuffer, icon, stack);
            drawDurabilityBar(commandBuffer, icon, stack);
            if (stack.count > 1U) {
                const std::string count = std::to_string(stack.count);
                const float textScale =
                    minecraftStyle ? rectangle.width / 16.0F : rectangle.width / 40.0F;
                drawHudText(commandBuffer, count,
                            rectangle.x + 17.0F * textScale - hudTextWidth(count, textScale),
                            rectangle.y + 9.0F * textScale, textScale, {1.0F, 1.0F, 1.0F, 1.0F});
            }
        }
    }

    [[nodiscard]] std::string rainModeLabel(int mode) const {
        switch (mode) {
        case 0:
            return translated("options.rebedrock.rainMode.texture", "Texture Rain");
        case 1:
            return translated("options.rebedrock.rainMode.particles", "Particle Rain");
        default:
            return translated("options.rebedrock.rainMode.async", "Asynchronous Particle Rain");
        }
    }

    // The particle-level multiplier (粒子效果): 低 0.5x / 中 1.0x (the default) /
    // 高 2x / 疯狂 3x. Scales the rain-drop budget and the particle system.

    [[nodiscard]] std::string particleLevelLabel(int level) const {
        switch (level) {
        case 0:
            return translated("options.rebedrock.particleLevel.low", "Low (0.5x)");
        case 2:
            return translated("options.rebedrock.particleLevel.high", "High (2x)");
        case 3:
            return translated("options.rebedrock.particleLevel.crazy", "Crazy (3x)");
        default:
            return translated("options.rebedrock.particleLevel.medium", "Medium (1x)");
        }
    }

    // Applies the option to the live particle system (the spawn-count and
    // live-cap scaling); the rain budget reads options.particleLevel directly.

    // PX-4: the localized, value-formatted label for a widget id. Wired into the
    // draw page as MenuBuildContext.labelFor, so a widget carries its own text and
    // the draw backend never re-derives it. Keyed on ui::WidgetId (the stable id);
    // the old MenuButton enum is gone.
    [[nodiscard]] std::string widgetLabel(ui::WidgetId button) const {
        // Every label carries its English text as the fallback, so a language
        // without the vanilla key still reads correctly.
        const auto toggle = [this](bool value) {
            return translated(value ? "options.on" : "options.off", value ? "ON" : "OFF");
        };
        const auto optionValue = [this](std::string name, std::string value) {
            const std::array<std::string_view, 2> arguments{name, value};
            return ui::formatTranslation(
                translated("options.generic_value", "%s: %s"), arguments);
        };
        const auto percentValue = [this](std::string name, int value) {
            const std::string number = std::to_string(value);
            const std::array<std::string_view, 2> arguments{name, number};
            return ui::formatTranslation(
                translated("options.percent_value", "%s: %s%%"), arguments);
        };
        switch (button) {
        case ui::WidgetId::Resume:
            return translated("menu.returnToGame", "Back to Game");
        case ui::WidgetId::Options:
            return translated("menu.options", "Options...");
        case ui::WidgetId::Resolution: {
            // The label shows the live window size so a maximized or manually
            // resized window reads correctly instead of echoing the last preset.
            const auto resolution = ui::kDisplayResolutions[menuSystem.resolutionIndex];
            int windowWidth = 0;
            int windowHeight = 0;
            glfwGetWindowSize(window, &windowWidth, &windowHeight);
            std::string value = std::to_string(windowWidth) + "x" + std::to_string(windowHeight);
            if (windowWidth == resolution.width && windowHeight == resolution.height) {
                return optionValue(
                    translated("options.fullscreen.resolution", "Fullscreen Resolution"), value);
            }
            value += " (" +
                     translated("options.rebedrock.resolution.windowed", "windowed") + ")";
            return optionValue(
                translated("options.fullscreen.resolution", "Fullscreen Resolution"), value);
        }
        case ui::WidgetId::GuiScale:
            return optionValue(
                translated("options.guiScale", "GUI Scale"),
                menuSystem.guiScaleSetting == 0 ? translated("options.guiScale.auto", "Auto")
                                                : std::to_string(menuSystem.guiScaleSetting));
        case ui::WidgetId::ViewDistance:
            return optionValue(
                translated("options.renderDistance", "Render Distance"),
                formatTemplate(translated("options.chunks", "%s chunks"),
                               std::to_string(viewDistanceChunks)));
        case ui::WidgetId::SimulationDistance:
            return optionValue(
                translated("options.simulationDistance", "Simulation Distance"),
                formatTemplate(translated("options.chunks", "%s chunks"),
                               std::to_string(simulationDistanceChunks)));
        case ui::WidgetId::MasterVolume:
            return percentValue(
                translated("soundCategory.master", "Master Volume"),
                static_cast<int>(std::lround(options.masterVolume * 100.0F)));
        case ui::WidgetId::VideoSettings:
            return translated("options.video", "Video Settings...");
        case ui::WidgetId::Controls:
            return translated("options.controls", "Controls...");
        case ui::WidgetId::AutoJump:
            return optionValue(translated("options.autoJump", "Auto-Jump"),
                               toggle(options.autoJump));
        case ui::WidgetId::FrameRateLimit:
            return optionValue(
                translated("options.framerateLimit", "Max Framerate"),
                options.frameRateLimit == 0
                    ? translated("options.framerateLimit.max", "Unlimited")
                    : formatTemplate(translated("options.framerate", "%s fps"),
                                     std::to_string(options.frameRateLimit)));
        case ui::WidgetId::AntiAliasing:
            return optionValue(
                translated("options.rebedrock.antiAliasing", "Anti-Aliasing"),
                toggle(options.antiAliasing));
        case ui::WidgetId::Anisotropy:
            return optionValue(translated("options.maxAnisotropy", "Anisotropic Filtering"),
                               std::to_string(options.anisotropy) + "x");
        case ui::WidgetId::ViewBobbing:
            return optionValue(translated("options.viewBobbing", "View Bobbing"),
                               toggle(options.viewBobbing));
        case ui::WidgetId::SmoothLighting:
            switch (options.smoothLightingQuality) {
            case world::SmoothLightingQuality::Off:
                return optionValue(translated("options.ao", "Smooth Lighting"),
                                   translated("options.ao.off", "OFF"));
            case world::SmoothLightingQuality::High:
                return optionValue(translated("options.ao", "Smooth Lighting"),
                                   translated("options.ao.max", "Maximum"));
            case world::SmoothLightingQuality::Standard:
                return optionValue(translated("options.ao", "Smooth Lighting"),
                                   translated("options.ao.min", "Minimum"));
            }
            return {};
        case ui::WidgetId::DynamicLight:
            return optionValue(
                translated("options.rebedrock.dynamicLights", "Dynamic Lighting"),
                toggle(options.dynamicLight));
        case ui::WidgetId::Vsync:
            return optionValue(translated("options.vsync", "VSync"), toggle(options.vsync));
        case ui::WidgetId::Difficulty:
            // Only present on the in-world options page, where a save is open.
            return optionValue(
                translated("options.difficulty", "Difficulty"),
                translated(gameplay::difficultyTranslationKey(
                               currentSave.has_value() ? currentSave->difficulty
                                                       : gameplay::Difficulty::Normal),
                           gameplay::difficultyName(currentSave.has_value()
                                                        ? currentSave->difficulty
                                                        : gameplay::Difficulty::Normal)));
        case ui::WidgetId::Experimental:
            return translated("selectWorld.experimental", "Experimental") + "...";
        case ui::WidgetId::RainMode:
            return optionValue(translated("options.rebedrock.rainMode", "Rain Mode"),
                               rainModeLabel(options.rainMode));
        case ui::WidgetId::ParticleLevel:
            return optionValue(translated("options.particles", "Particles"),
                               particleLevelLabel(options.particleLevel));
        case ui::WidgetId::SunShadows:
            return optionValue(translated("options.rebedrock.sunShadows", "Sun Shadows"),
                               toggle(options.sunShadows));
        case ui::WidgetId::RainCollisionCache:
            return optionValue(
                translated("options.rebedrock.rainCollisionCache", "Rain Collision Cache"),
                toggle(options.rainCollisionCache));
        case ui::WidgetId::Language:
            return translated("options.language", "Language...");
        case ui::WidgetId::ForceUnicodeFont:
            return optionValue(translated("options.forceUnicodeFont", "Force Unicode Font"),
                               toggle(options.forceUnicodeFont));
        case ui::WidgetId::Subtitles:
            // PX-6 Bug3: the sound-subtitles accessibility toggle.
            return optionValue(translated("options.showSubtitles", "Show Subtitles"),
                               toggle(options.showSubtitles));
        case ui::WidgetId::Done:
            return translated("gui.done", "Done");
        case ui::WidgetId::Singleplayer:
            return translated("menu.singleplayer", "Singleplayer");
        case ui::WidgetId::Exit:
            return translated("menu.quit", "Quit Game");
        case ui::WidgetId::PlaySelected:
            return translated("selectWorld.select", "Play Selected World");
        case ui::WidgetId::CreateWorld:
            return translated("selectWorld.create", "Create New World");
        case ui::WidgetId::Edit:
            return translated("selectWorld.edit", "Edit");
        case ui::WidgetId::SaveRename:
            return translated("gui.done", "Done");
        case ui::WidgetId::DeleteWorld:
            return translated("selectWorld.delete", "Delete");
        case ui::WidgetId::DeleteConfirm:
            return translated("selectWorld.deleteButton", "Delete");
        case ui::WidgetId::DeleteCancel:
            return translated("gui.cancel", "Cancel");
        case ui::WidgetId::Back:
            return translated("gui.back", "Back");
        case ui::WidgetId::CreateConfirm:
            return translated("selectWorld.create", "Create World");
        case ui::WidgetId::CreateGameMode:
            return optionValue(translated("selectWorld.gameMode", "Game Mode"),
                               gameModeLabel(menuSystem.createWorldGameMode));
        case ui::WidgetId::CreateAllowCommands:
            return optionValue(translated("selectWorld.allowCommands", "Allow Cheats"),
                               toggle(menuSystem.createWorldAllowCommands));
        case ui::WidgetId::SaveQuit:
            return translated("menu.returnToMenu", "Save and Quit to Title");
        case ui::WidgetId::Respawn:
            return translated("deathScreen.respawn", "Respawn");
        case ui::WidgetId::TitleScreen:
            return translated("deathScreen.titleScreen", "Title Screen");
        case ui::WidgetId::ResetKeyBinds:
            return translated("controls.resetAll", "Reset Keys");
        case ui::WidgetId::None:
        case ui::WidgetId::WorldRow:
        case ui::WidgetId::LanguageRow:
        case ui::WidgetId::KeyBindRow:  // per-action label comes from keyBindLabelFor
            return {};
        }
        return {};
    }

    [[nodiscard]] std::string gameModeLabel(gameplay::GameMode mode) const {
        return mode == gameplay::GameMode::Survival
                   ? translated("selectWorld.gameMode.survival", "survival")
                   : translated("selectWorld.gameMode.creative", "creative");
    }

    // Formats the single-argument vanilla strings used outside option fields.
    [[nodiscard]] static std::string formatTemplate(std::string text, std::string_view value) {
        const std::array<std::string_view, 1> arguments{value};
        return ui::formatTranslation(text, arguments);
    }

    // Frontend screen titles. The edit page shows the selected world's name,
    // matching 1.16.1's Edit World screen; the delete confirmation uses the
    // vanilla delete question as its heading.
    [[nodiscard]] std::string frontendTitle(ui::PageId page) const {
        if (page == ui::PageId::Title)
            return "MC Rebedrock";
        if (page == ui::PageId::WorldList)
            return translated("menu.singleplayer", "Singleplayer");
        if (page == ui::PageId::CreateWorld)
            return translated("selectWorld.create", "Create New World");
        if (page == ui::PageId::ConfirmDelete)
            return translated("selectWorld.deleteQuestion", "Delete World?");
        if (menuSystem.selectedWorldIndex < menuSystem.saveSummaries.size())
            return menuSystem.saveSummaries[menuSystem.selectedWorldIndex].displayName;
        return translated("selectWorld.edit", "Edit World");
    }

    void drawWorldNameField(VkCommandBuffer commandBuffer, const ui::HudLayout& layout,
                            const std::string& value) const {
        const float scale = layout.scale();
        const float width = 200.0F * scale;
        const ui::UiRect field{(static_cast<float>(swapchainExtent.width) - width) * 0.5F,
                               static_cast<float>(swapchainExtent.height) * 0.5F - 58.0F * scale,
                               width, 20.0F * scale};
        drawHudText(commandBuffer, translated("selectWorld.enterName", "World Name"), field.x,
                    field.y - 12.0F * scale, scale, {0.85F, 0.85F, 0.85F, 1.0F});
        drawHudQuad(commandBuffer, field, {0.02F, 0.02F, 0.02F, 0.95F});
        drawHudQuad(commandBuffer,
                    {field.x + scale, field.y + scale, field.width - 2.0F * scale,
                     field.height - 2.0F * scale},
                    {0.12F, 0.12F, 0.12F, 0.98F});
        drawHudText(commandBuffer, value, field.x + 4.0F * scale, field.y + 6.0F * scale, scale,
                    {1.0F, 1.0F, 1.0F, 1.0F});
    }

    // 26.1's menu background starts with an 85-degree perspective view from
    // inside the panorama cube. The camera slowly turns — a full 360° yaw over
    // kCycleSeconds so every one of the six faces gets a long turn in front of
    // the view, a gentle pitch sweep dips down to panorama_4 and up to
    // panorama_5, and a faint vanilla-style sine sway keeps it from feeling
    // mechanical. The dark quad afterwards keeps the white title and the menu
    // buttons readable over the scene.
    void drawTitleCarousel(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet,
                           bool blurred, float guiScale) const {
        // One full 360° yaw turn every five minutes, so each of the four side
        // faces stays centred for well over a minute. The pitch sweeps once per
        // turn and the vanilla-style sway rate matches the slower pace.
        constexpr double kCycleSeconds = 300.0;
        constexpr double kPi = 3.14159265358979323846;
        const double progress = uiTimeSeconds / kCycleSeconds;
        // Full 360° turn plus a small sine sway; pitch oscillates once per turn.
        const float yaw = static_cast<float>(progress * 2.0 * kPi) +
                          static_cast<float>(std::sin(uiTimeSeconds * 0.024) * 0.04);
        const float pitch = static_cast<float>(std::sin(progress * 2.0 * kPi)) * 0.44F;
        const float tanHalfFov = std::tan(static_cast<float>(85.0 * kPi / 180.0) * 0.5F);
        const float aspect =
            static_cast<float>(swapchainExtent.width) / static_cast<float>(swapchainExtent.height);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, panoramaPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                panoramaPipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
        // PX-5: the menu background blur radius, in framebuffer pixels. 26.1's
        // Screen.renderBlurredBackground drives its box blur from the
        // `menuBackgroundBlurriness` option (default 0.5) scaled to a radius; the
        // shipped default lands at a radius of 5. Kept as this named tunable so
        // the mac visual pass can match vanilla by adjusting one value (the exact
        // radius/kernel is a shader-visual call that cannot be judged headless).
        constexpr float kMenuBlurRadius = 5.0F;
        const PanoramaPush push{{yaw, pitch, tanHalfFov, aspect},
                                {blurred ? kMenuBlurRadius : 0.0F, 0.0F, 0.0F, 0.0F}};
        vkCmdPushConstants(commandBuffer, panoramaPipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipelineLayout,
                                0, 1, &descriptorSet, 0, nullptr);
        const ui::UiRect fullScreen{0.0F, 0.0F, static_cast<float>(swapchainExtent.width),
                                    static_cast<float>(swapchainExtent.height)};
        if (blurred) {
            // Screen.extractMenuBackground follows the blur in 26.1. Keep this
            // as the real pack-provided texture instead of baking its current
            // translucent-black pixels into code.
            drawGuiSprite(commandBuffer, fullScreen, 9.0F,
                          ui::tiledBackgroundSource(fullScreen.width, fullScreen.height, guiScale));
        } else {
            drawHudQuad(commandBuffer, fullScreen, {0.0F, 0.0F, 0.0F, 0.30F});
        }
    }

    // PX-4: the generic menu draw backend — paint one page's widgets by kind.
    // Replaced the three near-identical per-page button loops (which each called
    // menuButtonForIndex + menuButtonLabel). Each widget carries its own rect,
    // label, enabled and (Slider) display value; the pressed highlight matches the
    // widget whose id equals pressedMenuButton, and DeleteConfirm keeps its red
    // tint. List rows are painted by the dedicated list path, not here.
    void drawMenuWidgets(VkCommandBuffer commandBuffer, const ui::Page& widgets,
                         float scale) const {
        const auto cursor = currentFramebufferCursor();
        for (const auto& widget : widgets) {
            // PX-6 Bug1: the Controls key-bind rows are ListRow widgets — the old
            // draw skipped every non-Button/Slider kind, so the whole middle band
            // was invisible. Draw a ListRow as a hover-highlighted "Action: Key"
            // row (vanilla's EntryList look), not a full button frame.
            if (widget.kind == ui::WidgetKind::ListRow) {
                drawKeyBindRow(commandBuffer, widget, cursor.x, cursor.y, scale);
                continue;
            }
            if (widget.kind != ui::WidgetKind::Button &&
                widget.kind != ui::WidgetKind::Slider) {
                continue;
            }
            const bool pressed =
                static_cast<ui::WidgetId>(widget.debugId) == pressedMenuButton;
            const auto state = ui::buttonVisualState(widget.rect, cursor.x, cursor.y,
                                                     widget.enabled, pressed);
            if (widget.kind == ui::WidgetKind::Slider) {
                const float value = widget.slider.value ? widget.slider.value() : 0.0F;
                drawMinecraftSlider(commandBuffer, widget.rect, widget.label, state, value,
                                    scale);
            } else {
                const glm::vec4 tint =
                    static_cast<ui::WidgetId>(widget.debugId) == ui::WidgetId::DeleteConfirm
                        ? glm::vec4{0.72F, 0.22F, 0.22F, 1.0F}
                        : glm::vec4{1.0F};
                drawMinecraftButton(commandBuffer, widget.rect, widget.label, state, scale,
                                    tint);
            }
        }
    }

    // PX-6 Bug1: one Controls key-bind row — the "Action: Key" label on a subtle
    // row background that brightens on hover, matching vanilla's key-bind
    // EntryList (the row is clickable to begin the rebind capture).
    void drawKeyBindRow(VkCommandBuffer commandBuffer, const ui::Widget& widget, float cursorX,
                        float cursorY, float scale) const {
        const bool hovered = widget.rect.contains(cursorX, cursorY);
        drawHudQuad(commandBuffer, widget.rect,
                    hovered ? glm::vec4{0.28F, 0.28F, 0.32F, 0.9F}
                            : glm::vec4{0.0F, 0.0F, 0.0F, 0.55F});
        drawHudText(commandBuffer, widget.label, widget.rect.x + 4.0F * scale,
                    widget.rect.y + 1.5F * scale, scale, {1.0F, 1.0F, 1.0F, 1.0F}, false);
    }

    // PX-6 Bug1: the Controls key-bind list scrollbar. Only drawn when there are
    // more actions than the visible window; the thumb spans the visible fraction
    // and slides with the scroll offset (the world/language lists do the same).
    void drawControlsScrollbar(VkCommandBuffer commandBuffer, const ui::HudLayout& layout) const {
        const float fbWidth = static_cast<float>(swapchainExtent.width);
        const std::size_t total = input::keyBindRows().size();
        const std::size_t visible = ui::controlsVisibleRowCount(
            fbWidth, static_cast<float>(swapchainExtent.height), menuSystem.guiScaleSetting);
        if (total <= visible) {
            return;  // everything fits; no scrollbar
        }
        const auto track = ui::controlsScrollbarTrack(layout, fbWidth);
        drawHudQuad(commandBuffer, track, {0.0F, 0.0F, 0.0F, 0.6F});
        const std::size_t maximumFirst = total - visible;
        const std::size_t first = std::min(menuSystem.controlsListFirstIndex, maximumFirst);
        const float thumbHeight =
            std::max(track.height * static_cast<float>(visible) / static_cast<float>(total),
                     layout.scale() * 6.0F);
        const float travel = std::max(track.height - thumbHeight, 1.0F);
        const float thumbY = track.y + travel * static_cast<float>(first) /
                                           static_cast<float>(maximumFirst);
        drawHudQuad(commandBuffer, {track.x, thumbY, track.width, thumbHeight},
                    {0.55F, 0.55F, 0.55F, 1.0F});
    }

    void drawFrontend(VkCommandBuffer commandBuffer, const ui::HudLayout& layout,
                      VkDescriptorSet descriptorSet) const {
        const auto page = menuSystem.pageStack.current();
        // Like 26.1 Screen.extractBackground(): every no-world screen keeps the
        // rotating panorama alive. Secondary screens blur only that background;
        // their text, buttons and list rows are emitted afterwards and stay sharp.
        drawTitleCarousel(commandBuffer, descriptorSet, page != ui::PageId::Title, layout.scale());
        const float scale = layout.scale();
        const std::string title = frontendTitle(page);
        drawHudText(commandBuffer, title,
                    (static_cast<float>(swapchainExtent.width) -
                     hudTextWidth(title, scale * (page == ui::PageId::Title ? 2.0F : 1.0F))) *
                        0.5F,
                    14.0F * scale, scale * (page == ui::PageId::Title ? 2.0F : 1.0F),
                    {1.0F, 1.0F, 1.0F, 1.0F});

        if (page == ui::PageId::WorldList) {
            const std::size_t visibleRows = saveListVisibleRowCount();
            const std::size_t maximumFirst = menuSystem.saveSummaries.size() > visibleRows
                                                 ? menuSystem.saveSummaries.size() - visibleRows
                                                 : 0U;
            const std::size_t first = std::min(menuSystem.worldListFirstIndex, maximumFirst);
            const std::size_t remaining =
                menuSystem.saveSummaries.size() - std::min(first, menuSystem.saveSummaries.size());
            const std::size_t visible = std::min(remaining, visibleRows);
            // The 26.1 list background is independently pack-overridable from
            // the surrounding menu background.
            const auto firstRow = worldListRow(0, layout);
            const float listBandHeight =
                static_cast<float>(visibleRows) * 22.0F * scale + 8.0F * scale;
            drawGuiSprite(commandBuffer,
                          {0.0F, firstRow.y - 4.0F * scale,
                           static_cast<float>(swapchainExtent.width), listBandHeight},
                          kMenuListBackgroundGuiLayer,
                          ui::tiledBackgroundSource(static_cast<float>(swapchainExtent.width),
                                                    listBandHeight, scale),
                          {32.0F / 255.0F, 32.0F / 255.0F, 32.0F / 255.0F, 1.0F});
            if (visible == 0U) {
                const std::string_view empty = "No worlds yet. Create one to begin.";
                drawHudText(
                    commandBuffer, empty,
                    (static_cast<float>(swapchainExtent.width) - hudTextWidth(empty, scale)) * 0.5F,
                    34.0F * scale, scale, {0.85F, 0.85F, 0.85F, 1.0F});
            }
            for (std::size_t visibleIndex = 0; visibleIndex < visible; ++visibleIndex) {
                const std::size_t index = first + visibleIndex;
                const auto rectangle = worldListRow(visibleIndex, layout);
                const bool selected = index == menuSystem.selectedWorldIndex;
                drawHudQuad(commandBuffer, rectangle,
                            selected ? glm::vec4{0.95F, 0.95F, 0.95F, 0.95F}
                                     : glm::vec4{0.10F, 0.10F, 0.10F, 0.90F});
                drawHudQuad(commandBuffer,
                            {rectangle.x + scale, rectangle.y + scale,
                             rectangle.width - 2.0F * scale, rectangle.height - 2.0F * scale},
                            selected ? glm::vec4{0.28F, 0.28F, 0.28F, 0.96F}
                                     : glm::vec4{0.18F, 0.18F, 0.18F, 0.96F});
                drawHudText(commandBuffer, menuSystem.saveSummaries[index].displayName,
                            rectangle.x + 4.0F * scale, rectangle.y + 2.0F * scale, scale,
                            {1.0F, 1.0F, 1.0F, 1.0F});
                const std::string details =
                    "Seed " + std::to_string(menuSystem.saveSummaries[index].seed);
                drawHudText(commandBuffer, details, rectangle.x + 4.0F * scale,
                            rectangle.y + 11.0F * scale, scale * 0.75F,
                            {0.70F, 0.70F, 0.70F, 1.0F});
            }
        } else if (page == ui::PageId::CreateWorld) {
            const std::string value = menuSystem.createWorldName +
                                      (static_cast<int>(uiTimeSeconds * 2.0) % 2 == 0 ? "_" : "");
            drawWorldNameField(commandBuffer, layout, value);
        } else if (page == ui::PageId::EditWorld) {
            const std::string value = menuSystem.editWorldName +
                                      (static_cast<int>(uiTimeSeconds * 2.0) % 2 == 0 ? "_" : "");
            drawWorldNameField(commandBuffer, layout, value);
        } else if (page == ui::PageId::ConfirmDelete) {
            const std::string worldName =
                menuSystem.selectedWorldIndex < menuSystem.saveSummaries.size()
                    ? menuSystem.saveSummaries[menuSystem.selectedWorldIndex].displayName
                    : std::string{};
            const std::string warning = formatTemplate(
                translated("selectWorld.deleteWarning", "\"%s\" will be permanently lost!"),
                worldName);
            const float warningY =
                static_cast<float>(swapchainExtent.height) * 0.5F - 20.0F * scale;
            drawHudText(commandBuffer, warning,
                        (static_cast<float>(swapchainExtent.width) - hudTextWidth(warning, scale)) *
                            0.5F,
                        warningY, scale, {1.0F, 1.0F, 1.0F, 1.0F});
        }

        drawMenuWidgets(commandBuffer, buildDrawPage(), scale);
        if (!menuSystem.saveStatus.empty()) {
            drawHudText(commandBuffer, menuSystem.saveStatus, 4.0F * scale,
                        static_cast<float>(swapchainExtent.height) - 12.0F * scale, scale,
                        {1.0F, 0.75F, 0.35F, 1.0F});
        }
    }

    // Gui#renderPlayerHealth: hearts on the left of the hotbar, hunger on the
    // right, and the air row above the hunger row while submerged.
    void drawSurvivalStatusBars(VkCommandBuffer commandBuffer, const ui::HudLayout& layout) const {
        const float scale = layout.scale();
        const auto hotbar = layout.hotbarBackground();
        const float left = hotbar.x;
        const float right = hotbar.x + hotbar.width;
        const float top = hotbar.y - 17.0F * scale;
        const float icon = 9.0F * scale;
        const float step = 8.0F * scale;
        const auto iconRect = [&](float x, float y) { return ui::UiRect{x, y, icon, icon}; };
        // A recent hit swaps the empty heart for the blinking white container.
        const bool flashing = uiFrameData_.ticksSinceDamage < 10;
        const int health = static_cast<int>(std::ceil(uiFrameData_.health));
        for (int index = 9; index >= 0; --index) {
            const auto rectangle = iconRect(left + static_cast<float>(index) * step, top);
            drawGuiSprite(commandBuffer, rectangle, 1.0F,
                          {flashing ? 25.0F : 16.0F, 0.0F, 9.0F, 9.0F});
            if (index * 2 + 1 < health) {
                drawGuiSprite(commandBuffer, rectangle, 1.0F, {52.0F, 0.0F, 9.0F, 9.0F});
            } else if (index * 2 + 1 == health) {
                drawGuiSprite(commandBuffer, rectangle, 1.0F, {61.0F, 0.0F, 9.0F, 9.0F});
            }
        }
        const int food = uiFrameData_.foodLevel;
        for (int index = 0; index < 10; ++index) {
            const auto rectangle = iconRect(right - static_cast<float>(index) * step - icon, top);
            drawGuiSprite(commandBuffer, rectangle, 1.0F, {16.0F, 27.0F, 9.0F, 9.0F});
            if (index * 2 + 1 < food) {
                drawGuiSprite(commandBuffer, rectangle, 1.0F, {52.0F, 27.0F, 9.0F, 9.0F});
            } else if (index * 2 + 1 == food) {
                drawGuiSprite(commandBuffer, rectangle, 1.0F, {61.0F, 27.0F, 9.0F, 9.0F});
            }
        }
        if (cameraSubmergedInWater()) {
            constexpr float maximumAir =
                static_cast<float>(gameplay::PlayerVitals::kMaximumAirTicks);
            const float air =
                std::clamp(static_cast<float>(uiFrameData_.airTicks), 0.0F, maximumAir);
            const int full = static_cast<int>(std::ceil((air - 2.0F) * 10.0F / maximumAir));
            const int partial = static_cast<int>(std::ceil(air * 10.0F / maximumAir)) - full;
            for (int index = 0; index < full + partial; ++index) {
                drawGuiSprite(
                    commandBuffer,
                    iconRect(right - static_cast<float>(index) * step - icon, top - 10.0F * scale),
                    1.0F, {index < full ? 16.0F : 25.0F, 18.0F, 9.0F, 9.0F});
            }
        }
    }

    // Gui#renderExperienceBar: the 182x5 bar centred over the hotbar, seven
    // logical pixels above it, filled by the player's real progress fraction
    // (XP-0: uiFrameData_ carries it from the tick snapshot, ContextualBarRenderer's
    // extractExperienceLevel green level number drawn just above it).
    void drawExperienceBar(VkCommandBuffer commandBuffer, const ui::HudLayout& layout) const {
        const float scale = layout.scale();
        const auto bar = layout.experienceBar();
        // 26.1's named experience-bar background, then its green progress sprite.
        drawGuiSprite(commandBuffer, bar, 1.0F, {0.0F, 64.0F, 182.0F, 5.0F});
        const float progress = std::clamp(uiFrameData_.experienceProgress, 0.0F, 1.0F);
        if (progress > 0.0F) {
            // A partial fill samples only the leading columns of the sprite,
            // the way vanilla's blit(x, y, 0, 69, progressWidth, 5) does.
            const float filledWidth = progress * 182.0F * scale;
            drawGuiSprite(commandBuffer, {bar.x, bar.y, filledWidth, bar.height}, 1.0F,
                          {0.0F, 69.0F, progress * 182.0F, 5.0F});
        }
        // ContextualBarRenderer#extractExperienceLevel: the level number is
        // only drawn once the player has actually left level 0 (26.1 gates on
        // `hasExperience() && experienceLevel > 0`), so a fresh survival spawn
        // shows an empty bar with no "0" floating above it.
        if (uiFrameData_.experienceLevel > 0) {
            const std::string label = std::to_string(uiFrameData_.experienceLevel);
            const float textWidth = hudTextWidth(label, scale);
            const float textX = bar.x + (bar.width - textWidth) * 0.5F;
            // 26.1's y = guiHeight - 24 - 9 - 2, six logical pixels above the
            // bar's own top (guiHeight - 24 - 5); expressed relative to `bar`
            // so it tracks the same anchor HudLayout::experienceBar() uses.
            const float textY = bar.y - 6.0F * scale;
            // Vanilla draws the level number with a four-direction black
            // outline (not the usual single offset drop shadow) before the
            // green fill, so it reads over both the empty and filled bar.
            constexpr glm::vec4 kOutline{0.0F, 0.0F, 0.0F, 1.0F};
            drawHudText(commandBuffer, label, textX + scale, textY, scale, kOutline, false);
            drawHudText(commandBuffer, label, textX - scale, textY, scale, kOutline, false);
            drawHudText(commandBuffer, label, textX, textY + scale, scale, kOutline, false);
            drawHudText(commandBuffer, label, textX, textY - scale, scale, kOutline, false);
            constexpr glm::vec4 kLevelGreen{0.5019608F, 1.0F, 0.1254902F, 1.0F};
            drawHudText(commandBuffer, label, textX, textY, scale, kLevelGreen, false);
        }
    }

    // 1.16.1's InGameHud#updateVignetteDarkness: darkness eases toward
    // clamp(1 - brightnessAtEyes, 0, 1) at 1% per tick. Brightness is the
    // overworld light-level curve g/(4 - 3g) with g = light / 15. Sky light is
    // dimmed by the same daylight factor the sky shader uses, so the dark
    // corners also appear at night and in caves.
    void updateVignetteDarkness(float deltaSeconds) {
        const auto daylight =
            world::DayNightCycle::stateAtTick(clientMirror.world().dayTimeTicks);
        const float daylightFactor =
            std::clamp((daylight.skyBrightness - 0.08F) / 0.92F, 0.0F, 1.0F);
        const auto& playerSnap = clientMirror.player();
        const float eyeHeight = playerSnap.sneaking ? gameplay::PlayerController::kSneakingEyeHeight
                                                    : gameplay::PlayerController::kEyeHeight;
        const glm::vec3 eye = playerSnap.physicsCurrent + glm::vec3{0.0F, eyeHeight, 0.0F};
        const int eyeX = static_cast<int>(std::floor(eye.x));
        const int eyeY = static_cast<int>(std::floor(eye.y));
        const int eyeZ = static_cast<int>(std::floor(eye.z));
        const float sky = static_cast<float>(lightWorld.skyLight(eyeX, eyeY, eyeZ)) / 15.0F;
        const float block =
            static_cast<float>(lightWorld.blockLight(eyeX, eyeY, eyeZ)) / 15.0F;
        const float light = std::max(block, sky * daylightFactor);
        const float brightness = light / (4.0F - 3.0F * light);
        const float target = std::clamp(1.0F - brightness, 0.0F, 1.0F);
        vignetteDarkness_ += (target - vignetteDarkness_) * std::min(1.0F, 0.2F * deltaSeconds);
    }

    // ScreenEffectRenderer's damage tint, simplified to a full-screen red wash
    // that fades over the invulnerability window.
    void drawDamageOverlay(VkCommandBuffer commandBuffer) const {
        if (uiFrameData_.gameMode != gameplay::GameMode::Survival) {
            return;
        }
        constexpr int kFlashTicks = 10;
        if (uiFrameData_.ticksSinceDamage >= kFlashTicks) {
            return;
        }
        const float fade = 1.0F - static_cast<float>(uiFrameData_.ticksSinceDamage) /
                                      static_cast<float>(kFlashTicks);
        drawHudQuad(commandBuffer,
                    {0.0F, 0.0F, static_cast<float>(swapchainExtent.width),
                     static_cast<float>(swapchainExtent.height)},
                    {0.65F, 0.0F, 0.0F, 0.32F * fade});
    }

    // 26.1-style language screen: rows update a draft selection and Done
    // commits one asynchronous resource reload.
    void drawLanguageScreen(VkCommandBuffer commandBuffer, const ui::HudLayout& layout) const {
        const auto cursor = currentFramebufferCursor();
        const float scale = layout.scale();
        const std::string title = translated("options.language.title", "Language");
        drawHudText(commandBuffer, title,
                    (static_cast<float>(swapchainExtent.width) - hudTextWidth(title, scale)) * 0.5F,
                    14.0F * scale, scale, {1.0F, 1.0F, 1.0F, 1.0F});
        // The centred dark list box uses 26.1's independently replaceable list background.
        const auto box = languageListBox(layout);
        drawGuiSprite(commandBuffer, box, kMenuListBackgroundGuiLayer,
                      ui::tiledBackgroundSource(box.width, box.height, scale),
                      {32.0F / 255.0F, 32.0F / 255.0F, 32.0F / 255.0F, 1.0F});
        const std::size_t visible = languageVisibleRowCount();
        const std::size_t maximumFirst = menuSystem.languageCodes.size() > visible
                                             ? menuSystem.languageCodes.size() - visible
                                             : 0U;
        const std::size_t first = std::min(menuSystem.languageListFirstIndex, maximumFirst);
        for (std::size_t row = 0; row < visible; ++row) {
            const std::size_t index = first + row;
            if (index >= menuSystem.languageCodes.size()) {
                break;
            }
            const auto rectangle = languageRow(row, layout);
            const bool selected =
                menuSystem.languageCodes[index] == menuSystem.pendingLanguageCode;
            const bool hovered = rectangle.contains(cursor.x, cursor.y);
            if (selected || hovered) {
                drawHudQuad(commandBuffer, rectangle,
                            selected ? glm::vec4{0.30F, 0.30F, 0.30F, 0.95F}
                                     : glm::vec4{0.16F, 0.16F, 0.16F, 0.90F});
            }
            const std::string& name = index < menuSystem.languageDisplayNames.size()
                                          ? menuSystem.languageDisplayNames[index]
                                          : menuSystem.languageCodes[index];
            // Centred inside the box, exactly like 1.16.1's LanguageEntry#render,
            // which draws each name at width/2 - textWidth/2.
            drawHudText(commandBuffer, name,
                        rectangle.x + (rectangle.width - hudTextWidth(name, scale)) * 0.5F,
                        rectangle.y + 2.0F * scale, scale,
                        selected ? glm::vec4{1.0F, 1.0F, 1.0F, 1.0F}
                                 : glm::vec4{0.85F, 0.85F, 0.85F, 1.0F});
        }
        // A scrollbar thumb on the box's right edge when the list overflows,
        // mirroring the vanilla EntryListWidget's grey track.
        if (menuSystem.languageCodes.size() > visible) {
            const auto thumb = ui::languageScrollbarThumb(
                layout, static_cast<float>(swapchainExtent.width),
                menuSystem.languageCodes.size(), visible, first);
            drawHudQuad(commandBuffer, thumb,
                        {0.55F, 0.55F, 0.55F, 0.95F});
        }
        // The grey warning line between the list and the buttons, as vanilla
        // draws it at height - 56.
        const std::string warning = translated("options.languageWarning", "");
        if (!warning.empty()) {
            const std::string label = "(" + warning + ")";
            const float warningY = languageWarningY(layout);
            drawHudText(commandBuffer, label,
                        (static_cast<float>(swapchainExtent.width) - hudTextWidth(label, scale)) *
                            0.5F,
                        warningY, scale, {0.5F, 0.5F, 0.5F, 1.0F});
        }
        drawMenuWidgets(commandBuffer, buildDrawPage(), scale);
    }

    void drawPauseMenu(VkCommandBuffer commandBuffer, const ui::HudLayout& layout) const {
        const bool deathScreen = menuSystem.pageStack.current() == ui::PageId::Death;
        // 1.16.1's PauseScreen calls Screen.renderBackground(), which paints the
        // same dark gray gradient used by the gameSession.inventory() screens over the frozen
        // world. The death screen keeps its dark red wash instead, and the
        // options screen opened from the title (no world) shows the plain
        // optimized dirt backdrop like every other menu screen.
        if (deathScreen) {
            drawHudQuad(commandBuffer,
                        {0.0F, 0.0F, static_cast<float>(swapchainExtent.width),
                         static_cast<float>(swapchainExtent.height)},
                        {0.25F, 0.0F, 0.0F, 0.58F});
        } else if (worldSessionActive) {
            drawScreenDimOverlay(commandBuffer);
        }
        const float scale = layout.scale();
        const std::string title =
            menuSystem.pageStack.current() == ui::PageId::Options
                ? translated("options.title", "Options")
                : (menuSystem.pageStack.current() == ui::PageId::Experimental
                       ? translated("selectWorld.experimental", "Experimental")
                       : (menuSystem.pageStack.current() == ui::PageId::VideoSettings
                              ? translated("options.videoTitle", "Video Settings")
                              : (menuSystem.pageStack.current() == ui::PageId::Controls
                                     ? translated("controls.title", "Controls")
                                     : (deathScreen ? translated("deathScreen.title", "You Died!")
                                                    : translated("menu.game", "Game Menu")))));
        const std::size_t buttonCount = menuButtonCount();
        const auto firstButton =
            frontendButtonRect(layout, menuSystem.pageStack.current(), 0, buttonCount);
        const float titleScale = deathScreen ? scale * 2.0F : scale;
        // PX-6 Bug1: Controls is a three-band layout — the title is the TOP band,
        // above the scrolling key-bind list, not 30px over the bottom button band
        // (which is where firstButton sits). Other pages keep the historic
        // above-the-first-button title.
        const float titleY =
            menuSystem.pageStack.current() == ui::PageId::Controls
                ? ui::controlsListBox(layout, static_cast<float>(swapchainExtent.width)).y -
                      14.0F * titleScale
                : firstButton.y - 30.0F * titleScale;
        drawHudText(commandBuffer, title,
                    (static_cast<float>(swapchainExtent.width) - hudTextWidth(title, titleScale)) *
                        0.5F,
                    titleY, titleScale, {1.0F, 1.0F, 1.0F, 1.0F});
        drawMenuWidgets(commandBuffer, buildDrawPage(), scale);
        // PX-6 Bug1: the Controls key-bind list scrollbar (middle band), drawn
        // when the action count exceeds the visible window.
        if (menuSystem.pageStack.current() == ui::PageId::Controls) {
            drawControlsScrollbar(commandBuffer, layout);
        }
        if (!menuSystem.saveStatus.empty()) {
            drawHudText(commandBuffer, menuSystem.saveStatus, 4.0F * scale,
                        static_cast<float>(swapchainExtent.height) - 12.0F * scale, scale,
                        {1.0F, 1.0F, 1.0F, 1.0F});
        }
    }

    void drawPlayerPreview(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet,
                           const ui::HudLayout& layout) const {
        const auto& pose = playerModelAnimator.pose();
        const auto preview =
            layout.playerPreview(uiFrameData_.gameMode == gameplay::GameMode::Creative);
        // Vanilla's drawEntity Y coordinate is the feet anchor.  Our cuboid
        // coordinate system spans -16..+16 around its origin, so convert the
        // anchor to a model center before projecting it into view space.
        // Vanilla model coordinates use 16 units per block. drawEntity's
        // scale therefore maps one model unit to entityScale / 16 pixels.
        const float modelPixelsPerUnit = preview.entityScale / 16.0F;
        const float pixelX = preview.feetAnchor.x;
        const float pixelY = preview.feetAnchor.y - 16.0F * modelPixelsPerUnit * layout.scale();
        const float ndcX = pixelX / static_cast<float>(swapchainExtent.width) * 2.0F - 1.0F;
        const float ndcY = 1.0F - pixelY / static_cast<float>(swapchainExtent.height) * 2.0F;
        constexpr float depth = 2.35F;
        const glm::mat4 projection = camera.projectionMatrix(
            static_cast<float>(swapchainExtent.width) / static_cast<float>(swapchainExtent.height),
            cameraFarPlane());
        const float viewUnitsPerGuiPixel =
            2.0F * depth /
            (static_cast<float>(swapchainExtent.height) * std::abs(projection[1][1])) *
            layout.scale();
        const float modelUnit = viewUnitsPerGuiPixel * modelPixelsPerUnit;
        const glm::vec3 origin{
            ndcX * depth / projection[0][0],
            ndcY * depth / std::abs(projection[1][1]) + pose.idleBob * 16.0F * modelUnit, -depth};

        VkClearAttachment depthClear{};
        depthClear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        depthClear.clearValue.depthStencil = {1.0F, 0U};
        const VkClearRect clearRect{{{0, 0}, swapchainExtent}, 0U, 1U};
        vkCmdClearAttachments(commandBuffer, 1U, &depthClear, 1U, &clearRect);
        const int scissorX = std::max(0, static_cast<int>(std::floor(preview.clip.x)));
        const int scissorY = std::max(0, static_cast<int>(std::floor(preview.clip.y)));
        const int scissorRight =
            std::min(static_cast<int>(swapchainExtent.width),
                     static_cast<int>(std::ceil(preview.clip.x + preview.clip.width)));
        const int scissorBottom =
            std::min(static_cast<int>(swapchainExtent.height),
                     static_cast<int>(std::ceil(preview.clip.y + preview.clip.height)));
        const VkRect2D previewScissor{
            {scissorX, scissorY},
            {static_cast<std::uint32_t>(std::max(scissorRight - scissorX, 0)),
             static_cast<std::uint32_t>(std::max(scissorBottom - scissorY, 0))},
        };
        vkCmdSetScissor(commandBuffer, 0, 1, &previewScissor);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, heldItemPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, itemPipelineLayout,
                                0, 1, &descriptorSet, 0, nullptr);
        // The preview now renders through the same skeletal pose as the world
        // player (drawWorldPlayer): each bone's cube is drawn from
        // modelRoot * boneWorld * cubeRotation * T(centre), so the bone
        // hierarchy — head and arms as children of the body — composes, and the
        // whole figure turns rigidly with the cursor look instead of every part
        // spinning around its own centre. `origin` is in the camera's view
        // space, so the matrix cuboid is the matrixViewModel mode (data.x=6)
        // that projects through camera.projection without a second view pass.
        const auto& previewModel = playerModelAnimator.model();
        const auto& skeletonPose = playerModelAnimator.skeletonPose();
        // The geometry's feet sit at model y=0 while the previous hardcoded
        // layout anchored them 16 units below `origin`; shift the model root so
        // the figure lands in the same well.
        const glm::mat4 modelRoot = glm::translate(glm::mat4{1.0F}, origin) *
                                    glm::scale(glm::mat4{1.0F}, glm::vec3{modelUnit}) *
                                    glm::translate(glm::mat4{1.0F}, glm::vec3{0.0F, -16.0F, 0.0F});
        const auto layerForBone = [](std::string_view name) -> float {
            if (name == "head")
                return kPlayerHeadFirstLayer;
            if (name == "body")
                return kPlayerBodyFirstLayer;
            if (name == "rightArm")
                return kPlayerRightArmFirstLayer;
            if (name == "leftArm")
                return kPlayerLeftArmFirstLayer;
            if (name == "rightLeg")
                return kPlayerRightLegFirstLayer;
            if (name == "leftLeg")
                return kPlayerLeftLegFirstLayer;
            return -1.0F;
        };
        const auto pushPreviewCuboid = [&](const glm::mat4& cubeWorld, glm::vec3 dimensions,
                                           float layer) {
            const ItemPush push{
                {0.0F, 0.0F, 0.0F, 1.0F},
                {layer, 0.0F, 0.0F, 1.0F},
                {6.0F, 0.0F, 0.0F, 1.0F},
                {dimensions.x, dimensions.y, dimensions.z, 0.0F},
                cubeWorld,
            };
            vkCmdPushConstants(commandBuffer, itemPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                               sizeof(push), &push);
            vkCmdDraw(commandBuffer, 36U, 1, 0, 0);
        };
        for (std::size_t index = 0; index < previewModel.boneCount(); ++index) {
            const auto& bone = previewModel.bones()[index];
            const float layer = layerForBone(bone.name);
            if (layer < 0.0F) {
                continue;
            }
            const glm::mat4 boneWorld = skeletonPose.worldMatrix(static_cast<int>(index));
            for (const auto& cube : bone.cubes) {
                const glm::mat4 cubeRotation =
                    cube.hasRotation ? animation::rotationAboutPivot(cube.rotation, cube.pivot)
                                     : glm::mat4{1.0F};
                const glm::mat4 cubeWorld = modelRoot * boneWorld * cubeRotation *
                                            glm::translate(glm::mat4{1.0F}, cube.center());
                pushPreviewCuboid(cubeWorld, cube.renderSize(), layer);
            }
        }
        const VkRect2D fullScissor{{0, 0}, swapchainExtent};
        vkCmdSetScissor(commandBuffer, 0, 1, &fullScissor);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipelineLayout,
                                0, 1, &descriptorSet, 0, nullptr);
    }

    void drawWorkContainer(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet,
                           const ui::HudLayout& layout) const {
        drawScreenDimOverlay(commandBuffer);
        const auto panel = layout.inventoryPanel();
        const bool chestScreen = containerScreen == ContainerScreen::Chest;
        drawGuiSprite(
            commandBuffer, panel,
            chestScreen ? 10.0F : (containerScreen == ContainerScreen::CraftingTable ? 7.0F : 8.0F),
            {0.0F, 0.0F, 176.0F, 166.0F});
        if (chestScreen) {
            drawHudText(commandBuffer, translated("container.chest", "Chest"),
                        panel.x + 8.0F * layout.scale(), panel.y + 6.0F * layout.scale(),
                        layout.scale(), {0.25F, 0.25F, 0.25F, 1.0F}, false);
            drawHudText(commandBuffer, translated("container.inventory", "Inventory"),
                        panel.x + 8.0F * layout.scale(), panel.y + 73.0F * layout.scale(),
                        layout.scale(), {0.25F, 0.25F, 0.25F, 1.0F}, false);
            if (activeChest.has_value()) {
                const auto& chestItems = clientMirror.world().chestItems;
                for (std::size_t index = 0; index < gameplay::ChestBlockEntity::kSlotCount;
                     ++index) {
                    drawHudSlot(commandBuffer, layout.chestSlot(index), chestItems[index], false,
                                false, true);
                }
            }
        } else if (containerScreen == ContainerScreen::CraftingTable) {
            for (std::size_t index = 0; index < 9U; ++index) {
                drawHudSlot(commandBuffer, layout.tableCraftingSlot(index),
                            clientMirror.world().tableCraftingGrid[index], false, false, true);
            }
            drawHudSlot(commandBuffer, layout.tableCraftingOutput(),
                        clientMirror.world().tableCraftingOutput, false, false, true);
        } else {
            // N3c-4d: the furnace screen draws from the container display
            // snapshot; the block entity's position is not read here.
            const auto& worldSnap = clientMirror.world();
            drawHudSlot(commandBuffer, layout.furnaceInputSlot(), worldSnap.furnaceInput, false,
                        false, true);
            drawHudSlot(commandBuffer, layout.furnaceFuelSlot(), worldSnap.furnaceFuel, false,
                        false, true);
            drawHudSlot(commandBuffer, layout.furnaceOutputSlot(), worldSnap.furnaceOutput, false,
                        false, true);
            const float scale = layout.scale();
            const float fuel =
                std::clamp(clientMirror.world().furnaceFuelProgress, 0.0F, 1.0F);
            if (fuel > 0.0F) {
                const float height = std::ceil(13.0F * fuel);
                drawGuiSprite(commandBuffer,
                              {panel.x + 57.0F * scale, panel.y + (36.0F + 13.0F - height) * scale,
                               14.0F * scale, height * scale},
                              8.0F, {176.0F, 13.0F - height, 14.0F, height});
            }
            const float progress =
                std::clamp(clientMirror.world().furnaceCookProgress, 0.0F, 1.0F);
            if (progress > 0.0F) {
                const float width = std::ceil(24.0F * progress);
                drawGuiSprite(commandBuffer,
                              {panel.x + 79.0F * scale, panel.y + 34.0F * scale, width * scale,
                               17.0F * scale},
                              8.0F, {176.0F, 14.0F, width, 17.0F});
            }
        }
        for (std::size_t index = 0; index < gameplay::Inventory::kSlotCount; ++index) {
            const auto slot =
                chestScreen ? layout.chestInventorySlot(index) : layout.inventorySlot(index);
            drawHudSlot(commandBuffer, slot, clientMirror.world().inventorySlots[index],
                        index == uiFrameData_.selectedHotbarSlot, false, true);
        }
        // An in-progress drag previews the would-be placement in every swept
        // slot before the release, on top of the slots but under the cursor.
        drawDragPreview(commandBuffer, layout);
        if (!clientMirror.world().cursorStack.empty()) {
            const auto cursor = currentFramebufferCursor();
            const float size = 16.0F * layout.scale();
            drawHudSlot(commandBuffer, {cursor.x - size * 0.5F, cursor.y - size * 0.5F, size, size},
                        clientMirror.world().cursorStack, false, false, true);
        }
        static_cast<void>(descriptorSet);
    }

    void drawCreativeInventory(VkCommandBuffer commandBuffer, const ui::HudLayout& layout) const {
        const auto cursor = currentFramebufferCursor();
        const float scale = layout.scale();
        const auto panel = layout.creativePanel();
        drawScreenDimOverlay(commandBuffer);

        const std::size_t selectedTabIndex = static_cast<std::size_t>(menuSystem.creativeTab);
        // Tabs 0..5 sit on the top row; Spawn Eggs (6) and Inventory (7) share
        // the bottom row and use the bottom tab sprite.
        const std::size_t firstBottomTab = static_cast<std::size_t>(ui::CreativeTab::SpawnEggs);
        for (std::size_t tabIndex = 0; tabIndex < kCreativeTabCount; ++tabIndex) {
            const bool selected = tabIndex == selectedTabIndex;
            if (!selected) {
                const bool bottomTab = tabIndex >= firstBottomTab;
                drawGuiSprite(commandBuffer, layout.creativeTab(tabIndex), 4.0F,
                              {bottomTab ? static_cast<float>(tabIndex - firstBottomTab) * 28.0F
                                         : static_cast<float>(tabIndex) * 28.0F,
                               bottomTab ? 64.0F : 0.0F, 28.0F, 32.0F});
            }
        }
        drawGuiSprite(commandBuffer, panel,
                      menuSystem.creativeTab == ui::CreativeTab::Inventory ? 5.0F : 3.0F,
                      {0.0F, 0.0F, 195.0F, 136.0F});
        if (menuSystem.creativeTab == ui::CreativeTab::Inventory) {
            drawPlayerPreview(commandBuffer, currentFrameDescriptorSet(), layout);
        }

        const bool selectedBottomTab = selectedTabIndex >= firstBottomTab;
        drawGuiSprite(commandBuffer, layout.creativeTab(selectedTabIndex), 4.0F,
                      {selectedBottomTab
                           ? static_cast<float>(selectedTabIndex - firstBottomTab) * 28.0F
                           : static_cast<float>(selectedTabIndex) * 28.0F,
                       selectedBottomTab ? 96.0F : 32.0F, 28.0F, 32.0F});
        const std::array<gameplay::ItemStack, kCreativeTabCount> tabIcons{{
            {world::Block::Bricks, 1U},
            {world::Block::Dandelion, 1U},
            {world::Block::Chest, 1U},
            {world::Block::Air, 1U, &gameplay::items::IronIngot},
            {world::Block::Air, 1U, &gameplay::items::Apple},
            {world::Block::Air, 1U, &gameplay::items::DiamondPickaxe},
            {world::Block::Air, 1U, &gameplay::items::PigSpawnEgg},
            {world::Block::CraftingTable, 1U},
        }};
        for (std::size_t tabIndex = 0; tabIndex < tabIcons.size(); ++tabIndex) {
            const auto tab = layout.creativeTab(tabIndex);
            drawHudItemIcon(commandBuffer,
                            {tab.x + 6.0F * scale,
                             tab.y + (tabIndex >= firstBottomTab ? 7.0F : 9.0F) * scale,
                             16.0F * scale, 16.0F * scale},
                            tabIcons[tabIndex]);
        }

        std::optional<gameplay::ItemStack> hoveredStack;
        if (menuSystem.creativeTab == ui::CreativeTab::Inventory) {
            for (std::size_t index = 0; index < gameplay::Inventory::kSlotCount; ++index) {
                const auto slot = layout.creativeInventorySlot(index);
                const bool hovered = slot.contains(cursor.x, cursor.y);
                if (hovered && !clientMirror.world().inventorySlots[index].empty()) {
                    hoveredStack = clientMirror.world().inventorySlots[index];
                }
                drawHudSlot(commandBuffer, slot, clientMirror.world().inventorySlots[index],
                            index == uiFrameData_.selectedHotbarSlot, hovered, true);
            }
            const auto deleteSlot = layout.creativeDeleteSlot();
            if (deleteSlot.contains(cursor.x, cursor.y)) {
                drawHudQuad(commandBuffer, deleteSlot, {1.0F, 0.25F, 0.25F, 0.34F});
            }
        } else {
            constexpr std::array<std::pair<std::string_view, std::string_view>, 7> titles{{
                {"itemGroup.buildingBlocks", "Building Blocks"},
                {"itemGroup.decorations", "Decoration Blocks"},
                {"itemGroup.redstone", "Functional Blocks"},
                {"itemGroup.materials", "Materials"},
                {"itemGroup.food", "Foodstuffs"},
                {"itemGroup.tools", "Tools"},
                {"itemGroup.misc", "Spawn Eggs"},
            }};
            const auto title =
                translated(titles[selectedTabIndex].first, titles[selectedTabIndex].second);
            drawHudText(commandBuffer, title, panel.x + 8.0F * scale, panel.y + 6.0F * scale, scale,
                        {0.25F, 0.25F, 0.25F, 1.0F}, false);

            const bool hasScrollbar = creativeMaximumScrollRow() > 0U;
            drawGuiSprite(commandBuffer, layout.creativeScrollbarThumb(creativeScrollPosition()),
                          4.0F, {hasScrollbar ? 232.0F : 244.0F, 0.0F, 12.0F, 15.0F});

            const auto catalog = activeCreativeCatalog();
            const std::size_t firstCatalogIndex = menuSystem.creativeScrollRow * 9U;
            for (std::size_t visibleIndex = 0; visibleIndex < ui::HudLayout::kCreativeVisibleSlots;
                 ++visibleIndex) {
                const std::size_t catalogIndex = firstCatalogIndex + visibleIndex;
                if (catalogIndex >= catalog.size()) {
                    break;
                }
                const auto slot = layout.creativeSlot(visibleIndex);
                const bool hovered = slot.contains(cursor.x, cursor.y);
                if (hovered) {
                    hoveredStack = catalog[catalogIndex];
                }
                drawHudSlot(commandBuffer, slot, catalog[catalogIndex], false, hovered, true);
            }
            for (std::size_t index = 0; index < gameplay::Inventory::kHotbarSize; ++index) {
                const auto slot = layout.creativeHotbarSlot(index);
                drawHudSlot(commandBuffer, slot, clientMirror.world().inventorySlots[index],
                            index == uiFrameData_.selectedHotbarSlot,
                            slot.contains(cursor.x, cursor.y), true);
            }
        }

        // Real creative inventory/hotbar slots share QUICK_CRAFT with survival;
        // show the same prospective per-slot counts before the button is released.
        drawDragPreview(commandBuffer, layout);

        if (hoveredStack.has_value()) {
            const std::string_view label = itemDisplayName(*hoveredStack);
            const ui::UiRect tooltip{
                cursor.x + 12.0F * scale,
                cursor.y + 12.0F * scale,
                hudTextWidth(label, scale) + 8.0F * scale,
                14.0F * scale,
            };
            drawHudQuad(commandBuffer, tooltip, {0.05F, 0.03F, 0.08F, 0.94F});
            drawHudText(commandBuffer, label, tooltip.x + 4.0F * scale, tooltip.y + 3.0F * scale,
                        scale, {1.0F, 1.0F, 1.0F, 1.0F});
        }
        if (!clientMirror.world().cursorStack.empty()) {
            const float iconSize = 16.0F * scale;
            const ui::UiRect cursorRectangle{
                cursor.x - iconSize * 0.5F,
                cursor.y - iconSize * 0.5F,
                iconSize,
                iconSize,
            };
            drawHudItemIcon(commandBuffer, cursorRectangle, clientMirror.world().cursorStack);
            drawDurabilityBar(commandBuffer, cursorRectangle,
                              clientMirror.world().cursorStack);
            if (clientMirror.world().cursorStack.count > 1U) {
                const std::string count =
                    std::to_string(clientMirror.world().cursorStack.count);
                drawHudText(commandBuffer, count,
                            cursorRectangle.x + 17.0F * scale - hudTextWidth(count, scale),
                            cursorRectangle.y + 9.0F * scale, scale, {1.0F, 1.0F, 1.0F, 1.0F});
            }
        }
    }

    void drawChatOverlay(VkCommandBuffer commandBuffer, const ui::HudLayout& layout) const {
        const float scale = layout.scale();
        float messageY = chatOpen ? layout.chatInput().y - 12.0F * scale
                                  : static_cast<float>(swapchainExtent.height) - 28.0F * scale;
        // Vanilla ChatHud wraps every message to a fixed 320 unscaled-GUI-pixel
        // width (the default chat width) and stores one ChatHudLine per wrapped
        // row. rebedrock keeps the store line-oriented per logical line and wraps
        // to width here at draw time, so the wrap reflows for free when the GUI
        // scale or window width changes. Measurement is in unscaled GUI pixels
        // (scale 1) to match that width; the scale is applied only when drawing.
        constexpr float kChatWidth = 320.0F;
        const auto measure = [this](std::string_view piece) {
            return textFont.textWidth(piece, 1.0F);
        };
        const auto messages = chatHistory.messages();
        for (auto message = messages.rbegin(); message != messages.rend(); ++message) {
            if (!chatOpen && uiTimeSeconds >= message->createdAt + 5.0) {
                continue;
            }
            if (messageY < 2.0F * scale) {
                break;
            }
            const glm::vec4 color = message->successful
                                        ? glm::vec4{1.0F, 1.0F, 1.0F, 1.0F}
                                        : glm::vec4{1.0F, 0.35F, 0.35F, 1.0F};
            // Wrapped lines read top-to-bottom, so draw them bottom-up: the last
            // visual line sits nearest the input and earlier lines stack above.
            const std::vector<std::string> lines =
                ui::wrapText(message->text, kChatWidth, measure);
            for (auto line = lines.rbegin(); line != lines.rend(); ++line) {
                if (messageY < 2.0F * scale) {
                    break;
                }
                drawHudQuad(commandBuffer,
                            {2.0F * scale, messageY, hudTextWidth(*line, scale) + 4.0F * scale,
                             11.0F * scale},
                            {0.0F, 0.0F, 0.0F, 0.55F});
                drawHudText(commandBuffer, *line, 4.0F * scale, messageY + scale, scale, color,
                            false);
                messageY -= 11.0F * scale;
            }
        }
        if (!chatOpen) {
            return;
        }
        // Vanilla sizes the chat input to the whole screen width (GuiChat 1.16.1
        // gives its EditBox a width of windowWidth - 8), so the dark backdrop
        // always runs from the left edge to the right edge instead of hugging
        // the typed text.
        const ui::UiRect input = layout.chatInput();
        drawHudQuad(commandBuffer, input, {0.0F, 0.0F, 0.0F, 0.72F});
        const bool cursorVisible = static_cast<int>(uiTimeSeconds * 2.0) % 2 == 0;
        const std::string visibleText = chatInputText + (cursorVisible ? "_" : "");
        drawHudText(commandBuffer, visibleText, input.x + 2.0F * scale, input.y + 2.0F * scale,
                    scale, {1.0F, 1.0F, 1.0F, 1.0F}, false);
        // 26.1's CommandSuggestions draws the completion list in ONE opaque dark
        // box (not per-row translucent strips), the selected row highlighted, the
        // suggestion text white and its usage/hint in grey. Match that here: one
        // background rect sized to the widest row, then the rows over it.
        const std::size_t maxRows = std::min<std::size_t>(chatSuggestions_.size(), 8U);
        if (maxRows > 0) {
            const float rowHeight = 11.0F * scale;
            const float boxLeft = 2.0F * scale;
            // Width the box to the widest "text  hint" row so the opaque panel
            // fully backs every row (vanilla sizes the tooltip to its content).
            float widest = 0.0F;
            for (std::size_t row = 0; row < maxRows; ++row) {
                std::string hint = chatSuggestions_[row].hint;
                if (hint.starts_with("item.") || hint.starts_with("block.")) {
                    hint = translated(hint, hint);
                }
                const std::string measured =
                    chatSuggestions_[row].text + (hint.empty() ? "" : "  " + hint);
                widest = std::max(widest, hudTextWidth(measured, scale));
            }
            const float boxWidth = widest + 4.0F * scale;
            const float boxTop = input.y - static_cast<float>(maxRows) * rowHeight;
            // 26.1's opaque tooltip background (a near-black panel, alpha ~0.95).
            drawHudQuad(commandBuffer,
                        {boxLeft, boxTop, boxWidth, static_cast<float>(maxRows) * rowHeight},
                        {0.05F, 0.05F, 0.05F, 0.95F});
            for (std::size_t row = 0; row < maxRows; ++row) {
                const auto& suggestion = chatSuggestions_[row];
                const float rowY = input.y - (static_cast<float>(row) + 1.0F) * rowHeight;
                if (row == chatSuggestionIndex_) {
                    // The selected row's highlight bar.
                    drawHudQuad(commandBuffer, {boxLeft, rowY, boxWidth, rowHeight},
                                {0.10F, 0.10F, 0.10F, 1.0F});
                }
                std::string hint = suggestion.hint;
                if (hint.starts_with("item.") || hint.starts_with("block.")) {
                    hint = translated(hint, hint);
                }
                // The suggestion text: white when selected, light grey otherwise.
                const glm::vec4 textColor = row == chatSuggestionIndex_
                                                ? glm::vec4{1.0F, 1.0F, 1.0F, 1.0F}
                                                : glm::vec4{0.66F, 0.66F, 0.66F, 1.0F};
                drawHudText(commandBuffer, suggestion.text, boxLeft + 2.0F * scale, rowY + scale,
                            scale, textColor, false);
                // The usage/hint trails in grey after the text (26.1 usage colour).
                if (!hint.empty()) {
                    const float hintX =
                        boxLeft + 2.0F * scale + hudTextWidth(suggestion.text + "  ", scale);
                    drawHudText(commandBuffer, hint, hintX, rowY + scale, scale,
                                {0.53F, 0.53F, 0.53F, 1.0F}, false);
                }
            }
        }
    }

    // PX-6: the top-right toast overlay (26.1 ToastComponent). Each visible toast
    // is an opaque panel that slides in from the right by its slideFraction, with
    // a white title and grey subtitle. The queue's lifetime/cap/animation is the
    // Vulkan-free ui::ToastQueue; this only paints its current visibleToasts().
    // Passed by parameter (not a builder member) so wiring it is one call site.
    void drawToastOverlay(VkCommandBuffer commandBuffer, const ui::HudLayout& layout) const {
        const float scale = layout.scale();
        const float toastWidth = 160.0F * scale;
        const float toastHeight = 28.0F * scale;
        const float margin = 4.0F * scale;
        const float right = static_cast<float>(swapchainExtent.width);
        float slotY = margin;
        for (const ui::ActiveToast& active : toastQueue.visibleToasts()) {
            // slideFraction 1 = fully in; 0 = off the right edge.
            const float x = right - active.slideFraction * (toastWidth + margin);
            // PX-6 Bug4: draw the toast on the GUI atlas's nine-sliced widget
            // panel (the Button frame), not a flat grey quad — a proper 26.x
            // bordered background. (Vanilla's dedicated toasts.png is not in the
            // shipped assets; the widget panel is the closest real sprite. If a
            // toasts sprite is ever added, swap the sprite here.)
            const ui::UiRect panel{std::floor(x), std::floor(slotY), std::floor(toastWidth),
                                   std::floor(toastHeight)};
            drawScaledGuiSprite(commandBuffer, panel, 0.0F,
                                guiWidgetSprite(guiWidgetSprites, GuiWidgetSprite::Button), scale);
            drawHudText(commandBuffer, active.toast.title, x + 5.0F * scale, slotY + 4.0F * scale,
                        scale, {1.0F, 1.0F, 1.0F, 1.0F}, false);
            if (!active.toast.subtitle.empty()) {
                drawHudText(commandBuffer, active.toast.subtitle, x + 5.0F * scale,
                            slotY + 15.0F * scale, scale, {0.66F, 0.66F, 0.66F, 1.0F}, false);
            }
            slotY += toastHeight + margin;
        }
    }

    // PX-6: the bottom-right sound-subtitle overlay (26.1 SubtitleOverlay). Each
    // active caption is drawn right-aligned above the last, fading with its alpha.
    // The list/cap/fade is the Vulkan-free ui::SubtitleFeed; this only paints it.
    void drawSubtitleOverlay(VkCommandBuffer commandBuffer, const ui::HudLayout& layout) const {
        const float scale = layout.scale();
        const float right = static_cast<float>(swapchainExtent.width);
        const float bottom = static_cast<float>(swapchainExtent.height);
        const float lineHeight = 11.0F * scale;
        const float margin = 4.0F * scale;
        const auto& captions = subtitleFeed.activeCaptions();
        for (std::size_t i = 0; i < captions.size(); ++i) {
            const auto& caption = captions[captions.size() - 1U - i];  // newest at bottom
            const float alpha = caption.alpha();
            const float textWidth = hudTextWidth(caption.text, scale);
            const float y = bottom - margin - static_cast<float>(i + 1U) * lineHeight;
            const float x = right - margin - textWidth - 4.0F * scale;
            drawHudQuad(commandBuffer, {x, y, textWidth + 4.0F * scale, lineHeight},
                        {0.0F, 0.0F, 0.0F, 0.6F * alpha});
            drawHudText(commandBuffer, caption.text, x + 2.0F * scale, y + scale, scale,
                        {1.0F, 1.0F, 1.0F, alpha}, false);
        }
    }

    // 1.16.1's InGameHud.render, consolidated into a single layer so every
    // element that belongs to the HUD — the damage tint, vignette, first-person
    // held item, hotbar, survival status bars, crosshair and the held-item name —
    // is drawn together in vanilla order. drawHud then draws any open screen
    // (gameSession.inventory(), container, pause) on top, so its renderBackground gradient
    // darkens this whole layer uniformly instead of leaving individual elements
    // floating bright over the overlay.
    void drawInGameHudLayer(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet,
                            const ui::HudLayout& layout) const {
        // 1.16.1's GameRenderer renders the hand right after the world, against
        // a fresh depth buffer, and InGameHud.render (vignette, hotbar, ...)
        // follows it. So the hand sits at the very bottom of the HUD layer: the
        // damage tint, the vignette and any open screen's gradient are all drawn
        // over it afterwards.
        VkClearAttachment heldDepthClear{};
        heldDepthClear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        heldDepthClear.clearValue.depthStencil = {1.0F, 0U};
        const VkClearRect heldDepthRect{
            {{0, 0}, swapchainExtent},
            0U,
            1U,
        };
        vkCmdClearAttachments(commandBuffer, 1U, &heldDepthClear, 1U, &heldDepthRect);
        drawHeldItem(commandBuffer, descriptorSet);
        drawUnderwaterOverlay(commandBuffer, descriptorSet);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipeline);
        // drawHeldItem binds set 0 through itemPipelineLayout (128-byte vertex
        // push range). Switching back to the HUD pipeline is not enough: its
        // 64-byte vertex+fragment layout is incompatible, so the damage quad's
        // first draw must re-bind the shared set through hudPipelineLayout.
        // The underwater overlay does this itself, but it is conditional.
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipelineLayout,
                                0, 1, &descriptorSet, 0, nullptr);

        drawDamageOverlay(commandBuffer);
        drawVignette(commandBuffer, descriptorSet);

        // HUD hotbar and the survival status bars. The gameSession.player() gameSession.inventory()
        // keeps the HUD hotbar on screen in both survival and creative; container screens keep
        // their previous look, and status bars stay survival-only.
        const bool playerInventoryOpen =
            inventoryOpen && containerScreen == ContainerScreen::PlayerInventory;
        if (!inventoryOpen || playerInventoryOpen) {
            drawGuiSprite(commandBuffer, layout.hotbarBackground(), 0.0F,
                          {0.0F, 0.0F, 182.0F, 22.0F});
            drawGuiSprite(commandBuffer, layout.hotbarSelection(uiFrameData_.selectedHotbarSlot),
                          0.0F, {0.0F, 22.0F, 24.0F, 24.0F});
            for (std::size_t index = 0; index < gameplay::Inventory::kHotbarSize; ++index) {
                drawHudSlot(commandBuffer, layout.hotbarSlot(index), clientMirror.world().inventorySlots[index],
                            index == uiFrameData_.selectedHotbarSlot, false, true);
            }
            if (uiFrameData_.gameMode == gameplay::GameMode::Survival) {
                drawSurvivalStatusBars(commandBuffer, layout);
                drawExperienceBar(commandBuffer, layout);
            }
        }

        if (!inventoryOpen) {
            const float textScale = layout.scale();
            // GuiIngame#renderSelectedItemName: the held item's name appears
            // when the selection changes and fades out over two seconds, and an
            // empty hand shows nothing at all (no "Air" label).
            const auto& selectedStack = uiFrameData_.selectedStack;
            if (!selectedStack.empty()) {
                const std::size_t selectedSlot = uiFrameData_.selectedHotbarSlot;
                const bool selectionChanged =
                    selectedNameSlot_ == static_cast<std::size_t>(-1) ||
                    selectedSlot != selectedNameSlot_ ||
                    !gameplay::sameItem(selectedStack, selectedNameStack_);
                if (selectionChanged) {
                    selectedNameSlot_ = selectedSlot;
                    selectedNameStack_ = selectedStack;
                    selectedNameShownAt_ = uiTimeSeconds;
                }
                const double elapsed = uiTimeSeconds - selectedNameShownAt_;
                float alpha = 0.0F;
                if (elapsed < 2.0) {
                    // Full brightness for the first 1.5 s, then a half-second
                    // fade, mirroring the vanilla highlight's tail.
                    alpha = elapsed <= 1.5 ? 1.0F : static_cast<float>((2.0 - elapsed) / 0.5);
                }
                if (alpha > 0.0F) {
                    const std::string_view selectedName = itemDisplayName(selectedStack);
                    drawHudText(commandBuffer, selectedName,
                                (static_cast<float>(swapchainExtent.width) -
                                 hudTextWidth(selectedName, textScale)) *
                                    0.5F,
                                layout.hotbarBackground().y -
                                    (uiFrameData_.gameMode == gameplay::GameMode::Survival
                                         ? 30.0F
                                         : 12.0F) *
                                        textScale,
                                textScale, {1.0F, 1.0F, 1.0F, alpha});
                }
            } else {
                selectedNameSlot_ = static_cast<std::size_t>(-1);
                selectedNameStack_ = {};
                selectedNameShownAt_ = -1.0;
            }
        }

        // Crosshair — vanilla InGameHud draws it every frame while in-game, and
        // an open screen's gradient darkens it along with the rest of the layer.
        const ui::HudLayout crosshairLayout{static_cast<float>(swapchainExtent.width),
                                            static_cast<float>(swapchainExtent.height),
                                            menuSystem.guiScaleSetting};
        const float crosshairSize = 15.0F * crosshairLayout.scale();
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, crosshairPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipelineLayout,
                                0, 1, &descriptorSet, 0, nullptr);
        drawMinecraftCrosshair(commandBuffer,
                               {(static_cast<float>(swapchainExtent.width) - crosshairSize) * 0.5F,
                                (static_cast<float>(swapchainExtent.height) - crosshairSize) * 0.5F,
                                crosshairSize, crosshairSize});
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipeline);
    }

    void drawHud(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet) const {
        if (testScene.has_value())
            return;
        const ui::HudLayout layout{static_cast<float>(swapchainExtent.width),
                                   static_cast<float>(swapchainExtent.height),
                                   menuSystem.guiScaleSetting};
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipelineLayout,
                                0, 1, &descriptorSet, 0, nullptr);

        const auto page = menuSystem.pageStack.current();
        if (page == ui::PageId::Title || page == ui::PageId::WorldList ||
            page == ui::PageId::CreateWorld || page == ui::PageId::EditWorld ||
            page == ui::PageId::ConfirmDelete) {
            drawFrontend(commandBuffer, layout, descriptorSet);
            return;
        }

        if (page == ui::PageId::Options || page == ui::PageId::VideoSettings ||
            page == ui::PageId::Controls || page == ui::PageId::Language ||
            page == ui::PageId::Experimental) {
            if (!worldSessionActive) {
                drawTitleCarousel(commandBuffer, descriptorSet, true, layout.scale());
            }
            if (page == ui::PageId::Language) {
                drawLanguageScreen(commandBuffer, layout);
            } else {
                drawPauseMenu(commandBuffer, layout);
            }
            return;
        }

        if (!worldReady) {
            drawTitleCarousel(commandBuffer, descriptorSet, true, layout.scale());
            const float scale = layout.scale();
            const float progress = peakPendingSectionCount == 0U
                                       ? 0.0F
                                       : 1.0F - static_cast<float>(pendingSectionUpdates.size()) /
                                                    static_cast<float>(peakPendingSectionCount);
            const std::string message =
                spawnPositionInitialized
                    ? translated("multiplayer.downloadingTerrain", "Loading terrain...") + " " +
                          std::to_string(static_cast<int>(
                              std::lround(std::clamp(progress, 0.0F, 1.0F) * 100.0F))) +
                          "%"
                    : translated("menu.generatingTerrain", "Preparing spawn area...");
            drawHudText(
                commandBuffer, message,
                (static_cast<float>(swapchainExtent.width) - hudTextWidth(message, scale)) * 0.5F,
                static_cast<float>(swapchainExtent.height) * 0.5F, scale, {1.0F, 1.0F, 1.0F, 1.0F});
            return;
        }

        if (paused) {
            drawPauseMenu(commandBuffer, layout);
            return;
        }

        drawInGameHudLayer(commandBuffer, descriptorSet, layout);

        if (inventoryOpen && containerScreen != ContainerScreen::PlayerInventory) {
            drawWorkContainer(commandBuffer, descriptorSet, layout);
        }

        if (inventoryOpen && containerScreen == ContainerScreen::PlayerInventory &&
            uiFrameData_.gameMode == gameplay::GameMode::Creative) {
            drawCreativeInventory(commandBuffer, layout);
        }

        if (inventoryOpen && containerScreen == ContainerScreen::PlayerInventory &&
            uiFrameData_.gameMode == gameplay::GameMode::Survival) {
            double cursorWindowX = 0.0;
            double cursorWindowY = 0.0;
            int windowWidth = 0;
            int windowHeight = 0;
            int framebufferWidth = 0;
            int framebufferHeight = 0;
            glfwGetCursorPos(window, &cursorWindowX, &cursorWindowY);
            glfwGetWindowSize(window, &windowWidth, &windowHeight);
            glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
            // Scale by the live framebuffer size, not the swapchain extent: right
            // after a resize or maximize the extent is still the previous frame's,
            // which would shift the cursor (and the white slot highlight) by a
            // few pixels until the swapchain is recreated.
            const auto framebufferCursor =
                ui::windowToFramebuffer(cursorWindowX, cursorWindowY, windowWidth, windowHeight,
                                        framebufferWidth, framebufferHeight);
            const float cursorX = framebufferCursor.x;
            const float cursorY = framebufferCursor.y;
            drawScreenDimOverlay(commandBuffer);
            const auto panel = layout.inventoryPanel();
            const float textScale = layout.scale();
            drawGuiSprite(commandBuffer, panel, 2.0F, {0.0F, 0.0F, 176.0F, 166.0F});
            drawPlayerPreview(commandBuffer, descriptorSet, layout);
            for (std::size_t index = 0; index < 4U; ++index) {
                drawHudSlot(commandBuffer, layout.playerCraftingSlot(index),
                            clientMirror.world().playerCraftingGrid[index], false, false, true);
            }
            drawHudSlot(commandBuffer, layout.playerCraftingOutput(),
                        clientMirror.world().playerCraftingOutput, false, false, true);
            std::optional<std::size_t> hoveredSlot;
            for (std::size_t index = 0; index < gameplay::Inventory::kSlotCount; ++index) {
                const bool hovered = layout.inventorySlot(index).contains(cursorX, cursorY);
                if (hovered) {
                    hoveredSlot = index;
                }
                drawHudSlot(commandBuffer, layout.inventorySlot(index),
                            clientMirror.world().inventorySlots[index],
                            index == uiFrameData_.selectedHotbarSlot, hovered, true);
            }
            if (hoveredSlot.has_value() && !clientMirror.world().inventorySlots[*hoveredSlot].empty()) {
                const auto snapshot = clientMirror.world();
                const auto& hoveredStack = snapshot.inventorySlots[*hoveredSlot];
                std::string label{itemDisplayName(hoveredStack)};
                label += " x" + std::to_string(hoveredStack.count);
                const float labelWidth = hudTextWidth(label, textScale) + 8.0F * textScale;
                const ui::UiRect tooltip{
                    cursorX + 12.0F * textScale,
                    cursorY + 12.0F * textScale,
                    labelWidth,
                    14.0F * textScale,
                };
                drawHudQuad(commandBuffer, tooltip, {0.05F, 0.03F, 0.08F, 0.94F});
                drawHudText(commandBuffer, label, tooltip.x + 4.0F * textScale,
                            tooltip.y + 3.0F * textScale, textScale, {1.0F, 1.0F, 1.0F, 1.0F});
            }
            // An in-progress drag previews the would-be placement in every swept
            // slot before the release, on top of the slots but under the cursor.
            drawDragPreview(commandBuffer, layout);
            if (!clientMirror.world().cursorStack.empty()) {
                const float cursorIconSize = 16.0F * layout.scale();
                const ui::UiRect cursorRectangle{cursorX - cursorIconSize * 0.5F,
                                                 cursorY - cursorIconSize * 0.5F, cursorIconSize,
                                                 cursorIconSize};
                drawHudItemIcon(commandBuffer, cursorRectangle,
                                clientMirror.world().cursorStack);
                drawDurabilityBar(commandBuffer, cursorRectangle,
                                  clientMirror.world().cursorStack);
                if (clientMirror.world().cursorStack.count > 1U) {
                    const std::string count =
                        std::to_string(clientMirror.world().cursorStack.count);
                    const float textScale = layout.scale();
                    drawHudText(
                        commandBuffer, count,
                        cursorRectangle.x + 17.0F * textScale - hudTextWidth(count, textScale),
                        cursorRectangle.y + 9.0F * textScale, textScale, {1.0F, 1.0F, 1.0F, 1.0F});
                }
            }
        }

        if (debugOverlayOpen) {
            const auto& debugSnap = clientMirror.player();
            std::ostringstream coordinates;
            coordinates << std::fixed << std::setprecision(3)
                        << "XYZ: " << debugSnap.physicsCurrent.x << " / "
                        << debugSnap.physicsCurrent.y << " / " << debugSnap.physicsCurrent.z;
            // Vanilla DebugHud samples the block the gameSession.player()'s feet are in, and
            // a resting gameSession.player()'s feet sit exactly on the integer boundary, so
            // floor() lands on the air cell above the ground block. Rebedrock
            // rests the feet a collision epsilon below that boundary, which
            // would round down into the solid block (block light 0 by
            // construction); nudging past the epsilon reproduces vanilla's
            // sample point.
            const glm::ivec3 playerBlock{
                static_cast<int>(std::floor(debugSnap.physicsCurrent.x)),
                static_cast<int>(std::floor(debugSnap.physicsCurrent.y + 0.001F)),
                static_cast<int>(std::floor(debugSnap.physicsCurrent.z))};
            // The version line reads the single build-identity source (core::kVersion,
            // META's rodata manifest) rather than any persisted/hardcoded string, so
            // F3 always shows the version this binary actually is, tagged with the
            // git build ref for diagnostics.
            const std::string versionLine = "ReBedrock " + std::string{core::kVersion.name} +
                                            " (" + std::string{core::kVersion.buildRef} + ")";
            const std::array labels{
                versionLine + " | FPS: " + std::to_string(displayedFps),
                coordinates.str(),
                std::string{"Light: sky "} +
                    std::to_string(
                        lightWorld.skyLight(playerBlock.x, playerBlock.y, playerBlock.z)) +
                    " / block " +
                    std::to_string(
                        lightWorld.blockLight(playerBlock.x, playerBlock.y, playerBlock.z)),
            };
            const float scale = layout.scale();
            const float textX = 2.0F * scale;
            const float textY = 2.0F * scale;
            for (std::size_t line = 0; line < labels.size(); ++line) {
                const float y = textY + static_cast<float>(line) * 10.0F * scale;
                drawHudQuad(commandBuffer,
                            {textX - scale, y - scale,
                             hudTextWidth(labels[line], scale) + 4.0F * scale, 11.0F * scale},
                            {0.0F, 0.0F, 0.0F, 0.55F});
                drawHudText(commandBuffer, labels[line], textX, y, scale,
                            {0.92F, 0.92F, 0.92F, 1.0F}, false);
            }
        }
        drawChatOverlay(commandBuffer, layout);
        // PX-6: the game-in overlays sit above the chat/HUD: top-right toasts and
        // bottom-right sound subtitles.
        drawToastOverlay(commandBuffer, layout);
        drawSubtitleOverlay(commandBuffer, layout);
    }

    // ---- bound references to renderer-core state ----
    ui::MenuSystem& menuSystem;
    ui::UiFrameData& uiFrameData_;
    gameplay::GameSession& gameSession;
    const client::ClientMirror& clientMirror;
    ui::TextFont& textFont;
    ui::BitmapFontMetrics& fontMetrics;
    ui::Language& language;
    world::World& lightWorld;
    GLFWwindow*& window;
    config::GameOptions& options;
    PerspectiveCamera& camera;
    VkExtent2D& swapchainExtent;
    VkPipeline& hudPipeline;
    VkPipelineLayout& hudPipelineLayout;
    VkPipeline& vignettePipeline;
    VkPipeline& crosshairPipeline;
    VkPipeline& panoramaPipeline;
    VkPipelineLayout& panoramaPipelineLayout;
    VkPipeline& heldItemPipeline;
    VkPipelineLayout& itemPipelineLayout;
    bool& inventoryOpen;
    const ContainerScreen& containerScreen;
    const std::optional<gameplay::ChestPosition>& activeChest;
    bool& debugOverlayOpen;
    bool& inventoryDragActive;
    std::vector<gameplay::SlotRef>& inventoryDragSlots;
    bool& chatOpen;
    ui::ChatHistory& chatHistory;
    std::string& chatInputText;
    std::vector<gameplay::command::Suggestion>& chatSuggestions_;
    std::size_t& chatSuggestionIndex_;
    ui::ToastQueue& toastQueue;
    ui::SubtitleFeed& subtitleFeed;
    std::optional<persistence::SaveGame>& currentSave;
    int& displayedFps;
    animation::PlayerModelAnimator& playerModelAnimator;
    ui::WidgetId& pressedMenuButton;
    bool& spawnPositionInitialized;
    bool& worldReady;
    bool& worldSessionActive;
    int& simulationDistanceChunks;
    int& viewDistanceChunks;
    std::size_t& peakPendingSectionCount;
    const std::unordered_map<world::SectionPosition, world::SectionMeshUpdate,
                             world::SectionPositionHash>& pendingSectionUpdates;
    const std::optional<TestSceneOptions>& testScene;
    const GuiWidgetSpriteTable& guiWidgetSprites;
    bool& paused;
    double& uiTimeSeconds;

    // ---- world-render / per-frame couplings (bound to Impl lambdas) ----
    std::function<bool()> cameraSubmergedInWater;
    std::function<std::string(input::InputAction)> keyBindLabel;
    std::function<void(VkCommandBuffer, VkDescriptorSet)> drawHeldItem;
    std::function<VkDescriptorSet()> currentFrameDescriptorSet;
    std::function<std::span<const gameplay::ItemStack>()> activeCreativeCatalog;
    std::function<float()> creativeScrollPosition;
    std::function<std::size_t()> creativeMaximumScrollRow;
    std::function<std::vector<std::uint8_t>()> dragPlacementCounts;
    std::function<float()> cameraFarPlane;
    std::function<std::optional<ui::UiRect>(const ui::HudLayout&, const gameplay::SlotRef&)>
        dragSlotRectangle;

    // ---- owned UI animation / selection state ----
    float vignetteDarkness_ = 1.0F;
    mutable std::size_t selectedNameSlot_ = static_cast<std::size_t>(-1);
    mutable gameplay::ItemStack selectedNameStack_;
    mutable double selectedNameShownAt_ = -1.0;
};

} // namespace mc::render
