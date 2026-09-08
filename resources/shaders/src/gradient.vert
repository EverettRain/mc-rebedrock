#version 450

// UI-5：竖直渐变矩形（26.1 的 `GuiGraphicsExtractor.fillGradient`）。
//
// 为什么它有自己的管线，而不是 hud.vert 上的又一个绘制模式：
// 一条渐变要**两个**颜色，而 `HudPush` 已经正好 128 字节（Vulkan 保证的下限），
// 一个自由分量都不剩。硬塞的唯一办法是让某个字段在这个模式下改变含义——
// 那正是 RN-14 那条铁律禁止的事（它把图标的包围盒塞进 `color`，于是背包里每个方块
// 图标都成了黑菱形）。所以这里另开一个推送常量块，像 panorama / item 那样。
//
// 顶点色在**编码值**上线性插值，与 vanilla 一致：GUI 那趟的颜色附件是 UNORM 视图，
// 固定功能混合读写的就是 sRGB 编码值。别在这里加伽马转换。
layout(push_constant) uniform GradientPush {
    vec4 rect;        // 裁剪空间：原点 xy，尺寸 zw
    vec4 topColor;    // y = rect.y 那条边的颜色，RGBA
    vec4 bottomColor; // y = rect.y + rect.w 那条边的颜色
} gradient;

layout(location = 0) out vec4 fragmentColor;

const vec2 corners[6] = vec2[](
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
    vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0)
);

void main() {
    vec2 corner = corners[gl_VertexIndex];
    gl_Position = vec4(gradient.rect.xy + corner * gradient.rect.zw, 0.0, 1.0);
    // corner.y == 0 是矩形的**顶边**：framebufferToClip 把帧缓冲顶部映到 NDC 的 -1，
    // 而 rect.y 是矩形上缘。方向搞反会得到"上红下透"的死亡屏，那是另一个屏幕。
    fragmentColor = mix(gradient.topColor, gradient.bottomColor, corner.y);
}
