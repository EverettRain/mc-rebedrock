// A0：容器界面并入 ui::Page 之后的护栏。
//
// ★ 这一整个文件存在的理由：容器屏此前**整屏都在 Widget 模型之外**，于是菜单侧攒下的
//   每一条护栏对它一条都不生效（README 护栏 28 的完整版）。下面这些断言就是把那些
//   护栏第一次接到容器屏上——控件不越界、槽位与点击路由同一批、身份判断只有一处。

#include "gameplay/ScreenHandler.hpp"
#include "gameplay/ScreenTypes.hpp"
#include "ui/ContainerPage.hpp"
#include "ui/HudLayout.hpp"
#include "ui/MenuInteraction.hpp"
#include "ui/WidgetId.hpp"

#include <array>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& what, int line) {
    if (!condition) {
        std::printf("container_page_test line %d: %s\n", line, what.c_str());
        ++failures;
    }
}

#define CHECK(condition) check((condition), #condition, __LINE__)

using mc::gameplay::ContainerScreen;
using mc::gameplay::GameMode;
using mc::gameplay::SlotKind;
using mc::ui::ContainerPageKind;

// 每一种容器页对应的 ScreenContext。**创造背包不是一个 ContainerScreen**，
// 所以这张表的键是 ContainerPageKind，而不是 ContainerScreen。
[[nodiscard]] mc::gameplay::ScreenContext contextFor(ContainerPageKind kind) {
    mc::gameplay::ScreenContext context;
    switch (kind) {
    case ContainerPageKind::SurvivalInventory:
        context.screen = ContainerScreen::PlayerInventory;
        context.gameMode = GameMode::Survival;
        break;
    case ContainerPageKind::CreativeInventoryTab:
        context.screen = ContainerScreen::PlayerInventory;
        context.gameMode = GameMode::Creative;
        context.creativeInventoryTab = true;
        break;
    case ContainerPageKind::CreativeCatalogTab:
        context.screen = ContainerScreen::PlayerInventory;
        context.gameMode = GameMode::Creative;
        context.creativeInventoryTab = false;
        break;
    case ContainerPageKind::CraftingTable:
        context.screen = ContainerScreen::CraftingTable;
        break;
    case ContainerPageKind::Furnace:
        context.screen = ContainerScreen::Furnace;
        break;
    case ContainerPageKind::Chest:
        context.screen = ContainerScreen::Chest;
        context.chest = mc::gameplay::ChestPosition{4, 64, 4};
        break;
    case ContainerPageKind::EnchantingTable:
        context.screen = ContainerScreen::EnchantingTable;
        break;
    case ContainerPageKind::Anvil:
        context.screen = ContainerScreen::Anvil;
        break;
    case ContainerPageKind::Count:
        break;
    }
    return context;
}

// 六种画布，与菜单侧那条"控件不越界"用的是同一组：非整除的缩放档是常态
// （1280/3 = 426.67），只测整除档等于没测。
constexpr std::array<std::pair<float, float>, 6> kCanvases{{
    {1280.0F, 720.0F}, {854.0F, 480.0F}, {1920.0F, 1080.0F},
    {640.0F, 480.0F}, {1281.0F, 721.0F}, {1024.0F, 768.0F},
}};

// --- 1. 身份判断只有一处 -----------------------------------------------------
//
// A0 之前"这是不是创造背包"散在四处各判一次（HudRenderer 三条 if、ScreenHandler
// 两处、VulkanRenderer 一处）。收成一个不带 default 的函数之后，加一块容器屏
// 编译器会点名。
void testPageKind() {
    CHECK(mc::ui::containerPageKind(ContainerScreen::PlayerInventory, GameMode::Survival, true) ==
          ContainerPageKind::SurvivalInventory);
    // ★ 生存背包**没有页签这一说**：creativeInventoryTab 是创造那一档才有意义的轴，
    //   它不该把生存屏分成两种。
    CHECK(mc::ui::containerPageKind(ContainerScreen::PlayerInventory, GameMode::Survival, false) ==
          ContainerPageKind::SurvivalInventory);
    CHECK(mc::ui::containerPageKind(ContainerScreen::PlayerInventory, GameMode::Creative, true) ==
          ContainerPageKind::CreativeInventoryTab);
    CHECK(mc::ui::containerPageKind(ContainerScreen::PlayerInventory, GameMode::Creative, false) ==
          ContainerPageKind::CreativeCatalogTab);
    // 其余五屏与游戏模式无关：一个箱子在创造模式下还是一个箱子。
    for (const auto screen : {ContainerScreen::CraftingTable, ContainerScreen::Furnace,
                              ContainerScreen::Chest, ContainerScreen::EnchantingTable,
                              ContainerScreen::Anvil}) {
        check(mc::ui::containerPageKind(screen, GameMode::Survival, true) ==
                  mc::ui::containerPageKind(screen, GameMode::Creative, false),
              "a non-inventory screen must not depend on the game mode", __LINE__);
    }
}

// --- 2. 槽位是**派生**的，不是另抄一遍 ---------------------------------------
//
// ★ 这是这一轮最要紧的一条断言。页面若自己重新枚举"这一屏有哪些槽"，那就是同一个
//   事实的两份表述（README 护栏 18），而漏改的症状是"画出来的槽和点得到的槽不是
//   同一批"——两边各自都自洽、都编译得过，没有任何别的断言会红。
void testSlotsMatchTheClickRouter() {
    for (const auto& canvas : kCanvases) {
        for (int guiScale = 0; guiScale <= 3; ++guiScale) {
            const mc::ui::HudLayout layout{canvas.first, canvas.second, guiScale};
            for (int raw = 0; raw < static_cast<int>(ContainerPageKind::Count); ++raw) {
                const auto kind = static_cast<ContainerPageKind>(raw);
                const auto context = contextFor(kind);
                mc::ui::Page page;
                mc::ui::buildContainerPageInto(page, context, layout);
                const auto slots =
                    mc::gameplay::ScreenHandler::buildSlotLayout(context, layout);

                // 页面里的槽位（不含创造目录那 45 格——它们不是玩法槽位）与点击路由
                // 的那张表**逐个一致**：同数量、同 kind、同 index、同矩形、同顺序。
                std::vector<const mc::ui::Widget*> slotWidgets;
                for (const auto& widget : page) {
                    if (widget.kind == mc::ui::WidgetKind::Slot &&
                        widget.slotKind != SlotKind::CreativeCatalog) {
                        slotWidgets.push_back(&widget);
                    }
                }
                check(slotWidgets.size() == slots.size(),
                      "the page must carry exactly the click router's slots", __LINE__);
                if (slotWidgets.size() != slots.size()) continue;
                for (std::size_t i = 0; i < slots.size(); ++i) {
                    check(slotWidgets[i]->slotKind == slots[i].kind &&
                              slotWidgets[i]->slotIndex == slots[i].index,
                          "slot identity must match the click router", __LINE__);
                    check(slotWidgets[i]->rect.x == slots[i].rect.x &&
                              slotWidgets[i]->rect.y == slots[i].rect.y &&
                              slotWidgets[i]->rect.width == slots[i].rect.width &&
                              slotWidgets[i]->rect.height == slots[i].rect.height,
                          "slot geometry must match the click router", __LINE__);
                }
            }
        }
    }
}

// --- 3. 每一种槽都在某一屏出现过 ---------------------------------------------
//
// ★ 用 `SlotKind::Count` 哨兵遍历，不是"当时的最后一个枚举值"（README 护栏 25）。
//   加一种槽而忘了让它进页面，这里会指着它不放。
void testEverySlotKindAppears() {
    const mc::ui::HudLayout layout{1280.0F, 720.0F, 2};
    std::array<bool, static_cast<std::size_t>(SlotKind::Count)> seen{};
    for (int raw = 0; raw < static_cast<int>(ContainerPageKind::Count); ++raw) {
        mc::ui::Page page;
        mc::ui::buildContainerPageInto(page, contextFor(static_cast<ContainerPageKind>(raw)),
                                       layout);
        for (const auto& widget : page) {
            if (widget.kind == mc::ui::WidgetKind::Slot) {
                seen[static_cast<std::size_t>(widget.slotKind)] = true;
            }
        }
    }
    for (std::size_t raw = 0; raw < seen.size(); ++raw) {
        check(seen[raw],
              "SlotKind #" + std::to_string(raw) + " never appears on any container page",
              __LINE__);
    }
}

// --- 4. 控件不越界（菜单侧那条护栏第一次作用于容器屏）-------------------------
void testNoWidgetEscapesTheCanvas() {
    for (const auto& canvas : kCanvases) {
        for (int guiScale = 0; guiScale <= 4; ++guiScale) {
            const mc::ui::HudLayout layout{canvas.first, canvas.second, guiScale};
            for (int raw = 0; raw < static_cast<int>(ContainerPageKind::Count); ++raw) {
                mc::ui::Page page;
                mc::ui::buildContainerPageInto(
                    page, contextFor(static_cast<ContainerPageKind>(raw)), layout);
                check(!page.empty(), "every container page must have widgets", __LINE__);
                for (const auto& widget : page) {
                    const auto& rect = widget.rect;
                    check(rect.x >= 0.0F && rect.y >= 0.0F,
                          "a container widget escaped the top/left of the canvas", __LINE__);
                    check(rect.x + rect.width <= canvas.first &&
                              rect.y + rect.height <= canvas.second,
                          "a container widget escaped the bottom/right of the canvas", __LINE__);
                    check(rect.width > 0.0F && rect.height > 0.0F,
                          "a container widget has no area", __LINE__);
                }
            }
        }
    }
}

// --- 5. 可交互控件互不重叠 ---------------------------------------------------
//
// ★ 这条为 A2 铺路：今天的命中是 `ScreenHandler::slotAt`（取**第一个**命中），
//   而 `ui::hitTest` 取**最后一个**（后画即在上）。两者只在"没有重叠"时等价——
//   一旦有重叠，换命中函数就会静默改变点击落到哪个控件上。
void testInteractiveWidgetsDoNotOverlap() {
    const mc::ui::HudLayout layout{1280.0F, 720.0F, 2};
    for (int raw = 0; raw < static_cast<int>(ContainerPageKind::Count); ++raw) {
        mc::ui::Page page;
        mc::ui::buildContainerPageInto(page, contextFor(static_cast<ContainerPageKind>(raw)),
                                       layout);
        for (std::size_t i = 0; i < page.size(); ++i) {
            if (!page[i].interactive()) continue;
            for (std::size_t j = i + 1U; j < page.size(); ++j) {
                if (!page[j].interactive()) continue;
                const auto& a = page[i].rect;
                const auto& b = page[j].rect;
                const bool overlaps = a.x < b.x + b.width && b.x < a.x + a.width &&
                                      a.y < b.y + b.height && b.y < a.y + a.height;
                check(!overlaps, "two interactive container widgets overlap", __LINE__);
            }
        }
    }
}

// --- 6. 逐屏的形状 -----------------------------------------------------------
//
// 数出来的是**这一屏该有几个什么**，取的是 26.1 的数：箱子 27 格、工作台 3x3+1、
// 附魔台三条选项条、创造目录 9x5。照代码写的断言是固化，不是覆盖。
void testPerScreenShape() {
    const mc::ui::HudLayout layout{1280.0F, 720.0F, 2};
    const auto countOf = [&](ContainerPageKind kind, SlotKind slotKind) {
        mc::ui::Page page;
        mc::ui::buildContainerPageInto(page, contextFor(kind), layout);
        std::size_t total = 0;
        for (const auto& widget : page) {
            if (widget.kind == mc::ui::WidgetKind::Slot && widget.slotKind == slotKind) {
                ++total;
            }
        }
        return total;
    };
    const auto buttonsOf = [&](ContainerPageKind kind, mc::ui::WidgetId id) {
        mc::ui::Page page;
        mc::ui::buildContainerPageInto(page, contextFor(kind), layout);
        std::size_t total = 0;
        for (const auto& widget : page) {
            if (widget.kind == mc::ui::WidgetKind::Button &&
                widget.debugId == static_cast<std::uint16_t>(id)) {
                ++total;
            }
        }
        return total;
    };

    // 26.1 的单箱是 27 格（三行九列），玩家自己 36 格。
    CHECK(countOf(ContainerPageKind::Chest, SlotKind::ChestStorage) == 27U);
    CHECK(countOf(ContainerPageKind::Chest, SlotKind::PlayerInventory) == 36U);
    // 工作台是 3x3 加一个结果格；生存背包是 2x2 加一个。
    CHECK(countOf(ContainerPageKind::CraftingTable, SlotKind::TableCraftingGrid) == 9U);
    CHECK(countOf(ContainerPageKind::CraftingTable, SlotKind::TableCraftingOutput) == 1U);
    CHECK(countOf(ContainerPageKind::SurvivalInventory, SlotKind::PlayerCraftingGrid) == 4U);
    // ★ 创造背包**没有合成格**（26.1 的创造屏根本不带 2x2）。
    CHECK(countOf(ContainerPageKind::CreativeInventoryTab, SlotKind::PlayerCraftingGrid) == 0U);
    // 装备槽：四件护甲 + 副手，只在背包屏（生存与创造的背包页签）上。
    CHECK(countOf(ContainerPageKind::SurvivalInventory, SlotKind::Equipment) == 5U);
    CHECK(countOf(ContainerPageKind::CreativeInventoryTab, SlotKind::Equipment) == 5U);
    CHECK(countOf(ContainerPageKind::CreativeCatalogTab, SlotKind::Equipment) == 0U);
    CHECK(countOf(ContainerPageKind::Chest, SlotKind::Equipment) == 0U);
    // ★ 内容页签下**只有快捷栏那 9 格**是真的玩家槽，其余 27 格不在屏上。
    CHECK(countOf(ContainerPageKind::CreativeCatalogTab, SlotKind::PlayerInventory) == 9U);
    // 目录 9 列 x 5 行 = 45，且只在内容页签上。
    CHECK(countOf(ContainerPageKind::CreativeCatalogTab, SlotKind::CreativeCatalog) == 45U);
    CHECK(countOf(ContainerPageKind::CreativeInventoryTab, SlotKind::CreativeCatalog) == 0U);
    // 附魔台三条选项条。
    CHECK(buttonsOf(ContainerPageKind::EnchantingTable, mc::ui::WidgetId::EnchantOption) == 3U);
    CHECK(buttonsOf(ContainerPageKind::Chest, mc::ui::WidgetId::EnchantOption) == 0U);
    // 创造页签十一个（十个内容 + 一个背包），两个页签下都有。
    CHECK(buttonsOf(ContainerPageKind::CreativeInventoryTab, mc::ui::WidgetId::CreativeTab) == 11U);
    CHECK(buttonsOf(ContainerPageKind::CreativeCatalogTab, mc::ui::WidgetId::CreativeTab) == 11U);
    CHECK(buttonsOf(ContainerPageKind::SurvivalInventory, mc::ui::WidgetId::CreativeTab) == 0U);
    // 删除框只在背包页签；滚动条只在内容页签。
    CHECK(buttonsOf(ContainerPageKind::CreativeInventoryTab,
                    mc::ui::WidgetId::CreativeDeleteSlot) == 1U);
    CHECK(buttonsOf(ContainerPageKind::CreativeCatalogTab,
                    mc::ui::WidgetId::CreativeDeleteSlot) == 0U);
    CHECK(buttonsOf(ContainerPageKind::CreativeCatalogTab,
                    mc::ui::WidgetId::CreativeScrollbar) == 1U);
    CHECK(buttonsOf(ContainerPageKind::CreativeInventoryTab,
                    mc::ui::WidgetId::CreativeScrollbar) == 0U);
}

// --- 7. 面板在最底下，且不可交互 ---------------------------------------------
void testPanelIsFirstAndInert() {
    const mc::ui::HudLayout layout{1280.0F, 720.0F, 2};
    for (int raw = 0; raw < static_cast<int>(ContainerPageKind::Count); ++raw) {
        mc::ui::Page page;
        mc::ui::buildContainerPageInto(page, contextFor(static_cast<ContainerPageKind>(raw)),
                                       layout);
        check(!page.empty() && page.front().kind == mc::ui::WidgetKind::Panel,
              "the panel must come first (it is the backdrop)", __LINE__);
        check(!page.front().interactive(), "the panel must not be a click target", __LINE__);
        // 面板盖住整块界面，若它可交互，`ui::hitTest`（取最后一个命中）之外的每一次
        // 命中都会先撞上它。
    }
}

// --- 8. 拖拽预览那条查找 -----------------------------------------------------
//
// findSlotWidget 是 A0 唯一的生产消费者（dragSlotRectangle）所走的那条路。
void testFindSlotWidget() {
    const mc::ui::HudLayout layout{1280.0F, 720.0F, 2};
    mc::ui::Page page;
    const auto context = contextFor(ContainerPageKind::Chest);
    mc::ui::buildContainerPageInto(page, context, layout);

    const auto slots = mc::gameplay::ScreenHandler::buildSlotLayout(context, layout);
    for (const auto& slot : slots) {
        const auto* widget = mc::ui::findSlotWidget(page, slot.kind, slot.index);
        check(widget != nullptr, "every routed slot must be findable on the page", __LINE__);
        if (widget != nullptr) {
            check(widget->rect.x == slot.rect.x && widget->rect.y == slot.rect.y,
                  "findSlotWidget must return that slot's own rectangle", __LINE__);
        }
    }
    // 不在这一屏上的槽找不到，而不是返回别的槽。
    CHECK(mc::ui::findSlotWidget(page, SlotKind::AnvilOutput, 0U) == nullptr);
    CHECK(mc::ui::findSlotWidget(page, SlotKind::ChestStorage, 99U) == nullptr);
}

// --- 9. 命中测试第一次作用于容器屏 -------------------------------------------
void testHitTest() {
    const mc::ui::HudLayout layout{1280.0F, 720.0F, 2};
    mc::ui::Page page;
    const auto context = contextFor(ContainerPageKind::Chest);
    mc::ui::buildContainerPageInto(page, context, layout);

    // 每个槽的中心都命中它自己。
    for (const auto& widget : page) {
        if (widget.kind != mc::ui::WidgetKind::Slot) continue;
        const float cx = widget.rect.x + widget.rect.width * 0.5F;
        const float cy = widget.rect.y + widget.rect.height * 0.5F;
        const std::size_t hit = mc::ui::hitTest(page, cx, cy);
        check(hit != mc::ui::kNoWidget && page[hit].kind == mc::ui::WidgetKind::Slot &&
                  page[hit].slotKind == widget.slotKind &&
                  page[hit].slotIndex == widget.slotIndex,
              "a slot's own centre must hit that slot", __LINE__);
    }
    // 画布外命不中任何东西。
    CHECK(mc::ui::hitTest(page, -1.0F, -1.0F) == mc::ui::kNoWidget);
}

// --- 10. 每个槽都落在这一屏自己的面板里 --------------------------------------
//
// ★ 这条抓的是 `HudLayout::armorSlot` 注释里点名的那个坑：**两个面板的居中方式不同**
//   （生存 176x166 / 创造 195x136），把创造屏的槽锚到生存面板上，槽位就整体错位。
//   而那种错位**不会**越出画布、也不会让任何两个控件重叠——上面两条护栏都抓不住它。
//   页签与滚动条不在此列：26.1 的页签本来就画在面板之外。
void testSlotsStayInsideThePanel() {
    for (const auto& canvas : kCanvases) {
        for (int guiScale = 0; guiScale <= 3; ++guiScale) {
            const mc::ui::HudLayout layout{canvas.first, canvas.second, guiScale};
            for (int raw = 0; raw < static_cast<int>(ContainerPageKind::Count); ++raw) {
                mc::ui::Page page;
                mc::ui::buildContainerPageInto(
                    page, contextFor(static_cast<ContainerPageKind>(raw)), layout);
                if (page.empty()) continue;
                const auto& panel = page.front().rect;
                for (const auto& widget : page) {
                    if (widget.kind != mc::ui::WidgetKind::Slot) continue;
                    const auto& rect = widget.rect;
                    check(rect.x >= panel.x && rect.y >= panel.y &&
                              rect.x + rect.width <= panel.x + panel.width &&
                              rect.y + rect.height <= panel.y + panel.height,
                          "a slot escaped its own panel", __LINE__);
                }
            }
        }
    }
}

// --- 11. 源码护栏：生产路径真的走了这一页 ------------------------------------
//
// ★ README 护栏 29：「抽了一个纯函数」和「生产路径真的调了它」是两件事。把
//   `dragSlotRectangle` 改回自己遍历 `buildSlotLayout`，**上面十条断言一条都不会红**
//   ——容器页会退化成一段只有测试看得见的死代码，而 A1/A2 会在一个从没跑过的
//   装配器上继续盖房子。这条守的就是那件事。
void testTheRendererUsesThePage() {
    std::ifstream input{MC_REBEDROCK_RENDERER_SRC, std::ios::binary};
    if (!input) {
        std::printf("container_page_test: cannot open %s\n", MC_REBEDROCK_RENDERER_SRC);
        ++failures;
        return;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    // 剥掉 `//` 行注释：下面要禁的名字正好出现在本函数自己的注释里。
    std::string source;
    {
        std::istringstream lines{buffer.str()};
        std::string line;
        while (std::getline(lines, line)) {
            const auto comment = line.find("//");
            source += comment == std::string::npos ? line : line.substr(0, comment);
            source += '\n';
        }
    }

    const auto start = source.find("dragSlotRectangle(const ui::HudLayout&");
    if (start == std::string::npos) {
        std::printf("container_page_test: dragSlotRectangle not found — if it was renamed, "
                    "move this guard with it rather than deleting it\n");
        ++failures;
        return;
    }
    const auto open = source.find('{', start);
    std::string body;
    int depth = 0;
    for (std::size_t i = open; i < source.size() && open != std::string::npos; ++i) {
        if (source[i] == '{') { ++depth; }
        else if (source[i] == '}') {
            --depth;
            if (depth == 0) { body = source.substr(open, i - open + 1U); break; }
        }
    }
    CHECK(!body.empty());
    if (body.empty()) return;
    // 走的是容器页。
    CHECK(body.find("buildContainerPageInto(") != std::string::npos);
    CHECK(body.find("findSlotWidget(") != std::string::npos);
    // ★ 而**不是**自己再遍历一遍槽位表：那正是 A0 要消掉的第二份表述。
    CHECK(body.find("buildSlotLayout") == std::string::npos);
}

} // namespace

int main() {
    testPageKind();
    testSlotsMatchTheClickRouter();
    testEverySlotKindAppears();
    testNoWidgetEscapesTheCanvas();
    testInteractiveWidgetsDoNotOverlap();
    testPerScreenShape();
    testPanelIsFirstAndInert();
    testFindSlotWidget();
    testHitTest();
    testSlotsStayInsideThePanel();
    testTheRendererUsesThePage();
    if (failures != 0) {
        std::printf("container_page_test: %d checks failed\n", failures);
        return 1;
    }
    return 0;
}
