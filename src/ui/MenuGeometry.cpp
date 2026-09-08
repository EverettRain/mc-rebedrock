#include "ui/MenuGeometry.hpp"

#include "ui/ScrollList.hpp"
#include "ui/TitleScreenLayout.hpp"

#include <algorithm>
#include <cmath>

namespace mc::ui {
namespace {

// UI-3：与 HudLayout 私有助手同一套规则——版面在**逻辑像素整数网格**上解，最后一次乘
// scale 回到帧缓冲像素。spec §1.2：所有居中都是整数除法，否则与原版差 1px。
[[nodiscard]] float toFb(const HudLayout& layout, int logical) {
    return static_cast<float>(logical) * layout.scale();
}

// 一段 fb 像素长度回到逻辑像素。只用于把既有的 `box` 矩形接回整数网格——那些矩形
// 本身已由整数逻辑锚点算出，所以这次除法是精确的。
[[nodiscard]] int toLogical(const HudLayout& layout, float framebuffer) {
    return static_cast<int>(std::lround(framebuffer / layout.scale()));
}

// 世界列表那条带：首行顶边与行距，逻辑像素。
constexpr int kWorldListTop = 34;
constexpr int kWorldListRowStep = 22;

// UI-4：三张滚动列表都从这里取几何。行宽是从 26.1 源码查来的覆写值，不是估的：
// 语言 270（`LanguageSelectScreen:141` 的 `220 + 50`）、按键 340（`KeyBindsList:59`）、
// 世界列表 270（`WorldSelectionList:251`）。
[[nodiscard]] ScrollList scrollListOf(const UiRect& viewportFb, const HudLayout& layout,
                                      int rowWidth, int rowHeight) {
    return ScrollList{
        toLogical(layout, viewportFb.x),      toLogical(layout, viewportFb.y),
        toLogical(layout, viewportFb.width),  toLogical(layout, viewportFb.height),
        rowWidth,                             rowHeight,
    };
}

// 一个逻辑像素矩形回到帧缓冲像素。
[[nodiscard]] UiRect fbRect(const HudLayout& layout, const UiRect& logical) {
    return {toFb(layout, static_cast<int>(logical.x)), toFb(layout, static_cast<int>(logical.y)),
            toFb(layout, static_cast<int>(logical.width)),
            toFb(layout, static_cast<int>(logical.height))};
}

} // namespace

ScrollList languageScrollList(const HudLayout& layout, float framebufferWidth) {
    return scrollListOf(languageListBox(layout, framebufferWidth), layout, kLanguageRowWidth, 22);
}

ScrollList controlsScrollList(const HudLayout& layout, float framebufferWidth) {
    return scrollListOf(controlsListBox(layout, framebufferWidth), layout, kKeyBindsRowWidth, 12);
}

ScrollList worldScrollList(const HudLayout& layout, float framebufferWidth) {
    static_cast<void>(framebufferWidth);
    // 世界列表没有一个显式的"框"：它就是标题与底部按钮带之间那条带。
    return ScrollList{0,
                      kWorldListTop,
                      layout.logicalWidth(),
                      static_cast<int>(worldListVisibleRows(layout)) * kWorldListRowStep,
                      kWorldSelectionRowWidth,
                      kWorldListRowStep};
}

std::size_t menuButtonCount(PageId page, bool worldOpen) {
    switch (page) {
    case PageId::Title:
        // UI-2：26.1 的主菜单是七个可点控件（spec §6.3 的伪 XML 布局树，几何按
        // TitleScreen.init 核对过）——单人、多人、Realms、语言图标、选项、退出、无障碍图标
        return kTitleWidgetCount;
    case PageId::WorldList:
        return 4U;
    case PageId::CreateWorld:
        // 游戏模式、允许作弊、创建世界、返回
        return 4U;
    case PageId::EditWorld:
        return 3U;
    case PageId::ConfirmDelete:
        return 2U;
    case PageId::Options:
        // 没有打开世界时少一个按钮，因为不显示难度项
        // 字幕开关也在这一页，所以这里的计数比早先各多一个
        return worldOpen ? 8U : 7U;
    case PageId::Experimental:
        return 5U;
    case PageId::VideoSettings:
        // RN-23 起多一个：实体阴影开关。26.1 把它放在视频设置里
        // （VideoSettingsScreen.java:51），不在实验性内容里
        return 12U;
    case PageId::Controls:
        // 只数底部那条按钮带，即视角摇晃、自动跳跃、重置、完成
        // 上方那 24 个按键绑定行属于滚动列表而不是菜单按钮，因此不计入按钮上限
        return 4U;
    case PageId::Language:
        return 2U;
    case PageId::Pause:
        return 3U;
    case PageId::Death:
        return 2U;
    default:
        return 0U;
    }
}

UiRect worldListRow(std::size_t index, const HudLayout& layout, float framebufferWidth) {
    // UI-4：走统一的 ScrollList。行宽从自造的 300 改成 26.1 的 **270**
    // （`WorldSelectionList:251`）；行高仍比行距矮 2，那 2 像素是行与行之间的缝。
    const auto list = worldScrollList(layout, framebufferWidth);
    auto row = fbRect(layout, scrollListRow(list, index));
    row.height = toFb(layout, kWorldListRowStep - 2);
    return row;
}

std::size_t worldListVisibleRows(const HudLayout& layout) {
    // 世界列表那四个功能按钮排成两列各两个，整块因此在底部带上正好占两行
    constexpr int kButtonRows = 2;
    constexpr int kButtonHeight = 20;
    constexpr int kButtonStep = 24;
    constexpr int kBottomMargin = 16;   // canvas bottom to last button's bottom
    constexpr int kListToButtonGap = 12;
    // ceil 后的逻辑画布（spec §1.1），不是精确的 fb/scale
    const int buttonBlockTop =
        layout.logicalHeight() - kBottomMargin - kButtonHeight - (kButtonRows - 1) * kButtonStep;
    const int available = buttonBlockTop - kListToButtonGap - kWorldListTop;
    return static_cast<std::size_t>(std::max(available / kWorldListRowStep, 1));
}

std::size_t saveListVisibleRowCount(float framebufferWidth, float framebufferHeight, int guiScale,
                    bool forceUnicode) {
    return worldListVisibleRows(
        HudLayout{framebufferWidth, framebufferHeight, guiScale, forceUnicode});
}

float languageWarningY(const HudLayout& layout) {
    const float scale = layout.scale();
    const auto firstButton = layout.bottomMenuButton(0U, 2U, 2U);
    return firstButton.y - 16.0F * scale;
}

UiRect languageListBox(const HudLayout& layout, float framebufferWidth) {
    static_cast<void>(framebufferWidth);
    constexpr int kRowStep = 22;
    constexpr int kTopBound = 44;
    const int bottomBound = toLogical(layout, languageWarningY(layout)) - 8;
    // 高度按内容定：带里放得下几行就是几行
    const int rows = std::max((bottomBound - kTopBound) / kRowStep, 1);
    const int height = rows * kRowStep;
    const int top = kTopBound + (bottomBound - kTopBound - height) / 2;
    return {0.0F, toFb(layout, top), toFb(layout, layout.logicalWidth()),
            toFb(layout, height)};
}

UiRect languageRow(std::size_t index, const HudLayout& layout, float framebufferWidth) {
    // UI-4：走统一的 ScrollList。行宽 270 与 26.1 一致（本来就对），
    // 变的是滚动条与选中高亮，见 languageScrollbarTrack。
    const auto list = languageScrollList(layout, framebufferWidth);
    auto row = fbRect(layout, scrollListRow(list, index));
    row.height = toFb(layout, 20);
    return row;
}

std::size_t languageVisibleRowCount(float framebufferWidth, float framebufferHeight, int guiScale,
                    bool forceUnicode) {
    const HudLayout layout{framebufferWidth, framebufferHeight, guiScale, forceUnicode};
    const float scale = layout.scale();
    constexpr float kRowStep = 22.0F;
    const float rows = std::max(languageListBox(layout, framebufferWidth).height / (kRowStep * scale),
                                1.0F);
    return static_cast<std::size_t>(rows);
}

UiRect languageScrollbarTrack(const HudLayout& layout, float framebufferWidth) {
    // UI-4：26.1 把滚动条贴在**行的右缘**再留 2 像素（`AbstractSelectionList.scrollBarX`），
    // 宽 6。从前这里是自造的 10 宽轨道，按"中线右 144"摆——那是照 spec §2.7 那句
    // 「贴在视口右侧」猜的，而 spec 那一条是 1.20.2 之前的状态。
    return fbRect(layout, scrollListScrollbar(languageScrollList(layout, framebufferWidth)));
}

UiRect languageScrollbarThumb(const HudLayout& layout, float framebufferWidth,
                              std::size_t itemCount, std::size_t visibleRows,
                              std::size_t firstIndex) {
    static_cast<void>(visibleRows);
    // UI-4：滑块与轨道同宽（6），高度走 26.1 的 `clamp(h*h/内容高, 32, h-8)`。
    // 从前是"轨道 10 宽、滑块 4 宽居中、最小高 8"——三个数都是自造的。
    return fbRect(layout, scrollListThumb(languageScrollList(layout, framebufferWidth), itemCount,
                                          firstIndex));
}

std::size_t languageScrollIndexFromCursor(const HudLayout& layout, float framebufferWidth,
                                          std::size_t itemCount, std::size_t visibleRows,
                                          float cursorY) {
    static_cast<void>(visibleRows);
    return scrollListRowFromScrollbar(languageScrollList(layout, framebufferWidth), itemCount,
                                      cursorY / layout.scale());
}

// 按键设置页的绑定列表
// 框体位于标题与底部按钮带之间，后者是视角摇晃、自动跳跃、重置、完成
// 几何照搬语言列表，区别是这里要给两行底部按钮留位置，而不是给一行警告文字
UiRect controlsListBox(const HudLayout& layout, float framebufferWidth) {
    constexpr int kRowStep = 12;
    constexpr int kTopBound = 40;
    // 列表在底部按钮带上方结束
    // 带的顶行由四个底部按钮中的第一个推出来，两列即两行，与 languageWarningY 读取带位置的方式相同
    // 两处都不需要一个专门的高度取值函数
    const int bandTop = toLogical(layout, layout.bottomMenuButton(0U, 4U, 2U).y);
    const int bottomBound = bandTop - 12;
    const int rows = std::max((bottomBound - kTopBound) / kRowStep, 1);
    const int height = rows * kRowStep;
    static_cast<void>(framebufferWidth);
    return {0.0F, toFb(layout, kTopBound), toFb(layout, layout.logicalWidth()),
            toFb(layout, height)};
}

UiRect controlsRow(std::size_t visibleIndex, const HudLayout& layout, float framebufferWidth) {
    // UI-4：行宽从自造的 300 改成 26.1 的 **340**（`KeyBindsList:59`）。
    const auto list = controlsScrollList(layout, framebufferWidth);
    auto row = fbRect(layout, scrollListRow(list, visibleIndex));
    row.height = toFb(layout, 11);
    return row;
}

std::size_t controlsVisibleRowCount(float framebufferWidth, float framebufferHeight, int guiScale,
                    bool forceUnicode) {
    const HudLayout layout{framebufferWidth, framebufferHeight, guiScale, forceUnicode};
    const float scale = layout.scale();
    constexpr float kRowStep = 12.0F;
    const float rows =
        std::max(controlsListBox(layout, framebufferWidth).height / (kRowStep * scale), 1.0F);
    return static_cast<std::size_t>(rows);
}

UiRect controlsScrollbarTrack(const HudLayout& layout, float framebufferWidth) {
    return fbRect(layout, scrollListScrollbar(controlsScrollList(layout, framebufferWidth)));
}

std::size_t controlsScrollIndexFromCursor(const HudLayout& layout, float framebufferWidth,
                                          std::size_t itemCount, std::size_t visibleRows,
                                          float cursorY) {
    static_cast<void>(visibleRows);
    return scrollListRowFromScrollbar(controlsScrollList(layout, framebufferWidth), itemCount,
                                      cursorY / layout.scale());
}

UiRect frontendButtonRect(const HudLayout& layout, PageId page, std::size_t index,
                          std::size_t buttonCount) {
    // UI-2：主菜单走 spec §6.3 的版面，也就是逻辑像素上的整数运算（j = H/4 + 48）。
    // 其余屏幕仍走下面那些以帧缓冲像素做浮点的求解器——动 menuButton 会同时移动
    // 暂停页、死亡页与选项页（README 护栏第 4 条），所以这里只加分支，不改共用的那个。
    if (page == PageId::Title) {
        const float scale = layout.scale();
        const auto title =
            titleScreenLayout(layout.logicalWidth(), layout.logicalHeight(), 0, 0);
        const TitleRect rect = titleWidgetRect(title, index);
        return {
            static_cast<float>(rect.x) * scale,
            static_cast<float>(rect.y) * scale,
            static_cast<float>(rect.width) * scale,
            static_cast<float>(rect.height) * scale,
        };
    }
    if (page == PageId::WorldList) {
        return layout.bottomMenuButton(index, buttonCount, 2U);
    }
    // 按键设置页的底部带分两列，即视角摇晃、自动跳跃、重置、完成
    // 上方的按键绑定行走 controlsRow 那套滚动列表矩形，绝不走这个按钮网格
    if (page == PageId::Controls) {
        return layout.bottomMenuButton(index, buttonCount, 2U);
    }
    // 视频页的按钮数已经超出一列能放下的量
    // 它的各项设置堆进两个居中的列，"完成"单独占下方一行
    if (page == PageId::VideoSettings) {
        return layout.videoSettingsButton(index, buttonCount);
    }
    if (page == PageId::EditWorld || page == PageId::ConfirmDelete) {
        return layout.bottomMenuButton(index, buttonCount);
    }
    // vanilla 的 LanguageOptionsScreen 把"强制 Unicode 字体"与"完成"并排放在底部，而不是上下堆叠
    if (page == PageId::Language) {
        return layout.bottomMenuButton(index, buttonCount, 2U);
    }
    return layout.menuButton(index, buttonCount);
}

} // namespace mc::ui
