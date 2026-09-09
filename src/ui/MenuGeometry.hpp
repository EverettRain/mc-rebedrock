#pragma once

// 纯粹的前端菜单几何：标题页、世界列表页、语言页与选项页的各个矩形和可见行数
// 从渲染器里抽出来，绘制通道与输入命中测试因此共用同一份不依赖 Vulkan 的来源，而不是各算一遍布局
// 这里的一切都是帧缓冲尺寸、GUI 缩放、当前页面，以及调用方传进来的几个状态标志的函数

#include "ui/HudLayout.hpp"
#include "ui/HeaderAndFooterLayout.hpp"
#include "ui/OptionsList.hpp"
#include "ui/ScrollList.hpp"
#include "ui/MenuSystem.hpp"
#include "ui/NoticeScreen.hpp"
#include "ui/PageStack.hpp"
#include "ui/Widget.hpp"

#include <cstddef>

namespace mc::ui {

// 一个前端页面显示多少个底部按钮或菜单按钮
// 只有在世界打开着的时候，选项页才多出一个难度项


// 标题与底部按钮之间那条带里的一个存档列表行
[[nodiscard]] UiRect worldListRow(std::size_t index, const HudLayout& layout);

// 当前画布尺寸下，列表带里放得下多少个存档行
// UI-3：`forceUnicode` 参与缩放求解（26.1 `Window.calculateScale`），因此凡是自己构造
// HudLayout 的可见行数助手都要收下它。**故意不给默认值**：漏传一处就少了那次档位调整，
// 而那不会有任何东西变红——让编译器逐个点名。
[[nodiscard]] std::size_t saveListVisibleRowCount(float framebufferWidth, float framebufferHeight,
                                                  int guiScale, bool forceUnicode);

// UI-4：三张滚动列表的统一几何（GUI spec §2.7 / §5 的 L3）。
// 行宽是从 26.1 源码查来的覆写值：语言 270、按键 340、世界列表 270。
// 下面那些逐屏函数都从这里派生，不再各写各的。
[[nodiscard]] ScrollList languageScrollList(const HudLayout& layout);
[[nodiscard]] ScrollList keyBindsScrollList(const HudLayout& layout);
[[nodiscard]] ScrollList worldScrollList(const HudLayout& layout);
// 世界列表那条带在当前画布下放得下几行（逻辑像素版，saveListVisibleRowCount 的内核）
[[nodiscard]] std::size_t worldListVisibleRows(const HudLayout& layout);

// 灰色警告行的 Y 坐标、整宽的语言框，以及一个语言行
[[nodiscard]] float languageWarningY(const HudLayout& layout);
[[nodiscard]] UiRect languageListBox(const HudLayout& layout);
[[nodiscard]] UiRect languageRow(std::size_t index, const HudLayout& layout);
[[nodiscard]] std::size_t languageVisibleRowCount(float framebufferWidth, float framebufferHeight,
                                                  int guiScale, bool forceUnicode);
// 绘制与输入共用的滚动条几何，以及光标到行的映射
// 命中轨道比四像素宽的滑块更宽，这与 vanilla 列表控件那条好点的边槽一致，视觉上仍然窄
[[nodiscard]] UiRect languageScrollbarTrack(const HudLayout& layout);
[[nodiscard]] UiRect languageScrollbarThumb(const HudLayout& layout,
                                             std::size_t itemCount,
                                             std::size_t visibleRows,
                                             std::size_t firstIndex);
[[nodiscard]] std::size_t languageScrollIndexFromCursor(
    const HudLayout& layout, std::size_t itemCount,
    std::size_t visibleRows, float cursorY);

// 按键设置页的绑定列表是一个滚动列表，与世界列表和语言列表同类，而不是固定的按钮网格
// 24 个可重绑的动作会冲破 20 个按钮的菜单上限并抛出，所以它必须是滚动的
// 几何照搬语言列表：标题与底部按钮带之间一个按内容定尺寸的框，每个可见动作一行，外加一条滚动条
[[nodiscard]] UiRect keyBindsListBox(const HudLayout& layout);
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
// 把一页**已经装配好**的控件逐个填上矩形。
//
// ★ 这是"页面有几个按钮"这个事实的**唯一**来源：它从 `page` 里数出来，而不是另有
//   一张表说这一页有几个。从前那张表（`menuButtonCount`）与页面装配器是同一个事实的
//   两份表述，没有任何东西保证一致——UI-6c 把 Controls 从 4 个按钮改成 9 个时要手改
//   两处，而漏改的症状是 `frontendButtonRect` 抛 out_of_range **闪退**（与上一轮那次
//   崩溃同族）。更糟的是那张表带 `default: return 0`，把 -Wswitch 护栏也关掉了：
//   加一页会静默返回 0，而 0 正好是抛异常的条件。
//
// 它也解开了原来那个鸡生蛋：装配需要矩形、矩形需要按钮数、按钮数需要知道装配了什么。
// 分成两趟之后，装配只管"有哪些控件"，布局只管"它们在哪"。
//
// `keyBindFirstRow` 是绑定列表的滚动位置（那是屏幕状态，不是页面内容），
// 只有 `PageId::KeyBinds` 会读它；`optionsFirstRow` 同理，只有三段式设置页会读它。
//
// ★ 这两个滚动位置必须与**装配**用的那一个是同一个值。装配按窗口跳过控件、布局按同一
//   个 firstRow 折算行号，两边错开一行就是"名字和控件错位"或者"滚动条动了内容不动"。
// ★ `createWorldTab` 同理：它是**屏幕状态**，而装配与布局必须读同一个值——
//   装配按当前页造控件、布局按同一页算矩形，两边不同步就是"点 A 触发 B"。
// ★ `noticeMetrics` 同理：提示屏的内容列宽是 `max(标题宽, 正文格宽, 页脚宽)`，
//    而标题宽与复选框文字宽要量字体——`ui::` 这一层没有字体，所以由调用方量好传进来
//    （生产路径两处都从 `MenuBuildContext::noticeMetrics` 取同一个值）。
void layoutPageInto(Page& page, PageId id, const HudLayout& layout,
                    std::size_t keyBindFirstRow = 0U, std::size_t optionsFirstRow = 0U,
                    CreateWorldTab createWorldTab = CreateWorldTab::Game,
                    const NoticeMetrics& noticeMetrics = {});

// 一页里有几个**按钮**（不含绑定列表那些行内控件）。布局用它，测试也用它断言页面形状。
[[nodiscard]] std::size_t countPageButtons(const Page& page);

// UI-6b：按键绑定行里的两个格子（动作名 / 改键按钮）。一行两个控件，
// 几何在 ui/ListRow.hpp，这两个只是换算到帧缓冲像素。
[[nodiscard]] UiRect keyBindsNameCell(std::size_t visibleIndex, const HudLayout& layout);
[[nodiscard]] UiRect keyBindsChangeCell(std::size_t visibleIndex, const HudLayout& layout);
[[nodiscard]] UiRect keyBindsResetCell(std::size_t visibleIndex, const HudLayout& layout);
[[nodiscard]] UiRect keyBindsRow(std::size_t visibleIndex, const HudLayout& layout);
[[nodiscard]] std::size_t keyBindsVisibleRowCount(float framebufferWidth, float framebufferHeight,
                                                  int guiScale, bool forceUnicode);
[[nodiscard]] UiRect keyBindsScrollbarTrack(const HudLayout& layout);
[[nodiscard]] std::size_t keyBindsScrollIndexFromCursor(const HudLayout& layout,
                                                        std::size_t itemCount,
                                                        std::size_t visibleRows, float cursorY);

// 各前端页面共用的按钮几何
// 存档、编辑、删除与语言页贴底摆放，视频设置页分两列，其余按居中菜单摆放
[[nodiscard]] UiRect frontendButtonRect(const HudLayout& layout, PageId page, std::size_t index,
                                        std::size_t buttonCount,
                                        std::size_t optionsFirstRow = 0U,
                                        CreateWorldTab createWorldTab = CreateWorldTab::Game);

// UI-6d：三段式设置页的滚动窗口。装配（PageBuilder）与布局（frontendButtonRect）
// 必须读同一个窗口，所以它只有这一处来源。
//
// 非三段式的页面返回 `{0, 0}`——`rowCount == 0` 的约定是"不滚，全装配"。
[[nodiscard]] OptionsWindow optionsWindowFor(const HudLayout& layout, PageId page,
                                             std::size_t firstRow);
// 这一页的三段式版面。★ **绘制侧与布局侧都要走它**：带副页眉的页面（Options）
// 页眉更高（49 而不是 33），内容区相应变矮。两处各自造 frame 的后果是副页眉的控件
// 压在页眉分隔线上——实测如此。
[[nodiscard]] HeaderAndFooterLayout optionsFrame(const HudLayout& layout, PageId page);
// 这一页的设置列表最多能滚到第几行（再往下滚只会露出列表末尾之后的空白）。
[[nodiscard]] std::size_t optionsMaximumFirstRow(const HudLayout& layout, PageId page);
// 设置列表的滚动条轨道；只有真的滚得动才画（`rowCount` 覆盖不了所有行时）。
[[nodiscard]] UiRect optionsScrollbarTrack(const HudLayout& layout, PageId page);
[[nodiscard]] UiRect optionsScrollbarThumb(const HudLayout& layout, PageId page,
                                           std::size_t firstRow);
// UI-6e ⑤（D18）：拖设置列表的滚动条——光标位置 → 首行。
[[nodiscard]] std::size_t optionsScrollIndexFromCursor(const HudLayout& layout, PageId page,
                                                       float cursorY);

} // namespace mc::ui
