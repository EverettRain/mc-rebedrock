// A2：容器界面点击决策的无头测试。
//
// ★ 这个文件是 A2 的**前置**，不是它的附赠品。交接文档把交互迁移列为 A 路线里风险
//   最高的一段，理由是那 526 行**零无头测试**：它们住在一个链接 Vulkan 与 GLFW 的
//   翻译单元里，测试进不去，而它们决定的是"点一下会发生什么"。
//   所以先把决策抽成纯函数并在这里钉住，再改生产路径。
//
// ★ 断言写的是**行为**（点这里应该产生哪一条意图），不是把实现抄一遍。

#include "gameplay/ScreenHandler.hpp"
#include "gameplay/ScreenTypes.hpp"
#include "ui/ContainerInteraction.hpp"
#include "ui/ContainerPage.hpp"
#include "ui/HudLayout.hpp"
#include "ui/WidgetId.hpp"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string& what, int line) {
    if (!condition) {
        std::printf("container_interaction_test line %d: %s\n", line, what.c_str());
        ++failures;
    }
}

#define CHECK(condition) check((condition), #condition, __LINE__)

using mc::gameplay::ContainerScreen;
using mc::gameplay::GameMode;
using mc::gameplay::InventoryMouseButton;
using mc::gameplay::SlotKind;
using mc::ui::ContainerActionKind;

constexpr float kCanvasWidth = 1280.0F;
constexpr float kCanvasHeight = 720.0F;

[[nodiscard]] mc::ui::HudLayout layout() {
    return mc::ui::HudLayout{kCanvasWidth, kCanvasHeight, 2};
}

[[nodiscard]] mc::gameplay::ScreenContext chestContext() {
    mc::gameplay::ScreenContext context;
    context.screen = ContainerScreen::Chest;
    context.chest = mc::gameplay::ChestPosition{4, 64, 4};
    return context;
}

[[nodiscard]] mc::gameplay::ScreenContext creativeContext(bool inventoryTab) {
    mc::gameplay::ScreenContext context;
    context.screen = ContainerScreen::PlayerInventory;
    context.gameMode = GameMode::Creative;
    context.creativeInventoryTab = inventoryTab;
    return context;
}

[[nodiscard]] mc::ui::Page pageFor(const mc::gameplay::ScreenContext& context) {
    mc::ui::Page page;
    mc::ui::buildContainerPageInto(page, context, layout());
    return page;
}

[[nodiscard]] mc::ui::UiPoint centreOf(const mc::ui::UiRect& rect) {
    return {rect.x + rect.width * 0.5F, rect.y + rect.height * 0.5F};
}

// 页面里第 `ordinal` 个某种控件。
[[nodiscard]] const mc::ui::Widget* nthButton(const mc::ui::Page& page, mc::ui::WidgetId id,
                                              std::size_t ordinal) {
    std::size_t seen = 0;
    for (const auto& widget : page) {
        if (widget.kind == mc::ui::WidgetKind::Button &&
            widget.debugId == static_cast<std::uint16_t>(id)) {
            if (seen == ordinal) {
                return &widget;
            }
            ++seen;
        }
    }
    return nullptr;
}

[[nodiscard]] const mc::ui::Widget* nthSlot(const mc::ui::Page& page, SlotKind kind,
                                            std::uint16_t index) {
    return mc::ui::findSlotWidget(page, kind, index);
}

// --- 1. 槽位：点中心命中它自己 -----------------------------------------------
void testSlotClicks() {
    const auto context = chestContext();
    const auto page = pageFor(context);
    const mc::ui::ContainerViewState view;

    // 每一个玩法槽位都路由到它自己——**逐个**测，而不是挑一个代表：
    // "第 5 格路由对了"说不出"第 26 格没有偏移一格"。
    const auto slots = mc::gameplay::ScreenHandler::buildSlotLayout(context, layout());
    for (const auto& slot : slots) {
        const auto action = mc::ui::containerClickAction(page, centreOf(slot.rect),
                                                         InventoryMouseButton::Left, view);
        check(action.kind == ContainerActionKind::ClickSlot && action.slotKind == slot.kind &&
                  action.slotIndex == slot.index,
              "a slot's centre must route to that slot", __LINE__);
    }
    // 右键同样落在槽位上（按键只是被带上，不改变路由）。
    const auto* first = nthSlot(page, SlotKind::ChestStorage, 0U);
    CHECK(first != nullptr);
    if (first != nullptr) {
        const auto action = mc::ui::containerClickAction(page, centreOf(first->rect),
                                                         InventoryMouseButton::Right, view);
        CHECK(action.kind == ContainerActionKind::ClickSlot);
        CHECK(action.slotKind == SlotKind::ChestStorage);
    }
}

// --- 2. 面板内的空白 ≠ 面板外 ------------------------------------------------
//
// ★ 这是这一层最容易写错的一条：把"没命中任何控件"当成"点在外面"，玩家每次点到
//   槽位之间的缝隙都会把手上的东西扔到地上。
void testInsideVersusOutside() {
    const auto page = pageFor(chestContext());
    const mc::ui::ContainerViewState view;
    const auto& panel = page.front().rect;

    // 面板左上角往里一点：那里是标题那一行，没有任何控件，但它在面板**里**。
    const mc::ui::UiPoint insideBlank{panel.x + 2.0F, panel.y + 2.0F};
    CHECK(mc::ui::containerClickAction(page, insideBlank, InventoryMouseButton::Left, view).kind ==
          ContainerActionKind::None);

    // 面板之外：丢。
    for (const mc::ui::UiPoint outside : {mc::ui::UiPoint{panel.x - 4.0F, panel.y + 10.0F},
                                          mc::ui::UiPoint{panel.x + panel.width + 4.0F, panel.y},
                                          mc::ui::UiPoint{panel.x, panel.y - 4.0F},
                                          mc::ui::UiPoint{4.0F, 4.0F}}) {
        check(mc::ui::containerClickAction(page, outside, InventoryMouseButton::Left, view).kind ==
                  ContainerActionKind::DropCursor,
              "a click outside the panel must drop the carried stack", __LINE__);
    }
}

// --- 3. 附魔台的三条选项条 ---------------------------------------------------
void testEnchantOptions() {
    mc::gameplay::ScreenContext context;
    context.screen = ContainerScreen::EnchantingTable;
    const auto page = pageFor(context);
    const mc::ui::ContainerViewState view;

    for (std::size_t option = 0; option < 3U; ++option) {
        const auto* bar = nthButton(page, mc::ui::WidgetId::EnchantOption, option);
        check(bar != nullptr, "the enchanting screen must have three option bars", __LINE__);
        if (bar == nullptr) continue;
        const auto action = mc::ui::containerClickAction(page, centreOf(bar->rect),
                                                         InventoryMouseButton::Left, view);
        // ★ 第几条由页面里的**次序**决定，而不是另开三个 WidgetId。
        check(action.kind == ContainerActionKind::ClickEnchantOption && action.index == option,
              "each bar must report its own ordinal", __LINE__);
    }
    // 别的屏没有选项条，同一个坐标不会误报成它。
    const auto chest = pageFor(chestContext());
    const auto* bar = nthButton(page, mc::ui::WidgetId::EnchantOption, 0U);
    if (bar != nullptr) {
        CHECK(mc::ui::containerClickAction(chest, centreOf(bar->rect),
                                           InventoryMouseButton::Left, view)
                  .kind != ContainerActionKind::ClickEnchantOption);
    }
}

// --- 4. 创造目录：有货取货，空格清光标 ---------------------------------------
void testCreativeCatalog() {
    const auto page = pageFor(creativeContext(/*inventoryTab=*/false));
    // 目录里只有 20 件东西：前 20 格有货，其余 25 格是空格。
    mc::ui::ContainerViewState view;
    view.catalogSize = 20U;
    view.catalogFirstIndex = 0U;

    for (std::uint16_t cell = 0; cell < 45U; ++cell) {
        const auto* slot = nthSlot(page, SlotKind::CreativeCatalog, cell);
        check(slot != nullptr, "the catalogue must have 45 cells", __LINE__);
        if (slot == nullptr) continue;
        const auto action = mc::ui::containerClickAction(page, centreOf(slot->rect),
                                                         InventoryMouseButton::Left, view);
        if (cell < 20U) {
            check(action.kind == ContainerActionKind::ClickCreativeItem && action.index == cell,
                  "a stocked catalogue cell must take from the shelf", __LINE__);
        } else {
            // ★ 空格是**删除目标**，不是"丢到地上"——只有点在面板之外才生成掉落物。
            check(action.kind == ContainerActionKind::ClearCursor,
                  "an empty catalogue cell must clear the cursor, not drop it", __LINE__);
        }
    }

    // 滚动之后，同一个可见格对应的是清单里更靠后的一项：第 0 格现在有货还是空格，
    // 取决于 catalogFirstIndex。
    view.catalogFirstIndex = 20U;
    const auto* first = nthSlot(page, SlotKind::CreativeCatalog, 0U);
    CHECK(first != nullptr);
    if (first != nullptr) {
        CHECK(mc::ui::containerClickAction(page, centreOf(first->rect),
                                           InventoryMouseButton::Left, view)
                  .kind == ContainerActionKind::ClearCursor);
    }
}

// --- 5. 页签与滚动条只认左键 -------------------------------------------------
void testCreativeChrome() {
    const auto page = pageFor(creativeContext(/*inventoryTab=*/false));
    mc::ui::ContainerViewState view;
    view.catalogSize = 500U;
    view.catalogScrollable = true;

    for (std::size_t tab = 0; tab < mc::ui::kCreativeTabWidgetCount; ++tab) {
        const auto* widget = nthButton(page, mc::ui::WidgetId::CreativeTab, tab);
        check(widget != nullptr, "the creative screen must have eleven tabs", __LINE__);
        if (widget == nullptr) continue;
        const auto action = mc::ui::containerClickAction(page, centreOf(widget->rect),
                                                         InventoryMouseButton::Left, view);
        check(action.kind == ContainerActionKind::SetCreativeTab && action.index == tab,
              "each tab must select itself", __LINE__);

        // ★ 右键点页签**不是**切页签（26.1 只在左键那一支处理页签，:494）。它落到
        //   "点在面板外就丢东西"那条通用规则上——页签正好在面板之外。
        //   **但当前选中的那个页签除外**：26.1 `hasClickedOutside`（:650-654）
        //   把它排除在"外面"之外（UI-8 / D31 补上的）。
        const auto rightClick = mc::ui::containerClickAction(page, centreOf(widget->rect),
                                                             InventoryMouseButton::Right, view);
        check(rightClick.kind == (tab == view.selectedCreativeTab
                                      ? ContainerActionKind::None
                                      : ContainerActionKind::DropCursor),
              "right-clicking the selected tab must not drop; other tabs still do (D31)",
              __LINE__);
    }

    // ★ 换一个选中页签，"哪一个不丢"跟着走——而不是钉死第 0 个。
    {
        mc::ui::ContainerViewState moved = view;
        moved.selectedCreativeTab = 3U;
        const auto* zero = nthButton(page, mc::ui::WidgetId::CreativeTab, 0U);
        const auto* three = nthButton(page, mc::ui::WidgetId::CreativeTab, 3U);
        CHECK(zero != nullptr && three != nullptr);
        if (zero != nullptr && three != nullptr) {
            CHECK(mc::ui::containerClickAction(page, centreOf(zero->rect),
                                               InventoryMouseButton::Right, moved)
                      .kind == ContainerActionKind::DropCursor);
            CHECK(mc::ui::containerClickAction(page, centreOf(three->rect),
                                               InventoryMouseButton::Right, moved)
                      .kind == ContainerActionKind::None);
        }
    }

    const auto* scrollbar = nthButton(page, mc::ui::WidgetId::CreativeScrollbar, 0U);
    CHECK(scrollbar != nullptr);
    if (scrollbar != nullptr) {
        CHECK(mc::ui::containerClickAction(page, centreOf(scrollbar->rect),
                                           InventoryMouseButton::Left, view)
                  .kind == ContainerActionKind::BeginScrollbarDrag);
        // ★ 滚不动的时候滚动条是死的：点它什么都不发生，而**不是**掉进"丢东西"。
        view.catalogScrollable = false;
        CHECK(mc::ui::containerClickAction(page, centreOf(scrollbar->rect),
                                           InventoryMouseButton::Left, view)
                  .kind == ContainerActionKind::None);
    }

    // 删除框只在背包页签上，任意键都清光标。
    const auto inventoryPage = pageFor(creativeContext(/*inventoryTab=*/true));
    const auto* deleteSlot = nthButton(inventoryPage, mc::ui::WidgetId::CreativeDeleteSlot, 0U);
    CHECK(deleteSlot != nullptr);
    if (deleteSlot != nullptr) {
        for (const auto button : {InventoryMouseButton::Left, InventoryMouseButton::Right}) {
            check(mc::ui::containerClickAction(inventoryPage, centreOf(deleteSlot->rect), button,
                                               {})
                      .kind == ContainerActionKind::ClearCursor,
                  "the delete slot clears the cursor on either button", __LINE__);
        }
    }
}

// --- 6. 双击判定要的那个身份 -------------------------------------------------
void testSlotUnderCursor() {
    const auto context = chestContext();
    const auto page = pageFor(context);
    const auto slots = mc::gameplay::ScreenHandler::buildSlotLayout(context, layout());
    for (const auto& slot : slots) {
        const auto hit = mc::ui::containerSlotUnderCursor(page, centreOf(slot.rect));
        check(hit.hit && hit.slotKind == slot.kind && hit.slotIndex == slot.index,
              "the slot under the cursor must be that slot", __LINE__);
    }
    CHECK(!mc::ui::containerSlotUnderCursor(page, {0.0F, 0.0F}).hit);

    // ★ 目录格**不是**玩法槽位：它没有存储，双击收拢与拖拽收集都不该认它。
    const auto catalog = pageFor(creativeContext(false));
    const auto* cell = nthSlot(catalog, SlotKind::CreativeCatalog, 0U);
    CHECK(cell != nullptr);
    if (cell != nullptr) {
        CHECK(!mc::ui::containerSlotUnderCursor(catalog, centreOf(cell->rect)).hit);
    }
}

// --- 7. 「按下即生效」的那一组 -----------------------------------------------
void testImmediateControls() {
    const auto catalogPage = pageFor(creativeContext(/*inventoryTab=*/false));
    const auto* tab = nthButton(catalogPage, mc::ui::WidgetId::CreativeTab, 0U);
    const auto* scrollbar = nthButton(catalogPage, mc::ui::WidgetId::CreativeScrollbar, 0U);
    const auto* cell = nthSlot(catalogPage, SlotKind::CreativeCatalog, 0U);
    CHECK(tab != nullptr && scrollbar != nullptr && cell != nullptr);
    if (tab != nullptr) CHECK(mc::ui::containerImmediateControlAt(catalogPage, centreOf(tab->rect)));
    if (scrollbar != nullptr)
        CHECK(mc::ui::containerImmediateControlAt(catalogPage, centreOf(scrollbar->rect)));
    if (cell != nullptr)
        CHECK(mc::ui::containerImmediateControlAt(catalogPage, centreOf(cell->rect)));

    // ★ 真实物品槽**不算**：它要走快速合成拖拽那套状态机（按下开始拖，松手才分配）。
    const auto* hotbar = nthSlot(catalogPage, SlotKind::PlayerInventory, 0U);
    CHECK(hotbar != nullptr);
    if (hotbar != nullptr) {
        CHECK(!mc::ui::containerImmediateControlAt(catalogPage, centreOf(hotbar->rect)));
    }
    // 箱子屏一个"按下即生效"的控件都没有。
    const auto chest = pageFor(chestContext());
    for (const auto& widget : chest) {
        check(!mc::ui::containerImmediateControlAt(chest, centreOf(widget.rect)),
              "a chest screen has no press-immediate controls", __LINE__);
    }
}

// --- 8. 每一屏都答得出话（不带 default 的分派在这里体现为"不崩、不乱答"）------
void testEveryScreenAnswers() {
    for (int raw = 0; raw < static_cast<int>(mc::ui::ContainerPageKind::Count); ++raw) {
        const auto kind = static_cast<mc::ui::ContainerPageKind>(raw);
        mc::gameplay::ScreenContext context;
        switch (kind) {
        case mc::ui::ContainerPageKind::SurvivalInventory: break;
        case mc::ui::ContainerPageKind::CreativeInventoryTab: context = creativeContext(true); break;
        case mc::ui::ContainerPageKind::CreativeCatalogTab: context = creativeContext(false); break;
        case mc::ui::ContainerPageKind::CraftingTable:
            context.screen = ContainerScreen::CraftingTable; break;
        case mc::ui::ContainerPageKind::Furnace: context.screen = ContainerScreen::Furnace; break;
        case mc::ui::ContainerPageKind::Chest: context = chestContext(); break;
        case mc::ui::ContainerPageKind::EnchantingTable:
            context.screen = ContainerScreen::EnchantingTable; break;
        case mc::ui::ContainerPageKind::Anvil: context.screen = ContainerScreen::Anvil; break;
        case mc::ui::ContainerPageKind::Count: continue;
        }
        const auto page = pageFor(context);
        // 每一个控件的中心都要给出一条**不是 DropCursor** 的意图：控件都在面板里
        // （页签除外，它们本来就在面板外，而它们自己会被左键接住）。
        for (const auto& widget : page) {
            if (!widget.interactive()) continue;
            const auto action = mc::ui::containerClickAction(page, centreOf(widget.rect),
                                                             InventoryMouseButton::Left, {});
            check(action.kind != ContainerActionKind::DropCursor,
                  "clicking a control must never drop the carried stack", __LINE__);
        }
    }
}

} // namespace

int main() {
    testSlotClicks();
    testInsideVersusOutside();
    testEnchantOptions();
    testCreativeCatalog();
    testCreativeChrome();
    testSlotUnderCursor();
    testImmediateControls();
    testEveryScreenAnswers();
    if (failures != 0) {
        std::printf("container_interaction_test: %d checks failed\n", failures);
        return 1;
    }
    return 0;
}
