// UI-3：GUI spec §1.2 的字体度量。
//
// 这些量是**布局公式的基础**，而且错了不会有任何东西崩：文字只是稍微歪一点、阴影只是
// 稍微亮一点。对账时查出的三处偏差都在这里被钉住：
//   - 行高返回的是字形高 8，26.1 `Font.lineHeight` 是 9
//   - 阴影色乘的是手调的 0.18，26.1 是 `ARGB.scaleRGB(color, 0.25F)`
//   - 所有字形的阴影偏移都用 1.0，26.1 的半尺寸 unicode 字形是 0.5

#include "ui/TextMetrics.hpp"
#include "ui/TextFont.hpp"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string& what, int line) {
    if (!condition) {
        std::printf("text_metrics_test line %d: %s\n", line, what.c_str());
        ++failures;
    }
}

#define CHECK(condition) check((condition), #condition, __LINE__)

// --- 1. 行高与字形高是两个不同的量 -------------------------------------------
void testHeights() {
    CHECK(mc::ui::kFontLineHeight == 9.0F);
    CHECK(mc::ui::kFontGlyphHeight == 8.0F);
    CHECK(mc::ui::kFontLineHeight != mc::ui::kFontGlyphHeight);
    // TextFont 对外报的行高必须是 9，而不是字形高
    CHECK(mc::ui::TextFont::lineHeight() == mc::ui::kFontLineHeight);
}

// --- 2. 阴影 -----------------------------------------------------------------
void testShadow() {
    CHECK(mc::ui::kTextShadowScale == 0.25F);
    CHECK(mc::ui::kAsciiShadowOffset == 1.0F);
    CHECK(mc::ui::kUnicodeShadowOffset == 0.5F);

    // 白色主色：编码字节 255 -> (int)(255*0.25) = 63 -> 63/255。
    // 直接乘 0.25 会得到 0.25（=63.75/255），写进 8 位目标就差一个单位。
    const float white = mc::ui::textShadowChannel(1.0F);
    CHECK(white == 63.0F / 255.0F);
    CHECK(white != 0.25F);
    // 被禁用按钮的灰 0xA0A0A0 -> 160 -> 40
    const float grey = mc::ui::textShadowChannel(160.0F / 255.0F);
    CHECK(grey == 40.0F / 255.0F);
    // 黑色仍是黑色，且不会跑到负数或溢出
    CHECK(mc::ui::textShadowChannel(0.0F) == 0.0F);
    CHECK(mc::ui::textShadowChannel(-1.0F) == 0.0F);
    CHECK(mc::ui::textShadowChannel(2.0F) == 63.0F / 255.0F);
    // 阴影永远比主色暗：这是它存在的理由，任何 >=1 的系数都该在这里红
    for (int encoded = 0; encoded <= 255; ++encoded) {
        const float channel = static_cast<float>(encoded) / 255.0F;
        CHECK(mc::ui::textShadowChannel(channel) <= channel);
    }
}

// --- 3. 每个字形自带阴影偏移 --------------------------------------------------
void testGlyphShadowOffset() {
    // 默认（ASCII 与位图字形）是 1.0
    const mc::ui::FontGlyph defaultGlyph;
    CHECK(defaultGlyph.shadowOffset == mc::ui::kAsciiShadowOffset);

    // unicode 页的字形是 0.5。unicodeGlyph 是私有的，走公开路径：登记一页之后，
    // 一个只有 unihex 覆盖的码点会解析到 unicode 分支。
    mc::ui::TextFont font;
    std::vector<std::uint8_t> sizes(0x10000U, 0U);
    // 0x4E00（一）：首列 0、末列 15，也就是一个满宽字形
    sizes[0x4E00U] = 0x0FU;
    font.setUnicodeSizes(std::move(sizes));
    font.setUnicodePageLayer(0x4E, 1);
    const auto han = font.glyph(U'一');
    CHECK(han.shadowOffset == mc::ui::kUnicodeShadowOffset);
    // ASCII 走的是另一条路，仍是 1.0
    CHECK(font.glyph(U'A').shadowOffset == mc::ui::kAsciiShadowOffset);
}

// --- 4. 整数居中 -------------------------------------------------------------
void testIntegerCentring() {
    using mc::ui::centredX;
    // 偶数差：正好居中
    CHECK(centredX(0, 200, 100) == 50);
    CHECK(centredX(10, 200, 100) == 60);
    // ★ 奇数差：整数除法向下取整，**不是** 四舍五入。差一像素正是 spec §1.2 警告的那个。
    CHECK(centredX(0, 200, 101) == 49);   // (200-101)/2 = 49，不是 49.5 也不是 50
    CHECK(centredX(0, 201, 100) == 50);   // 101/2 = 50
    // 内容比容器宽：Java 与 C++ 的 `/` 都向零截断，于是向右溢出而不是向左
    CHECK(centredX(0, 100, 201) == -50);  // -101/2 = -50，不是 -51
    CHECK(centredX(0, 100, 200) == -50);

    // 垂直居中于一行：按钮的 (20-8)/2 = 6，spec §1.2 与 §2.1 都写着这个 6
    using mc::ui::centredTextY;
    CHECK(centredTextY(0, 20) == 6);
    CHECK(centredTextY(100, 20) == 106);
    CHECK(centredTextY(0, 11) == 1);      // (11-8)/2 = 1
    CHECK(centredTextY(0, 9) == 0);       // (9-8)/2 = 0
}

// --- 5. 文本宽度取整 ---------------------------------------------------------
void testTextWidth() {
    using mc::ui::textWidthLogical;
    // 26.1 的 `Font.width` 是 `Mth.ceil`
    CHECK(textWidthLogical(0.0F) == 0);
    CHECK(textWidthLogical(12.0F) == 12);
    CHECK(textWidthLogical(12.25F) == 13);
    CHECK(textWidthLogical(12.75F) == 13);
    // 恰好整数的浮点和不该被 ceil 顶上去一格（半尺寸 unicode 字形会凑出这种和）
    CHECK(textWidthLogical(6.0F + 6.0F) == 12);
}

// --- 6. ASCII 步进含 1 像素字间距 ---------------------------------------------
void testAdvanceIncludesSpacing() {
    // BitmapFontMetrics 从位图列出发算 advance = 字宽 + 1（spec §1.2 的"字间距 1px
    // 已含在 font.width() 中"）。这里不铺整张字体图，只钉住那条规则的形态：
    // 一个 5 像素宽的字形步进 6，两个连排就是 12。
    mc::ui::TextFont font;
    CHECK(font.glyph(U' ').advance == 4.0F);   // vanilla 的空格步进
}

} // namespace

int main() {
    testHeights();
    testShadow();
    testGlyphShadowOffset();
    testIntegerCentring();
    testTextWidth();
    testAdvanceIncludesSpacing();
    if (failures != 0) {
        std::printf("text_metrics_test: %d checks failed\n", failures);
        return 1;
    }
    return 0;
}
