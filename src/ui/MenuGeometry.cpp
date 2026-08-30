#include "ui/MenuGeometry.hpp"

#include <algorithm>
#include <cmath>

namespace mc::ui {

std::size_t menuButtonCount(PageId page, bool worldOpen) {
    switch (page) {
    case PageId::Title:
        return 3U;
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
        return 11U;
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
    const float scale = layout.scale();
    const float width = std::min(300.0F * scale, framebufferWidth - 20.0F * scale);
    return {
        (framebufferWidth - width) * 0.5F,
        (34.0F + static_cast<float>(index) * 22.0F) * scale,
        width,
        20.0F * scale,
    };
}

std::size_t saveListVisibleRowCount(float framebufferWidth, float framebufferHeight, int guiScale) {
    const HudLayout layout{framebufferWidth, framebufferHeight, guiScale};
    const float scale = layout.scale();
    constexpr float kListTop = 34.0F; // first row's top edge, in scale units
    constexpr float kRowStep = 22.0F; // vertical distance between row tops
    // 世界列表那四个功能按钮排成两列各两个，整块因此在底部带上正好占两行
    constexpr float kButtonRows = 2.0F;
    constexpr float kButtonHeight = 20.0F;
    constexpr float kButtonStep = 24.0F;
    constexpr float kBottomMargin = 16.0F; // canvas bottom to last button's bottom
    constexpr float kListToButtonGap = 12.0F;
    const float logicalHeight = framebufferHeight / scale;
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
    const float scale = layout.scale();
    constexpr float kRowStep = 22.0F;
    const float topBound = 44.0F * scale;
    const float warningY = languageWarningY(layout);
    const float bottomBound = warningY - 8.0F * scale;
    const float width = framebufferWidth;
    // 高度按内容定：带里放得下几行就是几行
    const std::size_t rows = std::max<std::size_t>(
        static_cast<std::size_t>((bottomBound - topBound) / (kRowStep * scale)), 1U);
    const float height = static_cast<float>(rows) * kRowStep * scale;
    const float top = topBound + (bottomBound - topBound - height) * 0.5F;
    return {0.0F, top, width, height};
}

UiRect languageRow(std::size_t index, const HudLayout& layout, float framebufferWidth) {
    const float scale = layout.scale();
    const auto box = languageListBox(layout, framebufferWidth);
    constexpr float kRowStep = 22.0F;
    // LanguageSelectionList 的背景是整宽的，但 vanilla 的条目选中矩形只有居中的 270 个逻辑像素
    // 把整条背景当作条目会让悬停与选中从一边拉到另一边，还会把两侧的空边槽变成可点击区域
    constexpr float kVanillaRowWidth = 270.0F;
    const float rowWidth = std::min(kVanillaRowWidth * scale,
                                    std::max(box.width - 32.0F * scale, 1.0F));
    return {
        box.x + (box.width - rowWidth) * 0.5F,
        box.y + static_cast<float>(index) * kRowStep * scale,
        rowWidth,
        20.0F * scale,
    };
}

std::size_t languageVisibleRowCount(float framebufferWidth, float framebufferHeight, int guiScale) {
    const HudLayout layout{framebufferWidth, framebufferHeight, guiScale};
    const float scale = layout.scale();
    constexpr float kRowStep = 22.0F;
    const float rows = std::max(languageListBox(layout, framebufferWidth).height / (kRowStep * scale),
                                1.0F);
    return static_cast<std::size_t>(rows);
}

UiRect languageScrollbarTrack(const HudLayout& layout, float framebufferWidth) {
    const float scale = layout.scale();
    const auto box = languageListBox(layout, framebufferWidth);
    // vanilla 把滚动条摆在居中的语言条目之外一点，而不是贴着整宽背景的边缘
    // 可见的滑块中心位于屏幕中线右侧 144 个逻辑像素处
    const float desiredCenter = box.x + box.width * 0.5F + 144.0F * scale;
    const float center = std::clamp(desiredCenter, box.x + 5.0F * scale,
                                    box.x + box.width - 5.0F * scale);
    return {center - 5.0F * scale, box.y + 2.0F * scale,
            10.0F * scale, std::max(box.height - 4.0F * scale, 1.0F)};
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
    const float scale = layout.scale();
    constexpr float kRowStep = 12.0F;
    const float topBound = 40.0F * scale;
    // 列表在底部按钮带上方结束
    // 带的顶行由四个底部按钮中的第一个推出来，两列即两行，与 languageWarningY 读取带位置的方式相同
    // 两处都不需要一个专门的高度取值函数
    const float bandTop = layout.bottomMenuButton(0U, 4U, 2U).y;
    const float bottomBound = bandTop - 12.0F * scale;
    const std::size_t rows = std::max<std::size_t>(
        static_cast<std::size_t>((bottomBound - topBound) / (kRowStep * scale)), 1U);
    const float height = static_cast<float>(rows) * kRowStep * scale;
    return {0.0F, topBound, framebufferWidth, height};
}

UiRect controlsRow(std::size_t visibleIndex, const HudLayout& layout, float framebufferWidth) {
    const float scale = layout.scale();
    const auto box = controlsListBox(layout, framebufferWidth);
    constexpr float kRowStep = 12.0F;
    constexpr float kRowWidth = 300.0F;
    const float rowWidth =
        std::min(kRowWidth * scale, std::max(box.width - 32.0F * scale, 1.0F));
    return {
        box.x + (box.width - rowWidth) * 0.5F,
        box.y + static_cast<float>(visibleIndex) * kRowStep * scale,
        rowWidth,
        11.0F * scale,
    };
}

std::size_t controlsVisibleRowCount(float framebufferWidth, float framebufferHeight, int guiScale) {
    const HudLayout layout{framebufferWidth, framebufferHeight, guiScale};
    const float scale = layout.scale();
    constexpr float kRowStep = 12.0F;
    const float rows =
        std::max(controlsListBox(layout, framebufferWidth).height / (kRowStep * scale), 1.0F);
    return static_cast<std::size_t>(rows);
}

UiRect controlsScrollbarTrack(const HudLayout& layout, float framebufferWidth) {
    const float scale = layout.scale();
    const auto box = controlsListBox(layout, framebufferWidth);
    const auto row = controlsRow(0U, layout, framebufferWidth);
    const float center = row.x + row.width + 6.0F * scale;
    return {center - 5.0F * scale, box.y + 2.0F * scale, 10.0F * scale,
            std::max(box.height - 4.0F * scale, 1.0F)};
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
