#include "ui/MenuGeometry.hpp"

#include "ui/ListRow.hpp"
#include "ui/WorldListRow.hpp"

#include "ui/CreateWorldLayout.hpp"
#include "ui/DualColumnList.hpp"
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
// UI-11 / A6：26.1 `WorldSelectionList` 的 `itemHeight` 是 **36**（:116），
// 不是本作从前自造的 22——那一行放不下 32x32 的缩略图，也放不下三行字。
constexpr int kWorldListRowStep = kWorldRowHeight;

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

ScrollList languageScrollList(const HudLayout& layout) {
    return scrollListOf(languageListBox(layout), layout, kLanguageRowWidth, 22);
}

ScrollList keyBindsScrollList(const HudLayout& layout) {
    // UI-6b：行高从自造的 12 改成 26.1 的 **20**（`KeyBindsList.ITEM_HEIGHT`）。
    // 12 塞不下一个 20 高的改键按钮——而 vanilla 那一行正是"名称 + 两个 20 高的按钮"。
    return scrollListOf(keyBindsListBox(layout), layout, kKeyBindsRowWidth,
                        kKeyBindRowHeight);
}

ScrollList worldScrollList(const HudLayout& layout) {
    // 世界列表没有一个显式的"框"：它就是标题与底部按钮带之间那条带。
    return ScrollList{0,
                      kWorldListTop,
                      layout.logicalWidth(),
                      static_cast<int>(worldListVisibleRows(layout)) * kWorldListRowStep,
                      kWorldSelectionRowWidth,
                      kWorldListRowStep};
}


// UI-11 / A6：世界行在**逻辑像素**下的矩形。行内那几块（缩略图、三行字）都从它派生，
// 所以它只有这一处；`worldListRow` 是它换算到帧缓冲像素的那一层。
// UI-13：世界列表那条带（列表视口）的矩形，帧缓冲像素。
//
// ★ 它存在的理由是一个**现场可见**的缺陷：绘制侧此前自己算了一份
//   `visibleRows * 22 + 8`，而 A6 把行距改成了 36。于是底衬与上下两条分隔线仍按
//   22 一行算高，带比内容矮了近四成——列表下缘那条线**穿过第五行的中间**，
//   后面的行画在带外面（现场截图 export/savelist-problem.png）。
//   「一份几何两处表述」的老形状：行距在 ScrollList 里，带高在绘制侧手抄。
//   现在两者都从 `worldScrollList` 派生，行距改了带高跟着改。
UiRect worldListBox(const HudLayout& layout) {
    const auto list = worldScrollList(layout);
    return {0.0F, toFb(layout, list.y), toFb(layout, list.width), toFb(layout, list.height)};
}

UiRect logicalWorldListRow(std::size_t index, const HudLayout& layout) {
    return scrollListRow(worldScrollList(layout), index);
}

UiRect worldListRow(std::size_t index, const HudLayout& layout) {
    // UI-4：走统一的 ScrollList。行宽从自造的 300 改成 26.1 的 **270**
    // （`WorldSelectionList:251`）；行高仍比行距矮 2，那 2 像素是行与行之间的缝。
    // UI-11 / A6：行**就是** itemHeight 那么高。26.1 的行与行之间没有缝：
    // 视觉上的间隔来自 `Entry.getContentY/Height` 上下各让出的 2 像素
    // （:475-481），那份内缩在 ui::worldRowParts 里。从前这里减 2 是自造的缝。
    return fbRect(layout, logicalWorldListRow(index, layout));
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

UiRect languageListBox(const HudLayout& layout) {
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

UiRect languageRow(std::size_t index, const HudLayout& layout) {
    // UI-4：走统一的 ScrollList。行宽 270 与 26.1 一致（本来就对），
    // 变的是滚动条与选中高亮，见 languageScrollbarTrack。
    const auto list = languageScrollList(layout);
    auto row = fbRect(layout, scrollListRow(list, index));
    row.height = toFb(layout, 20);
    return row;
}

std::size_t languageVisibleRowCount(float framebufferWidth, float framebufferHeight, int guiScale,
                    bool forceUnicode) {
    const HudLayout layout{framebufferWidth, framebufferHeight, guiScale, forceUnicode};
    const float scale = layout.scale();
    constexpr float kRowStep = 22.0F;
    const float rows = std::max(languageListBox(layout).height / (kRowStep * scale),
                                1.0F);
    return static_cast<std::size_t>(rows);
}

UiRect languageScrollbarTrack(const HudLayout& layout) {
    // UI-4：26.1 把滚动条贴在**行的右缘**再留 2 像素（`AbstractSelectionList.scrollBarX`），
    // 宽 6。从前这里是自造的 10 宽轨道，按"中线右 144"摆——那是照 spec §2.7 那句
    // 「贴在视口右侧」猜的，而 spec 那一条是 1.20.2 之前的状态。
    return fbRect(layout, scrollListScrollbar(languageScrollList(layout)));
}

UiRect languageScrollbarThumb(const HudLayout& layout,
                              std::size_t itemCount, std::size_t visibleRows,
                              std::size_t firstIndex) {
    static_cast<void>(visibleRows);
    // UI-4：滑块与轨道同宽（6），高度走 26.1 的 `clamp(h*h/内容高, 32, h-8)`。
    // 从前是"轨道 10 宽、滑块 4 宽居中、最小高 8"——三个数都是自造的。
    return fbRect(layout, scrollListThumb(languageScrollList(layout), itemCount,
                                          firstIndex));
}

std::size_t languageScrollIndexFromCursor(const HudLayout& layout,
                                          std::size_t itemCount, std::size_t visibleRows,
                                          float cursorY) {
    static_cast<void>(visibleRows);
    return scrollListRowFromScrollbar(languageScrollList(layout), itemCount,
                                      cursorY / layout.scale());
}

// 按键设置页的绑定列表
// 框体位于标题与底部按钮带之间，后者是视角摇晃、自动跳跃、重置、完成
// 几何照搬语言列表，区别是这里要给两行底部按钮留位置，而不是给一行警告文字
UiRect keyBindsListBox(const HudLayout& layout) {
    constexpr int kRowStep = kKeyBindRowHeight;
    constexpr int kTopBound = 40;
    // 列表在底部按钮带上方结束
    // 带的顶行由四个底部按钮中的第一个推出来，两列即两行，与 languageWarningY 读取带位置的方式相同
    // 两处都不需要一个专门的高度取值函数
    const int bandTop = toLogical(layout, layout.bottomMenuButton(0U, 4U, 2U).y);
    const int bottomBound = bandTop - 12;
    const int rows = std::max((bottomBound - kTopBound) / kRowStep, 1);
    const int height = rows * kRowStep;
    return {0.0F, toFb(layout, kTopBound), toFb(layout, layout.logicalWidth()),
            toFb(layout, height)};
}

// 一页里有几个按钮。绑定列表那三个行内控件（名称 / 改键 / 重置）不算——它们的矩形
// 来自列表几何，不占按钮网格的位置。
std::size_t countPageButtons(const Page& page) {
    std::size_t buttons = 0;
    for (const Widget& widget : page) {
        if (!isKeyBindRowWidget(widget) && !isPackRowWidget(widget) &&
            !isPackZoneWidget(widget) && !isScrollListRowWidget(widget)) {
            ++buttons;
        }
    }
    return buttons;
}

namespace {

// UI-11 / A5：提示屏的一趟布局。
//
// ★ "正文有几行"是从**页面里数出来的**，不是另一个参数说的：装配把每一行做成一个
//   Label，所以行数天然只有一份表述。给这里再加一个 `lineCount` 形参，就等于允许
//   "装配了 4 行、布局按 3 行算高度"——那正是 optionsFirstRow 那一族的形状。
void layoutNoticePageInto(Page& page, const HudLayout& layout, const NoticeMetrics& metrics) {
    std::size_t lineCount = 0;
    for (const Widget& widget : page) {
        if (static_cast<WidgetId>(widget.debugId) == WidgetId::NoticeMessage) {
            ++lineCount;
        }
    }
    const auto notice = noticeLayout(layout.logicalWidth(), layout.logicalHeight(),
                                     static_cast<int>(lineCount), metrics);
    std::size_t line = 0;
    for (Widget& widget : page) {
        switch (static_cast<WidgetId>(widget.debugId)) {
        case WidgetId::NoticeTitle:
            widget.rect = fbRect(layout, notice.title);
            break;
        case WidgetId::NoticeMessage:
            widget.rect = fbRect(
                layout, noticeMessageLineRect(notice.message, static_cast<int>(line++)));
            break;
        case WidgetId::NoticeStopShowing:
            widget.rect = fbRect(layout, notice.check);
            break;
        case WidgetId::NoticeProceed:
            widget.rect = fbRect(layout, notice.proceed);
            break;
        default:
            // 这一页上只剩 Back 一个控件。写成 default 而不是 `case WidgetId::Back`
            // 是因为 WidgetId 有上百个取值，穷举它没有意义——真正的护栏是
            // `notice_screen` 里那条"这一页恰好装配了这五种控件"的断言。
            widget.rect = fbRect(layout, notice.back);
            break;
        }
    }
}

} // namespace

void layoutPageInto(Page& page, PageId id, const HudLayout& layout,
                    std::size_t keyBindFirstRow, std::size_t optionsFirstRow,
                    CreateWorldTab createWorldTab, const NoticeMetrics& noticeMetrics) {
    if (pageLayoutKind(id) == PageLayoutKind::CentredNotice) {
        layoutNoticePageInto(page, layout, noticeMetrics);
        return;
    }
    // ★ UI-10 / D24：这里**不需要**两栏的窗口起点。装配只造窗口里的那几行，所以
    //   页面里第几个同栏的行天然就是屏幕上的第几行；绝对行号只有**回调**用得着
    //   （它要去索引真正的那个包）。给布局也塞一个 firstRow 参数是"只做有消费者的
    //   东西"的反面——那两个参数会立刻变成第二份可能与装配不同步的表述。
    // 按钮数从装配结果**数出来**，不是另一张表说的。这就是这两趟拆分的全部意义。
    const std::size_t buttonCount = countPageButtons(page);
    std::size_t buttonIndex = 0;
    std::size_t keyWidgetIndex = 0;
    // ★ 包行按**栏**分别计数：两栏是两张独立的列表，行号各自从 0 数起。
    const auto lists = dualColumnLists(
        headerAndFooterLayout(layout.logicalWidth(), layout.logicalHeight()).contentBox(),
        layout.logicalWidth());
    std::size_t availableRow = 0;
    std::size_t selectedRow = 0;
    // UI-10 / D24：右栏每一行后面跟着三块热区（取消选择 / 上移 / 下移），它们的矩形
    // 是**那一行**图标位的三块分区。记住上一行的图标位即可——装配保证它们紧跟在
    // 自己那一行之后（两处次序必须一致，这是护栏 21 那一族）。
    TransferIconZones rowZones{};
    std::size_t worldRowIndex = 0;
    std::size_t languageRowIndex = 0;
    for (Widget& widget : page) {
        if (isPackRowWidget(widget)) {
            const bool right = isSelectedPackRow(widget);
            const auto& list = right ? lists.selected : lists.available;
            const auto row = scrollListRow(list, right ? selectedRow++ : availableRow++);
            widget.rect = fbRect(layout, row);
            if (right) {
                rowZones = transferIconZones(transferIconCell(row));
            }
            continue;
        }
        if (isPackZoneWidget(widget)) {
            const auto id = static_cast<WidgetId>(widget.debugId);
            widget.rect = fbRect(layout, id == WidgetId::PackUnselect  ? rowZones.unselect
                                         : id == WidgetId::PackMoveUp ? rowZones.moveUp
                                                                      : rowZones.moveDown);
            continue;
        }
        // UI-11 / A6：滚动列表的行（世界 / 语言）与世界行的缩略图。
        //
        // ★ 它们此前落在按钮网格那条路上，拿到的是**底部按钮**的矩形（见
        //   ui::isScrollListRowWidget 上面那段）。绘制与命中两侧都各自去调
        //   worldListRow()/languageRow()，所以画面上看不出来——直到可见行数把
        //   buttonCount 顶过 20，`bottomMenuButton` 抛出来为止。
        if (isScrollListRowWidget(widget)) {
            const auto id = static_cast<WidgetId>(widget.debugId);
            if (id == WidgetId::LanguageRow) {
                widget.rect = languageRow(languageRowIndex++, layout);
                continue;
            }
            if (id == WidgetId::WorldIcon) {
                // 缩略图坐在**它那一行**里。行号取自控件自己（imageIndex），
                // 不是"上一行是第几行"——那样会依赖遍历顺序两次。
                widget.rect =
                    fbRect(layout, worldRowParts(logicalWorldListRow(widget.imageIndex, layout))
                                       .icon);
                continue;
            }
            widget.rect = worldListRow(worldRowIndex++, layout);
            continue;
        }
        if (isKeyBindRowWidget(widget)) {
            // ★ 控件序号与**屏幕行号**之间不是倍数关系：可见窗口里夹着分类标题行，
            //   它占一行却不产生控件。照 index/每行控件数 折行，标题行之后的每一行
            //   都会偏上一格，而画面上只表现为"名字和按钮错位了一行"。
            const std::size_t row = keyBindWidgetVisibleRow(keyBindFirstRow, keyWidgetIndex,
                                                            kKeyBindWidgetsPerRow);
            switch (keyWidgetIndex % kKeyBindWidgetsPerRow) {
            case 0U:
                widget.rect = keyBindsNameCell(row, layout);
                break;
            case 1U:
                widget.rect = keyBindsChangeCell(row, layout);
                break;
            default:
                widget.rect = keyBindsResetCell(row, layout);
                break;
            }
            ++keyWidgetIndex;
            continue;
        }
        widget.rect = frontendButtonRect(layout, id, buttonIndex, buttonCount, optionsFirstRow,
                                         createWorldTab);
        ++buttonIndex;
    }
}

UiRect keyBindsRow(std::size_t visibleIndex, const HudLayout& layout) {
    // UI-4：行宽从自造的 300 改成 26.1 的 **340**（`KeyBindsList:59`）。
    // UI-6b：行高不再被压成 11——它就是列表的行高 20，因为一行里要装两个 20 高的按钮。
    return fbRect(layout, scrollListRow(keyBindsScrollList(layout),
                                        visibleIndex));
}

// UI-6b：按键绑定行里的两个格子。几何在 [[ui/ListRow.hpp]]，这里只是换算到帧缓冲像素。
//
// 动作名是一段 Label，改键按钮是一个 Button——**一行两个控件**，而不是从前那样
// 整行一个 ListRow。焦点遍历因此会在名称与按钮之间走，与 26.1 的
// `KeyBindsList.KeyEntry.children()` 同义。
UiRect keyBindsNameCell(std::size_t visibleIndex, const HudLayout& layout) {
    const auto list = keyBindsScrollList(layout);
    return fbRect(layout, keyBindNameCell(scrollListRow(list, visibleIndex), kFontLineHeight));
}

UiRect keyBindsChangeCell(std::size_t visibleIndex, const HudLayout& layout) {
    const auto list = keyBindsScrollList(layout);
    return fbRect(layout, keyBindChangeCell(list, scrollListRow(list, visibleIndex)));
}

UiRect keyBindsResetCell(std::size_t visibleIndex, const HudLayout& layout) {
    const auto list = keyBindsScrollList(layout);
    return fbRect(layout, keyBindResetCell(list, scrollListRow(list, visibleIndex)));
}

std::size_t keyBindsVisibleRowCount(float framebufferWidth, float framebufferHeight, int guiScale,
                    bool forceUnicode) {
    const HudLayout layout{framebufferWidth, framebufferHeight, guiScale, forceUnicode};
    const float scale = layout.scale();
    constexpr float kRowStep = static_cast<float>(kKeyBindRowHeight);
    const float rows =
        std::max(keyBindsListBox(layout).height / (kRowStep * scale), 1.0F);
    return static_cast<std::size_t>(rows);
}

UiRect keyBindsScrollbarTrack(const HudLayout& layout) {
    return fbRect(layout, scrollListScrollbar(keyBindsScrollList(layout)));
}

std::size_t keyBindsScrollIndexFromCursor(const HudLayout& layout,
                                          std::size_t itemCount, std::size_t visibleRows,
                                          float cursorY) {
    static_cast<void>(visibleRows);
    return scrollListRowFromScrollbar(keyBindsScrollList(layout), itemCount,
                                      cursorY / layout.scale());
}

// 三段式设置页那张列表的几何。三个滚动条函数与布局都从这一处取，免得视口再有第二份。
namespace {
[[nodiscard]] HeaderAndFooterLayout optionsFrameImpl(const HudLayout& layout, PageId page) {
    // 带副页眉的页面（Options）页眉更高，内容区相应变矮。
    return optionsSubHeaderCount(page) > 0U
               ? headerAndFooterLayoutWithSubHeader(layout.logicalWidth(), layout.logicalHeight())
               : headerAndFooterLayout(layout.logicalWidth(), layout.logicalHeight());
}

[[nodiscard]] ScrollList optionsListOf(const HudLayout& layout, PageId page) {
    return optionsScrollList(optionsFrameImpl(layout, page).contentBox());
}
} // namespace

HeaderAndFooterLayout optionsFrame(const HudLayout& layout, PageId page) {
    return optionsFrameImpl(layout, page);
}

OptionsWindow optionsWindowFor(const HudLayout& layout, PageId page, std::size_t firstRow) {
    if (pageLayoutKind(page) != PageLayoutKind::HeaderFooterList) {
        // 不是三段式列表页：rowCount = 0，约定是"不滚，全装配"。
        return OptionsWindow{};
    }
    const auto list = optionsListOf(layout, page);
    const auto groups = optionsGroupsOf(page);
    const std::size_t rows = optionsRowCountOf(page);
    // ★ 变高版：分节行高 13 / 31，不是 25。等高公式在有分节行的页面上会算错
    //   "一屏装几行"与"最多滚到哪"，症状是滚到底还差一行、或滚过头露出空白。
    const std::size_t clamped = std::min(firstRow, optionsMaxFirstRow(groups, list.height, rows));
    return OptionsWindow{clamped, optionsVisibleRows(groups, clamped, list.height, rows)};
}

std::size_t optionsMaximumFirstRow(const HudLayout& layout, PageId page) {
    if (pageLayoutKind(page) != PageLayoutKind::HeaderFooterList) {
        return 0U;
    }
    const auto list = optionsListOf(layout, page);
    return optionsMaxFirstRow(optionsGroupsOf(page), list.height, optionsRowCountOf(page));
}

UiRect optionsScrollbarTrack(const HudLayout& layout, PageId page) {
    return fbRect(layout, scrollListScrollbar(optionsListOf(layout, page)));
}

std::size_t optionsScrollIndexFromCursor(const HudLayout& layout, PageId page, float cursorY) {
    // 与其余两张列表同一条换算（scrollListRowFromScrollbar 抓的是滑块中心）。
    return scrollListRowFromScrollbar(optionsListOf(layout, page), optionsRowCountOf(page),
                                      cursorY / layout.scale());
}

UiRect optionsScrollbarThumb(const HudLayout& layout, PageId page, std::size_t firstRow) {
    return fbRect(layout, scrollListThumb(optionsListOf(layout, page), optionsRowCountOf(page),
                                          firstRow));
}

UiRect frontendButtonRect(const HudLayout& layout, PageId page, std::size_t index,
                          std::size_t buttonCount, std::size_t optionsFirstRow,
                          CreateWorldTab createWorldTab) {
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
        const auto frame = optionsFrameImpl(layout, page);
        // ★ UI-6f（D23）：前 `optionsSubHeaderCount(page)` 个控件摆在**副页眉**里
        //   （26.1 `OptionsScreen.init()` 的页眉是 vertical layout：标题 + 一行控件）。
        //   它们不进内容区，所以后面那些的序号要减掉这个数——两侧对序号的含义
        //   必须一致，这与滚动窗口那次是同一族问题。
        const std::size_t subHeader = optionsSubHeaderCount(page);
        if (index < subHeader) {
            return fbRect(layout, subHeaderButton(layout.logicalWidth(), frame.headerHeight,
                                                  index, subHeader));
        }
        // 最后一个控件是 Done，它在页脚里居中，不在列表里。
        if (buttonCount > 0U && index + 1U == buttonCount) {
            return fbRect(layout, frame.footerButton());
        }
        const auto list = optionsScrollList(frame.contentBox());
        // ★ `index` 是**已装配**控件的序号，不是设置项的序号：滚上去的那些项根本没被
        //   造出来，因此不占序号。这条换算只有 `optionsScrolledSlot` 一处，装配侧
        //   （PageBuilder 的 `optionVisible`）与它走的是同一遍循环。
        const auto groups = optionsGroupsOf(page);
        // ★ 副页眉那几个控件不在列表里，所以内容区的序号要减掉它们——
        //   装配侧（optionVisible）做同样的减法，两侧对序号的含义必须一致。
        const auto slot = optionsScrolledSlot(groups, optionsFirstRow, index - subHeader);
        // ★ y 走**像素偏移**，不是"可见行号 × 25"：分节行高 13 / 31，等高公式会让
        //   分节之后的每一个控件都偏。`slot.row` 是可见行号，加回 firstRow 才是绝对行号。
        const int rowTop = optionsRowTop(groups, slot.row + optionsFirstRow) -
                           optionsRowTop(groups, optionsFirstRow);
        // addBig 的一项铺满行宽（310），addSmall 的一项是双列里的一格（150）。
        return fbRect(layout, slot.big ? optionsBigCellAt(list, rowTop)
                                       : optionsSmallCellAt(list, rowTop, slot.column));
    }
    case PageLayoutKind::HeaderFooterForm: {
        // 26.1 CreateWorldScreen：**标签栏（它同时是页眉）**、内容区表单、页脚
        // Create/Cancel 两个按钮。
        //
        // 装配顺序（PageBuilder 的 CreateWorld 分支）：
        //   [0..2] 三个页签 → [3..] 当前页的内容按钮 → [末尾两个] 页脚。
        // ★ 这里必须与那一遍**同一个顺序**：错开一位就是"点 A 触发 B"，而两边
        //   各自都自洽、都编译得过（README 护栏 21）。
        const auto bar = tabBarLayout(layout.logicalWidth(), kCreateWorldTabCount);
        if (index < kCreateWorldTabCount) {
            return fbRect(layout, bar.tab(index));
        }
        const auto form =
            createWorldLayout(layout.logicalWidth(), layout.logicalHeight(), createWorldTab);
        if (buttonCount >= 2U && index + 2U == buttonCount) {
            return fbRect(layout, form.footerLeft);
        }
        if (buttonCount >= 1U && index + 1U == buttonCount) {
            return fbRect(layout, form.footerRight);
        }
        // UI-10：页签之后的第一个控件是**输入框**（Game 页是世界名、World 页是种子），
        // 那两页因此比 More 页多一个。判据是"这一页有没有输入框"，也就是
        // `createWorldLayout` 给出的矩形是不是空的——与绘制侧同一个判据，
        // 而不是在这里再列一张"哪一页有框"的清单。
        const auto& field =
            createWorldTab == CreateWorldTab::Game ? form.nameField : form.seedField;
        std::size_t contentIndex = index - kCreateWorldTabCount;
        if (field.width > 0.0F) {
            if (contentIndex == 0U) {
                return fbRect(layout, field);
            }
            --contentIndex;
        }
        return fbRect(layout, createWorldOptionButton(form, layout.logicalWidth(), contentIndex));
    }
    case PageLayoutKind::HeaderFooterDualColumn: {
        // 26.1 PackSelectionScreen 的页脚：Open Folder 与 Done 横排。本作多两个
        // 调序按钮（26.1 那两个在行内），四个一排。
        const auto frame = headerAndFooterLayout(layout.logicalWidth(), layout.logicalHeight());
        const auto footer = frame.footerBox();
        constexpr int kGap = 4;
        constexpr int kWidth = 100;
        const int count = static_cast<int>(buttonCount);
        const int total = count * kWidth + (count - 1) * kGap;
        const int left = layout.logicalWidth() / 2 - total / 2;
        const int y = static_cast<int>(footer.y) + frame.footerHeight / 2 - kFooterButtonHeight / 2;
        return fbRect(layout, UiRect{static_cast<float>(left + static_cast<int>(index) *
                                                                     (kWidth + kGap)),
                                     static_cast<float>(y), static_cast<float>(kWidth),
                                     static_cast<float>(kFooterButtonHeight)});
    }
    case PageLayoutKind::CentredColumn:
    // UI-11 / A5：提示屏不走这条路——它的五种控件宽度各不相同，矩形由
    // `layoutNoticePageInto` 一次算全（那里才有"正文有几行"这个输入）。
    // 列在这里只是为了不带 default，好让下一页被编译器点名。
    case PageLayoutKind::CentredNotice:
        break;
    }
    return layout.menuButton(index, buttonCount);
}

} // namespace mc::ui
