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
#include "core/BrokenDownTime.hpp"
#include "persistence/SaveRepository.hpp"
#include "ui/WorldListRow.hpp"
#include "render/SkyLight.hpp"
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
#include "ui/SplashText.hpp"
#include "ui/TextMetrics.hpp"
#include "ui/TitleScreenLayout.hpp"
#include "ui/TooltipLayout.hpp"
#include "ui/SubtitleFeed.hpp"
#include "ui/Toast.hpp"
#include "gameplay/SnapshotSlots.hpp"
#include "ui/ContainerPage.hpp"
#include "ui/HudLayout.hpp"
#include "ui/ScrollingText.hpp"
#include "ui/SliderGeometry.hpp"
#include "ui/ItemTooltip.hpp"
#include "ui/Language.hpp"
#include "ui/MenuGeometry.hpp"
#include "ui/MenuSystem.hpp"
#include "assets/ResourcePackLibrary.hpp"
#include "ui/CreateWorldLayout.hpp"
#include "ui/DualColumnList.hpp"
#include "ui/HeaderAndFooterLayout.hpp"
#include "ui/KeyBindList.hpp"
#include "ui/ListRow.hpp"
#include "ui/OptionSlider.hpp"
#include "ui/PageBuilder.hpp"
#include "ui/PageLayoutKind.hpp"
#include "ui/PageTitles.hpp"
#include "ui/PageStack.hpp"
#include "ui/TextField.hpp"
#include "ui/TextFont.hpp"
#include "ui/TextWrap.hpp"
#include "ui/UiFrameData.hpp"
#include "world/ChunkStreamer.hpp"
#include "world/ItemModel.hpp"
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
#include <ctime>
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
    // The per-field look of a text box. Everything about WHAT the field holds
    // lives in ui::TextFieldState/Rules; this is only how it is painted.
    struct TextFieldStyle final {
        // vanilla's bordered EditBox. Chat draws a full-width strip instead, and
        // the anvil's field art comes with the container background — both pass
        // false and supply their own background.
        bool bordered = true;
        glm::vec4 background{0.0F, 0.0F, 0.0F, 0.0F};
        glm::vec4 textColor{0.878F, 0.878F, 0.878F, 1.0F}; // vanilla DEFAULT_TEXT_COLOR
        bool shadow = true;
        // An unfocused field shows its text but no blinking cursor.
        bool focused = true;
        // Grey text drawn after an appending cursor (the command completion's
        // ghost text). Ignored where the cursor is mid-string, as in vanilla.
        std::string suggestion;
    };

    struct Bindings final {
        ui::MenuSystem& menuSystem;
        // UI-6e ③：资源包选择屏要列出包并读草稿顺序。只读——启停与调序走输入侧的回调。
        const assets::ResourcePackLibrary& packLibrary;
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
        // RN-14: the same shaders and layout as hudPipeline with depth test and
        // write ON, for the block item icon — a multi-box item model needs a
        // depth buffer to composite (a wall's post and its arm interpenetrate).
        VkPipeline& hudBlockIconPipeline;
        VkPipelineLayout& hudPipelineLayout;
        VkPipeline& vignettePipeline;
        VkPipeline& crosshairPipeline;
        VkPipeline& panoramaPipeline;
        VkPipelineLayout& panoramaPipelineLayout;
        // UI-5：竖直渐变矩形（26.1 的 fillGradient）。自己的推送常量块，
        // 理由见 HudTypes.hpp 的 GradientPush。
        VkPipeline& gradientPipeline;
        VkPipelineLayout& gradientPipelineLayout;
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
        ui::TextFieldState& chatInput;
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
        // UI-2：标题美术在 binding 6 那张数组里的归一化子矩形
        const TitleArtUv& titleArtUv;
        // UI-2：截图通道钉死的光标位置；空表示照常读 GLFW
        const std::optional<ui::UiPoint>& pinnedCursor;
        // UI-6-0：这一趟是不是界面截图。
        //
        // 不复用 `pinnedCursor.has_value()`：那个字段的含义是"光标被钉住了"，
        // 让它同时兼职"我在拍界面"就是一个字段两个意思——RN-14 让所有方块图标
        // 变成黑菱形的正是这种兼职。
        const bool& uiCaptureActive;
        // UI-13：这一帧被选中去当存档缩略图。26.1 抓的是 `renderLevel` 之后、GUI 之前的
        // 画面（图里没有 HUD），而本作的离屏场景图要整帧录完才读得到——那时 HUD 已经
        // 画上去了。取 HUD 之前的内容要在两趟 pass 之间插一次 copy（动帧图与屏障，归 RN 线），
        // 这里的做法是让被选中的那**一帧**不画 HUD，拍完就恢复。
        // 一个存档一生只发生一次（见 `updateWorldIconRequest`）。
        const bool& worldIconCapturePending;
        bool& paused;
        double& uiTimeSeconds;
        std::function<bool()> cameraSubmergedInWater;
        // 按键设置里每行的两段文字（动作名 / 键名），取自 InputSystem 这一唯一事实源
        // 绘制页因此显示的是实时绑定。一个回调两个字段：填上下文的地方有两处，
        // 拆成两个回调就会漏掉其中一处（"键名汉化了、动作名没有"就是这么来的）
        std::function<ui::MenuBuildContext::KeyBindRowLabels(input::InputAction)> keyBindLabels;
        std::function<void(VkCommandBuffer, VkDescriptorSet)> drawHeldItem;
        std::function<VkDescriptorSet()> currentFrameDescriptorSet;
        // A1：当前打开的那一屏的 ScreenContext。**一处来源**（VulkanRenderer::screenContext()）：
        // 绘制侧自己再拼一份就是同一事实的两份表述，而两份 context 只要有一个字段不一致
        // （比如创造页签），画出来的槽位与点得到的槽位就不是同一批。
        std::function<gameplay::ScreenContext()> screenContext;
        std::function<std::span<const gameplay::ItemStack>()> activeCreativeCatalog;
        std::function<float()> creativeScrollPosition;
        std::function<std::size_t()> creativeMaximumScrollRow;
        std::function<std::vector<std::uint8_t>()> dragPlacementCounts;
        std::function<float()> cameraFarPlane;
        std::function<std::optional<ui::UiRect>(const ui::HudLayout&, const gameplay::SlotRef&)>
            dragSlotRectangle;
    };

    explicit HudRenderer(const Bindings& b)
        : menuSystem(b.menuSystem), packLibrary(b.packLibrary), uiFrameData_(b.uiFrameData_), gameSession(b.gameSession),
          clientMirror(b.clientMirror),
          textFont(b.textFont), fontMetrics(b.fontMetrics), language(b.language),
          lightWorld(b.lightWorld), window(b.window), options(b.options),
          camera(b.camera), swapchainExtent(b.swapchainExtent), hudPipeline(b.hudPipeline),
          hudBlockIconPipeline(b.hudBlockIconPipeline),
          hudPipelineLayout(b.hudPipelineLayout), vignettePipeline(b.vignettePipeline),
          crosshairPipeline(b.crosshairPipeline), panoramaPipeline(b.panoramaPipeline),
          panoramaPipelineLayout(b.panoramaPipelineLayout),
          gradientPipeline(b.gradientPipeline),
          gradientPipelineLayout(b.gradientPipelineLayout), heldItemPipeline(b.heldItemPipeline),
          itemPipelineLayout(b.itemPipelineLayout), inventoryOpen(b.inventoryOpen),
          containerScreen(b.containerScreen), activeChest(b.activeChest),
          debugOverlayOpen(b.debugOverlayOpen),
          inventoryDragActive(b.inventoryDragActive), inventoryDragSlots(b.inventoryDragSlots),
          chatOpen(b.chatOpen), chatHistory(b.chatHistory), chatInput(b.chatInput),
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
          guiWidgetSprites(b.guiWidgetSprites), titleArtUv(b.titleArtUv),
          pinnedCursor(b.pinnedCursor), uiCaptureActive(b.uiCaptureActive),
          worldIconCapturePending(b.worldIconCapturePending), paused(b.paused),
          uiTimeSeconds(b.uiTimeSeconds), cameraSubmergedInWater(b.cameraSubmergedInWater),
          keyBindLabels(b.keyBindLabels),
          drawHeldItem(b.drawHeldItem), currentFrameDescriptorSet(b.currentFrameDescriptorSet),
          screenContext(b.screenContext),
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
        drawContext_.keyBindLabelsFor = [this](input::InputAction action) {
            return keyBindLabels ? keyBindLabels(action)
                                 : ui::MenuBuildContext::KeyBindRowLabels{
                                       std::string{input::actionDisplayName(action)}, {}};
        };
        drawCallbacks_.viewDistance.value = [this] {
            return static_cast<float>(viewDistanceChunks - 2) / 34.0F;
        };
        drawCallbacks_.simulationDistance.value = [this] {
            return static_cast<float>(simulationDistanceChunks - 2) / 10.0F;
        };
        drawCallbacks_.masterVolume.value = [this] { return options.masterVolume; };
        // UI-6d：整数滑块由表驱动——一个回调服务 ui/OptionSlider.hpp 里所有的滑块。
        // 绘制侧只需要 value（拖拽在输入侧），所以这里只填它。
        // UI-6e：float 滑块的取值。**必须与输入侧一起填**——只填输入侧，滑块拖得动、
        // 标签也对，但把手永远画在最左端（`w.slider.value` 是空的，画 0）。
        // 实测就是这么错的一次：十个音量的百分比文字全对，把手全在 0。
        // 这与 UI-6c 那次"MenuBuildContext 两处填充只填了一处"是同一族。
        drawCallbacks_.floatSliderFor = [this](ui::WidgetId id) {
            ui::SliderBind bind;
            if (const auto* desc = ui::findFloatSlider(id)) {
                bind.value = [this, desc] { return ui::floatSliderValue(*desc, options); };
            }
            return bind;
        };
        drawCallbacks_.intSliderFor = [this](ui::WidgetId id) {
            ui::SliderBind bind;
            if (const auto* desc = ui::findIntSlider(id)) {
                bind.value = [this, desc] {
                    return ui::intSliderFraction(*desc, options.*(desc->field));
                };
            }
            return bind;
        };
    }

    HudRenderer(const HudRenderer&) = delete;
    HudRenderer& operator=(const HudRenderer&) = delete;

    // ---- 与渲染器内核重复的一组助手，都是对已绑定引用的纯读取 ----
    // 有了它们，搬过来的绘制代码不必反向调用 Impl，而 Impl 自己保留一份供输入路径使用
    [[nodiscard]] std::string_view translate(std::string_view key,
                                             std::string_view fallback) const {
        return language.translate(key, fallback);
    }
    // D12：译文要过一遍格式化器，哪怕没有参数。
    //
    // Java 的语言 JSON 用 `%` 作占位符前缀，字面百分号因此写成 `%%`——
    // `options.languageWarning` 就是 "…may not be 100%% accurate"。vanilla 的
    // `Component.translatable(key)` 即使不带参数也会走 String.format，所以屏幕上是一个 `%`；
    // 本作从前直接把原串画出去，于是显示成 `100%%`。
    //
    // 只折 `%%`，**不碰** `%s`：本作有一类串是"先取译文、之后再带参数格式化"的
    // （`options.generic_value` 就是 `"%s: %s"`），过一遍无参数的格式化器会把它吃空。
    [[nodiscard]] std::string translated(std::string_view key, std::string_view fallback) const {
        return ui::unescapeTranslationPercents(translate(key, fallback));
    }
    [[nodiscard]] ui::UiPoint currentFramebufferCursor() const {
        // UI-2：截图通道钉死的光标（见 VulkanRenderer::Impl 上那条注释）。
        // 绘制侧和输入侧读的是同一个钉子，两边因此不会一个亮一个不亮。
        if (pinnedCursor.has_value()) {
            return *pinnedCursor;
        }
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
    // 一页有几个按钮：从**已装配的**页面数出来，不是另一张表说的。
    [[nodiscard]] std::size_t menuButtonCount() const {
        return ui::countPageButtons(buildDrawPage());
    }
    [[nodiscard]] ui::UiRect worldListRow(std::size_t index, const ui::HudLayout& layout) const {
        return ui::worldListRow(index, layout);
    }
    [[nodiscard]] std::size_t saveListVisibleRowCount() const {
        return ui::saveListVisibleRowCount(static_cast<float>(swapchainExtent.width),
                                           static_cast<float>(swapchainExtent.height),
                                           menuSystem.guiScaleSetting, menuSystem.forceUnicodeFont);
    }
    [[nodiscard]] ui::UiRect languageListBox(const ui::HudLayout& layout) const {
        return ui::languageListBox(layout);
    }
    [[nodiscard]] float languageWarningY(const ui::HudLayout& layout) const {
        return ui::languageWarningY(layout);
    }
    [[nodiscard]] ui::UiRect languageRow(std::size_t index, const ui::HudLayout& layout) const {
        return ui::languageRow(index, layout);
    }
    [[nodiscard]] std::size_t languageVisibleRowCount() const {
        return ui::languageVisibleRowCount(static_cast<float>(swapchainExtent.width),
                                           static_cast<float>(swapchainExtent.height),
                                           menuSystem.guiScaleSetting, menuSystem.forceUnicodeFont);
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
    // UI-6e ③：资源包两栏的行数与选中行。**与输入侧那份必须一致**——
    // 两处各算一遍是 UI-6c/6d 已经栽过两次的形状，所以两边算的都是同一件事：
    // 左栏 = 已注册但不在草稿里的，右栏 = 草稿本身，各自被视口容量夹住。
    // UI-11 / A5：提示屏那三样要量字体的东西——正文的换行、标题宽、复选框文字宽。
    //
    // ★ 与 fillPackContext 同理，它是**一处**代码给两条路径（绘制的 drawContext_ 与
    //   输入的 ctx）填同一份值。这两处各填一遍的后果这条线上已经吃过：UI-6c 的
    //   keyBindLabelsFor 只填了一处，界面切中文后按键设置整屏还是英文。
    void fillNoticeContext(ui::MenuBuildContext& ctx, const ui::HudLayout& layout) const {
        if (menuSystem.pageStack.current() != ui::PageId::AdvancedGraphicsNotice) {
            return;
        }
        const auto measure = [this](std::string_view text) { return hudTextWidth(text, 1.0F); };
        ctx.noticeMessageLines = ui::wrapText(
            widgetLabel(ui::WidgetId::NoticeMessage),
            static_cast<float>(ui::noticeMessageWrapWidth(layout.logicalWidth())), measure);
        ctx.noticeMetrics = {
            static_cast<int>(measure(widgetLabel(ui::WidgetId::NoticeTitle))),
            static_cast<int>(measure(widgetLabel(ui::WidgetId::NoticeStopShowing))),
        };
        ctx.noticeStopShowing = menuSystem.noticeStopShowing;
    }

    void fillPackContext(ui::MenuBuildContext& ctx, const ui::HudLayout& layout) const {
        if (menuSystem.pageStack.current() != ui::PageId::ResourcePacks) {
            return;
        }
        const auto lists = ui::dualColumnLists(
            ui::headerAndFooterLayout(layout.logicalWidth(), layout.logicalHeight()).contentBox(),
            layout.logicalWidth());
        const std::size_t capacity = lists.available.visibleRows();
        std::size_t available = 0;
        for (const auto& pack : packLibrary.packs()) {
            if (!packLibrary.isEnabled(pack.id)) {
                ++available;
            }
        }
        const std::size_t selectedTotal = packLibrary.draftOrder().size();
        // ★ 钳制与窗口大小都归 `ui::packColumnWindow`（纯函数、有断言）。两栏各调
        //   一次，**各传各的 firstRow**——共用一个的后果是滚左边右边跟着动。
        const auto availableWindow =
            ui::packColumnWindow(available, capacity, menuSystem.packAvailableFirstRow);
        const auto selectedWindow =
            ui::packColumnWindow(selectedTotal, capacity, menuSystem.packSelectedFirstRow);
        // UI-10 / D24：**两栏各自滚动**（偏差的第二半）。从前这里直接把行数截断到
        // 一屏放得下的数量——包多过一屏时，下面那些**根本画不出来也点不到**。
        //
        // ★ 起点在这里钳一次，装配与布局都从 ctx 里取同一个值；钳制只发生在这一处
        //   （与设置列表的 `optionsWindowFor` 同一条规矩）。
        ctx.availablePackFirstRow = availableWindow.firstRow;
        ctx.selectedPackFirstRow = selectedWindow.firstRow;
        ctx.availablePackRowCount = availableWindow.rowCount;
        ctx.selectedPackRowCount = selectedWindow.rowCount;
        ctx.selectedPackTotalRows = selectedTotal;
        ctx.selectedPackRow = menuSystem.selectedPackRow;
    }

    [[nodiscard]] const ui::Page& buildDrawPage() const {
        const ui::PageId pageId = menuSystem.pageStack.current();
        const ui::HudLayout layout{static_cast<float>(swapchainExtent.width),
                                   static_cast<float>(swapchainExtent.height),
                                   menuSystem.guiScaleSetting, menuSystem.forceUnicodeFont};
        drawContext_.worldOpen = currentSave.has_value();
        drawContext_.worldSelectable = !menuSystem.saveSummaries.empty();
        // ★ UI-6c：窗口数的是**行**，不是动作——展开后的行表里夹着分类标题行。
        const float fbWidth = static_cast<float>(swapchainExtent.width);
        std::size_t keyFirst = 0U;
        drawContext_.keyBindFirstIndex = 0U;
        drawContext_.keyBindRowCount = 0U;
        if (pageId == ui::PageId::KeyBinds) {
            const std::size_t window = ui::keyBindsVisibleRowCount(
                fbWidth, static_cast<float>(swapchainExtent.height), menuSystem.guiScaleSetting, menuSystem.forceUnicodeFont);
            keyFirst = std::min(menuSystem.controlsListFirstIndex, ui::kKeyBindListRowCount);
            drawContext_.keyBindFirstIndex = keyFirst;
            drawContext_.keyBindRowCount =
                std::min(window, ui::kKeyBindListRowCount - keyFirst);
        }
        // 两趟：先装配（这一页有哪些控件），再布局（它们在哪）。
        // 按钮数由布局那一趟从装配结果**数出来**——从前它是另一张表说的，
        // 而那张表与装配器是同一个事实的两份表述（见 ui::layoutPageInto 的注释）。
        // UI-6d：设置列表的滚动窗口。装配用它跳过滚出去的项，布局用同一个 firstRow
        // 折算行号——两侧都从 ui::optionsWindowFor 取，钳制也只发生在那一处。
        drawContext_.optionsWindow =
            ui::optionsWindowFor(layout, pageId, menuSystem.optionsListFirstIndex);
        fillPackContext(drawContext_, layout);
        fillNoticeContext(drawContext_, layout);
        // UI-9：创建世界开在哪一页，以及三个页签上的字。
        // ★ **装配与布局必须读同一个值**——装配按当前页造控件、布局按同一页算矩形，
        //   两边不同步就是"点 A 触发 B"（护栏 21）。所以它从这一处喂给两遍。
        drawContext_.createWorldTab = menuSystem.createWorldTab;
        // UI-10 / D20：世界名框那句提示框文案。与 SaveRepository::create 用的是**同一个**
        // slug 函数，"预览说的"与"真正建出来的"因此不可能各自演化。重名时 create()
        // 还会加 `-2` 这类后缀，那要摸磁盘，预览不做——26.1 那行提示同样只给基名。
        drawContext_.createWorldFolderHint = folderHintForCreateWorld();
        drawContext_.createWorldTabLabels = {
            translated("createWorld.tab.game.title", "Game"),
            translated("createWorld.tab.world.title", "World"),
            translated("createWorld.tab.more.title", "More"),
        };
        ui::buildPageInto(drawPage_, pageId, drawContext_, drawCallbacks_);
        ui::layoutPageInto(drawPage_, pageId, layout, keyFirst,
                           drawContext_.optionsWindow.firstRow, menuSystem.createWorldTab,
                           drawContext_.noticeMetrics);
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
        // Named fields, not positions: the icon-only members below them are left
        // at zero deliberately, and saying so is what keeps "unused here" from
        // drifting back into "means something else here".
        const HudPush push{
            .rect =
                {
                    clipRectangle.x,
                    clipRectangle.y,
                    clipRectangle.width,
                    clipRectangle.height,
                },
            .color = color,
            .uvRect = uvRectangle,
            .data = {guiSprite ? kHudModeGuiSprite
                               : (fontGlyph ? kHudModeFontGlyph
                                            : (textured ? kHudModeBlockTexture : kHudModeFlat)),
                     textureLayer, 0.0F, 0.0F},
            .iconBoxMin = {},
            .iconBoxMax = {},
            .iconUv01 = {},
            .iconUv23 = {},
        };
        vkCmdPushConstants(commandBuffer, hudPipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);
        vkCmdDraw(commandBuffer, 6, 1, 0, 0);
    }

    // RN-14: draw a block's ITEM MODEL as the inventory icon — one draw per
    // visible face of each of the model's boxes.
    //
    // It used to be one draw of a fixed 18-vertex cube with a `portion` float
    // that squashed it to half height for a slab. That is a cube and a slab and
    // nothing else, which is the mechanical reason a stair, a wall, a fence gate,
    // a pressure plate and a button all showed as full cubes wearing their parent
    // block's sprite — the "a stair looks exactly like a plank" report. The boxes,
    // their uv rects and the inventory turn all come from world::ItemModel, the
    // same source the dropped and held item read, so the three surfaces cannot
    // drift apart again (RN-10f's rule).
    //
    // Depth-tested, unlike every other HUD draw: a wall's centre post and its arm
    // interpenetrate, so no back-to-front ordering of whole boxes composites them
    // correctly. The GUI pass clears depth every frame and nothing else in it
    // depth-tests against these pixels.
    void drawHudBlockIcon(VkCommandBuffer commandBuffer, const ui::UiRect& rectangle,
                          world::Block block) const {
        const float width = static_cast<float>(swapchainExtent.width);
        const float height = static_cast<float>(swapchainExtent.height);
        const auto clipRectangle = ui::framebufferToClip(rectangle, width, height);
        // RN-8c-D: the icon's faces come from world::cubeItemLayers, the same
        // source the dropped and held cube read. That is what makes a piston icon
        // show its platform on top (its item model is a plain cube_bottom_top)
        // while a furnace icon shows its front on the left (its item model is the
        // block's own). The chest is not drawn from the block's texture triple at
        // all — its item has three dedicated atlas layers.
        const bool chest = block == world::Block::Chest;
        const world::CubeItemLayers layers =
            chest ? world::CubeItemLayers{kChestItemTopLayer, kChestItemTopLayer,
                                          kChestItemSideLayer, kChestItemFrontLayer,
                                          kChestItemSideLayer}
                  : world::cubeItemLayers(block);
        const auto range = world::itemModelRange(block);
        if (range.count == 0U) {
            return;
        }
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, hudBlockIconPipeline);
        for (std::size_t b = 0; b < range.count; ++b) {
            const world::IconBox& icon =
                world::kItemIconBoxes[static_cast<std::size_t>(range.first) + b];
            for (std::uint32_t f = 0; f < world::kIconFaces.size(); ++f) {
                if (!icon.present[f]) {
                    continue;
                }
                const float layer = world::itemFaceLayer(layers, icon.slot[f]);
                const HudPush push = makeBlockIconPush(clipRectangle, icon, f, layer);
                vkCmdPushConstants(commandBuffer, hudPipelineLayout,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof(push), &push);
                vkCmdDraw(commandBuffer, 6, 1, f * 6U, 0);
            }
        }
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipeline);
    }

    void drawHudItemIcon(VkCommandBuffer commandBuffer, const ui::UiRect& rectangle,
                         const gameplay::ItemStack& stack) const {
        if (gameplay::isBlockStack(stack)) {
            // 画不画方块模型由 world::rendersAsModelItem 单点回答
            // 掉落物、手持物、背包图标三条物品渲染面共用它，不再各自列举 BlockModel
            //
            // RN-10f：这里曾经在单点之外**再写一条**分支，把楼梯/墙/栅栏门/按钮/压力板
            // 也画成 3D 图标。那条分支只存在于图标这一面，于是掉在地上和拿在手里的
            // 栅栏门仍是扁平贴图——同一件物品三处三个样。规则已并回单点，
            // 三条渲染面因此自动一致，这一层不再有自己的例外。
            //
            // RN-14：那个单点此前只有「立方体 / 扁平贴图」两个答案，于是每个异形方块
            // 都落进「立方体」，图标是父方块贴图的整立方体——楼梯与木板长得一模一样。
            // 现在它回答的是「哪个模型」，台阶的半砖也只是那个模型的一个盒子，
            // 不再是着色器里一个把立方体压扁的 portion 特例。
            if (world::rendersAsModelItem(stack.block)) {
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
        drawHudQuad(commandBuffer, destination, tint, layer, false,
                    {sourcePixels.x / kGuiAtlasSize, sourcePixels.y / kGuiAtlasSize,
                     sourcePixels.width / kGuiAtlasSize, sourcePixels.height / kGuiAtlasSize},
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
            .rect = {clipRectangle.x, clipRectangle.y, clipRectangle.width, clipRectangle.height},
            .color = {1.0F, 1.0F, 1.0F, 1.0F},
            .uvRect = {0.0F, 0.0F, 15.0F / atlasSize, 15.0F / atlasSize},
            .data = {kHudModeCrosshair, 1.0F, 0.0F, 0.0F},
            .iconBoxMin = {},
            .iconBoxMax = {},
            .iconUv01 = {},
            .iconUv23 = {},
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

    // UI-5：一条竖直渐变矩形，26.1 的 `GuiGraphicsExtractor.fillGradient(x0,y0,x1,y1,上,下)`。
    //
    // 它跑在自己的管线上（GradientPush 两个颜色，HudPush 塞不下第二个）。画完立刻把
    // HUD 管线绑回去——后面每一次 drawHudQuad / drawGuiSprite 都假定它已经绑好，
    // 忘了绑会让紧随其后的那一批精灵按渐变着色器画出来。
    //
    // 这一层是**一次**半透明合成，不是两次。从前死亡屏那块平色其实是同一个错误的
    // 另一半：把 0x60500000→0xA0803030 的渐变近似成一块 rgba(0.25,0,0,0.58) 的平色，
    // 于是屏幕上半部比 vanilla 暗、下半部比 vanilla 亮。
    void drawVerticalGradient(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet,
                              const ui::UiRect& rectangle,
                              const ui::GradientStops& stops) const {
        const auto clip = ui::framebufferToClip(rectangle, static_cast<float>(swapchainExtent.width),
                                                static_cast<float>(swapchainExtent.height));
        const auto push = makeGradientPush(clip, stops);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, gradientPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                gradientPipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
        vkCmdPushConstants(commandBuffer, gradientPipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);
        vkCmdDraw(commandBuffer, 6, 1, 0, 0);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipelineLayout,
                                0, 1, &descriptorSet, 0, nullptr);
    }

    [[nodiscard]] ui::UiRect fullScreenRect() const {
        return {0.0F, 0.0F, static_cast<float>(swapchainExtent.width),
                static_cast<float>(swapchainExtent.height)};
    }

    // `Screen.extractTransparentBackground`：容器/背包那一档的灰渐变，
    // 顶 0xC0101010 → 底 0xD0101010（Screen.java:467）。
    //
    // ★ 它**只**属于 26.1 意义上的 in-game UI（AbstractContainerScreen 与命令方块编辑屏）。
    // 暂停菜单不走这条——那一档是「整帧模糊 + inworld_menu_background」。
    // 从前本作让暂停菜单也铺这层灰，于是暂停时世界永远是清晰的、只是被压暗。
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
        // UI-4（UI-3 留的账）：标签整数居中。26.1 是
        // `x + (width - font.width(text)) / 2`，逻辑像素上的**整数除法**，
        // 而 font.width 本身是 Mth.ceil 的整数。此前这里是浮点的 *0.5F，
        // 于是奇数差时文字落在半个像素上。
        drawScrollingLabel(commandBuffer, snapped, label, textY, scale, textColor,
                           centredLabelX(snapped, label, scale));
    }

    // UI-6f（D17）：控件标签放不下时**剪裁 + 来回滚**，而不是画出控件外。
    //
    // ★ 26.1 `ActiveTextCollector.defaultScrollingHelper`：超宽时左对齐 + scissor +
    //   正弦缓动来回滚；装得下时居中但把中心夹在两端之内。本作从前一律照居中画，
    //   于是 "Rain Mode: Asynchronous Particle Rain" 这类标签一路画出按钮外。
    //
    // ★ **scissor 是动态状态**：设了必须恢复，否则后面所有绘制都被裁在这个控件里。
    void drawScrollingLabel(VkCommandBuffer commandBuffer, const ui::UiRect& box,
                            std::string_view label, float textY, float scale,
                            const glm::vec4& color, float centredX) const {
        const float margin = 2.0F * scale;
        const float room = box.width - margin * 2.0F;
        const auto scroll =
            ui::scrollingTextAt(hudTextWidth(label, scale), room, scrollingTextSeconds());
        if (!scroll.scrolls) {
            drawHudText(commandBuffer, label, centredX, textY, scale, color);
            return;
        }
        const VkRect2D clip{
            {static_cast<std::int32_t>(std::max(box.x + margin, 0.0F)),
             static_cast<std::int32_t>(std::max(box.y, 0.0F))},
            {static_cast<std::uint32_t>(std::max(room, 0.0F)),
             static_cast<std::uint32_t>(std::max(box.height, 0.0F))},
        };
        vkCmdSetScissor(commandBuffer, 0, 1, &clip);
        drawHudText(commandBuffer, label, box.x + margin - scroll.offset, textY, scale, color);
        // ★ 恢复成整屏，否则后面每一次绘制都还被裁在这个控件里。
        const VkRect2D full{{0, 0}, swapchainExtent};
        vkCmdSetScissor(commandBuffer, 0, 1, &full);
    }

    // 滚动用的时间。★ 出图时**钉住**（kPinnedTime）：这是时间驱动的动画，
    // 不钉住截图通道就不再"同一条命令行跑两遍逐字节相同"。
    [[nodiscard]] double scrollingTextSeconds() const {
        if (uiCaptureActive) {
            return ui::kPinnedTime;
        }
        return std::chrono::duration<double>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
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
        // ★ 把手位置与输入侧的光标换算必须同源：`ui::sliderHandleX` 与
        //   `ui::sliderFractionFromCursor` 互为逆。从前这里自己算一遍
        //   `x + value * (width - 8*scale)`，输入侧另算一遍——两处对"行程"的定义
        //   一旦分家，症状是"把手停的位置和你松手的位置差半格"，而两边各自都自洽。
        const float knobX = ui::sliderHandleX(snapped, value, scale);
        const auto& knob = guiWidgetSprite(guiWidgetSprites,
                                           state == ui::ButtonVisualState::Normal
                                               ? GuiWidgetSprite::SliderHandle
                                               : GuiWidgetSprite::SliderHandleHighlighted);
        drawScaledGuiSprite(commandBuffer,
                            {knobX, snapped.y,
                             static_cast<float>(ui::kSliderHandleWidth) * scale, snapped.height},
                            0.0F,
                            knob, scale);
        const glm::vec4 textColor = state == ui::ButtonVisualState::Disabled
                                        ? glm::vec4{0.63F, 0.63F, 0.63F, 1.0F}
                                        : (state == ui::ButtonVisualState::Hovered ||
                                                   state == ui::ButtonVisualState::Pressed
                                               ? glm::vec4{1.0F, 1.0F, 0.63F, 1.0F}
                                               : glm::vec4{1.0F});
        const float textY =
            snapped.y + (6.0F + (state == ui::ButtonVisualState::Pressed ? 1.0F : 0.0F)) * scale;
        // UI-4（UI-3 留的账）：标签整数居中。26.1 是
        // `x + (width - font.width(text)) / 2`，逻辑像素上的**整数除法**，
        // 而 font.width 本身是 Mth.ceil 的整数。此前这里是浮点的 *0.5F，
        // 于是奇数差时文字落在半个像素上。
        // UI-6f（D17）：滑块标签同样会超宽，与按钮走同一条剪裁+滚动的路。
        drawScrollingLabel(commandBuffer, snapped, label, textY, scale, textColor,
                           centredLabelX(snapped, label, scale));
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
                // UI-3：阴影色 = 主色 × 0.25，在 sRGB 编码字节上乘再截断
                // （26.1 `Font.java:428` 的 `ARGB.scaleRGB(textColor, 0.25F)`）。
                // 此前这里是手调的 0.18，比原版亮不止一档。
                const glm::vec4 shadowColor{
                    ui::textShadowChannel(color.r),
                    ui::textShadowChannel(color.g),
                    ui::textShadowChannel(color.b),
                    color.a,
                };
                // 偏移是**每个字形自己的**：ASCII 1 逻辑像素，半尺寸的 unicode 字形 0.5。
                const float offset = metrics.shadowOffset * scale;
                drawHudQuad(commandBuffer,
                            {glyph.x + offset, glyph.y + offset, glyph.width, glyph.height},
                            shadowColor, metrics.layer, false, uv, true);
            }
            if (metrics.visible) {
                drawHudQuad(commandBuffer, glyph, color, metrics.layer, false, uv, true);
            }
            cursorX += metrics.advance * scale;
        }
    }

    // UI-4：绕一个锚点旋转 + 缩放地画一行字（26.1 的 splash 就是这么画的）。
    //
    // 每个字形先在**未旋转**的局部空间里排好（锚点为原点），再把它的原点绕锚点转过去，
    // 最后让顶点着色器把这个字形自己的四边形绕**它自己的原点**转同样的角度。
    // 两步合起来正好是"整行绕锚点旋转"，而 push 常量里只需要多带一个角度。
    void drawHudTextRotated(VkCommandBuffer commandBuffer, std::string_view text, float anchorX,
                            float anchorY, float scale, float rotation,
                            const glm::vec4& color) const {
        const float width = static_cast<float>(swapchainExtent.width);
        const float height = static_cast<float>(swapchainExtent.height);
        const float aspect = height <= 0.0F ? 1.0F : width / height;
        const float cosine = std::cos(rotation);
        const float sine = std::sin(rotation);
        // 局部坐标（像素）绕原点转，x 与 y 同尺度，因此这里是普通的二维旋转
        const auto place = [&](float localX, float localY) {
            return ui::UiPoint{anchorX + localX * cosine - localY * sine,
                               anchorY + localX * sine + localY * cosine};
        };
        float cursorX = -hudTextWidth(text, scale) * 0.5F;
        for (const char32_t codepoint : ui::decodeUtf8(text)) {
            const auto metrics = textFont.glyph(codepoint);
            if (metrics.visible) {
                const float localX = cursorX + metrics.offsetX * scale;
                const float localY = ui::kSplashTextOffsetY * scale + metrics.offsetY * scale;
                const auto origin = place(localX, localY);
                const ui::UiRect glyph{origin.x, origin.y, metrics.pixelWidth * scale,
                                       metrics.pixelHeight * scale};
                const glm::vec4 uv{metrics.u, metrics.v, metrics.uvWidth, metrics.uvHeight};
                // 阴影同样是旋转的：它在字形的局部空间里偏 +1/+1，转过去仍贴着字
                const auto shadowOrigin =
                    place(localX + metrics.shadowOffset * scale,
                          localY + metrics.shadowOffset * scale);
                drawRotatedGlyph(commandBuffer,
                                 {shadowOrigin.x, shadowOrigin.y, glyph.width, glyph.height}, uv,
                                 {ui::textShadowChannel(color.r), ui::textShadowChannel(color.g),
                                  ui::textShadowChannel(color.b), color.a},
                                 metrics.layer, rotation, aspect);
                drawRotatedGlyph(commandBuffer, glyph, uv, color, metrics.layer, rotation,
                                 aspect);
            }
            cursorX += metrics.advance * scale;
        }
    }

    void drawRotatedGlyph(VkCommandBuffer commandBuffer, const ui::UiRect& rectangle,
                          const glm::vec4& uv, const glm::vec4& color, float layer,
                          float rotation, float aspect) const {
        const auto clip = ui::framebufferToClip(rectangle,
                                                static_cast<float>(swapchainExtent.width),
                                                static_cast<float>(swapchainExtent.height));
        const HudPush push{
            .rect = {clip.x, clip.y, clip.width, clip.height},
            .color = color,
            .uvRect = uv,
            .data = {kHudModeFontGlyph, layer, rotation, aspect},
            .iconBoxMin = {},
            .iconBoxMax = {},
            .iconUv01 = {},
            .iconUv23 = {},
        };
        vkCmdPushConstants(commandBuffer, hudPipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);
        vkCmdDraw(commandBuffer, 6, 1, 0, 0);
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

    // 一格槽位：物品图标 + 耐久条 + 数量。
    //
    // ★ A1 删掉了两个死参数。`selected`（画一圈黄框、加深底色）与 `minecraftStyle`
    //   （自造的深色格子底）：三个调用点**全部**传 `minecraftStyle = true`，于是那条
    //   分支连同 `selected` 一起从来没有被执行过——快捷栏那个选中框是 HUD 层另画的
    //   一张 24x24 精灵（`layout.hotbarSelection`），与这里无关。
    //   ★ 它还骗过了一次 sabotage：把"选中框只属于玩家自己的槽"这条判断放宽，
    //     箱子屏的图**一个像素都没变**——因为那个参数根本没人读。留着一个不被读的
    //     参数，等于给未来的每一次 sabotage 发一张免检票。
    // ★ UI-8 / D30：悬停高亮**不在这里**了。26.1 整屏只有一个 `hoveredSlot`，它的
    //   高亮是两张 24x24 九宫格精灵、画在槽位的 (x-4,y-4)，一张在所有槽位内容之前、
    //   一张在之后（`AbstractContainerScreen:183-190`）。逐格画一层白方块画不出
    //   "在物品之上的那一张"，也画不出比格子大一圈的柔边。
    void drawHudSlot(VkCommandBuffer commandBuffer, const ui::UiRect& rectangle,
                     const gameplay::ItemStack& stack) const {
        if (stack.empty()) {
            return;
        }
        drawHudItemIcon(commandBuffer, rectangle, stack);
        drawDurabilityBar(commandBuffer, rectangle, stack);
        if (stack.count > 1U) {
            const std::string count = std::to_string(stack.count);
            const float textScale = rectangle.width / 16.0F;
            drawHudText(commandBuffer, count,
                        rectangle.x + 17.0F * textScale - hudTextWidth(count, textScale),
                        rectangle.y + 9.0F * textScale, textScale, {1.0F, 1.0F, 1.0F, 1.0F});
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
        // UI-11 / A5：提示屏的标题。它就是这一屏的标题，所以取自 ui::pageTitle
        // 那张表——在静态标签表里再抄一份就是同一个事实的两份表述。
        case ui::WidgetId::NoticeTitle: {
            const auto entry = ui::pageTitle(ui::PageId::AdvancedGraphicsNotice);
            return translated(entry.key, entry.fallback);
        }
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
        // UI-6d：整数滑块的标签走表。最低档显示 OFF（26.1 的 genericValueOrOffLabel）。
        case ui::WidgetId::MenuBackgroundBlurriness: {
            const auto* desc = ui::findIntSlider(ui::WidgetId::MenuBackgroundBlurriness);
            if (desc == nullptr) {
                return {};
            }
            const int value = options.*(desc->field);
            return optionValue(translated(desc->nameKey, desc->nameFallback),
                               desc->offAtMinimum && value <= desc->minimum
                                   ? translated("options.off", "OFF")
                                   : std::to_string(value));
        }
        case ui::WidgetId::SimulationDistance:
            return optionValue(
                translated("options.simulationDistance", "Simulation Distance"),
                formatTemplate(translated("options.chunks", "%s chunks"),
                               std::to_string(simulationDistanceChunks)));
        // UI-6e：十类音量共用**一条**分支——名字、取值位置、OFF 规则全在
        // ui/OptionSlider.hpp 那张表里。
        //
        // ★ 从前这里只有主音量一个 case，写着自己的 `lround(...*100)`。那有两处不对：
        //   一是与 vanilla 相反（26.1 `Options.percentValueLabel` 是**截断**
        //   `(int)(value*100.0)`，不是四舍五入）；二是加一类音量就要再抄一个 case，
        //   而"登记进 kRuntimeWidgetLabels"只保证它**有归属**，不保证这里真的算了它
        //   ——实测九个新滑块的标签一开始全是空白，版面对了字没了。
        // UI-6e ④：视场角。★ 26.1 的两个特例判据是**取值等于某个具体数**，
        //   不是"到头了"：70 → `options.fov.min`（Normal）、110 → `options.fov.max`
        //   （Quake Pro），其余显示数字。而滑块的最小值是 **30** 不是 70——
        //   照"最小档显示 min 文本"写会让 30 显示 Normal、70 显示 70，两个都错。
        case ui::WidgetId::FieldOfView: {
            const std::string name = translated("options.fov", "FOV");
            if (options.fieldOfView == 70) {
                return optionValue(name, translated("options.fov.min", "Normal"));
            }
            if (options.fieldOfView == 110) {
                return optionValue(name, translated("options.fov.max", "Quake Pro"));
            }
            return optionValue(name, std::to_string(options.fieldOfView));
        }
        case ui::WidgetId::MasterVolume:
        case ui::WidgetId::MusicVolume:
        case ui::WidgetId::RecordVolume:
        case ui::WidgetId::WeatherVolume:
        case ui::WidgetId::BlockVolume:
        case ui::WidgetId::HostileVolume:
        case ui::WidgetId::NeutralVolume:
        case ui::WidgetId::PlayerVolume:
        case ui::WidgetId::AmbientVolume:
        case ui::WidgetId::VoiceVolume: {
            const auto* desc = ui::findFloatSlider(button);
            if (desc == nullptr) {
                return {};
            }
            const std::string name = translated(desc->nameKey, desc->nameFallback);
            const float value = ui::floatSliderValue(*desc, options);
            if (ui::floatSliderShowsOff(*desc, value)) {
                return optionValue(name, translated("options.off", "OFF"));
            }
            return percentValue(name, ui::floatSliderPercent(value));
        }
        case ui::WidgetId::Difficulty: {
            // 同一个按钮出现在两处，取值的来源不同：世界内的选项页读**已打开的存档**，
            // 创建世界页读那张表单的暂存值（此时还没有任何存档）。
            // ★ 标签算法仍然只有这一份——创建页复用同一个 WidgetId 正是为了这个：
            //   另起一个 id 就得再抄一遍"难度: XXX"，两份迟早分岔
            const auto difficulty =
                menuSystem.pageStack.current() == ui::PageId::CreateWorld
                    ? menuSystem.createWorldDifficulty
                    : (currentSave.has_value() ? currentSave->difficulty
                                               : gameplay::Difficulty::Normal);
            return optionValue(translated("options.difficulty", "Difficulty"),
                               translated(gameplay::difficultyTranslationKey(difficulty),
                                          gameplay::difficultyName(difficulty)));
        }
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
        // UI-2：主菜单不画这行——它画的是 logo 贴图（drawTitleBranding）。
        // 这里保留一条分支只为不让 Title 掉进下面那串存档名判断；文案取 vanilla 的
        // 旁白标题键，而不是从前那个自造的产品名。
        if (page == ui::PageId::Title)
            return translated("narrator.screen.title", "Title Screen");
        if (page == ui::PageId::WorldList)
            return translated("menu.singleplayer", "Singleplayer");
        // ★ UI-9：创建世界那一屏不再画标题（标签栏取代了页眉），所以这里没有它的分支。
        //   页脚那个 "Create New World" 是**按钮**，走 WidgetLabels，不走这里。
        if (page == ui::PageId::ConfirmDelete)
            return translated("selectWorld.deleteQuestion", "Delete World?");
        if (menuSystem.selectedWorldIndex < menuSystem.saveSummaries.size())
            return menuSystem.saveSummaries[menuSystem.selectedWorldIndex].displayName;
        return translated("selectWorld.edit", "Edit World");
    }

    // 创建世界那张表单的版面：世界名框、它下面那行文件夹预览、种子框，以及两行标签。
    //
    // 为什么算在这里而不是 HudLayout 里：它锚在**这一页按钮块的上沿**上，
    // 而按钮块居中、高度随按钮数变化（加了难度按钮之后是五个）。表单必须跟着按钮块
    // 一起上移，否则输入框会直接压在第一个按钮上——那正是只加按钮不动版面的症状。
    // 全程整数逻辑像素，与 UI-3 的版面口径一致。
    struct CreateWorldForm final {
        ui::UiRect nameField;
        ui::UiRect seedField;
        float nameLabelY = 0.0F;
        float folderLineY = 0.0F;
        float seedLabelY = 0.0F;
    };

    // 表单矩形全部来自 ui::createWorldLayout —— **与按钮位置同一个来源**
    // （`frontendButtonRect` 的 HeaderFooterForm 分支读的是同一个函数）。
    //
    // ★ 从前这里自己算：`buttonTop = 逻辑高/2 - 按钮数*12`，表单从 `buttonTop - 80`
    //   往上堆。1280x720 @ scale 3 的逻辑画布高 240，于是表单落在 **y = -20**，
    //   世界名输入框被切出画布顶部、文件夹提示与标题糊在一起。
    //   往上堆的版面没有上界，而"顶出画布"不会让任何断言变红——只有截图看得见。
    [[nodiscard]] CreateWorldForm createWorldForm(const ui::HudLayout& layout) const {
        const auto form =
            ui::createWorldLayout(layout.logicalWidth(), layout.logicalHeight(),
                                  menuSystem.createWorldTab);
        const float scale = layout.scale();
        const auto toFb = [scale](const ui::UiRect& rect) {
            return ui::UiRect{rect.x * scale, rect.y * scale, rect.width * scale,
                              rect.height * scale};
        };
        CreateWorldForm out;
        out.nameLabelY = form.nameLabel.y * scale;
        out.nameField = toFb(form.nameField);
        out.folderLineY = form.folderHint.y * scale;
        out.seedLabelY = form.seedLabel.y * scale;
        out.seedField = toFb(form.seedField);
        return out;
    }

    void drawCreateWorldForm(VkCommandBuffer commandBuffer, const ui::HudLayout& layout) const {
        const float scale = layout.scale();
        const auto form = createWorldForm(layout);
        const glm::vec4 labelColour{0.85F, 0.85F, 0.85F, 1.0F};
        // vanilla 的 GRAY：预览与提示都是"这不是你输入的内容"，不该和正文一个亮度
        const glm::vec4 hintColour{0.66F, 0.66F, 0.66F, 1.0F};

        // ★ UI-10：**框本身已经是控件**（`drawPageTextField`），这里只剩它上面那行标签。
        //   ★ 框下面那行"Will be saved in: …"的灰字**没有了**——26.1 是
        //     `nameEdit.setTooltip(Tooltip.create(selectWorld.targetFolder))`，也就是
        //     输入框的**悬停提示框**（偏差 D20）。文案在装配时喂进控件的 tooltip。
        //
        // ★ 按当前标签页跳过不属于它的那一组。判据是矩形本身为空
        //   （`createWorldLayout` 对不属于本页的字段返回空矩形），而不是在这里再判
        //   一次"现在是哪一页"——那会是同一事实的第二份表述，而症状是种子标签
        //   画在 y=0（画布顶）上，压着标签栏。
        static_cast<void>(hintColour);
        if (form.nameField.width > 0.0F) {
            drawHudText(commandBuffer, translated("selectWorld.enterName", "World Name"),
                        form.nameField.x, form.nameLabelY, scale, labelColour);
        }
        if (form.seedField.width > 0.0F) {
            drawHudText(commandBuffer, translated("selectWorld.enterSeed", "Seed"),
                        form.seedField.x, form.seedLabelY, scale, labelColour);
        }
    }

    void drawWorldNameField(VkCommandBuffer commandBuffer, const ui::HudLayout& layout,
                            const ui::TextFieldState& state) const {
        const float scale = layout.scale();
        const ui::UiRect field = layout.worldNameField();
        drawHudText(commandBuffer, translated("selectWorld.enterName", "World Name"), field.x,
                    field.y - 12.0F * scale, scale, {0.85F, 0.85F, 0.85F, 1.0F});
        drawTextField(commandBuffer, field, scale, state, ui::kWorldNameFieldRules,
                      TextFieldStyle{});
    }

    // UI-1: the one text field painter. Everything typeable in the game goes
    // through here, so the cursor, the selection and the horizontal scroll look
    // and behave the same in all three places rather than in none of them.
    //
    // Geometry is GUI spec §2.4: a 1px 0xFFA0A0A0 border over a 0xFF000000 fill,
    // text inset 4px and vertically centred on the box. The blink phase is
    // computed HERE, from the UI clock, and never enters the state — that is
    // what keeps the editing layer a pure function.
    void drawTextField(VkCommandBuffer commandBuffer, const ui::UiRect& field, float scale,
                       const ui::TextFieldState& state, const ui::TextFieldRules& rules,
                       const TextFieldStyle& style) const {
        if (style.bordered) {
            drawHudQuad(commandBuffer, field, {0.627F, 0.627F, 0.627F, 1.0F});
            drawHudQuad(commandBuffer,
                        {field.x + scale, field.y + scale, field.width - 2.0F * scale,
                         field.height - 2.0F * scale},
                        {0.0F, 0.0F, 0.0F, 1.0F});
        } else if (style.background.a > 0.0F) {
            drawHudQuad(commandBuffer, field, style.background);
        }

        // The same inner width the driver measured with: displayStart lives in
        // the state, so the two sides disagreeing would scroll the window to a
        // place the text is not.
        const float inset = ui::textFieldTextInset(scale, style.bordered);
        const ui::TextFieldMetrics metrics{
            [this, scale](std::string_view piece) { return hudTextWidth(piece, scale); },
            ui::textFieldInnerWidth(field.width, scale, style.bordered)};
        const auto view = ui::textFieldView(state, rules, metrics);
        const float textX = field.x + inset;
        // EditBox.java:487 — `textY = bordered ? getY() + (height - 8) / 2 : getY()`.
        // Only a BORDERED field centres its text in its own box; a bare one
        // draws at exactly its y, because the widget rect it was given is
        // already the text's line box (the anvil's is (62,24) 103x12, and
        // vanilla puts the glyphs at y=24, not y=26). Centring unconditionally
        // is what made the rename text sit low.
        const float textY =
            style.bordered ? field.y + (field.height - 8.0F * scale) * 0.5F : field.y;
        // vanilla's DEFAULT_TEXT_COLOR / textColorUneditable.
        const glm::vec4 colour =
            rules.editable ? style.textColor : glm::vec4{0.439F, 0.388F, 0.439F, 1.0F};

        const std::size_t selectionFirst =
            ui::textFieldByteOffset(view.visible, view.selectionStart);
        const std::size_t selectionLast = ui::textFieldByteOffset(view.visible, view.selectionEnd);
        const std::string before = view.visible.substr(0, selectionFirst);
        const std::string selected =
            view.visible.substr(selectionFirst, selectionLast - selectionFirst);
        const std::string after = view.visible.substr(selectionLast);
        const float selectionX = textX + hudTextWidth(before, scale);
        const float selectionEndX = selectionX + hudTextWidth(selected, scale);

        // The selection is a filled block with the text redrawn dark on top —
        // the readable stand-in for vanilla's inverting blend, which this HUD
        // pipeline has no equivalent of.
        if (!selected.empty()) {
            drawHudQuad(commandBuffer,
                        {selectionX, textY - scale, selectionEndX - selectionX, 10.0F * scale},
                        {0.85F, 0.85F, 0.95F, 1.0F});
        }
        if (!before.empty()) {
            drawHudText(commandBuffer, before, textX, textY, scale, colour, style.shadow);
        }
        if (!selected.empty()) {
            drawHudText(commandBuffer, selected, selectionX, textY, scale,
                        {0.05F, 0.05F, 0.10F, 1.0F}, false);
        }
        if (!after.empty()) {
            drawHudText(commandBuffer, after, selectionEndX, textY, scale, colour, style.shadow);
        }

        const float cursorX =
            view.cursorOnScreen
                ? textX + hudTextWidth(view.visible.substr(0, ui::textFieldByteOffset(
                                                                    view.visible,
                                                                    view.cursorOffset)),
                                       scale)
                : textX + field.width;
        // EditBox draws the suggestion only where the cursor is appending, so it
        // never collides with text the player is standing in the middle of.
        if (!style.suggestion.empty() && !view.insertCursor) {
            drawHudText(commandBuffer, style.suggestion, cursorX, textY, scale,
                        {0.5F, 0.5F, 0.5F, 1.0F}, style.shadow);
        }
        // TextCursorUtils.isCursorVisible: a 300ms blink interval.
        const bool blinkOn = static_cast<long long>(uiTimeSeconds * 1000.0) / 300LL % 2LL == 0LL;
        if (style.focused && rules.editable && blinkOn && view.cursorOnScreen) {
            if (view.insertCursor) {
                drawHudQuad(commandBuffer, {cursorX - scale, textY - scale, scale, 10.0F * scale},
                            colour);
            } else {
                drawHudText(commandBuffer, "_", cursorX, textY, scale, colour, style.shadow);
            }
        }
    }

    // UI-5：这一屏该用哪一档背景（ui::screenBackground 的三分支表 + 死亡屏那一档）。
    //
    // `worldOpen` 取 `worldReady` 而不是 `worldSessionActive`：26.1 判的是
    // `minecraft.level == null`，而地形还在生成时本作根本没有可供模糊的世界画面，
    // 那一段仍然该转全景。
    // 现在有没有界面盖在世界上。
    //
    // 26.1 里这就是 `minecraft.screen != null`：只有那时才调 Screen.extractBackground。
    // 本作的暂停界面不换页（页仍是 Game），所以 `paused` 与 `inventoryOpen` 也算。
    // 少了这个判断，游戏内 HUD 会被铺上一层 inworld_menu_background——
    // 档位表对 (Game, 有世界, 无容器) 给出的正是那一档，它只是没有资格被画出来。
    [[nodiscard]] bool screenOpen() const {
        return menuSystem.pageStack.current() != ui::PageId::Game || paused || inventoryOpen;
    }

    [[nodiscard]] ui::ScreenBackgroundKind currentBackgroundKind() const {
        return ui::screenBackground(menuSystem.pageStack.current(), worldReady, inventoryOpen);
    }

    // 26.1 的菜单背景是从全景立方体内部以 85 度透视看出去
    // 相机缓慢转动：偏航在 kCycleSeconds 内转满 360°，六个面各自都有较长时间正对视野
    // 俯仰做一次轻微扫掠，下探到 panorama_4、上仰到 panorama_5
    // 再叠一点 vanilla 式的正弦微晃，免得太机械
    //
    // ★ UI-5：全景**画在界面那趟之前**（graphMenuBackgroundStep），因为整帧模糊是
    // 一趟真正的后处理，跑在全景与界面之间——这正是 26.1 的
    // BEFORE_BLUR / AFTER_BLUR 两段（`GuiRenderer.java:182-184`）。
    // 从前它画在界面那趟里，而"模糊"是 panorama.frag 内部的一个 5x5 盒式近似：
    // 那种做法只能糊全景自己，糊不了世界，所以暂停菜单背后的世界一直是清晰的。
    void drawMenuPanorama(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet) const {
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
        const PanoramaPush push{{yaw, pitch, tanHalfFov, aspect}};
        vkCmdPushConstants(commandBuffer, panoramaPipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    }

    // UI-5：模糊之后铺的那一层（`Screen.extractMenuBackground`）。它跑在界面那趟里，
    // 也就是**模糊之后**——铺在模糊前会连同遮罩一起被糊掉。
    //
    // 配方由 titleBackgroundLayer 一处给出，三档各铺各的：主菜单是 panorama_overlay
    // （26.1 的原图是 1x1、alpha 恒 0，等于不画），无世界的二级界面是 menu_background，
    // 有世界的是 inworld_menu_background。三条的 tint 都是白色不透明——
    // 从前这里的未模糊分支画的是一块 30% 全屏黑，注释理由是"保证白色标题清晰"，
    // 那不是 vanilla：26.1 的 TitleScreen.extractBackground() 是空实现。
    void drawMenuBackgroundTile(VkCommandBuffer commandBuffer, ui::ScreenBackgroundKind kind,
                                float guiScale) const {
        const auto background = titleBackgroundLayer(kind);
        const auto fullScreen = fullScreenRect();
        // 非平铺就是整层拉满：源矩形取整个图集层，与 26.1 那次"整张纹理 blit 成全屏"一致
        const ui::UiRect source =
            background.tiled
                ? ui::tiledBackgroundSource(fullScreen.width, fullScreen.height, guiScale)
                : ui::UiRect{0.0F, 0.0F, kGuiAtlasSize, kGuiAtlasSize};
        drawGuiSprite(commandBuffer, fullScreen, background.guiLayer, source, background.tint);
    }

    // UI-5：滚动列表的底衬（`AbstractSelectionList.extractListBackground():226`）。
    //
    // 有世界时换成 inworld 那张，与外层菜单遮罩同一条规矩。tint 是**白色不透明**：
    // 变暗要来自纹理。从前这两处传的是 {32/255, 32/255, 32/255, 1}——用原版资源
    // 看不出区别（那张图的 RGB 本来就是 0，乘什么都是 0），但换个资源包就会被
    // 代码里的常数压掉八分之七，而那正是被删掉的那块 30% 黑遮罩的同一类错误。
    void drawListBackground(VkCommandBuffer commandBuffer, const ui::UiRect& box,
                            float guiScale) const {
        drawGuiSprite(commandBuffer, box, menuListBackgroundLayer(worldReady),
                      ui::tiledBackgroundSource(box.width, box.height, guiScale),
                      {1.0F, 1.0F, 1.0F, 1.0F});
    }

    // UI-5 / D9：列表上下那两道分隔线（`AbstractSelectionList.extractListSeparators():218-222`）。
    //
    // ★ 26.1 画的是两张 32x2 的分隔纹理，**不是** 4px 竖直渐隐带——渐隐是 1.20.2
    //   之前的做法。偏差表 D9 按旧 spec 记成"渐隐带缺绘制"，照它实现会画出一个
    //   26.1 根本没有的元素。几何在 ui::scrollListHeaderSeparator / FooterSeparator，
    //   两条都落在视口**之外**（上缘在 y-2，下缘在 bottom），所以不会盖住首末行。
    void drawListSeparators(VkCommandBuffer commandBuffer, const ui::UiRect& box,
                            float guiScale) const {
        const float thickness =
            static_cast<float>(ui::kScrollListSeparatorHeight) * std::max(guiScale, 1.0F);
        // 源矩形按 32 逻辑像素一个周期横向平铺：图集层里那 256 个纹素是同一张 32 宽的
        // 纹理连铺 8 次，所以取 box.width / guiScale 个纹素恰好是 width/(32*scale) 个周期。
        const float sourceWidth = box.width / std::max(guiScale, 1.0F);
        const auto strip = [&](float y, int spriteY) {
            drawGuiSprite(commandBuffer, {box.x, y, box.width, thickness}, kListSeparatorGuiLayer,
                          {0.0F, static_cast<float>(spriteY), sourceWidth,
                           static_cast<float>(kListSeparatorSpriteHeight)},
                          {1.0F, 1.0F, 1.0F, 1.0F});
        };
        strip(box.y - thickness, headerSeparatorSpriteY(worldReady));
        strip(box.y + box.height, footerSeparatorSpriteY(worldReady));
    }

    // UI-5：一屏的背景，按档位表一次画完（界面那趟这一侧）。
    // 全景不在这里——它在模糊之前，见 drawMenuPanorama。
    void drawScreenBackground(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet,
                              ui::ScreenBackgroundKind kind, float guiScale) const {
        if (ui::backgroundTilesMenuTexture(kind)) {
            drawMenuBackgroundTile(commandBuffer, kind, guiScale);
            return;
        }
        const auto stops = ui::backgroundGradient(kind);
        if (stops.top != 0U || stops.bottom != 0U) {
            drawVerticalGradient(commandBuffer, descriptorSet, fullScreenRect(), stops);
        }
    }

    // UI-2：从 binding 6 的标题美术里画一块。uv 是 TextureManager 记下的归一化子矩形，
    // 已经含了 26.1 那两处"只取纹理上半部分"的裁剪，所以这里不再有任何切片算术。
    void drawTitleTexture(VkCommandBuffer commandBuffer, const ui::UiRect& destination,
                          const glm::vec4& uv) const {
        const float width = static_cast<float>(swapchainExtent.width);
        const float height = static_cast<float>(swapchainExtent.height);
        const auto clip = ui::framebufferToClip(destination, width, height);
        const HudPush push{
            .rect = {clip.x, clip.y, clip.width, clip.height},
            .color = {1.0F, 1.0F, 1.0F, 1.0F},
            .uvRect = uv,
            .data = {kHudModeTitleTexture, 0.0F, 0.0F, 0.0F},
            .iconBoxMin = {},
            .iconBoxMax = {},
            .iconUv01 = {},
            .iconUv23 = {},
        };
        vkCmdPushConstants(commandBuffer, hudPipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);
        vkCmdDraw(commandBuffer, 6, 1, 0, 0);
    }

    // UI-2：主菜单的 logo、edition 副标题、左下版本行与右下版权行。
    //
    // 几何全部来自 ui::titleScreenLayout，也就是 26.1 的 TitleScreen.init 与
    // LogoRenderer 那套逻辑像素整数运算，绘制这里只负责乘 scale。
    //
    // 彩蛋 logo（minceraft，26.1 的概率是 1/10000）**本作恒关**：它是一张按运行时随机数
    // 二选一的图，而截图通道的全部价值在于两次运行逐字节相同。贴图已经烘进标题数组，
    // 将来要开只需把这个判断换成一次一次性的随机数。
    void drawTitleBranding(VkCommandBuffer commandBuffer, const ui::HudLayout& layout) const {
        const float scale = layout.scale();
        const std::string version = "Minecraft " + std::string{core::kVersion.name};
        // ★ 右下角那行**不**读 vanilla 的 `title.credits`。
        //
        // 26.1 那行是 "Copyright Mojang AB. Do not distribute!"，说的是**它自己那份代码与
        // 资源**的版权。本仓是一份独立实现，不在那份版权的管辖内，把它原样显示出来是一句
        // 事实错误的声明——而且它还会随玩家自备的资源包一起被翻译成各国语言。
        // 换成本项目自己的一行，键走 `lang/rebedrock/`（本仓唯一自有的翻译表命名空间），
        // 因此它不依赖任何资源包，也不会被资源包覆盖。
        const std::string copyright =
            translated("title.rebedrock.credits", "ReBedrock. Not affiliated with Mojang.");
        const auto title = ui::titleScreenLayout(
            layout.logicalWidth(), layout.logicalHeight(),
            static_cast<int>(hudTextWidth(version, 1.0F)),
            static_cast<int>(hudTextWidth(copyright, 1.0F)));
        const auto toFramebuffer = [scale](const ui::TitleRect& rect) {
            return ui::UiRect{
                static_cast<float>(rect.x) * scale,
                static_cast<float>(rect.y) * scale,
                static_cast<float>(rect.width) * scale,
                static_cast<float>(rect.height) * scale,
            };
        };
        drawTitleTexture(commandBuffer, toFramebuffer(title.logo), titleArtUv.logo);
        drawTitleTexture(commandBuffer, toFramebuffer(title.edition), titleArtUv.edition);
        drawTitleSplash(commandBuffer, layout);
        // 两行页脚都是普通的白色带阴影文本，26.1 用的是 font.drawInBatch 的默认样式
        drawHudText(commandBuffer, version, static_cast<float>(title.version.x) * scale,
                    static_cast<float>(title.version.y) * scale, scale,
                    {1.0F, 1.0F, 1.0F, 1.0F});
        drawHudText(commandBuffer, copyright, static_cast<float>(title.copyright.x) * scale,
                    static_cast<float>(title.copyright.y) * scale, scale,
                    {1.0F, 1.0F, 1.0F, 1.0F});
    }

    // UI-4：一行文字在一个矩形里水平居中，走 spec §1.2 的整数规矩。
    // 矩形是帧缓冲像素、文字宽是帧缓冲像素，两者都先除回逻辑像素做整数运算，再乘回去——
    // 中途乘 scale 再取整就不是整数版面了（见 ui/TextMetrics.hpp 的头注释）。
    [[nodiscard]] float centredLabelX(const ui::UiRect& rectangle, std::string_view label,
                                      float scale) const {
        const int width = ui::textWidthLogical(hudTextWidth(label, 1.0F));
        const int box = static_cast<int>(std::lround(rectangle.width / scale));
        return rectangle.x + static_cast<float>(ui::centredX(0, box, width)) * scale;
    }

    // UI-4：主菜单那行斜着的黄字（26.1 `SplashRenderer`）。
    //
    // 锚点 (W/2 + 123, 69)、旋转 -PI/9、缩放随时间脉动——三个数都取自源码，
    // 不是 GUI spec §6.3 的正文（那里的 `W/2 + 90` 与另一条缩放公式都是旧值，
    // 已作为更正 D 记在 UI-2 的落地记录里）。
    //
    // 无障碍设置 `hideSplashTexts` 打开时不画；资源包没有 texts/splashes.txt 时
    // splashLine 为空，同样不画——两条都与 vanilla 同形。
    void drawTitleSplash(VkCommandBuffer commandBuffer, const ui::HudLayout& layout) const {
        if (menuSystem.splashLine.empty()) {
            return;
        }
        const float scale = layout.scale();
        const float textWidth = hudTextWidth(menuSystem.splashLine, 1.0F);
        // 脉动的相位由 UI 时钟给出，因此截图通道钉住那个时钟就同时钉住了这里
        const auto milliseconds = static_cast<std::uint64_t>(uiTimeSeconds * 1000.0);
        const float pulse = ui::splashScale(textWidth, milliseconds);
        const float anchorX =
            (static_cast<float>(layout.logicalWidth()) / 2.0F + ui::kSplashAnchorX) * scale;
        const float anchorY = ui::kSplashAnchorY * scale;
        drawHudTextRotated(commandBuffer, menuSystem.splashLine, anchorX, anchorY,
                           scale * pulse, ui::kSplashRotation,
                           {1.0F, 1.0F, 0.0F, 1.0F});
    }

    // 通用的菜单绘制后端：按 widget 种类画出一页
    // 每个 widget 自带矩形、标签、启用状态和（滑块的）显示值
    // 按下高亮对应 id 等于 pressedMenuButton 的那个，删除确认按钮保留红色调
    // 列表行走专门的列表路径，不在这里画
    void drawMenuWidgets(VkCommandBuffer commandBuffer, const ui::Page& widgets,
                         float scale) const {
        const auto cursor = currentFramebufferCursor();
        // UI-10 / D20：光标下那个控件的提示框。**整页只有一个**，而且画在所有控件
        // 之后——它要盖在最上层（26.1 `Screen.render` 把 tooltip 留到最后）。
        // 26.1 的显示条件是"悬停，或键盘聚焦且上次输入来自键盘"，延迟默认为零。
        const std::string* hoveredTooltip = nullptr;
        std::size_t selectedPackRowsSeen = 0;
        // UI-4：键盘焦点与鼠标悬停共用 highlighted 那张精灵（26.1 `AbstractButton:46`）
        const std::size_t focused = menuSystem.focusFor(menuSystem.pageStack.current());
        for (std::size_t widgetIndex = 0; widgetIndex < widgets.size(); ++widgetIndex) {
            const ui::Widget& widget = widgets[widgetIndex];
            const bool widgetFocused = widgetIndex == focused;
            // 语言与世界列表的行：一块底衬加一行文本，样子对齐 vanilla 的列表项，
            // 而不是完整的按钮边框。
            // （UI-6b 之后按键绑定行不再走这里——它现在是 Label + Button 两个控件。）
            if (!widget.tooltip.empty() && widget.rect.contains(cursor.x, cursor.y)) {
                hoveredTooltip = &widget.tooltip;
            }
            // UI-10：输入框。它此前不是控件——绘制侧自己画、自己命中，于是
            // "控件不越界"那条通用护栏抓不住它（README 护栏 28）。
            if (widget.kind == ui::WidgetKind::TextField) {
                drawPageTextField(commandBuffer, widget, scale);
                continue;
            }
            // UI-10 / D24：纯命中区，自己什么都不画——箭头由**那一行**画（26.1 的
            // 三张精灵都落在同一个图标位上，热区只决定用哪张、要不要高亮）。
            if (widget.kind == ui::WidgetKind::IconZone) {
                continue;
            }
            if (widget.kind == ui::WidgetKind::ListRow) {
                // UI-10 / D24：右栏那一行能不能上/下移，取决于它是第几行——与布局
                // 数行号的方式**同一遍**（第几个同栏的行），不是另起一套。
                bool canMoveUp = false;
                bool canMoveDown = false;
                if (ui::isSelectedPackRow(widget)) {
                    canMoveUp = selectedPackRowsSeen > 0U;
                    canMoveDown = selectedPackRowsSeen + 1U < drawContext_.selectedPackRowCount;
                    ++selectedPackRowsSeen;
                }
                drawSelectionListRow(commandBuffer, widget, cursor.x, cursor.y, scale, canMoveUp,
                                     canMoveDown);
                continue;
            }
            // UI-6b：一行里的静态文本（按键绑定行的动作名）。左对齐于自己的矩形、
            // 竖直位置已由 keyBindNameCell 算好，所以这里只是把它画出来。
            if (widget.kind == ui::WidgetKind::Label) {
                drawHudText(commandBuffer, widget.label, widget.rect.x, widget.rect.y, scale,
                            {1.0F, 1.0F, 1.0F, 1.0F});
                continue;
            }
            // UI-11 / A5：复选框（26.1 `Checkbox`）。
            if (widget.kind == ui::WidgetKind::Checkbox) {
                drawCheckbox(commandBuffer, widget, scale, widgetFocused);
                continue;
            }
            // UI-9：标签页导航栏里的一个页签（26.1 `TabButton`）。
            if (widget.kind == ui::WidgetKind::Tab) {
                drawTabButton(commandBuffer, widget, cursor, scale, widgetFocused);
                continue;
            }
            // UI-4：图标钮 —— 同一张九宫格底，中间一张 15x15 的图标而不是一行标签
            if (widget.kind == ui::WidgetKind::IconButton) {
                drawIconButton(commandBuffer, widget, cursor.x, cursor.y, scale, widgetFocused);
                continue;
            }
            if (widget.kind != ui::WidgetKind::Button &&
                widget.kind != ui::WidgetKind::Slider) {
                continue;
            }
            const bool pressed =
                static_cast<ui::WidgetId>(widget.debugId) == pressedMenuButton;
            const auto state = ui::buttonVisualState(widget.rect, cursor.x, cursor.y,
                                                     widget.enabled, pressed, widgetFocused);
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
        // ★ 提示框最后画：它要盖在所有控件之上。
        if (hoveredTooltip != nullptr) {
            drawTooltipBox(commandBuffer, scale,
                           {{*hoveredTooltip, ui::TooltipStyle::NameCommon}});
        }
    }

    // UI-10 / D20：世界名框那句提示框文案。**一处来源**——绘制侧与输入侧都从这里取，
    // 各拼一遍就是同一事实的两份表述。
    [[nodiscard]] std::string folderHintForCreateWorld() const {
        return formatTemplate(
            translated("selectWorld.targetFolder", "Will be saved in: %s"),
            persistence::SaveRepository::slugForDisplayName(menuSystem.createWorldName.value));
    }

    // UI-10：页面里的一个输入框。文字与光标状态仍由 TextFieldState 管（编辑走输入侧
    // 那条既有路径），这里只按控件的矩形把它画出来。
    void drawPageTextField(VkCommandBuffer commandBuffer, const ui::Widget& widget,
                           float scale) const {
        const auto id = static_cast<ui::WidgetId>(widget.debugId);
        TextFieldStyle style;
        if (id == ui::WidgetId::CreateWorldNameField) {
            style.focused = !menuSystem.createWorldSeedFocused;
            drawTextField(commandBuffer, widget.rect, scale, menuSystem.createWorldName,
                          ui::kWorldNameFieldRules, style);
            return;
        }
        if (id == ui::WidgetId::CreateWorldSeedField) {
            style.focused = menuSystem.createWorldSeedFocused;
            // 空框里那行灰字就是 26.1 的 `seedEdit.setHint`。
            if (menuSystem.createWorldSeed.value.empty()) {
                style.suggestion =
                    translated("selectWorld.seedInfo", "Leave blank for a random seed");
            }
            drawTextField(commandBuffer, widget.rect, scale, menuSystem.createWorldSeed,
                          ui::kWorldNameFieldRules, style);
            return;
        }
    }

    // UI-4：图标钮的绘制（26.1 `SpriteIconButton`，iconOnly=true）。
    //
    // 底是和普通按钮同一张九宫格精灵，因此三种状态、按下色调、禁用灰全都自动一致；
    // 上面居中一张 15x15 的图标。图标按控件 id 查表——**图标是资源，而 ui:: 从不接触资源**，
    // 所以这张表住在绘制侧，不住在控件模型里。
    // UI-9：一个页签。26.1 `TabButton.extractWidgetRenderState`（:38-53）：
    //   1) 四态精灵铺满整个页签（选中 × 悬停/聚焦）；
    //   2) 选中时在 (x+2, y+2)-(right-2, bottom) 内衬一层菜单背景纹理——那是"选中的
    //      页签与内容区连成一片"的来源；
    //   3) 选中时再画一条下划线：宽 = min(文字宽, 页签宽-4)，居中，压在底边上方 2；
    //   4) 标签文字：未选中的**往下挪 3 像素**（选中那张精灵高出一截）。
    void drawTabButton(VkCommandBuffer commandBuffer, const ui::Widget& widget,
                       const ui::UiPoint& cursor, float scale, bool focused) const {
        const bool selected = tabIsSelected(widget);
        const bool hovered =
            widget.rect.contains(cursor.x, cursor.y) || focused;
        const auto sprite = selected ? (hovered ? GuiWidgetSprite::TabSelectedHighlighted
                                                : GuiWidgetSprite::TabSelected)
                                     : (hovered ? GuiWidgetSprite::TabHighlighted
                                                : GuiWidgetSprite::Tab);
        drawScaledGuiSprite(commandBuffer, widget.rect, kTabWidgetLayer,
                            guiWidgetSprite(guiWidgetSprites, sprite), scale, glm::vec4{1.0F});

        const float labelWidth = hudTextWidth(widget.label, scale);
        if (selected) {
            // ★ 内衬是**必需的**，不是装饰：`tab_selected.png` 的中间是完全透明的
            //   （实测中心像素 alpha = 0），不铺这一层，选中的页签中间就是个洞，
            //   背后的世界/全景会直接透出来。未选中那张自带 alpha 219 的底色，
            //   所以只有选中的需要。
            //   26.1 `TabButton.extractMenuBackground`：(x+2, y+2) 到 (right-2, bottom)。
            const ui::UiRect inlay{widget.rect.x + 2.0F * scale, widget.rect.y + 2.0F * scale,
                                   widget.rect.width - 4.0F * scale,
                                   widget.rect.height - 2.0F * scale};
            // ★ 用的是 **menu_background** 那张平铺纹理（26.1 `TabButton` 传的就是
            //   `Screen.MENU_BACKGROUND` 这个常量），**不是**列表底衬那张——
            //   两张都叫"背景"，但列表底衬在这个 uv 范围内几乎全透明，画上去等于没画
            //   （实测：换成纯色能看见，换回列表底衬就只剩背后的全景）。
            const auto background =
                titleBackgroundLayer(ui::ScreenBackgroundKind::PanoramaBlur);
            drawGuiSprite(commandBuffer, inlay, background.guiLayer,
                          ui::tiledBackgroundSource(inlay.width, inlay.height, scale),
                          background.tint);
            const auto underline = ui::tabUnderline(
                {widget.rect.x / scale, widget.rect.y / scale, widget.rect.width / scale,
                 widget.rect.height / scale},
                static_cast<int>(labelWidth / scale));
            drawHudQuad(commandBuffer,
                        {underline.x * scale, underline.y * scale, underline.width * scale,
                         underline.height * scale},
                        widget.enabled ? glm::vec4{1.0F, 1.0F, 1.0F, 1.0F}
                                       : glm::vec4{0.63F, 0.63F, 0.63F, 1.0F});
        }
        const auto box = ui::tabLabelBox({widget.rect.x / scale, widget.rect.y / scale,
                                          widget.rect.width / scale, widget.rect.height / scale},
                                         selected);
        drawHudText(commandBuffer, widget.label,
                    box.x * scale + (box.width * scale - labelWidth) * 0.5F,
                    box.y * scale + (box.height * scale - ui::kFontLineHeight * scale) * 0.5F,
                    scale, glm::vec4{1.0F});
    }

    // 这个页签是不是当前选中的那一个。
    // ★ 判据是**页面里的次序**（第几个 Tab 控件），不是 debugId——三个页签共用一个 id。
    [[nodiscard]] bool tabIsSelected(const ui::Widget& widget) const {
        const auto& page = drawPage_;
        std::size_t ordinal = 0;
        for (const auto& other : page) {
            if (other.kind != ui::WidgetKind::Tab) {
                continue;
            }
            if (&other == &widget) {
                return ordinal == static_cast<std::size_t>(menuSystem.createWorldTab);
            }
            ++ordinal;
        }
        return false;
    }

    // UI-11 / A5：一个复选框。左边一个方盒（四态精灵），右边一行文字。
    //
    // ★ 选精灵的判据是 `selected × isFocused()`（26.1 `Checkbox.extractContents`），
    //   **不是** hover——vanilla 的复选框悬停时盒子不变样，变的只有文字的效果。
    //   本作的按钮把焦点与悬停并成一档（`buttonVisualState`），复选框不能跟着并：
    //   那会让"鼠标扫过去"看起来像"选中了"。
    // ★ blit 的边长是 `ui::kCheckboxBoxSize`（17），不是精灵美术的 20。
    void drawCheckbox(VkCommandBuffer commandBuffer, const ui::Widget& widget, float scale,
                      bool focused) const {
        const auto parts = ui::checkboxParts(
            {widget.rect.x / scale, widget.rect.y / scale, widget.rect.width / scale,
             widget.rect.height / scale},
            static_cast<int>(ui::kFontLineHeight));
        const GuiWidgetSprite sprite = checkboxSprite(widget.checked, focused);
        drawScaledGuiSprite(commandBuffer,
                            {parts.box.x * scale, parts.box.y * scale, parts.box.width * scale,
                             parts.box.height * scale},
                            0.0F, guiWidgetSprite(guiWidgetSprites, sprite), scale,
                            glm::vec4{1.0F});
        // 26.1 `SafetyScreen.CHECK` 带 `withColor(-2039584)` = #E0E0E0。
        // 那是**这一句话**的颜色，不是复选框控件的默认色——控件本身不给文字着色。
        constexpr float kCheckTextChannel = 224.0F / 255.0F;
        drawHudText(commandBuffer, widget.label, parts.textX * scale, parts.textY * scale, scale,
                    {kCheckTextChannel, kCheckTextChannel, kCheckTextChannel, 1.0F});
    }

    void drawIconButton(VkCommandBuffer commandBuffer, const ui::Widget& widget, float cursorX,
                        float cursorY, float scale, bool focused = false) const {
        const auto id = static_cast<ui::WidgetId>(widget.debugId);
        const auto icon = id == ui::WidgetId::TitleAccessibility
                              ? GuiWidgetSprite::IconAccessibility
                              : GuiWidgetSprite::IconLanguage;
        const bool pressed = id == pressedMenuButton;
        const auto state =
            ui::buttonVisualState(widget.rect, cursorX, cursorY, widget.enabled, pressed, focused);
        const ui::UiRect snapped{std::floor(widget.rect.x), std::floor(widget.rect.y),
                                 std::floor(widget.rect.width + 0.5F),
                                 std::floor(widget.rect.height + 0.5F)};
        const GuiWidgetSprite face =
            state == ui::ButtonVisualState::Disabled
                ? GuiWidgetSprite::ButtonDisabled
                : (state == ui::ButtonVisualState::Normal ? GuiWidgetSprite::Button
                                                          : GuiWidgetSprite::ButtonHighlighted);
        drawScaledGuiSprite(commandBuffer, snapped, 0.0F,
                            guiWidgetSprite(guiWidgetSprites, face), scale, glm::vec4{1.0F});
        // 图标在钮内整数居中。算术在 ui::iconButtonIconRect 一处，那里有断言。
        const ui::UiRect iconRect = ui::iconButtonIconRect(snapped, scale);
        // 禁用态把图标一并压暗，与 vanilla 给禁用按钮上灰的做法一致
        const glm::vec4 tint = state == ui::ButtonVisualState::Disabled
                                   ? glm::vec4{0.63F, 0.63F, 0.63F, 1.0F}
                                   : glm::vec4{1.0F};
        drawScaledGuiSprite(commandBuffer, iconRect, 0.0F,
                            guiWidgetSprite(guiWidgetSprites, icon), scale, tint);
    }

    // UI-11 / A6：世界列表的一行，一比一照 26.1
    // `WorldSelectionList.WorldListEntry.extractContent():497-507` 与
    // `AbstractSelectionList.extractSelection():357-364`。
    //
    // 本作从前画的是自造的两层深灰底 + 名字 + 0.75 倍缩放的 "Seed 12345"。
    // 26.1 是：**只有选中的那一行**有底（一圈 1px 的边框色，里面纯黑），
    // 左边一张 32x32 的缩略图，右边三行字。
    void drawWorldListRow(VkCommandBuffer commandBuffer, const ui::HudLayout& layout,
                          std::size_t visibleIndex, std::size_t index) const {
        const float scale = layout.scale();
        const auto row = worldListRow(visibleIndex, layout);
        const auto parts = ui::worldRowParts(ui::logicalWorldListRow(visibleIndex, layout));
        const auto& summary = menuSystem.saveSummaries[index];
        const auto cursor = currentFramebufferCursor();
        const bool hovered = row.contains(cursor.x, cursor.y);
        // ★ 未选中的行**没有任何底衬**（`extractItem:348-354` 只在 selected 时画）。
        if (index == menuSystem.selectedWorldIndex) {
            // 边框色：有键盘焦点是白，否则 0xFF808080（`extractItem:350`）。
            // 本作的列表还没有"列表整体是否聚焦"这个状态，按无焦点那一档画。
            drawHudQuad(commandBuffer, row,
                        {ui::kWorldRowSecondaryChannel, ui::kWorldRowSecondaryChannel,
                         ui::kWorldRowSecondaryChannel, 1.0F});
            drawHudQuad(commandBuffer,
                        {row.x + scale, row.y + scale, row.width - 2.0F * scale,
                         row.height - 2.0F * scale},
                        {0.0F, 0.0F, 0.0F, 1.0F});
        }
        const ui::UiRect icon{parts.icon.x * scale, parts.icon.y * scale,
                              parts.icon.width * scale, parts.icon.height * scale};
        // 缩略图：这个存档有自己的 icon.png 就画它，没有就走 26.1 的**回落**分支
        // （`FaviconTexture.MISSING_LOCATION` = `misc/unknown_server.png`）。
        // ★ 槽位按 identifier 反查，不是"第几行就是第几个槽位"——列表滚起来以后
        //   那两个数就不一样了（见 MenuSystem::worldIconSlots 上的注释）。
        if (const auto slot = worldIconSlot(summary.identifier); slot.has_value()) {
            drawGuiSprite(commandBuffer, icon, kWorldIconLayer, worldIconSlotRect(*slot));
        } else {
            drawGuiSprite(commandBuffer, icon, kTabWidgetLayer,
                          {static_cast<float>(kWorldIconFallbackSpriteX),
                           static_cast<float>(kWorldIconFallbackSpriteY),
                           static_cast<float>(kWorldIconFallbackSize),
                           static_cast<float>(kWorldIconFallbackSize)});
        }
        if (hovered) {
            // `graphics.fill(contentX, contentY, +32, +32, -1601138544)` = 0xA0909090。
            drawHudQuad(commandBuffer, icon,
                        {ui::kWorldIconHoverChannel, ui::kWorldIconHoverChannel,
                         ui::kWorldIconHoverChannel, ui::kWorldIconHoverAlpha});
        }
        const glm::vec4 secondary{ui::kWorldRowSecondaryChannel, ui::kWorldRowSecondaryChannel,
                                  ui::kWorldRowSecondaryChannel, 1.0F};
        const float textX = parts.textX * scale;
        drawHudText(commandBuffer, clipToWorldRow(summary.displayName, scale), textX,
                    parts.nameY * scale, scale, {1.0F, 1.0F, 1.0F, 1.0F});
        drawHudText(commandBuffer,
                    clipToWorldRow(ui::worldRowMetaLine(
                                       summary.identifier,
                                       formatWorldLastPlayed(summary.lastPlayedUnixSeconds)),
                                   scale),
                    textX, parts.metaY * scale, scale, secondary);
        drawHudText(commandBuffer, clipToWorldRow(worldRowInfoLine(summary), scale), textX,
                    parts.infoY * scale, scale, secondary);
    }

    // 这个存档的缩略图在图集那一层的第几个槽位。没有就是 nullopt（画回落图标）。
    [[nodiscard]] std::optional<int> worldIconSlot(std::string_view identifier) const {
        for (std::size_t slot = 0; slot < menuSystem.worldIconSlots.size(); ++slot) {
            if (menuSystem.worldIconSlots[slot] == identifier) {
                return static_cast<int>(slot);
            }
        }
        return std::nullopt;
    }

    // 三行字都受同一个宽度上限（`WorldSelectionList:421`，见 ui::kWorldRowMaxTextWidth）。
    // 26.1 用 `StringWidget.setMaxWidth` 把超长的一行**裁**掉（CLAMPED），不是换行。
    [[nodiscard]] std::string clipToWorldRow(std::string text, float scale) const {
        const float limit = static_cast<float>(ui::kWorldRowMaxTextWidth) * scale;
        while (!text.empty() && hudTextWidth(text, scale) > limit) {
            // 按 UTF-8 码点边界退，绝不切在字节中间。
            do {
                text.pop_back();
            } while (!text.empty() &&
                     (static_cast<unsigned char>(text.back()) & 0xC0U) == 0x80U);
        }
        return text;
    }

    // 第 3 行：26.1 `LevelSummary.createInfo():166-186` —— 游戏模式 + 版本。
    // 拼接在 ui::worldRowInfoLine 一处（那里有断言），这里只负责取译文。
    [[nodiscard]] std::string worldRowInfoLine(const persistence::SaveSummary& summary) const {
        const std::string modeKey =
            "gameMode." + std::string{gameplay::gameModeName(summary.gameMode)};
        const std::string_view modeFallback = summary.gameMode == gameplay::GameMode::Creative
                                                  ? "Creative Mode"
                                                  : "Survival Mode";
        return ui::worldRowInfoLine(translated(modeKey, modeFallback),
                                    translated("selectWorld.version", "Version"),
                                    summary.versionName);
    }

    // 第 2 行括号里的日期。26.1 用 `Util.localizedDateFormatter(FormatStyle.SHORT)`，
    // 也就是**跟随系统语言环境**的短日期；本作没有本地化的日期格式化，
    // 固定用 ISO 的 `YYYY-MM-DD HH:MM`。已登记为偏差 D35。
    //
    // ★ **出图时按 UTC 解释，而不是去改进程的 `TZ`**。这个日期串是本地时区的函数，
    //   不钉住它，同一份夹具在两台机器上会渲染成两串不同的字（与 options.properties
    //   那次"结论取决于跑它的环境"同族）。第一版用的是 `setenv("TZ","UTC")` ——
    //   **两个毛病**：Windows 的 CRT 没有 `setenv`（交叉构建当场报错），而
    //   `getenv`/`setenv` 并发是未定义行为。把时区做成参数，两个问题一起没了。
    [[nodiscard]] std::string formatWorldLastPlayed(std::int64_t unixSeconds) const {
        if (unixSeconds <= 0) {
            return {};   // 26.1 的 `lastPlayed != -1L` 分支：没有记录就不加括号那一段
        }
        const std::tm broken = core::brokenDownTime(unixSeconds, uiCaptureActive);
        std::array<char, 32> buffer{};
        const std::size_t written =
            std::strftime(buffer.data(), buffer.size(), "%Y-%m-%d %H:%M", &broken);
        return std::string{buffer.data(), written};
    }

    // 选择列表的一行（语言 / 世界）：一层淡背景加一行文本，悬停时提亮。
    // UI-6b 之前按键绑定行也走这里，所以它从前叫 drawKeyBindRow；那一行现在是
    // Label + Button 两个控件，不再经过这条路径。
    void drawSelectionListRow(VkCommandBuffer commandBuffer, const ui::Widget& widget,
                              float cursorX, float cursorY, float scale,
                              bool packMoveUpAvailable = false,
                              bool packMoveDownAvailable = false) const {
        const bool hovered = widget.rect.contains(cursorX, cursorY);
        drawHudQuad(commandBuffer, widget.rect,
                    hovered ? glm::vec4{0.28F, 0.28F, 0.32F, 0.9F}
                            : glm::vec4{0.0F, 0.0F, 0.0F, 0.55F});
        drawHudText(commandBuffer, widget.label, widget.rect.x + 4.0F * scale,
                    widget.rect.y + 1.5F * scale, scale, {1.0F, 1.0F, 1.0F, 1.0F}, false);
        if (ui::isPackRowWidget(widget) && hovered) {
            drawTransferRowIcons(commandBuffer, widget, cursorX, cursorY, scale,
                                 packMoveUpAvailable, packMoveDownAvailable);
        }
    }

    // UI-10 / D24：可转移列表一行里的箭头（26.1 `TransferableSelectionList:153-195`）。
    //
    // 悬停时先在 32x32 的图标位上铺一层 `0xA0909090`，再按光标落在**哪一块热区**
    // 画对应的箭头：可用那一栏整格是 select；已选那一栏左半是 unselect、右上 1/4
    // 是 move_up、右下 1/4 是 move_down。
    //
    // ★ 三张箭头精灵**都画在整个图标位上**（源码里全是
    //   `blit(..., getContentX(), getContentY(), 32, 32)`），热区只决定用哪一张、
    //   要不要用 highlighted 那一版。把箭头画进各自的热区矩形里是错的——那样
    //   move_up 的箭头会被压扁到 16x16。
    void drawTransferRowIcons(VkCommandBuffer commandBuffer, const ui::Widget& row,
                              float cursorX, float cursorY, float scale, bool canMoveUp,
                              bool canMoveDown) const {
        const ui::UiRect logicalRow{row.rect.x / scale, row.rect.y / scale,
                                    row.rect.width / scale, row.rect.height / scale};
        const auto iconCell = ui::transferIconCell(logicalRow);
        const ui::UiRect icon{iconCell.x * scale, iconCell.y * scale, iconCell.width * scale,
                              iconCell.height * scale};
        drawHudQuad(commandBuffer, icon, {0.565F, 0.565F, 0.565F, 0.627F});

        const auto sprite = [&](GuiWidgetSprite which) {
            drawScaledGuiSprite(commandBuffer, icon, kTabWidgetLayer,
                                guiWidgetSprite(guiWidgetSprites, which), scale,
                                glm::vec4{1.0F});
        };
        const auto zones = ui::transferIconZones(iconCell);
        const auto over = [&](const ui::UiRect& zone) {
            return ui::UiRect{zone.x * scale, zone.y * scale, zone.width * scale,
                              zone.height * scale}
                .contains(cursorX, cursorY);
        };
        if (!ui::isSelectedPackRow(row)) {
            sprite(over(ui::UiRect{iconCell}) ? GuiWidgetSprite::TransferSelectHighlighted
                                              : GuiWidgetSprite::TransferSelect);
            return;
        }
        sprite(over(zones.unselect) ? GuiWidgetSprite::TransferUnselectHighlighted
                                    : GuiWidgetSprite::TransferUnselect);
        if (canMoveUp) {
            sprite(over(zones.moveUp) ? GuiWidgetSprite::TransferMoveUpHighlighted
                                      : GuiWidgetSprite::TransferMoveUp);
        }
        if (canMoveDown) {
            sprite(over(zones.moveDown) ? GuiWidgetSprite::TransferMoveDownHighlighted
                                        : GuiWidgetSprite::TransferMoveDown);
        }
    }

    // 按键设置列表的滚动条：仅当动作数多于可见窗口时绘制
    // 滑块长度对应可见比例，随滚动偏移移动（世界列表与语言列表同理）
    // UI-4：滚动条的**唯一**画法。26.1 的 AbstractScrollArea 用两张精灵画它
    // （`widget/scroller_background` 铺整条轨道、`widget/scroller` 画滑块），
    // 而不是手绘颜色——GUI spec §2.7 写的那两个颜色（轨道 0xFF000000、滑块 0xFF808080
    // 加亮边 0xFFC0C0C0）是 1.20.2 之前的画法。两张都是 6x32 九宫格 border 1，
    // 因此滑块拉长时两端那 1px 的亮边仍是 1px。
    //
    // 几何一律来自 ui::ScrollList，绘制这里不再自己算滑块高度或位置。
    void drawScrollbar(VkCommandBuffer commandBuffer, const ui::HudLayout& layout,
                       const ui::ScrollList& list, std::size_t itemCount,
                       std::size_t firstRow) const {
        if (!list.scrollable(itemCount)) {
            return;  // everything fits; vanilla draws no scrollbar either
        }
        const float scale = layout.scale();
        const auto toFb = [scale](const ui::UiRect& logical) {
            return ui::UiRect{logical.x * scale, logical.y * scale, logical.width * scale,
                              logical.height * scale};
        };
        drawScaledGuiSprite(commandBuffer, toFb(ui::scrollListScrollbar(list)), 0.0F,
                            guiWidgetSprite(guiWidgetSprites, GuiWidgetSprite::ScrollerBackground),
                            scale);
        drawScaledGuiSprite(commandBuffer,
                            toFb(ui::scrollListThumb(list, itemCount, firstRow)), 0.0F,
                            guiWidgetSprite(guiWidgetSprites, GuiWidgetSprite::Scroller), scale);
    }

    // UI-4：选中行的高亮框。26.1 的 `AbstractSelectionList.renderSelection` 画的是
    // 一圈灰边加黑底，而不是只把文字提亮——本作此前三张列表**一处高亮都没有**，
    // 选中语言后没有任何视觉反馈。
    void drawListSelection(VkCommandBuffer commandBuffer, const ui::UiRect& row, float scale,
                           bool listFocused) const {
        // `extractSelection`：先用外框色填满整个条目矩形，再用纯黑填内缩 1 像素的部分。
        // 框在矩形**之内**，不在外面。外框色：列表有焦点是白（-1），没有是灰（0xFF808080）。
        const glm::vec4 outline = listFocused ? glm::vec4{1.0F, 1.0F, 1.0F, 1.0F}
                                              : glm::vec4{0.502F, 0.502F, 0.502F, 1.0F};
        drawHudQuad(commandBuffer, row, outline);
        drawHudQuad(commandBuffer,
                    {row.x + scale, row.y + scale, row.width - 2.0F * scale,
                     row.height - 2.0F * scale},
                    {0.0F, 0.0F, 0.0F, 1.0F});
    }

    // UI-6c：绑定列表里的分类标题行（26.1 `KeyBindsList.CategoryEntry`）。
    //
    // 它**不是控件**：纯文本、不可交互、焦点不该停在上面，所以它不进 ui::Page，
    // 由这里直接画。26.1 的 CategoryEntry 把标题居中放在条目底端
    // （`extractContent`：`x = width/2 - w/2`，`y = getContentBottom() - height`）。
    void drawKeyBindCategoryRows(VkCommandBuffer commandBuffer, const ui::HudLayout& layout,
                                 float scale) const {
        const float fbWidth = static_cast<float>(swapchainExtent.width);
        const auto list = ui::keyBindsScrollList(layout);
        const std::size_t first =
            std::min(menuSystem.controlsListFirstIndex, ui::kKeyBindListRowCount);
        const std::size_t visible = ui::keyBindsVisibleRowCount(
            fbWidth, static_cast<float>(swapchainExtent.height), menuSystem.guiScaleSetting,
            menuSystem.forceUnicodeFont);
        for (std::size_t offset = 0; offset < visible; ++offset) {
            const std::size_t row = first + offset;
            if (row >= ui::kKeyBindListRowCount) {
                break;
            }
            const auto entry = ui::keyBindListRow(row);
            if (!entry.isCategory) {
                continue;
            }
            const std::string label =
                translated(input::categoryTranslationKey(entry.category),
                           input::categoryDisplayName(entry.category));
            const auto rowRect = ui::scrollListRow(list, offset);
            const auto content = ui::listRowContent(rowRect);
            // 底端对齐：标题贴着这一行的下缘，于是它读起来像是下面那组的抬头。
            const float y = (content.y + content.height -
                             static_cast<float>(ui::kFontLineHeight)) * scale;
            drawHudText(commandBuffer, label,
                        (fbWidth - hudTextWidth(label, scale)) * 0.5F, y, scale,
                        {1.0F, 1.0F, 1.0F, 1.0F});
        }
    }

    void drawKeyBindsScrollbar(VkCommandBuffer commandBuffer, const ui::HudLayout& layout) const {
        const float fbWidth = static_cast<float>(swapchainExtent.width);
        // UI-6c：滚动条的比例按**行**数算（含分类标题行）。用动作数会让滑块偏长、
        // 且滚到底时还剩几行没进来。
        const std::size_t total = ui::kKeyBindListRowCount;
        const std::size_t visible = ui::keyBindsVisibleRowCount(
            fbWidth, static_cast<float>(swapchainExtent.height), menuSystem.guiScaleSetting, menuSystem.forceUnicodeFont);
        if (total <= visible) {
            return;  // everything fits; no scrollbar
        }
        const auto list = ui::keyBindsScrollList(layout);
        const std::size_t first = std::min(menuSystem.controlsListFirstIndex, total - visible);
        drawScrollbar(commandBuffer, layout, list, total, first);
    }

    // UI-6f（D15）：设置列表里的**分节标题行**（26.1 `OptionsList.addHeader`）。
    //
    // ★ 它**不是控件**：纯文本、不可交互、焦点不该停在上面，所以它不进 ui::Page，
    //   由这里直接画——与绑定列表的分类标题行同一做法。
    //   行高不是 25：首个 13、其后 31（`OptionsList.java:52-56` 的
    //   `paddingTop + lineHeight + 4`），那 18 的留白属于标题行本身。
    void drawOptionsSectionHeaders(VkCommandBuffer commandBuffer, const ui::HudLayout& layout,
                                   float scale) const {
        const auto page = menuSystem.pageStack.current();
        const auto groups = ui::optionsGroupsOf(page);
        const auto frame = ui::optionsFrame(layout, page);
        const auto list = ui::optionsScrollList(frame.contentBox());
        const std::size_t total = ui::optionsRowCountOf(page);
        const auto window = ui::optionsWindowFor(layout, page, menuSystem.optionsListFirstIndex);
        if (window.rowCount == 0U) {
            return;
        }
        for (std::size_t offset = 0; offset < window.rowCount; ++offset) {
            const std::size_t row = window.firstRow + offset;
            if (row >= total) {
                break;
            }
            const auto info = ui::optionsRowAt(groups, row);
            if (!info.isHeader) {
                continue;
            }
            const std::string text = translated(info.headerKey, info.headerFallback);
            // 标题在这一行里**底端对齐**（26.1 的 HeaderEntry 把文字贴在条目下缘，
            // 上面那段 paddingTop 是与前一节之间的留白）。
            const int top = static_cast<int>(list.y) +
                            ui::optionsRowTop(groups, row) - ui::optionsRowTop(groups, window.firstRow);
            const float y =
                static_cast<float>(top + info.height - ui::kOptionsHeaderLineHeight -
                                   ui::kOptionsHeaderPadding) * scale;
            // ★ **左对齐于行的左缘，不是居中**（spec §13.2 #14 的答案，UI-12 查源码关掉）：
            //   26.1 `OptionsList.HeaderEntry.extractContent`（:196）是
            //   `widget.setPosition(screen.width / 2 - 155, ...)`，而 155 正是
            //   `getRowWidth() / 2`——也就是那张 310 宽列表的**行左缘**。
            //   本作从前把它按整屏居中，行左缘与画布中线在窄画布上差得出来。
            drawHudText(commandBuffer, text, static_cast<float>(list.rowLeft()) * scale, y, scale,
                        {1.0F, 1.0F, 1.0F, 1.0F});
        }
    }

    // UI-6e ③：资源包选择的两栏。26.1 `PackSelectionScreen`：两张 200 宽的列表，
    // 各自有标题（`pack.available.title` / `pack.selected.title`）与底衬。
    //
    // ★ 行的**矩形不在这里算**——它由 `layoutPageInto` 填进 Widget，绘制只按 Widget
    //   走一遍。世界列表那一屏是反面教材：它的行矩形由绘制侧另算一份，命中测试
    //   再算第三份，于是"点到的"和"看到的"是两条路。
    void drawResourcePackColumns(VkCommandBuffer commandBuffer, const ui::HudLayout& layout,
                                 float scale) const {
        const auto frame =
            ui::headerAndFooterLayout(layout.logicalWidth(), layout.logicalHeight());
        const auto lists = ui::dualColumnLists(frame.contentBox(), layout.logicalWidth());
        // 两栏各自的底衬与分隔线
        for (const auto& list : {lists.available, lists.selected}) {
            const ui::UiRect box{static_cast<float>(list.x) * scale,
                                 static_cast<float>(list.y) * scale,
                                 static_cast<float>(list.width) * scale,
                                 static_cast<float>(list.height) * scale};
            drawListBackground(commandBuffer, box, scale);
            drawListSeparators(commandBuffer, box, scale);
        }
        // 两栏的标题，画在各自列表正上方
        const auto columnTitle = [&](const ui::ScrollList& list, std::string_view key,
                                     std::string_view fallback) {
            const std::string text = translated(key, fallback);
            const float centre = (static_cast<float>(list.x) +
                                  static_cast<float>(list.width) * 0.5F) * scale;
            drawHudText(commandBuffer, text, centre - hudTextWidth(text, scale) * 0.5F,
                        (static_cast<float>(list.y) - 11.0F) * scale, scale,
                        {1.0F, 1.0F, 1.0F, 1.0F});
        };
        columnTitle(lists.available, "pack.available.title", "Available");
        columnTitle(lists.selected, "pack.selected.title", "Selected");
        // UI-10 / D24：两栏各自的滚动条。★ 只有真的滚得动才画（26.1 的
        //   `AbstractScrollArea` 同样是 `scrollable()` 才画），否则一屏装得下的
        //   列表旁边会挂一条永远满格的假滚动条。
        std::size_t availableTotal = 0;
        for (const auto& pack : packLibrary.packs()) {
            if (!packLibrary.isEnabled(pack.id)) {
                ++availableTotal;
            }
        }
        const ui::HudLayout hudLayout{static_cast<float>(swapchainExtent.width),
                                      static_cast<float>(swapchainExtent.height),
                                      menuSystem.guiScaleSetting, menuSystem.forceUnicodeFont};
        drawScrollbar(commandBuffer, hudLayout, lists.available, availableTotal,
                      drawContext_.availablePackFirstRow);
        drawScrollbar(commandBuffer, hudLayout, lists.selected,
                      packLibrary.draftOrder().size(), drawContext_.selectedPackFirstRow);

        // 每一行的包名与描述。行矩形取自已经布局好的 Widget。
        const auto& page = buildDrawPage();
        std::size_t availableRow = 0;
        std::size_t selectedRow = 0;
        std::vector<std::string> available;
        for (const auto& pack : packLibrary.packs()) {
            if (!packLibrary.isEnabled(pack.id)) {
                available.push_back(pack.id);
            }
        }
        for (const auto& widget : page) {
            if (!ui::isPackRowWidget(widget)) {
                continue;
            }
            const bool right = ui::isSelectedPackRow(widget);
            const std::size_t row = right ? selectedRow++ : availableRow++;
            const auto& ids = right ? packLibrary.draftOrder() : available;
            if (row >= ids.size()) {
                continue;
            }
            const auto* entry = packLibrary.find(ids[row]);
            if (entry == nullptr) {
                continue;
            }
            // 右栏当前选中的那一行加一圈高亮框——调序按钮作用在它身上，
            // 没有这个反馈玩家不知道自己在移哪一个。
            if (right && row == menuSystem.selectedPackRow) {
                drawHudQuad(commandBuffer, widget.rect, {1.0F, 1.0F, 1.0F, 1.0F});
                drawHudQuad(commandBuffer,
                            {widget.rect.x + scale, widget.rect.y + scale,
                             widget.rect.width - 2.0F * scale,
                             widget.rect.height - 2.0F * scale},
                            {0.0F, 0.0F, 0.0F, 1.0F});
            }
            const auto text = ui::transferTextCell(
                {widget.rect.x / scale, widget.rect.y / scale, widget.rect.width / scale,
                 widget.rect.height / scale});
            // ★ 截到格子里。不截的话包描述会一路画出画布右边缘——实测如此。
            //   26.1 的做法是 scissor + 滚动（偏差 D17），截断是它之前的下界。
            const float room = text.width * scale;
            const auto fit = [&](std::string_view value) {
                return ui::truncateToWidth(value, room, [&](std::string_view probe) {
                    return hudTextWidth(probe, scale);
                });
            };
            drawHudText(commandBuffer, fit(entry->title), text.x * scale, text.y * scale, scale,
                        entry->compatible ? glm::vec4{1.0F, 1.0F, 1.0F, 1.0F}
                                          : glm::vec4{1.0F, 0.6F, 0.4F, 1.0F});
            if (!entry->description.empty()) {
                drawHudText(commandBuffer, fit(entry->description), text.x * scale,
                            (text.y + static_cast<float>(ui::kFontLineHeight) + 1.0F) * scale,
                            scale, {0.66F, 0.66F, 0.66F, 1.0F});
            }
        }

        // 提交过一次之后提示"重启生效"——换包不做热重载（依据见 known-debt），
        // 没有这句提示玩家会以为开关没起作用。
        if (menuSystem.packRestartRequired) {
            const std::string note =
                translated("options.rebedrock.pack.restart", "Changes apply after restart");
            drawHudText(commandBuffer, note,
                        (static_cast<float>(swapchainExtent.width) -
                         hudTextWidth(note, scale)) * 0.5F,
                        static_cast<float>(frame.contentBox().y + frame.contentBox().height +
                                           2) * scale,
                        scale, {1.0F, 0.85F, 0.4F, 1.0F});
        }
    }

    // UI-6d：三段式设置页那张 OptionsList 的滚动条。装得下时不画——`optionsMaximumFirstRow`
    // 为 0 正是"装得下"，与其余三张列表同一约定。
    void drawOptionsScrollbar(VkCommandBuffer commandBuffer, const ui::HudLayout& layout) const {
        const auto page = menuSystem.pageStack.current();
        if (ui::optionsMaximumFirstRow(layout, page) == 0U) {
            return;
        }
        const auto window = ui::optionsWindowFor(layout, page, menuSystem.optionsListFirstIndex);
        drawScrollbar(commandBuffer, layout,
                      ui::optionsScrollList(
                          ui::headerAndFooterLayout(layout.logicalWidth(), layout.logicalHeight())
                              .contentBox()),
                      ui::optionsRowCountOf(page), window.firstRow);
    }

    // 不再收描述符集：背景（全景 / 模糊 / 遮罩 / 渐变）由 drawHud 一处按
    // ui::screenBackground 的档位表画，这里只剩前端各屏自己的内容。
    void drawFrontend(VkCommandBuffer commandBuffer, const ui::HudLayout& layout) const {
        const auto page = menuSystem.pageStack.current();
        const float scale = layout.scale();
        if (page == ui::PageId::Title) {
            // UI-2：主菜单的标题不是一行放大的文字，而是 gui/title/minecraft.png 加
            // gui/title/edition.png 两张贴图，再配左下的版本行与右下的版权行（spec §6.3）。
            // 从前这里画的是 2 倍缩放的 "MC Rebedrock"，那不是 26.1 的任何一个元素。
            drawTitleBranding(commandBuffer, layout);
        } else if (page != ui::PageId::CreateWorld) {
            // ★ UI-9：创建世界**没有标题行**——26.1 `CreateWorldScreen.repositionElements`
            //   把 `layout.setHeaderHeight(tabNavigationBar.getRectangle().bottom())`，
            //   也就是**标签栏就是这一屏的页眉**。多画一行标题会压在页签上。
            const std::string title = frontendTitle(page);
            drawHudText(commandBuffer, title,
                        (static_cast<float>(swapchainExtent.width) - hudTextWidth(title, scale)) *
                            0.5F,
                        14.0F * scale, scale, {1.0F, 1.0F, 1.0F, 1.0F});
        }

        if (page == ui::PageId::WorldList) {
            const std::size_t visibleRows = saveListVisibleRowCount();
            const std::size_t maximumFirst = menuSystem.saveSummaries.size() > visibleRows
                                                 ? menuSystem.saveSummaries.size() - visibleRows
                                                 : 0U;
            const std::size_t first = std::min(menuSystem.worldListFirstIndex, maximumFirst);
            const std::size_t remaining =
                menuSystem.saveSummaries.size() - std::min(first, menuSystem.saveSummaries.size());
            const std::size_t visible = std::min(remaining, visibleRows);
            // 26.1 的列表背景与周围菜单背景是两张可各自被资源包覆盖的贴图。
            // ★ 带的矩形取自 `ui::worldListBox`，**不再在这里自己算一份**：那份写的是
            //   `visibleRows * 22 + 8`，而 A6 把行距改成了 36，于是下缘那条分隔线
            //   穿过第五行的中间、后面的行画在带外面（现场 export/savelist-problem.png）。
            const ui::UiRect listBand = ui::worldListBox(layout);
            drawListBackground(commandBuffer, listBand, scale);
            drawListSeparators(commandBuffer, listBand, scale);
            if (visible == 0U) {
                const std::string_view empty = "No worlds yet. Create one to begin.";
                drawHudText(
                    commandBuffer, empty,
                    (static_cast<float>(swapchainExtent.width) - hudTextWidth(empty, scale)) * 0.5F,
                    34.0F * scale, scale, {0.85F, 0.85F, 0.85F, 1.0F});
            }
            for (std::size_t visibleIndex = 0; visibleIndex < visible; ++visibleIndex) {
                drawWorldListRow(commandBuffer, layout, visibleIndex, first + visibleIndex);
            }
        } else if (page == ui::PageId::CreateWorld) {
            drawCreateWorldForm(commandBuffer, layout);
        } else if (page == ui::PageId::EditWorld) {
            drawWorldNameField(commandBuffer, layout, menuSystem.editWorldName);
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

    // vanilla `Gui.updateVignetteBrightness`:
    //   levelBrightness = Lightmap.getBrightness(dim, getMaxLocalRawBrightness(eye))
    //   target          = clamp(1 - levelBrightness, 0, 1)
    //
    // The load-bearing detail is `getMaxLocalRawBrightness`, which is
    // `max(blockLight, skyLight - skyDarken)` — an INTEGER darkening SUBTRACTED
    // from an integer sky level. `skyDarken` is `(int)(15 - SKY_LIGHT_LEVEL)`,
    // and that track is flat 1.0 from tick 133 to 11867, so it is 0 for the
    // whole day: outdoors in daylight vanilla reads level 15, brightness 1, and
    // the vignette is exactly off.
    //
    // This used to MULTIPLY the sky level by the sun-elevation curve instead.
    // That curve is fractional for most of the day, so a player standing in open
    // daylight got, say, 0.6 -> brightness 0.27 -> a vignette at 73% strength.
    // The corners were crushed in broad daylight, which is what "the vignette is
    // much stronger than it used to be" was.
    void updateVignetteDarkness(float deltaSeconds) {
        const int skyDarken = SkyLight::skyDarken(clientMirror.world().dayTimeTicks);
        const auto& playerSnap = clientMirror.player();
        const float eyeHeight = playerSnap.sneaking ? gameplay::PlayerController::kSneakingEyeHeight
                                                    : gameplay::PlayerController::kEyeHeight;
        const glm::vec3 eye = playerSnap.physicsCurrent + glm::vec3{0.0F, eyeHeight, 0.0F};
        const int eyeX = static_cast<int>(std::floor(eye.x));
        const int eyeY = static_cast<int>(std::floor(eye.y));
        const int eyeZ = static_cast<int>(std::floor(eye.z));
        const int sky = static_cast<int>(lightWorld.skyLight(eyeX, eyeY, eyeZ)) - skyDarken;
        const int block = static_cast<int>(lightWorld.blockLight(eyeX, eyeY, eyeZ));
        const float light = static_cast<float>(std::max(0, std::max(block, sky))) / 15.0F;
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
        drawListBackground(commandBuffer, box, scale);
        drawListSeparators(commandBuffer, box, scale);
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
            // UI-4：选中项走 26.1 的形态——灰边黑底的高亮框（renderSelection），
            // 而不是从前那块半透明的深灰填充。悬停仍是那块淡填充。
            if (selected) {
                // 语言列表在本作里始终是该屏的焦点控件组，因此用白框——
                // 与 26.1 「列表有焦点则白、否则灰」一致。
                drawListSelection(commandBuffer, rectangle, scale, /*listFocused=*/true);
            } else if (hovered) {
                drawHudQuad(commandBuffer, rectangle, {0.16F, 0.16F, 0.16F, 0.90F});
            }
            const std::string& name = index < menuSystem.languageDisplayNames.size()
                                          ? menuSystem.languageDisplayNames[index]
                                          : menuSystem.languageCodes[index];
            // 在框内居中，与 vanilla 语言项的绘制一致：每个名字画在 width/2 - 文本宽/2
            drawHudText(commandBuffer, name, centredLabelX(rectangle, name, scale),
                        rectangle.y + 2.0F * scale, scale,
                        selected ? glm::vec4{1.0F, 1.0F, 1.0F, 1.0F}
                                 : glm::vec4{0.85F, 0.85F, 0.85F, 1.0F});
        }
        drawScrollbar(commandBuffer, layout,
                      ui::languageScrollList(layout),
                      menuSystem.languageCodes.size(), first);
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
        // 背景由 drawHud 一处按档位表画：暂停屏走「整帧模糊 + inworld_menu_background」，
        // 死亡屏走 `DeathScreen.extractDeathBackground` 的红渐变（0x60500000 → 0xA0803030）。
        // 从前这里各自铺一层：暂停屏铺的是背包那条灰渐变（于是世界永远清晰、只是被压暗），
        // 死亡屏铺的是一块 rgba(0.25, 0, 0, 0.58) 的平色（渐变的中点近似）。
        const float scale = layout.scale();
        // UI-6c：标题走 ui::pageTitle 那张表。从前这里是一串嵌套三目，每加一屏多嵌一层，
        // 而"某一屏的标题写成了另一屏的"在画面上只是一行字不对，没有东西会红。
        const auto titleEntry = ui::pageTitle(menuSystem.pageStack.current());
        const std::string title = translated(titleEntry.key, titleEntry.fallback);
        // 按键设置是三段式布局（页眉 / 滚动列表 / 页脚），列表因此有自己的底衬与
        // 上下两道分隔线，和语言、世界列表两屏同一套（`AbstractSelectionList:219-227`）。
        // 从前这一屏的列表直接坐在菜单背景上，既没有底衬也没有分隔线。
        const auto currentPage = menuSystem.pageStack.current();
        // UI-6c：走三段式版面（§2.8）的页面，标题在**页眉里居中**——那正是
        // `layout.addTitleHeader(title, font)` 的意思（`OptionsSubScreen.java:37`）。
        //
        // ★ 判定走 ui::pageLayoutKind，不再是这里手写的"KeyBinds 或 Controls"。
        //   UI-6d 把视频设置与高级图形也改成三段式之后，那份手写清单立刻就说了假话：
        //   两屏的标题会掉回"第一个按钮上方 30px"，而第一个按钮此时在列表里，
        //   标题于是压在列表第一行上。版面种类只有 ui/PageLayoutKind.hpp 一处来源。
        // ★ 判据是"是不是三段式"，不是"是不是那一种三段式"。见 usesHeaderAndFooter
        //   上面那段注释：这份判断已经说过两次假话，两次都是枚举了当时的取值。
        const bool headerAndFooterPage = ui::usesHeaderAndFooter(ui::pageLayoutKind(currentPage));
        if (currentPage == ui::PageId::KeyBinds) {
            const auto box = ui::keyBindsListBox(layout);
            drawListBackground(commandBuffer, box, scale);
            drawListSeparators(commandBuffer, box, scale);
            drawKeyBindCategoryRows(commandBuffer, layout, scale);
        } else if (headerAndFooterPage &&
                   ui::pageLayoutKind(currentPage) == ui::PageLayoutKind::HeaderFooterList) {
            // ★ 走 ui::optionsFrame，不是自己造一个默认 frame：带副页眉的页面
            //   （Options）页眉高 49 而不是 33。自己造的后果是副页眉的控件压在
            //   页眉分隔线上——实测如此。
            const auto frameBox = ui::optionsFrame(layout, currentPage).contentBox();
            const auto box = ui::UiRect{frameBox.x * scale, frameBox.y * scale,
                                        frameBox.width * scale, frameBox.height * scale};
            drawListBackground(commandBuffer, box, scale);
            drawListSeparators(commandBuffer, box, scale);
            drawOptionsSectionHeaders(commandBuffer, layout, scale);
        } else if (currentPage == ui::PageId::ResourcePacks) {
            drawResourcePackColumns(commandBuffer, layout, scale);
        }
        const std::size_t buttonCount = menuButtonCount();
        const auto firstButton =
            frontendButtonRect(layout, menuSystem.pageStack.current(), 0, buttonCount);
        const float titleScale = deathScreen ? scale * 2.0F : scale;
        // 其余页面还没有三段式版面，标题仍摆在第一个按钮上方 30px。
        const auto frame =
            ui::headerAndFooterLayout(layout.logicalWidth(), layout.logicalHeight());
        const float titleWidth = hudTextWidth(title, titleScale);
        const float titleY =
            headerAndFooterPage
                ? static_cast<float>(frame.headerTitle(0, ui::kFontLineHeight).y) * scale
                : firstButton.y - 30.0F * titleScale;
        // UI-11 / A5：提示屏的标题是**页面里的第一个控件**（26.1
        // `WarningScreen.init` 把 StringWidget 加进内容列），位置由整块内容的居中
        // 决定。这里再画一行就是两个标题——判据走 ui::drawsTitleAsWidget 那张
        // 不带 default 的表，而不是在这里写 `page == AdvancedGraphicsNotice`。
        if (!ui::drawsTitleAsWidget(ui::pageLayoutKind(currentPage))) {
            drawHudText(commandBuffer, title,
                        (static_cast<float>(swapchainExtent.width) - titleWidth) * 0.5F, titleY,
                        titleScale, {1.0F, 1.0F, 1.0F, 1.0F});
        }
        drawMenuWidgets(commandBuffer, buildDrawPage(), scale);
        // 按键绑定列表（中段）的滚动条，仅当动作数超出可见窗口时绘制
        if (currentPage == ui::PageId::KeyBinds) {
            drawKeyBindsScrollbar(commandBuffer, layout);
        } else if (headerAndFooterPage) {
            drawOptionsScrollbar(commandBuffer, layout);
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
                {kItemModeMatrixViewModel, 0.0F, 0.0F, 1.0F},
                {dimensions.x, dimensions.y, dimensions.z, 0.0F},
                cubeWorld,
            };
            vkCmdPushConstants(commandBuffer, itemPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                               sizeof(push), &push);
            vkCmdDraw(commandBuffer, 36U, 1, 0, 0);
        };
        // 与世界里的玩家同一条契约：骨骼数来自模型，矩阵来自姿态，而两者在动画器
        // 第一次 evaluate 之前并不同时可用。未绑定就一根骨骼也不画——留在循环条件里
        // 而不是提前 return，是因为下面还要还原 scissor 与管线。
        const std::size_t previewBoneCount =
            skeletonPose.bound() ? previewModel.boneCount() : 0U;
        for (std::size_t index = 0; index < previewBoneCount; ++index) {
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

    // A1：`drawEquipmentSlots` 已删——四个护甲槽加副手现在与别的槽走同一趟
    // `drawContainerSlots`。它此前是第三份"画一格槽位"的循环（另两份在
    // drawWorkContainer 与创造背包里），而三份各自决定要不要收提示框：护甲槽收，
    // 生存背包的 2x2 不收。26.1 没有这种区别。

    // I-2：提示框的行**组装**已整体搬进 ui/ItemTooltip（纯值、headless 可测），
    // 渲染器这边只剩"画盒子"。这个上下文是两者之间唯一的接线。
    //
    // advanced 在 vanilla 是 F3+H 的 `Options.advancedItemTooltips`；本项目还没
    // 有那个开关，暂以"F3 调试屏是否打开"充当，等真开关落地时只改这一处。
    [[nodiscard]] ui::TooltipContext tooltipContext() const {
        return ui::TooltipContext{debugOverlayOpen, &language};
    }

    // 颜色语义 → RGBA 的那张表。组装层只给 TooltipStyle，颜色值全在这里，
    // 资源包换主题时也只动这里。取值即 vanilla 的 ChatFormatting 原色。
    [[nodiscard]] static glm::vec4 tooltipLineColor(ui::TooltipStyle style) {
        switch (style) {
        case ui::TooltipStyle::NameCommon: return {1.0F, 1.0F, 1.0F, 1.0F};          // WHITE
        case ui::TooltipStyle::NameUncommon: return {1.0F, 1.0F, 0.333F, 1.0F};      // YELLOW
        case ui::TooltipStyle::NameRare: return {0.333F, 1.0F, 1.0F, 1.0F};          // AQUA
        case ui::TooltipStyle::NameEpic: return {1.0F, 0.333F, 1.0F, 1.0F};          // LIGHT_PURPLE
        case ui::TooltipStyle::Detail: return {0.667F, 0.667F, 0.667F, 1.0F};        // GRAY
        case ui::TooltipStyle::Curse: return {1.0F, 0.333F, 0.333F, 1.0F};           // RED
        case ui::TooltipStyle::AttributeBase: return {0.0F, 0.667F, 0.0F, 1.0F};     // DARK_GREEN
        case ui::TooltipStyle::AttributeBonus: return {0.333F, 0.333F, 1.0F, 1.0F};  // BLUE
        case ui::TooltipStyle::Advanced: return {0.333F, 0.333F, 0.333F, 1.0F};      // DARK_GRAY
        }
        return {1.0F, 1.0F, 1.0F, 1.0F};
    }

    // The one tooltip box every screen draws — 26.1 的 TooltipRenderUtil，
    // 每个界面都问它。Callers must draw it AFTER everything else on the screen —
    // a tooltip painted mid-pass gets covered by whatever is drawn next, which is
    // exactly the bug that made the enchanting bars' clues invisible on every bar
    // except the last one.（vanilla 不需要这条纪律：它把提示框推迟到
    // `extractDeferredElements` 的一个新 stratum 里画，天生在最上层。）
    //
    // 底衬是**两张九宫格精灵**而不是一块纯色矩形，与 vanilla 一致：
    // `tooltip/background`（0xF0100010 的填充）先画，`tooltip/frame`
    // （1px 竖直渐变边框 #5000FF → #28007F，alpha 0x50）叠在同一个矩形上。
    // 两张都从资源包读，于是换材质包就能换掉提示框的样子。
    //
    // 几何全部照抄 `GuiGraphics#tooltip` + `TooltipRenderUtil`，单位是未缩放的
    // GUI 像素：
    //   · 内容宽 = 最宽的一行；内容高 = 单行 8、多行 10n（`lines.size() == 1
    //     ? -2 : 0` 那句）；
    //   · 第一行之后多 2px——名称与其余行之间的那道缝；
    //   · 精灵画在内容矩形外扩 12（PADDING 3 + MARGIN 9），于是可见的填充正好
    //     是内容 +4，边框那 1px 落在内容 +3；
    //   · 定位是 DefaultTooltipPositioner：光标 +12/**-12**（不是 +12/+12），
    //     右边放不下就翻到光标左侧、至少留 4px，下边放不下就顶到
    //     `screenHeight - height - 3`。
    // 长行换行（170 GUI 像素）不是物品提示框的 vanilla 行为，是本项目沿用
    // `Tooltip.splitTooltip` 的那条上限，避免超长名字把框拉出屏幕。
    void drawTooltipBox(VkCommandBuffer commandBuffer, float scale,
                        const std::vector<ui::TooltipLine>& lines) const {
        if (lines.empty()) {
            return;
        }
        // 换行按未缩放的 GUI 像素量，于是 GUI 缩放改变时每行的断点不变。
        static constexpr float kMaxLineWidth = 170.0F;
        std::vector<ui::TooltipLine> visual;
        visual.reserve(lines.size());
        for (const auto& line : lines) {
            if (line.text.empty()) {
                visual.push_back(line);
                continue;
            }
            for (auto& piece : ui::wrapText(line.text, kMaxLineWidth,
                                            [this](std::string_view text) {
                                                return hudTextWidth(text, 1.0F);
                                            })) {
                visual.push_back({std::move(piece), line.style});
            }
        }
        if (visual.empty()) {
            return;
        }
        // 以下一律是未缩放的 GUI 像素，最后一步才乘 scale——精灵的边框宽度也按
        // 这套单位度量，两边混用会让 1px 的边框在不同 GUI 缩放下变粗变细。
        float contentWidth = 0.0F;
        for (const auto& line : visual) {
            contentWidth = std::max(contentWidth, hudTextWidth(line.text, 1.0F));
        }
        const float contentHeight = ui::tooltipContentHeight(visual.size());
        const auto cursor = currentFramebufferCursor();
        // 几何本身在 ui/TooltipLayout 里，纯值且有单测；这里只负责换算单位、
        // 取光标和屏幕，然后把结果画出来。
        const auto origin = ui::positionTooltip(
            static_cast<float>(swapchainExtent.width) / scale,
            static_cast<float>(swapchainExtent.height) / scale, cursor.x / scale, cursor.y / scale,
            contentWidth, contentHeight);
        const ui::UiRect content{origin.x * scale, origin.y * scale, contentWidth * scale,
                                 contentHeight * scale};
        const ui::UiRect unscaledBackdrop =
            ui::tooltipBackdrop(ui::UiRect{origin.x, origin.y, contentWidth, contentHeight});
        const ui::UiRect backdrop{unscaledBackdrop.x * scale, unscaledBackdrop.y * scale,
                                  unscaledBackdrop.width * scale, unscaledBackdrop.height * scale};
        drawScaledGuiSprite(commandBuffer, backdrop, kTooltipGuiLayer,
                            guiWidgetSprite(guiWidgetSprites, GuiWidgetSprite::TooltipBackground),
                            scale);
        drawScaledGuiSprite(commandBuffer, backdrop, kTooltipGuiLayer,
                            guiWidgetSprite(guiWidgetSprites, GuiWidgetSprite::TooltipFrame),
                            scale);
        for (std::size_t line = 0; line < visual.size(); ++line) {
            if (visual[line].text.empty()) {
                continue;
            }
            drawHudText(commandBuffer, visual[line].text, content.x,
                        content.y + ui::tooltipLineOffset(line) * scale, scale,
                        tooltipLineColor(visual[line].style));
        }
    }

    // ENCH-3: AnvilScreen, transcribed. GUI spec §10's anvil row: the three
    // slots at (27,47)/(76,47)/(134,47) on the same 176x166 panel, the level
    // cost centred at the bottom, and — when the price is at or past the wall —
    // vanilla's error marker over the output slot instead of a result.
    //
    // I-3 landed the custom-name storage, so the rename box is live. It still
    // goes dead when the left slot is empty, which is vanilla's own rule
    // (`slotChanged` -> `setEditable(!itemStack.isEmpty())`), and the field art
    // switches to the greyed variant with it.
    // A1：它只画这一屏**独有**的东西（两行标题、名字框、错误标记、价格），槽位归
    // `drawContainerSlots` 那一趟统一画。
    void drawAnvilScreen(VkCommandBuffer commandBuffer, const ui::HudLayout& layout,
                         const ui::UiRect& panel) const {
        const auto& snap = clientMirror.world();
        const float scale = layout.scale();
        drawHudText(commandBuffer, translated("container.repair", "Repair & Name"),
                    panel.x + 8.0F * scale, panel.y + 6.0F * scale, scale,
                    {0.25F, 0.25F, 0.25F, 1.0F}, false);
        drawHudText(commandBuffer, translated("container.inventory", "Inventory"),
                    panel.x + 8.0F * scale, panel.y + 73.0F * scale, scale,
                    {0.25F, 0.25F, 0.25F, 1.0F}, false);
        // AnvilScreen#extractBackground blits the name field UNCONDITIONALLY —
        // it is part of the background, not an optional decoration, and the base
        // anvil.png paints that whole 110x16 region pure red (255,0,0) as a
        // "you must cover me" marker. Skipping the blit is why the screen showed
        // a red bar where the name box belongs.
        //
        // Which variant, exactly as vanilla picks it: lit while there is
        // something in the left slot to rename, greyed when there is not.
        const bool renameEnabled = !snap.anvilLeft.empty();
        drawGuiSprite(commandBuffer,
                      {panel.x + 59.0F * scale, panel.y + 20.0F * scale, 110.0F * scale,
                       16.0F * scale},
                      kAnvilGuiLayer,
                      {0.0F,
                       static_cast<float>(kAnvilTextFieldSpriteY + (renameEnabled ? 0 : 17)),
                       110.0F, 16.0F});
        // GUI spec §10 as corrected in §13.3: the field art sits at (59,20)
        // 110x16 and the EditBox itself at (62,24) 103x12 — AnvilScreen.java:37.
        // The art is already blitted above, so the field itself is borderless.
        TextFieldStyle anvilNameStyle;
        anvilNameStyle.bordered = false;
        anvilNameStyle.focused = renameEnabled;
        anvilNameStyle.shadow = false;
        drawTextField(commandBuffer,
                      {panel.x + 62.0F * scale, panel.y + 24.0F * scale, 103.0F * scale,
                       12.0F * scale},
                      scale, anvilName_,
                      renameEnabled ? ui::kAnvilNameFieldRules : ui::kAnvilNameFieldDisabled,
                      anvilNameStyle);
        // ItemCombinerScreen#extractErrorIcon: shown whenever there is input but
        // no result — including the cost-0 cases (two items that cannot be
        // combined at all), which is why the condition is about the SLOTS and
        // not about the price.
        if ((!snap.anvilLeft.empty() || !snap.anvilRight.empty()) && snap.anvilResult.empty()) {
            drawGuiSprite(commandBuffer,
                          {panel.x + 99.0F * scale, panel.y + 45.0F * scale, 28.0F * scale,
                           21.0F * scale},
                          kAnvilGuiLayer,
                          {static_cast<float>(kAnvilErrorSpriteX),
                           static_cast<float>(kAnvilErrorSpriteY), 28.0F, 21.0F});
        }

        if (snap.anvilCost <= 0) {
            return;
        }
        // AnvilScreen#renderLabels' three branches, in order. The first is the
        // one that was missing: past the wall the line is "Too Expensive!", not
        // a price — a player looking at a cost of 277 with no other feedback has
        // no way to know the anvil has refused rather than merely become dear.
        const bool infiniteMaterials = uiFrameData_.gameMode == gameplay::GameMode::Creative;
        constexpr glm::vec4 kAffordable{0.502F, 1.0F, 0.125F, 1.0F};   // -8323296
        constexpr glm::vec4 kRefused{1.0F, 0.376F, 0.376F, 1.0F};      // -40864
        std::string label;
        glm::vec4 colour = kAffordable;
        if (snap.anvilCost >= gameplay::kAnvilMaximumCost && !infiniteMaterials) {
            label = translated("container.repair.expensive", "Too Expensive!");
            colour = kRefused;
        } else if (snap.anvilResult.empty()) {
            return; // an operation with no result and no wall: no line at all
        } else {
            label = formatTemplate(translated("container.repair.cost", "Enchantment Cost: %s"),
                                   std::to_string(snap.anvilCost));
            if (!infiniteMaterials && uiFrameData_.experienceLevel < snap.anvilCost) {
                colour = kRefused;
            }
        }
        // The translucent strip vanilla fills behind the line so it stays
        // readable over the inventory art.
        const float labelWidth = hudTextWidth(label, scale);
        const float labelX = panel.x + (176.0F - 8.0F) * scale - labelWidth - 2.0F * scale;
        drawHudQuad(commandBuffer,
                    {labelX - 2.0F * scale, panel.y + 67.0F * scale,
                     panel.x + (176.0F - 8.0F) * scale - (labelX - 2.0F * scale),
                     12.0F * scale},
                    {0.0F, 0.0F, 0.0F, 0.31F});
        drawHudText(commandBuffer, label, labelX, panel.y + 69.0F * scale, scale, colour, false);
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
    // Returns the option whose clue tooltip the caller must draw last, if the
    // cursor is over one — the tooltip cannot be drawn from inside the bar loop
    // (see drawTooltipBox).
    // A1：同铁砧——只画这一屏独有的东西（两行标题、三条选项条），槽位统一画。
    [[nodiscard]] std::optional<std::size_t> drawEnchantingScreen(
        VkCommandBuffer commandBuffer, const ui::HudLayout& layout,
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
        const bool infiniteMaterials = uiFrameData_.gameMode == gameplay::GameMode::Creative;
        const int lapisCount = static_cast<int>(snap.enchantingLapis.count);
        // One RandomSource for the whole screen, seeded from the enchantment
        // seed and advanced only by the bars that are live — EnchantmentNames'
        // initSeed + the `cost == 0` early-out, in that exact order, so the
        // three phrases match vanilla's for the same seed.
        world::gen::JavaRandom nameRandom(static_cast<std::uint64_t>(snap.enchantingSeed));
        std::optional<std::size_t> hoveredClue;
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
                // Recorded, NOT drawn: the two bars after this one are still to
                // be painted, and the tooltip sits 12px down-right of the cursor
                // — squarely inside the next bar's rectangle. Drawing it here is
                // what made bar 0's and bar 1's clues invisible while bar 2's,
                // drawn last, survived.
                hoveredClue = option;
            }
        }
        return hoveredClue;
    }

    // The hover tooltip over a live option bar. Kept out of the loop above
    // because it draws on top of every bar, not inside one.
    void drawEnchantingClueTooltip(VkCommandBuffer commandBuffer, float scale, std::size_t option,
                                   std::int32_t cost, int lapisCost,
                                   bool infiniteMaterials) const {
        const auto& snap = clientMirror.world();
        const auto clueId =
            static_cast<gameplay::EnchantmentId>(snap.enchantingClueIds[option]);
        // 附魔名走展示层同一份 Enchantment#getFullname，线索行与提示框的附魔行
        // 因此不可能给同一条附魔两个写法。
        const std::string clueName = ui::enchantmentLabel(
            clueId, static_cast<int>(snap.enchantingClueLevels[option]), tooltipContext());
        std::vector<ui::TooltipLine> lines;
        lines.push_back({formatTemplate(translated("container.enchant.clue", "%s . . . ?"),
                                        clueName),
                         ui::TooltipStyle::NameCommon});
        if (!infiniteMaterials) {
            if (uiFrameData_.experienceLevel < cost) {
                lines.push_back(
                    {formatTemplate(translated("container.enchant.level.requirement",
                                               "Level Requirement: %s"),
                                    std::to_string(cost)),
                     ui::TooltipStyle::Detail});
            } else {
                lines.push_back(
                    {lapisCost == 1
                         ? translated("container.enchant.lapis.one", "1 Lapis Lazuli")
                         : formatTemplate(translated("container.enchant.lapis.many",
                                                     "%s Lapis Lazuli"),
                                          std::to_string(lapisCost)),
                     ui::TooltipStyle::Detail});
                lines.push_back(
                    {lapisCost == 1
                         ? translated("container.enchant.level.one", "1 Enchantment Level")
                         : formatTemplate(translated("container.enchant.level.many",
                                                     "%s Enchantment Levels"),
                                          std::to_string(lapisCost)),
                     ui::TooltipStyle::Detail});
            }
        }
        drawTooltipBox(commandBuffer, scale, lines);
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

    // A1：工作台 / 熔炉 / 箱子 / 附魔台 / 铁砧这五屏。
    //
    // ★ 从前这里是一条 `if (chestScreen) … else if (CraftingTable) … else if …` 的
    //   五段链，每一段里既画这一屏独有的东西（标题、进度条、选项条），又**自己画一遍
    //   槽位**；链尾再补一段 36 格玩家背包。现在只剩前者：槽位一律由
    //   `drawContainerSlots` 遍历容器页画，而"这一屏画什么铭牌"是一处不带 `default`
    //   的分派——加一块容器屏时编译器点名，而不是让它掉进最后那个 else（从前那个
    //   else 是**熔炉**，也就是说加一屏忘了写分支，画出来的是熔炉的火焰与进度条）。
    void drawWorkContainer(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet,
                           const ui::HudLayout& layout) const {
        // 底衬由 drawHud 一处按档位表画（容器类走 Transparent 那一档），这里不再自己铺
        const bool trading = containerKind() == ui::ContainerPageKind::Trading;
        const auto panel = trading ? layout.tradingPanel() : layout.inventoryPanel();
        const float panelLayer = containerPanelLayer(containerKind());
        if (trading) {
            // MERCH-1：交易屏的面板是 276x166，塞不进 256 宽的图集层，所以按
            // `tradingPanelPieces()` 拆成三块画。拆法与烘焙侧共用那一个纯函数
            // （那里有 static_assert 钉住"不重叠、不留缝、放得下"）。
            const float scale = layout.scale();
            for (const auto& piece : tradingPanelPieces()) {
                drawGuiSprite(commandBuffer,
                              {panel.x + piece.offsetX * scale, panel.y + piece.offsetY * scale,
                               piece.source.width * scale, piece.source.height * scale},
                              panelLayer, piece.source);
            }
        } else {
            drawGuiSprite(commandBuffer, panel, panelLayer, {0.0F, 0.0F, 176.0F, 166.0F});
        }
        const auto hoveredClue = drawWorkContainerChrome(commandBuffer, layout, panel);
        const auto hoveredStack = drawContainerSlots(commandBuffer, containerPage(layout), layout);
        drawContainerCursorLayer(commandBuffer, layout, hoveredStack, hoveredClue);
        static_cast<void>(descriptorSet);
    }

    // 这一屏的“铭牌”：标题文字、熔炉的两条进度、附魔的三条选项条、铁砧的名字框与价格。
    // 返回附魔线索的那一条（若光标停在某条选项条上），它要画在所有槽位之上。
    [[nodiscard]] std::optional<std::size_t> drawWorkContainerChrome(
        VkCommandBuffer commandBuffer, const ui::HudLayout& layout,
        const ui::UiRect& panel) const {
        const float scale = layout.scale();
        const auto title = [&](std::string_view key, std::string_view fallback) {
            drawHudText(commandBuffer, translated(key, fallback), panel.x + 8.0F * scale,
                        panel.y + 6.0F * scale, scale, {0.25F, 0.25F, 0.25F, 1.0F}, false);
            drawHudText(commandBuffer, translated("container.inventory", "Inventory"),
                        panel.x + 8.0F * scale, panel.y + 73.0F * scale, scale,
                        {0.25F, 0.25F, 0.25F, 1.0F}, false);
        };
        switch (containerKind()) {
        case ui::ContainerPageKind::Chest:
            title("container.chest", "Chest");
            return std::nullopt;
        case ui::ContainerPageKind::CraftingTable:
            // UI-8 / D29：26.1 `AbstractContainerScreen.extractLabels`（:218-221）对
            // **每一块**容器屏都画屏名与 "Inventory" 两行，`CraftingScreen` 与
            // `AbstractFurnaceScreen` 都没有覆写它——本作此前这两屏一行都没画。
            // 屏名来自 `CraftingTableBlock.CONTAINER_TITLE`（container.crafting）。
            title("container.crafting", "Crafting");
            return std::nullopt;
        case ui::ContainerPageKind::Trading:
            // AR-M6 —— ★ **这里是交易界面的接入点，后端已经全部就绪，绘制未做。**
            //
            // 现在只画屏名。要画的东西全在 `clientMirror.world()` 的 trade* 字段里：
            // 三个格子（tradePaymentA/B、tradeResult）、逐行的 tradeWantsA/WantsB/
            // Gives + Levels/Uses/MaxUses/Locked/OutOfStock、tradeOfferCount、
            // tradeSelectedOffer、以及等级条的 tradeVillagerLevel/tradeXpInLevel/
            // tradeXpForNextLevel。几何锚点在 `HudLayout::tradingPanel/
            // tradingPaymentSlot/tradingResultSlot/tradingOffer`。
            // 契约见 docs/content-dev/AR-content-realization/
            // AR-M6-trading-backend-interface.md。
            title("merchant.trades", "Trades");
            return std::nullopt;
        case ui::ContainerPageKind::EnchantingTable:
            return drawEnchantingScreen(commandBuffer, layout, panel);
        case ui::ContainerPageKind::Anvil:
            drawAnvilScreen(commandBuffer, layout, panel);
            return std::nullopt;
        case ui::ContainerPageKind::Furnace: {
            // D29 同上。屏名来自 `FurnaceBlockEntity.DEFAULT_NAME`（container.furnace）。
            title("container.furnace", "Furnace");
            // 熔炉界面按容器显示快照绘制，这里不读方块实体的位置
            const auto& worldSnap = clientMirror.world();
            const float fuel = std::clamp(worldSnap.furnaceFuelProgress, 0.0F, 1.0F);
            if (fuel > 0.0F) {
                const float height = std::ceil(13.0F * fuel);
                drawGuiSprite(commandBuffer,
                              {panel.x + 57.0F * scale, panel.y + (36.0F + 13.0F - height) * scale,
                               14.0F * scale, height * scale},
                              8.0F, {176.0F, 13.0F - height, 14.0F, height});
            }
            const float progress = std::clamp(worldSnap.furnaceCookProgress, 0.0F, 1.0F);
            if (progress > 0.0F) {
                const float width = std::ceil(24.0F * progress);
                drawGuiSprite(commandBuffer,
                              {panel.x + 79.0F * scale, panel.y + 34.0F * scale, width * scale,
                               17.0F * scale},
                              8.0F, {176.0F, 14.0F, width, 17.0F});
            }
            return std::nullopt;
        }
        case ui::ContainerPageKind::SurvivalInventory:
        case ui::ContainerPageKind::CreativeInventoryTab:
        case ui::ContainerPageKind::CreativeCatalogTab:
        case ui::ContainerPageKind::Count:
            // 这三屏不走这条路（drawContainerLayer 分派到别处），哨兵不是一屏。
            return std::nullopt;
        }
        return std::nullopt;
    }


    // A1：创造背包的两个页签。
    //
    // ★ 槽位（背包页签的 36 格与护甲/副手、内容页签的 45 格目录与 9 格快捷栏）一律
    //   交给 `drawContainerSlots`。留在这里的只有这一屏独有的东西：页签行、面板、
    //   玩家预览、页签图标、删除框的高亮、目录标题与滚动条。
    //
    // ★ **面板画在未选中页签之后、选中页签之前**——页签是从面板后面探出来的，
    //   这个夹心顺序是它看起来"选中的那一个连着面板"的全部原因，不能重排。
    void drawCreativeInventory(VkCommandBuffer commandBuffer, const ui::HudLayout& layout) const {
        const auto cursor = currentFramebufferCursor();
        const float scale = layout.scale();
        const auto panel = layout.creativePanel();
        const bool inventoryTab = containerKind() == ui::ContainerPageKind::CreativeInventoryTab;

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
        drawGuiSprite(commandBuffer, panel, containerPanelLayer(containerKind()),
                      {0.0F, 0.0F, 195.0F, 136.0F});
        if (inventoryTab) {
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

        if (inventoryTab) {
            const auto deleteSlot = layout.creativeDeleteSlot();
            if (deleteSlot.contains(cursor.x, cursor.y)) {
                drawHudQuad(commandBuffer, deleteSlot, {1.0F, 0.25F, 0.25F, 0.34F});
            }
        } else {
            // 26.1 的十个内容页签，顺序同 CreativeTab（下标 0..9；背包页签走上面那一支）
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
        }

        const auto hoveredStack = drawContainerSlots(commandBuffer, containerPage(layout), layout);
        drawContainerCursorLayer(commandBuffer, layout, hoveredStack, std::nullopt);
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
                             ui::kChatLineHeight * scale},
                            {0.0F, 0.0F, 0.0F, 0.55F});
                drawHudText(commandBuffer, *line, 4.0F * scale, messageY + scale, scale, color,
                            false);
                messageY -= ui::kChatLineHeight * scale;
            }
        }
        if (!chatOpen) {
            return;
        }
        // vanilla 的聊天输入框占满屏幕宽度，其编辑框宽度为 windowWidth - 8
        // 所以深色底衬始终从左缘拉到右缘，而不是紧贴已输入的文字
        const ui::UiRect input = layout.chatInput();
        TextFieldStyle chatStyle;
        chatStyle.bordered = false;
        chatStyle.background = {0.0F, 0.0F, 0.0F, 0.72F};
        chatStyle.textColor = {1.0F, 1.0F, 1.0F, 1.0F};
        chatStyle.shadow = false;
        drawTextField(commandBuffer, input, scale, chatInput, ui::kChatFieldRules, chatStyle);
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
                drawHudSlot(commandBuffer, layout.hotbarSlot(index),
                            clientMirror.world().inventorySlots[index]);
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
                // Gui#tick compares the highlighted stack with ItemStack.matches,
                // which includes the components — so a held item that GAINS an
                // enchantment re-shows its name. sameItem() ignores enchantments
                // and damage, so it never noticed: enchanting the tool in your
                // hand changed nothing on screen. Full equality restores the
                // vanilla cue (and re-shows on a durability change, as vanilla
                // does).
                const bool selectionChanged =
                    selectedNameSlot_ == static_cast<std::size_t>(-1) ||
                    selectedSlot != selectedNameSlot_ ||
                    !(selectedStack == selectedNameStack_);
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
                    // I-2：名字与稀有度色都取展示层的名称行，不再自己解析一遍
                    // ——弹出的名字与提示框的第一行从此是同一个答案。
                    const ui::TooltipLine name = ui::itemNameLine(selectedStack, tooltipContext());
                    glm::vec4 color = tooltipLineColor(name.style);
                    color.a = alpha;
                    drawHudText(commandBuffer, name.text,
                                (static_cast<float>(swapchainExtent.width) -
                                 hudTextWidth(name.text, textScale)) *
                                    0.5F,
                                layout.hotbarBackground().y -
                                    (uiFrameData_.gameMode == gameplay::GameMode::Survival
                                         ? 30.0F
                                         : 12.0F) *
                                        textScale,
                                textScale, color);
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
                                            menuSystem.guiScaleSetting, menuSystem.forceUnicodeFont};
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

    // A1：这一屏的容器页。装配器在 ui/ContainerPage，几何与身份都从那里来。
    [[nodiscard]] ui::Page containerPage(const ui::HudLayout& layout) const {
        ui::Page page;
        ui::buildContainerPageInto(page, screenContext(), layout);
        return page;
    }

    // UI-8 / D30：光标下那一格的高亮。26.1 `AbstractContainerScreen:184/190`：
    // 两张 24x24 的九宫格精灵，画在槽位的 (x-4, y-4)——比 16x16 的格子大一圈。
    void drawSlotHighlight(VkCommandBuffer commandBuffer, const ui::UiRect& slot,
                           GuiWidgetSprite sprite, float scale) const {
        drawScaledGuiSprite(commandBuffer, ui::slotHighlightRect(slot, scale), 0.0F,
                            guiWidgetSprite(guiWidgetSprites, sprite), scale, glm::vec4{1.0F});
    }

    // A1：**容器屏的槽位一律走这一趟**——遍历容器页里的 `Slot` 控件。
    //
    // ★ 它取代的是五段各写各的循环：`drawWorkContainer` 的容器槽与 36 格、
    //   生存背包的 2x2 与 36 格、`drawEquipmentSlots`、创造背包两个页签各一段。
    //   五段里"选中框画在哪一格""要不要悬停高亮""提示框收不收这一格"三条规则各答
    //   各的——于是生存背包的 2x2 合成格既不高亮也不出提示框，而工作台的 3x3 两样都有。
    //   26.1 没有这种区别：`AbstractContainerScreen` 整屏只有**一个** `hoveredSlot`
    //   （`getHoveredSlot` 遍历全部槽位），高亮与提示框都挂在它上面（:183-196）。
    //
    // 返回光标下那一格的物品堆（若非空），交给提示框那一层。
    [[nodiscard]] std::optional<gameplay::ItemStack> drawContainerSlots(
        VkCommandBuffer commandBuffer, const ui::Page& page, const ui::HudLayout& layout) const {
        const auto cursor = currentFramebufferCursor();
        const auto& snapshot = clientMirror.world();
        // 创造目录那 45 格的内容不在世界快照里（无限货架），从目录清单按当前滚动行取。
        const auto catalog = activeCreativeCatalog();
        const std::size_t firstCatalogIndex = menuSystem.creativeScrollRow * 9U;
        // ★ 整屏只有**一个** hoveredSlot（26.1 `getHoveredSlot`），高亮挂在它身上。
        //   后画即在上，所以取最后一个命中——与 `ui::hitTest` 同一条规则。
        const ui::Widget* hoveredSlot = nullptr;
        for (const ui::Widget& widget : page) {
            if (widget.kind == ui::WidgetKind::Slot &&
                widget.rect.contains(cursor.x, cursor.y)) {
                hoveredSlot = &widget;
            }
        }
        const float scale = layout.scale();
        if (hoveredSlot != nullptr) {
            drawSlotHighlight(commandBuffer, hoveredSlot->rect, GuiWidgetSprite::SlotHighlightBack,
                              scale);
        }
        std::optional<gameplay::ItemStack> hoveredStack;
        for (const ui::Widget& widget : page) {
            if (widget.kind != ui::WidgetKind::Slot) {
                continue;
            }
            gameplay::ItemStack stack;
            if (widget.slotKind == gameplay::SlotKind::CreativeCatalog) {
                const std::size_t catalogIndex = firstCatalogIndex + widget.slotIndex;
                if (catalogIndex < catalog.size()) {
                    stack = catalog[catalogIndex];
                }
            } else {
                stack = gameplay::snapshotSlotStack(snapshot, widget.slotKind, widget.slotIndex);
            }
            if (&widget == hoveredSlot && !stack.empty()) {
                hoveredStack = stack;
            }
            drawHudSlot(commandBuffer, widget.rect, stack);
        }
        // ★ front 那一张画在**所有**槽位内容之上——这是本作此前画不出来的那一半。
        if (hoveredSlot != nullptr) {
            drawSlotHighlight(commandBuffer, hoveredSlot->rect,
                              GuiWidgetSprite::SlotHighlightFront, scale);
        }
        return hoveredStack;
    }

    // A1：槽位之后的三件事，收在一处：拖拽预览 → 提示框 → 光标上的物品堆。
    //
    // ★ **手上拖着东西时不画提示框**（26.1 `AbstractContainerScreen`：那一支的条件是
    //   `getCarried().isEmpty()`）。从前只有 `drawWorkContainer` 这么做，生存背包与
    //   创造背包是"两样都画"——提示框压在被拖着的那一格物品下面。
    void drawContainerCursorLayer(VkCommandBuffer commandBuffer, const ui::HudLayout& layout,
                                  const std::optional<gameplay::ItemStack>& hoveredStack,
                                  std::optional<std::size_t> hoveredClue) const {
        // 拖拽过程中在每个划过的槽位预览松手后的落位，画在槽位之上、光标之下
        drawDragPreview(commandBuffer, layout);
        const auto& cursorStack = clientMirror.world().cursorStack;
        if (cursorStack.empty()) {
            if (hoveredStack.has_value()) {
                drawTooltipBox(commandBuffer, layout.scale(),
                               ui::itemTooltipLines(*hoveredStack, tooltipContext()));
            } else if (hoveredClue.has_value()) {
                drawEnchantingClueTooltip(
                    commandBuffer, layout.scale(), *hoveredClue,
                    clientMirror.world().enchantingRequiredLevels[*hoveredClue],
                    static_cast<int>(*hoveredClue) + 1,
                    uiFrameData_.gameMode == gameplay::GameMode::Creative);
            }
            return;
        }
        // 光标上的那一堆。★ 画法与一格槽位完全相同（图标 + 耐久条 + 数量），
        // 所以走 drawHudSlot 那一支，而不是第三份手抄的"图标加数字"。
        const auto cursor = currentFramebufferCursor();
        const float size = 16.0F * layout.scale();
        drawHudSlot(commandBuffer,
                    {cursor.x - size * 0.5F, cursor.y - size * 0.5F, size, size}, cursorStack);
    }

    // A1：生存模式的背包屏。
    //
    // ★ 它此前**内联在 `drawHud` 里**（73 行，夹在调试叠加层与聊天之间）。那意味着
    //   它既不能被单独调用，也没有名字可以出现在任何一张分派表里——"哪一屏走哪个绘制
    //   函数"这件事因此在容器这一层根本无从谈起。抽出来是把它接进
    //   `ContainerPageKind` 分派的前提。
    void drawSurvivalInventory(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet,
                               const ui::HudLayout& layout) const {
        const auto panel = layout.inventoryPanel();
        drawGuiSprite(commandBuffer, panel, containerPanelLayer(containerKind()),
                      {0.0F, 0.0F, 176.0F, 166.0F});
        drawPlayerPreview(commandBuffer, descriptorSet, layout);
        const auto hoveredStack = drawContainerSlots(commandBuffer, containerPage(layout), layout);
        drawContainerCursorLayer(commandBuffer, layout, hoveredStack, std::nullopt);
    }

    // A1：这一屏是哪一种容器界面。身份只有一处（`ui::containerPageKind`），
    // 绘制侧只是把自己的三个状态喂给它。
    [[nodiscard]] ui::ContainerPageKind containerKind() const {
        return ui::containerPageKind(containerScreen, uiFrameData_.gameMode,
                                     menuSystem.creativeTab == ui::CreativeTab::Inventory);
    }

    // A1：容器这一层的入口。
    //
    // ★ 从前这里是**三条并列的 `if (inventoryOpen && …)`**，每条各自重复一遍
    //   "是不是背包屏"与"是不是创造"的判断（`drawHud` 里那三条）。加一块容器屏要
    //   在这里再加一条 if，而漏加的症状是"打开容器却什么都没画"——不是编译错误。
    //   现在是一处不带 `default` 的分派，加一种 ContainerPageKind 时编译器点名。
    void drawContainerLayer(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet,
                            const ui::HudLayout& layout) const {
        if (!inventoryOpen) {
            return;
        }
        switch (containerKind()) {
        case ui::ContainerPageKind::SurvivalInventory:
            drawSurvivalInventory(commandBuffer, descriptorSet, layout);
            return;
        case ui::ContainerPageKind::CreativeInventoryTab:
        case ui::ContainerPageKind::CreativeCatalogTab:
            drawCreativeInventory(commandBuffer, layout);
            return;
        case ui::ContainerPageKind::CraftingTable:
        case ui::ContainerPageKind::Furnace:
        case ui::ContainerPageKind::Chest:
        case ui::ContainerPageKind::EnchantingTable:
        case ui::ContainerPageKind::Anvil:
        // AR-M6：交易屏走同一条工作容器路径（三个格子 + 一排可点的行），所以槽位、
        // 悬停提示与光标层一到位就已经能用。缺的只有它自己的面板底图与行内绘制，
        // 见 drawWorkContainerChrome 里 Trading 分支的接入说明。
        case ui::ContainerPageKind::Trading:
            drawWorkContainer(commandBuffer, descriptorSet, layout);
            return;
        case ui::ContainerPageKind::Count:
            return;   // 哨兵，不是一屏
        }
    }

    void drawHud(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet) const {
        // 测试场景是方块预览的取景台，它要的是**一张只有方块的图**，所以那条路径不画界面。
        // UI-6-0 之后同一个夹具也给界面截图当世界背景用——那时界面正是要拍的东西。
        if (testScene.has_value() && !uiCaptureActive)
            return;
        // UI-13：这一帧要当存档缩略图，照 26.1 那样只留世界。见 bindings 里的注释。
        if (worldIconCapturePending)
            return;
        const ui::HudLayout layout{static_cast<float>(swapchainExtent.width),
                                   static_cast<float>(swapchainExtent.height),
                                   menuSystem.guiScaleSetting, menuSystem.forceUnicodeFont};
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipelineLayout,
                                0, 1, &descriptorSet, 0, nullptr);

        // UI-5：背景是**一处**决定的——档位表在 ui/ScreenBackground.hpp，绘制在这里。
        // 从前它散在三个分支里各调一次 drawTitleCarousel，加上容器界面各自铺的灰渐变，
        // 于是"哪一屏用哪一档"这件事没有单一来源，暂停菜单那一档干脆整个漏了。
        //
        // 游戏内 HUD（没有任何界面打开）不画背景：档位表只在 26.1 会调用
        // Screen.extractBackground 的时候有意义。
        const auto page = menuSystem.pageStack.current();
        if (screenOpen()) {
            drawScreenBackground(commandBuffer, descriptorSet, currentBackgroundKind(),
                                 layout.scale());
        }
        // 走哪个绘制函数由 ui::pageDrawKind 那张表决定（不带 default 的 switch，
        // 加一页会被 -Wswitch 点名）。从前这里是两串 `page == A || page == B || …`：
        // 加一页忘了加进去，它会掉进后面"游戏内"的分支——症状是打开新页面却看到
        // 游戏画面，而不是编译错误。
        switch (ui::pageDrawKind(page)) {
        case ui::PageDrawKind::Frontend:
            drawFrontend(commandBuffer, layout);
            return;
        case ui::PageDrawKind::Language:
            drawLanguageScreen(commandBuffer, layout);
            return;
        case ui::PageDrawKind::Settings:
            drawPauseMenu(commandBuffer, layout);
            return;
        case ui::PageDrawKind::InGame:
            break;
        }

        if (!worldReady) {
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

        drawContainerLayer(commandBuffer, descriptorSet, layout);

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
    const assets::ResourcePackLibrary& packLibrary;
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
    VkPipeline& hudBlockIconPipeline;
    VkPipelineLayout& hudPipelineLayout;
    VkPipeline& vignettePipeline;
    VkPipeline& crosshairPipeline;
    VkPipeline& panoramaPipeline;
    VkPipelineLayout& panoramaPipelineLayout;
    VkPipeline& gradientPipeline;
    VkPipelineLayout& gradientPipelineLayout;
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
    ui::TextFieldState& chatInput;
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
    // UI-2：标题美术在 binding 6 那张数组里的归一化子矩形，由 TextureManager 填
    const TitleArtUv& titleArtUv;
    // UI-2：截图通道钉死的光标位置；空表示照常读 GLFW
    const std::optional<ui::UiPoint>& pinnedCursor;
    const bool& uiCaptureActive;
    const bool& worldIconCapturePending;
    bool& paused;
    double& uiTimeSeconds;

    // ---- 与世界渲染/逐帧状态的耦合（绑到 Impl 的 lambda 上）----
    std::function<bool()> cameraSubmergedInWater;
    std::function<ui::MenuBuildContext::KeyBindRowLabels(input::InputAction)> keyBindLabels;
    std::function<void(VkCommandBuffer, VkDescriptorSet)> drawHeldItem;
    std::function<VkDescriptorSet()> currentFrameDescriptorSet;
    std::function<gameplay::ScreenContext()> screenContext;
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
    // UI-1: the anvil's rename field. It goes through the one text field layer
    // like every other typeable place, but its rules keep editable == false and
    // its value stays empty — an ItemStack has nowhere to store a custom name in
    // this build, and a box that accepts a name the game then throws away is
    // worse than a greyed-out one. The state lives here rather than being
    // conjured at the draw site so the day that storage lands has one place to
    // change.
    ui::TextFieldState anvilName_;

  public:
    // I-3: the renderer drives this field (keys and characters) and the anvil
    // screen draws it, so it is the one piece of text-field state the HUD hands
    // out rather than owning privately.
    // `ItemStack#getHoverName`: the custom name if the stack has one, otherwise
    // its ordinary translated name. The tooltip's name line already answers
    // exactly this (I-2 made it the single source), so this is a thin read of
    // that rather than a second answer.
    [[nodiscard]] std::string itemHoverName(const gameplay::ItemStack& stack) const {
        if (stack.empty()) {
            return {};
        }
        return ui::itemNameLine(stack, tooltipContext()).text;
    }

    [[nodiscard]] ui::TextFieldState& anvilName() { return anvilName_; }
    [[nodiscard]] const ui::TextFieldState& anvilName() const { return anvilName_; }

  private:
    mutable ui::MenuBuildContext drawContext_;
    ui::MenuCallbacks drawCallbacks_;
    float vignetteDarkness_ = 1.0F;
    mutable std::size_t selectedNameSlot_ = static_cast<std::size_t>(-1);
    mutable gameplay::ItemStack selectedNameStack_;
    mutable double selectedNameShownAt_ = -1.0;
};

} // namespace mc::render
