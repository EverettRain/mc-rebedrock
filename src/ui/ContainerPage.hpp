#pragma once

// A0：容器界面的页面装配器——把背包/箱子/工作台/熔炉/附魔台/铁砧/创造背包
// 装配成一页扁平的 `ui::Widget`，与菜单页用同一个模型。
//
// ## 它解决什么
//
// 本作此前有**两套并行的界面栈**：菜单走 `ui::Page` + `ui::Widget`（一处装配、
// 通用命中、通用派发），容器屏走 `ScreenHandler::SlotView` + `HudLayout` 的 28 个具名
// 槽位函数 + 绘制侧的 5 条 if/else 链 + 交互侧的 24 个函数。后者整屏都在 Widget 模型
// 之外，于是**菜单侧攒下的所有护栏对容器屏一条都不生效**——"控件不越界"抓不住它、
// 命中测试与焦点遍历也够不着它。这是 README 护栏 28 的完整版，不是它的小版本。
//
// ## 单一来源：槽位由 `ScreenHandler::buildSlotLayout` **派生**
//
// ★ 这一页**不重新回答"这一屏有哪些槽"**。那件事已经有一个答案（`buildSlotLayout`，
//   带 372 行专属测试），再写一遍就是同一个事实的两份表述（README 护栏 18），而漏改
//   的症状是"画出来的槽和点得到的槽不是同一批"——没有任何断言会红。
//
// 页面是槽位表的**超集**：槽位 + 面板 + 这一屏独有的非槽位控件（附魔三条选项条、
// 创造页签、删除框、目录滚动条）+ 创造目录那 45 格。
//
// ★ 目录那 45 格是页面**自己**加的，不进 `buildSlotLayout`：它们背后没有存储，
//   点它发的是 `ClickCreativeItem` 而不是 `ClickSlot`（见 `SlotKind::CreativeCatalog`
//   的注释）。真把它们塞进槽位表，一次 shift 点击就会把无限货架当成一块真存储去搬。
//
// ## 扁平，不嵌套
//
// 一行多个控件（三条选项条、十一个页签、45 格目录）一律是**相邻的扁平 Widget**，
// 与 26.1 `children()` 的线性 Tab 序一致，且不需要递归。`Widget::children` 那个
// 0 消费者的空壳已随 A0 删除。

#include "gameplay/ScreenHandler.hpp"
#include "ui/HudLayout.hpp"
#include "ui/Widget.hpp"

#include <cstdint>

namespace mc::ui {

// 这一屏是哪一种容器界面。
//
// ★ **创造背包不是一个独立的 `ContainerScreen`**，它是
//   `PlayerInventory + GameMode::Creative`，而它的两个页签画的东西几乎没有交集。
//   A0 之前这条身份判断散在四处各判一次（`HudRenderer` 三条 if、`ScreenHandler`
//   两处、`VulkanRenderer` 一处），这里把它收成一个函数——**不带 `default`**，
//   加一块容器屏时编译器会点名。
enum class ContainerPageKind : std::uint8_t {
    SurvivalInventory,
    // 创造背包的背包页签：玩家 36 格 + 护甲/副手 + 删除框（没有 2x2 合成格）。
    CreativeInventoryTab,
    // 创造背包的内容页签：9x5=45 格的只读目录 + 页签行 + 滚动条 + 快捷栏。
    CreativeCatalogTab,
    CraftingTable,
    Furnace,
    Chest,
    EnchantingTable,
    Anvil,

    // 哨兵，值等于种类数。不参与分派。
    Count,
};

[[nodiscard]] constexpr ContainerPageKind containerPageKind(gameplay::ContainerScreen screen,
                                                            gameplay::GameMode mode,
                                                            bool creativeInventoryTab) {
    switch (screen) {
    case gameplay::ContainerScreen::PlayerInventory:
        if (mode != gameplay::GameMode::Creative) {
            return ContainerPageKind::SurvivalInventory;
        }
        return creativeInventoryTab ? ContainerPageKind::CreativeInventoryTab
                                    : ContainerPageKind::CreativeCatalogTab;
    case gameplay::ContainerScreen::CraftingTable: return ContainerPageKind::CraftingTable;
    case gameplay::ContainerScreen::Furnace:       return ContainerPageKind::Furnace;
    case gameplay::ContainerScreen::Chest:         return ContainerPageKind::Chest;
    case gameplay::ContainerScreen::EnchantingTable:
        return ContainerPageKind::EnchantingTable;
    case gameplay::ContainerScreen::Anvil:         return ContainerPageKind::Anvil;
    case gameplay::ContainerScreen::Count:         break;   // 哨兵，不是一屏
    }
    return ContainerPageKind::SurvivalInventory;
}

// 创造背包顶上的页签数（26.1 十个内容页签 + 一个背包页签）。
// 绘制侧此前各写各的常量，这里是几何这一侧的那一份。
inline constexpr std::size_t kCreativeTabWidgetCount = 11U;

// 把这一屏装配成一页控件。`page` 会被清空后重填（与菜单页同样的约定：冷路径，
// 每次重建，不做脏标记）。
//
// A0 只填**几何与身份**，不填回调——这一步是只读重构，绘制与交互仍走原路径，
// 验收是容器屏截图逐字节不变。回调在 A2 与交互一起接上。
void buildContainerPageInto(Page& page, const gameplay::ScreenContext& context,
                            const HudLayout& layout);

// 页面里对应某个槽的那个控件，没有则返回 nullptr。
// 身份是 `kind + index` 两个值（`gameplay::SlotRef` 就是这两个字段）。
[[nodiscard]] const Widget* findSlotWidget(const Page& page, gameplay::SlotKind kind,
                                           std::uint16_t index);

} // namespace mc::ui
