#include "ui/MenuGeometry.hpp"

#include "ui/CreateWorldLayout.hpp"
#include "ui/HeaderAndFooterLayout.hpp"
#include "ui/KeyBindList.hpp"
#include "ui/ListRow.hpp"
#include "ui/OptionsList.hpp"
#include "ui/PageLayoutKind.hpp"
#include "ui/TextMetrics.hpp"

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

ScrollList keyBindsScrollList(const HudLayout& layout, float framebufferWidth) {
    // UI-6b：行高从自造的 12 改成 26.1 的 **20**（`KeyBindsList.ITEM_HEIGHT`）。
    // 12 塞不下一个 20 高的改键按钮——而 vanilla 那一行正是"名称 + 两个 20 高的按钮"。
    return scrollListOf(keyBindsListBox(layout, framebufferWidth), layout, kKeyBindsRowWidth,
                        kKeyBindRowHeight);
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
UiRect keyBindsListBox(const HudLayout& layout, float framebufferWidth) {
    constexpr int kRowStep = kKeyBindRowHeight;
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

// 一页里有几个按钮。绑定列表那三个行内控件（名称 / 改键 / 重置）不算——它们的矩形
// 来自列表几何，不占按钮网格的位置。
std::size_t countPageButtons(const Page& page) {
    std::size_t buttons = 0;
    for (const Widget& widget : page) {
        if (!isKeyBindRowWidget(widget)) {
            ++buttons;
        }
    }
    return buttons;
}

void layoutPageInto(Page& page, PageId id, const HudLayout& layout, float framebufferWidth,
                    std::size_t keyBindFirstRow, std::size_t optionsFirstRow) {
    // 按钮数从装配结果**数出来**，不是另一张表说的。这就是这两趟拆分的全部意义。
    const std::size_t buttonCount = countPageButtons(page);
    std::size_t buttonIndex = 0;
    std::size_t keyWidgetIndex = 0;
    for (Widget& widget : page) {
        if (isKeyBindRowWidget(widget)) {
            // ★ 控件序号与**屏幕行号**之间不是倍数关系：可见窗口里夹着分类标题行，
            //   它占一行却不产生控件。照 index/每行控件数 折行，标题行之后的每一行
            //   都会偏上一格，而画面上只表现为"名字和按钮错位了一行"。
            const std::size_t row = keyBindWidgetVisibleRow(keyBindFirstRow, keyWidgetIndex,
                                                            kKeyBindWidgetsPerRow);
            switch (keyWidgetIndex % kKeyBindWidgetsPerRow) {
            case 0U:
                widget.rect = keyBindsNameCell(row, layout, framebufferWidth);
                break;
            case 1U:
                widget.rect = keyBindsChangeCell(row, layout, framebufferWidth);
                break;
            default:
                widget.rect = keyBindsResetCell(row, layout, framebufferWidth);
                break;
            }
            ++keyWidgetIndex;
            continue;
        }
        widget.rect = frontendButtonRect(layout, id, buttonIndex, buttonCount, optionsFirstRow);
        ++buttonIndex;
    }
}

UiRect keyBindsRow(std::size_t visibleIndex, const HudLayout& layout, float framebufferWidth) {
    // UI-4：行宽从自造的 300 改成 26.1 的 **340**（`KeyBindsList:59`）。
    // UI-6b：行高不再被压成 11——它就是列表的行高 20，因为一行里要装两个 20 高的按钮。
    return fbRect(layout, scrollListRow(keyBindsScrollList(layout, framebufferWidth),
                                        visibleIndex));
}

// UI-6b：按键绑定行里的两个格子。几何在 [[ui/ListRow.hpp]]，这里只是换算到帧缓冲像素。
//
// 动作名是一段 Label，改键按钮是一个 Button——**一行两个控件**，而不是从前那样
// 整行一个 ListRow。焦点遍历因此会在名称与按钮之间走，与 26.1 的
// `KeyBindsList.KeyEntry.children()` 同义。
UiRect keyBindsNameCell(std::size_t visibleIndex, const HudLayout& layout,
                        float framebufferWidth) {
    const auto list = keyBindsScrollList(layout, framebufferWidth);
    return fbRect(layout, keyBindNameCell(scrollListRow(list, visibleIndex), kFontLineHeight));
}

UiRect keyBindsChangeCell(std::size_t visibleIndex, const HudLayout& layout,
                          float framebufferWidth) {
    const auto list = keyBindsScrollList(layout, framebufferWidth);
    return fbRect(layout, keyBindChangeCell(list, scrollListRow(list, visibleIndex)));
}

UiRect keyBindsResetCell(std::size_t visibleIndex, const HudLayout& layout,
                         float framebufferWidth) {
    const auto list = keyBindsScrollList(layout, framebufferWidth);
    return fbRect(layout, keyBindResetCell(list, scrollListRow(list, visibleIndex)));
}

std::size_t keyBindsVisibleRowCount(float framebufferWidth, float framebufferHeight, int guiScale,
                    bool forceUnicode) {
    const HudLayout layout{framebufferWidth, framebufferHeight, guiScale, forceUnicode};
    const float scale = layout.scale();
    constexpr float kRowStep = static_cast<float>(kKeyBindRowHeight);
    const float rows =
        std::max(keyBindsListBox(layout, framebufferWidth).height / (kRowStep * scale), 1.0F);
    return static_cast<std::size_t>(rows);
}

UiRect keyBindsScrollbarTrack(const HudLayout& layout, float framebufferWidth) {
    return fbRect(layout, scrollListScrollbar(keyBindsScrollList(layout, framebufferWidth)));
}

std::size_t keyBindsScrollIndexFromCursor(const HudLayout& layout, float framebufferWidth,
                                          std::size_t itemCount, std::size_t visibleRows,
                                          float cursorY) {
    static_cast<void>(visibleRows);
    return scrollListRowFromScrollbar(keyBindsScrollList(layout, framebufferWidth), itemCount,
                                      cursorY / layout.scale());
}

// 三段式设置页那张列表的几何。三个滚动条函数与布局都从这一处取，免得视口再有第二份。
namespace {
[[nodiscard]] ScrollList optionsListOf(const HudLayout& layout) {
    return optionsScrollList(
        headerAndFooterLayout(layout.logicalWidth(), layout.logicalHeight()).contentBox());
}
} // namespace

OptionsWindow optionsWindowFor(const HudLayout& layout, PageId page, std::size_t firstRow) {
    if (pageLayoutKind(page) != PageLayoutKind::HeaderFooterList) {
        // 不是三段式列表页：rowCount = 0，约定是"不滚，全装配"。
        return OptionsWindow{};
    }
    const auto list = optionsListOf(layout);
    const std::size_t rows = optionsRowCountOf(page);
    return OptionsWindow{std::min(firstRow, list.maximumFirstRow(rows)), list.visibleRows()};
}

std::size_t optionsMaximumFirstRow(const HudLayout& layout, PageId page) {
    if (pageLayoutKind(page) != PageLayoutKind::HeaderFooterList) {
        return 0U;
    }
    return optionsListOf(layout).maximumFirstRow(optionsRowCountOf(page));
}

UiRect optionsScrollbarTrack(const HudLayout& layout) {
    return fbRect(layout, scrollListScrollbar(optionsListOf(layout)));
}

UiRect optionsScrollbarThumb(const HudLayout& layout, PageId page, std::size_t firstRow) {
    return fbRect(layout, scrollListThumb(optionsListOf(layout), optionsRowCountOf(page),
                                          firstRow));
}

UiRect frontendButtonRect(const HudLayout& layout, PageId page, std::size_t index,
                          std::size_t buttonCount, std::size_t optionsFirstRow) {
    // 版式的**选择**在 ui/PageLayoutKind.hpp 那张表里（不带 default 的 switch，
    // 加一页会被 -Wswitch 点名）；这里只剩每种版式的参数。
    //
    // UI-2：主菜单走 spec §6.3 的版面，也就是逻辑像素上的整数运算（j = H/4 + 48）。
    // 其余屏幕仍走下面那些以帧缓冲像素做浮点的求解器——动 menuButton 会同时移动
    // 暂停页、死亡页与选项页（README 护栏第 4 条），所以那个共用的求解器不动。
    switch (pageLayoutKind(page)) {
    case PageLayoutKind::TitleScreen: {
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
    case PageLayoutKind::BottomBandTwoColumn:
        // vanilla 的 LanguageOptionsScreen 把"强制 Unicode 字体"与"完成"并排放在底部；
        // 世界列表与绑定列表页脚同形（后者是 `LinearLayout.horizontal().spacing(8)`）。
        return layout.bottomMenuButton(index, buttonCount, 2U);
    case PageLayoutKind::BottomBand:
        return layout.bottomMenuButton(index, buttonCount);
    case PageLayoutKind::VideoGrid:
        // 视频页的按钮数已经超出一列能放下的量：各项设置堆进两个居中的列，
        // "完成"单独占下方一行。
        return layout.videoSettingsButton(index, buttonCount);
    case PageLayoutKind::HeaderFooterList: {
        // 26.1 的 OptionsSubScreen：三段式版面里一张 OptionsList 双列，页脚一个按钮。
        const auto frame = headerAndFooterLayout(layout.logicalWidth(), layout.logicalHeight());
        // 最后一个控件是 Done，它在页脚里居中，不在列表里。
        if (buttonCount > 0U && index + 1U == buttonCount) {
            return fbRect(layout, frame.footerButton());
        }
        const auto list = optionsScrollList(frame.contentBox());
        // ★ `index` 是**已装配**控件的序号，不是设置项的序号：滚上去的那些项根本没被
        //   造出来，因此不占序号。这条换算只有 `optionsScrolledSlot` 一处，装配侧
        //   （PageBuilder 的 `optionVisible`）与它走的是同一遍循环。
        const auto slot =
            optionsScrolledSlot(optionsGroupsOf(page), optionsFirstRow, index);
        // addBig 的一项铺满行宽（310），addSmall 的一项是双列里的一格（150）。
        return fbRect(layout, slot.big ? optionsBigCell(list, slot.row)
                                       : optionsSmallCell(list, slot.row, slot.column));
    }
    case PageLayoutKind::HeaderFooterForm: {
        // 26.1 CreateWorldScreen：页眉标题、内容区表单、页脚 Create/Cancel 两个按钮。
        // 装配顺序是「游戏模式 / 难度 / 允许作弊 / 创建 / 返回」，前三个在内容区里
        // 从表单下方往下排，后两个在页脚横排。
        const auto form = createWorldLayout(layout.logicalWidth(), layout.logicalHeight());
        if (buttonCount >= 2U && index + 2U == buttonCount) {
            return fbRect(layout, form.footerLeft);
        }
        if (buttonCount >= 1U && index + 1U == buttonCount) {
            return fbRect(layout, form.footerRight);
        }
        return fbRect(layout, createWorldOptionButton(form, layout.logicalWidth(), index));
    }
    case PageLayoutKind::CentredColumn:
        break;
    }
    return layout.menuButton(index, buttonCount);
}

} // namespace mc::ui
