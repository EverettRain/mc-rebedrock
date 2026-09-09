#pragma once

// A2：容器界面的**点击决策**——一个纯函数，把"光标在哪、按了哪个键"变成一个意图值。
//
// ## 它解决什么
//
// A2 之前，这件事是 `VulkanRenderer::Impl::dispatchInventoryClick` 里的一百来行：
// 自己抄一遍 `glfwGetCursorPos`、自己造一个 `HudLayout`、自己按屏分支逐个 `contains`
// 测三条附魔选项条、十一个页签、滚动条轨道、删除框、45 个目录格、再落到槽位表。
// 那一百行**一条无头断言都没有**——它住在一个链接 Vulkan 与 GLFW 的翻译单元里，
// 测试进不去。而它决定的是"点一下会发生什么"，是容器界面最容易出错的一层。
//
// 现在决策是一个纯函数：输入一页已经装配好的控件（`ui::Page`）、光标、按键与
// 一点点视图状态，输出一个意图值。渲染器只负责把意图翻译成命令并入队。
//
// ## 为什么意图是**值**而不是回调
//
// 菜单侧的控件自带 `onActivate` 回调，那是对的——菜单按钮的动作五花八门。容器屏
// 不是：它的动作只有八种，而且**每一种都要变成一条网络命令**（`ClickSlot`、
// `ClickCreativeItem`、`DropCursor` …）。让回调去捕获 runtime 与 camera，等于把
// "点了什么"和"怎么发命令"重新焊在一起，无头测试又够不着了。
// 输出一个值，测试就能断言"点这里应该产生哪一条意图"。
//
// ## 一条刻意保留的怪癖
//
// ★ **右键点在创造页签上会把光标物品堆丢出去。** 页签在面板矩形之外，而"点在面板外
//   且手上有东西 ⇒ 丢出去"是容器屏的通用规则；页签只在**左键**那一支被拦截
//   （26.1 也是 `if (event.button() == 0)`）。26.1 的差别在 `hasClickedOutside`
//   （`CreativeModeInventoryScreen:650-654`）：它把"点在**当前选中**的那个页签上"
//   也算作不在外面，本作没有这一条。这是偏差 D31，A2 **不改**——A2 是重构，
//   改行为要另立一条，否则"截图与行为都没变"这句话就没意义了。

#include "gameplay/GameCommand.hpp"
#include "gameplay/ScreenTypes.hpp"
#include "ui/Widget.hpp"

#include <cstddef>
#include <cstdint>

namespace mc::ui {

enum class ContainerActionKind : std::uint8_t {
    // 什么都不做（点在面板内的空白处）。★ 它与 DropCursor 是两回事：容器屏的面板
    // 内部点空白是无操作，面板**外面**才丢东西。混成一个，玩家每次点面板空白都会
    // 把手上的东西扔到地上。
    None,
    // 一个真正的玩法槽位（`slotKind` + `slotIndex`）。
    ClickSlot,
    // 创造目录里的一格无限货架（`index` 是可见格序号 0..44）。
    ClickCreativeItem,
    // 清空光标（创造的删除框、以及目录里已经没有内容的空格）。
    ClearCursor,
    // 把光标上的东西扔到地上（点在面板之外）。
    DropCursor,
    // 附魔台的第 `index` 条选项条。
    ClickEnchantOption,
    // 切到第 `index` 个创造页签。
    SetCreativeTab,
    // 开始拖创造目录的滚动条。
    BeginScrollbarDrag,
};

struct ContainerAction final {
    ContainerActionKind kind = ContainerActionKind::None;
    // ClickSlot 用这两个字段；其余 kind 下没有意义。
    gameplay::SlotKind slotKind = gameplay::SlotKind::PlayerInventory;
    std::uint16_t slotIndex = 0U;
    // ClickCreativeItem / ClickEnchantOption / SetCreativeTab 用它：
    // 分别是可见格序号、第几条选项条、第几个页签。
    std::size_t index = 0U;

    [[nodiscard]] bool operator==(const ContainerAction&) const = default;
};

// 决策要用到的、不在页面里的那点状态。
struct ContainerViewState final {
    // 创造目录当前窗口的第一格在整张清单里的下标（滚动行 × 9），以及清单总长。
    // 两者决定一个可见格是"有货"还是"空格"（空格是删除目标）。
    std::size_t catalogFirstIndex = 0U;
    std::size_t catalogSize = 0U;
    // 目录滚得动吗（清单不足一屏时滚动条是死的，点它什么都不该发生）。
    bool catalogScrollable = false;
};

// 点一下会发生什么。
//
// `page` 是 `buildContainerPageInto` 装配出来的那一页；命中走 `ui::hitTest`，
// 也就是与菜单屏**同一套**命中规则。
[[nodiscard]] ContainerAction containerClickAction(const Page& page, UiPoint cursor,
                                                   gameplay::InventoryMouseButton button,
                                                   const ContainerViewState& view);

// 光标下那个**玩法槽位**的身份（双击判定与拖拽收集都要它），没有则返回 npos 形式的
// `slotKind = Count`。目录格不是玩法槽位，这里不返回它。
struct ContainerSlotHit final {
    bool hit = false;
    gameplay::SlotKind slotKind = gameplay::SlotKind::PlayerInventory;
    std::uint16_t slotIndex = 0U;

    [[nodiscard]] bool operator==(const ContainerSlotHit&) const = default;
};

[[nodiscard]] ContainerSlotHit containerSlotUnderCursor(const Page& page, UiPoint cursor);

// 这一点是不是"按下即生效"的创造控件（页签、删除框、滚动条、目录格）。
//
// ★ 它存在的理由是快速合成拖拽那套状态机：光标上有东西时，按下**只是开始拖拽**，
//   直到松手才分配。但创造目录的控件必须在按下那一刻就生效（切页签、取货、清空），
//   否则"选中物品再点快捷栏"要点好几下——真实物品槽被有意排除在外，两种游戏模式
//   与创造模式下打开的容器因此走同一套状态机。
[[nodiscard]] bool containerImmediateControlAt(const Page& page, UiPoint cursor);

} // namespace mc::ui
