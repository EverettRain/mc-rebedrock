#pragma once

// 渲染器内核（VulkanRenderer.cpp）与 HUD 绘制子系统（HudRenderer.hpp）共用的 HUD/前端类型与常量
// 放在 mc::render 而不是某个 .cpp 的匿名命名空间里，两边才能指同一份定义

#include "gameplay/ScreenHandler.hpp"
#include "render/BlockOutlineGeometry.hpp"
#include "ui/ContainerPage.hpp"
#include "ui/HudLayout.hpp"
#include "ui/ScreenBackground.hpp"
#include "world/ItemModel.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace mc::render {

// GUI 图集每一层的边长（像素）。createGuiTexture() 运行期校验所有层同尺寸，
// 绘制侧按这个数把图集像素换算成 UV。
inline constexpr float kGuiAtlasSize = 256.0F;

// guiTextures 数组的层号，与 createGuiTexture() 保持同步
// 11 是 misc/vignette.png，12 是烘焙好的 Screen.renderBackground 暗角渐变
// 9 是 gui/menu_background.png（26.1 的二级菜单遮罩，按 32px 平铺）
inline constexpr float kMenuBackgroundGuiLayer = 9.0F;
inline constexpr float kVignetteGuiLayer = 11.0F;
// UI-5：12 从前是烘好的 Screen.renderBackground 灰渐变。那条渐变现在由渐变管线画
// （GradientPush / ui::kTransparentBackgroundStops），烘图没有消费者了，这一格
// 让给 gui/inworld_menu_background.png——有世界时铺的是它，不是 menu_background。
// 就地替换而不是删格：层号是写死的常量，删一格会把后面每一层都推错一位。
inline constexpr float kInworldMenuBackgroundGuiLayer = 12.0F;
inline constexpr float kMenuListBackgroundGuiLayer = 13.0F;
// ENCH-2: gui/container/enchanting_table.png, with the level numerals and the
// three option-bar states packed into the space its 176x166 panel leaves. The
// pack positions below are shared by the baker (TextureManager::createGuiTexture)
// and the reader (HudRenderer's enchanting screen) so neither can drift.
inline constexpr float kEnchantingGuiLayer = 14.0F;
// The 3x2 grid of 16x16 level numerals: enabled on the first row, disabled on
// the second, to the right of the panel.
inline constexpr int kEnchantingLevelSpriteX = 176;
inline constexpr int kEnchantingLevelSpriteY = 0;
// The three 108x19 option-bar states, stacked 20px apart below the panel:
// normal, disabled, highlighted.
inline constexpr int kEnchantingBarSpriteY = 168;
// ENCH-3: gui/container/anvil.png, with its text-field and error sprites packed
// into the space its 176x166 panel leaves — same arrangement, same reason.
inline constexpr float kAnvilGuiLayer = 15.0F;
// AR-M6：交易屏**还没有**自己的面板层。vanilla 的 gui/container/villager.png 是
// 512x256，而本作 GUI 图集要求每一层同尺寸（TextureManager 里那句
// "Minecraft GUI textures must share one size"），把它加进去要么把整块图集撑到
// 512 宽、要么给它单独一张图 —— 两条都是渲染侧的一节，不是交易后端的。
//
// 负值是**约定**而不是随手取的数：`drawWorkContainer` 见到负层号就跳过底图，
// 于是这一屏的槽位、悬停与光标层照常工作，只是没有背景。前端把贴图接进图集后，
// 把这里换成真的层号，绘制侧那个判断自然失效。
inline constexpr float kTradingGuiLayer = 22.0F;

// MERCH-1：交易屏的面板是 **276x166**（26.1 `MerchantScreen:57`
// `super(menu, inventory, title, 276, 166)`，从 512x256 的 `container/villager.png`
// 左上角一次 blit 出来）。
//
// ★ **276 > 256**，它塞不进任何一个 GUI 图集层——这不是"把图集扩宽"能绕过去的，
//   必须拆成几块画。拆法是：左边 256x166 整块，右边剩下的 20x166 竖着劈成两半
//   （83 + 83），塞进同一层下方那条 90 高的空带。**一层三块**，而不是两层两块：
//   加层要同步改三处，能少加一层就少加一层。
//
// ★ 这段算术抽成纯函数而不是写在绘制侧，理由与 `iconButtonIconRect` 同：
//   拼错了**不改变任何别的返回值**，画出来只是面板右边少一条或错位一条，
//   而无头测试进不了 Vulkan 头。放在这里，"三块拼回去正好是 276x166"才有地方断言。
inline constexpr int kTradingPanelWidth = 276;
inline constexpr int kTradingPanelHeight = 166;
// 左边那块的宽度就是图集层的边长；右边剩下的部分劈成上下两半。
inline constexpr int kTradingPanelLeftWidth = 256;
inline constexpr int kTradingPanelRightWidth = kTradingPanelWidth - kTradingPanelLeftWidth;
inline constexpr int kTradingPanelRightHalfHeight = kTradingPanelHeight / 2;

// 一块：`source` 是它在图集层里的像素矩形，`offsetX/offsetY` 是它在面板里的落点。
struct TradingPanelPiece final {
    ui::UiRect source{};
    float offsetX = 0.0F;
    float offsetY = 0.0F;
};

// 面板拆成的三块，次序与烘焙侧一致。
[[nodiscard]] constexpr std::array<TradingPanelPiece, 3> tradingPanelPieces() {
    const auto left = static_cast<float>(kTradingPanelLeftWidth);
    const auto right = static_cast<float>(kTradingPanelRightWidth);
    const auto half = static_cast<float>(kTradingPanelRightHalfHeight);
    const auto full = static_cast<float>(kTradingPanelHeight);
    return {{
        {{0.0F, 0.0F, left, full}, 0.0F, 0.0F},
        {{0.0F, full, right, half}, left, 0.0F},
        {{right, full, right, half}, left, half},
    }};
}

// ★ 三块拼回去必须**正好**是 276x166：不重叠、不留缝、不超出 256x256 的层。
//   这三条是拼图唯一会出错的地方，钉在编译期。
static_assert(kTradingPanelRightHalfHeight * 2 == kTradingPanelHeight,
              "面板右侧那条要能被劈成等高的两半");
static_assert(kTradingPanelLeftWidth + kTradingPanelRightWidth == kTradingPanelWidth,
              "左右两块加起来要正好是面板宽");
static_assert(kTradingPanelHeight + kTradingPanelRightHalfHeight <= 256,
              "右侧两半要塞得进左块下方那条空带");
static_assert(kTradingPanelRightWidth * 2 <= 256, "右侧两半并排要放得下");

// UI-9：四张页签精灵所在的层（`TextureManager` 的 images 数组最后一格）。
// ★ 它们各 130x24，竖排要 96 高，`widgets` 那一层放不下——这是本作少数几次
//   真的加一层。加层要同步改三处：数组、kGuiLayerCount、这个常量。
inline constexpr float kTabWidgetLayer = 20.0F;
// UI-11 / A6：世界列表那一行的缺省缩略图（26.1 `FaviconTexture` 的
// `MISSING_LOCATION` = `textures/misc/unknown_server.png`）在同一层里的落位。
// 原图 128x128，按 64x64 存——它永远只画成 32x32。
inline constexpr int kWorldIconFallbackSize = 64;
inline constexpr int kWorldIconFallbackSpriteX = 0;
inline constexpr int kWorldIconFallbackSpriteY = 128;

// UI-11 / A6：**存档自己的**缩略图（`<world>/icon.png`）所在的层。
//
// ★ 这是本作第二次真的加一层（第一次是页签）。理由与那次同族：内容是**运行期
//   才知道**的（每个存档一张，进世界列表时才读盘），不能挤进别的层的空白——
//   那些层是启动时烘一次的静态美术，为了刷新一张缩略图去重烘整张图集，代价是
//   重新解码上百个 PNG，还会换掉 VkImage 把描述符里的绑定作废。
//   独占一层就能用 `uploadImageLayerRange` 原地改像素，图像句柄不变。
//   加层要同步改三处：images 数组、kGuiLayerCount、这个常量。
inline constexpr float kWorldIconLayer = 21.0F;
// 一层 256x256 按 64x64 切成 4x4，共 16 个槽位。
// 世界列表一屏最多放得下 (逻辑高 - 页眉 - 页脚) / 36 行，1080p@scale2 也只有十几行。
inline constexpr int kWorldIconSlotSize = 64;
inline constexpr int kWorldIconSlotsPerRow = 4;
inline constexpr int kWorldIconSlotCount = kWorldIconSlotsPerRow * kWorldIconSlotsPerRow;

// 第 `slot` 个槽位在那一层里的像素矩形。
//
// ★ 抽成纯函数而不是绘制侧的两行取模：槽位算错**不改变任何别的返回值**，
//   症状只是"某一行显示的是另一个存档的缩略图"——没有任何东西会红。
//   这正是 UI-4 图标居中那次的形状。
[[nodiscard]] constexpr ui::UiRect worldIconSlotRect(int slot) {
    const int column = slot % kWorldIconSlotsPerRow;
    const int row = slot / kWorldIconSlotsPerRow;
    return {static_cast<float>(column * kWorldIconSlotSize),
            static_cast<float>(row * kWorldIconSlotSize),
            static_cast<float>(kWorldIconSlotSize), static_cast<float>(kWorldIconSlotSize)};
}

// A1：容器界面那张面板底图在 GUI 图集里的层号。
//
// ★ 从前它是绘制侧一条**四段三元链**（`chestScreen ? 10 : CraftingTable ? 7 : …`），
//   而三元链没有穷尽性检查：加一块容器屏，它会静默落到链尾那个 `: 8.0F`——画出来
//   的是熔炉的面板。现在是不带 `default` 的 switch，加一种 ContainerPageKind
//   编译器会点名。
//
// ★ 它住在这里而不是 HudRenderer 里，是为了**测得到**：层号是纯绘制常量，改错它
//   不改变任何别的返回值，而无头测试进不了 Vulkan 头。放在层号常量自己身边，
//   「每一屏的面板互不相同」那条性质才有地方断言——少了它，把箱子的层号写成熔炉的
//   那次 sabotage 全套测试照样全绿（实测）。
[[nodiscard]] constexpr float containerPanelLayer(ui::ContainerPageKind kind) {
    switch (kind) {
    case ui::ContainerPageKind::Chest:           return 10.0F;
    case ui::ContainerPageKind::CraftingTable:   return 7.0F;
    case ui::ContainerPageKind::EnchantingTable: return kEnchantingGuiLayer;
    case ui::ContainerPageKind::Anvil:           return kAnvilGuiLayer;
    case ui::ContainerPageKind::Furnace:         return 8.0F;
    // AR-M6：交易屏自己的面板层。给它一个**独立**的层号而不是让它落到函数尾部
    // 那个 8.0F —— 那正是这个 switch 不带 default 要防的静默错误（会画成熔炉的面板）。
    case ui::ContainerPageKind::Trading:         return kTradingGuiLayer;
    case ui::ContainerPageKind::SurvivalInventory:    return 2.0F;
    case ui::ContainerPageKind::CreativeInventoryTab: return 5.0F;
    case ui::ContainerPageKind::CreativeCatalogTab:   return 3.0F;
    case ui::ContainerPageKind::Count:           break;   // 哨兵，不是一屏
    }
    return 8.0F;
}
// The 110x16 text field (normal then disabled) below the panel, and the 28x21
// "too expensive" error marker to the right of them.
inline constexpr int kAnvilTextFieldSpriteY = 168;
inline constexpr int kAnvilErrorSpriteX = 176;
inline constexpr int kAnvilErrorSpriteY = 0;
// I-2: gui/sprites/tooltip/background.png 与 tooltip/frame.png，两张 100x100
// 并排放在同一层（背景在左、边框在右）。它们是提示框的全部底衬，画法见
// HudRenderer::drawTooltipBox。
inline constexpr float kTooltipGuiLayer = 16.0F;
// UI-2: gui/title/background/panorama_overlay.png，最近邻拉伸到整层后铺满全屏。
// 26.1 里它是 1x1、alpha 恒 0 的全透明图（本地 26.1 资源包已解码确认，同包 vignette 仍
// 256x256、panorama_0 仍 1024x1024，所以不是转换压尺寸的产物），因此用原版资源时是零效果。
inline constexpr float kPanoramaOverlayGuiLayer = 17.0F;
// UI-5：gui/inworld_menu_list_background.png，滚动列表在**有世界**时的底衬
// （`AbstractSelectionList:226`）。同样按 32 逻辑像素平铺。
inline constexpr float kInworldMenuListBackgroundGuiLayer = 18.0F;
// UI-5 / D9：四张 32x2 的列表分隔纹理竖着叠在同一层里，各已横向平铺满 256。
// ★ 26.1 画的是分隔线，不是 4px 渐隐带——见 ui/ScrollList.hpp 顶上那段更正。
inline constexpr float kListSeparatorGuiLayer = 19.0F;
inline constexpr int kHeaderSeparatorSpriteY = 0;
inline constexpr int kFooterSeparatorSpriteY = 2;
inline constexpr int kInworldHeaderSeparatorSpriteY = 4;
inline constexpr int kInworldFooterSeparatorSpriteY = 6;
inline constexpr int kListSeparatorSpriteHeight = 2;

// UI-2：标题美术在它那张原生分辨率数组里的归一化子矩形，{u, v, 宽, 高}。
// 由 TextureManager::createTitleTexture() 填充，HudRenderer 绑一个 const 引用照着画。
// 已经含了 26.1 那两处"只取纹理上半部分"的裁剪：logo 取 44/64，edition 取 14/16。
struct TitleArtUv final {
    glm::vec4 logo{0.0F, 0.0F, 1.0F, 1.0F};
    glm::vec4 easterEggLogo{0.0F, 0.0F, 1.0F, 1.0F};
    glm::vec4 edition{0.0F, 0.0F, 1.0F, 1.0F};
};

// UI-2：标题/前端界面在全景之上铺的那一层。
//
// 26.1 只有一层，而且它**永远是资源包提供的真实纹理**，不是代码里写死的颜色：
//   - 主菜单（未模糊）：`TitleScreen.extractBackground()` 是空实现，全景之后只有
//     `Panorama.extractRenderState` 那一次 panorama_overlay 全屏 blit。
//   - 二级界面（模糊）：`Screen.extractMenuBackground` 铺 gui/menu_background.png。
// 两条分支的 tint 都是白色不透明——**任何"为了让白字清楚"而写死的变暗都属于自造**，
// spec §6.3 明确写的是"不模糊、不加菜单遮罩，主菜单本体是清晰的"。
// 把配方收在这一个 constexpr 里，绘制侧只是照着铺，于是"多铺了一层暗色"改不动它而不被发现。
struct TitleBackgroundLayer final {
    float guiLayer = kPanoramaOverlayGuiLayer;
    // 乘进纹素的颜色。两条分支都必须是白色不透明：变暗要来自纹理，不来自代码。
    glm::vec4 tint{1.0F, 1.0F, 1.0F, 1.0F};
    // menu_background 是按 32 逻辑像素平铺的，panorama_overlay 是整张拉满。
    bool tiled = false;
};

// UI-5：档位决定铺哪一张。从前的参数是一个 `bool blurred`，那时只有两档；
// 26.1 的第三档是"有世界"——它铺 inworld_menu_background 而不是 menu_background
// （`Screen.extractMenuBackground():450`）。
//
// ★ 26.1 原版这两张的像素恰好完全一样（都是纯 rgba(0,0,0,64)），所以用原版资源时
//   这一档分不分看不出区别。分它的理由是 26.1 的代码真按 `level == null` 选两个
//   不同的资源 id——资源包可以把它们做成两样。
//
// 渐变两档（容器、死亡屏）不铺任何遮罩，调用方按 backgroundTilesMenuTexture 判断，
// 不该走到这里；真走到了返回全透明的 panorama_overlay，也就是不画。
[[nodiscard]] constexpr TitleBackgroundLayer titleBackgroundLayer(ui::ScreenBackgroundKind kind) {
    switch (kind) {
    case ui::ScreenBackgroundKind::PanoramaBlur:
        return TitleBackgroundLayer{kMenuBackgroundGuiLayer, {1.0F, 1.0F, 1.0F, 1.0F}, true};
    case ui::ScreenBackgroundKind::InWorldBlur:
        return TitleBackgroundLayer{kInworldMenuBackgroundGuiLayer, {1.0F, 1.0F, 1.0F, 1.0F}, true};
    case ui::ScreenBackgroundKind::PanoramaClear:
    case ui::ScreenBackgroundKind::Transparent:
    case ui::ScreenBackgroundKind::RedGradient:
        break;
    }
    return TitleBackgroundLayer{kPanoramaOverlayGuiLayer, {1.0F, 1.0F, 1.0F, 1.0F}, false};
}

// 滚动列表的底衬与两道分隔线，同样按"有没有世界"取 inworld 那一套
// （`AbstractSelectionList:219-227`）。
[[nodiscard]] constexpr float menuListBackgroundLayer(bool worldOpen) {
    return worldOpen ? kInworldMenuListBackgroundGuiLayer : kMenuListBackgroundGuiLayer;
}

[[nodiscard]] constexpr int headerSeparatorSpriteY(bool worldOpen) {
    return worldOpen ? kInworldHeaderSeparatorSpriteY : kHeaderSeparatorSpriteY;
}

[[nodiscard]] constexpr int footerSeparatorSpriteY(bool worldOpen) {
    return worldOpen ? kInworldFooterSeparatorSpriteY : kFooterSeparatorSpriteY;
}
// 标题界面的六张全景面，拼成 logo 背后的那个世界；标题轮播把它们当幻灯片循环
// 也是 TextureManager 上传全景数组层时的层数（此前两处各写一份，ENCH-2 并到这里）
inline constexpr std::size_t kPanoramaFaces = 6U;
// 26.1 的十个内容页签加上"背包"伪页签，七个在上排、四个在下排
// 对应 tab_top_1..7 与 tab_bottom_1..4
inline constexpr std::size_t kCreativeTabCount = 11U;

// "当前开着哪个界面"是玩法事实，槽位路由要据此分派
// 所以枚举跟 ScreenHandler 放在一起，渲染器只是引用它
using ContainerScreen = gameplay::ContainerScreen;

// hud.frag dispatches on data.x with `>` comparisons against half-way values, so
// the modes are spaced apart. Named here rather than written as bare floats at
// each call site, which is how a mode number ends up meaning two things.
inline constexpr float kHudModeFlat = 0.0F;         // untextured quad
inline constexpr float kHudModeBlockTexture = 1.0F; // a block atlas sprite
inline constexpr float kHudModeFontGlyph = 2.0F;
inline constexpr float kHudModeGuiSprite = 3.0F;
inline constexpr float kHudModeBlockIcon = 4.25F; // RN-14's 3D item model icon
inline constexpr float kHudModeCrosshair = 5.0F;
// UI-2：主菜单 logo / edition 副标题，取自 binding 6 的原生分辨率标题数组
inline constexpr float kHudModeTitleTexture = 6.0F;

// The HUD's push constants, and the ONE meaning each field has.
//
// **A field's meaning does not change with the draw mode.** A field may go unused
// in a mode; it may never be reinterpreted. RN-14 broke that — it put the icon's
// box into `color` and its box maximum into `uvRect`, told hud.vert, and left
// hud.frag reading `color` as a tint. Every block icon in the inventory came out
// a black diamond: the tint was the box's minimum corner, and the alpha was a UV
// component that is zero on two of the three visible faces.
//
// Three consumers declare this block — here, hud.vert and hud.frag — and
// `hud_push_constant_test` holds all three together.
struct HudPush final {
    // Clip-space rectangle: origin xy, size zw. Every mode.
    glm::vec4 rect;
    // Tint, multiplied into the sampled texel. Every mode. The block icon passes
    // opaque white; anything else there is the regression above.
    glm::vec4 color;
    // Sprite source rectangle: origin xy, size zw. The sprite modes; unused by
    // the block icon, which carries per-corner UVs instead.
    glm::vec4 uvRect;
    // x = draw mode (the kHudMode* constants), y = atlas layer. Every mode.
    // UI-4: z = rotation in radians about the quad's own origin, w = the
    // framebuffer aspect that makes that rotation isotropic in pixels. Only the
    // rotated-text path (26.1's splash) sets them; every other mode leaves them
    // zero, exactly as it leaves iconBoxMin zero. They are new meanings for
    // components that never had one — NOT a reinterpretation of `data.x`/`data.y`.
    glm::vec4 data;
    // The block icon draws ONE face of ONE box of the block's item model per
    // call. These four carry that box and that face; every other mode leaves them
    // zero. The box is in 0..1 cell coordinates, already turned into the
    // inventory pose.
    glm::vec4 iconBoxMin; // xyz
    glm::vec4 iconBoxMax; // xyz
    // The face's four corner UVs in the order the quad emits them, resolved on
    // the CPU (mc::world::iconBoxOf). Resolving them there is what let hud.vert
    // drop its per-cube-model UV tables, and those tables were the reason a block
    // item could only ever be a cube.
    glm::vec4 iconUv01; // uv[0].xy, uv[1].xy
    glm::vec4 iconUv23; // uv[2].xy, uv[3].xy
};

// 128 bytes is Vulkan's guaranteed minimum and this block is now exactly that
// size. There is no room left: another field has to shrink something here, or
// move the icon's payload to a uniform buffer.
static_assert(sizeof(HudPush) == 128U, "HUD push constants must fit Vulkan's guaranteed minimum");

// One face of one box of a block's item icon, as push constants.
//
// It is a free function in a Vulkan-free header so that it can be TESTED. The
// call site is HudRenderer::drawHudBlockIcon, inside a translation unit no test
// links; keeping the arithmetic out here is the same move SceneReadback made with
// PreviewImageBytes, and for the same reason — the part that can go wrong quietly
// should not live where nothing can look at it.
[[nodiscard]] inline HudPush makeBlockIconPush(const ui::UiRect& clip,
                                               const world::IconBox& icon, std::size_t face,
                                               float atlasLayer) {
    const auto& uv = icon.uvCorner[face];
    HudPush push{};
    push.rect = {clip.x, clip.y, clip.width, clip.height};
    // White. hud.frag multiplies this into the texel, so the icon's colour is its
    // texture's colour — which is the whole of what an icon is.
    push.color = {1.0F, 1.0F, 1.0F, 1.0F};
    push.data = {kHudModeBlockIcon, atlasLayer, 0.0F, 0.0F};
    push.uvRect = {}; // a sprite rectangle; this mode has none
    push.iconBoxMin = {icon.from.x, icon.from.y, icon.from.z, 0.0F};
    push.iconBoxMax = {icon.to.x, icon.to.y, icon.to.z, 0.0F};
    push.iconUv01 = {uv[0].x, uv[0].y, uv[1].x, uv[1].y};
    push.iconUv23 = {uv[2].x, uv[2].y, uv[3].x, uv[3].y};
    return push;
}

// UI-5：一条竖直渐变矩形的推送常量（26.1 的 `GuiGraphicsExtractor.fillGradient`）。
//
// 它**不是** HudPush 的一个绘制模式，而是自己的管线与自己的块。理由是硬约束：
// 渐变要两个颜色，而 HudPush 已经正好 128 字节，一个自由分量都不剩；唯一的塞法
// 是让某个字段在这个模式下改变含义，那正是上面那条铁律禁止的事。
//
// 声明它的有三处：这里、gradient.vert、gradient.frag，由 hud_push_constant_test 一起钉住。
struct GradientPush final {
    // 裁剪空间矩形：原点 xy，尺寸 zw。
    glm::vec4 rect;
    // 上缘颜色（rect.y 那条边）与下缘颜色（rect.y + rect.w 那条边），RGBA 编码值。
    glm::vec4 topColor;
    glm::vec4 bottomColor;
};

static_assert(sizeof(GradientPush) == 48U, "gradient push constants must match the shader block");

// 把 ui::GradientStops 的两个 ARGB 色标变成一次绘制。
//
// 住在无 Vulkan 的头文件里是为了**可测**：调用点在 HudRenderer 那个翻译单元里，
// 没有测试链接它。分量顺序错位（ARGB 读成 RGBA）会把死亡屏的暗红变成暗青，
// 而"暗红"和"暗青"在一张缩略图上都只是"暗"。
[[nodiscard]] inline GradientPush makeGradientPush(const ui::UiRect& clip,
                                                   const ui::GradientStops& stops) {
    const auto top = ui::unpackArgb(stops.top);
    const auto bottom = ui::unpackArgb(stops.bottom);
    return GradientPush{
        {clip.x, clip.y, clip.width, clip.height},
        {top.r, top.g, top.b, top.a},
        {bottom.r, bottom.g, bottom.b, bottom.a},
    };
}

// 标题全景立方体：x = 偏航、y = 俯仰（弧度）、z = tan(fov/2)、w = 宽高比
//
// UI-5 删掉了第二个 vec4（那是只作用于全景的模糊半径）。模糊现在是一趟整帧后处理
// （MenuBlur），全景不再知道它的存在——从前那个 5x5 盒式近似只能糊全景自己，
// 于是有世界的界面（暂停、背包外的选项）背后永远是清晰的世界。
struct PanoramaPush final {
    glm::vec4 rotationFov;
};

struct ItemPush final {
    glm::vec4 positionSize;
    glm::vec4 textureLayersRotation;
    glm::vec4 data;
    // 非等比长方体的可选 xyz 尺寸；零向量表示沿用 positionSize.w 里的标量尺寸
    glm::vec4 dimensions;
    glm::mat4 viewModelTransform{1.0F};
};

static_assert(sizeof(ItemPush) <= 128U, "Item push constants must fit Vulkan's guaranteed minimum");

// RN-16: the selection wireframe's push block. One draw is one LINE now, not one
// box — see BlockOutlineGeometry.hpp for why the outline is the merged shape's
// edges rather than each box's twelve.
//
// It lives here, beside HudPush and ItemPush, for the reason this file's header
// comment gives: a push block that is assembled at its call site is a push block
// with as many declarations as call sites. The renderer used to build this one as
// a bare `std::array<glm::vec4, 3>` — the fields had no names on the C++ side at
// all, so nothing could hold them against the shader's.
struct OutlinePush final {
    // The block's cell corner, in world coordinates. RN-45: `.w` carries the line
    // width in pixels — the widening happens in the vertex shader and it needs a
    // number, and this struct's three w components were the free space
    // （HudPush 那条「已满 128 字节，只能用未赋义分量」的同一个办法）。
    glm::vec4 blockOrigin;
    // The line's two endpoints, in block-local (0..1) coordinates.
    // RN-45: `.w` carries the framebuffer size (start = width, end = height).
    glm::vec4 segmentStart;
    glm::vec4 segmentEnd;
};

static_assert(sizeof(OutlinePush) <= 128U,
              "Outline push constants must fit Vulkan's guaranteed minimum");

// The one writer. `blockPosition` is the targeted cell; the segment comes from
// `outlineEdgesOf` in block-local coordinates and is not transformed here — the
// shader adds the origin, exactly as the box form did.
[[nodiscard]] inline OutlinePush makeOutlineSegmentPush(glm::ivec3 blockPosition,
                                                        const OutlineSegment& segment,
                                                        float framebufferWidth,
                                                        float framebufferHeight) {
    return OutlinePush{
        glm::vec4{static_cast<float>(blockPosition.x), static_cast<float>(blockPosition.y),
                  static_cast<float>(blockPosition.z),
                  // RN-45：线宽是**帧缓冲宽度**的函数，不是一个常数——vanilla 的
                  // Window.getAppropriateLineWidth。单一源在 BlockOutlineGeometry.hpp
                  outlineLineWidthPixels(framebufferWidth)},
        glm::vec4{segment.start, framebufferWidth},
        glm::vec4{segment.end, framebufferHeight},
    };
}

// --- item_entity.vert's draw modes, and the categories over them
//
// One pipeline serves fifteen kinds of draw, selected by `data.x`. Two things
// live here and they are NOT the same thing:
//
//   * a MODE SELECTOR. A draw is exactly one mode. The set is exclusive and, over
//     the modes that have producers, complete.
//   * a CATEGORY. A property several modes share — "is placed by a matrix", "is a
//     block item's box". A category covers several modes ON PURPOSE.
//
// Conflating them is not hypothetical. `blockItemBox` deliberately covers both 10
// and 11: a held block IS a block-item box and has to reach the branch that
// resolves the UV rect. Rewriting that predicate as exclusive would put the
// regression this file's history is about straight back.
//
// What is removed instead is the THRESHOLD as a way of expressing membership.
// `data.x > 9.5` covers 10 and 11 today and would silently swallow a mode 12
// tomorrow — a new draw kind changing an existing branch's meaning by arithmetic
// accident. Every category below lists its members.
//
// The shader mirrors this block between its own begin/end markers and derives its
// comparison bounds from the constants (it keeps float `> m - 0.5 && < m + 0.5`
// comparisons — that is the right way to compare floats, and `==` is not).
// `hud_push_constant_test` parses both and holds them together.
//
// ---- item draw modes: begin ----
inline constexpr float kItemModeWorldBillboard = 0.0F;      // camera-facing, whole layer
inline constexpr float kItemModeBlockCube = 1.0F;           // falling block, breaking overlay
inline constexpr float kItemModeEntityShadow = 2.0F;        // the round shadow blob
inline constexpr float kItemModeHeldSprite = 3.0F;          // flat sprite in view space
inline constexpr float kItemModeViewSkinCuboid = 4.0F;      // skinned cuboid in view space
inline constexpr float kItemModeArticulatedCuboid = 5.0F;   // skinned cuboid in world space
inline constexpr float kItemModeMatrixViewModel = 6.0F;     // the view matrix carries the pose
inline constexpr float kItemModeGeneratedItem = 7.0F;       // extruded flat-sprite item model
inline constexpr float kItemModeWorldMatrixCuboid = 8.0F;   // chest lid, articulated bones
inline constexpr float kItemModeBoxUvEntity = 9.0F;         // mobs and NPCs
inline constexpr float kItemModeBlockItemDropped = 10.0F;   // RN-14: one face of one box
inline constexpr float kItemModeBlockItemHeld = 11.0F;      // the same, placed by the matrix
inline constexpr float kItemModeAtlasBillboard = -1.0F;     // sub-rect UV + opacity (xp orb)
inline constexpr float kItemModeHeldBillboard = -2.0F;      // billboard in view space
inline constexpr float kItemModeMatrixHeldBillboard = -3.0F; // billboard placed by the matrix
// ---- item draw modes: end ----

inline constexpr std::array kItemModes{
    kItemModeMatrixHeldBillboard, kItemModeHeldBillboard,    kItemModeAtlasBillboard,
    kItemModeWorldBillboard,      kItemModeBlockCube,        kItemModeEntityShadow,
    kItemModeHeldSprite,          kItemModeViewSkinCuboid,   kItemModeArticulatedCuboid,
    kItemModeMatrixViewModel,     kItemModeGeneratedItem,    kItemModeWorldMatrixCuboid,
    kItemModeBoxUvEntity,         kItemModeBlockItemDropped, kItemModeBlockItemHeld,
};

// Modes the shader still recognises that nothing pushes. Recorded rather than
// deleted: removing a branch of a shader that cannot be run in this container is
// a change to make with eyes on a screen. Each is a shape the renderer once had
// or was built toward; the test asserts this list is exactly the difference
// between what the shader knows and what the renderer sends, so a new orphan
// cannot appear quietly.
inline constexpr std::array kItemModesWithoutProducer{
    kItemModeWorldBillboard,   // every billboard today carries a sub-rect (mode -1)
    kItemModeHeldSprite,       // held flat items go through the extruded model (7)
    kItemModeViewSkinCuboid,   // no view-space skinned cuboid is drawn
    kItemModeArticulatedCuboid,// articulated bones use the world matrix (8)
    kItemModeHeldBillboard,    // no view-space billboard is drawn
    kItemModeMatrixHeldBillboard,
};

// The float comparison the shader makes, mirrored. Ranges, not equality: the mode
// arrives as a float and `==` on floats is how a mode silently stops matching.
[[nodiscard]] inline constexpr bool isItemMode(float value, float mode) {
    return value > mode - 0.5F && value < mode + 0.5F;
}

// --- The three top-level dispatch branches. Every mode takes exactly one. ---
[[nodiscard]] inline constexpr bool itemBranchGeneratedItem(float mode) {
    return isItemMode(mode, kItemModeGeneratedItem);
}
[[nodiscard]] inline constexpr bool itemBranchShadow(float mode) {
    return isItemMode(mode, kItemModeEntityShadow);
}
[[nodiscard]] inline constexpr bool itemBranchCuboid(float mode) {
    return isItemMode(mode, kItemModeBlockCube) || isItemMode(mode, kItemModeHeldSprite) ||
           isItemMode(mode, kItemModeViewSkinCuboid) ||
           isItemMode(mode, kItemModeArticulatedCuboid) ||
           isItemMode(mode, kItemModeMatrixViewModel) ||
           isItemMode(mode, kItemModeWorldMatrixCuboid) ||
           isItemMode(mode, kItemModeBoxUvEntity) ||
           isItemMode(mode, kItemModeBlockItemDropped) ||
           isItemMode(mode, kItemModeBlockItemHeld);
}
// The shader reaches this one by falling through, but it is written here as its
// own member list on purpose. Defined as the complement it would make the
// partition assertion vacuous — every mode would take exactly one branch by
// construction, including a mode that had just been dropped from the cuboid list
// and was now being drawn as a billboard.
[[nodiscard]] inline constexpr bool itemBranchBillboard(float mode) {
    return isItemMode(mode, kItemModeWorldBillboard) ||
           isItemMode(mode, kItemModeAtlasBillboard) ||
           isItemMode(mode, kItemModeHeldBillboard) ||
           isItemMode(mode, kItemModeMatrixHeldBillboard);
}

// --- Categories: several modes on purpose, each member named. ---
[[nodiscard]] inline constexpr bool itemBlockItemBox(float mode) {
    // 10 and 11 together, deliberately: a held block is a block-item box and must
    // reach the UV-rect resolution. This one is load-bearing history.
    return isItemMode(mode, kItemModeBlockItemDropped) ||
           isItemMode(mode, kItemModeBlockItemHeld);
}
// 六面闭合、且不镜像的盒。只有这样的几何才能逐片元丢背面：闭合保证背面永远被正面
// 挡着（半透明方块除外，那正是要丢它的原因），不镜像保证 gl_FrontFacing 说的是真话。
//
// **不是** itemBranchCuboid：那条分支还收着生物模型，而生物的左半边是沿局部 X 轴镜像
// 出来的，镜像翻绕序。按 gl_FrontFacing 丢会剃掉半个生物。
//
// RN-40：这份清单原来只有 kItemModeBlockCube 一个，写在着色器里没有镜像。模式 1 是
// 下落方块与破坏叠加；手持（11）与掉落（10）的方块物品自 RN-14 起是另外两个模式。
[[nodiscard]] inline constexpr bool itemClosedBox(float mode) {
    return isItemMode(mode, kItemModeBlockCube) ||
           isItemMode(mode, kItemModeBlockItemDropped) ||
           isItemMode(mode, kItemModeBlockItemHeld);
}
[[nodiscard]] inline constexpr bool itemUsesMatrix(float mode) {
    return isItemMode(mode, kItemModeMatrixViewModel) ||
           isItemMode(mode, kItemModeWorldMatrixCuboid) ||
           isItemMode(mode, kItemModeBoxUvEntity) ||
           isItemMode(mode, kItemModeBlockItemHeld);
}
[[nodiscard]] inline constexpr bool itemHeldInViewSpace(float mode) {
    return isItemMode(mode, kItemModeHeldSprite) ||
           isItemMode(mode, kItemModeViewSkinCuboid) ||
           isItemMode(mode, kItemModeMatrixViewModel) ||
           isItemMode(mode, kItemModeBlockItemHeld);
}
// The one category with a condition beyond membership: mode 6 joins only when
// `data.w` says the draw is a skin. Kept as it is; the members are still listed.
[[nodiscard]] inline constexpr bool itemPlayerSkinCuboid(float mode, float dataW) {
    return isItemMode(mode, kItemModeViewSkinCuboid) ||
           isItemMode(mode, kItemModeArticulatedCuboid) ||
           isItemMode(mode, kItemModeWorldMatrixCuboid) ||
           (isItemMode(mode, kItemModeMatrixViewModel) && !itemBlockItemBox(mode) &&
            dataW > 0.5F);
}
// The billboard tail's own categories. `itemAtlasBillboard` was written out twice
// in the shader (the UV and the opacity each spelled the compound condition), and
// two copies of a condition is one of them drifting.
[[nodiscard]] inline constexpr bool itemHeldBillboard(float mode) {
    return isItemMode(mode, kItemModeHeldBillboard) ||
           isItemMode(mode, kItemModeMatrixHeldBillboard);
}
[[nodiscard]] inline constexpr bool itemMatrixHeldBillboard(float mode) {
    return isItemMode(mode, kItemModeMatrixHeldBillboard);
}
[[nodiscard]] inline constexpr bool itemAtlasBillboard(float mode) {
    return isItemMode(mode, kItemModeAtlasBillboard);
}

// --- Where a block-item face's UV rect lives, and why it is written in one place
//
// The regression this exists to prevent: mode 10 put the rect in
// `data.yzw` + `positionSize.w`, mode 11 put all four numbers in
// `positionSize.xyzw`, and the shader reads only the first arrangement. Held
// blocks therefore got rect (0, 0, 0, maxV) — zero width in U — and every face
// stretched the atlas layer's u=0 column across itself. Nothing else showed,
// because a held block takes its position from the matrix and never looks at
// `positionSize.xyz`, which is why it survived the round that fixed the icons.
//
// The two modes' push blocks are declared identically and were checked to be so.
// Identical declarations do not make two producers agree about which field holds
// what; that is a separate property and it needs its own guardrail.
//
// So neither call site decides. Both call `setBlockItemFaceRect`, and
// `blockItemFaceRectOf` reads back what item_entity.vert reads — the two are
// mirrors of shader lines that `hud_push_constant_test` re-reads from the source.
inline constexpr void setBlockItemFaceRect(ItemPush& push, const world::ItemModelFace& face) {
    // 0..16 model units into 0..1 of the atlas layer, which is what the shader's
    // `rectCornerUv` expects.
    constexpr float kModelUnitsPerSprite = 16.0F;
    push.data.y = face.uv.minU / kModelUnitsPerSprite;
    push.data.z = face.uv.minV / kModelUnitsPerSprite;
    push.data.w = face.uv.maxU / kModelUnitsPerSprite;
    push.positionSize.w = face.uv.maxV / kModelUnitsPerSprite;
}

// The rect exactly as item_entity.vert reconstructs it:
//   vec4 itemRect = vec4(item.data.y, item.data.z, item.data.w, item.positionSize.w);
[[nodiscard]] inline constexpr glm::vec4 blockItemFaceRectOf(const ItemPush& push) {
    return {push.data.y, push.data.z, push.data.w, push.positionSize.w};
}

// The whole push for one face, per mode. The two differ in how the box is placed
// and in nothing else; both get their rect from the writer above, and neither
// call site assembles an ItemPush of its own any more — which is what keeps
// "these two modes fill the block the same way" a property of the code rather
// than of two people remembering.
[[nodiscard]] inline ItemPush makeDroppedBlockItemFacePush(const world::ItemModelFace& face,
                                                           float atlasLayer, glm::vec3 boxCentre,
                                                           glm::vec3 size, float yawRadians,
                                                           float packedLight) {
    ItemPush push{};
    // World space: the box's centre travels in positionSize.xyz and the yaw in
    // textureLayersRotation.w, because this draw has no matrix.
    push.positionSize = {boxCentre.x, boxCentre.y, boxCentre.z, 0.0F};
    push.textureLayersRotation = {atlasLayer, static_cast<float>(face.quadrant), 0.0F, yawRadians};
    push.data = {kItemModeBlockItemDropped, 0.0F, 0.0F, 0.0F};
    push.dimensions = {size.x, size.y, size.z, packedLight};
    setBlockItemFaceRect(push, face);
    return push;
}

[[nodiscard]] inline ItemPush makeHeldBlockItemFacePush(const world::ItemModelFace& face,
                                                        float atlasLayer, glm::vec3 size,
                                                        float packedLight,
                                                        const glm::mat4& boxTransform) {
    ItemPush push{};
    // The matrix places this one, so positionSize.xyz stays zero. That is exactly
    // why the old code could write the UV rect across all four of its components
    // and produce no symptom except a texture stretched from one column of texels.
    push.positionSize = {0.0F, 0.0F, 0.0F, 0.0F};
    push.textureLayersRotation = {atlasLayer, static_cast<float>(face.quadrant), 0.0F, 0.0F};
    push.data = {kItemModeBlockItemHeld, 0.0F, 0.0F, 0.0F};
    push.dimensions = {size.x, size.y, size.z, packedLight};
    push.viewModelTransform = boxTransform;
    setBlockItemFaceRect(push, face);
    return push;
}

// --- ItemPush's per-mode field map (the accounting the shader comment promised)
//
// One pipeline serves twelve draw kinds, so most of this block IS mode-dependent
// — unlike HudPush, where the reuse was a defect and was removed. Removing it
// here would mean either several pipelines or a uniform buffer, and that is a
// change to make deliberately rather than as a side effect of a bug fix. What is
// recorded instead is which fields carry different things, so the next reader
// does not have to reconstruct it from the dispatch chain:
//
//   positionSize.xyz  world position (0/1/2/10) · the UNINFLATED cube size that
//                     the box-UV net is built from (9) · unused where a matrix
//                     places the draw (6/8/11)
//   positionSize.w    scale (0/1/2) · the block-item face's maxV (10/11) ·
//                     a packed 0xRRGGBB wool tint (9)
//   textureLayersRotation.xyz  three atlas layers, or one layer plus the model's
//                     declared texture width/height (9)
//   textureLayersRotation.w    yaw (most) · the face-override bits (9)
//   data.x            the draw mode. Never anything else.
//   data.y            pitch (most) · shadow opacity (2) · the block-item face's
//                     minU (10/11) · the box-UV net origin U (9)
//   data.z            roll (most) · the block-item face's minV (10/11) ·
//                     the net origin V (9)
//   data.w            a player-skin flag (6) · the block-item face's maxU (10/11)
//                     · the net mirror flag (9)
//   dimensions.xyz    the drawn cube extent, when it is not cubic
//   dimensions.w      the packed scene lightmap, plus 512 for the hurt row (9)
//
// The four block-item entries are the ones that had two producers, and they are
// the ones now written in one place above. The rest have a single producer each.

} // namespace mc::render
