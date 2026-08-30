#pragma once

// 纯粹的前端菜单几何：标题页、世界列表页、语言页与选项页的各个矩形和可见行数
// 从渲染器里抽出来，绘制通道与输入命中测试因此共用同一份不依赖 Vulkan 的来源，而不是各算一遍布局
// 这里的一切都是帧缓冲尺寸、GUI 缩放、当前页面，以及调用方传进来的几个状态标志的函数

#include "ui/HudLayout.hpp"
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
[[nodiscard]] std::size_t saveListVisibleRowCount(float framebufferWidth, float framebufferHeight,
                                                  int guiScale);

// 灰色警告行的 Y 坐标、整宽的语言框，以及一个语言行
[[nodiscard]] float languageWarningY(const HudLayout& layout);
[[nodiscard]] UiRect languageListBox(const HudLayout& layout, float framebufferWidth);
[[nodiscard]] UiRect languageRow(std::size_t index, const HudLayout& layout,
                                 float framebufferWidth);
[[nodiscard]] std::size_t languageVisibleRowCount(float framebufferWidth, float framebufferHeight,
                                                  int guiScale);
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
[[nodiscard]] UiRect controlsRow(std::size_t visibleIndex, const HudLayout& layout,
                                 float framebufferWidth);
[[nodiscard]] std::size_t controlsVisibleRowCount(float framebufferWidth, float framebufferHeight,
                                                  int guiScale);
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
