#include "render/WorldIcon.hpp"

#include <algorithm>
#include <cstddef>

namespace mc::render {

std::vector<std::uint8_t> worldIconFromFrame(std::span<const std::uint8_t> rgba, int width,
                                             int height) {
    if (width <= 0 || height <= 0) return {};
    // 用 64 位算期望长度：宽高都是 int，width*height*kWorldIconChannels 在 int
    // 里对一张 4K 帧就会溢出，而溢出后的小数字完全可能与一段短缓冲的长度相等
    // ——那正是这道检查本来要拦住的越界读。
    const auto expectedBytes = static_cast<std::uint64_t>(width) *
        static_cast<std::uint64_t>(height) * static_cast<std::uint64_t>(kWorldIconChannels);
    if (static_cast<std::uint64_t>(rgba.size()) != expectedBytes) return {};

    // 26.1 GameRenderer.java:636-646（Mojang 映射），逐字照抄，没有代数化简：
    //
    //     int width = screenshot.getWidth();
    //     int height = screenshot.getHeight();
    //     int x = 0;
    //     int y = 0;
    //     if (width > height) {
    //         x = (width - height) / 2;
    //         width = height;
    //     } else {
    //         y = (height - width) / 2;
    //         height = width;
    //     }
    //
    // 原版是就地改写 width/height 这两个局部量；这里改名成 cropWidth/cropHeight
    // 只为不覆盖参数，赋值的顺序与除法的写法都原样保留。注意 `width > height`
    // 是严格大于：正方形帧走 else 分支，(h - w) / 2 == 0，结果一样，但分支归属
    // 与原版一致，将来任一分支再加东西时不会错位。
    int cropWidth = width;
    int cropHeight = height;
    int x = 0;
    int y = 0;
    if (cropWidth > cropHeight) {
        x = (cropWidth - cropHeight) / 2;
        cropWidth = cropHeight;
    } else {
        y = (cropHeight - cropWidth) / 2;
        cropHeight = cropWidth;
    }

    // 重采样。原版这一步是 stb 的带滤波缩放（见 WorldIcon.hpp 里那条登记的偏差），
    // 本作用最近邻，映射式与 src/render/vulkan/AtlasLayerFit.hpp 的 resizedRegion
    // 一字不差：源坐标 = 窗口起点 + 目标坐标 * 窗口边长 / 目标边长，整数除法向零
    // 取整。cropWidth/cropHeight 在上面已经相等（都等于短边），仍各写各的，是为了
    // 让「两个轴用的是各自的窗口边长」这件事在代码里看得见。
    std::vector<std::uint8_t> icon(kWorldIconBytes);
    for (int targetY = 0; targetY < kWorldIconSize; ++targetY) {
        const int sourceY = y + targetY * cropHeight / kWorldIconSize;
        for (int targetX = 0; targetX < kWorldIconSize; ++targetX) {
            const int sourceX = x + targetX * cropWidth / kWorldIconSize;
            const auto source = static_cast<std::size_t>(
                (static_cast<std::size_t>(sourceY) * static_cast<std::size_t>(width) +
                 static_cast<std::size_t>(sourceX)) *
                kWorldIconChannels);
            const auto target = static_cast<std::size_t>(
                (static_cast<std::size_t>(targetY) * kWorldIconSize +
                 static_cast<std::size_t>(targetX)) *
                kWorldIconChannels);
            std::copy_n(rgba.begin() + static_cast<std::ptrdiff_t>(source), kWorldIconChannels,
                        icon.begin() + static_cast<std::ptrdiff_t>(target));
        }
    }
    return icon;
}

} // namespace mc::render
