// MERCH-1：交易屏里一行交易的几何、面板的三块拆法、以及那八张小精灵的落位。
//
// ★ 断言钉的是 26.1 `MerchantScreen` 里那几条式子算出来的数，不是本作实现的抄本：
//     :57      super(menu, inventory, title, 276, 166)
//     :72-79   buttonY = yo + 16 + 2，每行 += 20，按钮 88x20 在 xo + 5
//     :168-190 offerY = yo + 16 + 1；decorHeight = offerY + 2；
//              costA 在 xo+5+5、costB 在 xo+5+35、箭头在 xo+5+35+20 且 y = decorHeight+3、
//              result 在 xo+5+68
//     :117     缺货的大叉在 leftPos + 83 + 99, topPos + 35，28x21
//     :125     等级条在 xo+136, yo+16，102x5
//     :153-156 滑块在 xo+94, yo+18，6x27

#include "render/vulkan/HudTypes.hpp"
#include "ui/TradeRow.hpp"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string& what, int line) {
    if (!condition) {
        std::printf("trade_row_test line %d: %s\n", line, what.c_str());
        ++failures;
    }
}

#define CHECK(condition) check((condition), #condition, __LINE__)

bool sameRect(const mc::ui::UiRect& rect, float x, float y, float width, float height) {
    return rect.x == x && rect.y == y && rect.width == width && rect.height == height;
}

// --- 1. 一行交易里的四块 -----------------------------------------------------
void testRowParts() {
    const auto first = mc::ui::tradeRowParts(0);
    // 按钮：xo + 5，buttonY = 16 + 2 = 18，88x20。
    CHECK(sameRect(first.button, 5.0F, 18.0F, 88.0F, 20.0F));
    // ★ 物品在 decorHeight = (16 + 1) + 2 = **19**，比按钮上缘低 1 像素。
    //   两者差 1 不是笔误：按钮走 `16+2`，物品走 `(16+1)+2`。
    CHECK(first.wantsA.y == 19.0F);
    CHECK(first.button.y == 18.0F);
    // 三个物品位：5+5 / 5+35 / 5+68。
    CHECK(sameRect(first.wantsA, 10.0F, 19.0F, 16.0F, 16.0F));
    CHECK(sameRect(first.wantsB, 40.0F, 19.0F, 16.0F, 16.0F));
    CHECK(sameRect(first.gives, 73.0F, 19.0F, 16.0F, 16.0F));
    // 箭头：x = 5+35+20 = 60，y = decorHeight + 3 = 22，10x9。
    CHECK(sameRect(first.arrow, 60.0F, 22.0F, 10.0F, 9.0F));

    // 行距 20，横坐标一列到底。
    const auto last = mc::ui::tradeRowParts(mc::ui::kTradeVisibleRows - 1);
    CHECK(last.button.y == 18.0F + 6.0F * 20.0F);
    CHECK(last.wantsA.y == 19.0F + 6.0F * 20.0F);
    CHECK(last.wantsA.x == first.wantsA.x);
    CHECK(last.arrow.x == first.arrow.x);
    // 七行整块落在面板里（面板 276x166）。
    CHECK(last.button.y + last.button.height <= 166.0F);
    CHECK(first.gives.x + first.gives.width <= 94.0F);   // 不越过滚动条那一列
}

// --- 2. 那几块固定装饰 -------------------------------------------------------
void testFixedDecorations() {
    CHECK(mc::ui::kTradeVisibleRows == 7);
    CHECK(mc::ui::kTradeScrollerX == 94 && mc::ui::kTradeScrollerY == 18);
    CHECK(mc::ui::kTradeScrollerWidth == 6 && mc::ui::kTradeScrollerHeight == 27);
    CHECK(mc::ui::kTradeLevelBarX == 136 && mc::ui::kTradeLevelBarY == 16);
    CHECK(mc::ui::kTradeLevelBarWidth == 102 && mc::ui::kTradeLevelBarHeight == 5);
    // 缺货的大叉：`leftPos + 83 + 99` = 182。★ 那两个加数是 vanilla 自己写的
    // （83 是右半边的偏移、99 是 MERCHANT_MENU_PART_X），合成 182 就看不出来历了。
    CHECK(mc::ui::kTradeOutOfStockX == 182 && mc::ui::kTradeOutOfStockY == 35);
    CHECK(mc::ui::kTradeOutOfStockWidth == 28 && mc::ui::kTradeOutOfStockHeight == 21);
    CHECK(mc::ui::kTradeMaxVillagerLevel == 5);
}

// --- 3. 面板拆成的三块：不重叠、不留缝、拼回去正好 276x166 -------------------
void testPanelPieces() {
    const auto pieces = mc::render::tradingPanelPieces();
    CHECK(mc::render::kTradingPanelWidth == 276);
    CHECK(mc::render::kTradingPanelHeight == 166);
    // 三块的面积加起来等于整块面板。
    float area = 0.0F;
    for (const auto& piece : pieces) {
        area += piece.source.width * piece.source.height;
        // 每一块都在 256x256 的层里。
        CHECK(piece.source.x + piece.source.width <= 256.0F);
        CHECK(piece.source.y + piece.source.height <= 256.0F);
        // 落回面板里也不越界。
        CHECK(piece.offsetX + piece.source.width <= 276.0F);
        CHECK(piece.offsetY + piece.source.height <= 166.0F);
    }
    CHECK(area == 276.0F * 166.0F);
    // 三块在**面板**里两两不重叠。
    for (std::size_t a = 0; a < pieces.size(); ++a) {
        for (std::size_t b = a + 1; b < pieces.size(); ++b) {
            const auto& first = pieces[a];
            const auto& second = pieces[b];
            const bool apart = first.offsetX + first.source.width <= second.offsetX ||
                               second.offsetX + second.source.width <= first.offsetX ||
                               first.offsetY + first.source.height <= second.offsetY ||
                               second.offsetY + second.source.height <= first.offsetY;
            CHECK(apart);
        }
    }
    // 三块在**图集层**里同样两两不重叠——重叠的症状是后烘的那块盖掉前一块。
    for (std::size_t a = 0; a < pieces.size(); ++a) {
        for (std::size_t b = a + 1; b < pieces.size(); ++b) {
            const auto& first = pieces[a].source;
            const auto& second = pieces[b].source;
            const bool apart = first.x + first.width <= second.x ||
                               second.x + second.width <= first.x ||
                               first.y + first.height <= second.y ||
                               second.y + second.height <= first.y;
            CHECK(apart);
        }
    }
}

// --- 4. 八张小精灵：尺寸对得上 26.1 的资源，且互不重叠、不压到面板 ----------
void testSpriteAtlasSlots() {
    using mc::render::TradingSprite;
    const auto rect = [](TradingSprite sprite) { return mc::render::tradingSpriteRect(sprite); };
    // 尺寸逐条对 vanilla 的 PNG（已逐张解码核对过）。
    CHECK(sameRect(rect(TradingSprite::Arrow), 40.0F, 166.0F, 10.0F, 9.0F));
    CHECK(rect(TradingSprite::ArrowOutOfStock).width == 10.0F);
    CHECK(rect(TradingSprite::ArrowOutOfStock).height == 9.0F);
    CHECK(rect(TradingSprite::OutOfStock).width == 28.0F);
    CHECK(rect(TradingSprite::OutOfStock).height == 21.0F);
    CHECK(rect(TradingSprite::Scroller).width == 6.0F);
    CHECK(rect(TradingSprite::Scroller).height == 27.0F);
    CHECK(rect(TradingSprite::LevelBarBackground).width == 102.0F);
    CHECK(rect(TradingSprite::LevelBarBackground).height == 5.0F);

    // 名字表与枚举同序、同长——烘焙侧按下标取图，错位就是"画出来是隔壁那张"。
    CHECK(mc::render::kTradingSpriteNames.size() ==
          static_cast<std::size_t>(TradingSprite::Count));

    // 每一张都落在层里、都不压到面板那三块占用的区域，而且两两不重叠。
    const auto pieces = mc::render::tradingPanelPieces();
    for (std::size_t index = 0; index < static_cast<std::size_t>(TradingSprite::Count); ++index) {
        const auto one = rect(static_cast<TradingSprite>(index));
        CHECK(one.width > 0.0F && one.height > 0.0F);
        CHECK(one.x + one.width <= 256.0F);
        CHECK(one.y + one.height <= 256.0F);
        for (const auto& piece : pieces) {
            const bool apart = piece.source.x + piece.source.width <= one.x ||
                               one.x + one.width <= piece.source.x ||
                               piece.source.y + piece.source.height <= one.y ||
                               one.y + one.height <= piece.source.y;
            CHECK(apart);
        }
        for (std::size_t other = 0; other < index; ++other) {
            const auto two = rect(static_cast<TradingSprite>(other));
            const bool apart = two.x + two.width <= one.x || one.x + one.width <= two.x ||
                               two.y + two.height <= one.y || one.y + one.height <= two.y;
            CHECK(apart);
        }
    }
}

} // namespace

int main() {
    testRowParts();
    testFixedDecorations();
    testPanelPieces();
    testSpriteAtlasSlots();
    if (failures > 0) {
        std::printf("trade_row_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("trade_row_test: all checks passed\n");
    return 0;
}
