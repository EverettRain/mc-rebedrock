#include "ui/ContainerPage.hpp"

#include "ui/WidgetId.hpp"

#include <algorithm>

namespace mc::ui {
namespace {

[[nodiscard]] Widget panelWidget(const HudLayout& layout, ContainerPageKind kind) {
    const bool creative = kind == ContainerPageKind::CreativeInventoryTab ||
                          kind == ContainerPageKind::CreativeCatalogTab;
    Widget widget;
    widget.kind = WidgetKind::Panel;   // 不可交互：面板只是底图
    widget.rect = creative ? layout.creativePanel() : layout.inventoryPanel();
    return widget;
}

[[nodiscard]] Widget buttonWidget(const UiRect& rect, WidgetId id) {
    Widget widget;
    widget.kind = WidgetKind::Button;
    widget.rect = rect;
    widget.debugId = static_cast<std::uint16_t>(id);
    return widget;
}

[[nodiscard]] Widget slotWidget(const UiRect& rect, gameplay::SlotKind kind,
                                std::uint16_t index) {
    Widget widget;
    widget.kind = WidgetKind::Slot;
    widget.rect = rect;
    widget.slotKind = kind;
    widget.slotIndex = index;
    return widget;
}

// 这一屏独有的非槽位控件。
//
// ★ 不带 `default`：加一块容器屏时编译器会指名道姓，而不是让它静默地少几个控件。
void appendScreenControls(Page& page, ContainerPageKind kind, const HudLayout& layout) {
    switch (kind) {
    case ContainerPageKind::EnchantingTable:
        // ENCH-2：三条 108x19 的选项条是**真按钮**而不是装饰，所以它们是控件而不是
        // 绘制侧的三个矩形。三条相邻扁平排列，第几条由页面里的次序决定。
        for (std::size_t option = 0; option < 3U; ++option) {
            page.push_back(
                buttonWidget(layout.enchantingOption(option), WidgetId::EnchantOption));
        }
        return;
    case ContainerPageKind::CreativeInventoryTab:
        for (std::size_t tab = 0; tab < kCreativeTabWidgetCount; ++tab) {
            page.push_back(buttonWidget(layout.creativeTab(tab), WidgetId::CreativeTab));
        }
        // 删除框只在背包页签上——内容页签下那块地方是目录的一部分。
        page.push_back(
            buttonWidget(layout.creativeDeleteSlot(), WidgetId::CreativeDeleteSlot));
        return;
    case ContainerPageKind::CreativeCatalogTab:
        for (std::size_t tab = 0; tab < kCreativeTabWidgetCount; ++tab) {
            page.push_back(buttonWidget(layout.creativeTab(tab), WidgetId::CreativeTab));
        }
        // 滚动条：控件是**轨道**（那是命中区），把手是绘制时按滚动位置从同一条轨道
        // 派生出来的（`HudLayout::creativeScrollbarThumb`），不是第二个控件。
        // 它只在内容页签上：背包页签下目录是空的，最大滚动行为 0，轨道点不动。
        page.push_back(
            buttonWidget(layout.creativeScrollbarTrack(), WidgetId::CreativeScrollbar));
        return;
    case ContainerPageKind::SurvivalInventory:
    case ContainerPageKind::CraftingTable:
    case ContainerPageKind::Furnace:
    case ContainerPageKind::Chest:
    case ContainerPageKind::Anvil:
        // 这几屏除了槽位没有别的可点的东西（铁砧的名字输入框是 TextField，它今天
        // 由绘制侧自己驱动，A1/A2 再并进来——只做有消费者的控件）。
        return;
    case ContainerPageKind::Count:
        return;   // 哨兵，不是一屏
    }
}

} // namespace

void buildContainerPageInto(Page& page, const gameplay::ScreenContext& context,
                            const HudLayout& layout) {
    page.clear();
    const ContainerPageKind kind =
        containerPageKind(context.screen, context.gameMode, context.creativeInventoryTab);

    // 次序即绘制次序，也就是命中时的层次（后者靠 `ui::hitTest` 取最后一个命中）：
    // 面板在最底下，然后是这一屏独有的控件，最后是槽位。
    page.push_back(panelWidget(layout, kind));
    appendScreenControls(page, kind, layout);

    // ★ 槽位**派生**自 `buildSlotLayout`，不在这里重新枚举一遍——见头文件的
    //   「单一来源」一节。几何形式（`buildSlotLayout` 而不是 `buildSlots`）也是有意的：
    //   它刻意把每个 storage 置空，页面因此够不着模拟线程拥有的背包内存。
    for (const gameplay::SlotView& slot :
         gameplay::ScreenHandler::buildSlotLayout(context, layout)) {
        page.push_back(slotWidget(slot.rect, slot.kind, slot.index));
    }

    // 创造目录那 45 格。它们**不是** `buildSlotLayout` 里的槽（没有存储），但它们确实
    // 是这一屏上可点的格子——A0 之前绘制侧与交互侧各有一段循环画它/命中它，两段都在
    // Widget 模型之外。45 格**全部**登记，包括目录没填满时那些空格：今天它们同样是
    // 命中目标（点空格等于清光标），不是"看不见就不存在"。
    if (kind == ContainerPageKind::CreativeCatalogTab) {
        for (std::size_t cell = 0; cell < HudLayout::kCreativeVisibleSlots; ++cell) {
            page.push_back(slotWidget(layout.creativeSlot(cell),
                                      gameplay::SlotKind::CreativeCatalog,
                                      static_cast<std::uint16_t>(cell)));
        }
    }
}

const Widget* findSlotWidget(const Page& page, gameplay::SlotKind kind, std::uint16_t index) {
    const auto found = std::ranges::find_if(page, [&](const Widget& widget) {
        return widget.kind == WidgetKind::Slot && widget.slotKind == kind &&
               widget.slotIndex == index;
    });
    return found == page.end() ? nullptr : &*found;
}

} // namespace mc::ui
