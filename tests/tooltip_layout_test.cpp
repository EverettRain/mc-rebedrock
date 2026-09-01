#include "ui/TooltipLayout.hpp"

#include <cassert>
#include <cmath>

namespace {

[[nodiscard]] bool near(float value, float expected) {
    return std::fabs(value - expected) < 1.0e-4F;
}

} // namespace

// 提示框的几何，对标 26.1 的 `GuiGraphics#tooltip`、`DefaultTooltipPositioner`
// 与 `TooltipRenderUtil`。这半边此前只活在 Vulkan 头里，一条断言都没有——
// "多一行高多少""贴着右边缘往哪翻"错了不会崩，只会看起来怪，正是最该钉住的那类。
int main() {
    using namespace mc::ui;

    // --- 框高：单行是 8 不是 10（`lines.size() == 1 ? -2 : 0`），多行才是 10n。 ---
    {
        assert(near(tooltipContentHeight(0U), 0.0F));
        assert(near(tooltipContentHeight(1U), 8.0F));
        assert(near(tooltipContentHeight(2U), 20.0F));
        assert(near(tooltipContentHeight(5U), 50.0F));
    }

    // --- 逐行落位：名称行之后多 2px 的缝，之后每行 10。 ---
    {
        assert(near(tooltipLineOffset(0U), 0.0F));
        assert(near(tooltipLineOffset(1U), 12.0F));
        assert(near(tooltipLineOffset(2U), 22.0F));
        assert(near(tooltipLineOffset(3U), 32.0F));
        // 原版行为：最后一行会略微探进下边的内边距——框高 10n 容不下那 2px 的缝。
        const std::size_t lines = 3U;
        assert(tooltipLineOffset(lines - 1U) + kTooltipLineHeight >
               tooltipContentHeight(lines));
    }

    // --- 屏幕中间：光标右上方 +12/-12，纵向是负的。 ---
    {
        const auto origin = positionTooltip(400.0F, 300.0F, 100.0F, 150.0F, 80.0F, 30.0F);
        assert(near(origin.x, 112.0F));
        assert(near(origin.y, 138.0F));
    }

    // --- 贴右边缘：整个翻到光标左侧（-24-w），而不是被截断。 ---
    {
        const auto origin = positionTooltip(400.0F, 300.0F, 380.0F, 150.0F, 80.0F, 30.0F);
        assert(near(origin.x, 380.0F + 12.0F - 24.0F - 80.0F)); // 288
        assert(origin.x + 80.0F <= 400.0F);
    }

    // --- 比屏幕还宽的框翻不过去，夹到左缘 4px，绝不为负。 ---
    {
        const auto origin = positionTooltip(200.0F, 300.0F, 190.0F, 150.0F, 400.0F, 30.0F);
        assert(near(origin.x, 4.0F));
    }

    // --- 贴下边缘：顶到 screenHeight - h - 3（加的是 PADDING_BOTTOM，不是整个 MARGIN）。 ---
    {
        const auto origin = positionTooltip(400.0F, 300.0F, 100.0F, 295.0F, 80.0F, 50.0F);
        assert(near(origin.y, 300.0F - 50.0F - 3.0F)); // 247
        assert(origin.y + 50.0F + 3.0F <= 300.0F);
    }

    // --- 贴上边缘：本项目比 vanilla 多夹一次上边（原版会切掉上沿）。 ---
    {
        const auto origin = positionTooltip(400.0F, 300.0F, 100.0F, 2.0F, 80.0F, 30.0F);
        assert(near(origin.y, 4.0F));
    }

    // --- 底衬精灵：内容矩形每边外扩 12（PADDING 3 + MARGIN 9）。 ---
    {
        const auto backdrop = tooltipBackdrop(UiRect{50.0F, 60.0F, 100.0F, 20.0F});
        assert(near(backdrop.x, 38.0F));
        assert(near(backdrop.y, 48.0F));
        assert(near(backdrop.width, 124.0F));
        assert(near(backdrop.height, 44.0F));
        // 精灵最外 8px 透明、边框那 1px 在第 9 行：可见填充落在内容 +4，边框落在内容 +3。
        assert(near(backdrop.x + 8.0F, 50.0F - 4.0F));
        assert(near(backdrop.x + 9.0F, 50.0F - 3.0F));
    }

    return 0;
}
