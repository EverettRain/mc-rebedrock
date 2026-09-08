#pragma once

// 纯粹的前端菜单几何：标题页、世界列表页、语言页与选项页的各个矩形和可见行数
// 从渲染器里抽出来，绘制通道与输入命中测试因此共用同一份不依赖 Vulkan 的来源，而不是各算一遍布局
// 这里的一切都是帧缓冲尺寸、GUI 缩放、当前页面，以及调用方传进来的几个状态标志的函数

#include "ui/HudLayout.hpp"
#include "ui/ScrollList.hpp"
#include "ui/PageStack.hpp"

#include <cstddef>

namespace mc::ui {

// 一个前端页面显示多少个底部按钮或菜单按钮
// 只有在世界打开着的时候，选项页才多出一个难度项
[[nodiscard]] std::size_t menuButtonCount(PageId page, bool worldOpen);

// 标题与底部按钮之间那条带里的一个存档列表行
[[nodiscard]] UiRect worldListRow(std::size_t index, const HudLayout& layout,
                                  float framebufferWidth);

// 当前画布尺寸下，列表带里放得下多少个存档行
// UI-3：`forceUnicode` 参与缩放求解（26.1 `Window.calculateScale`），因此凡是自己构造
// HudLayout 的可见行数助手都要收下它。**故意不给默认值**：漏传一处就少了那次档位调整，
// 而那不会有任何东西变红——让编译器逐个点名。
[[nodiscard]] std::size_t saveListVisibleRowCount(float framebufferWidth, float framebufferHeight,
                                                  int guiScale, bool forceUnicode);

// UI-4：三张滚动列表的统一几何（GUI spec §2.7 / §5 的 L3）。
// 行宽是从 26.1 源码查来的覆写值：语言 270、按键 340、世界列表 270。
// 下面那些逐屏函数都从这里派生，不再各写各的。
[[nodiscard]] ScrollList languageScrollList(const HudLayout& layout, float framebufferWidth);
[[nodiscard]] ScrollList controlsScrollList(const HudLayout& layout, float framebufferWidth);
[[nodiscard]] ScrollList worldScrollList(const HudLayout& layout, float framebufferWidth);
// 世界列表那条带在当前画布下放得下几行（逻辑像素版，saveListVisibleRowCount 的内核）
[[nodiscard]] std::size_t worldListVisibleRows(const HudLayout& layout);

// 灰色警告行的 Y 坐标、整宽的语言框，以及一个语言行
[[nodiscard]] float languageWarningY(const HudLayout& layout);
[[nodiscard]] UiRect languageListBox(const HudLayout& layout, float framebufferWidth);
[[nodiscard]] UiRect languageRow(std::size_t index, const HudLayout& layout,
                                 float framebufferWidth);
[[nodiscard]] std::size_t languageVisibleRowCount(float framebufferWidth, float framebufferHeight,
                                                  int guiScale, bool forceUnicode);
// 绘制与输入共用的滚动条几何，以及光标到行的映射
// 命中轨道比四像素宽的滑块更宽，这与 vanilla 列表控件那条好点的边槽一致，视觉上仍然窄
[[nodiscard]] UiRect languageScrollbarTrack(const HudLayout& layout, float framebufferWidth);
[[nodiscard]] UiRect languageScrollbarThumb(const HudLayout& layout, float framebufferWidth,
                                             std::size_t itemCount,
                                             std::size_t visibleRows,
                                             std::size_t firstIndex);
[[nodiscard]] std::size_t languageScrollIndexFromCursor(
    const HudLayout& layout, float framebufferWidth, std::size_t itemCount,
    std::size_t visibleRows, float cursorY);

// 按键设置页的绑定列表是一个滚动列表，与世界列表和语言列表同类，而不是固定的按钮网格
// 24 个可重绑的动作会冲破 20 个按钮的菜单上限并抛出，所以它必须是滚动的
// 几何照搬语言列表：标题与底部按钮带之间一个按内容定尺寸的框，每个可见动作一行，外加一条滚动条
[[nodiscard]] UiRect controlsListBox(const HudLayout& layout, float framebufferWidth);
// 一页里第 `widgetIndex` 个控件的矩形。**页面 → 矩形这件事只有这一处。**
//
// ★ 它存在的理由是一次真实的崩溃：UI-6b 把按键绑定行拆成两个控件之后，绘制侧与
//   输入侧**各有一份**同样的映射 lambda（`buildDrawPage` 与 `menuRectProvider`），
//   我只改了绘制侧。于是点 Controls 底部任何一个按钮，输入侧把列表后半段的序号
//   当成按钮序号，`buttonIndex` 越过 buttonCount，`bottomMenuButton` 抛
//   `menu button index or count is invalid` —— 直接闪退。
//
//   两份镜像的代码不是"两处要同步"，是"迟早会不同步"。收成一个函数之后，
//   `menu_layout` 测的就是**生产代码本身**，而不是它在测试里的一份抄本。
[[nodiscard]] UiRect menuWidgetRect(PageId page, std::size_t widgetIndex,
                                    const HudLayout& layout, float framebufferWidth,
                                    std::size_t buttonCount, std::size_t keyBindRowCount);

// UI-6b：按键绑定行里的两个格子（动作名 / 改键按钮）。一行两个控件，
// 几何在 ui/ListRow.hpp，这两个只是换算到帧缓冲像素。
[[nodiscard]] UiRect controlsNameCell(std::size_t visibleIndex, const HudLayout& layout,
                                      float framebufferWidth);
[[nodiscard]] UiRect controlsChangeCell(std::size_t visibleIndex, const HudLayout& layout,
                                        float framebufferWidth);
[[nodiscard]] UiRect controlsRow(std::size_t visibleIndex, const HudLayout& layout,
                                 float framebufferWidth);
[[nodiscard]] std::size_t controlsVisibleRowCount(float framebufferWidth, float framebufferHeight,
                                                  int guiScale, bool forceUnicode);
[[nodiscard]] UiRect controlsScrollbarTrack(const HudLayout& layout, float framebufferWidth);
[[nodiscard]] std::size_t controlsScrollIndexFromCursor(const HudLayout& layout,
                                                        float framebufferWidth,
                                                        std::size_t itemCount,
                                                        std::size_t visibleRows, float cursorY);

// 各前端页面共用的按钮几何
// 存档、编辑、删除与语言页贴底摆放，视频设置页分两列，其余按居中菜单摆放
[[nodiscard]] UiRect frontendButtonRect(const HudLayout& layout, PageId page, std::size_t index,
                                        std::size_t buttonCount);

} // namespace mc::ui
