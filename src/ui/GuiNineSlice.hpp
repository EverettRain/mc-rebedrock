#pragma once

// 把一句"把这个精灵画进那个矩形"变成一组遵守该精灵 26.1 gui.scaling 的四边形
// 这里不碰 Vulkan，也不碰渲染器，九宫格几何因此能被无头断言，而不是只能靠眼睛看
// 九宫格恰恰是那种很容易错得不明显的东西
//
// 下面的一切都以精灵像素为单位，也就是 mcmeta 里的边框与参考尺寸所用的单位
// 两次换算都只在输出时发生一次：目标一侧按 scale（GUI 缩放）换成帧缓冲像素
// 源一侧按美术自身的分辨率换成图集像素
// 因此资源包即使把美术做成 2 倍分辨率，3 像素的边框读出来仍是 3 个精灵像素

#include "assets/GuiSpriteScaling.hpp"
#include "ui/HudLayout.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace mc::ui {

// 一次子矩形绘制，destination 以帧缓冲像素计，source 以图集像素计
struct GuiSpriteQuad final {
    UiRect destination;
    UiRect source;
};

// 重复区只有一两个像素宽的精灵，铺满一个大窗口需要成千上万个四边形
// 某个轴上的重复次数超过这个值时，该轴改为拉伸，而不是把命令缓冲淹掉
inline constexpr int kMaximumGuiTilesPerAxis = 64;

namespace detail {

// 一个轴上的三条带之一：前边框、中段、后边框
// 偏移与长度都以精灵像素计，相对矩形的前缘
struct GuiSpan final {
    float destinationOffset = 0.0F;
    float destinationLength = 0.0F;
    float sourceOffset = 0.0F;
    float sourceLength = 0.0F;
    // 随目标一起变大的那条带，区别于固定尺寸的边框
    // 只有它们会平铺或拉伸
    bool inner = false;
};

// 拆分一个轴
// 两侧边框都被夹到目标的一半，与 vanilla 一致
// 画得比自身边框还窄的控件因此显示两个半边框、没有中段，而不是切片互相重叠或翻转
// 源一侧同样使用夹紧后的内缩量，边框带因此始终在精灵空间里 1:1 采样，保持锐利
[[nodiscard]] inline std::array<GuiSpan, 3> guiSliceAxis(float destinationLength,
                                                         float referenceLength, int leadingBorder,
                                                         int trailingBorder) {
    const float half = destinationLength * 0.5F;
    const float leading = std::min(static_cast<float>(std::max(leadingBorder, 0)), half);
    const float trailing = std::min(static_cast<float>(std::max(trailingBorder, 0)), half);
    const float middle = std::max(destinationLength - leading - trailing, 0.0F);
    const float sourceMiddle = std::max(referenceLength - leading - trailing, 0.0F);
    return {
        GuiSpan{0.0F, leading, 0.0F, leading, false},
        GuiSpan{leading, middle, leading, sourceMiddle, true},
        GuiSpan{leading + middle, trailing, referenceLength - trailing, trailing, false},
    };
}

// 一条带重复几次，以及一次重复在目标上有多长
// 不重复的带就是一步覆盖整条带，那等于一次普通拉伸
struct GuiRepeat final {
    int count = 1;
    float step = 0.0F;
};

[[nodiscard]] inline GuiRepeat guiRepeat(bool repeat, float destinationLength,
                                         float sourceLength) {
    if (!repeat || sourceLength <= 0.0F) {
        return GuiRepeat{1, destinationLength};
    }
    // 这个 epsilon 让恰好整除的情况停在一次重复上，比如 194 像素的中段铺 194 像素的带
    // 否则会多出一个零宽的第二次
    const int count =
        std::max(static_cast<int>(std::ceil(destinationLength / sourceLength - 1.0e-4F)), 1);
    if (count > kMaximumGuiTilesPerAxis) {
        return GuiRepeat{1, destinationLength};
    }
    return GuiRepeat{count, sourceLength};
}

} // namespace detail

// 按 scaling 与 GUI 缩放 scale，产出把 source（图集像素）画进 destination（帧缓冲像素）的那些四边形
// emit 每个四边形调用一次，按行主序
// 这些目标四边形恰好划分 destination，不留缝隙
template <typename Emit>
void forEachGuiSpriteQuad(const UiRect& destination, const UiRect& source,
                          const assets::GuiSpriteScaling& scaling, float scale, Emit&& emit) {
    if (destination.width <= 0.0F || destination.height <= 0.0F || source.width <= 0.0F ||
        source.height <= 0.0F || scale <= 0.0F) {
        return;
    }
    if (scaling.type == assets::GuiSpriteScalingType::Stretch) {
        emit(GuiSpriteQuad{destination, source});
        return;
    }
    // 附属文件可以不写参考尺寸，这时边框就以美术自身的尺寸为基准来度量
    const float referenceWidth =
        scaling.width > 0 ? static_cast<float>(scaling.width) : source.width;
    const float referenceHeight =
        scaling.height > 0 ? static_cast<float>(scaling.height) : source.height;
    const float atlasPerSpriteX = source.width / referenceWidth;
    const float atlasPerSpriteY = source.height / referenceHeight;

    // tile 是同一套拆分的无边框情形：没有外框，只有一条重复整张精灵的中段
    const bool nineSlice = scaling.type == assets::GuiSpriteScalingType::NineSlice;
    const auto columns = detail::guiSliceAxis(destination.width / scale, referenceWidth,
                                              nineSlice ? scaling.border.left : 0,
                                              nineSlice ? scaling.border.right : 0);
    const auto rows = detail::guiSliceAxis(destination.height / scale, referenceHeight,
                                           nineSlice ? scaling.border.top : 0,
                                           nineSlice ? scaling.border.bottom : 0);
    // 26.1 默认平铺九宫格的中段，除非精灵自己要求拉伸
    // tile 类型的精灵则总是重复
    const bool repeatInner = !nineSlice || !scaling.stretchInner;

    for (const auto& row : rows) {
        if (row.destinationLength <= 0.0F || row.sourceLength <= 0.0F) {
            continue;
        }
        const auto vertical =
            detail::guiRepeat(repeatInner && row.inner, row.destinationLength, row.sourceLength);
        for (const auto& column : columns) {
            if (column.destinationLength <= 0.0F || column.sourceLength <= 0.0F) {
                continue;
            }
            const auto horizontal = detail::guiRepeat(repeatInner && column.inner,
                                                      column.destinationLength, column.sourceLength);
            for (int tileY = 0; tileY < vertical.count; ++tileY) {
                const float y = row.destinationOffset + static_cast<float>(tileY) * vertical.step;
                const float height =
                    std::min(vertical.step, row.destinationOffset + row.destinationLength - y);
                if (height <= 0.0F) {
                    break;
                }
                for (int tileX = 0; tileX < horizontal.count; ++tileX) {
                    const float x =
                        column.destinationOffset + static_cast<float>(tileX) * horizontal.step;
                    const float width = std::min(
                        horizontal.step, column.destinationOffset + column.destinationLength - x);
                    if (width <= 0.0F) {
                        break;
                    }
                    // 越过末端的那次重复会被裁掉，它只采样源的前一部分
                    // 而不是把整个源压进剩下的那点空间里
                    emit(GuiSpriteQuad{
                        UiRect{destination.x + x * scale, destination.y + y * scale, width * scale,
                               height * scale},
                        UiRect{source.x + column.sourceOffset * atlasPerSpriteX,
                               source.y + row.sourceOffset * atlasPerSpriteY,
                               column.sourceLength * (width / horizontal.step) * atlasPerSpriteX,
                               row.sourceLength * (height / vertical.step) * atlasPerSpriteY},
                    });
                }
            }
        }
    }
}

} // namespace mc::ui
