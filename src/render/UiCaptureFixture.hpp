#pragma once

// A0-0：容器界面截图的**确定性内容**。
//
// 界面截图通道此前只拍得到前端页面与游戏内 HUD（偏差表 D14 的余项）。容器界面拍不到，
// 原因有两条，缺一不可：
//   1. 拍摄目标的类型是 `PageId`，而容器屏不是 PageId（见 UiCaptureTarget）；
//   2. **屏上的东西全部来自 `clientMirror.world()` 这份逐 tick 世界快照**，而截图
//      通道不启动模拟线程——镜像永远停在默认值上，于是每一个槽位都是空的。一张全空
//      的背包证明不了任何"1:1"，因为物品图标、堆叠数字、耐久条、附魔提示框
//      一条都没进画。
//
// 这里补的是第二条：一份**只由目标决定**的世界/玩家快照。它经由生产的编解码通道
// （`net::makeLoopbackPair` + `ClientMirror::pump`，与 RN 的阴影导出同一条路）注入，
// 所以镜像的写入者仍然只有那一个，不需要给 ClientMirror 开后门。
//
// 内容的挑选不是随手填的：每一样都要让某一条**绘制路径**进画，否则拍到的图对那条
// 路径什么都没说。
//   - 堆叠数 > 1 → 槽位右下角的数字（drawHudText 的右对齐分支）
//   - `damage > 0` → 耐久条（drawDurabilityBar）
//   - 带附魔的物品 → 提示框的多行与配色（ui::itemTooltipLines + tooltipLineColor）
//   - 方块与物品各有 → 方块图标与物品图标是**两条管线**（hudBlockIconPipeline 与
//     hud 的图集分支），只填一类会漏掉另一类
//   - 熔炉的两条进度、附魔台的三条选项与线索、铁砧的价格 → 各自的条形与数字
//
// ★ 不填光标堆（`cursorStack`）：非空的光标堆会**抑制提示框**、并且画在光标位置上，
//   而截图把光标钉在画布外（kUiCaptureCursorX/Y）。填了它等于用一个看不见的东西
//   换掉一整块看得见的提示框。

#include "gameplay/PlayerTickSnapshot.hpp"
#include "gameplay/WorldSnapshot.hpp"
#include "persistence/SaveRepository.hpp"
#include "render/UiCapture.hpp"

#include <cstddef>
#include <vector>

namespace mc::render {

// 这个目标要拍的那份世界快照。非容器目标返回默认值（一份空快照），与 A0-0 之前
// 的行为逐字节一致——这是既有十八屏基线不受影响的原因。
// `carryStack` = 光标上拿着一堆东西（26.1 的 `getCarried()`）。
// ★ 它是一个参数而不是夹具的固定内容：手上拿着东西会**抑制提示框**（vanilla 的规则），
//   把它写死就再也拍不到"悬停且手上是空的"那一档——而那是二十张常规基线的那一档。
[[nodiscard]] gameplay::WorldSnapshot uiCaptureWorldSnapshot(const UiCaptureTarget& target,
                                                             bool carryStack = false);

// 配套的玩家快照：游戏模式、选中的快捷栏格、以及生存状态条读的那几个量。
// ★ 游戏模式在这里，不在世界快照里——创造背包这一档是 `PlayerInventory + Creative`
//   两个事实的合取，而它们分别住在两份快照里，这是生产路径本来的形状。
[[nodiscard]] gameplay::PlayerTickSnapshot uiCapturePlayerSnapshot(const UiCaptureTarget& target);

// UI-11 / A6：世界列表那三屏（列表 / 编辑 / 删除确认）的确定性存档清单。
//
// ★ 立它的理由与容器夹具完全同族：**世界列表此前拍出来永远是"No worlds yet"**，
//   而"一行长什么样"正是 A6 要改的东西——一张空列表对它什么都没说。
//
// 内容同样是按**绘制路径**挑的，不是随手填的：
//   - 第 0 行有 `lastPlayedUnixSeconds` → 第二行走 `<目录名> (<日期>)` 那一支
//   - 第 1 行 `lastPlayedUnixSeconds == 0` → 走 26.1 `lastPlayed != -1L` 的**否定**支
//     （只有目录名，没有括号）
//   - 第 2 行名字长到超过 231 逻辑像素 → 走裁剪那一支（`StringWidget` 的 CLAMPED）
//   - 选中行钉在第 1 行 → 一张图里同时有"选中的行"与"没选中的行"两种底
//
// 非世界列表的目标返回空清单，既有基线因此逐字节不变。
[[nodiscard]] std::vector<persistence::SaveSummary> uiCaptureSaveSummaries(
    const UiCaptureTarget& target);

// 上面那份清单里，哪一行是选中的。空清单时是 npos。
[[nodiscard]] std::size_t uiCaptureSelectedWorldRow(const UiCaptureTarget& target);

// 第 0 个存档那张 64x64 的缩略图（`hasIcon` 为真的那一行画的就是它）。
//
// ★ 它**过一遍生产的缩放函数** `render::worldIconFromFrame`，而不是直接画一张
//   64x64 的图案：那样拍到的图既证明缩略图这条读取路径通了，也证明裁剪与缩放
//   没把画面弄反——与容器夹具走 net::makeLoopbackPair 而不是给镜像开后门同理。
[[nodiscard]] std::vector<std::uint8_t> uiCaptureWorldIcon();

} // namespace mc::render
