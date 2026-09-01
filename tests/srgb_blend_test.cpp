#include "ui/SrgbBlend.hpp"

#include <cassert>
#include <cmath>

namespace {

[[nodiscard]] bool near(float value, float expected, float tolerance = 1.0e-4F) {
    return std::fabs(value - expected) <= tolerance;
}

// 提示框那两张精灵的美术常量，与 HudRenderer 的补偿用的是同一组
constexpr float kFillColor = 16.0F / 255.0F;   // 0xF0100010 的 R/B
constexpr float kFillAlpha = 240.0F / 255.0F;  // 它的 0xF0
constexpr float kFrameBlue = 1.0F;             // 边框渐变最亮处的蓝
constexpr float kFrameAlpha = 80.0F / 255.0F;  // 它的 0x50
constexpr float kPanelBackdrop = 0.776F;       // 原版背包面板的浅灰
constexpr float kFillBackdrop = 0.09F;         // 边框压着的填充

} // namespace

// vanilla 在 sRGB 编码值上混合，本渲染器（sRGB 交换链）在线性空间混合。
// 不透明的东西两边一样，半透明的深色叠加层不是——这就是提示框"比原版更透"的
// 全部原因。这些断言钉住三件事：两种混合的差有多大、补偿解得对不对、
// 补偿之后在整个背景域内还差多少。
int main() {
    using namespace mc::ui;

    // --- 传输函数：端点与往返。 ---
    {
        assert(near(srgbToLinear(0.0F), 0.0F));
        assert(near(srgbToLinear(1.0F), 1.0F));
        assert(near(linearToSrgb(1.0F), 1.0F));
        assert(near(srgbToLinear(0.5F), 0.2140F, 1.0e-3F));
        for (int step = 0; step <= 20; ++step) {
            const float value = static_cast<float>(step) / 20.0F;
            assert(near(linearToSrgb(srgbToLinear(value)), value, 1.0e-4F));
        }
    }

    // --- 差本身：深色背景上两种混合几乎重合，浅色背景上我们亮一倍。 ---
    // 这条是缺陷本身。它一旦不成立（比如有人把交换链换成 UNORM 并让 HUD 输出
    // 编码值），下面的补偿就该整个删掉，而不是继续留着。
    {
        assert(near(srgbSpaceBlend(kFillColor, 0.05F, kFillAlpha),
                    linearSpaceBlend(kFillColor, 0.05F, kFillAlpha), 1.0e-3F));
        const float vanilla = srgbSpaceBlend(kFillColor, kPanelBackdrop, kFillAlpha);
        const float ours = linearSpaceBlend(kFillColor, kPanelBackdrop, kFillAlpha);
        assert(near(vanilla, 0.1047F, 1.0e-3F));
        assert(near(ours, 0.2152F, 1.0e-3F));
        assert(ours > vanilla * 2.0F); // 两倍亮 = 肉眼可见地更透
    }

    // --- 解方程：补偿后的 alpha 在它自己的底色上精确复现原版。 ---
    {
        const float alpha = linearBlendAlphaMatchingSrgb(kFillColor, kPanelBackdrop, kFillAlpha);
        assert(alpha > kFillAlpha); // 线性混合放走的背景更多，只能把 alpha 往上抬
        assert(near(alpha, 0.99F, 5.0e-3F));
        assert(near(linearSpaceBlend(kFillColor, kPanelBackdrop, alpha),
                    srgbSpaceBlend(kFillColor, kPanelBackdrop, kFillAlpha), 1.0e-3F));
    }

    // --- 补偿是按一个底色解的，但提示框会压在各种东西上：全域残差要够小。 ---
    {
        const float alpha = linearBlendAlphaMatchingSrgb(kFillColor, kPanelBackdrop, kFillAlpha);
        for (int step = 0; step <= 20; ++step) {
            const float backdrop = static_cast<float>(step) / 20.0F;
            const float vanilla = srgbSpaceBlend(kFillColor, backdrop, kFillAlpha);
            const float ours = linearSpaceBlend(kFillColor, backdrop, alpha);
            assert(std::fabs(ours - vanilla) < 0.012F);
        }
    }

    // --- 边框：按蓝通道对齐（它压着的是刚画完的填充，底色是已知的）。 ---
    {
        const float alpha = linearBlendAlphaMatchingSrgb(kFrameBlue, kFillBackdrop, kFrameAlpha);
        assert(alpha < kFrameAlpha); // 深底上的亮色反过来：线性混合会过亮，得往下压
        assert(near(alpha, 0.1087F, 5.0e-3F));
        assert(near(linearSpaceBlend(kFrameBlue, kFillBackdrop, alpha),
                    srgbSpaceBlend(kFrameBlue, kFillBackdrop, kFrameAlpha), 1.0e-3F));
        // 已知残差：红通道（0x50）随之偏低，那 1px 描边因此比原版略偏蓝。
        const float red = 80.0F / 255.0F;
        const float vanillaRed = srgbSpaceBlend(red, kFillBackdrop, kFrameAlpha);
        const float oursRed = linearSpaceBlend(red, kFillBackdrop, alpha);
        assert(oursRed < vanillaRed);
        assert(vanillaRed - oursRed < 0.04F);
    }

    // --- 退化情形：源与底同色时方程无解，原样返回。 ---
    {
        assert(near(linearBlendAlphaMatchingSrgb(0.4F, 0.4F, 0.5F), 0.5F));
    }

    return 0;
}
