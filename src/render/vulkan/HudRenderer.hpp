#pragma once
// HUD 与前端界面的绘制子系统
// 它自持少量 UI 动画状态，其余一律通过引用成员访问渲染器内核，引用在 Bindings 里一次性绑定
// 另有几个 std::function 钩子接世界渲染侧的耦合
// 全部内联在头文件里，与 VulkanDevice 同一形态
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
#include "ui/EnchantmentNames.hpp"
#include "ui/OptionCycle.hpp"
#include "ui/WidgetLabels.hpp"
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
        // HUD 对玩家与世界的读取一律取自客户端镜像
        const client::ClientMirror& clientMirror;
        ui::TextFont& textFont;
        ui::BitmapFontMetrics& fontMetrics;
        ui::Language& language;
        // HUD 采样光照所用的世界是渲染侧自有的客户端区块缓存，绝不是服务端世界
    // 因为 HUD 的光照读取不能去抢服务端锁
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
        // 可拉伸控件的图集矩形与 26.1 gui.scaling，由 TextureManager::createGuiTexture() 填充
        const GuiWidgetSpriteTable& guiWidgetSprites;
        bool& paused;
        double& uiTimeSeconds;
        std::function<bool()> cameraSubmergedInWater;
        // 按键设置里每行的标签形如"动作: 按键"，取自 InputSystem 这一唯一事实源
        // 绘制页因此显示的是实时绑定
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
          dragSlotRectangle(b.dragSlotRectangle) {
        // 绘制侧 Page 装配件里每帧都不变的部分，构造时装一次
        // buildDrawPage 位于 drawFrontend / drawPauseMenu / drawLanguageScreen 三个
        // 每帧绘制函数的路径上，把这些留在函数里就是每帧重新构造一遍：一个含 31 个
        // std::function 的 MenuCallbacks、上下文里两个捕获 this 的 std::function，
        // 以及一个新的 vector<Widget>
        // 它们的取值逐帧完全相同
        drawContext_.labelFor = [this](std::uint16_t id) {
            return widgetLabel(static_cast<ui::WidgetId>(id));
        };
        drawContext_.keyBindLabelFor = [this](input::InputAction action) {
            return keyBindLabel ? keyBindLabel(action)
                                : std::string{input::actionDisplayName(action)};
        };
        drawCallbacks_.viewDistance.value = [this] {
            return static_cast<float>(viewDistanceChunks - 2) / 34.0F;
        };
        drawCallbacks_.simulationDistance.value = [this] {
            return static_cast<float>(simulationDistanceChunks - 2) / 10.0F;
        };
        drawCallbacks_.masterVolume.value = [this] { return options.masterVolume; };
    }

    HudRenderer(const HudRenderer&) = delete;
    HudRenderer& operator=(const HudRenderer&) = delete;

    // ---- 与渲染器内核重复的一组助手，都是对已绑定引用的纯读取 ----
    // 有了它们，搬过来的绘制代码不必反向调用 Impl，而 Impl 自己保留一份供输入路径使用
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

    // 当前页面的 ui::Page，供绘制使用，与派发共用 ui::buildPage 这一唯一来源
    // 这里只接上标签和滑块显示值，不接动作回调
    // 绘制后端只读 widget 的 label、rect、kind、enabled 和 slider.value
    // 回调故意留空，绘制永远不会触发它们
    // 页面本身仍逐帧装配，只是装配进常驻的 drawPage_，容量因此跨帧复用
    // 这里刻意没有做「整页缓存 + 失效」：页面内容依赖 menuSystem 的十余个字段、
    // GameOptions 的每一个字段、实时窗口尺寸、语言表和按键捕获状态，手工维护这份
    // 失效清单漏掉任何一项，症状就是菜单显示陈旧内容——用一个静默 bug 换几十次分配
    // 并不划算。真正让它可缓存的前置是把 widgetLabel 的 switch 变成表（见
    // docs/CODE_PROBLEMS-branches.md §2.1）：标签依赖收敛到「表行 + 该选项的值」之后，
    // 失效 key 才写得干净
    [[nodiscard]] const ui::Page& buildDrawPage() const {
        const ui::PageId pageId = menuSystem.pageStack.current();
        const ui::HudLayout layout{static_cast<float>(swapchainExtent.width),
                                   static_cast<float>(swapchainExtent.height),
                                   menuSystem.guiScaleSetting};
        const std::size_t count = menuButtonCount();
        drawContext_.worldOpen = currentSave.has_value();
        drawContext_.worldSelectable = !menuSystem.saveSummaries.empty();
        // 按键设置页喂进滚动窗口与实时按键标签，可见行按 controlsRow 排版
        // 末尾四个是底部按钮带，其余页面不受影响
        // 上下文是常驻的，因此这两个字段每帧先归零，非按键页看到的仍是「没有按键行」
        const float fbWidth = static_cast<float>(swapchainExtent.width);
        std::size_t keyRows = 0U;
        drawContext_.keyBindFirstIndex = 0U;
        drawContext_.keyBindRowCount = 0U;
        if (pageId == ui::PageId::Controls) {
            const std::size_t total = input::keyBindRows().size();
            const std::size_t window = ui::controlsVisibleRowCount(
                fbWidth, static_cast<float>(swapchainExtent.height), menuSystem.guiScaleSetting);
            const std::size_t first = std::min(menuSystem.controlsListFirstIndex, total);
            keyRows = std::min(window, total - first);
            drawContext_.keyBindFirstIndex = first;
            drawContext_.keyBindRowCount = keyRows;
        }
        ui::buildPageInto(drawPage_, pageId, drawContext_, drawCallbacks_,
                          [layout, pageId, count, fbWidth, keyRows](std::size_t index) {
                              if (pageId == ui::PageId::Controls && index < keyRows) {
                                  return ui::controlsRow(index, layout, fbWidth);
                              }
                              const std::size_t buttonIndex =
                                  pageId == ui::PageId::Controls ? index - keyRows : index;
                              return ui::frontendButtonRect(layout, pageId, buttonIndex, count);
                          });
        return drawPage_;
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

    // `portion` 表示图标显示的是台阶的哪一半：0 整块、1 下半、2 上半
    // 着色器从 uvRect.x 读它（方块分支本来就忽略 uvRect），据此把立方体压成半砖
    // 之所以借道 uvRect 而不是 data，是要把 data 里那三个纹理层槽留给箱子/熔炉的正面
    void drawHudBlockIcon(VkCommandBuffer commandBuffer, const ui::UiRect& rectangle,
                          world::Block block, float portion = 0.0F) const {
        const float width = static_cast<float>(swapchainExtent.width);
        const float height = static_cast<float>(swapchainExtent.height);
        const auto clipRectangle = ui::framebufferToClip(rectangle, width, height);
        const auto textures = world::textureLayers(block);
        const bool chest = block == world::Block::Chest;
        // 熔炉、侦测器、活塞这类有朝向的立方体把六个面解析进独立的 kBlockDirectionalLayers 表
        // 它们不用扁平的顶侧槽，`textures.side` 只是普通侧面贴图，绝不是正面
        // 因此图标朝向相机的那个面必须取真正的正面层，否则熔炉本该是炉膛的位置会是一片空白侧面
        const bool directional =
            world::blockDefinition(block).model == world::BlockModel::DirectionalCube;
        const auto& directionalFaces = world::directionalLayers(block);
        const float frontLayer = directional ? directionalFaces.front : textures.side;
        const float topLayer = directional ? directionalFaces.top : textures.top;
        const HudPush push{
            {clipRectangle.x, clipRectangle.y, clipRectangle.width, clipRectangle.height},
            {1.0F, 1.0F, 1.0F, 1.0F},
            {portion, 0.0F, 1.0F, 1.0F},
            {(chest || directional) ? 4.25F : 4.0F, chest ? kChestItemTopLayer : topLayer,
             chest ? kChestItemFrontLayer : frontLayer,
             chest ? kChestItemSideLayer : (directional ? directionalFaces.side : textures.side)},
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
            // 走不走立方体图标由 world::rendersAsCubeItem 单点回答
            // 掉落物、手持物、背包图标三条物品渲染面共用它，不再各自列举 BlockModel
            // 台阶也在集合内，只是按下半砖显示，与 vanilla 的台阶物品渲染一致
            if (world::rendersAsCubeItem(stack.block)) {
                drawHudBlockIcon(commandBuffer, rectangle, stack.block,
                                 world::isSlab(stack.block) ? 1.0F : 0.0F);
                return;
            }
            // 楼梯、墙、栅栏门、按钮、压力板这类异形方块与 vanilla 一样显示 3D 方块图标
            // 只有薄片状的门/活板门物品保持扁平贴图，同样对齐 vanilla 各自的物品渲染
            // 这一层目前是图标独有的：掉落物与手持物没有对应处理，异形方块在那两处仍是扁平贴图
            if (world::isShapedBlockModel(model) && !world::isThinLeafIconModel(model)) {
                drawHudBlockIcon(commandBuffer, rectangle, stack.block);
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

    // 按精灵自己的 26.1 gui.scaling 把它画进 `destination`
    // `scale` 是 GUI 缩放，表示每个 GUI 像素对应多少帧缓冲像素
    // 正是它把精灵声明的像素边框换算成目标长度
    // 于是 3px 的按钮边框在任何按钮宽度下都还是 3 个 GUI 像素，不会跟着位图一起糊开
    // 切片算法本身在 ui::forEachGuiSpriteQuad 里，不含 Vulkan 且有单测
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

    // Screen.renderBackground 用一层竖直渐变压暗每个打开的游戏内界面
    // 顶部为 rgba(16,16,16,0xC0)，底部为 rgba(16,16,16,0xD0)
    // 该渐变在 createGuiTexture() 里烘进 kScreenDimGuiLayer
    void drawScreenDimOverlay(VkCommandBuffer commandBuffer) const {
        drawGuiSprite(commandBuffer,
                      {0.0F, 0.0F, static_cast<float>(swapchainExtent.width),
                       static_cast<float>(swapchainExtent.height)},
                      kScreenDimGuiLayer, {0.0F, 0.0F, 256.0F, 256.0F}, {1.0F, 1.0F, 1.0F, 1.0F});
    }

    // vanilla 的 HUD 用乘性混合（dst * (1 - src)）画暗角贴图：四角压暗画面，中心不受影响
    // 它必须跑在专用的暗角管线上；画完立即重新绑回 HUD 管线，后续 HUD 精灵才保持常规的 alpha 混合
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
        // 对齐到整数帧缓冲像素
        // 窗口最大化或尺寸为奇数时，按钮原点带小数会让最近邻采样偏移不到一个纹素
        // 1px 边框会因此画得粗细不匀，甚至在顶边采到相邻纹素
        const ui::UiRect snapped{std::floor(rectangle.x), std::floor(rectangle.y),
                                 std::floor(rectangle.width + 0.5F),
                                 std::floor(rectangle.height + 0.5F)};
        const GuiWidgetSprite face =
            state == ui::ButtonVisualState::Disabled
                ? GuiWidgetSprite::ButtonDisabled
                : (state == ui::ButtonVisualState::Normal ? GuiWidgetSprite::Button
                                                          : GuiWidgetSprite::ButtonHighlighted);
        // 按下态是把调用方给的色调压暗，而不是写死成灰色，红色的删除按钮因此在各状态下颜色都协调
        const glm::vec4 buttonTint =
            state == ui::ButtonVisualState::Pressed
                ? glm::vec4{tint.r * 0.78F, tint.g * 0.78F, tint.b * 0.78F, 1.0F}
                : tint;
        // 九宫格保证无论排版把按钮拉成什么尺寸，边框都保持声明的像素宽度
        // 两列布局的设置页因此不会把一张 200px 位图抻成边缘发虚的矩形
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
        // 轨道像按钮一样做九宫格
        // 滑块本体按原生 8x20 绘制，此时九宫格等同恒等变换，因此始终精确落在自己的美术上
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

    // 语言文件通过 provider 按 ResourceLocation 解析，不做路径推算

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
        // 即饱和度与明度拉满的 hsvToRgb((1 - spent) / 3, 1, 1)，取值范围只在红绿之间
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

    // 按 widget id 给出本地化并填好数值的标签
    // 作为 MenuBuildContext.labelFor 接进绘制页，于是每个 widget 自带文本，绘制后端不必再推导一遍
    [[nodiscard]] std::string widgetLabel(ui::WidgetId button) const {
        // 每条标签都带英文兜底，缺少对应 vanilla 键的语言也能正常显示
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
        // 标签来源分三处，归类见 ui/WidgetLabels.hpp，那里的 static_assert 保证
        // 每个 WidgetId 恰好属于一类：
        //   循环选项 → OptionCycle 表；点击时步进的是同一行，标签与行为不可能不一致
        //   静态标签 → WidgetLabels 表；只由翻译键决定
        //   运行期标签 → 下面的 switch；要读实时窗口尺寸、当前存档难度或滑块数值
        if (const ui::OptionDesc* option = ui::findCyclingOption(button); option != nullptr) {
            return optionValue(
                translated(option->nameKey, option->nameFallback),
                ui::optionValueLabel(*option, ui::readOption(*option, options),
                                     [this](std::string_view key, std::string_view fallback) {
                                         return translated(key, fallback);
                                     }));
        }
        if (const ui::StaticWidgetLabel* label = ui::findStaticLabel(button); label != nullptr) {
            std::string text = translated(label->key, label->fallback);
            text += label->suffix;
            return text;
        }

        switch (button) {
        case ui::WidgetId::Resolution: {
            // 标签显示实时窗口尺寸，最大化或手动拖拽过的窗口因此读数正确
            // 而不是回显上一次选中的预设
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
        case ui::WidgetId::Difficulty:
            // 只出现在世界内的选项页，此时才有打开的存档
            return optionValue(
                translated("options.difficulty", "Difficulty"),
                translated(gameplay::difficultyTranslationKey(
                               currentSave.has_value() ? currentSave->difficulty
                                                       : gameplay::Difficulty::Normal),
                           gameplay::difficultyName(currentSave.has_value()
                                                        ? currentSave->difficulty
                                                        : gameplay::Difficulty::Normal)));
        case ui::WidgetId::CreateGameMode:
            return optionValue(translated("selectWorld.gameMode", "Game Mode"),
                               gameModeLabel(menuSystem.createWorldGameMode));
        case ui::WidgetId::CreateAllowCommands:
            return optionValue(translated("selectWorld.allowCommands", "Allow Cheats"),
                               toggle(menuSystem.createWorldAllowCommands));
        default:
            // 其余 id 的标签不出自这里：循环选项与静态标签已在上面两张表里返回，
            // 列表行（世界/语言/按键）各自带文本。穷尽性护栏因此不再由 -Wswitch 承担，
            // 而是 WidgetLabels.hpp 的 everyWidgetIdHasExactlyOneLabelSource()——
            // 它检查的是「有没有明确归属」，而不是「有没有在 switch 里写一行」
            return {};
        }
    }

    [[nodiscard]] std::string gameModeLabel(gameplay::GameMode mode) const {
        return mode == gameplay::GameMode::Survival
                   ? translated("selectWorld.gameMode.survival", "survival")
                   : translated("selectWorld.gameMode.creative", "creative");
    }

    // 格式化选项字段之外用到的单参数 vanilla 字符串
    [[nodiscard]] static std::string formatTemplate(std::string text, std::string_view value) {
        const std::array<std::string_view, 1> arguments{value};
        return ui::formatTranslation(text, arguments);
    }

    // 前端界面标题
    // 编辑页显示所选世界的名字，与 vanilla 的"编辑世界"界面一致
    // 删除确认页用 vanilla 的删除询问句作标题
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

    // 26.1 的菜单背景是从全景立方体内部以 85 度透视看出去
    // 相机缓慢转动：偏航在 kCycleSeconds 内转满 360°，六个面各自都有较长时间正对视野
    // 俯仰做一次轻微扫掠，下探到 panorama_4、上仰到 panorama_5
    // 再叠一点 vanilla 式的正弦微晃，免得太机械
    // 之后那层暗色四边形保证白色标题和菜单按钮在场景上仍然清晰
    void drawTitleCarousel(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet,
                           bool blurred, float guiScale) const {
        // 每五分钟转满一圈，四个侧面各自正对视野一分多钟
        // 俯仰每圈扫掠一次，vanilla 式微晃的速率也按这个较慢的节奏配
        constexpr double kCycleSeconds = 300.0;
        constexpr double kPi = 3.14159265358979323846;
        const double progress = uiTimeSeconds / kCycleSeconds;
        // 整圈偏航加一点正弦微晃；俯仰每圈振荡一次
        const float yaw = static_cast<float>(progress * 2.0 * kPi) +
                          static_cast<float>(std::sin(uiTimeSeconds * 0.024) * 0.04);
        const float pitch = static_cast<float>(std::sin(progress * 2.0 * kPi)) * 0.44F;
        const float tanHalfFov = std::tan(static_cast<float>(85.0 * kPi / 180.0) * 0.5F);
        const float aspect =
            static_cast<float>(swapchainExtent.width) / static_cast<float>(swapchainExtent.height);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, panoramaPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                panoramaPipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
        // 菜单背景模糊半径，单位为帧缓冲像素
        // 26.1 的 Screen.renderBlurredBackground 由 `menuBackgroundBlurriness` 选项换算出半径
        // 该选项默认 0.5，出厂默认半径因此落在 5
        // 留成具名常量，是因为确切的半径与卷积核属于要肉眼判定的视觉参数
        // headless 判不了，调它时只需改这一个值
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
            // 26.1 里模糊之后紧跟 Screen.extractMenuBackground
            // 这里坚持用资源包提供的真实纹理，而不是把它当前的半透明黑像素写死进代码
            drawGuiSprite(commandBuffer, fullScreen, 9.0F,
                          ui::tiledBackgroundSource(fullScreen.width, fullScreen.height, guiScale));
        } else {
            drawHudQuad(commandBuffer, fullScreen, {0.0F, 0.0F, 0.0F, 0.30F});
        }
    }

    // 通用的菜单绘制后端：按 widget 种类画出一页
    // 每个 widget 自带矩形、标签、启用状态和（滑块的）显示值
    // 按下高亮对应 id 等于 pressedMenuButton 的那个，删除确认按钮保留红色调
    // 列表行走专门的列表路径，不在这里画
    void drawMenuWidgets(VkCommandBuffer commandBuffer, const ui::Page& widgets,
                         float scale) const {
        const auto cursor = currentFramebufferCursor();
        for (const auto& widget : widgets) {
            // 按键设置的每一行都是 ListRow，画成带悬停高亮的"动作: 按键"行
            // 样子对齐 vanilla 的列表项，而不是完整的按钮边框
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

    // 按键设置的一行："动作: 按键"标签配一层淡背景，悬停时提亮，与 vanilla 的按键列表一致
    // 点击该行即开始捕获新按键
    void drawKeyBindRow(VkCommandBuffer commandBuffer, const ui::Widget& widget, float cursorX,
                        float cursorY, float scale) const {
        const bool hovered = widget.rect.contains(cursorX, cursorY);
        drawHudQuad(commandBuffer, widget.rect,
                    hovered ? glm::vec4{0.28F, 0.28F, 0.32F, 0.9F}
                            : glm::vec4{0.0F, 0.0F, 0.0F, 0.55F});
        drawHudText(commandBuffer, widget.label, widget.rect.x + 4.0F * scale,
                    widget.rect.y + 1.5F * scale, scale, {1.0F, 1.0F, 1.0F, 1.0F}, false);
    }

    // 按键设置列表的滚动条：仅当动作数多于可见窗口时绘制
    // 滑块长度对应可见比例，随滚动偏移移动（世界列表与语言列表同理）
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
        // 与 26.1 的 Screen.extractBackground() 一致：所有无世界界面都保持全景在转
        // 二级界面只模糊背景，其文本、按钮和列表行在之后绘制，保持清晰
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
            // 26.1 的列表背景与周围菜单背景是两张可各自被资源包覆盖的贴图
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

    // 快捷栏左侧的生命值、右侧的饥饿值，潜水时在饥饿行上方再加一行氧气
    void drawSurvivalStatusBars(VkCommandBuffer commandBuffer, const ui::HudLayout& layout) const {
        const float scale = layout.scale();
        const auto hotbar = layout.hotbarBackground();
        const float left = hotbar.x;
        const float right = hotbar.x + hotbar.width;
        const float top = hotbar.y - 17.0F * scale;
        const float icon = 9.0F * scale;
        const float step = 8.0F * scale;
        const auto iconRect = [&](float x, float y) { return ui::UiRect{x, y, icon, icon}; };
        // 刚受过伤时，空心用闪白的心形容器代替
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
        // 护甲行：护甲点数（0-20，每图标两点）用十个图标画在生命值上一行，同样左对齐
        // 数值由客户端从镜像过来的装备槽求和，与 LivingEntity#getArmor 累加四件护甲的修饰值同法
        // 因此无需改动传输格式，装备本来就为背包界面同步到客户端
        // 护甲为零时整行隐藏，与 vanilla 一致
        int armorPoints = 0;
        for (std::size_t slot = 0; slot < 4U; ++slot) {
            const auto& piece = clientMirror.world().equipmentSlots[static_cast<std::size_t>(
                gameplay::equipmentSlotAt(slot))];
            armorPoints += static_cast<int>(gameplay::armorValue(piece.item));
        }
        if (armorPoints > 0) {
            const float armorTop = top - 10.0F * scale;
            for (int index = 0; index < 10; ++index) {
                const auto rectangle = iconRect(left + static_cast<float>(index) * step, armorTop);
                drawGuiSprite(commandBuffer, rectangle, 1.0F, {16.0F, 9.0F, 9.0F, 9.0F});
                if (index * 2 + 1 < armorPoints) {
                    drawGuiSprite(commandBuffer, rectangle, 1.0F, {34.0F, 9.0F, 9.0F, 9.0F});
                } else if (index * 2 + 1 == armorPoints) {
                    drawGuiSprite(commandBuffer, rectangle, 1.0F, {25.0F, 9.0F, 9.0F, 9.0F});
                }
            }
        }
    }

    // 经验条为 182x5，居中于快捷栏上方 7 个逻辑像素处
    // 填充比例取玩家真实的经验进度，由 uiFrameData_ 从 tick 快照带来
    // 绿色等级数字画在它正上方
    void drawExperienceBar(VkCommandBuffer commandBuffer, const ui::HudLayout& layout) const {
        const float scale = layout.scale();
        const auto bar = layout.experienceBar();
        // 先画 26.1 具名的经验条背景，再画绿色进度贴图
        drawGuiSprite(commandBuffer, bar, 1.0F, {0.0F, 64.0F, 182.0F, 5.0F});
        const float progress = std::clamp(uiFrameData_.experienceProgress, 0.0F, 1.0F);
        if (progress > 0.0F) {
            // 未满时只采样贴图前若干列，与 vanilla 的 blit(x, y, 0, 69, progressWidth, 5) 一致
            const float filledWidth = progress * 182.0F * scale;
            drawGuiSprite(commandBuffer, {bar.x, bar.y, filledWidth, bar.height}, 1.0F,
                          {0.0F, 69.0F, progress * 182.0F, 5.0F});
        }
        // 等级数字只有在玩家真正离开 0 级后才绘制
        // 26.1 的判据是 `hasExperience() && experienceLevel > 0`
        // 生存模式刚出生时因此只有一条空经验条，上面不会飘一个 "0"
        if (uiFrameData_.experienceLevel > 0) {
            const std::string label = std::to_string(uiFrameData_.experienceLevel);
            const float textWidth = hudTextWidth(label, scale);
            const float textX = bar.x + (bar.width - textWidth) * 0.5F;
            // 26.1 取 y = guiHeight - 24 - 9 - 2，即经验条顶边之上 6 个逻辑像素
            // 经验条顶边本身是 guiHeight - 24 - 5
            // 这里相对 `bar` 表达，与 HudLayout::experienceBar() 用同一个锚点
            const float textY = bar.y - 6.0F * scale;
            // vanilla 在绿色字面之前先画四向黑色描边，而不是常见的单向投影阴影
            // 数字因此压在空的和满的经验条上都看得清
            constexpr glm::vec4 kOutline{0.0F, 0.0F, 0.0F, 1.0F};
            drawHudText(commandBuffer, label, textX + scale, textY, scale, kOutline, false);
            drawHudText(commandBuffer, label, textX - scale, textY, scale, kOutline, false);
            drawHudText(commandBuffer, label, textX, textY + scale, scale, kOutline, false);
            drawHudText(commandBuffer, label, textX, textY - scale, scale, kOutline, false);
            constexpr glm::vec4 kLevelGreen{0.5019608F, 1.0F, 0.1254902F, 1.0F};
            drawHudText(commandBuffer, label, textX, textY, scale, kLevelGreen, false);
        }
    }

    // 暗角强度每 tick 以 1% 的速度趋近 clamp(1 - 眼部亮度, 0, 1)
    // 亮度取主世界的光照曲线 g/(4 - 3g)，其中 g = 光照等级 / 15
    // 天光按天空着色器同一个日照系数衰减，因此夜里和洞穴里同样会出现暗角
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

    // 受伤染色：简化成一层全屏红色，在无敌帧窗口内淡出
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

    // 26.1 风格的语言界面：点击行只更新草稿选择，按 Done 才提交一次异步资源重载
    void drawLanguageScreen(VkCommandBuffer commandBuffer, const ui::HudLayout& layout) const {
        const auto cursor = currentFramebufferCursor();
        const float scale = layout.scale();
        const std::string title = translated("options.language.title", "Language");
        drawHudText(commandBuffer, title,
                    (static_cast<float>(swapchainExtent.width) - hudTextWidth(title, scale)) * 0.5F,
                    14.0F * scale, scale, {1.0F, 1.0F, 1.0F, 1.0F});
        // 居中的深色列表框用的是 26.1 中可被单独替换的列表背景
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
            // 在框内居中，与 vanilla 语言项的绘制一致：每个名字画在 width/2 - 文本宽/2
            drawHudText(commandBuffer, name,
                        rectangle.x + (rectangle.width - hudTextWidth(name, scale)) * 0.5F,
                        rectangle.y + 2.0F * scale, scale,
                        selected ? glm::vec4{1.0F, 1.0F, 1.0F, 1.0F}
                                 : glm::vec4{0.85F, 0.85F, 0.85F, 1.0F});
        }
        // 列表超出时在框右缘画滚动滑块，对应 vanilla 列表控件的灰色轨道
        if (menuSystem.languageCodes.size() > visible) {
            const auto thumb = ui::languageScrollbarThumb(
                layout, static_cast<float>(swapchainExtent.width),
                menuSystem.languageCodes.size(), visible, first);
            drawHudQuad(commandBuffer, thumb,
                        {0.55F, 0.55F, 0.55F, 0.95F});
        }
        // 列表与按钮之间的灰色提示行，vanilla 把它画在 height - 56 处
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
        // 暂停界面走 Screen.renderBackground()，在冻结的世界上铺一层与背包界面相同的深灰渐变
        // 死亡界面改用暗红色底衬；从标题界面（无世界）打开的选项界面则与其它菜单一样显示普通底衬
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
        // 按键设置是三段式布局，标题属于顶部那一段，位于滚动列表上方
        // 它不在底部按钮带上方 30px 处，那里是 firstButton 所在的位置
        // 其余页面仍把标题放在第一个按钮之上
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
        // 按键设置列表（中段）的滚动条，仅当动作数超出可见窗口时绘制
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
        // vanilla 绘制实体时的 Y 坐标是脚底锚点
        // 而本项目的长方体坐标系以原点为中心向两侧展开 -16..+16
        // 因此投影到视图空间之前要先把锚点换算成模型中心
        // vanilla 模型每方块 16 单位，于是缩放系数把 1 个模型单位映射为 entityScale / 16 像素
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
        // 预览与世界中的玩家走同一套骨骼姿态
        // 每根骨骼的方块按 modelRoot * boneWorld * cubeRotation * T(中心) 绘制
        // 头和手臂作为身体的子节点，骨骼层级因此能正确复合
        // 整个人随光标视线整体转动，而不是各部件各转各的
        // `origin` 位于相机视图空间，因此这里用矩阵长方体的 matrixViewModel 模式
        // 即 data.x=6，直接经 camera.projection 投影，无需第二次视图变换
        const auto& previewModel = playerModelAnimator.model();
        const auto& skeletonPose = playerModelAnimator.skeletonPose();
        // 几何体的脚位于模型 y=0，而版面锚点期望脚在 `origin` 下方 16 单位处
        // 所以把模型根节点整体下移，让人物落在同一个位置
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

    // 四个护甲槽加副手，随玩家背包出现在哪里就画在哪里（生存背包界面与创造模式的背包页签）
    // 屏幕顺序到矩形、屏幕顺序到 EquipmentSlot 两处映射与 ScreenHandler::appendEquipmentSlots 相同
    // 显示与点击因此一致
    // 返回鼠标悬停的物品堆（若有），好让调用方的提示框也覆盖护甲槽
    [[nodiscard]] std::optional<gameplay::ItemStack>
    drawEquipmentSlots(VkCommandBuffer commandBuffer, const ui::HudLayout& layout, float cursorX,
                       float cursorY, bool creative) const {
        std::optional<gameplay::ItemStack> hovered;
        for (std::size_t index = 0; index < gameplay::kEquipmentScreenSlotCount; ++index) {
            const auto rect =
                index < 4U ? layout.armorSlot(index, creative) : layout.offhandSlot(creative);
            const auto& stack = clientMirror.world().equipmentSlots[static_cast<std::size_t>(
                gameplay::equipmentSlotAt(index))];
            const bool isHovered = rect.contains(cursorX, cursorY);
            if (isHovered && !stack.empty()) {
                hovered = stack;
            }
            drawHudSlot(commandBuffer, rect, stack, false, isHovered, true);
        }
        return hovered;
    }

    // ENCH-2: EnchantmentScreen#extractBackground, transcribed. Every offset here
    // is vanilla's, panel-relative: the two slots at (15,47)/(35,47), the three
    // 108x19 option bars at (60, 14+19i), the 16x16 level numeral at
    // (61, 15+19i), the galactic phrase at (80, 16+19i) clipped to
    // `86 - costTextWidth`, and the cost number right-aligned at
    // (80+86, 16+19i+7).
    //
    // Whether a bar reads as affordable is a *client* judgement drawn from the
    // snapshot (level, lapis count, game mode) purely so the bar can grey out;
    // the authority is GameSession::purchaseEnchantment, which re-checks all of
    // it. A client that drew a bar bright would still be refused.
    void drawEnchantingScreen(VkCommandBuffer commandBuffer, const ui::HudLayout& layout,
                              const ui::UiRect& panel) const {
        const auto& snap = clientMirror.world();
        const float scale = layout.scale();
        const auto cursor = currentFramebufferCursor();
        drawHudText(commandBuffer, translated("container.enchant", "Enchant"),
                    panel.x + 8.0F * scale, panel.y + 6.0F * scale, scale,
                    {0.25F, 0.25F, 0.25F, 1.0F}, false);
        drawHudText(commandBuffer, translated("container.inventory", "Inventory"),
                    panel.x + 8.0F * scale, panel.y + 73.0F * scale, scale,
                    {0.25F, 0.25F, 0.25F, 1.0F}, false);
        drawHudSlot(commandBuffer, layout.enchantingItemSlot(), snap.enchantingItem, false, false,
                    true);
        drawHudSlot(commandBuffer, layout.enchantingLapisSlot(), snap.enchantingLapis, false,
                    false, true);

        const bool infiniteMaterials = uiFrameData_.gameMode == gameplay::GameMode::Creative;
        const int lapisCount = static_cast<int>(snap.enchantingLapis.count);
        // One RandomSource for the whole screen, seeded from the enchantment
        // seed and advanced only by the bars that are live — EnchantmentNames'
        // initSeed + the `cost == 0` early-out, in that exact order, so the
        // three phrases match vanilla's for the same seed.
        world::gen::JavaRandom nameRandom(static_cast<std::uint64_t>(snap.enchantingSeed));
        for (std::size_t option = 0; option < 3U; ++option) {
            const auto bar = layout.enchantingOption(option);
            const std::int32_t cost = snap.enchantingRequiredLevels[option];
            const auto lapisCost = static_cast<int>(option) + 1;
            if (cost == 0) {
                drawGuiSprite(commandBuffer, bar, kEnchantingGuiLayer,
                              {0.0F, static_cast<float>(kEnchantingBarSpriteY + 20), 108.0F,
                               19.0F});
                continue;
            }
            const std::string costText = std::to_string(cost);
            const float costWidth = hudTextWidth(costText, scale);
            const float phraseWidth = 86.0F * scale - costWidth;
            const std::string phrase = ui::toGalactic(ui::randomEnchantmentName(nameRandom));
            const bool affordable =
                infiniteMaterials ||
                (lapisCount >= lapisCost && uiFrameData_.experienceLevel >= cost &&
                 uiFrameData_.experienceLevel >= lapisCost);
            const bool hovered = affordable && bar.contains(cursor.x, cursor.y);
            // Bar state, level numeral row and the two text colours all follow
            // from affordable/hovered — vanilla's three branches, one table.
            const float barSpriteY = static_cast<float>(
                kEnchantingBarSpriteY + (affordable ? (hovered ? 40 : 0) : 20));
            const float numeralRow =
                static_cast<float>(kEnchantingLevelSpriteY + (affordable ? 0 : 16));
            const glm::vec4 phraseColor =
                affordable ? (hovered ? glm::vec4{1.0F, 1.0F, 0.502F, 1.0F}
                                      : glm::vec4{0.408F, 0.369F, 0.290F, 1.0F})
                           : glm::vec4{0.204F, 0.184F, 0.145F, 1.0F};
            const glm::vec4 costColor = affordable ? glm::vec4{0.502F, 1.0F, 0.125F, 1.0F}
                                                   : glm::vec4{0.251F, 0.498F, 0.063F, 1.0F};
            drawGuiSprite(commandBuffer, bar, kEnchantingGuiLayer,
                          {0.0F, barSpriteY, 108.0F, 19.0F});
            drawGuiSprite(commandBuffer,
                          {panel.x + 61.0F * scale, panel.y + (15.0F + 19.0F * static_cast<float>(option)) * scale,
                           16.0F * scale, 16.0F * scale},
                          kEnchantingGuiLayer,
                          {static_cast<float>(kEnchantingLevelSpriteX + 16 * static_cast<int>(option)),
                           numeralRow, 16.0F, 16.0F});
            drawHudText(commandBuffer, clipTextToWidth(phrase, scale, phraseWidth),
                        panel.x + 80.0F * scale,
                        panel.y + (16.0F + 19.0F * static_cast<float>(option)) * scale, scale,
                        phraseColor, false);
            drawHudText(commandBuffer, costText, panel.x + (80.0F + 86.0F) * scale - costWidth,
                        panel.y + (23.0F + 19.0F * static_cast<float>(option)) * scale, scale,
                        costColor, false);
            // EnchantmentScreen#extractRenderState's hover tooltip: the ONE
            // revealed enchantment ("Sharpness . . . ?" — vanilla never shows
            // the level of the clue's siblings, and neither do we), then the
            // two price lines. Vanilla hovers a 17px-tall band, one shorter
            // than the bar it draws.
            const ui::UiRect clueBand{bar.x, bar.y, bar.width, 17.0F * scale};
            if (snap.enchantingClueLevels[option] > 0U &&
                clueBand.contains(cursor.x, cursor.y)) {
                drawEnchantingClueTooltip(commandBuffer, scale, option, cost, lapisCost,
                                          infiniteMaterials);
            }
        }
    }

    // The hover tooltip over a live option bar. Kept out of the loop above
    // because it draws on top of every bar, not inside one.
    void drawEnchantingClueTooltip(VkCommandBuffer commandBuffer, float scale, std::size_t option,
                                   std::int32_t cost, int lapisCost,
                                   bool infiniteMaterials) const {
        const auto& snap = clientMirror.world();
        const auto clueId =
            static_cast<gameplay::EnchantmentId>(snap.enchantingClueIds[option]);
        std::string clueName{translated(
            "enchantment.minecraft." + std::string{gameplay::enchantmentVanillaName(clueId)},
            std::string{gameplay::enchantmentVanillaName(clueId)})};
        const auto clueLevel = static_cast<int>(snap.enchantingClueLevels[option]);
        // Enchantment.getFullname: the level numeral is omitted for a level-1
        // enchantment whose maximum is also 1 (Silk Touch is "Silk Touch", not
        // "Silk Touch I"), and drawn for every other.
        if (clueLevel > 1 || gameplay::enchantmentDefinition(clueId).maxLevel > 1) {
            clueName += ' ';
            clueName += translated("enchantment.level." + std::to_string(clueLevel),
                                   romanNumeral(clueLevel));
        }
        std::vector<std::string> lines;
        lines.push_back(formatTemplate(translated("container.enchant.clue", "%s . . . ?"),
                                       clueName));
        if (!infiniteMaterials) {
            if (uiFrameData_.experienceLevel < cost) {
                lines.push_back(
                    formatTemplate(translated("container.enchant.level.requirement",
                                              "Level Requirement: %s"),
                                   std::to_string(cost)));
            } else {
                lines.push_back(lapisCost == 1
                                    ? translated("container.enchant.lapis.one", "1 Lapis Lazuli")
                                    : formatTemplate(
                                          translated("container.enchant.lapis.many",
                                                     "%s Lapis Lazuli"),
                                          std::to_string(lapisCost)));
                lines.push_back(
                    lapisCost == 1
                        ? translated("container.enchant.level.one", "1 Enchantment Level")
                        : formatTemplate(translated("container.enchant.level.many",
                                                    "%s Enchantment Levels"),
                                         std::to_string(lapisCost)));
            }
        }
        const auto cursor = currentFramebufferCursor();
        float widest = 0.0F;
        for (const auto& line : lines) {
            widest = std::max(widest, hudTextWidth(line, scale));
        }
        const ui::UiRect box{cursor.x + 12.0F * scale, cursor.y + 12.0F * scale,
                             widest + 8.0F * scale,
                             (4.0F + 10.0F * static_cast<float>(lines.size())) * scale};
        drawHudQuad(commandBuffer, box, {0.05F, 0.03F, 0.08F, 0.94F});
        for (std::size_t line = 0; line < lines.size(); ++line) {
            drawHudText(commandBuffer, lines[line], box.x + 4.0F * scale,
                        box.y + (3.0F + 10.0F * static_cast<float>(line)) * scale, scale,
                        line == 0U ? glm::vec4{1.0F, 1.0F, 1.0F, 1.0F}
                                   : glm::vec4{0.667F, 0.667F, 0.667F, 1.0F});
        }
    }

    // enchantment.level.<n>'s fallback when the language file has no entry: the
    // Roman numeral vanilla's own keys spell out. Only 1..10 are ever needed
    // (no enchantment goes higher), and anything past that falls back to the
    // decimal so a datapack level can still be read.
    [[nodiscard]] static std::string romanNumeral(int level) {
        static constexpr std::array<std::string_view, 10> kNumerals{
            "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X"};
        if (level >= 1 && level <= static_cast<int>(kNumerals.size())) {
            return std::string{kNumerals[static_cast<std::size_t>(level - 1)]};
        }
        return std::to_string(level);
    }

    // Font#getSplitter().headByWidth: the longest prefix of `text` that fits, cut
    // on whole codepoints so a multi-byte glyph is never sliced in half.
    [[nodiscard]] std::string clipTextToWidth(std::string_view text, float scale,
                                              float maxWidth) const {
        if (hudTextWidth(text, scale) <= maxWidth) {
            return std::string{text};
        }
        std::string fitted;
        for (const char32_t codepoint : ui::decodeUtf8(text)) {
            std::string candidate = fitted;
            ui::appendUtf8(candidate, codepoint);
            if (hudTextWidth(candidate, scale) > maxWidth) {
                break;
            }
            fitted = std::move(candidate);
        }
        return fitted;
    }

    void drawWorkContainer(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet,
                           const ui::HudLayout& layout) const {
        drawScreenDimOverlay(commandBuffer);
        const auto panel = layout.inventoryPanel();
        const bool chestScreen = containerScreen == ContainerScreen::Chest;
        const float panelLayer =
            chestScreen                                              ? 10.0F
            : containerScreen == ContainerScreen::CraftingTable       ? 7.0F
            : containerScreen == ContainerScreen::EnchantingTable     ? kEnchantingGuiLayer
                                                                      : 8.0F;
        drawGuiSprite(commandBuffer, panel, panelLayer, {0.0F, 0.0F, 176.0F, 166.0F});
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
        } else if (containerScreen == ContainerScreen::EnchantingTable) {
            drawEnchantingScreen(commandBuffer, layout, panel);
        } else {
            // 熔炉界面按容器显示快照绘制，这里不读方块实体的位置
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
        // 拖拽过程中在每个划过的槽位预览松手后的落位，画在槽位之上、光标之下
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
        // 前七个页签（建筑方块…战斗）在上排；食物、原料、刷怪蛋和背包在下排，用下排页签贴图
        const std::size_t firstBottomTab = static_cast<std::size_t>(ui::CreativeTab::FoodAndDrink);
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
        // 每个页签一个代表图标，顺序同 CreativeTab
        const std::array<gameplay::ItemStack, kCreativeTabCount> tabIcons{{
            {world::Block::Bricks, 1U},                                 // BuildingBlocks
            {world::Block::WhiteWool, 1U},                              // ColoredBlocks
            {world::Block::Dirt, 1U},                                   // NaturalBlocks
            {world::Block::CraftingTable, 1U},                          // Functional
            {world::Block::RedstoneBlock, 1U},                          // Redstone
            {world::Block::Air, 1U, &gameplay::items::DiamondPickaxe},  // Tools
            {world::Block::Air, 1U, &gameplay::items::IronSword},       // Combat
            {world::Block::Air, 1U, &gameplay::items::Apple},           // FoodAndDrink
            {world::Block::Air, 1U, &gameplay::items::IronIngot},       // Ingredients
            {world::Block::Air, 1U, &gameplay::items::PigSpawnEgg},     // SpawnEggs
            {world::Block::Chest, 1U},                                  // Inventory
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
            // 创造模式的背包页签显示与生存相同的护甲与副手槽
            // 但它锚定在创造面板上，免得被两种面板不同的尺寸与中心带偏
            if (const auto hoveredEquipment = drawEquipmentSlots(commandBuffer, layout, cursor.x,
                                                                 cursor.y, /*creative=*/true)) {
                hoveredStack = hoveredEquipment;
            }
            const auto deleteSlot = layout.creativeDeleteSlot();
            if (deleteSlot.contains(cursor.x, cursor.y)) {
                drawHudQuad(commandBuffer, deleteSlot, {1.0F, 0.25F, 0.25F, 0.34F});
            }
        } else {
            // 26.1 的十个内容页签，顺序同 CreativeTab（下标 0..9；背包页签由上面的分支处理）
            constexpr std::array<std::pair<std::string_view, std::string_view>, 10> titles{{
                {"itemGroup.buildingBlocks", "Building Blocks"},
                {"itemGroup.coloredBlocks", "Colored Blocks"},
                {"itemGroup.natural", "Natural Blocks"},
                {"itemGroup.functional", "Functional Blocks"},
                {"itemGroup.redstone", "Redstone Blocks"},
                {"itemGroup.tools", "Tools & Utilities"},
                {"itemGroup.combat", "Combat"},
                {"itemGroup.foodAndDrink", "Food & Drinks"},
                {"itemGroup.ingredients", "Ingredients"},
                {"itemGroup.spawnEggs", "Spawn Eggs"},
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

        // 创造模式的真实背包/快捷栏槽位与生存共用快速合成拖拽，松手前同样显示每格的预计落位数量
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
        // vanilla 的聊天把每条消息按固定的 320 个未缩放 GUI 像素折行
        // 并为每个折行后的行单独存一条记录
        // 本项目的存储按逻辑行组织，折行放到绘制时做，于是 GUI 缩放或窗口宽度一变，折行自动重排
        // 度量用未缩放 GUI 像素（scale 1）以对齐那个宽度，缩放只在绘制时施加
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
            // 折行后的文本自上而下阅读，因此自下而上绘制：最后一行贴近输入框，更早的行依次往上堆
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
        // vanilla 的聊天输入框占满屏幕宽度，其编辑框宽度为 windowWidth - 8
        // 所以深色底衬始终从左缘拉到右缘，而不是紧贴已输入的文字
        const ui::UiRect input = layout.chatInput();
        drawHudQuad(commandBuffer, input, {0.0F, 0.0F, 0.0F, 0.72F});
        const bool cursorVisible = static_cast<int>(uiTimeSeconds * 2.0) % 2 == 0;
        const std::string visibleText = chatInputText + (cursorVisible ? "_" : "");
        drawHudText(commandBuffer, visibleText, input.x + 2.0F * scale, input.y + 2.0F * scale,
                    scale, {1.0F, 1.0F, 1.0F, 1.0F}, false);
        // 26.1 的命令补全把候选列表画在同一个不透明深色框里，而不是画成逐行的半透明条
        // 选中行高亮，候选文字为白色，用法提示为灰色
        // 这里照此实现：先按最宽一行画一个背景矩形，再把各行画在其上
        const std::size_t maxRows = std::min<std::size_t>(chatSuggestions_.size(), 8U);
        if (maxRows > 0) {
            const float rowHeight = 11.0F * scale;
            const float boxLeft = 2.0F * scale;
            // 框宽取最宽的"候选 + 提示"行，保证不透明面板托住每一行
            // vanilla 的提示框同样按内容定宽
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
            // 26.1 的不透明提示框底色（近黑面板，alpha 约 0.95）
            drawHudQuad(commandBuffer,
                        {boxLeft, boxTop, boxWidth, static_cast<float>(maxRows) * rowHeight},
                        {0.05F, 0.05F, 0.05F, 0.95F});
            for (std::size_t row = 0; row < maxRows; ++row) {
                const auto& suggestion = chatSuggestions_[row];
                const float rowY = input.y - (static_cast<float>(row) + 1.0F) * rowHeight;
                if (row == chatSuggestionIndex_) {
                    // 选中行的高亮条
                    drawHudQuad(commandBuffer, {boxLeft, rowY, boxWidth, rowHeight},
                                {0.10F, 0.10F, 0.10F, 1.0F});
                }
                std::string hint = suggestion.hint;
                if (hint.starts_with("item.") || hint.starts_with("block.")) {
                    hint = translated(hint, hint);
                }
                // 候选文字：选中为白色，否则为浅灰
                const glm::vec4 textColor = row == chatSuggestionIndex_
                                                ? glm::vec4{1.0F, 1.0F, 1.0F, 1.0F}
                                                : glm::vec4{0.66F, 0.66F, 0.66F, 1.0F};
                drawHudText(commandBuffer, suggestion.text, boxLeft + 2.0F * scale, rowY + scale,
                            scale, textColor, false);
                // 用法提示以灰色跟在候选文字之后（26.1 的用法提示配色）
                if (!hint.empty()) {
                    const float hintX =
                        boxLeft + 2.0F * scale + hudTextWidth(suggestion.text + "  ", scale);
                    drawHudText(commandBuffer, hint, hintX, rowY + scale, scale,
                                {0.53F, 0.53F, 0.53F, 1.0F}, false);
                }
            }
        }
    }

    // 右上角的吐司提示叠加层（对应 26.1 的 ToastComponent）
    // 每条可见提示是一块不透明面板，按 slideFraction 从右侧滑入，白色标题配灰色副标题
    // 生命周期、条数上限和动画都在不含 Vulkan 的 ui::ToastQueue 里
    // 这里只负责画出它当前的 visibleToasts()
    void drawToastOverlay(VkCommandBuffer commandBuffer, const ui::HudLayout& layout) const {
        const float scale = layout.scale();
        const float toastWidth = 160.0F * scale;
        const float toastHeight = 28.0F * scale;
        const float margin = 4.0F * scale;
        const float right = static_cast<float>(swapchainExtent.width);
        float slotY = margin;
        for (const ui::ActiveToast& active : toastQueue.visibleToasts()) {
            // slideFraction 为 1 表示完全滑入，0 表示还在右缘之外
            const float x = right - active.slideFraction * (toastWidth + margin);
            // 提示画在 GUI 图集里九宫格的控件面板也就是按钮边框上，而不是一块纯灰四边形
            // 这样才有真正的带边框背景
            // vanilla 专用的 toasts.png 不在随包资源里，控件面板是最接近的现成贴图
            // 将来若补上该贴图，换掉这里即可
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

    // 右下角的音效字幕叠加层（对应 26.1 的 SubtitleOverlay）
    // 每条生效的字幕右对齐地叠在上一条之上，按各自 alpha 淡出
    // 列表、上限与淡出都在不含 Vulkan 的 ui::SubtitleFeed 里，这里只负责画
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

    // 整个 HUD 收在同一层里按 vanilla 顺序绘制
    // 依次是受伤染色、暗角、第一人称手持物、快捷栏、生存状态条、准星、手持物名称
    // 之后 drawHud 才把背包、容器、暂停这些打开的界面画在上面
    // 它的背景渐变因此能均匀压暗整层，不会让个别元素亮着浮在叠加层之上
    void drawInGameHudLayer(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet,
                            const ui::HudLayout& layout) const {
        // vanilla 在世界之后、用一张干净的深度缓冲绘制手臂，随后才是 HUD（暗角、快捷栏等）
        // 所以手臂位于 HUD 层的最底部：受伤染色、暗角以及任何打开界面的渐变都画在它之上
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
        // drawHeldItem 是经 itemPipelineLayout（128 字节顶点推送区间）绑定 set 0 的
        // 仅仅切回 HUD 管线不够，它 64 字节的顶点加片元布局与之不兼容
        // 受伤四边形的第一次绘制因此必须经 hudPipelineLayout 重新绑定共享描述符集
        // 水下叠加层自己会做这件事，但它是有条件才画的
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipelineLayout,
                                0, 1, &descriptorSet, 0, nullptr);

        drawDamageOverlay(commandBuffer);
        drawVignette(commandBuffer, descriptorSet);

        // HUD 快捷栏与生存状态条
        // 玩家背包界面在生存和创造下都保留屏幕上的快捷栏
        // 容器界面维持原有外观
        // 状态条仅生存模式显示
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
            // 手持物名称：切换选中格时出现，两秒内淡出；空手什么都不显示（不会出现"空气"字样）
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
                    // 前 1.5 秒全亮，随后半秒淡出，与 vanilla 的高亮收尾一致
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

        // 准星：vanilla 在游戏中每帧绘制，打开界面时它和整层一起被渐变压暗
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
            // 按实时帧缓冲尺寸换算，而不是交换链范围
            // 窗口刚缩放或最大化时范围还停留在上一帧，直到交换链重建为止
            // 那期间光标和白色槽位高亮会偏几个像素
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
            const auto hoveredEquipment =
                drawEquipmentSlots(commandBuffer, layout, cursorX, cursorY, /*creative=*/false);
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
            // 光标下既可能是主背包槽，也可能是护甲/副手槽，提示框两者都覆盖
            std::optional<gameplay::ItemStack> tooltipStack = hoveredEquipment;
            if (!tooltipStack.has_value() && hoveredSlot.has_value() &&
                !clientMirror.world().inventorySlots[*hoveredSlot].empty()) {
                tooltipStack = clientMirror.world().inventorySlots[*hoveredSlot];
            }
            if (tooltipStack.has_value()) {
                const auto& hoveredStack = *tooltipStack;
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
            // 拖拽过程中在每个划过的槽位预览松手后的落位，画在槽位之上、光标之下
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
            // vanilla 的调试信息采样玩家脚所在的方块，而静止玩家的脚正好落在整数边界上
            // 于是 floor() 取到的是地面方块上方那格空气
            // 本项目让脚停在该边界下方一个碰撞 epsilon 处，直接取整会落进实心方块
            // 那格的方块光照按定义为 0
            // 把这个 epsilon 补回去才能复现 vanilla 的采样点
            const glm::ivec3 playerBlock{
                static_cast<int>(std::floor(debugSnap.physicsCurrent.x)),
                static_cast<int>(std::floor(debugSnap.physicsCurrent.y + 0.001F)),
                static_cast<int>(std::floor(debugSnap.physicsCurrent.z))};
            // 版本行读的是构建身份的唯一来源 core::kVersion，而不是持久化或写死的字符串
            // F3 显示的因此永远是这个二进制自己的版本，并带上 git 构建标识便于诊断
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
        // 游戏内叠加层位于聊天/HUD 之上：右上角吐司提示与右下角音效字幕
        drawToastOverlay(commandBuffer, layout);
        drawSubtitleOverlay(commandBuffer, layout);
    }

    // ---- 绑定到渲染器内核状态的引用 ----
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

    // ---- 与世界渲染/逐帧状态的耦合（绑到 Impl 的 lambda 上）----
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

    // ---- 自持的 UI 动画与选择状态 ----
    // 绘制侧的 Page 装配件，构造时装配一次（见构造函数体与 buildDrawPage）
    // drawPage_ 是每帧重装的输出缓冲，留成成员是为了让它的容量跨帧活下来
    mutable ui::Page drawPage_;
    mutable ui::MenuBuildContext drawContext_;
    ui::MenuCallbacks drawCallbacks_;
    float vignetteDarkness_ = 1.0F;
    mutable std::size_t selectedNameSlot_ = static_cast<std::size_t>(-1);
    mutable gameplay::ItemStack selectedNameStack_;
    mutable double selectedNameShownAt_ = -1.0;
};

} // namespace mc::render
