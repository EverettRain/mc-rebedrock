#include "ui/ContainerInteraction.hpp"

#include "ui/ContainerPage.hpp"
#include "ui/MenuInteraction.hpp"
#include "ui/WidgetId.hpp"

namespace mc::ui {
namespace {

[[nodiscard]] bool isButton(const Widget& widget, WidgetId id) {
    return widget.kind == WidgetKind::Button &&
           widget.debugId == static_cast<std::uint16_t>(id);
}

// 一个控件在**同 id 的控件**里排第几。三条选项条、十一个页签都靠次序区分，
// 与按键绑定行、资源包行同一个做法——不给每一条各开一个 WidgetId。
[[nodiscard]] std::size_t ordinalAmongSameId(const Page& page, std::size_t hit) {
    std::size_t ordinal = 0;
    for (std::size_t i = 0; i < hit; ++i) {
        if (page[i].kind == page[hit].kind && page[i].debugId == page[hit].debugId) {
            ++ordinal;
        }
    }
    return ordinal;
}

// 点在面板之外吗。面板是页面的第一个控件（`buildContainerPageInto` 的约定）。
//
// ★ 它是"丢东西"的判据，而不是"没命中任何控件"。两者不是一回事：面板内部的空白
//   （槽位之间的缝、标题那一行）没有命中任何控件，但**不该**把手上的东西扔出去。
[[nodiscard]] bool outsidePanel(const Page& page, UiPoint cursor) {
    if (page.empty() || page.front().kind != WidgetKind::Panel) {
        return false;
    }
    return !page.front().rect.contains(cursor.x, cursor.y);
}

} // namespace

ContainerAction containerClickAction(const Page& page, UiPoint cursor,
                                     gameplay::InventoryMouseButton button,
                                     const ContainerViewState& view) {
    const bool leftButton = button == gameplay::InventoryMouseButton::Left;
    const std::size_t hit = hitTest(page, cursor.x, cursor.y);
    if (hit != kNoWidget) {
        const Widget& widget = page[hit];
        if (widget.kind == WidgetKind::Slot) {
            if (widget.slotKind != gameplay::SlotKind::CreativeCatalog) {
                return {ContainerActionKind::ClickSlot, widget.slotKind, widget.slotIndex, 0U};
            }
            // 目录格：有货就取货，空格是删除目标。
            // ★ 空格**不是**"丢到地上"——只有点在面板之外才会生成掉落物实体，
            //   与 vanilla 的容器一致。
            const std::size_t catalogIndex = view.catalogFirstIndex + widget.slotIndex;
            if (catalogIndex >= view.catalogSize) {
                return {ContainerActionKind::ClearCursor, {}, 0U, 0U};
            }
            return {ContainerActionKind::ClickCreativeItem, {}, 0U, widget.slotIndex};
        }
        if (isButton(widget, WidgetId::EnchantOption)) {
            return {ContainerActionKind::ClickEnchantOption, {}, 0U, ordinalAmongSameId(page, hit)};
        }
        if (isButton(widget, WidgetId::CreativeDeleteSlot)) {
            return {ContainerActionKind::ClearCursor, {}, 0U, 0U};
        }
        // ★ 页签与滚动条**只认左键**（26.1 `CreativeModeInventoryScreen:494` 也是
        //   `if (event.button() == 0)`）。
        if (isButton(widget, WidgetId::CreativeTab)) {
            const std::size_t tab = ordinalAmongSameId(page, hit);
            if (leftButton) {
                return {ContainerActionKind::SetCreativeTab, {}, 0U, tab};
            }
            // D31：非左键落到下面那条"面板外就丢东西"的通用规则上——页签正好在面板
            // 之外。但 26.1 的 `hasClickedOutside`（:650-654）把**当前选中的那个页签**
            // 排除在"外面"之外，所以右键点它什么都不发生；点别的页签仍然丢。
            if (tab == view.selectedCreativeTab) {
                return {ContainerActionKind::None, {}, 0U, 0U};
            }
        }
        if (leftButton && isButton(widget, WidgetId::CreativeScrollbar)) {
            // 滚不动的时候滚动条是死的：点它什么都不发生，也不该掉进"丢东西"。
            return view.catalogScrollable
                       ? ContainerAction{ContainerActionKind::BeginScrollbarDrag, {}, 0U, 0U}
                       : ContainerAction{ContainerActionKind::None, {}, 0U, 0U};
        }
    }
    return outsidePanel(page, cursor)
               ? ContainerAction{ContainerActionKind::DropCursor, {}, 0U, 0U}
               : ContainerAction{ContainerActionKind::None, {}, 0U, 0U};
}

ContainerSlotHit containerSlotUnderCursor(const Page& page, UiPoint cursor) {
    const std::size_t hit = hitTest(page, cursor.x, cursor.y);
    if (hit == kNoWidget || page[hit].kind != WidgetKind::Slot ||
        page[hit].slotKind == gameplay::SlotKind::CreativeCatalog) {
        return {};
    }
    return {true, page[hit].slotKind, page[hit].slotIndex};
}

bool containerImmediateControlAt(const Page& page, UiPoint cursor) {
    const std::size_t hit = hitTest(page, cursor.x, cursor.y);
    if (hit == kNoWidget) {
        return false;
    }
    const Widget& widget = page[hit];
    // 目录格算，真实物品槽不算——后者要走快速合成拖拽那套状态机。
    if (widget.kind == WidgetKind::Slot) {
        return widget.slotKind == gameplay::SlotKind::CreativeCatalog;
    }
    return isButton(widget, WidgetId::CreativeTab) ||
           isButton(widget, WidgetId::CreativeDeleteSlot) ||
           isButton(widget, WidgetId::CreativeScrollbar);
}

} // namespace mc::ui
