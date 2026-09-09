#include "render/WorldIcon.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// 世界图标那段「帧缓冲 -> 64x64」的算术。全部无头、无文件、无 GPU。
//
// 这里的每个期望值都是**手算**出来的，算的是 26.1 的式子
// （GameRenderer.java:636-646 的居中裁剪）加上本作登记在案的最近邻采样映射，
// 而不是先跑一遍实现再把它吐出来的数字钉住——后者对任何实现都恒真。
// 因此每个断言旁边都写着它是怎么算出来的。

namespace {

using namespace mc;

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"world_icon_test line " + std::to_string(line) +
                                 " failed: " + expression};
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

// 每个像素都把自己的坐标编进 R/G 两个通道：R = x、G = y。于是「第 (tx,ty) 个
// 目标像素采到了源图的哪一个像素」这件事可以被断言直接读出来，而不用靠像素差。
// 宽高都限制在 256 以内，坐标与通道值一一对应。
// B/A 也随两个轴变化：只看 R/G 的话，一个把四个通道搞混或丢掉 alpha 的实现照样
// 全绿。
[[nodiscard]] std::vector<std::uint8_t> makeCoordinateFrame(int width, int height) {
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4U);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * width + x) * 4U;
            rgba[offset + 0U] = static_cast<std::uint8_t>(x);
            rgba[offset + 1U] = static_cast<std::uint8_t>(y);
            rgba[offset + 2U] = static_cast<std::uint8_t>((x * 3 + y * 5 + 7) & 0xFF);
            rgba[offset + 3U] = static_cast<std::uint8_t>(255 - ((x + y) & 0x7F));
        }
    }
    return rgba;
}

struct Rgba final {
    std::uint8_t r = 0U;
    std::uint8_t g = 0U;
    std::uint8_t b = 0U;
    std::uint8_t a = 0U;
    friend bool operator==(const Rgba&, const Rgba&) = default;
};

[[nodiscard]] Rgba iconPixel(const std::vector<std::uint8_t>& icon, int x, int y) {
    const auto offset = (static_cast<std::size_t>(y) * render::kWorldIconSize + x) * 4U;
    return Rgba{icon[offset + 0U], icon[offset + 1U], icon[offset + 2U], icon[offset + 3U]};
}

[[nodiscard]] Rgba framePixel(const std::vector<std::uint8_t>& rgba, int width, int x, int y) {
    const auto offset = (static_cast<std::size_t>(y) * width + x) * 4U;
    return Rgba{rgba[offset + 0U], rgba[offset + 1U], rgba[offset + 2U], rgba[offset + 3U]};
}

// 一张 160x90 的宽帧，26.1 的裁剪算术手算如下
// （GameRenderer.java:640-646，width=160、height=90）：
//
//     width > height          -> 160 > 90，走 if 分支
//     x = (width - height)/2  -> (160 - 90)/2 = 70/2 = 35
//     width = height          -> 90
//     y                       -> 0（从未被赋值）
//
// 裁剪窗口因此是 x∈[35,125)、y∈[0,90) 的 90x90 正方形：左右各切掉 35 列，上下
// 一列不切。这是整个函数里唯一一处"照抄 vanilla"的算术，所以它单独被钉住。
void testWideFrameCropsToTheCentredSquare() {
    constexpr int kWidth = 160;
    constexpr int kHeight = 90;
    const auto frame = makeCoordinateFrame(kWidth, kHeight);
    const auto icon = render::worldIconFromFrame(frame, kWidth, kHeight);
    REQUIRE(icon.size() == render::kWorldIconBytes);

    // 图标左上角必须落在窗口的左上角 (35, 0)。裁剪起点若被写成 0（不裁）或
    // 70（少除一次 2），这一条立刻红。
    REQUIRE(iconPixel(icon, 0, 0) == framePixel(frame, kWidth, 35, 0));
    REQUIRE(iconPixel(icon, 0, 0).r == 35U);
    REQUIRE(iconPixel(icon, 0, 0).g == 0U);

    // 采样映射：sourceX = 35 + floor(tx * 90 / 64) = 35 + floor(tx * 45 / 32)。
    // 下面这张表是手算的：
    //   tx=0  -> 0*45/32 = 0        -> 35
    //   tx=1  -> 45/32   = 1（余13） -> 36
    //   tx=2  -> 90/32   = 2（余26） -> 37   ← 四舍五入会给 3，是这条把截断钉死
    //   tx=3  -> 135/32  = 4（余7）  -> 39   ← 跳过了 38，等比缩小的证据
    //   tx=4  -> 180/32  = 5（余20） -> 40
    //   tx=8  -> 360/32  = 11（余8） -> 46
    //   tx=16 -> 720/32  = 22（余16）-> 57
    //   tx=32 -> 1440/32 = 45（整除）-> 80
    //   tx=48 -> 2160/32 = 67（余16）-> 102
    //   tx=63 -> 2835/32 = 88（余19）-> 123  ← 四舍五入会给 89 -> 124
    struct Sample final {
        int target = 0;
        int source = 0;
    };
    constexpr Sample kColumns[] = {
        {0, 35}, {1, 36}, {2, 37}, {3, 39}, {4, 40},
        {8, 46}, {16, 57}, {32, 80}, {48, 102}, {63, 123},
    };
    for (const auto& column : kColumns) {
        REQUIRE(iconPixel(icon, column.target, 0) ==
                framePixel(frame, kWidth, column.source, 0));
    }

    // 竖直方向窗口不偏移（y=0），窗口边长同样是 90，所以行的映射是同一串数字去掉
    // 那个 +35：sourceY = floor(ty * 45 / 32)。
    constexpr Sample kRows[] = {
        {0, 0}, {1, 1}, {2, 2}, {3, 4}, {4, 5},
        {8, 11}, {16, 22}, {32, 45}, {48, 67}, {63, 88},
    };
    for (const auto& row : kRows) {
        REQUIRE(iconPixel(icon, 0, row.target) == framePixel(frame, kWidth, 35, row.source));
    }

    // 右下角一次把两个轴都钉住：(123, 88)。
    REQUIRE(iconPixel(icon, 63, 63) == framePixel(frame, kWidth, 123, 88));
    REQUIRE(iconPixel(icon, 63, 63).r == 123U);
    REQUIRE(iconPixel(icon, 63, 63).g == 88U);

    // 窗口右边界是开区间：第 124..159 列（以及第 89 行）永远不会被采到。逐像素
    // 扫一遍，任何越出窗口的采样都会被抓住——这比只看四个角强，因为一个把
    // cropWidth 错当成 width 的实现在左上角是对的。
    for (int y = 0; y < render::kWorldIconSize; ++y) {
        for (int x = 0; x < render::kWorldIconSize; ++x) {
            const auto pixel = iconPixel(icon, x, y);
            REQUIRE(pixel.r >= 35U && pixel.r <= 123U);
            REQUIRE(pixel.g <= 88U);
        }
    }
}

// 竖帧走的是 else 分支（GameRenderer.java:643-646，width=90、height=160）：
//
//     width > height -> 90 > 160 为假
//     y = (height - width)/2 -> (160 - 90)/2 = 35
//     height = width         -> 90
//     x                      -> 0
//
// 也就是上下各切 35 行、左右不切。宽帧那条测试无论如何都覆盖不到这一支。
void testTallFrameCropsTheOtherAxis() {
    constexpr int kWidth = 90;
    constexpr int kHeight = 160;
    const auto frame = makeCoordinateFrame(kWidth, kHeight);
    const auto icon = render::worldIconFromFrame(frame, kWidth, kHeight);
    REQUIRE(icon.size() == render::kWorldIconBytes);

    // 左上角 = (0, 35)。两个轴要是被搞反了，这里会读成 (35, 0)。
    REQUIRE(iconPixel(icon, 0, 0) == framePixel(frame, kWidth, 0, 35));
    REQUIRE(iconPixel(icon, 0, 0).r == 0U);
    REQUIRE(iconPixel(icon, 0, 0).g == 35U);

    // 右下角 = (88, 35 + 88) = (88, 123)，同一串 floor(tx*45/32) 的末项。
    REQUIRE(iconPixel(icon, 63, 63) == framePixel(frame, kWidth, 88, 123));
    REQUIRE(iconPixel(icon, 63, 63).r == 88U);
    REQUIRE(iconPixel(icon, 63, 63).g == 123U);
}

// 奇数差的居中裁剪：(width - height) 是奇数时整数除法向下取整，窗口于是**偏左
// 一格**而不是恰好居中。这正是"不要代数化简、原样保留除法"要护住的东西。
// 161x90：x = (161 - 90)/2 = 71/2 = 35（不是 35.5，也不是 36）。
void testOddDifferenceTruncatesTowardsTheLeft() {
    constexpr int kWidth = 161;
    constexpr int kHeight = 90;
    const auto frame = makeCoordinateFrame(kWidth, kHeight);
    const auto icon = render::worldIconFromFrame(frame, kWidth, kHeight);
    REQUIRE(icon.size() == render::kWorldIconBytes);

    REQUIRE(iconPixel(icon, 0, 0).r == 35U);
    // 窗口是 [35, 125)，右边还剩 161 - 125 = 36 列没用上，比左边多切一列。
    REQUIRE(iconPixel(icon, 63, 0).r == 123U);
}

// 已经是正方形、且边长正好等于目标边长的帧：裁剪窗口是整张图（x=y=0），采样映射
// 退化成 floor(t * 64 / 64) = t，也就是恒等。于是结果必须与输入**逐字节相同**。
// 这是最强的一条：任何行/列偏移、任何通道错位、任何转置都活不过它。
void testExactSizeFrameIsCopiedVerbatim() {
    const auto frame = makeCoordinateFrame(render::kWorldIconSize, render::kWorldIconSize);
    const auto icon =
        render::worldIconFromFrame(frame, render::kWorldIconSize, render::kWorldIconSize);
    REQUIRE(icon.size() == render::kWorldIconBytes);
    REQUIRE(icon == frame);
}

// 比目标还小的帧要放大而不是拒绝：32x32 时 sourceX = floor(tx * 32 / 64) =
// floor(tx / 2)，每个源像素被复制成 2x2 一块。手算：tx=0,1 -> 0；tx=2,3 -> 1；
// tx=62,63 -> 31。
void testSmallFrameScalesUp() {
    constexpr int kSize = 32;
    const auto frame = makeCoordinateFrame(kSize, kSize);
    const auto icon = render::worldIconFromFrame(frame, kSize, kSize);
    REQUIRE(icon.size() == render::kWorldIconBytes);

    REQUIRE(iconPixel(icon, 0, 0) == framePixel(frame, kSize, 0, 0));
    REQUIRE(iconPixel(icon, 1, 0) == framePixel(frame, kSize, 0, 0));
    REQUIRE(iconPixel(icon, 2, 0) == framePixel(frame, kSize, 1, 0));
    REQUIRE(iconPixel(icon, 3, 0) == framePixel(frame, kSize, 1, 0));
    REQUIRE(iconPixel(icon, 63, 63) == framePixel(frame, kSize, 31, 31));
    REQUIRE(iconPixel(icon, 62, 62) == framePixel(frame, kSize, 31, 31));
}

// 退化输入一律返回空 vector，绝不返回一张「大小对、内容是垃圾」的图。
// 长度那一条是唯一护栏：worldIconFromFrame 是唯一同时看得见缓冲长度和声明尺寸
// 的地方，放过去就是越界读，而越界读会产出一张看着完全正常的 64x64 图标。
void testDegenerateInputReturnsNothing() {
    const auto frame = makeCoordinateFrame(160, 90);

    // 零宽 / 零高：没有任何像素可裁，更没有短边可言。
    REQUIRE(render::worldIconFromFrame(frame, 0, 90).empty());
    REQUIRE(render::worldIconFromFrame(frame, 160, 0).empty());
    REQUIRE(render::worldIconFromFrame({}, 0, 0).empty());
    // 负数宽高（一个把窗口尺寸算错的调用方完全可能传进来）同样被拒，而不是拿去
    // 算出一个负的索引。
    REQUIRE(render::worldIconFromFrame(frame, -160, 90).empty());
    REQUIRE(render::worldIconFromFrame(frame, 160, -90).empty());

    // 缓冲短了一个字节：160*90*4 = 57600，给 57599。
    REQUIRE(frame.size() == 57600U);
    REQUIRE(render::worldIconFromFrame(std::span{frame}.first(57599U), 160, 90).empty());
    // 短一整行：57600 - 640 = 56960。这一份长度足够让所有采样都落在缓冲内，
    // 所以「越界就崩」抓不到它，只有长度检查能。
    REQUIRE(render::worldIconFromFrame(std::span{frame}.first(56960U), 160, 90).empty());
    // 长了也拒：== 而不是 >=。多出来的字节意味着调用方对尺寸的理解与这里不同，
    // 那么它对"哪一行是第一行"的理解多半也不同。
    std::vector<std::uint8_t> tooLong = frame;
    tooLong.push_back(0U);
    REQUIRE(render::worldIconFromFrame(tooLong, 160, 90).empty());
    // 尺寸对得上但两个轴写反了：160x90 的缓冲声明成 90x160，字节数相同，长度
    // 检查放行——所以这一条不该返回空，而该按 90x160 去裁（else 分支）。它守的
    // 是"长度检查不要越权替调用方猜尺寸"。
    REQUIRE(render::worldIconFromFrame(frame, 90, 160).size() == render::kWorldIconBytes);
}

} // namespace

int main() {
    testWideFrameCropsToTheCentredSquare();
    testTallFrameCropsTheOtherAxis();
    testOddDifferenceTruncatesTowardsTheLeft();
    testExactSizeFrameIsCopiedVerbatim();
    testSmallFrameScalesUp();
    testDegenerateInputReturnsNothing();
    return 0;
}
