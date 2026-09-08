#version 450

// UI-5：竖直渐变矩形。与 gradient.vert 共用同一个推送常量块——
// 这一段一个字段都不读，但仍然完整声明：推送常量块是一段内存布局，
// 不是"本阶段刚好要用的字段清单"（同 hud.frag 里那段注释的理由）。
layout(push_constant) uniform GradientPush {
    vec4 rect;
    vec4 topColor;
    vec4 bottomColor;
} gradient;

layout(location = 0) in vec4 fragmentColor;
layout(location = 0) out vec4 outColor;

void main() {
    // 直接输出插值后的编码值，由固定功能混合按 alpha 压在已有画面上——
    // 与 vanilla 的 fillGradient 一模一样：**一层**半透明，不是两层叠加。
    outColor = fragmentColor;
}
