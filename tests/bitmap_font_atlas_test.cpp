// D4：位图字体 provider 铺进 256x256 层数组的算术。
//
// 修复前是把整张 provider 图重采样进一层 256x256：128x128 的 ascii.png 正好 2 倍放大所以
// 无损，而 128x536 的 nonlatin_european.png 与 144x900 的 accented.png 被纵向压到每格 3 行，
// 约 2247 个码点渲染错误。ASCII 完好，所以英文界面看不出问题——它一直没被发现的原因。
//
// `loadBitmapProvider` 住在 Vulkan 那个翻译单元里，无头测不到；这段算术因此被搬进
// ui/BitmapFontAtlas.hpp，好让"铺错了"有地方变红。下面的数字全部来自 26.1 资源包的实测尺寸。

#include "ui/BitmapFontAtlas.hpp"

#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string& what, int line) {
    if (!condition) {
        std::printf("bitmap_font_atlas_test line %d: %s\n", line, what.c_str());
        ++failures;
    }
}

#define CHECK(condition) check((condition), #condition, __LINE__)

void expectThrows(int w, int h, int cols, int rows, const char* what, int line) {
    try {
        static_cast<void>(mc::ui::bitmapFontLayout(w, h, cols, rows));
    } catch (const std::invalid_argument&) {
        return;
    }
    std::printf("bitmap_font_atlas_test line %d: %s did not throw\n", line, what);
    ++failures;
}

#define EXPECT_THROWS(w, h, c, r) expectThrows((w), (h), (c), (r), #w "x" #h, __LINE__)

// --- 1. 26.1 那四个 provider 的实测尺寸 --------------------------------------
void testRealProviders() {
    // font/ascii.png 128x128，16x16 格
    const auto ascii = mc::ui::bitmapFontLayout(128, 128, 16, 16);
    CHECK(ascii.cellWidth == 8 && ascii.cellHeight == 8);
    CHECK(ascii.rowsPerLayer == 32);   // 256 / 8
    CHECK(ascii.layerCount == 1);

    // font/nonlatin_european.png 128x536，16x67 格 —— 修复前被压到每格 3 行
    const auto nonlatin = mc::ui::bitmapFontLayout(128, 536, 16, 67);
    CHECK(nonlatin.cellWidth == 8 && nonlatin.cellHeight == 8);
    CHECK(nonlatin.rowsPerLayer == 32);
    CHECK(nonlatin.layerCount == 3);   // ceil(67/32)

    // font/accented.png 144x900，16x75 格，格 9x12
    const auto accented = mc::ui::bitmapFontLayout(144, 900, 16, 75);
    CHECK(accented.cellWidth == 9 && accented.cellHeight == 12);
    CHECK(accented.rowsPerLayer == 21);  // 256 / 12 = 21（余 4 像素留空）
    CHECK(accented.layerCount == 4);     // ceil(75/21)

    // 四个 provider 合起来 1 + 3 + 4 + 1 = 9 层
    const auto sga = mc::ui::bitmapFontLayout(128, 128, 16, 16);
    CHECK(ascii.layerCount + nonlatin.layerCount + accented.layerCount + sga.layerCount == 9);
}

// --- 2. ★ 格高必须原样保住 ---------------------------------------------------
//
// 这是缺陷本身：修复前 8 像素高的格在层里只剩 3 行。
void testCellHeightSurvives() {
    const auto nonlatin = mc::ui::bitmapFontLayout(128, 536, 16, 67);
    CHECK(nonlatin.cellHeight == 536 / 67);         // 8，不是被压过的 3
    CHECK(nonlatin.cellHeight * nonlatin.rowsPerLayer <= mc::ui::kFontLayerSize);
    const auto accented = mc::ui::bitmapFontLayout(144, 900, 16, 75);
    CHECK(accented.cellHeight == 900 / 75);         // 12
    CHECK(accented.cellHeight * accented.rowsPerLayer <= mc::ui::kFontLayerSize);
}

// --- 3. 落点：按整格行切，没有字形跨层 ---------------------------------------
void testCellPlacement() {
    const auto layout = mc::ui::bitmapFontLayout(128, 536, 16, 67);
    // 第 0 层最后一整行是第 31 行
    CHECK(mc::ui::bitmapFontCell(layout, 31, 0) == (mc::ui::BitmapFontCell{0, 0, 248}));
    // 第 32 行翻到第 1 层的顶端 —— 而不是被劈成两半
    CHECK(mc::ui::bitmapFontCell(layout, 32, 0) == (mc::ui::BitmapFontCell{1, 0, 0}));
    CHECK(mc::ui::bitmapFontCell(layout, 66, 15) == (mc::ui::BitmapFontCell{2, 120, 16}));
    // Б(U+0411) 在第 4 行第 7 列 —— 诊断书里那个例子
    CHECK(mc::ui::bitmapFontCell(layout, 4, 7) == (mc::ui::BitmapFontCell{0, 56, 32}));

    // ★ 通用性质：任何一格都完整落在它那一层之内，一个像素都不跨界
    for (const auto spec : {std::array{128, 536, 16, 67}, std::array{144, 900, 16, 75},
                            std::array{128, 128, 16, 16}}) {
        const auto plan = mc::ui::bitmapFontLayout(spec[0], spec[1], spec[2], spec[3]);
        for (int row = 0; row < plan.rows; ++row) {
            for (int column = 0; column < plan.columns; ++column) {
                const auto cell = mc::ui::bitmapFontCell(plan, row, column);
                CHECK(cell.layer >= 0 && cell.layer < plan.layerCount);
                CHECK(cell.x >= 0 && cell.x + plan.cellWidth <= mc::ui::kFontLayerSize);
                CHECK(cell.y >= 0 && cell.y + plan.cellHeight <= mc::ui::kFontLayerSize);
            }
        }
    }
}

// --- 4. 每一层从源图的哪里拷、拷多少 -----------------------------------------
void testLayerSlices() {
    const auto layout = mc::ui::bitmapFontLayout(128, 536, 16, 67);
    CHECK(mc::ui::bitmapFontLayerSourceY(layout, 0) == 0);
    CHECK(mc::ui::bitmapFontLayerHeight(layout, 0) == 256);
    CHECK(mc::ui::bitmapFontLayerSourceY(layout, 1) == 256);
    CHECK(mc::ui::bitmapFontLayerHeight(layout, 1) == 256);
    CHECK(mc::ui::bitmapFontLayerSourceY(layout, 2) == 512);
    CHECK(mc::ui::bitmapFontLayerHeight(layout, 2) == 24);   // 尾层只剩 3 行 x 8

    // 各层加起来正好是整张图，一行不多一行不少
    int total = 0;
    for (int layer = 0; layer < layout.layerCount; ++layer) {
        total += mc::ui::bitmapFontLayerHeight(layout, layer);
        CHECK(mc::ui::bitmapFontLayerHeight(layout, layer) <= mc::ui::kFontLayerSize);
        CHECK(mc::ui::bitmapFontLayerSourceY(layout, layer) +
                  mc::ui::bitmapFontLayerHeight(layout, layer) <= 536);
    }
    CHECK(total == 536);

    // accented 的尾层：75 - 3*21 = 12 行 x 12 像素
    const auto accented = mc::ui::bitmapFontLayout(144, 900, 16, 75);
    CHECK(mc::ui::bitmapFontLayerHeight(accented, 3) == 12 * 12);
    int accentedTotal = 0;
    for (int layer = 0; layer < accented.layerCount; ++layer) {
        accentedTotal += mc::ui::bitmapFontLayerHeight(accented, layer);
    }
    CHECK(accentedTotal == 900);
}

// --- 5. 坏资源是抛，不是悄悄夹住 ---------------------------------------------
void testRejects() {
    EXPECT_THROWS(0, 128, 16, 16);
    EXPECT_THROWS(128, 0, 16, 16);
    EXPECT_THROWS(128, 128, 0, 16);
    EXPECT_THROWS(128, 128, 16, 0);
    EXPECT_THROWS(130, 128, 16, 16);   // 宽不是整格数
    EXPECT_THROWS(128, 130, 16, 16);   // 高不是整格数
    // 比一层还宽：现有 provider 都 <= 256，真出现了要先决定横向怎么切；
    // 悄悄截掉会让右半张表整体消失而没有任何提示
    EXPECT_THROWS(512, 256, 16, 16);
    // 一格比一层还高
    EXPECT_THROWS(16, 1024, 1, 2);

    // 边界上的合法值：正好一层宽、格正好一层高
    CHECK(mc::ui::bitmapFontLayout(256, 256, 16, 16).layerCount == 1);
    CHECK(mc::ui::bitmapFontLayout(16, 256, 1, 1).rowsPerLayer == 1);

    bool threw = false;
    try {
        const auto layout = mc::ui::bitmapFontLayout(128, 128, 16, 16);
        static_cast<void>(mc::ui::bitmapFontCell(layout, 16, 0));
    } catch (const std::out_of_range&) {
        threw = true;
    }
    CHECK(threw);
}

} // namespace

int main() {
    testRealProviders();
    testCellHeightSurvives();
    testCellPlacement();
    testLayerSlices();
    testRejects();
    if (failures != 0) {
        std::printf("bitmap_font_atlas_test: %d checks failed\n", failures);
        return 1;
    }
    return 0;
}
