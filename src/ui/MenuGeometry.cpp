#include "ui/MenuGeometry.hpp"

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

[[nodiscard]] int centredLogical(int canvas, int extent) { return (canvas - extent) / 2; }

// 一段 fb 像素长度回到逻辑像素。只用于把既有的 `box` 矩形接回整数网格——那些矩形
// 本身已由整数逻辑锚点算出，所以这次除法是精确的。
[[nodiscard]] int toLogical(const HudLayout& layout, float framebuffer) {
    return static_cast<int>(std::lround(framebuffer / layout.scale()));
}

} // namespace

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
    static_cast<void>(framebufferWidth);
    const int canvas = layout.logicalWidth();
    const int width = std::min(300, canvas - 20);
    return {
        toFb(layout, centredLogical(canvas, width)),
        toFb(layout, 34 + static_cast<int>(index) * 22),
        toFb(layout, width),
        toFb(layout, 20),
    };
}

std::size_t saveListVisibleRowCount(float framebufferWidth, float framebufferHeight, int guiScale,
                    bool forceUnicode) {
    const HudLayout layout{framebufferWidth, framebufferHeight, guiScale, forceUnicode};
    constexpr float kListTop = 34.0F; // first row's top edge, in scale units
    constexpr float kRowStep = 22.0F; // vertical distance between row tops
    // 世界列表那四个功能按钮排成两列各两个，整块因此在底部带上正好占两行
    constexpr float kButtonRows = 2.0F;
    constexpr float kButtonHeight = 20.0F;
    constexpr float kButtonStep = 24.0F;
    constexpr float kBottomMargin = 16.0F; // canvas bottom to last button's bottom
    constexpr float kListToButtonGap = 12.0F;
    // ceil 后的逻辑画布（spec §1.1），不是精确的 fb/scale
    const auto logicalHeight = static_cast<float>(layout.logicalHeight());
    const float buttonBlockTop =
        logicalHeight - kBottomMargin - kButtonHeight - (kButtonRows - 1.0F) * kButtonStep;
    const float available = buttonBlockTop - kListToButtonGap - kListTop;
    const float rows = std::max(available / kRowStep, 1.0F);
    return static_cast<std::size_t>(rows);
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
    const auto box = languageListBox(layout, framebufferWidth);
    constexpr float kRowStep = 22.0F;
    // LanguageSelectionList 的背景是整宽的，但 vanilla 的条目选中矩形只有居中的 270 个逻辑像素
    // 把整条背景当作条目会让悬停与选中从一边拉到另一边，还会把两侧的空边槽变成可点击区域
    constexpr float kVanillaRowWidth = 270.0F;
    const int boxWidth = toLogical(layout, box.width);
    const int rowWidth = std::min(static_cast<int>(kVanillaRowWidth), std::max(boxWidth - 32, 1));
    return {
        box.x + toFb(layout, centredLogical(boxWidth, rowWidth)),
        box.y + toFb(layout, static_cast<int>(index) * static_cast<int>(kRowStep)),
        toFb(layout, rowWidth),
        toFb(layout, 20),
    };
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
    const auto box = languageListBox(layout, framebufferWidth);
    // vanilla 把滚动条摆在居中的语言条目之外一点，而不是贴着整宽背景的边缘
    // 可见的滑块中心位于屏幕中线右侧 144 个逻辑像素处
    const int boxWidth = toLogical(layout, box.width);
    const int desiredCenter = boxWidth / 2 + 144;
    const int center = std::clamp(desiredCenter, 5, boxWidth - 5);
    return {box.x + toFb(layout, center - 5), box.y + toFb(layout, 2), toFb(layout, 10),
            std::max(box.height - toFb(layout, 4), 1.0F)};
}

UiRect languageScrollbarThumb(const HudLayout& layout, float framebufferWidth,
                              std::size_t itemCount, std::size_t visibleRows,
                              std::size_t firstIndex) {
    const float scale = layout.scale();
    const auto track = languageScrollbarTrack(layout, framebufferWidth);
    if (itemCount <= visibleRows || itemCount == 0U) {
        return {track.x + 3.0F * scale, track.y, 4.0F * scale, track.height};
    }
    const std::size_t maximumFirst = itemCount - visibleRows;
    const float thumbHeight = std::max(
        track.height * static_cast<float>(visibleRows) / static_cast<float>(itemCount),
        8.0F * scale);
    const float travel = std::max(track.height - thumbHeight, 1.0F);
    const float normalized = static_cast<float>(std::min(firstIndex, maximumFirst)) /
                             static_cast<float>(maximumFirst);
    return {track.x + 3.0F * scale, track.y + normalized * travel,
            4.0F * scale, thumbHeight};
}

std::size_t languageScrollIndexFromCursor(const HudLayout& layout, float framebufferWidth,
                                          std::size_t itemCount, std::size_t visibleRows,
                                          float cursorY) {
    if (itemCount <= visibleRows) {
        return 0U;
    }
    const std::size_t maximumFirst = itemCount - visibleRows;
    const auto track = languageScrollbarTrack(layout, framebufferWidth);
    const auto thumb = languageScrollbarThumb(layout, framebufferWidth, itemCount, visibleRows, 0U);
    const float travel = std::max(track.height - thumb.height, 1.0F);
    const float normalized =
        std::clamp((cursorY - track.y - thumb.height * 0.5F) / travel, 0.0F, 1.0F);
    return static_cast<std::size_t>(
        std::lround(normalized * static_cast<float>(maximumFirst)));
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
    const auto box = controlsListBox(layout, framebufferWidth);
    constexpr int kRowStep = 12;
    constexpr int kRowWidth = 300;
    const int boxWidth = toLogical(layout, box.width);
    const int rowWidth = std::min(kRowWidth, std::max(boxWidth - 32, 1));
    return {
        box.x + toFb(layout, centredLogical(boxWidth, rowWidth)),
        box.y + toFb(layout, static_cast<int>(visibleIndex) * kRowStep),
        toFb(layout, rowWidth),
        toFb(layout, 11),
    };
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
    const auto box = controlsListBox(layout, framebufferWidth);
    const auto row = controlsRow(0U, layout, framebufferWidth);
    const float center = row.x + row.width + toFb(layout, 6);
    return {center - toFb(layout, 5), box.y + toFb(layout, 2), toFb(layout, 10),
            std::max(box.height - toFb(layout, 4), 1.0F)};
}

std::size_t controlsScrollIndexFromCursor(const HudLayout& layout, float framebufferWidth,
                                          std::size_t itemCount, std::size_t visibleRows,
                                          float cursorY) {
    if (itemCount <= visibleRows) {
        return 0U;
    }
    const std::size_t maximumFirst = itemCount - visibleRows;
    const auto track = controlsScrollbarTrack(layout, framebufferWidth);
    const float travel = std::max(track.height, 1.0F);
    const float normalized = std::clamp((cursorY - track.y) / travel, 0.0F, 1.0F);
    return static_cast<std::size_t>(std::lround(normalized * static_cast<float>(maximumFirst)));
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
