#pragma once

// D4：位图字体 provider 怎么铺进字体纹理数组。
//
// 字体数组的每一层都是 256x256（`kFontLayerSize`），而 26.1 的位图 provider 比这高得多：
// `font/nonlatin_european.png` 是 128x536、`font/accented.png` 是 144x900。
//
// 修复之前的做法是把整张图**最近邻重采样**进 256x256。128x128 的 `ascii.png` 正好是 2 倍
// 放大所以无损，另外两张却被纵向压扁：8 像素高的格只剩 3~4 行，而且会采到格外的相邻行。
// 约 2247 个码点（全部重音拉丁、希腊、西里尔、希伯来、亚美尼亚）因此渲染成"可辨认但不对、
// 上方带游离笔画"的样子。ASCII 完好，所以英文界面看不出问题——它一直没被发现的原因。
// 诊断书：docs/content-dev/UI-gui-parity/D4-bitmap-font-resample-diagnosis.md
//
// 现在改成**按原生分辨率铺进多层**，切割一律落在**整格行**上，任何字形都不会跨层。
//
// 这里只有算术：不读文件、不碰 Vulkan、不碰像素，因此每一条都能被无头断言。
// `loadBitmapProvider` 住在 Vulkan 那个翻译单元里，无头测不到——把这段搬出来正是为了
// 让"铺错了"这件事有地方变红（同 ui/GuiNineSlice.hpp 的理由）。

#include <algorithm>
#include <stdexcept>
#include <string>

namespace mc::ui {

// 字体纹理数组每层的边长。
inline constexpr int kFontLayerSize = 256;

// 一个位图 provider 铺进数组的方案。
struct BitmapFontLayout final {
    int cellWidth = 0;
    int cellHeight = 0;
    int columns = 0;      // 表的列数（provider 的 `chars` 每行长度）
    int rows = 0;         // 表的行数（`chars` 的行数）
    int rowsPerLayer = 0; // 一层放得下多少**整**格行
    int layerCount = 0;

    [[nodiscard]] constexpr bool operator==(const BitmapFontLayout&) const = default;
};

// 一格在数组里的落点：第几层、层内左上角像素。
struct BitmapFontCell final {
    int layer = 0;
    int x = 0;
    int y = 0;

    [[nodiscard]] constexpr bool operator==(const BitmapFontCell&) const = default;
};

// 解出铺法。`imageWidth/Height` 是 provider 图片的原生尺寸，`columns/rows` 是它的字符表形状。
//
// 抛而不是夹的三种情形，都是"资源坏了"而不是"参数没调好"：
//   - 表是空的，或者图不是整格数；
//   - 一格比一层还高（256 以内放不下一整行）；
//   - 图比一层还宽——现有 provider 都 <= 256 宽（128 / 144），真出现了要先决定横向怎么切，
//     悄悄截掉会让右半张表整体消失而没有任何提示。
[[nodiscard]] inline BitmapFontLayout bitmapFontLayout(int imageWidth, int imageHeight,
                                                       int columns, int rows) {
    if (columns <= 0 || rows <= 0 || imageWidth <= 0 || imageHeight <= 0 ||
        imageWidth % columns != 0 || imageHeight % rows != 0) {
        throw std::invalid_argument("bitmap font image is not a whole number of cells");
    }
    if (imageWidth > kFontLayerSize) {
        throw std::invalid_argument("bitmap font provider is wider than a font layer: " +
                                    std::to_string(imageWidth));
    }
    BitmapFontLayout layout;
    layout.cellWidth = imageWidth / columns;
    layout.cellHeight = imageHeight / rows;
    layout.columns = columns;
    layout.rows = rows;
    layout.rowsPerLayer = kFontLayerSize / layout.cellHeight;
    if (layout.rowsPerLayer <= 0) {
        throw std::invalid_argument("bitmap font cell is taller than a font layer");
    }
    // 向上取整：最后一层通常装不满，那没关系，空白处永远采不到
    layout.layerCount = (rows + layout.rowsPerLayer - 1) / layout.rowsPerLayer;
    return layout;
}

// 第 (row, column) 格落在哪一层的哪个像素。
//
// **按整格行切**：`row / rowsPerLayer` 定层，`row % rowsPerLayer` 定层内行号。
// 这正是"没有字形跨层"的来源——若按像素高度硬切，8 像素的格会在层边界被劈成两半，
// 而那种错误在画面上表现为某几行字被拦腰截断，很难倒推回切割逻辑。
[[nodiscard]] inline BitmapFontCell bitmapFontCell(const BitmapFontLayout& layout, int row,
                                                   int column) {
    if (row < 0 || row >= layout.rows || column < 0 || column >= layout.columns) {
        throw std::out_of_range("bitmap font cell is outside the provider's table");
    }
    return BitmapFontCell{
        row / layout.rowsPerLayer,
        column * layout.cellWidth,
        (row % layout.rowsPerLayer) * layout.cellHeight,
    };
}

// 某一层要从源图的第几行像素开始拷、拷多少行。
// 尾层不满时只拷剩下的那些，不越过源图末尾。
[[nodiscard]] inline int bitmapFontLayerSourceY(const BitmapFontLayout& layout, int layer) {
    return layer * layout.rowsPerLayer * layout.cellHeight;
}

[[nodiscard]] inline int bitmapFontLayerHeight(const BitmapFontLayout& layout, int layer) {
    const int remaining = layout.rows - layer * layout.rowsPerLayer;
    return std::min(remaining, layout.rowsPerLayer) * layout.cellHeight;
}

} // namespace mc::ui
